/***************************************************************************
 * Original code from RockBox
 * was: apps/menus/eq_menu.c
 * Copyright (C) 2006 Dan Everton
 * GNU General Public License (version 2+)
 *
 * Equaliser settings menu: applying the settings to the DSP, the numeric
 * band list, preset save/reset, and the menu itself. The curve editor and
 * the presets list are eq_editor.c.
 *
 * The band list is not a plain settings list. Its list has one row per
 * band, but selecting a band expands it into that band's cutoff/gain/Q rows
 * in place -- so a list index does not map to a fixed item. selection_to_
 * banditem() does that mapping, and every list callback goes through it.
 *
 * Gains are in tenths of a decibel throughout (EQ_USER_DIVISOR), so a
 * displayed "-4.5 dB" is -45 here.
 *
 * Parts, in order:
 *   - applying settings to the DSP, with Auto Headroom
 *   - the band list: the expand-in-place list and its index mapping
 *   - preset save/reset, and the menu declarations
 ****************************************************************************/

#include "config.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "eq_settings.h"
#include "system.h"
#include "kernel.h"
#include "widgets/list.h"
#include "widgets/menu.h"
#include "input/action.h"
#include "settings/settings.h"
#include "draw/icon_bitmaps.h"
#include "lang.h"
#include "speech/talk.h"
#include "sound.h"
#include "dsp_proc_settings.h"
#include "widgets/keyboard.h"
#include "common_settings.h"
#include "exported_settings.h"
#include "audio/pcmbuf.h"
#include "widgets/option_select.h"
#include "root_menu.h"          /* MENU_ATTACHED_USB */
#include "string-extra.h"
#include "dsp_core.h"
#include "dsp_misc.h"
#include "pcm_sampr.h"
#include "eq_curve.h"
#include "eq_editor.h"

/*
 * Utility functions
 */

const char* eq_q_format(char* buffer, size_t buffer_size, int value, const char* unit)
{
    snprintf(buffer, buffer_size, "%d.%d %s", value / EQ_USER_DIVISOR,
         value % EQ_USER_DIVISOR, unit);
    return buffer;
}

const char* eq_precut_format(char* buffer, size_t buffer_size, int value, const char* unit)
{
    snprintf(buffer, buffer_size, "%s%d.%d %s", value == 0 ? " " : "-",
        value / EQ_USER_DIVISOR, value % EQ_USER_DIVISOR, unit);
    return buffer;
}

void eq_enabled_option_callback(bool enabled)
{
    (void)enabled;
    eq_apply();
}

/*
 * Settings functions
 */
unsigned int eq_dsp_frequency(void)
{
    unsigned int fs = dsp_get_output_frequency(dsp_get_config(CODEC_IDX_AUDIO));
    return fs >= 8000 ? fs : HW_SAMPR_DEFAULT;
}

void eq_apply(void)
{
    if (global_settings.eq_auto_headroom)
        global_settings.eq_precut = eq_curve_headroom(eq_curve_peak(
            global_settings.eq_band_settings, eq_dsp_frequency()));
    dsp_eq_enable(global_settings.eq_enabled);
    dsp_set_eq_precut(global_settings.eq_precut);
    /* Update all bands */
    for(int i = 0; i < EQ_NUM_BANDS; i++) {
        dsp_set_eq_coefs(i, &global_settings.eq_band_settings[i]);
    }
}

int eq_setting_callback(int action,
                        const struct menu_item_ex *this_item,
                        struct gui_synclist *this_list)
{
    (void)this_list;
    switch (action)
    {
        case ACTION_ENTER_MENUITEM:
            action = lowlatency_callback(action, this_item, NULL);
            break;
        case ACTION_EXIT_MENUITEM:
            eq_apply();
            action = lowlatency_callback(action, this_item, NULL);
            break;
    }

    return action;
}
MENUITEM_SETTING(eq_enable, &global_settings.eq_enabled, eq_setting_callback);
MENUITEM_SETTING(eq_precut, &global_settings.eq_precut, eq_setting_callback);

static const char* db_format(char* buffer, size_t buffer_size, int value,
                      const char* unit)
{
    int v = abs(value);

    snprintf(buffer, buffer_size, "%s%d.%d %s", value < 0 ? "-" : "",
             v / 10, v % 10, unit);
    return buffer;
}

static int32_t get_dec_talkid(int value, int unit)
{
    return TALK_ID_DECIMAL(value, 1, unit);
}

static const struct int_setting gain_int_setting = {
    .option_callback = NULL,
    .unit = UNIT_DB,
    .step = EQ_GAIN_STEP,
    .min = EQ_GAIN_MIN,
    .max = EQ_GAIN_MAX,
    .formatter = db_format,
    .get_talk_id = get_dec_talkid,
};

