/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to settings_walk.c: visiting every setting that has a row.
 ****************************************************************************/

/* Walk the settings menu tree and hand back every setting row in it.
 *
 * Walking the tree rather than settings[] is what makes a screen built on this
 * behave like the menus do, and it decides three things at once:
 *
 *   - settings that exist only in a .cfg file (the quickscreen slots, the
 *     start directory) are never visited, so nothing has to filter them out;
 *   - the menu a row sits in comes with it, which is what tells two settings
 *     of the same name apart;
 *   - only settings declared MENUITEM_SETTING are visited, which is exactly
 *     the set do_setting_screen() can open -- the colours are MENUITEM_FUNCTION
 *     rows running the colour picker, and are correctly skipped.
 *
 * The `item` handed to the visitor is what a caller needs to *open* the
 * setting: pass it to do_setting_from_menu_standalone(), not the settings_list,
 * or a setting whose effect lives in its menu callback will change value and do
 * nothing.
 *
 * Cross-listing means one setting can sit in two menus; the walk visits each
 * row it finds, so a caller that does not want duplicates dedupes on
 * `item->variable`.
 */

#ifndef _SETTINGS_WALK_H_
#define _SETTINGS_WALK_H_

#include <stdbool.h>
#include "settings/settings.h"
#include "widgets/menu.h"

/* Return false to stop the walk. */
typedef bool (*settings_walk_fn)(const struct menu_item_ex *item,
                                 const struct settings_list *setting,
                                 const char *parent, void *ctx);

void settings_walk(settings_walk_fn visit, void *ctx);

/* The label a menu or a setting row shows, which for a setting given its own
 * wording in a menu is not the setting's own name. */
const char *settings_walk_menu_label(const struct menu_item_ex *menu);
const char *settings_walk_item_label(const struct menu_item_ex *item,
                                     const struct settings_list *setting);

#endif /* _SETTINGS_WALK_H_ */
