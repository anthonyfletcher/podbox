/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A live view of the three background tasks: the tag database, the carousel's
 * album index and the artwork thumbnail cache.
 *
 * Everything shown is read from state those three already keep for their own
 * purposes -- nothing here asks them to measure anything, and nothing here
 * runs when the screen is closed. The one thing deliberately *not* shown is
 * the database's current file: tc_stat.curentry is only held still long enough
 * to read if tagcache_screensync_enable() is on, and that makes the scanning
 * thread yield in a loop waiting for this screen to acknowledge every entry --
 * it would slow the scan down to the refresh rate to display it. The Debug
 * menu's Database Info screen does that; this one stays out of the way.
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include "config.h"
#include "kernel.h"
#include "lang.h"
#include "widgets/list.h"
#include "input/action.h"
#include "database/tagcache.h"
#include "database/album_index.h"
#include "metadata/art_cache.h"
#include "system/bg_task.h"
#include "bg_task_info.h"

/* The database drives itself and is not ticked as a bg_task, so its state has
 * to be read from its own stat block rather than from bg_task_state(). */
static const char *tagcache_state(const struct tagcache_stat *stat)
{
    if (stat->commit_step > 0)
        return "Committing";
    if (stat->scanning)
        return "Scanning";
    if (!stat->initialized)
        return "Starting";
    if (!stat->ready)
        return "No database";
    return "Idle";
}

static void add_tagcache_lines(void)
{
    struct tagcache_stat *stat = tagcache_get_stat();

    simplelist_addline("Database: %s", tagcache_state(stat));

    if (stat->commit_step > 0)
        simplelist_addline("  Step %d of %d", stat->commit_step,
                           tagcache_get_max_commit_step());

    simplelist_addline("  Tracks found: %d", stat->total_entries);

    /* Only meaningful mid-scan; processed_entries is left over otherwise. */
    if (tagcache_is_busy())
        simplelist_addline("  Processed: %d (%d%%)", stat->processed_entries,
                           stat->progress);
}

static void add_album_index_lines(void)
{
    const char *step = album_index_activity();
    int done, total;

    album_index_progress(&done, &total);

    simplelist_addline("Album index: %s",
                       bg_task_state(&album_index_task));
    simplelist_addline("  Covered: %d entries", album_index_task.done_total);

    if (step[0])
    {
        /* Held over after a pass, so label it for what it is. */
        simplelist_addline("  %s: %s",
                           album_index_is_busy() ? "Step" : "Last step", step);
        if (album_index_is_busy() && total > 0)
            simplelist_addline("  Progress: %d/%d", done, total);
    }
}

static void add_art_cache_lines(void)
{
    struct art_cache_counts c;
    const char *dir = art_cache_activity();

    art_cache_get_counts(&c);

    simplelist_addline("Art cache: %s", bg_task_state(&art_cache_task));
    simplelist_addline("  Covered: %d entries", art_cache_task.done_total);

    if (dir[0])
        simplelist_addline("  Now: %s", dir);

    /* Zero until a pass has run this boot; the counts describe that pass. */
    if (c.albums || c.artists)
    {
        simplelist_addline("  Albums: %d seen, %d art", c.albums, c.album_art);
        simplelist_addline("  Artists: %d seen, %d art", c.artists,
                           c.artist_art);
    }
}

static int bg_task_info_callback(int action, struct gui_synclist *lists)
{
    (void)lists;

    simplelist_reset_lines();

    add_tagcache_lines();
    simplelist_addline(" ");
    add_album_index_lines();
    simplelist_addline(" ");
    add_art_cache_lines();

    if (action == ACTION_NONE)
        return ACTION_REDRAW;

    return action;
}

bool bg_task_info_screen(void)
{
    struct simplelist_info info;

    simplelist_info_init(&info, "Background Tasks", 0, NULL);
    info.action_callback = bg_task_info_callback;
    info.scroll_all = true;
    /* Twice a second. The tasks tick every five, so nothing here changes
     * faster than that, and a status screen is not worth a redraw per tick. */
    info.timeout = HZ / 2;

    return simplelist_show_list(&info);
}
