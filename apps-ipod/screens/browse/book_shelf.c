/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The Audiobooks shelf's three states: the books in progress, the ones not
 * started, and the ones finished.
 *
 * None of the three can be written in tagnavi.config. A condition there picks
 * tracks and then groups them, so a book appears if any one of its tracks
 * matches -- a half-read book would be "not started" by its unread chapters --
 * and "finished" is about a book's last track, which no condition can name.
 *
 * Two sources, and each answers what the other cannot. The resume file
 * (metadata/book_resume.c) knows where each recent book was left and whether
 * its saved track played to its end; the database knows every book, how long
 * it is, and which of its tracks have been played at all. A book with a
 * resume line is in progress unless that line says its last track ended. A
 * book without one is finished if its last track has been played, not started
 * if nothing of it has, and in progress otherwise.
 *
 * A book is a row of the database's album tables that is spoken word and
 * nothing else: an album by one album artist, keyed as the resume file keys
 * it. Its tracks are that album's spoken ones.
 * Podcasts are left out: a show has no last episode to have finished.
 *
 * Choosing a book plays it. An In progress one resumes; the others start at
 * the beginning. A book's context menu is the one it has in the browser:
 * Add to Queue, Add to Playlist, Show Track Info, Show in Files, and Mark as,
 * which marks it Finished, Not started or In progress by hand and moves it to
 * that shelf until it is played again.
 *
 * Parts, in order:
 *   - the arena and what it holds
 *   - collecting: the resume file, the books, their tracks
 *   - deciding each book's state
 *   - the list
 *   - playing a book
 *   - a book's context menu
 *   - the way in
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "cpu.h"                      /* cpu_boost */
#include "file.h"                     /* MAX_PATH */
#include "dir.h"                      /* ATTR_DIRECTORY */
#include "audio.h"
#include "lang.h"
#include "strnatcmp.h"
#include "widgets/list.h"
#include "widgets/menu.h"             /* MENUITEM_STRINGLIST, do_menu */
#include "widgets/splash.h"
#include "settings/settings.h"
#include "system/activity.h"
#include "system/app_buffer.h"
#include "system/app_util.h"          /* warn_on_pl_erase */
#include "database/tagcache.h"
#include "database/db_spoken.h"
#include "database/path_key.h"
#include "metadata/book_resume.h"
#include "playlist/playlist.h"
#include "screens/context_menu.h"     /* the queue and playlist submenus */
#include "system/strutil.h"           /* open_utf8 */
#include "viewers/properties.h"
#include "root_menu.h"
#include "book_shelf.h"

/* Books past this are not collected. A shelf that long is a library this
 * screen was not written for, and the list says nothing about the rest. */
#define BOOKS_MAX 1024

/* As many as the resume file keeps. */
#define RESUMES_MAX BOOK_RESUME_MAX

/* ------------------------------------------------------------------ *
 * the arena                                                          *
 * ------------------------------------------------------------------ */

struct shelf_resume
{
    uint64_t book;              /* book_resume_key() */
    struct book_resume pos;
    int owner;                  /* its books[] entry, or -1 */
    int32_t path;               /* into the arena, for a book keyed by its
                                   file, or -1 */
};

struct shelf_book
{
    uint64_t key;               /* book_resume_key() of its id */
    long     seek;              /* the album tag's seek */
    long     artist_seek;       /* and the album artist's */
    int      row;               /* its album-table row */
    uint32_t name;              /* the album, into the arena */
    uint32_t id;                /* its book_resume_id(), into the arena */
    int      played;            /* tracks with a playcount */
    unsigned long length;       /* all its tracks, ms */
    unsigned long before;       /* the tracks ahead of the resume track, ms */
    long     lastplayed;
    uint32_t last_key;          /* the highest disc/track number */
    bool     last_heard;        /* whether that track has been played */
    bool     counted;           /* at least one track that is not a podcast */
    int      resume;            /* its resumes[] entry, or -1 */
    uint32_t resume_key;        /* the resume track's disc/track number */
    bool     resume_found;
};

/* Rows are books[] indices; a resume line with no book behind it -- one
 * keyed by its file's path -- is -(its resumes[] index) - 1. */
#define ROW_RESUME(r)     (-(r) - 1)
#define ROW_IS_RESUME(v)  ((v) < 0)
#define ROW_RESUME_OF(v)  (-(v) - 1)

