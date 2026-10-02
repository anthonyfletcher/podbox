/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The library as an iAP accessory browses it, over USB or the dock's serial
 * line.
 *
 * Playlists are [All Tracks] at index 0, which accessories hide and open as
 * their Songs list, then the Queue, then the folders Moods and Journeys while
 * the Playlist Engine is on, and Audiobooks, then the playlist catalogue's
 * files in directory order. A folder's Track category lists what it holds,
 * and choosing one plays it. Choosing the Queue leaves playback alone and a
 * file plays at once; either way the Track category is then the Queue, which
 * the caller names and plays.
 *
 * Genres, Artists (album artists), Composers, Albums and Audiobooks come from
 * the database, and only while it is in RAM. Choosing one narrows everything
 * below it, in the order Genre > Artist > Composer > Album; an audiobook is
 * an album on its own. The Track category is then the songs under the
 * selection -- the whole library at the top -- and choosing one replaces the
 * Queue with them all and plays from it, a book from where it was left when
 * it is played from its start.
 *
 * Every list lives in one block, allocated the first time the database is
 * browsed and sized by Accessory Browsing, and freed when the accessory
 * goes. A list longer than the block is cut short. The block also holds the
 * stack of a thread that builds the Queue and the engine's playlists, which
 * take longer than an accessory waits for a reply; while it runs the Track
 * category is the Queue as it grows, and the database lists are refused.
 *
 * Parts, in order:
 *   - the selection
 *   - the block and its thread
 *   - building a list
 *   - playing a list
 *   - the Playlist category
 *   - the entry points
 *   - the library as iAP2 sends it: every track by its key, and playing
 *     the keys a car chooses
 ****************************************************************************/

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "thread.h"
#include "core_alloc.h"
#include "dir.h"
#include "lang.h"
#include "audio.h"
#include "settings.h"
#include "string-extra.h"
#include "playlist/playlist.h"
#include "playlist/mood_screen.h"
#include "database/tagcache.h"
#include "database/path_key.h"
#include "database/sound_index.h"
#include "database/sound_mix.h"
#include "database/sound_mood.h"
#include "metadata/book_resume.h"
#include "iap-library.h"

/* iAP's database categories */
enum {
    TYPE_PLAYLIST = 1,
    TYPE_ARTIST,
    TYPE_ALBUM,
    TYPE_GENRE,
    TYPE_TRACK,
    TYPE_COMPOSER,
    TYPE_AUDIOBOOK,
};

/* SelectSortDBRecord's orders */
enum {
    SORT_GENRE,
    SORT_ARTIST,
    SORT_COMPOSER,
    SORT_ALBUM,
    SORT_NAME,
    SORT_PLAYLIST,
    SORT_RELEASE_DATE,
};

#define PLAYLIST_ALL    0
#define PLAYLIST_QUEUE  1
#define INDEX_UP        0xFFFFFFFF /* a selection of -1 goes back up a level */

/* ------------------------------------------------------------------ *
 * the selection                                                      *
 * ------------------------------------------------------------------ */

enum { LEVEL_GENRE, LEVEL_ARTIST, LEVEL_COMPOSER, LEVEL_ALBUM, LEVEL_COUNT };

static const int level_tag[LEVEL_COUNT] = {
    tag_genre, tag_albumartist, tag_composer, tag_album
};

/* The seek of each level's chosen value, or -1 */
static long chosen[LEVEL_COUNT] = { -1, -1, -1, -1 };
static bool chose_book;    /* the album chosen is an audiobook */
/* Whether Songs at the top, with nothing chosen, is the whole library rather
 * than the Queue: set by ResetDBSelection and by choosing [All Tracks], so an
 * accessory's first look on connecting finds the Queue. */
static bool all_songs;
static int track_sort = IAP_LIBRARY_SORT_DEFAULT;

