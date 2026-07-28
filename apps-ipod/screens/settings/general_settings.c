/***************************************************************************
 * Original code from RockBox
 * was: apps/menus/settings_menu.c
 * Copyright (C) 2007 Jonathan Gordon
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * The general settings menu: filesystem, database, language, voice, hotkey
 * and startup options.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>
#include "config.h"
#include "lang.h"
#include "kernel.h"
#include "input/action.h"
#include "settings/settings.h"
#include "rbpaths.h"
#include "widgets/menu.h"
#include "widgets/keyboard.h"
#include "exported_settings.h"
#include "screens/browse/browser.h"
#include "screens/browse/browser_db.h"
#include "usb.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"
#include "speech/talk.h"
#include "powermgmt.h"
#include "audio/playback.h"
#include "screens/playback/quick_screen.h"
#include "dircache.h"
#include "widgets/folder_select.h"
#include "screens/context_menu.h"
#include "system/activity.h"
#include "system/format_time.h"
#include "system/volume.h"
#include "pathfuncs.h"


/** Tagcache menu **/

static void tagcache_rebuild_with_splash(void)
{
    tagcache_rebuild();
    splash(HZ*2, ID2P(LANG_TAGCACHE_FORCE_UPDATE_SPLASH));
}

static void tagcache_update_with_splash(void)
{
    tagcache_update();
    splash(HZ*2, ID2P(LANG_TAGCACHE_FORCE_UPDATE_SPLASH));
}

static int dirs_to_scan(void)
{
    if (folder_select(str(LANG_SELECT_FOLDER),
                      (char *)global_settings.tagcache_scan_paths,
                      sizeof(global_settings.tagcache_scan_paths)))
    {
        static const char *lines[] = {ID2P(LANG_TAGCACHE_BUSY),
                                      ID2P(LANG_TAGCACHE_FORCE_UPDATE)};
        static const struct text_message message = {lines, 2};

        if (gui_syncyesno_run(&message, NULL, NULL) == YESNO_YES)
            tagcache_rebuild_with_splash();
    }
    return 0;
}

MENUITEM_SETTING(tagcache_ram, &global_settings.tagcache_ram, NULL);
MENUITEM_SETTING(tagcache_scan_on_eject, &global_settings.tagcache_scan_on_eject, NULL);
MENUITEM_SETTING(tagcache_scan_on_startup,
                 &global_settings.tagcache_scan_on_startup, NULL);
MENUITEM_FUNCTION(tc_init, 0, ID2P(LANG_TAGCACHE_FORCE_UPDATE),
                  (int(*)(void))tagcache_rebuild_with_splash, NULL, Icon_NOICON);
MENUITEM_FUNCTION(tc_update, 0, ID2P(LANG_TAGCACHE_UPDATE),
                  (int(*)(void))tagcache_update_with_splash, NULL, Icon_NOICON);
MENUITEM_SETTING(runtimedb, &global_settings.runtimedb, NULL);

MENUITEM_FUNCTION(tc_export, 0, ID2P(LANG_TAGCACHE_EXPORT),
                  browser_db_export,
                  NULL, Icon_NOICON);

MENUITEM_FUNCTION(tc_import, 0, ID2P(LANG_TAGCACHE_IMPORT),
                  browser_db_import,
                  NULL, Icon_NOICON);
MENUITEM_FUNCTION(tc_paths, 0, ID2P(LANG_SELECT_DATABASE_DIRS),
                  dirs_to_scan, NULL, Icon_NOICON);

MENUITEM_SETTING(debug_log_tagcache, &global_settings.debug_log_tagcache, NULL);
MAKE_MENU(tagcache_menu, ID2P(LANG_TAGCACHE), 0, Icon_NOICON,
                &tagcache_ram,
                &tagcache_scan_on_eject, &tagcache_scan_on_startup,
                &tc_init, &tc_update, &runtimedb,
                &tc_export, &tc_import, &tc_paths, &debug_log_tagcache
                );