/* The app buffer, claimed while the list is up. Laid out as the arrays below
 * with the book names above them. */
static struct shelf_book   *books;
static struct shelf_resume *resumes;
static int                 *rows;
static char  *names;
static size_t names_sz;
static size_t names_used;
static int book_ct, resume_ct, row_ct;

static enum book_shelf shelf_kind;

static bool claim(void)
{
    size_t sz, fixed;
    char *p = app_claim_buffer(&sz, "book shelf");

    fixed = BOOKS_MAX * sizeof(*books) + RESUMES_MAX * sizeof(*resumes)
          + (BOOKS_MAX + RESUMES_MAX) * sizeof(*rows);
    if (p == NULL || sz < fixed + MAX_PATH)
    {
        if (p != NULL)
            app_release_buffer("book shelf");
        return false;
    }

    books   = (struct shelf_book *)p;   p += BOOKS_MAX * sizeof(*books);
    resumes = (struct shelf_resume *)p; p += RESUMES_MAX * sizeof(*resumes);
    rows    = (int *)p;                 p += (BOOKS_MAX + RESUMES_MAX)
                                             * sizeof(*rows);
    names    = p;
    names_sz = sz - fixed;

    book_ct = resume_ct = row_ct = 0;
    return true;
}

static void release(void)
{
    app_release_buffer("book shelf");
}

static const char *book_name(int b)
{
    return names + books[b].name;
}

static const char *book_id(int b)
{
    return names + books[b].id;
}

/* ------------------------------------------------------------------ *
 * collecting                                                         *
 * ------------------------------------------------------------------ */

/* A track's place in its book: disc above track, so disc 2's first follows
 * disc 1's last. Both masked -- they come from file tags, and a nonsense value
 * must not reach the sign bit and sort itself to the front. */
static uint32_t track_key(const struct tagcache_search *tcs)
{
    long disc  = tagcache_get_numeric(tcs, tag_discnumber);
    long track = tagcache_get_numeric(tcs, tag_tracknumber);

    if (disc < 0)
        disc = 0;
    if (track < 0)
        track = 0;
    return (uint32_t)(((disc & 0x7fff) << 16) | (track & 0xffff));
}

/* A file's path goes into the arena ahead of the book names. */
static bool collect_resume(uint64_t book, const char *path,
                           const struct book_resume *pos, void *data)
{
    struct shelf_resume *r = &resumes[resume_ct];
    (void)data;

    if (resume_ct >= RESUMES_MAX)
        return false;

    r->book = book;
    r->pos = *pos;
    r->owner = -1;
    r->path = -1;
    if (path != NULL)
    {
        size_t len = strlen(path) + 1;

        if (len > names_sz - names_used)
            return true;
        memcpy(names + names_used, path, len);
        r->path = (int32_t)names_used;
        names_used += len;
    }
    resume_ct++;
    return true;
}

/* Spoken word only. tagcache keeps the pointer rather than a copy, so this
 * lives as long as any search that uses it. */
static struct tagcache_search_clause spoken_clause = {
    .tag = tag_virt_spoken,
    .type = clause_is,
    .numeric = true,
    .source = source_constant,
    .numeric_data = 1,
    .str = NULL,
};

bool book_shelf_is_last_track(const char *book, uint64_t track)
{
    struct tagcache_search tcs;
    static char file[MAX_PATH];
    static char id[BOOK_ID_MAX];
    struct tagcache_album al;
    uint64_t want = book_resume_key(book);
    uint32_t last = 0, mine = 0;
    bool seen = false, found = false;

    if (!tagcache_search(&tcs, tag_filename))
        return false;

    tagcache_search_add_clause(&tcs, &spoken_clause);

    while (tagcache_get_next(&tcs, file, sizeof(file)))
    {
        uint32_t key;

        if (!tagcache_album_get(tagcache_album_of(tcs.idx_id), &al)
            || !book_resume_id_of(al.album_seek, al.artist_seek, id,
                                  sizeof(id))
            || book_resume_key(id) != want)
            continue;

        key = track_key(&tcs);
        if (!seen || key >= last)
            last = key;
        seen = true;

        if (!found && path_key(file) == track)
        {
            mine = key;
            found = true;
        }
    }

    tagcache_search_finish(&tcs);
    return found && mine == last;
}

