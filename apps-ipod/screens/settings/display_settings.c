/***************************************************************************
 * Original code from RockBox
 * was: apps/menus/display_menu.c
 * Copyright (C) 2007 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * Display settings menu: backlight, LCD, scrolling, status bar and related
 * options.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include "config.h"
#include "system/appevents.h"
#include "lang.h"
#include "input/action.h"
#include "settings/settings.h"
#include "widgets/menu.h"
#include "screens/browse/browser.h"
#include "widgets/list.h"
#include "audio/peak_meter.h"
#include "speech/talk.h"
#include "lcd.h"
#include "widgets/mask_select.h"
#include "widgets/splash.h"
#include "draw/viewport.h"
#include "skin/statusbar.h" /* statusbar_vals enum*/
#include "rbunicode.h"

static int selectivebacklight_callback(int action,
                                       const struct menu_item_ex *this_item,
                                       struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM:
        case ACTION_STD_MENU:
        case ACTION_STD_CANCEL:
            set_selective_backlight_actions(
                                       global_settings.bl_selective_actions,
                                       global_settings.bl_selective_actions_mask,
                                       global_settings.bl_filter_first_keypress);
            break;
    }

    return action;
}

static int filterfirstkeypress_callback(int action,
                                        const struct menu_item_ex *this_item,
                                        struct gui_synclist *this_list)
{
    /*(void)this_item;REMOVED*/
    switch (action)
    {
        case ACTION_EXIT_MENUITEM:
            set_backlight_filter_keypress(global_settings.bl_filter_first_keypress);
            selectivebacklight_callback(action,this_item, this_list);/*uses Filter First KP*/
            break;
    }

    return action;
}

static int selectivebacklight_set_mask(void* param)
{
    (void)param;
     int mask = global_settings.bl_selective_actions_mask;
            struct s_mask_items maskitems[]={
                                       {ID2P(LANG_ACTION_VOLUME)   , SEL_ACTION_VOL},
                                       {ID2P(LANG_ACTION_PLAY), SEL_ACTION_PLAY},
                                       {ID2P(LANG_ACTION_SEEK), SEL_ACTION_SEEK},
                                       {ID2P(LANG_ACTION_SKIP), SEL_ACTION_SKIP},
                     {ID2P(LANG_ACTION_DISABLE_UNMAPPED), SEL_ACTION_NOUNMAPPED}
                        ,{ID2P(LANG_ACTION_DISABLE_EXT_POWER), SEL_ACTION_NOEXT}
                                            };

            mask = mask_select(mask, ID2P(LANG_BACKLIGHT_SELECTIVE)
                                   , maskitems, ARRAYLEN(maskitems));

            if (mask == SEL_ACTION_NONE ||  mask == SEL_ACTION_NOEXT)
                global_settings.bl_selective_actions = false;
            else if (global_settings.bl_selective_actions_mask != mask)
                global_settings.bl_selective_actions = true;

            global_settings.bl_selective_actions_mask = mask;

    return true;
}


/** LCD menu **/
MENUITEM_SETTING(backlight_timeout, &global_settings.backlight_timeout, NULL);
MENUITEM_SETTING(backlight_timeout_plugged,
                &global_settings.backlight_timeout_plugged, NULL);

MENUITEM_SETTING(backlight_on_button_hold,
                &global_settings.backlight_on_button_hold, NULL);

MENUITEM_SETTING(caption_backlight, &global_settings.caption_backlight, NULL);
#if defined(HAVE_BACKLIGHT_FADING_INT_SETTING)
MENUITEM_SETTING(backlight_fade_in, &global_settings.backlight_fade_in, NULL);
MENUITEM_SETTING(backlight_fade_out, &global_settings.backlight_fade_out, NULL);
#endif
MENUITEM_SETTING(bl_filter_first_keypress,
                    &global_settings.bl_filter_first_keypress,
                    filterfirstkeypress_callback);

MENUITEM_SETTING(bl_selective_actions,
                 &global_settings.bl_selective_actions,
                                                    selectivebacklight_callback);

MENUITEM_FUNCTION(sel_backlight_mask, 0, ID2P(LANG_SETTINGS),
                  selectivebacklight_set_mask, selectivebacklight_callback,
                  Icon_Menu_setting);

MAKE_MENU(sel_backlight, ID2P(LANG_BACKLIGHT_SELECTIVE),
          NULL, Icon_Menu_setting, &bl_selective_actions, &sel_backlight_mask);

