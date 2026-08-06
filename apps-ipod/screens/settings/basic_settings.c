/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The basic settings page: one short flat list of the settings most people
 * want, in place of the full tree. Which of the two the root Settings entry
 * opens is global_settings.settings_mode; see root_menu.c.
 *
 * Every entry here is a second menu item pointing at the *same* variable as
 * the full tree, never a copy of the value -- so a setting changed on this
 * page reads back correctly under Advanced, and vice versa. An entry that
 * carries a menu callback in the full tree must carry the same one here, or
 * the two behave differently; Enable EQ is the only such entry so far.
 ****************************************************************************/

#include <stdbool.h>
#include "config.h"
#include "lang.h"
#include "settings/settings.h"
#include "widgets/menu.h"
#include "draw/icon.h"
#include "eq_settings.h"
#include "exported_settings.h"
#include "root_menu.h"

/* Function items already declared elsewhere. MENUITEM_FUNCTION* and MAKE_MENU
   make their item non-static, so these are the very same items the full tree
   shows rather than second copies. */
extern const struct menu_item_ex
        eq_browse,              /* eq_settings.c    */
        browse_themes,          /* theme_settings.c */
        browse_fonts,           /* theme_settings.c */
        main_menu_config_item,  /* main_menu.c      */
        main_menu_;             /* main_menu.c -- the full settings tree */

MENUITEM_SETTING(basic_volume, &global_status.volume, NULL);
MENUITEM_SETTING(basic_bass, &global_settings.bass, NULL);
MENUITEM_SETTING(basic_treble, &global_settings.treble, NULL);
MENUITEM_SETTING(basic_eq_enable, &global_settings.eq_enabled,
                 eq_setting_callback);
MENUITEM_SETTING(basic_shuffle, &global_settings.playlist_shuffle, NULL);
MENUITEM_SETTING(basic_repeat, &global_settings.repeat_mode, NULL);
MENUITEM_SETTING(basic_backlight, &global_settings.backlight_timeout, NULL);
MENUITEM_SETTING(basic_brightness, &global_settings.brightness, NULL);
MENUITEM_SETTING(basic_poweroff, &global_settings.poweroff, NULL);

/* The escape hatch: everything not on this page is one entry away. Backing out
   of the full tree returns here, so only a request to leave the settings
   altogether (the Menu button, or a jump to the WPS) is passed up. */
static int enter_advanced(void)
{
    int ret = do_menu(&main_menu_, NULL, NULL, false);
    return ret == GO_TO_PREVIOUS ? 0 : ret;
}

MENUITEM_FUNCTION(basic_advanced_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_SETTINGS_MODE_ADVANCED), enter_advanced,
                  NULL, Icon_Submenu);

MAKE_MENU(basic_settings_menu, ID2P(LANG_SETTINGS), NULL,
          Icon_Submenu_Entered,
          &basic_volume,
          &basic_bass,
          &basic_treble,
          &basic_shuffle,
          &basic_repeat,
          &basic_brightness,
          &basic_backlight,
          &eq_browse,
          &basic_eq_enable,
          &browse_themes,
          &browse_fonts,
          &basic_poweroff,
          &main_menu_config_item,
          &basic_advanced_item);
