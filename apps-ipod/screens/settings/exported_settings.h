/***************************************************************************
 * Original code from RockBox
 * was: apps/menus/exported_menus.h
 * Copyright (C) 2006 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * Declares the menu roots that other menus embed as submenus.
 ****************************************************************************/
#ifndef _EXPORTED_MENUS_H
#define _EXPORTED_MENUS_H

#include "widgets/menu.h"
/* not needed for plugins */

extern const struct menu_item_ex
        playback_settings,          /* playback_menu.c  */
        sound_settings,             /* sound_menu.c     */
        bookmark_settings_menu,
        playlist_settings,          /* playlist_menu.c  */
        viewer_settings_menu,       /* playlist_menu.c  */
        equalizer_menu,             /* eq_menu.c        */
        theme_menu                  /* theme_menu.c     */
        , album_covers_menu         /* album_covers_settings.c */
        , art_cache_menu            /* album_covers_settings.c */
        , text_viewer_menu          /* text_viewer_settings.c */
        , lyric_viewer_menu         /* lyric_viewer_settings.c */
        , wps_settings              /* general_settings.c -- shown under
                                       Playback, defined beside the browser and
                                       hotkey settings it shares a screen
                                       with */
        , wps_art_source            /* general_settings.c -- listed under both
                                       Playback > Now Playing Screen and
                                       Appearance > Elements */
        , scroll_settings_menu      /* display_settings.c -- shown under UI
                                       Settings, defined beside the other
                                       LCD scroll settings it configures */
        /* The six top-level branches, and the pieces they share.
           general_settings.c builds Library, Battery & Power and System;
           theme_settings.c builds Appearance. */
        , library_menu              /* general_settings.c */
        , power_menu                /* general_settings.c */
        , system_menu               /* general_settings.c */
        , appearance_menu           /* theme_settings.c   */
        , viewers_menu              /* general_settings.c -- listed under both
                                       Appearance and Library */
        , peak_meter_menu           /* display_settings.c -- under Appearance */
        /* display_settings.c -- the backlight rows, listed one by one under
           Battery & Power rather than behind an LCD Settings screen */
        , backlight_timeout
        , backlight_timeout_plugged
        , backlight_on_button_hold
        , caption_backlight
#if defined(HAVE_BACKLIGHT_FADING_INT_SETTING)
        , backlight_fade_in
        , backlight_fade_out
#endif
        , bl_filter_first_keypress
        , sel_backlight
        , lcd_sleep_after_backlight_off
        , brightness_item           /* display_settings.c -- listed under both
                                       Appearance and Battery & Power, as one
                                       item so there is only one callback */
        , codepage_setting          /* display_settings.c -- under System >
                                       Language & Text */
        , autoresume_menu           /* general_settings.c -- under Playback */
        , car_adapter_mode_menu;    /* general_settings.c -- under Battery */

struct browse_folder_info {
    const char* dir;
    int show_options;
};
int browse_folder(void *param); /* in theme_menu.c as it is mostly used there */
int main_menu_config(void); /* in main_menu_config.c */

#endif /*_EXPORTED_MENUS_H */