/** File view menu **/
MENUITEM_SETTING(sort_case, &global_settings.sort_case, NULL);
MENUITEM_SETTING(sort_dir, &global_settings.sort_dir, NULL);
MENUITEM_SETTING(sort_file, &global_settings.sort_file, NULL);
MENUITEM_SETTING(interpret_numbers, &global_settings.interpret_numbers, NULL);
MENUITEM_SETTING(dirfilter, &global_settings.dirfilter, NULL);
MENUITEM_SETTING(show_filename_ext, &global_settings.show_filename_ext, NULL);
MENUITEM_SETTING(browse_current, &global_settings.browse_current, NULL);
MENUITEM_SETTING(show_path_in_browser, &global_settings.show_path_in_browser, NULL);
MENUITEM_SETTING(hotkey_tree_item, &global_settings.hotkey_tree, NULL);
static int clear_start_directory(void)
{
    path_append(global_settings.start_directory, PATH_ROOTSTR,
                PA_SEP_HARD, sizeof(global_settings.start_directory));
    settings_save();
    splash(HZ, ID2P(LANG_RESET_DONE_CLEAR));
    return false;
}
MENUITEM_FUNCTION(clear_start_directory_item, 0, ID2P(LANG_RESET_START_DIR),
                  clear_start_directory, NULL, Icon_file_view_menu);

static int filemenu_callback(int action,
                             const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list);
MAKE_MENU(file_menu, ID2P(LANG_FILE), filemenu_callback, Icon_file_view_menu,
                &sort_case, &sort_dir, &sort_file, &interpret_numbers,
                &dirfilter, &show_filename_ext, &browse_current,
                &show_path_in_browser,
                &clear_start_directory_item
                ,&hotkey_tree_item
                );
static int filemenu_callback(int action,
                             const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    (void)this_list;

    /* Show File View menu in Settings or File Browser,
       but not in Database or Playlist Catalog */
    if (action == ACTION_REQUEST_MENUITEM &&
        this_item == &file_menu &&
        get_current_activity() != ACTIVITY_SETTINGS &&
        (context_menu_get_source() != CONTEXT_TREE
         || *browser_get_context()->dirfilter == SHOW_M3U))
        return ACTION_EXIT_MENUITEM;

    return action;
}



/** System menu **/

/* Battery */
#if BATTERY_CAPACITY_INC > 0
MENUITEM_SETTING(battery_capacity, &global_settings.battery_capacity, NULL);
#endif

static int usbcharging_callback(int action,
                                const struct menu_item_ex *this_item,
                                struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM: /* on exit */
            usb_charging_enable(global_settings.usb_charging);
            break;
    }
    return action;
}
MENUITEM_SETTING(usb_charging, &global_settings.usb_charging, usbcharging_callback);
MAKE_MENU(battery_menu, ID2P(LANG_BATTERY_MENU), 0, Icon_NOICON,
#if BATTERY_CAPACITY_INC > 0
            &battery_capacity,
#endif
            &usb_charging,
         );
MENUITEM_SETTING(usb_mode, &global_settings.usb_mode, NULL);
/* Disk */
MENUITEM_SETTING(disk_spindown, &global_settings.disk_spindown, NULL);
MENUITEM_SETTING(storage_mode, &global_settings.storage_mode, NULL);
static int dircache_callback(int action,
                             const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM: /* on exit */
            if (global_settings.dircache)
            {
                if (dircache_enable() < 0)
                    splash(HZ*2, ID2P(LANG_PLEASE_REBOOT));
            }
            else
            {
                dircache_disable();
            }
            break;
    }
    return action;
}
MENUITEM_SETTING(dircache, &global_settings.dircache, dircache_callback);
MAKE_MENU(disk_menu, ID2P(LANG_DISK_MENU), 0, Icon_NOICON,
          &disk_spindown,
          &storage_mode,
            &dircache,
         );

