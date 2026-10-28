/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Settings menu for the core text viewer (viewers/text_viewer). The same
 * menu is reached from Settings and, in the viewer, by holding Menu.
 ****************************************************************************/

#include <stdbool.h>
#include "config.h"
#include "lang.h"
#include "settings/settings.h"
#include "widgets/menu.h"
#include "screens/browse/browser.h"            /* browse_context, rockbox_browse */
#include "files/filetypes.h"       /* SHOW_FONT */
#include "rbpaths.h"         /* FONT_DIR */
#include "draw/icon_bitmaps.h"
#include "viewers/text_viewer/text_viewer.h"   /* contents, TV_COLOUR_* */
#include "widgets/color_picker.h"           /* set_color */

MENUITEM_SETTING(text_viewer_colour_mode,
                 &global_settings.text_viewer_colour_mode, NULL);
/* Custom's two colours, offered only while Custom is chosen. Neither may be
 * set to the other, or the page would be blank. */
static int custom_colour_callback(int action,
                                  const struct menu_item_ex *this_item,
                                  struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM
        && global_settings.text_viewer_colour_mode != TV_COLOUR_CUSTOM)
        return ACTION_EXIT_MENUITEM;
    return action;
}

static int set_custom_colour(void *which)
{
    bool text = which != NULL;
    int *colour = text ? &global_settings.text_viewer_fg
                       : &global_settings.text_viewer_bg;
    int other = text ? global_settings.text_viewer_bg
                     : global_settings.text_viewer_fg;
    int old = *colour;

    set_color(str(text ? LANG_TEXT_VIEWER_TEXT_COLOUR
                       : LANG_TEXT_VIEWER_PAGE_COLOUR), (unsigned *)colour,
              other);
    if (*colour != old)
        settings_save();
    return 0;
}
MENUITEM_FUNCTION_W_PARAM(text_viewer_text_colour, 0,
                          ID2P(LANG_TEXT_VIEWER_TEXT_COLOUR),
                          set_custom_colour, (void *)1,
                          custom_colour_callback, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(text_viewer_page_colour, 0,
                          ID2P(LANG_TEXT_VIEWER_PAGE_COLOUR),
                          set_custom_colour, NULL,
                          custom_colour_callback, Icon_NOICON);

MENUITEM_SETTING(text_viewer_brightness,
                 &global_settings.text_viewer_brightness, NULL);
MENUITEM_SETTING(text_viewer_backlight,
                 &global_settings.text_viewer_backlight, NULL);
MENUITEM_SETTING(text_viewer_margin,
                 &global_settings.text_viewer_margin, NULL);
MENUITEM_SETTING(text_viewer_line_spacing,
                 &global_settings.text_viewer_line_spacing, NULL);
MENUITEM_SETTING(text_viewer_footer,
                 &global_settings.text_viewer_footer, NULL);
MENUITEM_SETTING(text_viewer_read_aloud,
                 &global_settings.text_viewer_read_aloud, NULL);

/* Pick a .fnt from the fonts folder, storing just its name in the setting;
 * unlike the theme's own font item this does not touch the global UI font. */
static int text_viewer_font_pick(void)
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

    if (global_settings.text_viewer_font_file[0])
    {
        snprintf(name, sizeof name, "%s.fnt",
                 global_settings.text_viewer_font_file);
        browse.selected = name;
    }

    rockbox_browse(&browse);

    if (browse.flags & BROWSE_SELECTED)
        set_file(path, (char *)global_settings.text_viewer_font_file);
    return 0;
}
MENUITEM_FUNCTION(text_viewer_font_item, 0, ID2P(LANG_CUSTOM_FONT),
                  text_viewer_font_pick, NULL, Icon_Font);

/* Drop back to the theme's UI font. */
static int text_viewer_font_reset(void)
{
    global_settings.text_viewer_font_file[0] = '\0';
    settings_save();
    return 0;
}
MENUITEM_FUNCTION(text_viewer_font_reset_item, 0,
                  ID2P(LANG_TEXT_VIEWER_FONT_DEFAULT),
                  text_viewer_font_reset, NULL, Icon_Font);

/* Only inside the reader, with a book of chapters open: the same menu is
 * also reached from Settings */
static int contents_callback(int action, const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && !text_viewer_has_contents())
        return ACTION_EXIT_MENUITEM;
    return action;
}
MENUITEM_FUNCTION(text_viewer_contents_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_BROWSE_CHAPTERS), text_viewer_contents,
                  contents_callback, Icon_NOICON);

MAKE_MENU(text_viewer_menu, ID2P(LANG_TEXT_VIEWER), NULL, Icon_NOICON,
          &text_viewer_contents_item,
          &text_viewer_colour_mode,
          &text_viewer_text_colour,
          &text_viewer_page_colour,
          &text_viewer_brightness,
          &text_viewer_backlight,
          &text_viewer_margin,
          &text_viewer_line_spacing,
          &text_viewer_footer,
          &text_viewer_read_aloud,
          &text_viewer_font_item,
          &text_viewer_font_reset_item);
