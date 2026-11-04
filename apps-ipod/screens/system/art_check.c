/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Check a File's Art: pick a file, have the art cache make its art again, and
 * read how it found each picture. The work and the report are the art
 * cache's (art_cache_check()); this picks the file, waits and shows the
 * report. The report is English whatever the language, as the logs are.
 ****************************************************************************/

#include <stdbool.h>
#include "config.h"
#include "kernel.h"
#include "lang.h"
#include "settings/settings.h"          /* ID2P, SHOW_ALL */
#include "input/action.h"
#include "widgets/splash.h"
#include "widgets/dialog_prose.h"
#include "screens/browse/browser.h"     /* browse_context, rockbox_browse */
#include "system/shutdown.h"            /* default_event_handler */
#include "system/activity.h"
#include "metadata/art_cache.h"
#include "art_check.h"

/* Static, not on the stack: a check left early still writes into it */
static char report[4096];

bool art_check_screen(void)
{
    char path[MAX_PATH];
    struct browse_context browse = {
        .dirfilter = SHOW_ALL,
        .flags = BROWSE_SELECTONLY | BROWSE_NO_CONTEXT_MENU,
        .title = ID2P(LANG_ART_CHECK),
        .icon = Icon_NOICON,
        .root = "/",
        .selected = NULL,
        .buf = path,
        .bufsize = sizeof path,
    };
    int walked, total, action;

    /* The file browser it is, so a theme dresses its rows as one */
    push_current_activity(ACTIVITY_FILEBROWSER);
    rockbox_browse(&browse);
    if (get_current_activity() == ACTIVITY_FILEBROWSER)
        pop_current_activity();
    if (!(browse.flags & BROWSE_SELECTED))
        return false;

    /* Refused only while a check left early is still running */
    if (!art_cache_check(path, report, sizeof report))
    {
        splash(HZ * 2, ID2P(LANG_WAIT));
        return false;
    }
    splash(0, ID2P(LANG_WAIT));
    while (!art_cache_check_finished(&walked, &total))
    {
        if (walked > 0 && total > 0)
            splash_progress(walked, total, "%s", str(LANG_ART_CHECK));
        action = get_action(CONTEXT_STD, HZ / 10);
        if (action == ACTION_STD_CANCEL)
            return false;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
            return true;
    }
    return dialog_prose_show(str(LANG_ART_CHECK), report);
}
