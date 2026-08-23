/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Featured artists: the guests the library credits, and their tracks.
 *
 * Two lists over the table in database/db_featured.c -- the names it holds,
 * and the tracks recorded against one of them. Nothing here reads a tag file
 * to find a credit; that is done once, and this is what looks at the result.
 *
 * The tracks do need the database: the table records a master-index id per
 * credit and nothing else, because a title and an album artist are strings
 * the table would otherwise have to hold a second copy of. So a track list
 * retrieves its rows when it opens, into borrowed memory, and gives it back
 * before anything else can want it.
 *
 * Parts, in order:
 *   - the guest list, in name order
 *   - one guest's tracks: retrieving the rows, and drawing them
 *   - playing what the list shows
 *   - the two ways in: every guest, and one artist's guest appearances
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
#include "input/action.h"
#include "system/activity.h"
#include "system/app_buffer.h"       /* where the track rows are read into */
#include "system/app_util.h"         /* warn_on_pl_erase */
#include "database/tagcache.h"
#include "database/db_featured.h"
#include "screens/browse/browser_db.h"
#include "playlist/playlist.h"
#include "root_menu.h"
#include "featured_artists.h"

/* Tracks one guest's list will show. The table holds at most
 * DB_FEATURED_PAIR_MAX credits in total, so this only bites on a guest who
 * has more of the library to themselves than anyone is likely to; the rest
 * are dropped from the list and from what it plays. */
#define TRACKS_MAX 256

/* ------------------------------------------------------------------ *
 * the guest list                                                     *
 * ------------------------------------------------------------------ */

/* The guests in name order. The table itself is in discovery order -- which
 * is the order the tag files happened to be crawled in -- and the pairs index
 * into it by position, so it is this that is sorted rather than the table. */
static uint16_t order[DB_FEATURED_GUEST_MAX];
static int order_ct;

static int compare_guests(const void *a, const void *b)
{
    return strnatcasecmp(db_featured_name(*(const uint16_t *)a),
                         db_featured_name(*(const uint16_t *)b));
}

static void sort_guests(void)
{
    order_ct = MIN(db_featured_count(), DB_FEATURED_GUEST_MAX);

    for (int i = 0; i < order_ct; i++)
        order[i] = (uint16_t)i;

    qsort(order, order_ct, sizeof(*order), compare_guests);
}

static const char *guest_get_name(int n, void *data, char *buffer,
                                  size_t buffer_len)
{
    int g = order[n];

    (void)data;
    snprintf(buffer, buffer_len, "%s (%d)", db_featured_name(g),
             db_featured_track_count(g));
    return buffer;
}

/* What the context action asked for, since a list callback can only end the
 * list and not say why. */
static int pending_go_to;

/* A guest who is also an album artist has somewhere else to be: their own
 * albums. That is one alternative to the row's own action, and a menu of one
 * is worse than the action itself, so context goes straight there. A guest
 * with no albums of their own -- which is most of them, and the reason this
 * feature exists -- has nowhere to go and context does nothing. */
static int guest_action_cb(int action, struct gui_synclist *lists)
{
    if (action == ACTION_STD_CONTEXT)
    {
        int g = order[gui_synclist_get_sel_pos(lists)];
        long seek = db_featured_artist_seek(g);

        if (seek >= 0)
        {
            browser_db_enter_artist_albums_on_next_load(seek,
                                                        db_featured_name(g));
            pending_go_to = GO_TO_ALBUM_COVERS_TRACKS;
            return ACTION_STD_CANCEL;
        }
    }

    return action;
}

/* ------------------------------------------------------------------ *
 * one guest's tracks                                                 *
 * ------------------------------------------------------------------ */

static int32_t track_id[TRACKS_MAX];
/* Offsets into the borrowed arena, which is the whole app buffer and far
 * larger than 64K -- so these are not uint16_t as the table's own are. */
static uint32_t track_off[TRACKS_MAX];
static int track_ct;
static const char *track_rows;

/* Fill the arena with "Title - Album Artist" per track, and track_off with
 * where each landed. Returns how many fitted, which is the length of the list
 * from here on -- a row with no text is worse than a shorter list. */
static int read_track_rows(char *arena, size_t arena_sz)
{
    struct tagcache_search tcs;
    char title[TAGCACHE_BUFSZ];
    char artist[TAGCACHE_BUFSZ];
    size_t used = 0;
    int ct = 0;

    if (!tagcache_search(&tcs, tag_title))
        return 0;

    for (int i = 0; i < track_ct; i++)
    {
        size_t avail = arena_sz - used;
        int len;

        if (!tagcache_retrieve(&tcs, track_id[i], tag_title,
                               title, sizeof(title)))
            continue;
        if (!tagcache_retrieve(&tcs, track_id[i], tag_albumartist,
                               artist, sizeof(artist)))
            artist[0] = '\0';

        if (artist[0])
            len = snprintf(arena + used, avail, "%s - %s", title, artist);
        else
            len = snprintf(arena + used, avail, "%s", title);

        /* snprintf reports what it would have written, so this is the test
         * for the row having fitted whole rather than for an error. */
        if (len < 0 || (size_t)len >= avail)
            break;

        track_off[ct] = (uint32_t)used;
        track_id[ct] = track_id[i];      /* close the gaps left by a failed
                                          * retrieve, so the ids still line up
                                          * with the rows */
        used += (size_t)len + 1;
        ct++;
    }

    tagcache_search_finish(&tcs);
    return ct;
}

