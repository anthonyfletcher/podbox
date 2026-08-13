/***************************************************************************
 * Original code from RockBox
 * was: apps/menus/main_menu.c
 * Copyright (C) 2007 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * The main menu: system info, version, running time, credits, licences,
 * and the manage-settings submenu.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include "config.h"
#include "timefuncs.h" /* get_time, valid_time */
#include "string.h"
#include "lang.h"
#include "kernel.h"
#include "rbpaths.h"
#include "input/action.h"
#include "settings/settings.h"
#include "power.h"
#include "powermgmt.h"
#include "widgets/menu.h"
#include "system/app_util.h"
#include "screens/settings/exported_settings.h"
#include "screens/settings/settings_search.h"
#include "screens/settings/settings_changed.h"
#include "screens/browse/browser.h"
#include "storage.h"
#include "widgets/yesno.h"
#include "widgets/keyboard.h"
#include "screens/system/runtime_info.h"
#include "speech/talk.h"
#include "widgets/splash.h"
#include "screens/system/debug_menu.h"
#include "screens/system/bg_task_info.h"
#include "version.h"
#include "time.h"
#include "screens/playback/wps.h"
#include "skin_buffer.h"
#include "disk.h"
#include "viewers/credits.h"
#include "root_menu.h"
#include "viewers/text_viewer/text_viewer.h"
#include "dir.h"

static const struct browse_folder_info config = {ROCKBOX_DIR, SHOW_CFG};
static int show_info(void);
/** Manage settings menu **/

static int reset_settings(void)
{
    static const char *lines[]={ID2P(LANG_ARE_YOU_SURE), ID2P(LANG_RESET)};
    static const char *yes_lines[]={
        ID2P(LANG_SETTINGS),
        ID2P(LANG_RESET_DONE_CLEAR)
    };
    static const char *no_lines[]={
        ID2P(LANG_SETTINGS),
        ID2P(LANG_CANCEL)
    };
    static const struct text_message message={lines, 2};
    static const struct text_message yes_message={yes_lines, 2};
    static const struct text_message no_message={no_lines, 2};

    switch(gui_syncyesno_run(&message, &yes_message, &no_message))
    {
        case YESNO_YES:
            settings_reset();
            settings_save();
            settings_apply(true);
            settings_apply_skins();
            break;
        case YESNO_TMO:
        case YESNO_NO:
            break;
        case YESNO_USB:
            return 1;
    }
    return 0;
}
static int write_settings_file(void* param)
{
    return settings_save_config((intptr_t)param);
}

