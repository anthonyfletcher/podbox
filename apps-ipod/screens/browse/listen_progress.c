/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Listening progress: how much of an album or an artist has been heard, and
 * which of its tracks have not been.
 *
 * The figures are the database's own per-track playcount, the one the runtime
 * data gathering setting maintains -- not the playback log. Only the database
 * can say what a whole album is, and once it is being asked that it can answer
 * the rest in the same walk. The log knows a great deal more about a play and
 * nothing whatever about a track that was never played.
 *
 * The scope is the browse row the context menu was opened on: browser_db.c
 * scopes the search to that row, ancestor levels and all. A book is the one
 * exception, named by its album and album artist, since a shelf has no browse
 * row behind it.
 *
 * Two levels, and the artist one lists albums rather than tracks: an artist's
 * unheard tracks run to hundreds and say nothing about where the gaps are,
 * where "Pablo Honey, 0%" does.
 *
 * A book or a podcast show opens as its Listening Stats: the figures
 * metadata/listen_stats.c keeps -- time listened, the days, the longest
 * sitting -- and for a book the time left from where it was left, at the
 * Spoken Word Speed, above the same summary and unheard tracks. Every book
 * together has the screen too, from the Audiobooks menu.
 *
 * Parts, in order:
 *   - the arena the row text is written into
 *   - walking one album's tracks
 *   - collecting an artist's albums
 *   - Listening Stats' lines
 *   - the two lists
 *   - the way in
 ****************************************************************************/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "cpu.h"                     /* cpu_boost */
#include "file.h"                    /* MAX_PATH */
#include "lang.h"
#include "strnatcmp.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "settings/settings.h"
#include "input/action.h"
#include "system/activity.h"
#include "system/app_buffer.h"       /* where the row text is written */
#include "database/tagcache.h"
#include "database/db_spoken.h"
#include "database/path_key.h"
#include "metadata/book_resume.h"
#include "metadata/listen_stats.h"
#include "speech/talk.h"             /* UNIT_SEC */
#include "system/format_time.h"
#include "viewers/playback_viewer/pv_stats.h"   /* the calendar */
#include "screens/browse/browser_db.h"
#include "root_menu.h"
#include "listen_progress.h"

/* Ceilings on what one visit collects. An artist past the first is not a
 * library this screen was written for; an album past the second still counts
 * whole, because the cap is on the rows listed and never on the figures. */
#define ALBUMS_MAX 128
#define TRACKS_MAX 256

/* Where the row text lives while a list is up: the app buffer, claimed for
 * the whole visit. The album names sit at the bottom of it and stay for as
 * long as the artist list does, so an album opened from that list writes its
 * track titles above them rather than over them. */
static char  *arena;
static size_t arena_sz;
static size_t arena_albums;    /* what the album names took */

/* What scopes every search here to the browse row the screen was opened on.
 * Captured before the arena is claimed, because reading that row can want the
 * same memory -- see browser_db.h. */
static struct browser_db_filters scope;

struct lp_album
{
    long     seek;             /* the album tag's seek, to filter tracks by */
    uint32_t off;              /* its name, into the arena */
    int      total;
    int      played;
};

static struct lp_album albums[ALBUMS_MAX];
static int album_ct;
static int artist_total, artist_played;

struct lp_track
{
    uint32_t off;              /* the title, into the arena */
    uint32_t key;              /* disc and track number, for album order */
};

static struct lp_track tracks[TRACKS_MAX];
static int track_ct;
static int track_total, track_played;

/* Listening Stats' lines, above the summary row; none for an album or an
 * artist */
#define LS_LINES 8
static char ls_line[LS_LINES][80];
static int ls_n;

/* While a book's tracks are walked: where it was left, and each track's
 * place and length, from which what is left to hear is summed */
#define LS_TRACKS 512
static bool ls_book;
static struct book_resume ls_pos;
static struct
{
    uint32_t key;
    uint32_t ms;
} ls_tracks[LS_TRACKS];
static int ls_track_ct;
static uint32_t ls_at_key;      /* the place's track, once found */
static bool ls_at;

/* Whole percent, rounded down, so 100% means every track rather than nearly
 * every track. */
static int percent(int played, int total)
{
    return total > 0 ? (played * 100) / total : 0;
}