static int type_level(int type)
{
    switch (type)
    {
        case TYPE_GENRE:     return LEVEL_GENRE;
        case TYPE_ARTIST:    return LEVEL_ARTIST;
        case TYPE_COMPOSER:  return LEVEL_COMPOSER;
        case TYPE_ALBUM:
        case TYPE_AUDIOBOOK: return LEVEL_ALBUM;
        case TYPE_TRACK:     return LEVEL_COUNT;
    }
    return -1;
}

static bool nothing_chosen(void)
{
    for (int i = 0; i < LEVEL_COUNT; i++)
    {
        if (chosen[i] >= 0)
            return false;
    }
    return true;
}

static void clear_chosen(int from_level)
{
    for (int i = from_level; i < LEVEL_COUNT; i++)
        chosen[i] = -1;
    if (from_level <= LEVEL_ALBUM)
        chose_book = false;
}

/* ------------------------------------------------------------------ *
 * the block and its thread                                           *
 * ------------------------------------------------------------------ */

/* key is the seek a list is ordered and told apart by: the value's own for
 * a genre, artist, composer or album, the title's for a song. idx is the
 * song. */
struct entry {
    int32_t key;
    int32_t idx;
};

#define WORKER_STACK_SIZE (DEFAULT_STACK_SIZE * 12)

struct block {
    long stack[WORKER_STACK_SIZE / sizeof(long)];
    struct tagcache_search tcs;
    struct book_resume resume;
    char name[TAGCACHE_BUFSZ];
    char other[TAGCACHE_BUFSZ]; /* the second name a comparison reads */
    struct entry entries[];
};

/* Accessory Browsing, as list entries */
static const uint32_t size_entries[] = { 0, 5000, 10000, 20000, 40000 };

static int handle;
static struct block *block;
static uint32_t capacity;

static int list_type;           /* the category entries[] holds, or 0 */
static uint32_t list_count;
/* The database commit the lists and the selection were read under */
static int32_t lists_commit = -1;

enum { EV_PLAY = 1, EV_MIX, EV_EXIT };
static struct event_queue worker_q;
static unsigned int worker_id;
/* Set by the caller before it posts work and cleared by the worker; while it
 * is set the worker owns the block. */
static volatile bool building;
static volatile bool leaving;

/* What EV_PLAY plays, taken before the selection is cleared */
static bool play_book;
static long play_book_seek;

static void play_tracks(uint32_t start);
static void play_mix(int mix);

static void worker(void)
{
    struct queue_event ev;

    while (1)
    {
        queue_wait(&worker_q, &ev);
        if (ev.id == EV_EXIT)
            return;
        if (ev.id == EV_PLAY)
            play_tracks(ev.data);
        else if (ev.id == EV_MIX)
            play_mix(ev.data);
        building = false;
    }
}

static bool open_block(void)
{
    if (block)
        return true;

    uint32_t n = size_entries[global_settings.iap_browse_size];
    if (n == 0)
        return false;

    /* Immovable: the thread's stack is in it */
    handle = core_alloc_ex(sizeof(struct block) + n * sizeof(struct entry),
                           &buflib_ops_locked);
    if (handle <= 0)
    {
        handle = 0;
        return false;
    }
    block = core_get_data(handle);
    capacity = n;

    /* Not on the broadcast list, so it has no USB connection to answer */
    queue_init(&worker_q, false);
    worker_id = create_thread(worker, block->stack, sizeof(block->stack), 0,
                              "iap library" IF_PRIO(, PRIORITY_BACKGROUND)
                              IF_COP(, CPU));
    if (!worker_id)
    {
        queue_delete(&worker_q);
        core_free(handle);
        handle = 0;
        block = NULL;
        return false;
    }
    return true;
}

static void start_work(long id, intptr_t data)
{
    building = true;
    list_type = 0;
    queue_post(&worker_q, id, data);
}

/* ------------------------------------------------------------------ *
 * building a list                                                    *
 * ------------------------------------------------------------------ */

static struct tagcache_search_clause no_spoken = {
    .tag = tag_virt_spoken,
    .type = clause_is,
    .numeric = true,
    .source = source_constant,
    .numeric_data = 0,
    .str = NULL,
};