/* A string into the arena, or false when it is full */
static bool arena_add(const char *s, uint32_t *at)
{
    size_t len = strlen(s) + 1;

    if (len > names_sz - names_used)
        return false;
    memcpy(names + names_used, s, len);
    *at = (uint32_t)names_used;
    names_used += len;
    return true;
}

/* Every album-table row that is a book, in table order for find_book().
 * Needs the database in RAM, where the tables are. */
static bool collect_books(void)
{
    struct tagcache_album al;
    char name[BOOK_KEY_MAX];
    char id[BOOK_ID_MAX];

    if (tagcache_album_count() == 0)
        return false;

    for (int n = 0; book_ct < BOOKS_MAX && tagcache_album_get(n, &al); n++)
    {
        struct shelf_book *b = &books[book_ct];

        if (al.spoken != al.tracks
            || !tagcache_seek_string(tag_album, al.album_seek, name,
                                     sizeof(name))
            || !book_resume_id_of(al.album_seek, al.artist_seek, id,
                                  sizeof(id)))
            continue;

        memset(b, 0, sizeof(*b));
        if (!arena_add(name, &b->name) || !arena_add(id, &b->id))
            break;
        b->key = book_resume_key(id);
        b->seek = al.album_seek;
        b->artist_seek = al.artist_seek;
        b->row = n;
        b->resume = -1;
        book_ct++;
    }
    return true;
}