static const char *track_get_name(int n, void *data, char *buffer,
                                  size_t buffer_len)
{
    (void)data;
    (void)buffer;
    (void)buffer_len;
    return track_rows + track_off[n];
}

/* ------------------------------------------------------------------ *
 * playing it                                                         *
 * ------------------------------------------------------------------ */

/* Everything the list showed, starting at the row chosen -- so a guest's
 * appearances play on through one another rather than stopping after one.
 *
 * The paths are not in the table: the filename is the one tag the ramcache
 * does not hold as a string, so each is retrieved here. Nothing borrows the
 * app buffer for this, which is why the rows have to have been given back
 * before it runs -- playlist_insert_context_add() takes it itself. */
static int play_from(int start)
{
    struct tagcache_search tcs;
    struct playlist_insert_context context;
    char path[MAX_PATH];
    int added = 0, start_index = 0;

    if (!warn_on_pl_erase())
        return GO_TO_PREVIOUS;
    if (playlist_create(NULL, NULL) < 0)
        return GO_TO_PREVIOUS;

    cpu_boost(true);

    if (!tagcache_search(&tcs, tag_filename))
    {
        cpu_boost(false);
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        return GO_TO_PREVIOUS;
    }
    if (playlist_insert_context_create(NULL, &context, PLAYLIST_INSERT_LAST,
                                       false, false) < 0)
    {
        tagcache_search_finish(&tcs);
        cpu_boost(false);
        return GO_TO_PREVIOUS;
    }

    for (int i = 0; i < track_ct; i++)
    {
        if (!tagcache_retrieve(&tcs, track_id[i], tag_filename,
                               path, sizeof(path)))
            continue;
        if (playlist_insert_context_add(&context, path) < 0)
            break;
        /* Where the chosen row ended up, which is not i once a track has
         * failed to resolve. */
        if (i == start)
            start_index = added;
        added++;
    }

    playlist_insert_context_release(&context);
    tagcache_search_finish(&tcs);
    cpu_boost(false);

    if (added <= 0)
        return GO_TO_PREVIOUS;

    playlist_start(start_index, 0, 0);
    return GO_TO_WPS;
}

/* ------------------------------------------------------------------ *
 * the two levels                                                     *
 * ------------------------------------------------------------------ */

/* Whatever track_id/track_ct already hold, under 'title'. GO_TO_WPS if
 * something was played, GO_TO_PREVIOUS otherwise. */
static int run_track_list(const char *title)
{
    struct simplelist_info info;
    char *arena;
    size_t arena_sz = 0;
    int ret = GO_TO_PREVIOUS;

    arena = app_claim_buffer(&arena_sz, "featured tracks");
    track_ct = read_track_rows(arena, arena_sz);
    track_rows = arena;

    if (track_ct == 0)
    {
        app_release_buffer("featured tracks");
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        return GO_TO_PREVIOUS;
    }

    simplelist_info_init(&info, (char *)title, track_ct, NULL);
    info.get_name = track_get_name;
    if (simplelist_show_list(&info))
        ret = GO_TO_ROOT;

    /* Given back before anything is played: the insert context wants it. */
    app_release_buffer("featured tracks");
    track_rows = NULL;

    if (info.selection >= 0)
        ret = play_from(info.selection);

    return ret;
}

/* Guest 'g' of the table, from the guest list above. */
static int show_tracks(int g)
{
    track_ct = db_featured_track_ids(g, track_id, TRACKS_MAX);
    if (track_ct == 0)
        return GO_TO_PREVIOUS;

    return run_track_list(db_featured_name(g));
}

/* The artist the browser's [Featured In] row was drawn against. Held here
 * because a browse level hands back a bare screen code with nowhere to carry
 * a name, and copied because current_title[] is the browser's own. */
static char armed_artist[128];

void featured_artists_arm(const char *artist)
{
    strmemccpy(armed_artist, artist, sizeof(armed_artist));
}

int featured_artists_run(void)
{
    int ret;

    track_ct = db_featured_guest_tracks(armed_artist, track_id, TRACKS_MAX);
    if (track_ct == 0)
        return GO_TO_PREVIOUS;

    push_current_activity(ACTIVITY_FEATUREDARTISTS);
    ret = run_track_list(armed_artist);
    pop_current_activity();

    return ret;
}

int featured_artists_show(void)
{
    struct simplelist_info info;
    int ret = GO_TO_PREVIOUS;
    int selection = 0;

    if (db_featured_count() == 0)
        return GO_TO_PREVIOUS;

    push_current_activity(ACTIVITY_FEATUREDARTISTS);
    sort_guests();
    pending_go_to = 0;

    while (1)
    {
        simplelist_info_init(&info, str(LANG_FEATURED_ARTISTS), order_ct, NULL);
        info.get_name = guest_get_name;
        info.action_callback = guest_action_cb;
        info.selection = selection;
        if (simplelist_show_list(&info))
        {
            ret = GO_TO_ROOT;
            break;
        }

        if (pending_go_to != 0)
        {
            ret = pending_go_to;
            break;
        }
        if (info.selection < 0)
            break;

        /* Come back to the guest that was chosen, not to the top. */
        selection = info.selection;

        ret = show_tracks(order[selection]);
        if (ret != GO_TO_PREVIOUS)
            break;
    }

    pop_current_activity();
    return ret;
}
