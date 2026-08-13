/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Text search across the tag database: type, and matching titles, albums and
 * album artists appear. Reached from the root menu (off by default) and from
 * Music; both rows are hidden unless the database is held in RAM.
 *
 * The box itself is widgets/search_dialog.c. This is the provider behind it:
 * what a match is, where matches are kept, and what selecting one does.
 *
 * The scan passes no filter and no clause, so tagcache_get_next() crawls the
 * tag file sequentially instead of walking the master index. It runs when the
 * query settles, not per keystroke.
 *
 * Selecting a result returns a GO_TO_* code for root_menu.c to dispatch.
 *
 * Three settings shape it, under General Settings > Search: how many results
 * are kept, how many letters must be typed before a scan runs, and which of
 * the three tags leads.
 *
 * Parts, in order:
 *   - the matches
 *   - the scan
 *   - the provider
 *   - acting on a result
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "string-extra.h"
#include "settings/settings.h"
#include "lang.h"
#include "widgets/search_dialog.h"
#include "widgets/splash.h"
#include "database/tagcache.h"
#include "playlist/playlist.h"
#include "screens/browse/browser_db.h"   /* the two enter_..._on_next_load */
#include "system/activity.h"
#include "root_menu.h"                /* the GO_TO_* codes returned */
#include "db_search.h"

/* The "bitmaps/" prefix is required, not cosmetic: dependencies are generated
 * with -MG, so a header that does not exist yet is recorded at the path as
 * written. Only this spelling matches the rule bitmaps.make gives it. */
#include "bitmaps/podbox_icon_album.h"
#include "bitmaps/podbox_icon_artist.h"
#include "bitmaps/podbox_icon_track.h"

#define MAX_MATCHES  200
#define NAME_ARENA   (8 * 1024)

/* Which tags are searched, one row per value of the order setting. Results are
 * appended in scan order and never sorted, so the row read is also the order
 * they appear in. Title is per track and dominates the cost; the other two are
 * unique-valued tags whose blobs hold one entry per distinct value, so they are
 * nearly free.
 *
 * tag_artist is deliberately absent. It is the per-track artist, so on any
 * compilation it produces a row per guest that leads to the same albums the
 * album artist already does -- duplicates that push real results off a list
 * this short. What a search means by "artist" is the album artist. */
static const int search_tags[DB_SEARCH_ORDER_COUNT][3] = {
    { tag_title,       tag_album,       tag_albumartist },
    { tag_title,       tag_albumartist, tag_album       },
    { tag_album,       tag_title,       tag_albumartist },
    { tag_album,       tag_albumartist, tag_title       },
    { tag_albumartist, tag_album,       tag_title       },
    { tag_albumartist, tag_title,       tag_album       },
};

/* Both count settings are indices into evenly spaced choice lists, so the
 * value is arithmetic rather than a second table to hold in step with
 * settings_list.c. */
static int setting_max_rows(void)
{
    return 25 * (global_settings.db_search_max_rows + 1);
}

static int setting_min_letters(void)
{
    return global_settings.db_search_min_letters + 1;
}

/* The query the screen last closed on, so reopening resumes rather than
 * starting blank. Deliberately not a setting: it is a convenience within a
 * session, not something worth persisting across a boot. */
static char last_query[SEARCH_MAX_QUERY + 1];

/* ---- the matches -------------------------------------------------------- */

static struct match {
    uint16_t name_off;          /* into name_arena */
    uint8_t  tag;
    int32_t  seek;              /* tag seek: what the browser is armed with */
    int32_t  idx_id;            /* master index id: what a track is played by */
} matches[MAX_MATCHES];

static int  match_count;
static char name_arena[NAME_ARENA];
static int  arena_used;
static int  row_limit;          /* the max-rows setting, taken once per scan */

static bool add_match(int tag, const char *name, int32_t seek, int32_t idx_id)
{
    int len = strlen(name) + 1;

    if (match_count >= row_limit || arena_used + len > NAME_ARENA)
        return false;

    matches[match_count].name_off = arena_used;
    matches[match_count].tag = tag;
    matches[match_count].seek = seek;
    matches[match_count].idx_id = idx_id;
    match_count++;

    memcpy(&name_arena[arena_used], name, len);
    arena_used += len;
    return true;
}

/* ---- the scan ----------------------------------------------------------- */

/* Re-run the whole search for `query`.
 *
 * The loop does not yield. What keeps this off the critical path is the settle
 * delay in front of it, not chunking: a scan only starts once the query has
 * stood still, and on a library this size it is over in a fraction of that.
 *
 * The clause machinery is deliberately unused. A clause makes tagcache walk
 * the master index -- one 96-byte index_entry per track, plus a scattered read
 * into the string blob for each. With no filter and no clause,
 * tagcache_get_next() instead crawls the tag file sequentially: the same
 * strings for a fraction of the memory traffic, and already sorted. */
