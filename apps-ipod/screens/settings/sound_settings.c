/***************************************************************************
 * Original code from RockBox
 * was: apps/menus/sound_menu.c
 * Copyright (C) 2007 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * Sound settings menu: volume, bass, treble, balance and the
 * channel/stereo options.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include "config.h"
#include "sound.h"
#include "lang.h"
#include "input/action.h"
#include "settings/settings.h"
#include "widgets/menu.h"
#include "eq_settings.h"
#include "exported_settings.h"
#include "common_settings.h"
#include "widgets/splash.h"
#include "kernel.h"
#include "speech/talk.h"
#include "widgets/option_select.h"
#include "system/volume.h"

static const char* vol_limit_format(char* buffer, size_t buffer_size, int value,
                      const char* unit)
{
    (void)unit;
    format_sound_value(buffer, buffer_size, SOUND_VOLUME, value);
    return buffer;
}

static int volume_limit_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;

    static struct int_setting volume_limit_int_setting;

    volume_limit_int_setting = (struct int_setting)
    {
        .option_callback = NULL,
        .unit = UNIT_DB,
        .step = sound_steps(SOUND_VOLUME),
        .min = sound_min(SOUND_VOLUME),
        .max = sound_max(SOUND_VOLUME),
        .formatter = vol_limit_format,
        .get_talk_id = NULL
    };

    struct settings_list setting;
    setting.flags = F_BANFROMQS|F_INT_SETTING|F_T_INT|F_NO_WRAP;
    setting.lang_id = LANG_VOLUME_LIMIT;
    setting.default_val.int_ = volume_limit_int_setting.max;
    setting.int_setting = &volume_limit_int_setting;

    switch (action)
    {
        case ACTION_ENTER_MENUITEM:
            setting.setting = &global_settings.volume_limit;
            option_screen(&setting, NULL, false, ID2P(LANG_VOLUME_LIMIT));
            /* Fallthrough */
        case ACTION_EXIT_MENUITEM: /* on exit */
            setvol();
            break;
    }
    return action;
}

/** Sound menu **/
MENUITEM_SETTING(volume, &global_status.volume, NULL);
MENUITEM_SETTING(volume_limit, &global_settings.volume_limit, volume_limit_callback);
MENUITEM_SETTING(bass, &global_settings.bass,
    NULL
);

MENUITEM_SETTING(bass_cutoff, &global_settings.bass_cutoff, NULL);


MENUITEM_SETTING(treble, &global_settings.treble,
    NULL
);

MENUITEM_SETTING(treble_cutoff, &global_settings.treble_cutoff, NULL);


MENUITEM_SETTING(balance, &global_settings.balance, NULL);
MENUITEM_SETTING(channel_config, &global_settings.channel_config,
    lowlatency_callback
);
MENUITEM_SETTING(stereo_width, &global_settings.stereo_width,
    lowlatency_callback
);





    /* Crossfeed Submenu */
    MENUITEM_SETTING(crossfeed, &global_settings.crossfeed, lowlatency_callback);
    MENUITEM_SETTING(crossfeed_direct_gain,
                     &global_settings.crossfeed_direct_gain, lowlatency_callback);
    MENUITEM_SETTING(crossfeed_cross_gain,
                     &global_settings.crossfeed_cross_gain, lowlatency_callback);
    MENUITEM_SETTING(crossfeed_hf_attenuation,
                     &global_settings.crossfeed_hf_attenuation, lowlatency_callback);
    MENUITEM_SETTING(crossfeed_hf_cutoff,
                     &global_settings.crossfeed_hf_cutoff, lowlatency_callback);
    MAKE_MENU(crossfeed_menu,ID2P(LANG_CROSSFEED), NULL, Icon_NOICON,
              &crossfeed, &crossfeed_direct_gain, &crossfeed_cross_gain,
              &crossfeed_hf_attenuation, &crossfeed_hf_cutoff);

static int timestretch_callback(int action,
                                const struct menu_item_ex *this_item,
                                struct gui_synclist *this_list)
{
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM: /* on exit */
            if (global_settings.timestretch_enabled && !dsp_timestretch_available())
                splash(HZ*2, ID2P(LANG_PLEASE_REBOOT));
            break;
    }
    lowlatency_callback(action, this_item, NULL);
    return action;
}
    MENUITEM_SETTING(timestretch_enabled,
                     &global_settings.timestretch_enabled, timestretch_callback);

    MENUITEM_SETTING(dithering_enabled,
                     &global_settings.dithering_enabled, lowlatency_callback);
    MENUITEM_SETTING(afr_enabled,
                     &global_settings.afr_enabled, lowlatency_callback);
    MENUITEM_SETTING(pbe,
                     &global_settings.pbe, lowlatency_callback);
    MENUITEM_SETTING(pbe_precut,
                     &global_settings.pbe_precut, lowlatency_callback);
    MAKE_MENU(pbe_menu,ID2P(LANG_PBE), NULL, Icon_NOICON,
              &pbe,&pbe_precut);
    MENUITEM_SETTING(surround_enabled,
                     &global_settings.surround_enabled, lowlatency_callback);
    MENUITEM_SETTING(surround_balance,
                     &global_settings.surround_balance, lowlatency_callback);
    MENUITEM_SETTING(surround_fx1,
                     &global_settings.surround_fx1, lowlatency_callback);
    MENUITEM_SETTING(surround_fx2,
                     &global_settings.surround_fx2, lowlatency_callback);
    MENUITEM_SETTING(surround_method2,
                     &global_settings.surround_method2, lowlatency_callback);
    MENUITEM_SETTING(surround_mix,
                     &global_settings.surround_mix, lowlatency_callback);
    MAKE_MENU(surround_menu,ID2P(LANG_SURROUND), NULL, Icon_NOICON,
              &surround_enabled,&surround_balance,&surround_fx1,&surround_fx2,&surround_method2,&surround_mix);

    /* compressor submenu */
    MENUITEM_SETTING(compressor_threshold,
                     &global_settings.compressor_settings.threshold,
                     lowlatency_callback);
    MENUITEM_SETTING(compressor_gain,
                     &global_settings.compressor_settings.makeup_gain,
                     lowlatency_callback);
    MENUITEM_SETTING(compressor_ratio,
                     &global_settings.compressor_settings.ratio,
                     lowlatency_callback);
    MENUITEM_SETTING(compressor_knee,
                     &global_settings.compressor_settings.knee,
                     lowlatency_callback);
    MENUITEM_SETTING(compressor_attack,
                     &global_settings.compressor_settings.attack_time,
                     lowlatency_callback);
    MENUITEM_SETTING(compressor_release,
                     &global_settings.compressor_settings.release_time,
                     lowlatency_callback);
    MAKE_MENU(compressor_menu,ID2P(LANG_COMPRESSOR), NULL, Icon_NOICON,
              &compressor_threshold, &compressor_gain, &compressor_ratio,
              &compressor_knee, &compressor_attack, &compressor_release);



MAKE_MENU(sound_settings, ID2P(LANG_SOUND_SETTINGS), NULL, Icon_Audio,
          &volume
          ,&volume_limit
          ,&bass
          ,&bass_cutoff
          ,&treble
          ,&treble_cutoff
          ,&balance,&channel_config,&stereo_width
          ,&crossfeed_menu, &equalizer_menu, &dithering_enabled
          ,&surround_menu, &pbe_menu, &afr_enabled
          ,&timestretch_enabled
          ,&compressor_menu
         );
