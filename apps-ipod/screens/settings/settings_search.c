/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Text search across the settings tree: type, and the settings whose name,
 * cfg name, topic or search words match appear, each saying which menu it
 * lives in. Selecting one opens it in place and returns to the results.
 *
 * The box is widgets/search_dialog.c and the scan is a settings_walk() over
 * the menu tree -- see settings_walk.h for why the tree and not settings[].
 * This is the provider between them.
 *
 * Parts, in order:
 *   - the hits
 *   - the provider
 *   - the entry point
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "lang.h"
#include "settings/settings.h"
#include "settings/settings_tags.h"
#include "widgets/menu.h"
#include "widgets/search_dialog.h"
#include "system/activity.h"
#include "root_menu.h"
#include "settings_walk.h"
#include "settings_search.h"

/* Far more than fits on screen. A longer list is not more useful: the answer
 * to too many results is another letter. */
#define MAX_HITS   80

/* ---- the hits ----------------------------------------------------------- */

static struct hit {
    const struct menu_item_ex *item;    /* the row it was found through */
    const char                *parent;  /* the menu that row sits in */
} hits[MAX_HITS];

static int hit_count;

/* Cross-listing puts one setting in two menus, so the same setting can be
 * reached twice in one walk. The first place it is found wins: showing it once
 * per menu it appears in is noise, not information. */
static bool already_hit(const void *variable)
{
    for (int i = 0; i < hit_count; i++)
        if (hits[i].item->variable == variable)
            return true;
    return false;
}

/* ---- the provider ------------------------------------------------------- */

static bool collect_match(const struct menu_item_ex *item,
                          const struct settings_list *setting,
                          const char *parent, void *ctx)
{
    const char *query = ctx;

    if (settings_tags_match(setting, query) && !already_hit(item->variable))
    {
        hits[hit_count].item = item;
        hits[hit_count].parent = parent;
        hit_count++;
    }

    return hit_count < MAX_HITS;
}

/* Two letters before anything runs. One letter over a tree this size matches
 * most of it, and a screenful of near-everything is worse than nothing. */
#define MIN_LETTERS 2

static int settings_scan(const char *query, void *ctx)
{
    (void)ctx;

    hit_count = 0;

    if ((int)strlen(query) < MIN_LETTERS)
        return 0;

    settings_walk(collect_match, (void*)query);

    return hit_count;
}

/* "Backlight  (LCD Settings)" -- the parent is what tells two settings of the
 * same name apart, and there are several such pairs. */
static const char *settings_row_text(int i, void *ctx)
{
    static char buf[MAX_PATH];
    const struct settings_list *setting;

    (void)ctx;

    setting = find_setting(hits[i].item->variable);
    if (!setting)
        return "";

    snprintf(buf, sizeof(buf), "%s  (%s)",
             settings_walk_item_label(hits[i].item, setting), hits[i].parent);
    return buf;
}

/* ---- the entry point ---------------------------------------------------- */

int settings_search_run(void)
{
    static const struct search_provider provider = {
        .activity = ACTIVITY_SETTINGS_SEARCH,
        .scan     = settings_scan,
        .row_text = settings_row_text,
        .row_icon = NULL,
    };
    /* Reopen on the last thing searched for: a search is usually one of a run,
     * and retyping the stem on a click wheel is most of the work. */
    static char last_query[SEARCH_MAX_QUERY + 1];
    struct search_provider p = provider;

    p.title = str(LANG_DB_SEARCH);

    /* Back to the results after each change rather than out to the menu.
     * Searching is how the setting was found, so it is also where the next one
     * is most likely to be found. */
    for (;;)
    {
        int chosen = search_dialog_run(&p, NULL, last_query,
                                       sizeof(last_query));

        if (chosen == SEARCH_USB)
            return GO_TO_ROOT;
        if (chosen < 0)
            return 0;

        do_setting_from_menu_standalone(hits[chosen].item, NULL);

        /* The change may have moved the setting out of the result set, and
         * the hits hold pointers into a walk that is now stale. Re-running the
         * scan on reopen is what makes that safe -- the dialog does it, since
         * it reopens with the query pending. */
    }
}
