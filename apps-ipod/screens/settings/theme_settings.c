/***************************************************************************
 * Original code from RockBox
 * was: apps/menus/theme_menu.c
 * Copyright (C) 2007 Jonathan Gordon
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Theme settings menu: skin selection, fonts, colours and backdrop.
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include "config.h"
#include "lang.h"
#include "kernel.h"
#include "input/action.h"
#include "settings/settings.h"
#include "rbpaths.h"
#include "widgets/menu.h"
#include "dir.h"
#include "screens/browse/browser.h"
#include "widgets/list.h"
#include "widgets/color_picker.h"
#include "lcd.h"
#include "skin/backdrop.h"
#include "exported_settings.h"
#include "system/appevents.h"
#include "draw/viewport.h"
#include "skin/statusbar_skinned.h"
#include "skin/skin_engine.h"
#include "widgets/splash.h"
#include "draw/icon_bitmaps.h"
#include "files/filetypes.h"

/**
* Menu to clear the backdrop image
 */
static int clear_main_backdrop(void)
{
    global_settings.backdrop_file[0] = '-';
    global_settings.backdrop_file[1] = '\0';
    skin_backdrop_load_setting();
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    viewportmanager_theme_undo(SCREEN_MAIN, true);
    settings_save();
    return 0;
}
MENUITEM_FUNCTION(clear_main_bd, 0, ID2P(LANG_CLEAR_BACKDROP),
                  clear_main_backdrop, NULL, Icon_NOICON);

enum Colors {
    COLOR_FG = 0,
    COLOR_BG,
    COLOR_LSS,
    COLOR_LSE,
    COLOR_LST,
    COLOR_SEP,
    /* The nine modal-dialog colours. Only used when Dialog Colours is On --
       Auto derives them and ignores what is set here. */
    COLOR_DLG_BOX_FG,
    COLOR_DLG_BOX_BG,
    COLOR_DLG_BOX_BORDER,
    COLOR_DLG_BTN_FG,
    COLOR_DLG_BTN_BG,
    COLOR_DLG_BTN_BORDER,
    COLOR_DLG_BTN_FG_SEL,
    COLOR_DLG_BTN_BG_SEL,
    COLOR_DLG_BTN_BORDER_SEL,
    COLOR_COUNT
};
static struct colour_info
{
    int *setting;
    int lang_id;
} colors[COLOR_COUNT] = {
    [COLOR_FG] = {&global_settings.fg_color, LANG_FOREGROUND_COLOR},
    [COLOR_BG] = {&global_settings.bg_color, LANG_BACKGROUND_COLOR},
    [COLOR_LSS] = {&global_settings.lss_color, LANG_SELECTOR_START_COLOR},
    [COLOR_LSE] = {&global_settings.lse_color, LANG_SELECTOR_END_COLOR},
    [COLOR_LST] = {&global_settings.lst_color, LANG_SELECTOR_TEXT_COLOR},
    [COLOR_SEP] = {&global_settings.list_separator_color, LANG_LIST_SEPARATOR_COLOR},
    [COLOR_DLG_BOX_FG] = {&global_settings.dialog_box_fg, LANG_DIALOG_BOX_FG},
    [COLOR_DLG_BOX_BG] = {&global_settings.dialog_box_bg, LANG_DIALOG_BOX_BG},
    [COLOR_DLG_BOX_BORDER] = {&global_settings.dialog_box_border,
                              LANG_DIALOG_BOX_BORDER},
    [COLOR_DLG_BTN_FG] = {&global_settings.dialog_btn_fg, LANG_DIALOG_BTN_FG},
    [COLOR_DLG_BTN_BG] = {&global_settings.dialog_btn_bg, LANG_DIALOG_BTN_BG},
    [COLOR_DLG_BTN_BORDER] = {&global_settings.dialog_btn_border,
                              LANG_DIALOG_BTN_BORDER},
    [COLOR_DLG_BTN_FG_SEL] = {&global_settings.dialog_btn_fg_sel,
                              LANG_DIALOG_BTN_FG_SEL},
    [COLOR_DLG_BTN_BG_SEL] = {&global_settings.dialog_btn_bg_sel,
                              LANG_DIALOG_BTN_BG_SEL},
    [COLOR_DLG_BTN_BORDER_SEL] = {&global_settings.dialog_btn_border_sel,
                                  LANG_DIALOG_BTN_BORDER_SEL},
};

