/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Which folders the artwork cache found no art for.
 *
 * The cache has always known its own totals -- art_cache_get_counts() reports
 * how many album and artist folders it saw and how many ended up with a
 * thumbnail -- but the difference was only ever a number. This shows the
 * folders behind it, so a gap in the artwork is something that can be gone and
 * fixed rather than merely counted.
 *
 * A report, not a settings screen, which is why it sits here beside the
 * background-task view rather than under screens/settings/. It reads the list
 * the last completed pass wrote (see art_cache_noart_list()) and displays it;
 * nothing here scans, and nothing runs when the screen is closed.
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "kernel.h"
#include "lang.h"
#include "settings/settings.h"   /* ID2P */
#include "widgets/list.h"
#include "widgets/splash.h"
#include "input/action.h"
#include "files/path_list.h"
#include "metadata/art_cache.h"
#include "art_health.h"

static struct path_list list;

/* The folder's own name, not the path it sits at: for any tidily-laid-out
 * library that is the album or artist, and it is what the row is about. The
 * path is still what was stored, and is a keypress away. */
static const char *health_get_name(int selected_item, void *data,
                                   char *buffer, size_t buffer_len)
{
    (void)data; (void)buffer; (void)buffer_len;
    return path_list_leaf(&list, selected_item);
}

/* Paths are long and the row scrolls, so SELECT shows the whole thing at once
 * rather than leaving it to be read a character at a time. */
static int health_action(int action, struct gui_synclist *lists)
{
    if (action == ACTION_STD_OK)
    {
        splashf(HZ * 3, "%s",
                path_list_get(&list, gui_synclist_get_sel_pos(lists)));
        return ACTION_REDRAW;
    }
    return action;
}

bool art_health_screen(bool artists)
{
    struct simplelist_info info;
    char title[64];

    if (!path_list_load(&list, art_cache_noart_list(artists), PATH_LIST_MAX))
    {
        /* Nothing missing and no pass yet look the same from here, so say the
         * neutral thing rather than claiming the artwork is complete. */
        splash(HZ * 2, ID2P(LANG_ART_HEALTH_NONE));
        return false;
    }

    snprintf(title, sizeof(title), "%s (%d%s)",
             str(artists ? LANG_ART_HEALTH_ARTISTS : LANG_ART_HEALTH_ALBUMS),
             list.count, list.truncated ? "+" : "");

    simplelist_info_init(&info, title, list.count, NULL);
    info.get_name = health_get_name;
    info.action_callback = health_action;

    simplelist_show_list(&info);

    path_list_free(&list);
    return true;
}