static const struct int_setting q_int_setting = {
    .option_callback = NULL,
    .unit = UNIT_INT,
    .step = EQ_Q_STEP,
    .min = EQ_Q_MIN,
    .max = EQ_Q_MAX,
    .formatter = eq_q_format,
    .get_talk_id = get_dec_talkid,
};

static const struct int_setting cutoff_int_setting = {
    .option_callback = NULL,
    .unit = UNIT_HERTZ,
    .step = EQ_CUTOFF_STEP,
    .min = EQ_CUTOFF_MIN,
    .max = EQ_CUTOFF_MAX,
    .formatter = NULL,
    .get_talk_id = NULL,
};

static const struct choice_setting type_choice_setting = {
    .option_callback = NULL,
    .count = 3,
    .cfg_vals = "low_shelf,peaking,high_shelf",
    {.desc = (const unsigned char*[]) {
        ID2P(LANG_EQUALIZER_BAND_LOW_SHELF),
        ID2P(LANG_EQUALIZER_BAND_PEAK),
        ID2P(LANG_EQUALIZER_BAND_HIGH_SHELF),
    }}
};

static int banditem_lang(int band, int item);

/* The setting behind one of a band's rows -- 1 cutoff, 2 Q, 3 gain, 4 type --
 * for its picker, and for what the skin reads of the row. */
static void band_setting(struct settings_list *setting, int band, int item)
{
    struct eq_band_setting *eq = &global_settings.eq_band_settings[band];

    setting->flags = F_BANFROMQS|F_INT_SETTING|F_T_INT|F_NO_WRAP;
    switch (item)
    {
        case 1: /* cutoff */
            setting->default_val.int_ = eq_defaults[band].cutoff;
            setting->int_setting = &cutoff_int_setting;
            setting->setting = &eq->cutoff;
            break;
        case 2: /* Q */
            setting->default_val.int_ = eq_defaults[band].q;
            setting->int_setting = &q_int_setting;
            setting->setting = &eq->q;
            break;
        case 3: /* Gain */
            setting->default_val.int_ = eq_defaults[band].gain;
            setting->int_setting = &gain_int_setting;
            setting->setting = &eq->gain;
            break;
        case 4: /* Type */
            setting->flags = F_BANFROMQS|F_CHOICE_SETTING|F_T_INT;
            setting->default_val.int_ = eq_defaults[band].type;
            setting->choice_setting = &type_choice_setting;
            setting->setting = &eq->type;
            break;
    }
    setting->lang_id = banditem_lang(band, item);
}

static const char *band_value(int band, int item, char *buffer, size_t len)
{
    struct settings_list setting;

    band_setting(&setting, band, item);
    return option_get_valuestring(&setting, buffer, len,
                                  option_value_as_int(&setting));
}

static int band_range(int band, int item)
{
    struct settings_list setting;

    band_setting(&setting, band, item);
    return option_value_position(&setting);
}

static int simplelist_action_callback(int action, struct gui_synclist *lists)
{
    (void)lists;
    if (action == ACTION_STD_OK)
        return ACTION_STD_CANCEL;
    return action;
}

static void selection_to_banditem(int selection, int expanded_band, int *band, int *item)
{
    int diff = selection - expanded_band;

    if (expanded_band < 0 || diff < 0)
    {
        *item = 0;
        *band = selection;
    }
    else if (diff < 5)
    {
        *item = selection - expanded_band;
        *band = expanded_band;
    }
    else
    {
        *item = 0;
        *band = expanded_band + diff - 4;
    }
}

/* The name of an expanded band's row: 1 cutoff, 2 Q, 3 gain, 4 type. */
static int banditem_lang(int band, int item)
{
    switch (item)
    {
    case 1: /* cutoff */
        /* A peak filter is described by the centre of its bell, the two
           shelves by the corner they turn at. */
        if (global_settings.eq_band_settings[band].type == EQ_FILTER_PEAK)
            return LANG_EQUALIZER_BAND_CENTER;
        return LANG_EQUALIZER_BAND_CUTOFF;
    case 2: /* Q */
        return LANG_EQUALIZER_BAND_Q;
    case 3: /* Gain */
        return LANG_GAIN;
    case 4: /* Type */
        return LANG_EQUALIZER_FILTER_TYPE;
    }
    return -1;
}