static struct tagcache_search_clause only_spoken = {
    .tag = tag_virt_spoken,
    .type = clause_is,
    .numeric = true,
    .source = source_constant,
    .numeric_data = 1,
    .str = NULL,
};

/* How the songs under the selection are ordered: by title alone, or by
 * year and then the group tag where either is set, then by album where
 * neither already is, then by disc and track. */
static bool title_only;
static bool by_year;
static int group_tag;

static void choose_track_order(void)
{
    title_only = false;
    by_year = false;
    group_tag = -1;

    /* An album keeps its own order, whatever is asked */
    if (chosen[LEVEL_ALBUM] >= 0)
        return;

    switch (track_sort)
    {
        case SORT_GENRE:        group_tag = tag_genre;       break;
        case SORT_ARTIST:       group_tag = tag_albumartist; break;
        case SORT_COMPOSER:     group_tag = tag_composer;    break;
        case SORT_ALBUM:        group_tag = tag_album;       break;
        case SORT_NAME:         title_only = true;           break;
        case SORT_RELEASE_DATE: by_year = true;              break;
        default:
            if (chosen[LEVEL_ARTIST] >= 0 || chosen[LEVEL_COMPOSER] >= 0)
                group_tag = tag_album;
            else
                title_only = true;
            break;
    }
}

static int sort_tag;

static int compare_numbers(long a, long b)
{
    return a < b ? -1 : a > b;
}

static int compare_key(const void *p1, const void *p2)
{
    const struct entry *e1 = p1, *e2 = p2;

    if (e1->key != e2->key)
        return compare_numbers(e1->key, e2->key);
    return compare_numbers(e1->idx, e2->idx);
}

static const char *value_name(const struct entry *e, int tag, char *buf)
{
    if (!tagcache_seek_string(tag, e->key, buf, TAGCACHE_BUFSZ))
        strmemccpy(buf, UNTAGGED, TAGCACHE_BUFSZ);
    return buf;
}

static int compare_name(const void *p1, const void *p2)
{
    int res = strcasecmp(
        tagcache_sort_name(value_name(p1, sort_tag, block->name)),
        tagcache_sort_name(value_name(p2, sort_tag, block->other)));

    return res ? res : compare_key(p1, p2);
}

/* Two songs by one of their string tags */
static int compare_song_tag(const struct entry *e1, const struct entry *e2,
                            int tag)
{
    if (!tagcache_entry_string(e1->idx, tag, block->name, sizeof(block->name)))
        *block->name = '\0';
    if (!tagcache_entry_string(e2->idx, tag, block->other,
                               sizeof(block->other)))
        *block->other = '\0';
    return strcasecmp(tagcache_sort_name(block->name),
                      tagcache_sort_name(block->other));
}

static int compare_song_number(const struct entry *e1, const struct entry *e2,
                               int tag)
{
    long n1 = 0, n2 = 0;

    tagcache_entry_numeric(e1->idx, tag, &n1);
    tagcache_entry_numeric(e2->idx, tag, &n2);
    return compare_numbers(n1, n2);
}

static int compare_track(const void *p1, const void *p2)
{
    const struct entry *e1 = p1, *e2 = p2;
    int res;

    if (title_only)
        return compare_key(p1, p2);

    if (by_year && (res = compare_song_number(e1, e2, tag_year)))
        return res;
    if (group_tag >= 0 && (res = compare_song_tag(e1, e2, group_tag)))
        return res;
    if ((by_year || group_tag >= 0) && group_tag != tag_album
        && (res = compare_song_tag(e1, e2, tag_album)))
        return res;
    if ((res = compare_song_number(e1, e2, tag_discnumber)))
        return res;
    if ((res = compare_song_number(e1, e2, tag_tracknumber)))
        return res;
    return compare_key(p1, p2);
}

/* Sorts a list of values and keeps one entry for each */
static uint32_t merge_values(struct entry *e, uint32_t n)
{
    uint32_t unique = 0;

    qsort(e, n, sizeof(*e), compare_key);
    for (uint32_t i = 0; i < n; i++)
    {
        if (unique == 0 || e[i].key != e[unique - 1].key)
            e[unique++] = e[i];
    }
    return unique;
}