/* Limits menu */
MENUITEM_SETTING(max_files_in_dir, &global_settings.max_files_in_dir, NULL);
MENUITEM_SETTING(max_files_in_playlist, &global_settings.max_files_in_playlist, NULL);
MENUITEM_SETTING(default_glyphs, &global_settings.glyphs_to_cache, NULL);
MAKE_MENU(limits_menu, ID2P(LANG_LIMITS_MENU), 0, Icon_NOICON,
           &max_files_in_dir, &max_files_in_playlist
           ,&default_glyphs
           );

/* Volume adjustment */
MENUITEM_SETTING(volume_adjust_mode, &global_settings.volume_adjust_mode, NULL);
MENUITEM_SETTING(volume_adjust_norm_steps, &global_settings.volume_adjust_norm_steps, NULL);

/* Keyclick menu */
MENUITEM_SETTING(keyclick, &global_settings.keyclick, NULL);
MENUITEM_SETTING(keyclick_repeats, &global_settings.keyclick_repeats, NULL);
MENUITEM_SETTING(keyclick_hardware, &global_settings.keyclick_hardware, NULL);
MAKE_MENU(keyclick_menu, ID2P(LANG_KEYCLICK), 0, Icon_NOICON,
           &keyclick, &keyclick_hardware, &keyclick_repeats);

MENUITEM_SETTING(car_adapter_mode, &global_settings.car_adapter_mode, NULL);
MENUITEM_SETTING(car_adapter_mode_delay, &global_settings.car_adapter_mode_delay, NULL);
MAKE_MENU(car_adapter_mode_menu, ID2P(LANG_CAR_ADAPTER_MODE), 0, Icon_NOICON,
           &car_adapter_mode, &car_adapter_mode_delay);
MENUITEM_SETTING(serial_bitrate, &global_settings.serial_bitrate, NULL);
MENUITEM_SETTING(accessory_supply, &global_settings.accessory_supply, NULL);
MENUITEM_SETTING(lineout_onoff, &global_settings.lineout_active, NULL);
MENUITEM_SETTING(usb_hid, &global_settings.usb_hid, NULL);
MENUITEM_SETTING(usb_keypad_mode, &global_settings.usb_keypad_mode, NULL);
#ifdef USB_ENABLE_AUDIO
MENUITEM_SETTING(usb_audio, &global_settings.usb_audio, NULL);
#endif


MENUITEM_SETTING(shortcuts_replaces_quickscreen, &global_settings.shortcuts_replaces_qs, NULL);


MENUITEM_SETTING(wps_select_action, &global_settings.wps_select_action, NULL);

MENUITEM_SETTING(show_debug_menu, &global_settings.show_debug_menu, NULL);


MAKE_MENU(system_menu, ID2P(LANG_SYSTEM),
          0, Icon_System_menu,
            &battery_menu,
            &disk_menu,
            &limits_menu,
            &volume_adjust_mode,
            &volume_adjust_norm_steps,
            &shortcuts_replaces_quickscreen,
            &car_adapter_mode_menu,
            &serial_bitrate,
            &accessory_supply,
            &lineout_onoff,
            &keyclick_menu,
            &usb_hid,
            &usb_keypad_mode,
#ifdef USB_ENABLE_AUDIO
            &usb_audio,
#endif

            &usb_mode,
            &wps_select_action,
            &show_debug_menu,
         );


/** Startup/shutdown menu **/


char* sleeptimer_getname(int selected_item, void * data,
                         char *buffer, size_t buffer_len)
{
    (void)selected_item;
    (void)data;
    return string_sleeptimer(buffer, buffer_len);
}

int sleeptimer_voice(int selected_item, void*data)
{
    (void)selected_item;
    (void)data;
    talk_sleeptimer(-1);
    return 0;
}

/* Handle restarting a current sleep timer to the newly set default
   duration */
