/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The charts: albums and artists ranked by how they have actually been
 * listened to, and a random album.
 *
 * The rankings cannot be written in tagnavi.config. A %format groups by the
 * tag it displays and sorts on the formatted string, so there is no way to
 * express "sum playcount across an album" -- which is why the database's own
 * Playback History menu is otherwise entirely track-level. They are computed
 * here instead, from figures the database index summarises per album and per
 * artist while it is built (see database/db_summary.c).
 *
 * Albums and artists rank identically -- most played, most recently played,
 * fewest plays -- and differ only in which array of the index they read, so
 * one set of comparisons serves both. That is what enum album_chart encodes:
 * a ranking and a domain.
 *
 * These are not screens the user navigates *to* as a group. Each chart is a
 * row in Playback History, and Random album is a row of its own, both added by
 * load_root() in browser_db.c; this file supplies what happens when one is
 * chosen. That is why the entry points are one-shot actions that claim the
 * index, do their job and release it, rather than a screen holding it open.
 *
 * Nothing here scans. It reads the saved index and sorts a list of indices
 * into it.
 *
 * Parts, in order:
 *   - the index, claimed per action
 *   - ranking, over either domain
 *   - the chart list
 *   - playing a random album
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"          /* MIN */
#include "kernel.h"
#include "cpu.h"             /* cpu_boost */
#include "lang.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"
#include "settings/settings.h"
#include "input/action.h"
#include "system/activity.h"
#include "system/app_buffer.h"        /* where the index is read into */
#include "database/tagcache.h"
#include "database/db_summary.h"   /* the index, and SUCCESS/ERROR_* */
#include "screens/browse/browser_db.h"
#include "playlist/playlist.h"
#include "root_menu.h"
#include "album_charts.h"

/* How many entries a chart shows. One constant for all six, so they cannot
 * drift apart. */
#define CHART_LEN 10

/* The rankings, shared by both domains. */
enum chart_rank {
    RANK_MOST_PLAYED = 0,
    RANK_RECENTLY_PLAYED,
    RANK_FEWEST_PLAYS,
};

/* The index, live only for the duration of one action. */
static struct db_summary_t idx;

/* The current chart: indices into whichever array it ranks, best first, with
 * the random tiebreak each entry was admitted on (see rank_before()). */
static int chart[CHART_LEN];
static long chart_tiebreak[CHART_LEN];
static int chart_len;
static enum album_chart chart_kind;

/* Whether the "runtime data is off" offer has been made this boot. */
static bool runtimedb_asked;

static enum chart_rank rank_of(enum album_chart kind)
{
    return CHART_IS_ARTIST(kind)
         ? (enum chart_rank)(kind - ARTIST_CHART_MOST_PLAYED)
         : (enum chart_rank)kind;
}

static int chart_pool(void)
{
    return CHART_IS_ARTIST(chart_kind) ? idx.artist_ct : idx.album_ct;
}

/* The two figures every ranking works on, whichever domain is being ranked.
 * Reading them through here is what lets one comparison serve both. */
struct chart_stats { int playcount; long lastplayed; };

static struct chart_stats stats_of(int i)
{
    struct chart_stats s;

    if (CHART_IS_ARTIST(chart_kind))
    {
        s.playcount = idx.artist_index[i].playcount;
        s.lastplayed = idx.artist_index[i].lastplayed;
    }
    else
    {
        s.playcount = idx.album_index[i].playcount;
        s.lastplayed = idx.album_index[i].lastplayed;
    }
    return s;
}

static const char *entry_name(int i)
{
    if (CHART_IS_ARTIST(chart_kind))
        return idx.artist_names + idx.artist_index[i].name_idx;
    return idx.album_names + idx.album_index[i].name_idx;
}

/* Albums show who they are by; an artist row is already the artist. */
static const char *entry_subtitle(int i)
{
    int a;

    if (CHART_IS_ARTIST(chart_kind))
        return NULL;
    a = idx.album_index[i].artist_idx;
    return a >= 0 ? idx.artist_names + a : NULL;
}

/* ---- ranking ----------------------------------------------------------- */

/* Most played and Recently played both need something to have happened, and
 * exclude the untouched. Fewest plays is the opposite: never-played entries
 * are exactly what it is for, so everything qualifies. */
static bool eligible(struct chart_stats s, enum chart_rank rank)
{
    switch (rank)
    {
        case RANK_MOST_PLAYED:      return s.playcount > 0;
        case RANK_RECENTLY_PLAYED:  return s.playcount > 0 && s.lastplayed > 0;
        case RANK_FEWEST_PLAYS:     return true;
    }
    return false;
}

/* Does (a, rnd_a) rank ahead of (b, rnd_b)?
 *
 * The tiebreak matters only for the fewest-plays ranking. On any real library
 * the whole top of that chart is a mass of entries with zero plays -- far more
 * than the ten shown. Ordered by anything stable it would show the same ten
 * every time, which is the opposite of a prompt to go and listen to something.
 * Each candidate gets a random key instead, so equal play counts come out as a
 * fresh sample on each visit. The other two rank on figures that rarely tie
 * and where a stable order is wanted, so they never consult it. */