/* Fills entries[] with the list of one category under the selection above
 * it. The database search reads RAM only, so it is quick enough to run on
 * the accessory's thread; tagcache_search_ready() keeps it from waiting out
 * a commit there. */
static bool build(int type)
{
    int level = type_level(type);
    bool books;
    struct tagcache_marks marks;

    if (level < 0 || building)
        return false;

    /* A commit while browsing moves every seek held here */
    tagcache_get_marks(&marks);
    if (marks.commitid != lists_commit)
    {
        lists_commit = marks.commitid;
        clear_chosen(0);
        list_type = 0;
    }

    books = type == TYPE_AUDIOBOOK || (level == LEVEL_COUNT && chose_book);
    if (list_type == type)
        return true;
    list_type = 0;

    if (!tagcache_is_in_ram() || !tagcache_search_ready() || !open_block())
        return false;

    int tag = level < LEVEL_COUNT ? level_tag[level] : tag_title;
    if (level == LEVEL_COUNT)
        choose_track_order();

    struct tagcache_search *tcs = &block->tcs;
    if (!tagcache_search(tcs, tag))
        return false;

    /* The audiobooks are a list of their own, under no selection */
    for (int i = 0; i < level && type != TYPE_AUDIOBOOK; i++)
    {
        if (chosen[i] >= 0)
            tagcache_search_add_filter(tcs, level_tag[i], chosen[i]);
    }
    if (books)
        tagcache_search_add_clause(tcs, &only_spoken);
    else if (global_settings.segregate_audiobooks)
        tagcache_search_add_clause(tcs, &no_spoken);

    /* With a filter or a clause the search yields every song, so a value
     * comes once per song holding it. Merging them whenever the block fills
     * means only a list of more distinct values than it holds is cut short. */
    struct entry *e = block->entries;
    uint32_t n = 0;
    while (tagcache_get_next(tcs, block->name, sizeof(block->name)))
    {
        if (n == capacity)
        {
            if (level < LEVEL_COUNT)
                n = merge_values(e, n);
            if (n == capacity)
                break;
        }
        e[n].key = tcs->result_seek;
        e[n].idx = tcs->idx_id;
        n++;
    }
    tagcache_search_finish(tcs);

    if (level == LEVEL_COUNT)
        qsort(e, n, sizeof(*e), compare_track);
    else
    {
        n = merge_values(e, n);

        /* The database keeps names in case-blind order already; only
         * Sort Ignoring The/A/An needs them read */
        if (tagcache_tag_skips_articles(tag))
        {
            sort_tag = tag;
            qsort(e, n, sizeof(*e), compare_name);
        }
    }

    list_type = type;
    list_count = n;
    return true;
}

/* ------------------------------------------------------------------ *
 * playing a list                                                     *
 * ------------------------------------------------------------------ */

/* Where the book being played was left, if it was */
static bool find_book_position(void)
{
    char book[BOOK_KEY_MAX];

    return tagcache_seek_string(tag_album, play_book_seek, book, sizeof(book))
           && book_resume_get(book, &block->resume);
}

/* On the worker: replaces the Queue with the song list in entries[] and
 * plays the one at start. A Queue that fills up keeps what it took. */