static int run_search(const char *query, void *ctx)
{
    char buf[TAGCACHE_BUFSZ];
    struct tagcache_search tcs;
    const int *tags = search_tags[global_settings.db_search_order];
    unsigned int i;

    (void)ctx;

    match_count = 0;
    arena_used = 0;

    /* The setting is a second, lower ceiling; MAX_MATCHES and NAME_ARENA stay
     * the hard array bounds, so no setting can overrun them. */
    row_limit = MIN(setting_max_rows(), MAX_MATCHES);

    /* Too short to bother with -- and, since the threshold is at least one
     * letter, this is also the empty-query case. Everything above is already
     * cleared, so a query backspaced below the threshold empties the results
     * rather than leaving the last scan's on screen. */
    if ((int)strlen(query) < setting_min_letters())
        return 0;

    for (i = 0; i < ARRAYLEN(search_tags[0]); i++)
    {
        if (!tagcache_search(&tcs, tags[i]))
            continue;

        while (tagcache_get_next(&tcs, buf, sizeof(buf)))
        {
            if (strcasestr(buf, query)
                && !add_match(tags[i], buf, tcs.result_seek, tcs.idx_id))
                break;
        }

        tagcache_search_finish(&tcs);
    }

    return match_count;
}

/* ---- the provider ------------------------------------------------------- */

static const char *match_text(int i, void *ctx)
{
    (void)ctx;
    return &name_arena[matches[i].name_off];
}

/* The icon says which tag matched, so the row is just the text. Album artist
 * shares the artist icon -- the distinction is not worth a third glyph. */
static const struct bitmap *match_icon(int i, void *ctx)
{
    (void)ctx;
    switch (matches[i].tag)
    {
        case tag_album:       return &bm_podbox_icon_album;
        case tag_artist:
        case tag_albumartist: return &bm_podbox_icon_artist;
        default:              return &bm_podbox_icon_track;
    }
}

/* ---- acting on a result ------------------------------------------------- */

/* A title row is one track, so it plays rather than browsing to itself. The
 * path is not held in the match -- only the master index id is -- because the
 * filename is the one tag the ramcache does not keep as a string. */
static int play_track(const struct match *m)
{
    struct tagcache_search tcs;
    char path[MAX_PATH];
    bool got;

    if (!tagcache_search(&tcs, tag_filename))
        return GO_TO_PREVIOUS;
    got = tagcache_retrieve(&tcs, m->idx_id, tag_filename, path, sizeof(path));
    tagcache_search_finish(&tcs);

    if (!got || playlist_create(NULL, NULL) < 0)
        return GO_TO_PREVIOUS;
    if (playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false, true) < 0)
        return GO_TO_PREVIOUS;

    playlist_start(0, 0, 0);
    return GO_TO_WPS;
}

static int act_on(const struct match *m)
{
    const char *name = &name_arena[m->name_off];

    switch (m->tag)
    {
        case tag_album:
            /* straight into that album's tracks, as Album covers does */
            browser_db_enter_album_tracks_on_next_load(m->seek, name);
            return GO_TO_ALBUM_COVERS_TRACKS;
        case tag_albumartist:
            /* straight into that artist's albums, as Artist portraits does */
            browser_db_enter_artist_albums_on_next_load(m->seek, name);
            return GO_TO_ALBUM_COVERS_TRACKS;
        default:
            return play_track(m);
    }
    return GO_TO_PREVIOUS;
}

int db_search_run(void)
{
    static const struct search_provider provider = {
        .activity = ACTIVITY_DB_SEARCH,
        .scan     = run_search,
        .row_text = match_text,
        .row_icon = match_icon,
    };
    struct search_provider p = provider;
    int chosen;

    /* Both gates the real feature would need. A commit holds tagcache's
     * read_lock for its whole length and tagcache_search() waits on it with
     * sleep(1), so calling in from a foreground screen during one is a freeze
     * with no way out; and off the ramcache every entry costs a seek and a
     * read, which on a disk is not slow but unusable. */
    if (!tagcache_is_usable() || tagcache_get_commit_step() != 0)
    {
        splash(HZ * 2, "Database busy");
        return GO_TO_PREVIOUS;
    }
    if (!tagcache_is_in_ram())
    {
        splash(HZ * 2, "Needs the database in RAM");
        return GO_TO_PREVIOUS;
    }

    /* str() is not a constant expression, so the title is filled in here
     * rather than in the initialiser above. */
    p.title = str(LANG_DB_SEARCH);

    match_count = 0;
    arena_used = 0;

    chosen = search_dialog_run(&p, NULL, last_query, sizeof(last_query));

    if (chosen == SEARCH_USB)
        return GO_TO_ROOT;        /* USB: root_menu handles the screen */

    if (chosen >= 0)
        return act_on(&matches[chosen]);

    return GO_TO_PREVIOUS;
}