static bool rank_before(struct chart_stats a, long rnd_a,
                        struct chart_stats b, long rnd_b,
                        enum chart_rank rank)
{
    switch (rank)
    {
        case RANK_MOST_PLAYED:
            return a.playcount > b.playcount;
        case RANK_RECENTLY_PLAYED:
            return a.lastplayed > b.lastplayed;
        case RANK_FEWEST_PLAYS:
            if (a.playcount != b.playcount)
                return a.playcount < b.playcount;
            return rnd_a < rnd_b;
    }
    return false;
}

/* Fill chart[] with the top CHART_LEN entries.
 *
 * Insertion into a fixed top-N rather than sorting the whole index: the list
 * is ten long and the library can be thousands of albums, so this is one pass
 * and no scratch memory, against a qsort that would need somewhere to put a
 * permutation of the lot. */
static void build_chart(enum album_chart kind)
{
    enum chart_rank rank;
    int i, j, k, pool;

    chart_len = 0;
    chart_kind = kind;
    rank = rank_of(kind);
    pool = chart_pool();
    srand(current_tick);

    for (i = 0; i < pool; i++)
    {
        struct chart_stats cand = stats_of(i);
        long rnd = rand();

        if (!eligible(cand, rank))
            continue;

        /* Where does it land? */
        for (j = 0; j < chart_len; j++)
        {
            if (rank_before(cand, rnd, stats_of(chart[j]),
                            chart_tiebreak[j], rank))
                break;
        }
        if (j == CHART_LEN)
            continue;                    /* worse than everything held */

        for (k = MIN(chart_len, CHART_LEN - 1); k > j; k--)
        {
            chart[k] = chart[k - 1];
            chart_tiebreak[k] = chart_tiebreak[k - 1];
        }
        chart[j] = i;
        chart_tiebreak[j] = rnd;
        if (chart_len < CHART_LEN)
            chart_len++;
    }
}

/* ---- the chart list ---------------------------------------------------- */

/* The entry, with the play count where that is what it is ranked on. Showing
 * the figure is what makes the ordering legible -- a bare list of ten looks
 * arbitrary. The recently-played ranking works off a serial rather than a date
 * (see tagcache_increase_serial()), so there is no figure worth showing. */
static const char *chart_get_name(int selected_item, void *data,
                                  char *buffer, size_t buffer_len)
{
    const char *name, *sub;
    struct chart_stats s;
    int i;
    (void)data;

    if (selected_item < 0 || selected_item >= chart_len)
    {
        buffer[0] = '\0';
        return buffer;
    }

    i = chart[selected_item];
    name = entry_name(i);
    sub = entry_subtitle(i);
    s = stats_of(i);

    if (rank_of(chart_kind) == RANK_RECENTLY_PLAYED)
        snprintf(buffer, buffer_len, sub ? "%s - %s" : "%s", name, sub);
    else if (sub)
        snprintf(buffer, buffer_len, "%s - %s (%d)", name, sub, s.playcount);
    else
        snprintf(buffer, buffer_len, "%s (%d)", name, s.playcount);

    return buffer;
}

static int chart_title(enum album_chart kind)
{
    switch (kind)
    {
        case ALBUM_CHART_MOST_PLAYED:      return LANG_MOST_PLAYED_ALBUMS;
        case ALBUM_CHART_RECENTLY_PLAYED:  return LANG_RECENTLY_PLAYED_ALBUMS;
        case ALBUM_CHART_FORGOTTEN:        return LANG_FORGOTTEN_ALBUMS;
        case ARTIST_CHART_MOST_PLAYED:     return LANG_MOST_PLAYED_ARTISTS;
        case ARTIST_CHART_RECENTLY_PLAYED: return LANG_RECENTLY_PLAYED_ARTISTS;
        case ARTIST_CHART_FORGOTTEN:       return LANG_FORGOTTEN_ARTISTS;
    }
    return LANG_MOST_PLAYED_ALBUMS;
}

/* An empty chart has two very different causes, and reporting "no history
 * yet" for both makes the feature look broken: with runtimedb off nothing is
 * ever counted, so the charts would stay empty however much was played. Name
 * the setting and offer it, rather than leaving it to be found under General
 * Settings. Asked at most once per boot, so declining does not nag. */
static void report_empty(void)
{
    if (!global_settings.runtimedb)
    {
        if (!runtimedb_asked)
        {
            runtimedb_asked = true;
            if (yesno_pop(str(LANG_RUNTIMEDB_OFF_PROMPT)))
            {
                global_settings.runtimedb = true;
                settings_save();
            }
        }
        return;
    }

    splash(HZ * 2, ID2P(LANG_NO_ALBUM_HISTORY));
}