/* The book on album-table row 'row', or -1 */
static int find_book(int row)
{
    int lo = 0, hi = book_ct - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;

        if (books[mid].row == row)
            return mid;
        if (books[mid].row < row)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static void match_resumes(void)
{
    for (int r = 0; r < resume_ct; r++)
    {
        for (int b = 0; b < book_ct; b++)
        {
            if (books[b].resume < 0 && books[b].key == resumes[r].book)
            {
                books[b].resume = r;
                resumes[r].owner = b;
                break;
            }
        }
    }
}

/* One walk over every spoken-word track. 'before' says which pass: the first
 * gathers each book's figures and finds its resume track, the second adds up
 * what comes ahead of that track -- which cannot be done in the first, since
 * the resume track may be met last. */
static bool walk_tracks(bool before)
{
    struct tagcache_search tcs;
    char path[MAX_PATH];
    char text[TAGCACHE_BUFSZ];

    if (!tagcache_search(&tcs, tag_filename))
        return false;

    tagcache_search_add_clause(&tcs, &spoken_clause);

    while (tagcache_get_next(&tcs, path, sizeof(path)))
    {
        struct shelf_book *b;
        uint32_t key;
        long length;
        int i;

        i = find_book(tagcache_album_of(tcs.idx_id));
        if (i < 0)
            continue;
        b = &books[i];

        key = track_key(&tcs);
        length = tagcache_get_numeric(&tcs, tag_length);
        if (length < 0)
            length = 0;

        if (before)
        {
            if (b->resume_found && key < b->resume_key)
                b->before += (unsigned long)length;
            continue;
        }

        if (tagcache_retrieve(&tcs, tcs.idx_id, tag_genre, text, sizeof(text))
            && db_spoken_is_podcast_genre(text))
            continue;

        b->counted = true;
        b->length += (unsigned long)length;

        long playcount = tagcache_get_numeric(&tcs, tag_playcount);
        long lastplayed = tagcache_get_numeric(&tcs, tag_lastplayed);

        if (playcount > 0)
            b->played++;
        if (lastplayed > b->lastplayed)
            b->lastplayed = lastplayed;
        if (key >= b->last_key)
        {
            b->last_key = key;
            b->last_heard = playcount > 0;
        }

        if (b->resume >= 0 && !b->resume_found
            && resumes[b->resume].pos.track != 0
            && path_key(path) == resumes[b->resume].pos.track)
        {
            b->resume_key = key;
            b->resume_found = true;
        }
    }

    tagcache_search_finish(&tcs);
    return true;
}

/* ------------------------------------------------------------------ *
 * deciding each book's state                                         *
 * ------------------------------------------------------------------ */

static bool book_finished(const struct shelf_book *b)
{
    if (b->resume < 0)
        return b->last_heard;

    return resumes[b->resume].pos.left == BOOK_LEFT_ENDED && b->resume_found
        && b->resume_key == b->last_key;
}

static enum book_shelf book_state(const struct shelf_book *b)
{
    if (b->resume >= 0)
    {
        switch (resumes[b->resume].pos.left)
        {
            case BOOK_LEFT_FINISHED:  return BOOK_SHELF_FINISHED;
            case BOOK_LEFT_UNSTARTED: return BOOK_SHELF_NOT_STARTED;
            default:                  break;
        }
    }
    if (book_finished(b))
        return BOOK_SHELF_FINISHED;
    if (b->resume < 0 && b->played == 0)
        return BOOK_SHELF_NOT_STARTED;
    return BOOK_SHELF_IN_PROGRESS;
}

/* How far through, 0-99, or -1 where there is nothing to say it from. */
static int book_percent(const struct shelf_book *b)
{
    const struct book_resume *pos;
    unsigned long long heard;
    int pct;

    if (b->resume < 0 || !b->resume_found || b->length == 0)
        return -1;

    pos = &resumes[b->resume].pos;
    heard = (unsigned long long)b->before + pos->elapsed;
    pct = (int)(heard * 100 / b->length);

    /* 100 is for Finished to say. */
    return pct > 99 ? 99 : pct;
}

/* A resume line with no book behind it, keyed by its file: one file with no
 * album tag, which is a book of one track. */
static bool resume_is_file(int r)
{
    return resumes[r].owner < 0 && resumes[r].path >= 0;
}

static const char *resume_path(int r)
{
    return names + resumes[r].path;
}

/* What a row is called: a book's name, or a file's own name. */
static const char *row_name(int v)
{
    const char *path, *base;

    if (!ROW_IS_RESUME(v))
        return book_name(v);

    path = resume_path(ROW_RESUME_OF(v));
    base = strrchr(path, '/');
    return base ? base + 1 : path;
}

/* In progress: the resume file's order first, which is most recent first, and
 * then the books played without a line, most recent first. Finished: most
 * recent first. Not started: by name. */
static int compare_rows(const void *a_v, const void *b_v)
{
    int a = *(const int *)a_v;
    int b = *(const int *)b_v;
    int ra = ROW_IS_RESUME(a) ? ROW_RESUME_OF(a) : books[a].resume;
    int rb = ROW_IS_RESUME(b) ? ROW_RESUME_OF(b) : books[b].resume;
    long la, lb;

    if (shelf_kind == BOOK_SHELF_NOT_STARTED)
        return strnatcasecmp(row_name(a), row_name(b));

    if (shelf_kind == BOOK_SHELF_IN_PROGRESS && (ra >= 0 || rb >= 0))
    {
        if (ra < 0)
            return 1;
        if (rb < 0)
            return -1;
        return ra - rb;
    }

    /* A file with no book row has no lastplayed; it follows the books. */
    la = ROW_IS_RESUME(a) ? -1 : books[a].lastplayed;
    lb = ROW_IS_RESUME(b) ? -1 : books[b].lastplayed;
    if (la != lb)
        return la > lb ? -1 : 1;
    return ra - rb;
}

static void build_rows(void)
{
    row_ct = 0;

    for (int b = 0; b < book_ct; b++)
    {
        if (books[b].counted && book_state(&books[b]) == shelf_kind)
            rows[row_ct++] = b;
    }

    for (int r = 0; r < resume_ct; r++)
    {
        enum book_shelf state;

        if (!resume_is_file(r))
            continue;
        switch (resumes[r].pos.left)
        {
            case BOOK_LEFT_ENDED:
            case BOOK_LEFT_FINISHED:  state = BOOK_SHELF_FINISHED;    break;
            case BOOK_LEFT_UNSTARTED: state = BOOK_SHELF_NOT_STARTED; break;
            default:                  state = BOOK_SHELF_IN_PROGRESS; break;
        }
        if (state == shelf_kind)
            rows[row_ct++] = ROW_RESUME(r);
    }

    qsort(rows, row_ct, sizeof(*rows), compare_rows);
}

/* ------------------------------------------------------------------ *
 * the list                                                           *
 * ------------------------------------------------------------------ */

static const char *shelf_get_name(int n, void *data, char *buffer,
                                  size_t buffer_len)
{
    int v = rows[n];
    int pct;
    (void)data;

    if (ROW_IS_RESUME(v))
        return row_name(v);

    pct = shelf_kind == BOOK_SHELF_IN_PROGRESS ? book_percent(&books[v]) : -1;
    if (pct < 0)
        return book_name(v);

    snprintf(buffer, buffer_len, str(LANG_BOOK_SHELF_ROW), book_name(v), pct);
    return buffer;
}

/* A book, as it is in the database browser's list of books. */
static enum list_row_kind shelf_get_kind(int n, void *data)
{
    (void)n;
    (void)data;
    return LIST_ROW_CONTAINER;
}

static int shelf_title(enum book_shelf which)
{
    switch (which)
    {
        case BOOK_SHELF_IN_PROGRESS: return LANG_BOOKS_IN_PROGRESS;
        case BOOK_SHELF_NOT_STARTED: return LANG_BOOKS_NOT_STARTED;
        case BOOK_SHELF_FINISHED:    return LANG_BOOKS_FINISHED;
    }
    return LANG_BOOKS_IN_PROGRESS;
}

/* ------------------------------------------------------------------ *
 * playing a book                                                     *
 * ------------------------------------------------------------------ */

/* What playing needs, copied out of the arena before it is released: the
 * playlist is built in the same buffer. */
static struct
{
    long seek;                  /* the album, or -1 for a file */
    long artist_seek;           /* and its album artist */
    char path[MAX_PATH];        /* the file, when 'seek' is -1 */
    uint64_t track;             /* where to start, or 0 for the beginning */
    bool after;                 /* start on the track after 'track' */
    unsigned long elapsed;
    unsigned long offset;
} chosen;

static void choose(int v)
{
    const struct book_resume *pos = NULL;

    memset(&chosen, 0, sizeof(chosen));
    chosen.seek = -1;

    if (ROW_IS_RESUME(v))
    {
        pos = &resumes[ROW_RESUME_OF(v)].pos;
        strmemccpy(chosen.path, resume_path(ROW_RESUME_OF(v)),
                   sizeof(chosen.path));
        if (pos->left == BOOK_LEFT_PARTWAY)
        {
            chosen.elapsed = pos->elapsed;
            chosen.offset = pos->offset;
        }
        return;
    }

    chosen.seek = books[v].seek;
    chosen.artist_seek = books[v].artist_seek;
    if (shelf_kind != BOOK_SHELF_IN_PROGRESS || books[v].resume < 0)
        return;

    pos = &resumes[books[v].resume].pos;
    chosen.track = pos->track;
    /* A track that played to its end, in a book that did not: the listener
     * stopped between chapters, so go on from the next one. */
    chosen.after = pos->left == BOOK_LEFT_ENDED;
    if (pos->left == BOOK_LEFT_PARTWAY)
    {
        chosen.elapsed = pos->elapsed;
        chosen.offset = pos->offset;
    }
}

struct book_track
{
    int32_t  idx_id;
    uint32_t key;
};

static int compare_book_tracks(const void *a_v, const void *b_v)
{
    const struct book_track *a = a_v;
    const struct book_track *b = b_v;

    return a->key < b->key ? -1 : a->key > b->key;
}

/* The tracks of album 'seek' by 'artist_seek' to 'fn', in the order its
 * track list shows them, sorted in 'list'. How many were found, or -1 if the
 * database could not be read. 'fn' returns false to stop. */
static int each_track(long seek, long artist_seek, struct book_track *list,
                      int cap, bool (*fn)(const char *path, void *data),
                      void *data)
{
    struct tagcache_search tcs;
    char path[MAX_PATH];
    int found = 0;

    if (!tagcache_search(&tcs, tag_filename))
        return -1;
    tagcache_search_add_filter(&tcs, tag_album, seek);
    tagcache_search_add_filter(&tcs, tag_albumartist, artist_seek);
    tagcache_search_add_clause(&tcs, &spoken_clause);

    while (found < cap && tagcache_get_next(&tcs, path, sizeof(path)))
    {
        list[found].idx_id = tcs.idx_id;
        list[found].key = track_key(&tcs);
        found++;
    }

    qsort(list, found, sizeof(*list), compare_book_tracks);

    for (int i = 0; i < found; i++)
    {
        if (tagcache_retrieve(&tcs, list[i].idx_id, tag_filename,
                              path, sizeof(path))
            && !fn(path, data))
            break;
    }

    tagcache_search_finish(&tcs);
    return found;
}

struct queued
{
    struct playlist_insert_context *ctx;
    int added;
    int start;
    bool matched;
};

static bool queue_track(const char *path, void *data)
{
    struct queued *q = data;

    if (playlist_insert_context_add(q->ctx, path) < 0)
        return false;
    if (chosen.track != 0 && path_key(path) == chosen.track)
    {
        q->start = chosen.after ? q->added + 1 : q->added;
        q->matched = true;
    }
    q->added++;
    return true;
}

/* The chosen book's tracks into the playlist just created. The index to
 * start at, or -1. */
static int queue_book(struct playlist_insert_context *ctx)
{
    struct queued q = { ctx, 0, 0, false };
    size_t list_sz;
    struct book_track *list = app_get_buffer(&list_sz, "book play");

    if (each_track(chosen.seek, chosen.artist_seek, list,
                   (int)(list_sz / sizeof(*list)), queue_track, &q) < 0
        || q.added == 0)
        return -1;
    /* The saved track is gone -- renamed or moved -- so its position belongs
     * to nothing here. The book starts at its first chapter, from the top. */
    if (!q.matched)
    {
        chosen.elapsed = 0;
        chosen.offset = 0;
    }
    /* The last chapter ended and the book is not finished -- it can only be
     * a book whose chapters were not all played. Start it again. */
    return q.start < q.added ? q.start : 0;
}

static int play_chosen(void)
{
    struct playlist_insert_context ctx;
    int start;

    if (global_settings.party_mode && audio_status())
    {
        splash(HZ, ID2P(LANG_PARTY_MODE));
        return GO_TO_PREVIOUS;
    }
    if (!warn_on_pl_erase())
        return GO_TO_PREVIOUS;
    if (playlist_create(NULL, NULL) < 0)
        return GO_TO_PREVIOUS;

    if (playlist_insert_context_create(NULL, &ctx, PLAYLIST_INSERT_LAST,
                                       false, false) < 0)
    {
        /* create() keeps the playlist lock even when it fails; release() is
         * the only thing that gives it back. */
        playlist_insert_context_release(&ctx);
        return GO_TO_PREVIOUS;
    }

    cpu_boost(true);
    if (chosen.seek < 0)
        start = playlist_insert_context_add(&ctx, chosen.path) < 0 ? -1 : 0;
    else
        start = queue_book(&ctx);
    cpu_boost(false);

    playlist_insert_context_release(&ctx);

    if (start < 0)
    {
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        return GO_TO_PREVIOUS;
    }

    playlist_start(start, chosen.elapsed, chosen.offset);
    return GO_TO_WPS;
}

/* ------------------------------------------------------------------ *
 * a book's context menu                                              *
 * ------------------------------------------------------------------ */

/* The menu's order is enum book_shelf's. */
bool book_shelf_mark_menu(const char *book, int current)
{
    static const enum book_left marks[] = {
        [BOOK_SHELF_IN_PROGRESS] = BOOK_LEFT_PARTWAY,
        [BOOK_SHELF_NOT_STARTED] = BOOK_LEFT_UNSTARTED,
        [BOOK_SHELF_FINISHED]    = BOOK_LEFT_FINISHED,
    };
    struct book_resume pos;
    int choice;
    bool guessed = false;

    /* Marking in progress starts the book again, so a book that has a place
     * to resume from is in progress already. A book with nothing saved has
     * almost always not been started; that is only a guess, so choosing it
     * still marks the book. */
    if (current < 0)
    {
        if (!book_resume_find(book, &pos))
        {
            current = BOOK_SHELF_NOT_STARTED;
            guessed = true;
        }
        else if (pos.left == BOOK_LEFT_FINISHED
                 || (pos.left == BOOK_LEFT_ENDED
                     && book_shelf_is_last_track(book, pos.track)))
            current = BOOK_SHELF_FINISHED;
        else if (pos.left == BOOK_LEFT_UNSTARTED)
            current = BOOK_SHELF_NOT_STARTED;
        else
            current = BOOK_SHELF_IN_PROGRESS;
    }
    choice = current;

    MENUITEM_STRINGLIST(menu, ID2P(LANG_BOOK_MARK_AS), NULL,
                        ID2P(LANG_BOOKS_IN_PROGRESS),
                        ID2P(LANG_BOOKS_NOT_STARTED),
                        ID2P(LANG_BOOKS_FINISHED));
    push_current_activity(ACTIVITY_CONTEXTMENU);
    choice = do_menu(&menu, &choice, NULL, false);
    if (get_current_activity() == ACTIVITY_CONTEXTMENU)
        pop_current_activity();

    if (choice < 0 || choice >= (int)ARRAYLEN(marks)
        || (choice == current && !guessed))
        return false;
    return book_resume_mark(book, marks[choice]);
}

/* The book the menu is about: its album and album artist, or -1 for one held
 * in a single file known only by its path. */
static long menu_seek;
static long menu_artist_seek;
static char menu_path[MAX_PATH];

/* Where the shelf goes once the menu has closed, or GO_TO_PREVIOUS to stay */
static int menu_exit;

/* The menu book's tracks to 'fn'. Sorted in the arena's free tail, since the
 * list is up and holds the rest of it. */
static int menu_tracks(bool (*fn)(const char *path, void *data), void *data)
{
    uintptr_t p = ALIGN_UP((uintptr_t)(names + names_used), sizeof(int32_t));
    uintptr_t end = (uintptr_t)(names + names_sz);

    if (menu_seek < 0)
    {
        fn(menu_path, data);
        return 1;
    }
    return each_track(menu_seek, menu_artist_seek, (struct book_track *)p,
                      p < end ? (int)((end - p) / sizeof(struct book_track)) : 0,
                      fn, data);
}

static bool insert_track(const char *path, void *data)
{
    return playlist_insert_context_add(data, path) >= 0;
}

/* Add to Queue's rows */
static bool menu_insert(int position, bool queue, bool create_new)
{
    struct playlist_insert_context ctx;
    bool ok;

    (void)create_new;
    if (playlist_insert_context_create(NULL, &ctx, position, queue, false) < 0)
    {
        playlist_insert_context_release(&ctx);
        return false;
    }
    ok = menu_tracks(insert_track, &ctx) > 0;
    playlist_insert_context_release(&ctx);
    return ok;
}

static bool write_track(const char *path, void *data)
{
    return fdprintf(*(int *)data, "%s\n", path) > 0;
}

/* Add to Playlist's rows */
static int menu_add_to_playlist(const char *playlist, bool new_playlist)
{
    int fd = new_playlist ? open_utf8(playlist, O_CREAT | O_WRONLY | O_TRUNC)
                          : open(playlist, O_CREAT | O_WRONLY | O_APPEND, 0666);
    int n;

    if (fd < 0)
        return -1;
    n = menu_tracks(write_track, &fd);
    close(fd);
    return n > 0 ? 0 : -1;
}

static bool first_track(const char *path, void *data)
{
    strmemccpy(data, path, MAX_PATH);
    return false;
}

/* The menu a book in the browser has, its rows run for the book's tracks.
 * 'book' is its id, 'name' what it is called. True if the book was marked. */
static bool book_menu(const char *book, const char *name)
{
    char first[MAX_PATH];
    char sel[MAX_PATH];
    int choice;

    MENUITEM_STRINGLIST(menu, ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                        ID2P(LANG_PLAYING_NEXT), ID2P(LANG_ADD_TO_PL),
                        ID2P(LANG_MENU_SHOW_ID3_INFO), ID2P(LANG_SHOW_IN_FILES),
                        ID2P(LANG_BOOK_MARK_AS));

    push_current_activity(ACTIVITY_CONTEXTMENU);
    choice = do_menu(&menu, NULL, NULL, false);
    if (get_current_activity() == ACTIVITY_CONTEXTMENU)
        pop_current_activity();

    /* Led by a slash, as the browser does, so a new playlist is offered the
     * book's name */
    snprintf(sel, sizeof(sel), "%s%s", name[0] == '/' ? "" : "/", name);
    first[0] = '\0';

    switch (choice)
    {
        case 0:
            if (context_menu_show_playlist(sel, ATTR_DIRECTORY, menu_insert)
                == ONPLAY_START_PLAY)
                menu_exit = GO_TO_WPS;
            break;
        case 1:
            context_menu_show_playlist_cat(sel, ATTR_DIRECTORY,
                                           menu_add_to_playlist);
            break;
        case 2:
            menu_tracks(first_track, first);
            if (first[0] && properties(first) == GO_TO_ROOT)
                menu_exit = GO_TO_ROOT;
            break;
        case 3:
            menu_tracks(first_track, first);
            if (first[0])
            {
                browser_reveal_on_next_load(first);
                menu_exit = GO_TO_FILEBROWSER;
            }
            break;
        case 4:
            return book_shelf_mark_menu(book, shelf_kind);
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * the way in                                                         *
 * ------------------------------------------------------------------ */

static void report_empty(void)
{
    splash(HZ * 2, ID2P(LANG_BOOK_SHELF_EMPTY));
}

/* The row a mark was made on, or -1. A list callback can only end the list,
 * not say why, so this is how the list learns to rebuild. */
static int marked_row;

/* Context: the book's menu. A mark rebuilds the list, which has moved the
 * book to another one; a row that leaves the shelf ends it. */
static int shelf_action_cb(int action, struct gui_synclist *lists)
{
    int n, v;
    bool marked;

    if (action != ACTION_STD_CONTEXT)
        return action;

    n = gui_synclist_get_sel_pos(lists);
    if (n < 0 || n >= row_ct)
        return action;
    v = rows[n];

    menu_seek = ROW_IS_RESUME(v) ? -1 : books[v].seek;
    menu_artist_seek = ROW_IS_RESUME(v) ? -1 : books[v].artist_seek;
    if (ROW_IS_RESUME(v))
        strmemccpy(menu_path, resume_path(ROW_RESUME_OF(v)), sizeof(menu_path));
    marked = ROW_IS_RESUME(v) ? book_menu(menu_path, menu_path)
                              : book_menu(book_id(v), book_name(v));
    if (marked)
        marked_row = n;
    return marked || menu_exit != GO_TO_PREVIOUS ? ACTION_STD_CANCEL
                                                 : ACTION_REDRAW;
}

/* Everything the list is drawn from, read afresh. */
static bool load(void)
{
    bool ok;

    book_ct = resume_ct = row_ct = 0;
    names_used = 0;

    cpu_boost(true);
    book_resume_each(collect_resume, NULL);
    ok = collect_books();
    if (ok)
    {
        match_resumes();
        ok = walk_tracks(false);
    }
    if (ok && shelf_kind == BOOK_SHELF_IN_PROGRESS)
        ok = walk_tracks(true);
    cpu_boost(false);

    if (ok)
        build_rows();
    return ok;
}

void book_shelf_arm(enum book_shelf which)
{
    shelf_kind = which;
}

int book_shelf_run(void)
{
    struct simplelist_info info;
    int ret = GO_TO_PREVIOUS;
    int selection = 0;
    bool picked = false;

    /* Writes down a book that has just ended, and where the playing one is,
     * so both are on the shelf they belong on. */
    book_resume_save();

    if (!claim())
    {
        splash(HZ * 2, ID2P(LANG_OUT_OF_MEMORY));
        return GO_TO_PREVIOUS;
    }

    push_current_activity(ACTIVITY_BOOKSHELF);

    if (!tagcache_is_in_ram())
        splash(0, ID2P(LANG_WAIT));

    /* Round again after a mark, which has moved the book to another list. */
    menu_exit = GO_TO_PREVIOUS;
    do
    {
        marked_row = -1;

        if (!load())
        {
            splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
            break;
        }
        if (row_ct == 0)
        {
            report_empty();
            break;
        }

        simplelist_info_init(&info, str(shelf_title(shelf_kind)), row_ct,
                             NULL);
        info.get_name = shelf_get_name;
        info.get_kind = shelf_get_kind;
        info.action_callback = shelf_action_cb;
        info.selection = MIN(selection, row_ct - 1);

        if (simplelist_show_list(&info))
            ret = GO_TO_ROOT;
        else if (menu_exit != GO_TO_PREVIOUS)
            ret = menu_exit;
        else if (info.selection >= 0 && info.selection < row_ct)
        {
            choose(rows[info.selection]);
            picked = true;
        }
        selection = marked_row;
    } while (marked_row >= 0 && menu_exit == GO_TO_PREVIOUS);

    pop_current_activity();
    release();

    if (picked)
        ret = play_chosen();
    return ret;
}
