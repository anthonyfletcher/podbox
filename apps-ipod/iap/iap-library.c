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
 * browsed or a car asks for a cover, sized by Accessory Browsing, and freed
 * when the accessory goes. A list longer than the block is cut short. The
 * block also holds the stack of a thread that builds the Queue and the
 * engine's playlists, which take longer than an accessory waits for a reply;
 * while it runs the Track category is the Queue as it grows, and the
 * database lists are refused.
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
 *   - playlists for iAP2: the Playlist category's rows as lists of keys,
 *     read together, and the Queue as one of its own
 *   - artwork for iAP2: the playing track's JPEG, read on the worker, and
 *     the next one's encoded ahead
 *   - the player's name
 ****************************************************************************/

#include "config.h"
#include <stdio.h>
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
#include "metadata/albumart.h"
#include "system/strutil.h"
#include "metadata/art_cache.h"
#include "draw/jpeg_enc.h"
#include "usb_log.h"
#include "storage.h"
#include "file.h"
#include "rbpaths.h"
#include "iap-library.h"
#include "iap-core.h"

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

/* iAP2's pools, in the block so they cost nothing with no accessory */
#define LIST_MAX  4096
#define LISTS_MAX IAP_LIBRARY_LISTS_MAX
#define QUEUE_MAX 300
#define BOOKS_MAX 256
#define BOOK_MAX  512
#define ART_JPEG_MAX (64 * 1024)    /* 25-50 KB a 300px cover at 85 */