static int sleeptimer_duration_cb(int action,
                                  const struct menu_item_ex *this_item,
                                  struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    static int initial_duration;
    switch (action)
    {
        case ACTION_ENTER_MENUITEM:
            initial_duration = global_settings.sleeptimer_duration;
            break;
        case ACTION_EXIT_MENUITEM:
            if (initial_duration != global_settings.sleeptimer_duration
                    && get_sleep_timer())
                set_sleeptimer_duration(global_settings.sleeptimer_duration);
    }
    return action;
}

MENUITEM_SETTING(start_screen, &global_settings.start_in_screen, NULL);
MENUITEM_SETTING(poweroff, &global_settings.poweroff, NULL);
MENUITEM_FUNCTION_DYNTEXT(sleeptimer_toggle, 0, toggle_sleeptimer,
                          sleeptimer_getname, sleeptimer_voice, NULL,
                          NULL, Icon_NOICON);
MENUITEM_SETTING(sleeptimer_duration,
                 &global_settings.sleeptimer_duration,
                 sleeptimer_duration_cb);
MENUITEM_SETTING(sleeptimer_on_startup,
                 &global_settings.sleeptimer_on_startup, NULL);
MENUITEM_SETTING(keypress_restarts_sleeptimer,
                 &global_settings.keypress_restarts_sleeptimer, NULL);
MENUITEM_SETTING(show_shutdown_message, &global_settings.show_shutdown_message, NULL);

#define SETTINGS_CLEAR_ON_HOLD
MENUITEM_SETTING(clear_settings_on_hold,
                 &global_settings.clear_settings_on_hold, NULL);

MAKE_MENU(startup_shutdown_menu, ID2P(LANG_STARTUP_SHUTDOWN),
          0, Icon_System_menu,
            &show_shutdown_message,
            &start_screen,
            &poweroff,
            &sleeptimer_toggle,
            &sleeptimer_duration,
            &sleeptimer_on_startup,
            &keypress_restarts_sleeptimer,
#if defined(SETTINGS_CLEAR_ON_HOLD)
            &clear_settings_on_hold,
#undef SETTINGS_CLEAR_ON_HOLD
#endif
         );


/** Bookmark menu **/
static int bmark_callback(int action,
                          const struct menu_item_ex *this_item,
                          struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM: /* on exit */
            if(global_settings.autocreatebookmark ==  BOOKMARK_RECENT_ONLY_YES ||
               global_settings.autocreatebookmark ==  BOOKMARK_RECENT_ONLY_ASK)
            {
                if(global_settings.usemrb == BOOKMARK_NO)
                    global_settings.usemrb = BOOKMARK_YES;

            }
            break;
    }
    return action;
}
MENUITEM_SETTING(autocreatebookmark,
                 &global_settings.autocreatebookmark, bmark_callback);
MENUITEM_SETTING(autoupdatebookmark, &global_settings.autoupdatebookmark, NULL);
MENUITEM_SETTING(autoloadbookmark, &global_settings.autoloadbookmark, NULL);
MENUITEM_SETTING(usemrb, &global_settings.usemrb, NULL);
MAKE_MENU(bookmark_settings_menu, ID2P(LANG_BOOKMARK_SETTINGS), 0,
          Icon_Bookmark,
          &autocreatebookmark, &autoupdatebookmark, &autoloadbookmark, &usemrb);

/** Autoresume menu **/

static int autoresume_callback(int action,
                               const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;

    if (action == ACTION_EXIT_MENUITEM  /* on exit */
        && global_settings.autoresume_enable
        && !tagcache_is_usable())
    {
        static const char *lines[] = {ID2P(LANG_TAGCACHE_BUSY),
                                      ID2P(LANG_TAGCACHE_FORCE_UPDATE)};
        static const struct text_message message = {lines, 2};

        if (gui_syncyesno_run(&message, NULL, NULL) == YESNO_YES)
            tagcache_rebuild_with_splash();
    }
    return action;
}