/* ------------------------------------------------------------------ *
 * one album's tracks                                                 *
 * ------------------------------------------------------------------ */

static int compare_tracks(const void *a_v, const void *b_v)
{
    const struct lp_track *a = a_v;
    const struct lp_track *b = b_v;

    return a->key < b->key ? -1 : a->key > b->key;
}

/* Walk the tracks the browse row stands for, narrowed to one album when
 * 'seek' is not -1. Counts them into *total and *played, and when 'list' says
 * to, collects the unheard ones into tracks[] in album order.
 *
 * False only when the database would not answer. A row whose tracks have all
 * been heard is a complete answer and an empty list. */
static bool walk_album(long seek, bool list, int *total, int *played)
{
    struct tagcache_search tcs;
    char title[TAGCACHE_BUFSZ];
    size_t used = arena_albums;
    bool full = false;

    *total = *played = 0;
    if (list)
        track_ct = 0;
    ls_track_ct = 0;
    ls_at = false;

    if (!tagcache_search(&tcs, tag_title))
        return false;

    if (!browser_db_add_filters(&tcs, &scope)
        || (seek >= 0 && !tagcache_search_add_filter(&tcs, tag_album, seek)))
    {
        /* A filter refused leaves the search wider than the row asked for,
         * which would count somebody else's tracks into this album. */
        tagcache_search_finish(&tcs);
        return false;
    }

    while (tagcache_get_next(&tcs, title, sizeof(title)))
    {
        int disc, track, len;
        size_t avail = arena_sz - used;

        (*total)++;
        if (ls_book && ls_track_ct < LS_TRACKS)
        {
            char path[MAX_PATH];
            long d = tagcache_get_numeric(&tcs, tag_discnumber);
            long t = tagcache_get_numeric(&tcs, tag_tracknumber);
            long ms = tagcache_get_numeric(&tcs, tag_length);
            uint32_t key = (uint32_t)(((d & 0x7fff) << 16) | (t & 0xffff));

            ls_tracks[ls_track_ct].key = key;
            ls_tracks[ls_track_ct].ms = MAX(ms, 0);
            ls_track_ct++;
            if (!ls_at && ls_pos.track
                && tagcache_retrieve(&tcs, tcs.idx_id, tag_filename, path,
                                     sizeof(path))
                && path_key(path) == ls_pos.track)
            {
                ls_at = true;
                ls_at_key = key;
            }
        }
        if (tagcache_get_numeric(&tcs, tag_playcount) > 0)
        {
            (*played)++;
            continue;
        }

        if (!list || full || track_ct >= TRACKS_MAX)
            continue;

        /* snprintf reports what it would have written, so this is the test
         * for the title having fitted whole rather than for an error. Out of
         * room stops the listing and not the count: the figure above the list
         * has to describe the album the list is of. */
        len = snprintf(arena + used, avail, "%s", title);
        if (len < 0 || (size_t)len >= avail)
        {
            full = true;
            continue;
        }

        disc  = tagcache_get_numeric(&tcs, tag_discnumber);
        track = tagcache_get_numeric(&tcs, tag_tracknumber);
        if (disc < 0)
            disc = 0;
        if (track < 0)
            track = 0;

        tracks[track_ct].off = (uint32_t)used;
        /* Disc above track, so disc 2's track 1 follows disc 1's last. Both
         * are masked: they come from file tags, and a nonsense value must not
         * shift into the sign bit and sort itself to the front. */
        tracks[track_ct].key = (uint32_t)(((disc & 0x7fff) << 16)
                                          | (track & 0xffff));
        used += (size_t)len + 1;
        track_ct++;
    }

    tagcache_search_finish(&tcs);

    if (list)
        qsort(tracks, track_ct, sizeof(*tracks), compare_tracks);

    return true;
}

/* ------------------------------------------------------------------ *
 * an artist's albums                                                 *
 * ------------------------------------------------------------------ */

static int compare_albums(const void *a_v, const void *b_v)
{
    const struct lp_album *a = a_v;
    const struct lp_album *b = b_v;

    return strnatcasecmp(arena + a->off, arena + b->off);
}

/* Every album under the browse row, with what has been heard of each.
 *
 * Two passes because they cannot be one: the counting is a search of its own
 * per album, and a tagcache search cannot run inside another. So the names and
 * seeks are collected first and the figures filled in afterwards. */