struct block {
    long stack[WORKER_STACK_SIZE / sizeof(long)];
    struct tagcache_search tcs;
    struct book_resume resume;
    char name[TAGCACHE_BUFSZ];
    char other[TAGCACHE_BUFSZ]; /* the second name a comparison reads */
    /* iAP2's playlists and covers, the worker's alone; see their parts */
    uint64_t list_keys[LIST_MAX];
    struct iap_library_list lists[LISTS_MAX];
    uint64_t queue_keys[QUEUE_MAX];
    int32_t book_seeks[BOOKS_MAX];
    uint32_t book_order[BOOK_MAX];
    uint8_t art_jpeg[ART_JPEG_MAX];
    char art_jpeg_of[MAX_PATH];     /* the cache file art_jpeg holds */
    uint32_t art_jpeg_len;          /* and its length */
    unsigned int art_jpeg_gen;      /* art_cache_generation() it was made at */
    struct mp3entry art_next;       /* the track a prefetch encodes for */
    fb_data art_band[16 * ART_CACHE_MAX_DIM];
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

enum { EV_PLAY = 1, EV_MIX, EV_ARTWORK, EV_PREFETCH, EV_LIST, EV_EXIT };
static struct event_queue worker_q;
static unsigned int worker_id;
/* Set by the caller before it posts work and cleared by the worker; while it
 * is set the worker owns the block. */
static volatile bool building;
static volatile bool leaving;
/* IAP_LIST_*, the playlists' and the Queue's: see the playlists for iAP2 */
static volatile int lists_state, queue_state;

/* What EV_PLAY plays, taken before the selection is cleared */
static bool play_book;
static long play_book_seek;

static void play_tracks(uint32_t start);
static void play_mix(int mix);
static void read_artwork(intptr_t request);
static void prefetch_artwork(void);
static void read_lists(intptr_t what);

static void worker(void)
{
    struct queue_event ev;

    while (1)
    {
        queue_wait(&worker_q, &ev);
        if (ev.id == EV_EXIT)
            return;
        if (ev.id == EV_ARTWORK)
        {
            read_artwork(ev.data);  /* the lists are not its to finish */
            continue;
        }
        if (ev.id == EV_PREFETCH)
        {
            prefetch_artwork();
            continue;
        }
        if (ev.id == EV_LIST)
        {
            read_lists(ev.data);    /* nor are iAP2's */
            continue;
        }
        if (ev.id == EV_PLAY)
            play_tracks(ev.data);
        else if (ev.id == EV_MIX)
            play_mix(ev.data);
        building = false;
    }
}

/* lists is whether the caller needs entries[]; without, Accessory Browsing
 * Off still gets the worker and the buffers, with no entries */
static bool open_block(bool lists)
{
    uint32_t n = size_entries[global_settings.iap_browse_size];

    if (block)
        return !lists || capacity > 0;
    if (lists && n == 0)
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
    block->art_jpeg_of[0] = '\0';
    block->art_next.path[0] = '\0';
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

static void prefetch_give_way(void);

static void start_work(long id, intptr_t data)
{
    prefetch_give_way();
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
 * a commit there. A sort that reads two names a comparison is not: a list
 * longer than NAME_SORT_MAX keeps the database's own order instead, by title
 * for songs, and without the The/A/An skip for the rest. */
#define NAME_SORT_MAX 2000
static bool build(int type)
{
    int level = type_level(type);
    bool books;
    int32_t commit = tagcache_commit_id();

    if (level < 0 || building)
        return false;

    /* A commit while browsing moves every seek held here. Read per record
     * named, so the commit count alone: the marks walk the whole index. */
    if (commit != lists_commit)
    {
        lists_commit = commit;
        clear_chosen(0);
        list_type = 0;
    }

    books = type == TYPE_AUDIOBOOK || (level == LEVEL_COUNT && chose_book);
    if (list_type == type)
        return true;
    list_type = 0;

    /* The block first: allocating it can yield, and a commit begun in that
     * gap would hold the search up on this thread */
    if (!tagcache_is_in_ram() || !open_block(true) ||
        !tagcache_search_ready())
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
    {
        if (n > NAME_SORT_MAX && (group_tag >= 0 || by_year))
            title_only = true;
        qsort(e, n, sizeof(*e), compare_track);
    }
    else
    {
        n = merge_values(e, n);

        /* The database keeps names in case-blind order already; only
         * Sort Ignoring The/A/An needs them read */
        if (tagcache_tag_skips_articles(tag) && n <= NAME_SORT_MAX)
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

/* Songs added before an unshuffled Queue starts playing; the rest follow
 * while it plays */
#define PLAY_AHEAD 10

static bool insert_open(struct playlist_insert_context *context, int position)
{
    if (playlist_insert_context_create(NULL, context, position,
                                       false, false) < 0)
    {
        /* create() keeps the playlist lock even when it fails */
        playlist_insert_context_release(context);
        return false;
    }
    return true;
}

/* Adds entries[from] up to entries[to - 1] where the context puts them, or
 * from entries[to - 1] down to entries[from] when backwards. A song whose
 * file cannot be found is left out. The playlist is let go after each song,
 * so a track change or an accessory asking for a name waits for one song,
 * not the whole Queue. left_at, if given, is where the book's resume track
 * went. Returns false once the Queue is full or the accessory has gone. */
static bool insert_songs(struct playlist_insert_context *context,
                         uint32_t from, uint32_t to, bool backwards,
                         int *left_at)
{
    struct tagcache_search *tcs = &block->tcs;

    for (uint32_t k = from; k < to; k++)
    {
        uint32_t i = backwards ? to - 1 - (k - from) : k;

        if (leaving)
            return false;
        if (!tagcache_retrieve(tcs, block->entries[i].idx, tag_filename,
                               block->name, sizeof(block->name)))
            continue;
        if (left_at && *left_at < 0
            && path_key(block->name) == block->resume.track)
            *left_at = context->count;
        if (playlist_insert_context_add(context, block->name) < 0)
            return false;
        playlist_insert_context_yield(context);
    }
    return true;
}

/* An unshuffled Queue plays as soon as the chosen song and a few after it
 * are in. The rest of the songs after it go on the end and the ones before
 * it on the front, last first, so the order ends up the list's. */
static void play_tracks_now(uint32_t start)
{
    struct playlist_insert_context context;
    uint32_t ahead = MIN(start + PLAY_AHEAD, list_count);
    bool started = false, more;

    if (!insert_open(&context, PLAYLIST_INSERT_LAST))
        return;
    more = insert_songs(&context, start, ahead, false, NULL);
    playlist_insert_context_release(&context);
    if (leaving)
        return;
    if (context.count > 0)
    {
        playlist_start(0, 0, 0);
        started = true;
    }

    if (more && insert_open(&context, PLAYLIST_INSERT_LAST))
    {
        more = insert_songs(&context, ahead, list_count, false, NULL);
        playlist_insert_context_release(&context);
        if (!started && context.count > 0 && !leaving)
        {
            playlist_start(0, 0, 0);
            started = true;
        }
    }

    if (more && insert_open(&context, PLAYLIST_PREPEND))
    {
        insert_songs(&context, 0, start, true, NULL);
        playlist_insert_context_release(&context);
        if (!started && context.count > 0 && !leaving)
            playlist_start(0, 0, 0);
    }
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

    /* A shuffle needs every song, and a book its resume track found, before
     * either can start */
    if (!resume && (play_book || !global_settings.playlist_shuffle))
    {
        play_tracks_now(start);
        tagcache_search_finish(tcs);
        return;
    }

    if (!insert_open(&context, PLAYLIST_INSERT_LAST))
    {
        tagcache_search_finish(tcs);
        return;
    }

    /* A song left out moves the chosen one up by as many */
    int *resume_at = NULL, left_at = -1;
    if (resume)
        resume_at = &left_at;
    uint32_t first = 0;
    if (insert_songs(&context, 0, start, false, resume_at))
    {
        first = context.count;
        insert_songs(&context, start, list_count, false, resume_at);
    }
    uint32_t inserted = context.count;
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

static const int folder_lang[] = {
    [FOLDER_MOODS] = LANG_MOODS,
    [FOLDER_JOURNEYS] = LANG_JOURNEYS,
    [FOLDER_BOOKS] = LANG_AUDIOBOOKS,
};

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

/* The catalogue's filenames as the Playlist category was last counted, one
 * after another, so that naming a record reads RAM rather than the
 * directory. A catalogue that does not fit is scanned for each name. */
static char catalog_names[2048];
static uint32_t catalog_count;
static bool catalog_cached;

static uint32_t count_catalog(void)
{
    DIR *dir = opendir(global_settings.playlist_catalog_dir);
    size_t used = 0;

    catalog_count = 0;
    catalog_cached = false;
    if (!dir)
        return 0;

    bool fits = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (!is_playlist_file(entry->d_name))
            continue;
        catalog_count++;
        size_t len = strlen(entry->d_name) + 1;
        if (fits && used + len <= sizeof(catalog_names))
        {
            memcpy(catalog_names + used, entry->d_name, len);
            used += len;
        }
        else
            fits = false;
    }
    closedir(dir);
    catalog_cached = fits;
    return catalog_count;
}

/* The nth (from 1) playlist's filename, or false */
static bool catalog_name(uint32_t nth, char *name, size_t size)
{
    if (!catalog_cached)
        return find_catalog_playlist(nth, name, size) == nth;
    if (nth == 0 || nth > catalog_count)
        return false;

    const char *p = catalog_names;
    while (--nth)
        p += strlen(p) + 1;
    strmemccpy(name, p, size);
    return true;
}

static bool play_catalog_playlist(uint32_t nth)
{
    char file[MAX_PATH];

    if (!catalog_name(nth, file, sizeof(file)))
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
        catalog_name(index - PLAYLIST_QUEUE - folders, buf, size);
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

    if (!open_block(true))
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
                 + count_catalog();
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
    catalog_cached = false;
    lists_state = queue_state = IAP_LIST_IDLE;
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
        {
            iap_utf8_cut(track_names[i]);
            *names[i] = track_names[i];
        }
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

    /* Accessory Browsing Off has no entries to play a car's choice from, so
     * the car is sent no library, as when the database is not in RAM. A
     * block opened while it was Off has none until the accessory leaves. */
    if (!size_entries[global_settings.iap_browse_size] || (block && !capacity))
        return 0;
    for (int n = 0; tagcache_path_slot(n, &key, &idx); n++)
        if (idx >= 0)
            live++;
    if (!live)
        return 0;
    /* A deletion leaves the commit count alone, so the live count too */
    uint32_t rev = (uint32_t)tagcache_commit_id() << 20 ^ live;
    /* 0 and 1 are the car's revisions for a partial library and for the
     * playing track alone */
    return rev > 1 ? rev : 2;
}

uint64_t iap_library_key(const char *path)
{
    return path_key(path);
}

bool iap_library_play_keys(const uint8_t *keys, size_t n, uint32_t start)
{
    if (building || !open_block(true) || !n)
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

void iap_library_shuffle_toggle(void)
{
    iap_shuffle_state(!global_settings.playlist_shuffle);
}

void iap_library_repeat_next(void)
{
    iap_repeat_next();
}

/* ------------------------------------------------------------------ *
 * playlists for iAP2                                                 *
 * ------------------------------------------------------------------ */

/* iAP2's playlists are the Playlist category's rows, without [All Tracks]
 * and without the engine's Moods and Journeys: the Queue, Audiobooks and
 * each book, then the catalogue. A car names a playlist's tracks to play it,
 * never the playlist, so a mix would have to be made for every connection,
 * not when chosen; seconds each on the 5G, with its audio heard to stutter.
 * All are read on the worker in one go, every list of track keys into one
 * pool, so that the car is sent them together: it reloads its library at
 * each update. A list that no longer
 * fits the pool is left out, and so are rows past LISTS_MAX; an audiobook
 * keeps its first BOOK_MAX tracks and the Queue its first QUEUE_MAX. The
 * Queue as a list of its own, for the car's queue, has a pool of its own.
 * Trap: the Queue is read an entry at a time from the playlist's file, and
 * a few thousand hold the 5G's worker for 20 seconds. */
static int lists_n;
static struct iap_library_list queue_list;

static struct {
    int folders[3], n_folders;
    int books;
    uint32_t catalog;
} rows;

/* The audiobooks' album seeks, in the database's order */
static int find_books(void)
{
    struct tagcache_search *tcs = &block->tcs;
    int n = 0;

    if (!tagcache_search(tcs, tag_album))
        return 0;
    tagcache_search_add_clause(tcs, &only_spoken);
    while (n < BOOKS_MAX
           && tagcache_get_next(tcs, block->name, sizeof(block->name)))
    {
        int i = 0;
        while (i < n && block->book_seeks[i] != tcs->result_seek)
            i++;
        if (i == n)
            block->book_seeks[n++] = tcs->result_seek;
    }
    tagcache_search_finish(tcs);
    return n;
}

static void find_rows(void)
{
    int all[3];
    const int n = find_folders(all);

    rows.n_folders = rows.books = 0;
    for (int i = 0; i < n; i++)
        if (all[i] == FOLDER_BOOKS)
        {
            rows.folders[rows.n_folders++] = all[i];
            rows.books = find_books();
        }
    rows.catalog = count_catalog();
}

/* The Queue as it plays, from the track playing at the start */
static uint32_t queue_keys(uint64_t *keys, uint32_t room)
{
    struct playlist_track_info info;
    const int amount = playlist_amount();
    const int first = playlist_get_first_index(NULL);
    uint32_t n = 0;

    room = MIN(room, QUEUE_MAX);
    for (int d = 0; d < amount && n < room && !leaving; d++)
    {
        if (playlist_get_track_info(NULL, (first + d) % amount, &info) < 0)
            break;
        keys[n++] = path_key(info.filename);
        if (!(d & 31))
            yield();
    }
    return n;
}

/* A book's tracks, by disc and track number */
static uint32_t book_keys(int32_t seek, uint64_t *keys, uint32_t room)
{
    struct tagcache_search *tcs = &block->tcs;
    uint32_t n = 0;

    room = MIN(room, BOOK_MAX);
    if (!tagcache_search(tcs, tag_filename))
        return 0;
    tagcache_search_add_filter(tcs, tag_album, seek);
    tagcache_search_add_clause(tcs, &only_spoken);
    while (n < room
           && tagcache_get_next(tcs, block->name, sizeof(block->name)))
    {
        const uint32_t order =
            (uint32_t)tagcache_get_numeric(tcs, tag_discnumber) << 16
            | (tagcache_get_numeric(tcs, tag_tracknumber) & 0xffff);
        const uint64_t key = path_key(block->name);
        uint32_t i = n++;
        for (; i > 0 && block->book_order[i - 1] > order; i--)
        {
            block->book_order[i] = block->book_order[i - 1];
            keys[i] = keys[i - 1];
        }
        block->book_order[i] = order;
        keys[i] = key;
    }
    tagcache_search_finish(tcs);
    return n;
}

/* A saved playlist's tracks that the database holds, relative paths read
 * from the playlist's own folder */
static uint32_t file_keys(const char *path, uint64_t *keys, uint32_t room)
{
    char line[MAX_PATH], full[MAX_PATH];
    uint32_t n = 0;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return 0;
    for (int i = 0; n < room && !leaving
                    && read_line(fd, line, sizeof(line)) > 0; i++)
    {
        if (!i && !memcmp(line, "\xef\xbb\xbf", 3))  /* a byte order mark */
            memmove(line, line + 3, strlen(line + 3) + 1);
        if (*line == '#' || !*line)
            continue;
        /* As playing it would: relative, drive letters, dot segments, and
         * a .m3u's Latin-1 */
        if (playlist_line_path(path, line, full, sizeof(full)) <= 0)
            continue;
        const uint64_t key = path_key(full);
        if (tagcache_find_key(key) >= 0)
            keys[n++] = key;
        if (!(i & 31))
            yield();
    }
    close(fd);
    return n;
}

static void list_name(struct iap_library_list *l, const char *name)
{
    strmemccpy(l->name, name, sizeof(l->name));
    iap_utf8_cut(l->name);
}

/* One of Audiobooks' rows: a book */
static void read_book(struct iap_library_list *l, int index,
                      const char *folder_name, uint32_t room)
{
    if (!tagcache_seek_string(tag_album, block->book_seeks[index], block->name,
                              sizeof(block->name)))
        strmemccpy(block->name, UNTAGGED, sizeof(block->name));
    list_name(l, block->name);
    l->count = book_keys(block->book_seeks[index], (uint64_t *)l->keys, room);
    l->id = name_id(5, l->name, folder_name);
}

/* One row, its keys at 'keys'; false past the last */
static bool read_row(int row, struct iap_library_list *l, uint64_t *keys,
                     uint32_t room)
{
    memset(l, 0, sizeof(*l));
    l->keys = keys;
    if (row == 0)
    {
        list_name(l, str(LANG_CURRENT_PLAYLIST));
        l->id = name_id(7, l->name, NULL);
        l->count = queue_keys(keys, room);
        return true;
    }

    int r = row - 1;
    for (int f = 0; f < rows.n_folders; f++)
    {
        const char *folder_name = str(folder_lang[rows.folders[f]]);
        const int held = rows.books;
        if (r == 0)
        {
            list_name(l, folder_name);
            l->id = name_id(5, folder_name, NULL);
            l->folder = true;
            return true;
        }
        if (r <= held)
        {
            read_book(l, r - 1, folder_name, room);
            l->parent = name_id(5, folder_name, NULL);
            return true;
        }
        r -= 1 + held;
    }
    if (r < 0 || (uint32_t)r >= rows.catalog)
        return false;

    char path[MAX_PATH], name[MAX_PATH];
    if (catalog_name(r + 1, name, sizeof(name)))
    {
        snprintf(path, sizeof(path), "%s/%s",
                 global_settings.playlist_catalog_dir, name);
        char *dot = strrchr(name, '.');
        if (dot)
            *dot = '\0';
        list_name(l, name);
        l->id = name_id(6, l->name, NULL);
        l->count = file_keys(path, keys, room);
    }
    return true;
}

/* On the worker: every row, or (what 1) the Queue alone */
static void read_lists(intptr_t what)
{
    if (what)
    {
        memset(&queue_list, 0, sizeof(queue_list));
        queue_list.keys = block->queue_keys;
        if (!leaving)
            queue_list.count = queue_keys(block->queue_keys, QUEUE_MAX);
        queue_state = leaving ? IAP_LIST_IDLE : IAP_LIST_READY;
        return;
    }

    uint32_t used = 0;
    lists_n = 0;
    find_rows();
    for (int row = 0; lists_n < LISTS_MAX && !leaving; row++)
    {
        struct iap_library_list *l = &block->lists[lists_n];
        if (!read_row(row, l, block->list_keys + used, LIST_MAX - used))
            break;
        if (l->folder || l->count)
        {
            used += l->count;
            lists_n++;
        }
    }
    lists_state = leaving ? IAP_LIST_IDLE : IAP_LIST_READY;
}

bool iap_library_playlists_ask(void)
{
    if (lists_state == IAP_LIST_BUSY || !open_block(false))
        return false;
    lists_state = IAP_LIST_BUSY;
    prefetch_give_way();
    queue_post(&worker_q, EV_LIST, 0);
    return true;
}

int iap_library_playlists(const struct iap_library_list **all)
{
    if (lists_state != IAP_LIST_READY)
        return -1;
    *all = block->lists;
    return lists_n;
}

void iap_library_playlists_done(void)
{
    if (lists_state == IAP_LIST_READY)
        lists_state = IAP_LIST_IDLE;
}

bool iap_library_queue_ask(void)
{
    if (queue_state == IAP_LIST_BUSY || !open_block(false))
        return false;
    queue_state = IAP_LIST_BUSY;
    prefetch_give_way();
    queue_post(&worker_q, EV_LIST, 1);
    return true;
}

const struct iap_library_list *iap_library_queue(void)
{
    return queue_state == IAP_LIST_READY ? &queue_list : NULL;
}

void iap_library_queue_done(void)
{
    if (queue_state == IAP_LIST_READY)
        queue_state = IAP_LIST_IDLE;
}

/* ------------------------------------------------------------------ *
 * artwork for iAP2                                                   *
 * ------------------------------------------------------------------ */

/* The track's cover as a JPEG, from where Car Artwork says by Album Art's
 * rules (albumart_find_source()), read on the worker, never the USB thread,
 * into a ring of blocks the USB side sends from a packet's worth at a time.
 * Trap: a read per packet seeks the disk away from the buffering thread
 * hundreds of times a cover, and on the 5G the new track's audio runs dry.
 * An image is sent as stored, and larger ones than ART_MAX not at all; a
 * cache thumbnail is encoded as a JPEG first, into art_jpeg. The next
 * track's thumbnail is encoded there ahead of time, once the playing one's
 * cover has gone, so that it is ready the moment the track starts. */
#define ART_MAX   (512 * 1024)
#define ART_CHUNK 1000
#define ART_BLOCK (8 * ART_CHUNK)
#define ART_BUFS  4

static struct mp3entry art_track;   /* the request, copied */
static struct {
    volatile int state;             /* IAP_ART_* */
    volatile bool cancel;
    volatile bool reading;          /* the worker is in read_artwork() */
    unsigned request;               /* the latest find's number */
    volatile uint32_t size;
    uint8_t buf[ART_BUFS][ART_BLOCK];
    volatile size_t len[ART_BUFS];  /* bytes ready in each, 0 when free */
    unsigned rd, wr;
    size_t off;                     /* bytes of buf[rd] already sent */
    volatile bool prefetching;      /* an EV_PREFETCH is posted or running */
    volatile bool prefetch_stop;    /* a different track wants the cover */
} art;

static struct albumart_source art_source;

/* Work the car is waiting on goes ahead of a cover nobody has asked for: the
 * worker takes one event at a time, and an encode is seconds on the 5G. */
static void prefetch_give_way(void)
{
    if (art.prefetching)
        art.prefetch_stop = true;
}

struct aat_read
{
    int fd;
    int width;
    bool prefetch;  /* stopped by prefetch_stop */
};

static bool aat_rows(void *ctx, fb_data *band, int y, int rows)
{
    const struct aat_read *a = ctx;
    const ssize_t n = (ssize_t)rows * a->width * FB_DATA_SZ;
    (void)y;    /* the rows come in order */
    if (a->prefetch && (art.prefetch_stop || leaving))
        return false;
    return read(a->fd, band, n) == n;
}

/* A cache thumbnail as a JPEG in the block's art_jpeg: its length, or 0. A
 * cover too busy to fit at 85 is tried at 60 before it is given up. */
static uint32_t encode_cache(const char *path, bool prefetch)
{
    static const int quality[] = { 85, 60 };
    struct art_cache_header hdr;
    int n = -1;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return 0;
    if (read(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr) &&
        hdr.magic == ART_CACHE_MAGIC &&
        hdr.version == ART_CACHE_FORMAT_VERSION && hdr.layout == AA_ROWS &&
        hdr.width > 0 && hdr.width <= ART_CACHE_MAX_DIM &&
        hdr.height > 0 && hdr.height <= ART_CACHE_MAX_DIM)
    {
        struct aat_read a = { fd, hdr.width, prefetch };
        const struct jpeg_enc_src src =
            { hdr.width, hdr.height, block->art_band, aat_rows, &a };
        for (size_t i = 0; n < 0 && i < ARRAYLEN(quality); i++)
            if (lseek(fd, sizeof(hdr), SEEK_SET) == (off_t)sizeof(hdr))
                n = jpeg_encode(&src, quality[i], block->art_jpeg,
                                sizeof(block->art_jpeg));
    }
    close(fd);
    return n > 0 ? n : 0;
}

/* The thumbnail's JPEG: art_jpeg as it is when it holds this file's, which
 * an album's tracks share, and the art cache has not changed since; else
 * encoded now. A stopped prefetch or a failed encode leaves art_jpeg holding
 * nobody's, so the next ask tries again. */
static uint32_t cache_jpeg(const char *path, bool prefetch)
{
    unsigned int gen = art_cache_generation();

    if (strcmp(path, block->art_jpeg_of) || gen != block->art_jpeg_gen)
    {
        block->art_jpeg_of[0] = '\0';
        block->art_jpeg_gen = gen;
        block->art_jpeg_len = encode_cache(path, prefetch);
        if ((prefetch && art.prefetch_stop) || block->art_jpeg_len == 0)
            return 0;
        strlcpy(block->art_jpeg_of, path, sizeof(block->art_jpeg_of));
    }
    return block->art_jpeg_len;
}

/* Album Art's preference for the car, or AA_OFF */
static int car_preference(void)
{
    if (global_settings.car_artwork == CAR_ARTWORK_AS_ALBUM_ART)
        return global_settings.album_art;
    return global_settings.car_artwork - 1;
}

static void read_artwork(intptr_t request)
{
    int fd = -1;
    uint32_t left = 0, sent = 0;
    const long start = current_tick;

    /* Trap: a find that timed out can still be queued when the next is
     * posted, and reading both fills the ring a second time with nobody
     * left to drain it, which holds the worker until unplug */
    if ((unsigned)request != art.request)
        return;
    art.reading = true;
#ifdef HAVE_PRIORITY_SCHEDULING
    /* Trap: at background priority the worker waits behind the codec and
     * the library sync, seconds before the first bytes and a cover at a
     * fraction of the 140 KB/s the car takes from an iPhone */
    int priority = thread_set_priority(worker_id, PRIORITY_USER_INTERFACE);
#endif
    if (!albumart_find_source(&art_track, car_preference(), true,
                              &art_source))
        art_source.kind = AA_SOURCE_NONE;
    if (art_source.kind == AA_SOURCE_CACHE)
    {
#ifdef HAVE_PRIORITY_SCHEDULING
        /* Trap: encoding at the raised priority takes the CPU from the
         * codec just as the new track starts */
        thread_set_priority(worker_id, priority);
#endif
        left = cache_jpeg(art_source.path, false);
        /* Unreadable, or too busy even at 60: the image itself, if any */
        if (!left &&
            (!albumart_find_source(&art_track, AA_PREFER_EMBEDDED, true,
                                   &art_source) ||
             art_source.kind == AA_SOURCE_CACHE))
            art_source.kind = AA_SOURCE_NONE;
    }
    if (art_source.kind != AA_SOURCE_NONE &&
        art_source.kind != AA_SOURCE_CACHE &&
        (fd = open(art_source.path, O_RDONLY)) >= 0)
    {
        off_t size = art_source.size;
        if (size < 0)
            size = lseek(fd, 0, SEEK_END);
        if (size > 0 && size <= ART_MAX &&
            lseek(fd, art_source.pos, SEEK_SET) == art_source.pos)
            left = size;
    }

    usb_log(USB_LOG_IAP2_EVENT, USB_LOG_IAP2_COVER, 0,
            (uint32_t)(left ? art_source.kind : AA_SOURCE_NONE) << 24 | left,
            (current_tick - start) * 1000 / HZ);
    art.size = left;
    art.state = left ? IAP_ART_FOUND : IAP_ART_NONE;
    while (left && !art.cancel && !leaving)
    {
        if (art.len[art.wr])
        {
            sleep(1);       /* every buffer waits for the USB side */
            continue;
        }
        ssize_t got = MIN(left, ART_BLOCK);
        if (fd >= 0)
            got = read(fd, art.buf[art.wr], got);
        else
            memcpy(art.buf[art.wr], block->art_jpeg + sent, got);
        if (got <= 0)
        {
            art.state = IAP_ART_FAILED;
            break;
        }
        left -= got;
        sent += got;
        art.len[art.wr] = got;
        art.wr = (art.wr + 1) % ART_BUFS;
    }
    if (fd >= 0)
        close(fd);
#ifdef HAVE_PRIORITY_SCHEDULING
    thread_set_priority(worker_id, priority);
#endif
    art.reading = false;
}

/* Trap: a car's pick starts its first track while the worker is still
 * building the queue; the find waits behind that rather than giving up. */
bool iap_library_artwork_find(const struct mp3entry *id3)
{
    if (art.reading || !open_block(false))
        return false;
    if (art.prefetching && strcmp(id3->path, block->art_next.path))
        art.prefetch_stop = true;
    copy_mp3entry(&art_track, id3);
    art.cancel = false;
    art.size = 0;
    memset((void *)art.len, 0, sizeof(art.len));
    art.rd = art.wr = 0;
    art.off = 0;
    art.state = IAP_ART_FINDING;
    queue_post(&worker_q, EV_ARTWORK, ++art.request);
    return true;
}

static void prefetch_artwork(void)
{
    if (!art.prefetch_stop &&
        albumart_find_source(&block->art_next, car_preference(), true,
                             &art_source) &&
        art_source.kind == AA_SOURCE_CACHE)
        cache_jpeg(art_source.path, true);
    art.prefetching = false;
}

/* Looked at once a second, and taken once a track: only while the disk is
 * spinning, or in the playing track's last PREFETCH_LEAD ms, when the cover
 * would be read at the change anyway. The lead covers the two to three
 * seconds the 5G takes to encode one. */
#define PREFETCH_LEAD (20 * 1000)

void iap_library_artwork_prefetch(void)
{
    static long next_look;
    const struct mp3entry *id3;

    if (!block || building || art.reading || art.prefetching ||
        car_preference() == AA_OFF || TIME_BEFORE(current_tick, next_look))
        return;
    next_look = current_tick + HZ;
    id3 = audio_current_track();
    if (!id3 || (!storage_disk_is_active() &&
                 id3->elapsed + PREFETCH_LEAD < id3->length))
        return;
    id3 = audio_next_track();
    if (!id3 || !strcmp(id3->path, block->art_next.path))
        return;
    copy_mp3entry(&block->art_next, id3);
    art.prefetch_stop = false;
    art.prefetching = true;
    queue_post(&worker_q, EV_PREFETCH, 0);
}

int iap_library_artwork_state(uint32_t *size)
{
    *size = art.size;
    return art.state;
}

size_t iap_library_artwork_chunk(const uint8_t **data)
{
    *data = art.buf[art.rd] + art.off;
    return MIN(art.len[art.rd] - art.off, ART_CHUNK);
}

void iap_library_artwork_next(void)
{
    art.off += MIN(art.len[art.rd] - art.off, ART_CHUNK);
    if (art.off < art.len[art.rd])
        return;
    art.off = 0;
    art.len[art.rd] = 0;
    art.rd = (art.rd + 1) % ART_BUFS;
}

void iap_library_artwork_stop(void)
{
    art.cancel = true;
}

/* ------------------------------------------------------------------ *
 * the player's name                                                  *
 * ------------------------------------------------------------------ */

#define PLAYER_NAME_FILE ROCKBOX_DIR "/playername.txt"

/* Read once, at boot: an accessory asks mid-exchange, where a read could wait
 * out the disk spinning up */
static char player_name[32];

void iap_player_name_load(void)
{
    char raw[3 + sizeof(player_name)];
    char *name = raw;
    ssize_t got = -1;
    int fd = open(PLAYER_NAME_FILE, O_RDONLY);

    if (fd >= 0)
    {
        got = read(fd, raw, sizeof(raw) - 1);
        close(fd);
    }
    raw[got > 0 ? got : 0] = '\0';
    if (!strncmp(raw, "\xEF\xBB\xBF", 3))
        name += 3;
    name[strcspn(name, "\r\n")] = '\0';
    strlcpy(player_name, name, sizeof(player_name));
    iap_utf8_cut(player_name);

    /* Missing, empty or the model's name: PodBox's is written for the
     * owner to change */
    if (player_name[0] == '\0' || !strcmp(player_name, MODEL_NAME))
    {
        strlcpy(player_name, IAP_PLAYER_NAME_DEFAULT, sizeof(player_name));
        fd = open(PLAYER_NAME_FILE, O_CREAT|O_WRONLY|O_TRUNC, 0666);
        if (fd >= 0)
        {
            fdprintf(fd, "%s", player_name);
            close(fd);
        }
    }
}

void iap_player_name(char *buf, size_t size)
{
    strlcpy(buf, player_name[0] ? player_name : IAP_PLAYER_NAME_DEFAULT,
            size);
    iap_utf8_cut(buf);
}

void iap_utf8_cut(char *s)
{
    size_t n = strlen(s), i = n;

    while (i && ((unsigned char)s[i - 1] & 0xC0) == 0x80)
        i--;
    if (i && (unsigned char)s[i - 1] >= 0xC0)
    {
        unsigned char lead = s[i - 1];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;

        if (n - (i - 1) < need)
            s[i - 1] = '\0';
    }
}
