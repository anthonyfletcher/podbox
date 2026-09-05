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
 * The scope is the browse row the context menu was opened on, which is why
 * nothing here takes an album or an artist as an argument: browser_db.c scopes
 * the search to that row, ancestor levels and all.
 *
 * Two levels, and the artist one lists albums rather than tracks: an artist's
 * unheard tracks run to hundreds and say nothing about where the gaps are,
 * where "Pablo Honey, 0%" does.
 *
 * Parts, in order:
 *   - the arena the row text is written into
 *   - walking one album's tracks
 *   - collecting an artist's albums
 *   - the two lists
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
#include "cpu.h"                     /* cpu_boost */
#include "file.h"                    /* MAX_PATH */
#include "lang.h"
#include "strnatcmp.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"
#include "settings/settings.h"
#include "input/action.h"
#include "system/activity.h"
#include "system/app_buffer.h"       /* where the row text is written */
#include "database/tagcache.h"
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

    simplelist_info_init(&info, (char *)title, track_ct + 1, NULL);
    info.get_name = track_get_name;
    info.action_callback = track_action_cb;

    return simplelist_show_list(&info) ? GO_TO_ROOT : GO_TO_PREVIOUS;
}

static int run_album_list(const char *title)
{
    struct simplelist_info info;
    int selection = 1;
    int ret = GO_TO_PREVIOUS;

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
        if (!walk_album(albums[selection - 1].seek, true, &track_total,
                        &track_played))
        {
            cpu_boost(false);
            splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
            continue;
        }
        cpu_boost(false);

        ret = run_track_list(arena + albums[selection - 1].off);
        if (ret == GO_TO_ROOT)
            break;
    }

    return ret;
}

/* ------------------------------------------------------------------ *
 * the way in                                                         *
 * ------------------------------------------------------------------ */

/* Nothing played and nothing counting plays are the same screen and very
 * different problems, and 0% for the second makes the feature look broken
 * rather than the setting. Name the setting and offer it, as the album charts
 * do, and at most once a boot so that declining does not nag. */
static bool runtimedb_asked;

static void offer_runtimedb(void)
{
    if (global_settings.runtimedb || runtimedb_asked)
        return;

    runtimedb_asked = true;
    if (yesno_pop(str(LANG_RUNTIMEDB_OFF_PROMPT)))
    {
        global_settings.runtimedb = true;
        settings_save();
    }
}

int listen_progress_show(void)
{
    /* Copied rather than pointed at: the browse row's entry lives in the
     * browser's paging buffer, and a scan below can page a different chunk of
     * the level in underneath it. */
    char title[MAX_PATH];
    bool artist, ok;
    int ret;

    /* Both of these read the browse row, so both happen before the claim
     * below: paging a chunk of the level in borrows the same buffer. */
    if (!browser_db_current_filters(&scope))
        return GO_TO_PREVIOUS;
    browser_db_current_entry_name(title, sizeof(title));

    artist = (scope.scope == BROWSER_DB_SCOPE_ARTIST);

    push_current_activity(ACTIVITY_LISTENPROGRESS);
    arena = app_claim_buffer(&arena_sz, "listening progress");
    arena_albums = 0;

    cpu_boost(true);
    ok = artist ? scan_artist()
                : walk_album(-1, true, &track_total, &track_played);
    cpu_boost(false);

    if (!ok)
    {
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        ret = GO_TO_PREVIOUS;
    }
    else
    {
        if ((artist ? artist_played : track_played) == 0)
            offer_runtimedb();

        ret = artist ? run_album_list(title) : run_track_list(title);
    }

    app_release_buffer("listening progress");
    arena = NULL;
    pop_current_activity();

    return ret;
}