MENUITEM_FUNCTION_W_PARAM(browse_configs, 0, ID2P(LANG_CUSTOM_CFG),
                          browse_folder, (void*)&config, NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(save_settings_item, 0, ID2P(LANG_SAVE_SETTINGS),
                          write_settings_file, (void*)SETTINGS_SAVE_ALL,
                          NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(save_theme_item, 0, ID2P(LANG_SAVE_THEME),
                          write_settings_file, (void*)SETTINGS_SAVE_THEME,
                          NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(save_sound_item, 0, ID2P(LANG_SAVE_SOUND),
                          write_settings_file, (void*)SETTINGS_SAVE_SOUND,
                          NULL, Icon_NOICON);
MENUITEM_FUNCTION(reset_settings_item, 0, ID2P(LANG_RESET),
                  reset_settings, NULL, Icon_NOICON);

MENUITEM_SETTING(settings_mode_item, &global_settings.settings_mode, NULL);

MAKE_MENU(manage_settings, ID2P(LANG_MANAGE_MENU), NULL, Icon_Config,
          &settings_mode_item,
          &browse_configs, &reset_settings_item,
          &save_settings_item, &save_sound_item, &save_theme_item);

/** Info menu **/

static int show_credits(void)
{
    return credits_screen();
}

static int show_license(void)
{
    return text_viewer(ROCKBOX_DIR "/docs/COPYING.txt") == GO_TO_ROOT
           ? SYS_USB_CONNECTED : 0;
}

static int show_third_party_licenses(void)
{
    return text_viewer(ROCKBOX_DIR "/docs/LICENSES.txt") == GO_TO_ROOT
           ? SYS_USB_CONNECTED : 0;
}

#define SIZE_FMT "%s %s"
struct info_data
{
    sector_t size[NUM_VOLUMES];
    sector_t free[NUM_VOLUMES];
    unsigned long name[NUM_VOLUMES];
    bool new_data;
};

enum infoscreenorder
{
    INFO_BATTERY = 0,
    INFO_BUFFER,
    INFO_ROOT_DIR_TITLE,
    INFO_ROOT_DIR,
    INFO_VERSION_TITLE,
    INFO_VERSION,
    INFO_DATE,
    INFO_TIME,
    INFO_DISK1, /* capacity/free on internal */
    INFO_COUNT
};

/* NOTE:  This code is not quite right for
   (HAVE_MULTIVOLUME && !HAVE_MULTIDRIVE), but there are no
   in-tree matches so.. eh.
*/
static int refresh_data(struct info_data *info)
{
    int i = 0;
    int drive = 0;
    for (i = 0 ; i < NUM_VOLUMES ; i++)
    {
        volume_size(IF_MV(i,) &info->size[i], &info->free[i]);
        if (drive > 0)
            info->name[i] = LANG_DISK_NAME_MMC;
        else
            info->name[i] = LANG_DISK_NAME_INTERNAL;
        if (volume_partition(i) == -1)
        {
            info->name[i] = 0;
            break; /* ie stop when we run out of valid partitions */
        }
    }

    info->new_data = false;
    return i;
}

static int info_speak_item(int selected_item, void * data)
{
    struct info_data *info = (struct info_data*)data;

    struct tm *tm;
    int i;

    if (info->new_data)
        refresh_data(info);

    switch (selected_item)
    {
        case INFO_VERSION: /* version */
            talk_id(LANG_VERSION, false);
            talk_spell(rbversion, true);
        case INFO_VERSION_TITLE:  /*fallthrough*/
            break;

        case INFO_TIME:
            talk_id(LANG_CURRENT_TIME, false);
            /* fallthrough */
        case INFO_DATE:
            tm = get_time();

            if (!valid_time(tm))
            {
                talk_id(LANG_UNKNOWN, true);
                break;
            }
            if (selected_item == INFO_DATE)
                talk_date(tm, true);
            else
                talk_time(tm, true);

            break;

        case INFO_ROOT_DIR:
            talk_id(LANG_DISPLAY_FULL_PATH, false);
            talk_fullpath(root_realpath(), true);
        case INFO_ROOT_DIR_TITLE: /*fallthrough*/
            break;
        case INFO_BUFFER: /* buffer */
        {
            talk_id(LANG_BUFFER_STAT, false);
            long kib = audio_buffer_size() >> 10; /* to KiB */
            output_dyn_value(NULL, 0, kib, kibyte_units, 3, true);
            break;
        }
        case INFO_BATTERY: /* battery */
            /* Go by what power management reports */
            if (charging_state())
            {
                talk_id(LANG_BATTERY_CHARGE, true);
                if (battery_level() >= 0)
                    talk_value(battery_level(), UNIT_PERCENT, true);
            }
            else
            if (battery_level() >= 0)
            {
                int time_left = battery_time();
                talk_id(LANG_BATTERY_TIME, false);
                talk_value(battery_level(), UNIT_PERCENT, true);
                if (time_left >= 0)
                    talk_value(time_left * 60, UNIT_TIME, true);
            }
            else talk_id(VOICE_BLANK, false);
            break;
        case INFO_DISK1: /* disk 1 */
        default:
            i = selected_item - INFO_DISK1;
            if (info->size[i]) {
                talk_ids(false, info->name[i], LANG_DISK_FREE_INFO);
                output_dyn_value(NULL, 0, info->free[i], kibyte_units, 3, true);
                talk_id(LANG_DISK_SIZE_INFO, true);
                output_dyn_value(NULL, 0, info->size[i], kibyte_units, 3, true);
            } else if (info->name[i]) {
                talk_ids(true, info->name[i], LANG_NOT_PRESENT);
            }
            break;
    }
    return 0;
}

static int info_action_callback(int action, struct gui_synclist *lists)
{
    struct info_data *info = (struct info_data *)lists->data;
    if (info->new_data)
        refresh_data(info);

    char s1[32];
    char s2[32];
    struct tm *tm;
    if (action == ACTION_STD_CANCEL)
    {
        return action;
    }
    else if (action == ACTION_STD_OK
        )
    {
        action = ACTION_REDRAW;
        info->new_data = true;
        splash(0, ID2P(LANG_SCANNING_DISK));
        for (int i = 0; i < NUM_VOLUMES; i++)
            volume_recalc_free(IF_MV(i));
        gui_synclist_speak_item(lists);
    }
    else if (action == ACTION_NONE)
    {
        static int last_redraw = 0;
        if (TIME_AFTER(current_tick, last_redraw + HZ*5))
        {
            last_redraw = current_tick;
        }
        else
            return action;
    }
/* Note: these need to be in the same order as enum infoscreenorder */
    simplelist_reset_lines();
/* INFO_BATTERY */
    /* Go by what power management reports */
    if (charging_state())
        simplelist_setline(str(LANG_BATTERY_CHARGE));
    else
    if (battery_level() >= 0)
        simplelist_addline(str(LANG_BATTERY_TIME),
        battery_level(), battery_time() / 60, battery_time() % 60);
    else
        simplelist_setline("Battery n/a"); /* translating worth it? */

/* INFO_BUFFER */
    long kib = audio_buffer_size() >> 10; /* to KiB */
    output_dyn_value(s1, sizeof(s1), kib, kibyte_units, 3, true);
    simplelist_addline("%s %s", str(LANG_BUFFER_STAT), s1);


/* INFO_ROOT_DIR_TITLE */
    simplelist_setline(str(LANG_DISPLAY_FULL_PATH));
/* INFO_ROOT_DIR */
    simplelist_setline(root_realpath());
/* INFO_VERSION_TITLE */
    simplelist_setline(str(LANG_VERSION));
/* INFO_VERSION */
    simplelist_setline(rbversion);

    tm = get_time();
    if (valid_time(tm))
    {
/* INFO_DATE */
        simplelist_addline("%s %d %d",
            str(LANG_MONTH_JANUARY + tm->tm_mon),
            tm->tm_mday,
            tm->tm_year+1900);
/* INFO_TIME */
        simplelist_addline("%02d:%02d:%02d %s",
            global_settings.timeformat == 0 ? tm->tm_hour :
                 ((tm->tm_hour + 11) % 12) + 1,
                 tm->tm_min,
                 tm->tm_sec,
                 global_settings.timeformat == 0 ? "" :
                 tm->tm_hour>11 ? "P" : "A");
    }
    else
    {
/* INFO_DATE */
        simplelist_setline(str(LANG_UNKNOWN));
/* INFO_TIME */
        simplelist_setline("--:--:--");
    }
/* INFO_DISK, capacity/free on internal */
    for (int i = 0; i < NUM_VOLUMES ; i++) {
        if (info->size[i]) {
            output_dyn_value(s1, sizeof s1, info->free[i], kibyte_units, 3, true);
            output_dyn_value(s2, sizeof s2, info->size[i], kibyte_units, 3, true);
            simplelist_addline("%s %s/%s", str(info->name[i]), s1, s2);
        } else if (info->name[i]) {
            simplelist_addline("%s %s", str(info->name[i]),
                str(LANG_NOT_PRESENT));
        }
    }

    return action;
}

static int show_info(void)
{
    struct info_data data = {.new_data = true };
    struct simplelist_info info;
    refresh_data(&data);
    simplelist_info_init(&info, str(LANG_ROCKBOX_INFO), 0, (void*)&data);
    simplelist_setline(str(LANG_WAIT)); /* set a line so list is not empty on load */
    if(global_settings.talk_menu)
         info.get_talk = info_speak_item;
    info.action_callback = info_action_callback;
    return (simplelist_show_list(&info)) ? 1 : 0;
}


MENUITEM_FUNCTION(show_info_item, 0, ID2P(LANG_ROCKBOX_INFO),
                  show_info, NULL, Icon_NOICON);

int time_screen(void* ignored);
MENUITEM_FUNCTION(timedate_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_TIME_MENU),
                  time_screen,  NULL, Icon_Menu_setting );

MENUITEM_FUNCTION(show_credits_item, 0, ID2P(LANG_CREDITS),
                  show_credits, NULL, Icon_NOICON);

MENUITEM_FUNCTION(show_runtime_item, 0, ID2P(LANG_RUNNING_TIME),
                  view_runtime, NULL, Icon_NOICON);

/* Hide the Debug entry unless the "Show Debug Menu" setting is on (default off:
 * a menu item whose callback returns ACTION_EXIT_MENUITEM for the count request
 * is left out of the list -- see init_menu_lists() in menu.c). */
static int debug_menu_callback(int action,
                               const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && !global_settings.show_debug_menu)
        return ACTION_EXIT_MENUITEM;
    return action;
}

MENUITEM_FUNCTION(debug_menu_item, 0, ID2P(LANG_DEBUG),
                  debug_menu, debug_menu_callback, Icon_NOICON);

MENUITEM_FUNCTION(show_license_item, 0, ID2P(LANG_LICENSE),
                  show_license, NULL, Icon_NOICON);

MENUITEM_FUNCTION(show_third_party_licenses_item, 0,
                  ID2P(LANG_THIRD_PARTY_LICENSES),
                  show_third_party_licenses, NULL, Icon_NOICON);

/* The screen reports a USB attach the way simplelist does, as a bool; the menu
 * wants it as MENU_ATTACHED_USB so it unwinds rather than redrawing. */
static int bg_task_info_item(void)
{
    return bg_task_info_screen() ? MENU_ATTACHED_USB : 0;
}

MENUITEM_FUNCTION(bg_task_info, 0, ID2P(LANG_BG_TASK_INFO),
                  bg_task_info_item, NULL, Icon_NOICON);

MAKE_MENU(info_menu, ID2P(LANG_SYSTEM), 0, Icon_System_menu,
          &show_info_item, &show_credits_item,
          &show_runtime_item, &bg_task_info, &show_license_item,
          &show_third_party_licenses_item, &debug_menu_item);

MENUITEM_FUNCTION(main_menu_config_item, 0, ID2P(LANG_MAIN_MENU_SETTINGS),
                  main_menu_config, NULL, Icon_Menu_setting);

/** Main menu **/

/* At the top of the tree rather than buried in it: the whole point is to not
 * have to know where a setting lives. Walks this same menu, which is why it is
 * declared after everything it can reach. */
MENUITEM_FUNCTION(settings_search_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_DB_SEARCH), settings_search_run,
                  NULL, Icon_Menu_setting);

