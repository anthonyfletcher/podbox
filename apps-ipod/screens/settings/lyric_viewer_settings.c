/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Settings menu for the synchronised lyrics viewer
 * (apps-ipod/viewers/lyric_viewer). The same menu is reached from Settings
 * and, in the viewer, by holding Menu.
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include "config.h"
#include "lang.h"
#include "settings/settings.h"
#include "widgets/menu.h"
#include "screens/browse/browser.h"            /* browse_context, rockbox_browse */
#include "files/filetypes.h"       /* SHOW_FONT */
#include "rbpaths.h"         /* FONT_DIR */
#include "draw/icon_bitmaps.h"

MENUITEM_SETTING(lyric_colour_mode,
                 &global_settings.lyric_colour_mode, NULL);
MENUITEM_SETTING(lyric_line_spacing,
                 &global_settings.lyric_line_spacing, NULL);
MENUITEM_SETTING(lyric_align,
                 &global_settings.lyric_align, NULL);
MENUITEM_SETTING(lyric_prev_opacity,
                 &global_settings.lyric_prev_opacity, NULL);
MENUITEM_SETTING(lyric_next_opacity,
                 &global_settings.lyric_next_opacity, NULL);
MENUITEM_SETTING(lyric_anim,
                 &global_settings.lyric_anim, NULL);
MENUITEM_SETTING(lyric_highlight,
                 &global_settings.lyric_highlight, NULL);
MENUITEM_SETTING(lyric_backlight,
                 &global_settings.lyric_backlight, NULL);

/* Pick a .fnt from the fonts folder, storing just its name in the setting;
 * like the text viewer's, this does not touch the global UI font. */
static int lyric_font_pick(void)
{
    char path[MAX_PATH], name[MAX_FILENAME + 10];
    struct browse_context browse = {
        .dirfilter = SHOW_FONT,
        .flags = BROWSE_SELECTONLY | BROWSE_NO_CONTEXT_MENU,
        .title = ID2P(LANG_CUSTOM_FONT),
        .icon = Icon_Font,
        .root = FONT_DIR,
        .selected = NULL,
        .buf = path,
        .bufsize = sizeof path,
    };

    if (global_settings.lyric_font_file[0])
    {
        snprintf(name, sizeof name, "%s.fnt",
                 global_settings.lyric_font_file);
        browse.selected = name;
    }

    rockbox_browse(&browse);

    if (browse.flags & BROWSE_SELECTED)
        set_file(path, (char *)global_settings.lyric_font_file);
    return 0;
}
MENUITEM_FUNCTION(lyric_font_item, 0, ID2P(LANG_CUSTOM_FONT),
                  lyric_font_pick, NULL, Icon_Font);

/* Drop back to the theme's UI font. */
static int lyric_font_reset(void)
{
    global_settings.lyric_font_file[0] = '\0';
    settings_save();
    return 0;
}
MENUITEM_FUNCTION(lyric_font_reset_item, 0, ID2P(LANG_LYRICS_FONT_DEFAULT),
                  lyric_font_reset, NULL, Icon_Font);

MAKE_MENU(lyric_viewer_menu, ID2P(LANG_LYRICS_MENU), NULL, Icon_Menu_setting,
          &lyric_colour_mode,
          &lyric_align,
          &lyric_line_spacing,
          &lyric_prev_opacity,
          &lyric_next_opacity,
          &lyric_anim,
          &lyric_highlight,
          &lyric_backlight,
          &lyric_font_item,
          &lyric_font_reset_item);