static bool scan_artist(void)
{
    /* The dedupe list for the album search, sized against the ceiling on
     * albums rather than against the library. Without it the search reports
     * an album once per track of it. */
    static uint32_t uniq[ALBUMS_MAX * 2];
    struct tagcache_search tcs;
    char name[TAGCACHE_BUFSZ];
    size_t used = 0;

    album_ct = 0;
    artist_total = artist_played = 0;

    if (!tagcache_search(&tcs, tag_album))
        return false;

    tagcache_search_set_uniqbuf(&tcs, uniq, sizeof(uniq));

    if (!browser_db_add_filters(&tcs, &scope))
    {
        tagcache_search_finish(&tcs);
        return false;
    }

    while (album_ct < ALBUMS_MAX && tagcache_get_next(&tcs, name, sizeof(name)))
    {
        size_t avail = arena_sz - used;
        int len = snprintf(arena + used, avail, "%s", name);

        if (len < 0 || (size_t)len >= avail)
            break;

        albums[album_ct].seek = tcs.result_seek;
        albums[album_ct].off = (uint32_t)used;
        used += (size_t)len + 1;
        album_ct++;
    }

    tagcache_search_finish(&tcs);
    arena_albums = used;

    if (album_ct == 0)
        return false;

    qsort(albums, album_ct, sizeof(*albums), compare_albums);

    splash_progress_set_delay(HZ / 2);
    for (int i = 0; i < album_ct; i++)
    {
        splash_progress(i, album_ct, "%s", str(LANG_WAIT));
        if (!walk_album(albums[i].seek, false, &albums[i].total,
                        &albums[i].played))
            return false;
        artist_total += albums[i].total;
        artist_played += albums[i].played;
        yield();
    }

    return true;
}

/* ------------------------------------------------------------------ *
 * Listening Stats' lines                                             *
 * ------------------------------------------------------------------ */

/* A line "label: value", value formatted */
static void ls_add(int label, const char *fmt, ...)
{
    char value[64];
    va_list ap;

    if (ls_n == LS_LINES)
        return;
    va_start(ap, fmt);
    vsnprintf(value, sizeof(value), fmt, ap);
    va_end(ap);
    snprintf(ls_line[ls_n++], sizeof(ls_line[0]), "%s: %s", str(label),
             value);
}

static const char *ls_time(char *buf, size_t size, uint32_t secs)
{
    return format_time_auto(buf, size, secs, UNIT_SEC, false);
}

static void ls_date(char *buf, size_t size, int day)
{
    int y, m, d;

    pv_civil_from_days(day + LS_EPOCH, &y, &m, &d);
    snprintf(buf, size, "%s %d %d", str(LANG_MONTH_JANUARY + m - 1), d, y);
}

/* n days, or one */
static const char *ls_days(char *buf, size_t size, int n)
{
    if (n == 1)
        return str(LANG_RS_ONE_DAY);
    snprintf(buf, size, str(LANG_RS_DAYS), n);
    return buf;
}

/* What is left of the book walked, at the Spoken Word Speed, in seconds;
 * -1 when the book is finished or its place was not found */
static long ls_left(void)
{
    const bool unstarted = !ls_pos.track
                           || ls_pos.left == BOOK_LEFT_UNSTARTED;
    uint64_t ms = 0;
    int i;

    if (ls_pos.left == BOOK_LEFT_FINISHED || (!unstarted && !ls_at))
        return -1;
    for (i = 0; i < ls_track_ct; i++)
    {
        if (unstarted || ls_tracks[i].key > ls_at_key)
            ms += ls_tracks[i].ms;
        else if (ls_tracks[i].key == ls_at_key
                 && ls_pos.left == BOOK_LEFT_PARTWAY
                 && ls_tracks[i].ms > ls_pos.elapsed)
            ms += ls_tracks[i].ms - ls_pos.elapsed;
    }
    return (long)(ms * 100 / MAX(global_settings.spoken_speed, 1) / 1000);
}

/* The figures that every Listening Stats screen has */
static void ls_add_figures(const struct listen_stats *r)
{
    char t[32];

    if (r->seconds)
        ls_add(LANG_LS_TIME, "%s", ls_time(t, sizeof(t), r->seconds));
    if (r->longest)
        ls_add(LANG_RS_LONGEST, "%s", ls_time(t, sizeof(t), r->longest));
    if (r->best_streak)
        ls_add(LANG_RS_STREAK, "%s", ls_days(t, sizeof(t), r->best_streak));
}