static char *advancedmenu_item_get_name(int selected_item, void *data, char *buffer, size_t len)
{
    int band;
    int item;
    int lang;

    selection_to_banditem(selected_item, *(intptr_t*)data, &band, &item);

    if (item == 0) /* Band title */
    {
        snprintf(buffer, len, str(LANG_EQUALIZER_BAND), band);
        return buffer;
    }

    lang = banditem_lang(band, item);
    if(lang < 0)
        buffer[0] = 0;
    else {
        buffer[0] = '\t';
        strmemccpy(&buffer[1], str(lang), len - 1);
    }

    return buffer;
}

static int advancedmenu_speak_item(int selected_item, void *data)
{
    (void)data;
    int band;
    int item;

    selection_to_banditem(selected_item, *(intptr_t*)data, &band, &item);

    if (item == 0) /* Band title */
    {
        talk_id(LANG_EQUALIZER_BAND, false);
        talk_number(band, true);
        return -1;
    }
    talk_id(banditem_lang(band, item), true);
    return -1;
}

static enum themable_icons advancedmenu_get_icon(int selected_item, void * data)
{
    (void)data;
    int band;
    int item;

    selection_to_banditem(selected_item, *(intptr_t*)data, &band, &item);

    if (item == 0)
        return Icon_Submenu;
    else
        return Icon_Menu_setting;
}
/* A band's title row expands it; the rows under it are its settings. */
static enum list_row_kind advancedmenu_get_kind(int selected_item, void *data)
{
    int band, item;

    selection_to_banditem(selected_item, *(intptr_t*)data, &band, &item);
    return item == 0 ? LIST_ROW_PLAIN : LIST_ROW_SETTING;
}

static const char *advancedmenu_get_value(int selected_item, void *data,
                                          char *buffer, size_t len)
{
    int band, item;

    selection_to_banditem(selected_item, *(intptr_t*)data, &band, &item);
    return item == 0 ? NULL : band_value(band, item, buffer, len);
}

static int advancedmenu_get_range(int selected_item, void *data)
{
    int band, item;

    selection_to_banditem(selected_item, *(intptr_t*)data, &band, &item);
    return item == 0 ? -1 : band_range(band, item);
}

static int eq_do_advanced_menu(void * param)
{
    (void)param;
    struct simplelist_info info;
    struct settings_list setting;
    int band, item;
    intptr_t selected_band = -1;

    simplelist_info_init(&info, str(LANG_EQUALIZER_ADVANCED),
            EQ_NUM_BANDS, &selected_band);
    info.get_name = (list_get_name*)advancedmenu_item_get_name;
    if(global_settings.talk_menu)
        info.get_talk = advancedmenu_speak_item;
    info.get_icon = advancedmenu_get_icon;
    info.get_kind = advancedmenu_get_kind;
    info.get_value = advancedmenu_get_value;
    info.get_range = advancedmenu_get_range;
    info.action_callback = simplelist_action_callback;
    info.selection = -1;
    info.title_icon = Icon_EQ;

    while (true)
    {
        if (simplelist_show_list(&info))
            return MENU_ATTACHED_USB;
        if (info.selection < 0)
            break;
        selection_to_banditem(info.selection, selected_band, &band, &item);
        if (item == 0) /* title: expand or collapse the band */
        {
            int extra;
            if (selected_band == band)
            {
                extra = 0;
                selected_band = -1;
            }
            else
            {
                extra = 4;
                selected_band = band;
            }
            info.selection = band;
            info.count = EQ_NUM_BANDS + extra;
            continue;
        }
        band_setting(&setting, band, item);
        pcmbuf_set_low_latency(true);

        option_screen(&setting, NULL, false, str(setting.lang_id));
        eq_apply();
        pcmbuf_set_low_latency(false);
    }
    return 0;
}
int eq_band_list(void)
{
    return eq_do_advanced_menu(NULL);
}

bool eq_save_preset(char *path, size_t len)
{
    /* make sure that the eq is enabled for setting saving */
    bool enabled = global_settings.eq_enabled;
    global_settings.eq_enabled = true;

    bool result = settings_save_config_as(SETTINGS_SAVE_EQPRESET, path, len);

    global_settings.eq_enabled = enabled;

    return result;
}

void eq_reset_defaults(void)
{
    for (int i = 0; i < EQ_NUM_BANDS; i++)
        global_settings.eq_band_settings[i] = eq_defaults[i];
    eq_apply();
}

MENUITEM_FUNCTION(eq_edit, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_EQUALIZER_EDIT),
                  eq_editor_screen, lowlatency_callback, Icon_EQ);
MENUITEM_FUNCTION(eq_presets, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_EQUALIZER_PRESETS),
                  eq_presets_screen, lowlatency_callback, Icon_NOICON);

MAKE_MENU(equalizer_menu, ID2P(LANG_EQUALIZER), NULL, Icon_EQ,
        &eq_enable, &eq_edit, &eq_presets, &eq_precut);
