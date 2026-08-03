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
        display_menu,               /* display_menu.c   */
        playback_settings,          /* playback_menu.c  */
        sound_settings,             /* sound_menu.c     */
        settings_menu_item,         /* settings_menu.c  */
        bookmark_settings_menu,
        playlist_settings,          /* playlist_menu.c  */
        viewer_settings_menu,       /* playlist_menu.c  */
        equalizer_menu,             /* eq_menu.c        */
        theme_menu                  /* theme_menu.c     */
        , album_covers_menu         /* album_covers_menu.c */
        , art_cache_menu            /* album_covers_menu.c */
        , text_viewer_menu         /* text_viewer_menu.c */
        , lyric_viewer_menu;        /* lyric_viewer_settings.c */

struct browse_folder_info {
    const char* dir;
    int show_options;
};
int browse_folder(void *param); /* in theme_menu.c as it is mostly used there */
int main_menu_config(void); /* in main_menu_config.c */

#endif /*_EXPORTED_MENUS_H */