static int autoresume_nexttrack_callback(int action,
                                         const struct menu_item_ex *this_item,
                                         struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    static int oldval = 0;
    switch (action)
    {
        case ACTION_ENTER_MENUITEM:
            oldval = global_settings.autoresume_automatic;
            break;
        case ACTION_EXIT_MENUITEM:
            if (global_settings.autoresume_automatic == AUTORESUME_NEXTTRACK_CUSTOM
                && !folder_select(str(LANG_AUTORESUME),
                                  (char *)global_settings.autoresume_paths,
                                  sizeof(global_settings.autoresume_paths)))
            {
                global_settings.autoresume_automatic = oldval;
            }
    }
    return action;
}

MENUITEM_SETTING(autoresume_enable, &global_settings.autoresume_enable,
                 autoresume_callback);
MENUITEM_SETTING(autoresume_automatic, &global_settings.autoresume_automatic,
                 autoresume_nexttrack_callback);

MAKE_MENU(autoresume_menu, ID2P(LANG_AUTORESUME),
          0, Icon_NOICON,
          &autoresume_enable, &autoresume_automatic);


/** Voice menu **/
static int talk_callback(int action,
                         const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list);

MENUITEM_SETTING(talk_menu_item, &global_settings.talk_menu, NULL);
MENUITEM_SETTING(talk_dir_item, &global_settings.talk_dir, NULL);
MENUITEM_SETTING(talk_dir_clip_item, &global_settings.talk_dir_clip, talk_callback);
MENUITEM_SETTING(talk_file_item, &global_settings.talk_file, NULL);
MENUITEM_SETTING(talk_file_clip_item, &global_settings.talk_file_clip, talk_callback);
static int talk_callback(int action,
                         const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list)
{
    (void)this_list;
    static int oldval = 0;
    switch (action)
    {
        case ACTION_ENTER_MENUITEM:
            oldval = global_settings.talk_file_clip;
            break;
        case ACTION_EXIT_MENUITEM:
            audio_set_crossfade(global_settings.crossfade);
            if (this_item == &talk_dir_clip_item)
                break;
            if (!oldval && global_settings.talk_file_clip)
            {
                /* force reload if newly talking thumbnails,
                because the clip presence is cached only if enabled */
                reload_directory();
            }
            break;
    }
    return action;
}
MENUITEM_SETTING(talk_filetype_item, &global_settings.talk_filetype, NULL);
MENUITEM_SETTING(talk_battery_level_item,
                 &global_settings.talk_battery_level, NULL);
MENUITEM_SETTING(talk_mixer_amp_item, &global_settings.talk_mixer_amp, NULL);
MAKE_MENU(voice_settings_menu, ID2P(LANG_VOICE), 0, Icon_Voice,
          &talk_menu_item, &talk_dir_item, &talk_dir_clip_item,
          &talk_file_item, &talk_file_clip_item, &talk_filetype_item,
          &talk_battery_level_item, &talk_mixer_amp_item);

/** WPS settings menu **/

MENUITEM_SETTING(browser_default,
                 &global_settings.browser_default, NULL);

MENUITEM_SETTING(hotkey_wps_item, &global_settings.hotkey_wps, NULL);

MAKE_MENU(wps_settings, ID2P(LANG_WPS), 0, Icon_Playback_menu
            ,&browser_default
            ,&hotkey_wps_item
            );



/** Settings menu **/

static struct browse_folder_info langs = { LANG_DIR, SHOW_LNG };

MENUITEM_FUNCTION_W_PARAM(browse_langs, 0, ID2P(LANG_LANGUAGE),
                          browse_folder, (void*)&langs, NULL, Icon_Language);

MAKE_MENU(settings_menu_item, ID2P(LANG_GENERAL_SETTINGS), 0,
          Icon_General_settings_menu,
          &wps_settings,
          &playlist_settings, &file_menu,
          &tagcache_menu,
          &album_covers_menu,
          &art_cache_menu,
          &text_viewer_menu,
          &display_menu, &system_menu,
          &startup_shutdown_menu,
          &bookmark_settings_menu,
          &autoresume_menu,
          &browse_langs, &voice_settings_menu,
          );