/* Say why the index could not be read. */
static void report_index_error(int res)
{
    /* Chosen before ID2P rather than inside it: the macro does arithmetic on
     * its argument, so a conditional there is not the string id it looks
     * like. */
    int msg = (res == ERROR_BUFFER_FULL) ? LANG_OUT_OF_MEMORY
                                         : LANG_NO_ALBUM_HISTORY;
    splash(HZ * 2, ID2P(msg));
}

/* Read the index into the app buffer. SUCCESS, or an ERROR_* having already
 * said why.
 *
 * Not core_alloc(): the audio buffer holds the whole of core, so a request
 * from here shrinks it, and shrinking it stops playback and rebuffers the
 * current track. The app buffer is linker-reserved RAM that sits idle unless
 * a screen has borrowed it, so it costs nothing dynamic and playback never
 * notices. The carousel reads the same index into the same buffer.
 *
 * Claimed rather than borrowed because it is held while the chart is on
 * screen, which means nothing reachable from that list may want it. Nothing
 * does: the list passes no action callback, so it has no context menu, and the
 * handoff into the browser happens after release(). */
static int acquire(void)
{
    size_t bufsz;
    void *buf = app_claim_buffer(&bufsz, "album charts");
    int res;

    memset(&idx, 0, sizeof(idx));
    res = db_summary_build_into(&idx, buf, bufsz);

    if (res < SUCCESS)
    {
        app_release_buffer("album charts");
        report_index_error(res);
    }
    return res;
}

static void release(void)
{
    app_release_buffer("album charts");
}

/* See album_charts.h: the browser can only return a bare screen code, so which
 * chart was chosen is left here for root_menu.c's dispatch to pick up. */
static enum album_chart armed_chart = ALBUM_CHART_MOST_PLAYED;

void album_charts_arm(enum album_chart kind)
{
    armed_chart = kind;
}

int album_charts_run(void)
{
    return album_charts_show(armed_chart);
}

int album_charts_show(enum album_chart kind)
{
    struct simplelist_info info;
    int ret = GO_TO_PREVIOUS;

    if (acquire() < SUCCESS)
        return GO_TO_PREVIOUS;

    push_current_activity(ACTIVITY_ALBUMCHARTS);

    build_chart(kind);

    if (chart_len == 0)
        report_empty();
    else
    {
        simplelist_info_init(&info, str(chart_title(kind)), chart_len, NULL);
        info.get_name = chart_get_name;

        if (simplelist_show_list(&info))
            ret = GO_TO_ROOT;
        else if (info.selection >= 0)
        {
            /* The same handoffs Album covers and Artist portraits use: arm the
             * browser to enter directly on its next load, then return the code
             * that makes root_menu.c run it. The title is copied into the
             * browser's own buffer, so releasing the index does not strand
             * it. An artist opens their album list, an album its tracks. */
            int i = chart[info.selection];

            if (CHART_IS_ARTIST(kind))
                browser_db_enter_artist_albums_on_next_load(
                    idx.artist_index[i].seek, entry_name(i));
            else
                browser_db_enter_album_tracks_on_next_load(
                    idx.album_index[i].seek, entry_name(i));
            ret = GO_TO_ALBUM_COVERS_TRACKS;
        }
    }

    pop_current_activity();
    release();
    return ret;
}

/* ---- random album ------------------------------------------------------ */

/* Pick an album at random and start playing it.
 *
 * Deliberately not a browse: the request is that it plays, so this builds the
 * playlist and starts it rather than dropping the user in a track list. It
 * needs no playback history, so unlike the charts it works with runtime data
 * gathering switched off.
 *
 * One record is all this needs. The names in the index are for display and
 * nothing here displays anything, so it reads that record straight out of the
 * saved file rather than acquiring the whole index -- which would cost the
 * audio buffer 384K and the current track a rebuffer, moments before playback
 * is told to go somewhere else entirely. The charts still acquire; they read
 * every entry.
 *
 * The reader holds the build lock, so it is closed before anything slow
 * happens: db_summary_play_album() searches the database and starts playback,
 * neither of which should be waiting on this. */
int album_random(void)
{
    struct db_summary_reader r;
    struct album_data album;
    bool got;
    int res, pick;

    res = db_summary_reader_open(&r);
    if (res < SUCCESS)
    {
        report_index_error(res);
        return GO_TO_PREVIOUS;
    }

    /* An index with no albums in it is not opened at all, so there is always
     * something to pick from here. */
    srand(current_tick);
    pick = rand() % r.album_ct;
    got = db_summary_read_album(&r, pick, &album);
    db_summary_reader_close(&r);

    if (!got)
    {
        report_index_error(ERROR_NO_ALBUMS);
        return GO_TO_PREVIOUS;
    }

    return db_summary_play_album(&album) > 0 ? GO_TO_WPS : GO_TO_PREVIOUS;
}