/* Reports USB the way the other simplelist screens here do, so the menu
 * unwinds rather than redrawing over the USB screen. */
static int settings_changed_item_fn(void)
{
    return settings_changed_screen() ? MENU_ATTACHED_USB : 0;
}

MENUITEM_FUNCTION(settings_changed_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_SETTINGS_CHANGED), settings_changed_item_fn,
                  NULL, Icon_Menu_setting);

/* Six branches, cut by what a person is trying to do rather than by which
 * subsystem implements it.
 *
 * General Settings is gone. It had fourteen unrelated branches under one word,
 * and it was where anything that was not obviously Sound or Theme ended up:
 * playlists, the database, the backlight, the USB mode, the language. Battery &
 * Power is the branch this re-cut exists for -- it is what an iPod owner tunes
 * most, and its settings were spread over four places with the word "battery"
 * appearing in none of them.
 *
 * Search and Changed Settings bracket the tree deliberately: one for when you
 * know what you want and not where it lives, the other for when you want to
 * know what you have already changed. */
MAKE_MENU(main_menu_, ID2P(LANG_SETTINGS), NULL,
        Icon_Submenu_Entered,
        &settings_search_item,
        &sound_settings,
        &playback_settings,
        &library_menu,
        &appearance_menu,
        &power_menu,
        &system_menu,
        &settings_changed_item,
        );