static void play_tracks(uint32_t start)
{
    struct tagcache_search *tcs = &block->tcs;
    struct playlist_insert_context context;
    bool resume;

    if (leaving || !tagcache_search_ready())
        return;
    resume = play_book && start == 0 && find_book_position();
    if (!tagcache_search(tcs, tag_filename))
        return;

    book_resume_save();
    if (playlist_create(NULL, NULL) < 0)
    {
        tagcache_search_finish(tcs);
        return;
    }
    if (playlist_insert_context_create(NULL, &context, PLAYLIST_INSERT_LAST,
                                       false, false) < 0)
    {
        /* create() keeps the playlist lock even when it fails */
        playlist_insert_context_release(&context);
        tagcache_search_finish(tcs);
        return;
    }

    /* A song whose file cannot be found is left out, which moves the
     * chosen one up by as many */
    uint32_t inserted = 0, first = 0;
    int left_at = -1;
    for (uint32_t i = 0; i < list_count && !leaving; i++)
    {
        if (i == start)
            first = inserted;
        if (!tagcache_retrieve(tcs, block->entries[i].idx, tag_filename,
                               block->name, sizeof(block->name)))
            continue;
        if (playlist_insert_context_add(&context, block->name) < 0)
            break;
        if (resume && left_at < 0 && !strcmp(block->name, block->resume.track))
            left_at = inserted;
        inserted++;
        yield();
    }
    playlist_insert_context_release(&context);
    tagcache_search_finish(tcs);

    if (inserted == 0 || leaving)
        return;

    int index = first < inserted ? (int)first : 0;
    unsigned long elapsed = 0, offset = 0;

    /* A book keeps its order, and picks up where it was left */
    if (left_at >= 0)
    {
        index = left_at;
        elapsed = block->resume.elapsed;
        offset = block->resume.offset;
    }
    else if (!play_book && global_settings.playlist_shuffle)
    {
        index = playlist_shuffle(current_tick, index);
        if (!global_settings.play_selected)
            index = 0;
    }
    playlist_start(index, elapsed, offset);
}

/* Hands the song list to the worker; the Queue is the Track category from
 * here on, growing while it builds */
static bool start_playing(uint32_t start)
{
    if (!build(TYPE_TRACK) || start >= list_count)
        return false;
    if (global_settings.party_mode && audio_status())
        return false;

    play_book = chose_book;
    play_book_seek = chosen[LEVEL_ALBUM];
    clear_chosen(0);
    all_songs = false;
    start_work(EV_PLAY, start);
    return true;
}

/* ------------------------------------------------------------------ *
 * the Playlist category                                              *
 * ------------------------------------------------------------------ */

/* The rows between the Queue and the saved playlists. Each is a folder: its
 * Track category lists what it holds, and choosing one of those plays it. */
enum { FOLDER_NONE, FOLDER_MOODS, FOLDER_JOURNEYS, FOLDER_BOOKS };
static int folder;

static int journeys_offered(void)
{
    int lang, from, to, n = 0;

    while (mood_screen_journey(n, &lang, &from, &to))
        n++;
    return n;
}

/* The folders offered, in order; returns how many */
static int find_folders(int *rows)
{
    int n = 0;

    /* Everything here needs the worker, and so the block */
    if (global_settings.iap_browse_size == 0)
        return 0;

    if (global_settings.playlist_engine && sound_index_exists())
    {
        rows[n++] = FOLDER_MOODS;
        rows[n++] = FOLDER_JOURNEYS;
    }
    if (tagcache_is_in_ram())
        rows[n++] = FOLDER_BOOKS;
    return n;
}

/* The folders as the Playlist category was last counted. An accessory counts
 * it before naming or choosing a playlist, and the engine's half is a file
 * check, so it is asked once a count rather than once a name. */
static int folder_rows[3];
static int folder_count_seen = -1;

static int folders_offered(const int **rows)
{
    if (folder_count_seen < 0)
        folder_count_seen = find_folders(folder_rows);
    *rows = folder_rows;
    return folder_count_seen;
}

/* A mood, or a journey after the last mood */
static bool mix_moods(int mix, int *lang, int *from, int *to)
{
    if (mix < MOOD_COUNT)
    {
        *lang = sound_mood_name(mix);
        *from = *to = mix;
        return true;
    }
    return mood_screen_journey(mix - MOOD_COUNT, lang, from, to);
}

/* On the worker */
static void play_mix(int mix)
{
    int lang, from, to;

    if (!leaving && mix_moods(mix, &lang, &from, &to))
        sound_mix_mood_unasked(from, to, global_settings.mix_length);
}

