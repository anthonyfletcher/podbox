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
 * A book is an album the database calls one -- spoken word and nothing else
 * (database/db_spoken.c) -- keyed by name, as the resume file keys it.
 * Podcasts are left out: a show has no last episode to have finished.
 *
 * Choosing a book plays it. An In progress one resumes; the others start at
 * the beginning.
 *
 * Parts, in order:
 *   - the arena and what it holds
 *   - collecting: the resume file, the books, their tracks
 *   - deciding each book's state
 *   - the list
 *   - playing a book
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
#include "audio.h"
#include "lang.h"
#include "strnatcmp.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"
#include "settings/settings.h"
#include "system/activity.h"
#include "system/app_buffer.h"
#include "system/app_util.h"          /* warn_on_pl_erase */
#include "database/tagcache.h"
#include "database/db_spoken.h"
#include "metadata/book_resume.h"
#include "playlist/playlist.h"
#include "root_menu.h"
#include "book_shelf.h"

/* Books past this are not collected. A shelf that long is a library this
 * screen was not written for, and the list says nothing about the rest. */
#define BOOKS_MAX 1024

/* As many as the resume file keeps. */
#define RESUMES_MAX 64

/* ------------------------------------------------------------------ *
 * the arena                                                          *
 * ------------------------------------------------------------------ */

struct shelf_resume
{
    char book[BOOK_KEY_MAX];
    struct book_resume pos;
    int owner;                  /* its books[] entry, or -1 */
};

