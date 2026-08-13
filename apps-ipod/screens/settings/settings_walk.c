/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Walking the settings menu tree. See settings_walk.h for why a screen over
 * settings walks the menus rather than settings[].
 ****************************************************************************/

#include <stdbool.h>
#include "config.h"
#include "system.h"
#include "lang.h"
#include "settings/settings.h"
#include "widgets/menu.h"
#include "settings_walk.h"

/* The full settings tree, as the root Settings entry opens it. */
extern const struct menu_item_ex main_menu_;

/* The tree is about five deep. The guard is against a menu that ends up
 * containing itself, which would otherwise recurse until the stack gives out. */
#define MAX_DEPTH 12

const char *settings_walk_menu_label(const struct menu_item_ex *menu)
{
    if (menu->flags & MENU_HAS_DESC)
        return P2STR(menu->callback_and_desc->desc);
    return "";
}

const char *settings_walk_item_label(const struct menu_item_ex *item,
                                     const struct settings_list *setting)
{
    if ((item->flags & MENU_TYPE_MASK) == MT_SETTING_W_TEXT)
        return P2STR(item->callback_and_desc->desc);
    return str(setting->lang_id);
}

static bool walk(const struct menu_item_ex *menu, const char *parent,
                 settings_walk_fn visit, void *ctx, int depth)
{
    int count = MENU_GET_COUNT(menu->flags);

    if (depth > MAX_DEPTH)
        return true;

    for (int i = 0; i < count; i++)
    {
        const struct menu_item_ex *item = menu->submenus[i];
        int type;

        if (!item)
            continue;

        type = item->flags & MENU_TYPE_MASK;

        if (type == MT_MENU)
        {
            /* Only MT_MENU carries submenus. A string list keeps its item
             * count in the same bits but holds strings in the union, so
             * recursing into one would walk whatever those pointers happen
             * to be. */
            if (!walk(item, settings_walk_menu_label(item), visit, ctx,
                      depth + 1))
                return false;
        }
        else if (type == MT_SETTING || type == MT_SETTING_W_TEXT)
        {
            const struct settings_list *setting = find_setting(item->variable);

            /* No name to show means no row worth offering. */
            if (!setting || setting->lang_id == -1)
                continue;

            if (!visit(item, setting, parent, ctx))
                return false;
        }
    }

    return true;
}

void settings_walk(settings_walk_fn visit, void *ctx)
{
    walk(&main_menu_, settings_walk_menu_label(&main_menu_), visit, ctx, 0);
}