MENUITEM_SETTING(lcd_sleep_after_backlight_off,
                &global_settings.lcd_sleep_after_backlight_off, NULL);
/* Listed under both Appearance and Battery & Power: it is how bright the
   screen looks and the second biggest draw on the battery, and there is no
   sense in which it is one and not the other. */
MENUITEM_SETTING_EXPORTED(brightness_item, &global_settings.brightness, NULL);
/* now the actual menu */
MAKE_MENU(lcd_settings,ID2P(LANG_LCD_MENU),
            NULL, Icon_Display_menu
            ,&backlight_timeout
            ,&backlight_timeout_plugged
            ,&backlight_on_button_hold
            ,&caption_backlight
#if defined(HAVE_BACKLIGHT_FADING_INT_SETTING)
            ,&backlight_fade_in, &backlight_fade_out
#endif
            ,&bl_filter_first_keypress
            ,&sel_backlight
            ,&lcd_sleep_after_backlight_off
            /* Brightness is not here. This screen is the backlight -- when it
               is on and for how long -- and brightness is a property of the
               panel. It sits one level up, beside the backlight, under
               Battery & Power. */
         );



/** Scroll menu **/
MENUITEM_SETTING_W_TEXT(scroll_speed, &global_settings.scroll_speed,
                         ID2P(LANG_SCROLL), NULL);
MENUITEM_SETTING(scroll_delay, &global_settings.scroll_delay, NULL);
MENUITEM_SETTING_W_TEXT(scroll_step, &global_settings.scroll_step,
                        ID2P(LANG_SCROLL_STEP_EXAMPLE), NULL);
MENUITEM_SETTING(bidir_limit, &global_settings.bidir_limit, NULL);

/* list acceleration */
MENUITEM_SETTING(offset_out_of_view, &global_settings.offset_out_of_view, NULL);
MENUITEM_SETTING(disable_mainmenu_scrolling, &global_settings.disable_mainmenu_scrolling, NULL);
MENUITEM_SETTING(screen_scroll_step, &global_settings.screen_scroll_step, NULL);
MENUITEM_SETTING(scroll_paginated, &global_settings.scroll_paginated, NULL);
MENUITEM_SETTING(list_wraparound, &global_settings.list_wraparound, NULL);
MENUITEM_SETTING(list_order, &global_settings.list_order, NULL);

MAKE_MENU(scroll_settings_menu, ID2P(LANG_SCROLL_MENU), 0, Icon_NOICON,
          &scroll_speed, &scroll_delay,
          &scroll_step,
          &bidir_limit,
          &offset_out_of_view,
          &disable_mainmenu_scrolling,
          &screen_scroll_step,
          &scroll_paginated,
          &list_wraparound,
          &list_order,
          );

/** Peak meter menu **/

static int peakmeter_callback(int action,
                              const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM:
            peak_meter_init_times(global_settings.peak_meter_release,
                                    global_settings.peak_meter_hold,
                                    global_settings.peak_meter_clip_hold);
            break;
    }
    return action;
}
MENUITEM_SETTING(peak_meter_hold,
                 &global_settings.peak_meter_hold, peakmeter_callback);
MENUITEM_SETTING(peak_meter_clip_hold,
                 &global_settings.peak_meter_clip_hold, peakmeter_callback);
MENUITEM_SETTING(peak_meter_release,
                 &global_settings.peak_meter_release, peakmeter_callback);
/**
 * Menu to select wether the scale of the meter
 * displays dBfs of linear values.
 */
static int peak_meter_scale(void) {
    bool retval = false;
    bool use_dbfs = global_settings.peak_meter_dbfs;
    retval = set_bool_options(str(LANG_PM_SCALE),
        &use_dbfs,
        STR(LANG_PM_DBFS), STR(LANG_PM_LINEAR),
        NULL);

    /* has the user really changed the scale? */
    if (use_dbfs != global_settings.peak_meter_dbfs) {

        /* store the change */
        global_settings.peak_meter_dbfs = use_dbfs;
        peak_meter_set_use_dbfs(use_dbfs);

        /* If the user changed the scale mode the meaning of
           peak_meter_min (peak_meter_max) has changed. Thus we have
           to convert the values stored in global_settings. */
        if (use_dbfs) {

            /* we only store -dBfs */
            global_settings.peak_meter_min = -peak_meter_get_min() / 100;
            global_settings.peak_meter_max = -peak_meter_get_max() / 100;

            /* limit the returned value to the allowed range */
            if(global_settings.peak_meter_min > 89)
                global_settings.peak_meter_min = 89;
        } else {
            int max;

            /* linear percent */
            global_settings.peak_meter_min = peak_meter_get_min();

            /* converting dBfs -> percent results in a precision loss.
               I assume that the user doesn't bother that conversion
               dBfs <-> percent isn't symmetrical for odd values but that
               he wants 0 dBfs == 100%. Thus I 'correct' the percent value
               resulting from dBfs -> percent manually here */
            max = peak_meter_get_max();
            global_settings.peak_meter_max = max < 99 ? max : 100;
        }
        settings_apply_pm_range();
    }
    return retval;
}