/**
 * Menu for fore/back/selection colors
 */
static int set_color_func(void* color)
{
    int res, c = (intptr_t)color, banned_color=-1, old_color;
    /* Don't let foreground be set the same as background and vice-versa */
    if (c == COLOR_BG)
        banned_color = *colors[COLOR_FG].setting;
    else if (c == COLOR_FG || c == COLOR_SEP)
        banned_color = *colors[COLOR_BG].setting;

    old_color = *colors[c].setting;
    res = (int)set_color(&screens[SCREEN_MAIN],str(colors[c].lang_id),
                         colors[c].setting, banned_color);
    if (old_color != *colors[c].setting)
    {
        settings_save();
        settings_apply(false);
        settings_apply_skins();
    }
    return res;
}

static int reset_color(void)
{
    global_settings.fg_color = LCD_DEFAULT_FG;
    global_settings.bg_color = LCD_DEFAULT_BG;
    global_settings.lss_color = LCD_DEFAULT_LS;
    global_settings.lse_color = LCD_DEFAULT_BG;
    global_settings.lst_color = LCD_DEFAULT_FG;
    global_settings.list_separator_color = LCD_DARKGRAY;
    global_settings.colors_file[0] = '-';
    global_settings.colors_file[1] = '\0';

    read_color_theme_file();
    settings_save();
    settings_apply(false);
    settings_apply_skins();
    return 0;
}
MENUITEM_FUNCTION_W_PARAM(set_bg_col, 0, ID2P(LANG_BACKGROUND_COLOR),
                          set_color_func, (void*)COLOR_BG, NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(set_fg_col, 0, ID2P(LANG_FOREGROUND_COLOR),
                          set_color_func, (void*)COLOR_FG, NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(set_lss_col, 0, ID2P(LANG_SELECTOR_START_COLOR),
                          set_color_func, (void*)COLOR_LSS, NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(set_lse_col, 0, ID2P(LANG_SELECTOR_END_COLOR),
                          set_color_func, (void*)COLOR_LSE, NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(set_lst_col, 0, ID2P(LANG_SELECTOR_TEXT_COLOR),
                          set_color_func, (void*)COLOR_LST, NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(set_sep_col, 0, ID2P(LANG_LIST_SEPARATOR_COLOR),
                          set_color_func, (void*)COLOR_SEP, NULL, Icon_NOICON);
MENUITEM_FUNCTION(reset_colors, 0, ID2P(LANG_RESET_COLORS),
                  reset_color, NULL, Icon_NOICON);

MAKE_MENU(lss_settings, ID2P(LANG_SELECTOR_COLOR_MENU),
            NULL, Icon_NOICON,
            &set_lss_col, &set_lse_col, &set_lst_col
         );

/* now the actual menu */
MAKE_MENU(colors_settings, ID2P(LANG_COLORS_MENU),
            NULL, Icon_Display_menu,
            &lss_settings,  &set_sep_col,
            &set_bg_col, &set_fg_col, &reset_colors
         );

/* Modal dialog chrome. Every setting here is F_THEMERESET: loading a theme
   returns all of them to the shipped defaults, so a theme that says nothing
   about dialogs cannot inherit the last theme's. That applies to changes made
   here too -- they last until the next theme is loaded. */
MENUITEM_FUNCTION_W_PARAM(set_dlg_box_fg, 0, ID2P(LANG_DIALOG_BOX_FG),
                          set_color_func, (void*)COLOR_DLG_BOX_FG,
                          NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(set_dlg_box_bg, 0, ID2P(LANG_DIALOG_BOX_BG),
                          set_color_func, (void*)COLOR_DLG_BOX_BG,
                          NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(set_dlg_box_border, 0, ID2P(LANG_DIALOG_BOX_BORDER),
                          set_color_func, (void*)COLOR_DLG_BOX_BORDER,
                          NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(set_dlg_btn_fg, 0, ID2P(LANG_DIALOG_BTN_FG),
                          set_color_func, (void*)COLOR_DLG_BTN_FG,
                          NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(set_dlg_btn_bg, 0, ID2P(LANG_DIALOG_BTN_BG),
                          set_color_func, (void*)COLOR_DLG_BTN_BG,
                          NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(set_dlg_btn_border, 0, ID2P(LANG_DIALOG_BTN_BORDER),
                          set_color_func, (void*)COLOR_DLG_BTN_BORDER,
                          NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(set_dlg_btn_fg_sel, 0, ID2P(LANG_DIALOG_BTN_FG_SEL),
                          set_color_func, (void*)COLOR_DLG_BTN_FG_SEL,
                          NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(set_dlg_btn_bg_sel, 0, ID2P(LANG_DIALOG_BTN_BG_SEL),
                          set_color_func, (void*)COLOR_DLG_BTN_BG_SEL,
                          NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(set_dlg_btn_border_sel, 0,
                          ID2P(LANG_DIALOG_BTN_BORDER_SEL),
                          set_color_func, (void*)COLOR_DLG_BTN_BORDER_SEL,
                          NULL, Icon_NOICON);

MAKE_MENU(dialog_colors_menu, ID2P(LANG_COLORS_MENU), NULL, Icon_Display_menu,
            &set_dlg_box_fg, &set_dlg_box_bg, &set_dlg_box_border,
            &set_dlg_btn_fg, &set_dlg_btn_bg, &set_dlg_btn_border,
            &set_dlg_btn_fg_sel, &set_dlg_btn_bg_sel, &set_dlg_btn_border_sel);

MENUITEM_SETTING(dialog_colors, &global_settings.dialog_colors, NULL);
MENUITEM_SETTING(dialog_box_border_width,
                 &global_settings.dialog_box_border_width, NULL);
MENUITEM_SETTING(dialog_box_margin, &global_settings.dialog_box_margin, NULL);
MENUITEM_SETTING(dialog_btn_border_width,
                 &global_settings.dialog_btn_border_width, NULL);
MENUITEM_SETTING(dialog_btn_border_radius,
                 &global_settings.dialog_btn_border_radius, NULL);

MAKE_MENU(dialog_settings, ID2P(LANG_DIALOGS_MENU), NULL, Icon_Display_menu,
            &dialog_box_border_width,
            &dialog_box_margin,
            &dialog_btn_border_width,
            &dialog_btn_border_radius,
            &dialog_colors,
            &dialog_colors_menu);



/** Bars menu **/
/*                                  */

static int statusbar_callback_ex(int action,const struct menu_item_ex *this_item,
                                enum screen_type screen)
{
    (void)this_item;
    /* we save the old statusbar value here, so the old statusbars can get
     * removed and cleared from the display properly on exiting
     * (in gui_statusbar_changed() ) */
    static enum statusbar_values old_bar[NB_SCREENS];
    switch (action)
    {
        case ACTION_ENTER_MENUITEM:
            old_bar[screen] = statusbar_position(screen);
            break;
        case ACTION_EXIT_MENUITEM:
            if (old_bar[screen] != statusbar_position(screen))
                settings_apply_skins();
            break;
    }
    return ACTION_REDRAW;
}

static int statusbar_callback(int action,
                             const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    (void)this_list;
    return statusbar_callback_ex(action, this_item, SCREEN_MAIN);
}

MENUITEM_SETTING(scrollbar_item, &global_settings.scrollbar, NULL);
MENUITEM_SETTING(scrollbar_width, &global_settings.scrollbar_width, NULL);
MENUITEM_SETTING(statusbar, &global_settings.statusbar, statusbar_callback);
MENUITEM_SETTING(volume_type, &global_settings.volume_type, NULL);
MENUITEM_SETTING(battery_display, &global_settings.battery_display, NULL);
MAKE_MENU(bars_menu, ID2P(LANG_BARS_MENU), 0, Icon_NOICON,
          &scrollbar_item, &scrollbar_width, &statusbar,
          &volume_type
          , &battery_display
          );

/*                                  */

static struct browse_folder_info fonts = {FONT_DIR, SHOW_FONT};
static struct browse_folder_info sbs   = {SBS_DIR, SHOW_SBS};
static struct browse_folder_info wps = {WPS_DIR, SHOW_WPS};
static struct browse_folder_info themes = {THEME_DIR, SHOW_CFG};

int browse_folder(void *param)
{
    const char *ext, *setting;
    int lang_id = -1;
    char selected[MAX_FILENAME+10];
    const struct browse_folder_info *info =
        (const struct browse_folder_info*)param;

    struct browse_context browse = {
        .dirfilter = info->show_options,
        .icon = Icon_NOICON,
        .root = info->dir,
    };

    if (!dir_exists(info->dir)) {
        splash(HZ, ID2P(LANG_PLAYLIST_DIRECTORY_ACCESS_ERROR));
        return GO_TO_PREVIOUS;
    }

    /* if we are in a special settings folder, center the current setting */
    switch(info->show_options)
    {
        case SHOW_LNG:
            ext = "lng";
            if (global_settings.lang_file[0])
                setting = global_settings.lang_file;
            else
                setting = "english";
            lang_id = LANG_LANGUAGE;
            break;
        case SHOW_WPS:
            ext = "wps";
            setting = global_settings.wps_file;
            lang_id = LANG_WHILE_PLAYING;
            break;
        case SHOW_FONT:
            ext = "fnt";
            setting = global_settings.font_file;
            lang_id = LANG_CUSTOM_FONT;
            break;
        case SHOW_SBS:
            ext = "sbs";
            setting = global_settings.sbs_file;
            lang_id = LANG_BASE_SKIN;
            break;
        default:
            ext = setting = NULL;
            break;
    }

    /* If we've found a file to center on, do it */
    if (setting)
    {
        /* if setting != NULL, ext is initialized */
        snprintf(selected, sizeof(selected), "%s.%s", setting, ext);
        browse.selected = selected;
        browse.icon = Icon_Questionmark;
        browse.title = str(lang_id);
    }

    browser_get_context()->browse = NULL;  /*bugfix - force root dir reload */
    return rockbox_browse(&browse);
}

MENUITEM_FUNCTION_W_PARAM(browse_fonts, 0, ID2P(LANG_CUSTOM_FONT),
                          browse_folder, (void*)&fonts, NULL, Icon_Font);

MENUITEM_FUNCTION_W_PARAM(browse_sbs, 0, ID2P(LANG_BASE_SKIN),
                          browse_folder, (void*)&sbs, NULL, Icon_Wps);
MENUITEM_FUNCTION_W_PARAM(browse_wps, 0, ID2P(LANG_WHILE_PLAYING),
                          browse_folder, (void*)&wps, NULL, Icon_Wps);

static int showicons_callback(int action,
                             const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    static bool old_icons;
    switch (action)
    {
        case ACTION_ENTER_MENUITEM:
            old_icons = global_settings.show_icons;
            break;
        case ACTION_EXIT_MENUITEM:
            if (old_icons != global_settings.show_icons)
                icons_init();
            break;
    }
    return ACTION_REDRAW;
}

MENUITEM_SETTING(show_icons, &global_settings.show_icons, showicons_callback);
MENUITEM_FUNCTION_W_PARAM(browse_themes, 0, ID2P(LANG_CUSTOM_THEME),
                          browse_folder, (void*)&themes, NULL, Icon_Config);
MENUITEM_SETTING(cursor_style, &global_settings.cursor_style, NULL);
MENUITEM_SETTING(sep_menu, &global_settings.list_separator_height, NULL);
MENUITEM_SETTING(dynamic_colors, &global_settings.dynamic_colors, NULL);
MENUITEM_SETTING(wps_art_source, &global_settings.wps_art_source, NULL);

/* Art beside the rows in the database browser. Both come from the shared
 * thumbnail cache, so turning them off only stops them being drawn. They are
 * a property of the look rather than of the database, and a theme that wants
 * them says so in its .cfg. */
MENUITEM_SETTING(db_albumart, &global_settings.db_albumart, NULL);
MENUITEM_SETTING(db_artistart, &global_settings.db_artistart, NULL);

MENUITEM_SETTING(shortcuts_replaces_quickscreen,
                 &global_settings.shortcuts_replaces_qs, NULL);

/* The pieces a theme is built from. Loading a theme sets all of them at once,
   so these are for adjusting one afterwards. */
MAKE_MENU(theme_settings_menu, ID2P(LANG_THEME_SETTINGS_MENU), NULL, Icon_Wps,
            &browse_wps,
            &browse_sbs,
            &show_icons,
            &clear_main_bd,
            &bars_menu,
            &cursor_style,
            &sep_menu,
            &colors_settings,
            &dialog_settings,
            &db_albumart,
            &db_artistart);

MAKE_MENU(theme_menu, ID2P(LANG_THEME_MENU),
            NULL, Icon_Wps,
            &browse_themes,
            &browse_fonts,
            &theme_settings_menu,
            &scroll_settings_menu,
            &dynamic_colors,
            &wps_art_source,
            &shortcuts_replaces_quickscreen,
);