static bool is_playlist_file(const char *name)
{
    const char *ext = strrchr(name, '.');
    return ext && (!strcasecmp(ext, ".m3u") || !strcasecmp(ext, ".m3u8"));
}

/* Counts the catalogue's playlists, stopping at the nth (from 1) and copying
 * its filename; nth 0 counts them all. */
static uint32_t find_catalog_playlist(uint32_t nth, char *name, size_t size)
{
    DIR *dir = opendir(global_settings.playlist_catalog_dir);
    if (!dir)
        return 0;

    uint32_t n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (!is_playlist_file(entry->d_name))
            continue;
        if (++n == nth)
        {
            strmemccpy(name, entry->d_name, size);
            break;
        }
    }
    closedir(dir);
    return n;
}

static bool play_catalog_playlist(uint32_t nth)
{
    char file[MAX_PATH];

    if (find_catalog_playlist(nth, file, sizeof(file)) != nth)
        return false;

    book_resume_save();
    if (playlist_create(global_settings.playlist_catalog_dir, file) == -1)
        return false;
    if (global_settings.playlist_shuffle)
        playlist_shuffle(current_tick, -1);
    playlist_start(0, 0, 0);
    return true;
}

static bool playlist_record_name(uint32_t index, char *buf, size_t size)
{
    static const int folder_lang[] = {
        [FOLDER_MOODS] = LANG_MOODS,
        [FOLDER_JOURNEYS] = LANG_JOURNEYS,
        [FOLDER_BOOKS] = LANG_AUDIOBOOKS,
    };
    const int *rows;
    uint32_t folders = folders_offered(&rows);

    if (index == PLAYLIST_ALL)
        strmemccpy(buf, str(LANG_TAGNAVI_ALL_TRACKS), size);
    else if (index == PLAYLIST_QUEUE)
        strmemccpy(buf, str(LANG_CURRENT_PLAYLIST), size);
    else if (index <= PLAYLIST_QUEUE + folders)
        strmemccpy(buf, str(folder_lang[rows[index - PLAYLIST_QUEUE - 1]]),
                   size);
    else
    {
        *buf = '\0';
        find_catalog_playlist(index - PLAYLIST_QUEUE - folders, buf, size);
        char *dot = strrchr(buf, '.');
        if (dot)
            *dot = '\0';
    }
    return true;
}

static bool playlist_record_select(uint32_t index)
{
    const int *rows;
    uint32_t folders = folders_offered(&rows);

    if (index == INDEX_UP)
        return true;
    if (building)
        return false;

    clear_chosen(0);
    all_songs = false;
    folder = FOLDER_NONE;
    list_type = 0;

    if (index == PLAYLIST_ALL)
    {
        iap_library_reset();
        return true;
    }
    if (index == PLAYLIST_QUEUE)
        return true;
    if (index <= PLAYLIST_QUEUE + folders)
    {
        folder = rows[index - PLAYLIST_QUEUE - 1];
        return true;
    }
    if (global_settings.party_mode && audio_status())
        return false;
    return play_catalog_playlist(index - PLAYLIST_QUEUE - folders);
}

/* The Track category of a folder */
static bool folder_count(uint32_t *count)
{
    switch (folder)
    {
        case FOLDER_MOODS:
            *count = MOOD_COUNT;
            return true;
        case FOLDER_JOURNEYS:
            *count = journeys_offered();
            return true;
        case FOLDER_BOOKS:
            if (!build(TYPE_AUDIOBOOK))
                return false;
            *count = list_count;
            return true;
    }
    return false;
}

static bool folder_name(uint32_t index, char *buf, size_t size)
{
    int lang, from, to;

    switch (folder)
    {
        case FOLDER_MOODS:
        case FOLDER_JOURNEYS:
            if (index >= MOOD_COUNT
                || !mix_moods(index + (folder == FOLDER_JOURNEYS
                                       ? MOOD_COUNT : 0),
                              &lang, &from, &to))
                return false;
            strmemccpy(buf, str(lang), size);
            return true;
        case FOLDER_BOOKS:
            if (!build(TYPE_AUDIOBOOK) || index >= list_count)
                return false;
            if (!tagcache_seek_string(tag_album, block->entries[index].key,
                                      buf, size))
                strmemccpy(buf, UNTAGGED, size);
            return true;
    }
    return false;
}