/**
 * Adjust the min value of the value range that
 * the peak meter shall visualize.
 */
static int peak_meter_min(void) {
    bool retval = false;
    if (global_settings.peak_meter_dbfs) {

        /* for dBfs scale */
        int range_max = -global_settings.peak_meter_max;
        int min = -global_settings.peak_meter_min;

        retval =  set_int(str(LANG_PM_MIN), str(LANG_PM_DBFS), UNIT_DB,
            &min, NULL, 1, -89, range_max, NULL);

        global_settings.peak_meter_min = - min;
    }

    /* for linear scale */
    else {
        int min = global_settings.peak_meter_min;

        retval =  set_int(str(LANG_PM_MIN), "%", UNIT_PERCENT,
            &min, NULL,
            1, 0, global_settings.peak_meter_max - 1, NULL);

        global_settings.peak_meter_min = (unsigned char)min;
    }

    settings_apply_pm_range();
    return retval;
}


/**
 * Adjust the max value of the value range that
 * the peak meter shall visualize.
 */
static int peak_meter_max(void) {
    bool retval = false;
    if (global_settings.peak_meter_dbfs) {

        /* for dBfs scale */
        int range_min = -global_settings.peak_meter_min;
        int max = -global_settings.peak_meter_max;

        retval =  set_int(str(LANG_PM_MAX), str(LANG_PM_DBFS), UNIT_DB,
            &max, NULL, 1, range_min, 0, NULL);

        global_settings.peak_meter_max = - max;

    }

    /* for linear scale */
    else {
        int max = global_settings.peak_meter_max;

        retval =  set_int(str(LANG_PM_MAX), "%", UNIT_PERCENT,
            &max, NULL,
            1, global_settings.peak_meter_min + 1, 100, NULL);

        global_settings.peak_meter_max = (unsigned char)max;
    }

    settings_apply_pm_range();
    return retval;
}


/* Advanced like the three settings above them. These are function rows rather
   than settings rows, so the tag on the setting cannot hide them and the flag
   has to be given here -- and until all six carry it the screen itself, which
   is meant to disappear in Standard, stays put. */
MENUITEM_FUNCTION(peak_meter_scale_item, MENU_ADVANCED, ID2P(LANG_PM_SCALE),
                  peak_meter_scale, NULL, Icon_NOICON);
MENUITEM_FUNCTION(peak_meter_min_item, MENU_ADVANCED, ID2P(LANG_PM_MIN),
                  peak_meter_min, NULL, Icon_NOICON);
MENUITEM_FUNCTION(peak_meter_max_item, MENU_ADVANCED, ID2P(LANG_PM_MAX),
                  peak_meter_max, NULL, Icon_NOICON);
MAKE_MENU(peak_meter_menu, ID2P(LANG_PM_MENU), NULL, Icon_NOICON,
          &peak_meter_release, &peak_meter_hold,
          &peak_meter_clip_hold,
          &peak_meter_scale_item, &peak_meter_min_item, &peak_meter_max_item);



static int codepage_callback(int action,
                             const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    static int old_codepage;
    int new_codepage = global_settings.default_codepage;
    switch (action)
    {
        case ACTION_ENTER_MENUITEM:
            old_codepage = new_codepage;
            break;
        case ACTION_EXIT_MENUITEM:
            if (new_codepage != old_codepage)
                set_codepage(new_codepage);
            break;
    }
    return action;
}

/* Listed under System > Language & Text: it is about reading tags, not the
   display. Exported because that menu is built in general_settings.c. */
MENUITEM_SETTING_EXPORTED(codepage_setting, &global_settings.default_codepage, codepage_callback);


/* There is no Display menu any more. Its three entries went to the branches
   they belong to: the backlight and brightness to Battery & Power, the peak
   meter to Appearance, and the codepage to System > Language & Text, which is
   about reading text rather than displaying it. */