/* One book's or show's lines, 'key' its book_resume_key() */
static void ls_build(uint64_t key)
{
    struct listen_stats r;
    char d[40], n[24];
    long left;

    ls_n = 0;
    listen_stats_get(key, &r);
    ls_add_figures(&r);
    if (ls_book && (left = ls_left()) > 0)
        ls_add(LANG_RS_LEFT, str(LANG_RS_ABOUT),
               ls_time(n, sizeof(n), (uint32_t)left));
    if (r.started)
    {
        ls_date(d, sizeof(d), r.started);
        ls_add(LANG_RS_STARTED, "%s", d);
    }
    if (r.last && r.last != r.finished)
    {
        ls_date(d, sizeof(d), r.last);
        ls_add(LANG_LS_LAST, "%s", d);
    }
    if (r.finished)
    {
        ls_date(d, sizeof(d), r.finished);
        if (r.started && r.finished >= r.started)
            ls_add(LANG_BOOKS_FINISHED, str(LANG_RS_IN_DAYS), d,
                   ls_days(n, sizeof(n), r.finished - r.started + 1));
        else
            ls_add(LANG_BOOKS_FINISHED, "%s", d);
    }
}

/* ------------------------------------------------------------------ *
 * the two lists                                                      *
 * ------------------------------------------------------------------ */

/* Both lists open on the statement the screen exists to make, which is why
 * row 0 is not part of the list proper and everything below it is offset by
 * one. It is the only place the statement can go: a list has one title line,
 * and that line is already carrying the album or artist name. */
static const char *summary_row(char *buffer, size_t buffer_len,
                               int played, int total)
{
    snprintf(buffer, buffer_len, str(LANG_LISTEN_PROGRESS_SUMMARY),
             percent(played, total), played, total);
    return buffer;
}

static const char *artist_get_name(int n, void *data, char *buffer,
                                   size_t buffer_len)
{
    const struct lp_album *a;
    (void)data;

    if (n == 0)
        return summary_row(buffer, buffer_len, artist_played, artist_total);

    a = &albums[n - 1];
    snprintf(buffer, buffer_len, str(LANG_LISTEN_PROGRESS_ALBUM),
             arena + a->off, percent(a->played, a->total),
             a->total - a->played);
    return buffer;
}

static const char *track_get_name(int n, void *data, char *buffer,
                                  size_t buffer_len)
{
    (void)data;

    if (n < ls_n)
        return ls_line[n];
    n -= ls_n;
    if (n == 0)
        return summary_row(buffer, buffer_len, track_played, track_total);

    return arena + tracks[n - 1].off;
}

/* There is nothing to select on a track list, and nothing to select on either
 * list's summary row. Swallowing SELECT rather than letting it end the list
 * keeps the screen where the user left it: a report that closes when you press
 * the wrong button reads as a crash. */
static int track_action_cb(int action, struct gui_synclist *lists)
{
    (void)lists;
    return action == ACTION_STD_OK ? ACTION_NONE : action;
}

static int artist_action_cb(int action, struct gui_synclist *lists)
{
    if (action == ACTION_STD_OK && gui_synclist_get_sel_pos(lists) == 0)
        return ACTION_NONE;
    return action;
}

/* Whatever tracks[] already holds, under 'title'. */
static int run_track_list(const char *title)
{
    struct simplelist_info info;

    simplelist_info_init(&info, (char *)title, ls_n + track_ct + 1, NULL);
    info.get_name = track_get_name;
    info.action_callback = track_action_cb;

    return simplelist_show_list(&info) ? GO_TO_ROOT : GO_TO_PREVIOUS;
}

static int run_album_list(const char *title)
{
    struct simplelist_info info;
    int selection = 1;
    int ret = GO_TO_PREVIOUS;
    bool ok;

    while (1)
    {
        simplelist_info_init(&info, (char *)title, album_ct + 1, NULL);
        info.get_name = artist_get_name;
        info.action_callback = artist_action_cb;
        info.selection = selection;

        if (simplelist_show_list(&info))
        {
            ret = GO_TO_ROOT;
            break;
        }
        if (info.selection <= 0)
            break;

        /* Kept, so that backing out of an album returns to it rather than to
         * the top of the list. */
        selection = info.selection;

        cpu_boost(true);
        ok = walk_album(albums[selection - 1].seek, true, &track_total,
                        &track_played);
        cpu_boost(false);
        if (!ok)
        {
            splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
            continue;
        }

        ret = run_track_list(arena + albums[selection - 1].off);
        if (ret == GO_TO_ROOT)
            break;
    }

    return ret;
}