static bool folder_select(uint32_t index)
{
    uint32_t count;
    int was = folder;

    if (index == INDEX_UP || !folder_count(&count) || index >= count)
        return false;
    if (global_settings.party_mode && audio_status())
        return false;

    if (was == FOLDER_BOOKS)
    {
        /* The book's chapters, played from where it was left */
        clear_chosen(0);
        chosen[LEVEL_ALBUM] = block->entries[index].key;
        chose_book = true;
        folder = FOLDER_NONE;
        list_type = 0;
        return start_playing(0);
    }

    if (!open_block())
        return false;
    folder = FOLDER_NONE;
    all_songs = false;
    start_work(EV_MIX, index + (was == FOLDER_JOURNEYS ? MOOD_COUNT : 0));
    return true;
}

/* ------------------------------------------------------------------ *
 * the entry points                                                   *
 * ------------------------------------------------------------------ */

bool iap_library_count(int type, uint32_t *count)
{
    if (type == TYPE_PLAYLIST)
    {
        folder_count_seen = find_folders(folder_rows);
        *count = PLAYLIST_QUEUE + 1 + folder_count_seen
                 + find_catalog_playlist(0, NULL, 0);
        return true;
    }
    if (type == TYPE_TRACK && iap_library_tracks_are_queue())
    {
        *count = playlist_amount();
        return true;
    }
    if (type == TYPE_TRACK && folder != FOLDER_NONE)
        return folder_count(count);
    if (!build(type))
        return false;
    *count = list_count;
    return true;
}

bool iap_library_name(int type, uint32_t index, char *buf, size_t size)
{
    if (type == TYPE_PLAYLIST)
        return playlist_record_name(index, buf, size);
    if (type == TYPE_TRACK && folder != FOLDER_NONE)
        return folder_name(index, buf, size);

    if (!build(type) || index >= list_count)
        return false;

    const struct entry *e = &block->entries[index];
    bool named = type == TYPE_TRACK
        ? tagcache_entry_string(e->idx, tag_title, buf, size)
        : tagcache_seek_string(level_tag[type_level(type)], e->key, buf, size);
    if (!named)
        strmemccpy(buf, UNTAGGED, size);
    return true;
}

bool iap_library_select(int type, uint32_t index, int sort)
{
    int level = type_level(type);

    if (type == TYPE_PLAYLIST)
        return playlist_record_select(index);
    if (level < 0)
        return false;
    if (level == LEVEL_COUNT)
    {
        if (folder != FOLDER_NONE)
            return folder_select(index);
        return index != INDEX_UP && start_playing(index);
    }

    long seek = -1;
    if (index != INDEX_UP)
    {
        if (!build(type) || index >= list_count)
            return false;
        seek = block->entries[index].key;
    }

    /* A choice clears the ones below it; a book stands alone */
    clear_chosen(type == TYPE_AUDIOBOOK ? 0 : level);
    chosen[level] = seek;
    chose_book = type == TYPE_AUDIOBOOK && seek >= 0;
    folder = FOLDER_NONE;
    track_sort = sort;
    list_type = 0;
    return true;
}

void iap_library_reset(void)
{
    clear_chosen(0);
    all_songs = global_settings.iap_browse_size != 0 && tagcache_is_in_ram();
    folder = FOLDER_NONE;
    track_sort = IAP_LIBRARY_SORT_DEFAULT;
    list_type = 0;
}

bool iap_library_tracks_are_queue(void)
{
    return building
           || (folder == FOLDER_NONE && !all_songs && nothing_chosen());
}

bool iap_library_building(void)
{
    return building;
}