struct shelf_book
{
    long     seek;              /* the album tag's seek */
    uint32_t name;              /* into the arena */
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
static uint32_t            *uniq;
static char  *names;
static size_t names_sz;
static int book_ct, resume_ct, row_ct;

static enum book_shelf shelf_kind;

static bool claim(void)
{
    size_t sz, fixed;
    char *p = app_claim_buffer(&sz, "book shelf");

    fixed = BOOKS_MAX * sizeof(*books) + RESUMES_MAX * sizeof(*resumes)
          + (BOOKS_MAX + RESUMES_MAX) * sizeof(*rows)
          + BOOKS_MAX * 2 * sizeof(*uniq);
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
    uniq    = (uint32_t *)p;            p += BOOKS_MAX * 2 * sizeof(*uniq);
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

static bool collect_resume(const char *book, const struct book_resume *pos,
                           void *data)
{
    (void)data;

    if (resume_ct >= RESUMES_MAX)
        return false;

    strmemccpy(resumes[resume_ct].book, book, sizeof(resumes[0].book));
    resumes[resume_ct].pos = *pos;
    resumes[resume_ct].owner = -1;
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

static int compare_names(const void *a_v, const void *b_v)
{
    const struct shelf_book *a = a_v;
    const struct shelf_book *b = b_v;

    return strcmp(names + a->name, names + b->name);
}

/* Every album that is a book, sorted by name for find_book(). */
static bool collect_books(void)
{
    struct tagcache_search tcs;
    char name[TAGCACHE_BUFSZ];
    size_t used = 0;

    /* False is a normal answer -- another thread is building it, or a commit
     * landed -- and then every album reads as music this once. */
    db_spoken_group_ensure(tag_album);

    if (!tagcache_search(&tcs, tag_album))
        return false;

    tagcache_search_set_uniqbuf(&tcs, uniq, BOOKS_MAX * 2 * sizeof(*uniq));
    tagcache_search_add_clause(&tcs, &spoken_clause);

    while (book_ct < BOOKS_MAX && tagcache_get_next(&tcs, name, sizeof(name)))
    {
        struct shelf_book *b;
        size_t avail = names_sz - used;
        int len;

        if (!strcmp(name, UNTAGGED)
            || !db_spoken_group_is_book(tag_album, tcs.result_seek))
            continue;

        len = snprintf(names + used, avail, "%s", name);
        if (len < 0 || (size_t)len >= avail)
            break;

        b = &books[book_ct++];
        memset(b, 0, sizeof(*b));
        b->seek = tcs.result_seek;
        b->name = (uint32_t)used;
        b->resume = -1;
        used += (size_t)len + 1;
    }

    tagcache_search_finish(&tcs);

    qsort(books, book_ct, sizeof(*books), compare_names);
    return true;
}

static int find_book(const char *name)
{
    int lo = 0, hi = book_ct - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        int c = strcmp(name, book_name(mid));

        if (c == 0)
            return mid;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return -1;
}

/* Whether a resume line's key names 'name'. The key was cut to BOOK_KEY_MAX
 * on the way into the file, so a key that long matches as a prefix. */
static bool key_names(const char *key, const char *name)
{
    size_t n = strlen(key);

    if (n < BOOK_KEY_MAX - 1)
        return !strcmp(key, name);
    return !strncmp(key, name, n);
}

static void match_resumes(void)
{
    for (int r = 0; r < resume_ct; r++)
    {
        for (int b = 0; b < book_ct; b++)
        {
            if (books[b].resume < 0 && key_names(resumes[r].book, book_name(b)))
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

        if (!tagcache_retrieve(&tcs, tcs.idx_id, tag_album, text, sizeof(text)))
            continue;
        i = find_book(text);
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
            && !strcmp(path, resumes[b->resume].pos.track))
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

    return resumes[b->resume].pos.ended && b->resume_found
        && b->resume_key == b->last_key;
}

static enum book_shelf book_state(const struct shelf_book *b)
{
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
    return resumes[r].owner < 0 && resumes[r].book[0] == '/';
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
        return strnatcasecmp(book_name(a), book_name(b));

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
        state = resumes[r].pos.ended ? BOOK_SHELF_FINISHED
                                     : BOOK_SHELF_IN_PROGRESS;
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
    {
        const char *path = resumes[ROW_RESUME_OF(v)].book;
        const char *base = strrchr(path, '/');

        return base ? base + 1 : path;
    }

    pct = shelf_kind == BOOK_SHELF_IN_PROGRESS ? book_percent(&books[v]) : -1;
    if (pct < 0)
        return book_name(v);

    snprintf(buffer, buffer_len, str(LANG_BOOK_SHELF_ROW), book_name(v), pct);
    return buffer;
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
    char track[MAX_PATH];       /* where to start, or "" for the beginning */
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
        strmemccpy(chosen.track, pos->track, sizeof(chosen.track));
        if (!pos->ended)
        {
            chosen.elapsed = pos->elapsed;
            chosen.offset = pos->offset;
        }
        return;
    }

    chosen.seek = books[v].seek;
    if (shelf_kind != BOOK_SHELF_IN_PROGRESS || books[v].resume < 0)
        return;

    pos = &resumes[books[v].resume].pos;
    strmemccpy(chosen.track, pos->track, sizeof(chosen.track));
    /* A track that played to its end, in a book that did not: the listener
     * stopped between chapters, so go on from the next one. */
    chosen.after = pos->ended;
    if (!pos->ended)
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

/* The chosen book's tracks, in the order its track list shows them, into the
 * playlist just created. The index to start at, or -1. */
static int queue_book(struct playlist_insert_context *ctx)
{
    struct tagcache_search tcs;
    char path[MAX_PATH];
    struct book_track *list;
    size_t list_sz;
    int cap, found = 0, added = 0, start = 0;
    bool matched = false;

    if (!tagcache_search(&tcs, tag_filename))
        return -1;
    tagcache_search_add_filter(&tcs, tag_album, chosen.seek);

    list = app_get_buffer(&list_sz, "book play");
    cap = (int)(list_sz / sizeof(*list));

    while (found < cap && tagcache_get_next(&tcs, path, sizeof(path)))
    {
        list[found].idx_id = tcs.idx_id;
        list[found].key = track_key(&tcs);
        found++;
    }

    qsort(list, found, sizeof(*list), compare_book_tracks);

    for (int i = 0; i < found; i++)
    {
        if (!tagcache_retrieve(&tcs, list[i].idx_id, tag_filename,
                               path, sizeof(path)))
            continue;
        if (playlist_insert_context_add(ctx, path) < 0)
            break;
        if (chosen.track[0] && !strcmp(path, chosen.track))
        {
            start = chosen.after ? added + 1 : added;
            matched = true;
        }
        added++;
    }

    tagcache_search_finish(&tcs);

    if (added == 0)
        return -1;
    /* The saved track is gone -- renamed or moved -- so its position belongs
     * to nothing here. The book starts at its first chapter, from the top. */
    if (!matched)
    {
        chosen.elapsed = 0;
        chosen.offset = 0;
    }
    /* The last chapter ended and the book is not finished -- it can only be
     * a book whose chapters were not all played. Start it again. */
    return start < added ? start : 0;
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
        start = playlist_insert_context_add(&ctx, chosen.track) < 0 ? -1 : 0;
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
 * the way in                                                         *
 * ------------------------------------------------------------------ */

/* Not started and Finished read the database's playcounts, which nothing
 * writes with runtime data gathering off. Say so and offer it, once a boot. */
static bool runtimedb_asked;

static void report_empty(void)
{
    if (!global_settings.runtimedb && !runtimedb_asked)
    {
        runtimedb_asked = true;
        if (yesno_pop(str(LANG_RUNTIMEDB_OFF_PROMPT)))
        {
            global_settings.runtimedb = true;
            settings_save();
        }
        return;
    }

    splash(HZ * 2, ID2P(LANG_BOOK_SHELF_EMPTY));
}

void book_shelf_arm(enum book_shelf which)
{
    shelf_kind = which;
}

int book_shelf_run(void)
{
    struct simplelist_info info;
    int ret = GO_TO_PREVIOUS;
    bool picked = false;
    bool ok;

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

    if (!ok)
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
    else
    {
        build_rows();

        if (row_ct == 0)
            report_empty();
        else
        {
            simplelist_info_init(&info, str(shelf_title(shelf_kind)), row_ct,
                                 NULL);
            info.get_name = shelf_get_name;

            if (simplelist_show_list(&info))
                ret = GO_TO_ROOT;
            else if (info.selection >= 0 && info.selection < row_ct)
            {
                choose(rows[info.selection]);
                picked = true;
            }
        }
    }

    pop_current_activity();
    release();

    if (picked)
        ret = play_chosen();
    return ret;
}