/* ------------------------------------------------------------------ *
 * the way in                                                         *
 * ------------------------------------------------------------------ */

/* Whatever 'scope' names, under 'title'; with Listening Stats when 'stats'
 * names a book or show by its book_resume_key() */
static int show(const char *title, uint64_t stats)
{
    bool artist = (scope.scope == BROWSER_DB_SCOPE_ARTIST);
    bool ok;
    int ret;

    push_current_activity(ACTIVITY_LISTENPROGRESS);
    arena = app_claim_buffer(&arena_sz, "listening progress");
    arena_albums = 0;

    cpu_boost(true);
    ok = artist ? scan_artist()
                : walk_album(-1, true, &track_total, &track_played);
    cpu_boost(false);
    ls_n = 0;
    if (ok && stats)
        ls_build(stats);

    if (!ok)
    {
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        ret = GO_TO_PREVIOUS;
    }
    else
        ret = artist ? run_album_list(title) : run_track_list(title);

    app_release_buffer("listening progress");
    arena = NULL;
    pop_current_activity();

    return ret;
}

int listen_progress_show(void)
{
    /* Copied rather than pointed at: the browse row's entry lives in the
     * browser's paging buffer, and a scan can page a different chunk of the
     * level in underneath it. */
    char title[MAX_PATH];

    /* Both of these read the browse row, so both happen before show()'s
     * claim: paging a chunk of the level in borrows the same buffer. */
    if (!browser_db_current_filters(&scope))
        return GO_TO_PREVIOUS;
    browser_db_current_entry_name(title, sizeof(title));
    ls_book = false;
    return show(title, 0);
}

int listen_progress_show_book(long album_seek, long artist_seek,
                              const char *title)
{
    memset(&scope, 0, sizeof(scope));
    scope.scope = BROWSER_DB_SCOPE_ALBUM;
    scope.tag[0] = tag_albumartist;
    scope.seek[0] = artist_seek;
    scope.tag[1] = tag_album;
    scope.seek[1] = album_seek;
    scope.count = 2;

    /* A book's or a show's stats are kept under the book resume file's
     * key; a file with no album has none */
    char id[BOOK_ID_MAX];
    struct tagcache_album al;
    uint64_t key = 0;
    int row = tagcache_album_find(album_seek, artist_seek);

    ls_book = false;
    if (book_resume_id_of(album_seek, artist_seek, id, sizeof(id)))
    {
        ls_book = row >= 0 && tagcache_album_get(row, &al)
                  && db_spoken_album_kind(&al) == 1;
        /* A show's are kept by its name alone, as listen_stats.c keeps
         * them, whatever artist each episode names */
        key = book_resume_key(ls_book ? id : title);
        if (!ls_book || !book_resume_find(id, &ls_pos))
        {
            memset(&ls_pos, 0, sizeof(ls_pos));
            ls_pos.left = BOOK_LEFT_UNSTARTED;
        }
    }
    return show(title, key);
}

int listen_progress_show_books(void)
{
    struct simplelist_info info;
    struct listen_stats all;
    int books, finished;
    char d[40];

    ls_n = 0;
    listen_stats_sum(1, &all, &books, &finished);
    if (!books)
        strmemccpy(ls_line[ls_n++], str(LANG_LS_NOT_HEARD),
                   sizeof(ls_line[0]));
    else
    {
        ls_add(LANG_LS_BOOKS, str(LANG_RS_BOOKS_FINISHED), books, finished);
        ls_add_figures(&all);
        if (all.started)
        {
            ls_date(d, sizeof(d), all.started);
            ls_add(LANG_LS_SINCE, "%s", d);
        }
    }
    simplelist_info_init(&info, str(LANG_LISTENING_STATS), ls_n, NULL);
    info.get_name = track_get_name;
    return simplelist_show_list(&info) ? GO_TO_ROOT : GO_TO_PREVIOUS;
}