void iap_library_close(void)
{
    if (worker_id)
    {
        leaving = true;
        queue_post(&worker_q, EV_EXIT, 0);
        thread_wait(worker_id);
        queue_delete(&worker_q);
        worker_id = 0;
        building = false;
        leaving = false;
    }
    if (handle > 0)
        core_free(handle);
    handle = 0;
    block = NULL;
    capacity = 0;
    folder_count_seen = -1;
    iap_library_reset();
    all_songs = false;
}

/* ------------------------------------------------------------------ *
 * the library as iAP2 sends it                                       *
 * ------------------------------------------------------------------ */

/* An album, artist, genre or composer's ID, from its name (and an album's
 * from its artist's too), so that it is the same in every track and at
 * every connection. Salted per kind, so an artist and an album of the same
 * name differ; never 0, which iAP2 reads as none. */
static uint64_t name_id(int kind, const char *name, const char *also)
{
    uint64_t h = path_key_fold_hash(name) ^ (uint64_t)kind << 56;
    if (also)
        h = (h * 1099511628211ull) ^ path_key_fold_hash(also);
    return h ? h : 1;
}

static char track_names[6][128];

int iap_library_track_slots(void)
{
    return tagcache_path_slots();
}

bool iap_library_track(int n, struct iap_library_track *t)
{
    static const int tags[6] = {
        tag_title, tag_album, tag_artist, tag_albumartist, tag_genre,
        tag_composer,
    };
    const char **names[6] = {
        &t->title, &t->album, &t->artist, &t->albumartist, &t->genre,
        &t->composer,
    };
    int idx;
    long v;

    memset(t, 0, sizeof(*t));
    if (!tagcache_path_slot(n, &t->key, &idx))
        return false;
    if (idx < 0)
    {
        t->key = 0;
        return true;
    }
    for (int i = 0; i < 6; i++)
        if (tagcache_entry_string(idx, tags[i], track_names[i],
                                  sizeof(track_names[i])))
            *names[i] = track_names[i];
    if (tagcache_entry_numeric(idx, tag_length, &v) && v > 0)
        t->length = v;
    if (tagcache_entry_numeric(idx, tag_tracknumber, &v) && v > 0)
        t->tracknum = v;
    if (tagcache_entry_numeric(idx, tag_discnumber, &v) && v > 0)
        t->discnum = v;

    if (t->album)
        t->album_id = name_id(1, t->album,
                              t->albumartist ? t->albumartist : t->artist);
    if (t->artist)
        t->artist_id = name_id(2, t->artist, NULL);
    if (t->albumartist)
        t->albumartist_id = name_id(2, t->albumartist, NULL);
    if (t->genre)
        t->genre_id = name_id(3, t->genre, NULL);
    if (t->composer)
        t->composer_id = name_id(4, t->composer, NULL);
    return true;
}

uint32_t iap_library_revision(void)
{
    uint64_t key;
    int idx;
    uint32_t live = 0;

    for (int n = 0; tagcache_path_slot(n, &key, &idx); n++)
        if (idx >= 0)
            live++;
    if (!live)
        return 0;
    /* A deletion leaves the commit count alone, so the live count too */
    uint32_t rev = (uint32_t)tagcache_commit_id() << 20 ^ live;
    return rev ? rev : 1;
}

uint64_t iap_library_key(const char *path)
{
    return path_key(path);
}

bool iap_library_play_keys(const uint8_t *keys, size_t n, uint32_t start)
{
    if (building || !open_block() || !n)
        return false;
    if (global_settings.party_mode && audio_status())
        return false;

    uint32_t count = 0, first = 0;
    for (size_t i = 0; i < n && count < capacity; i++)
    {
        uint64_t key = 0;
        for (int b = 0; b < 8; b++)
            key = key << 8 | keys[i * 8 + b];
        int idx = tagcache_find_key(key);
        if (idx < 0)
            continue;
        if (i <= start)
            first = count;
        block->entries[count].key = 0;
        block->entries[count].idx = idx;
        count++;
    }
    if (!count)
        return false;

    list_count = count;
    play_book = false;
    clear_chosen(0);
    all_songs = false;
    start_work(EV_PLAY, first);
    return true;
}
