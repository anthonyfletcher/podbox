/***************************************************************************
 * Original code from RockBox
 * was: apps/menus/playback_menu.c
 * Copyright (C) 2007 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * Playback settings menu: shuffle, repeat, crossfade, replaygain, resume
 * and buffering options.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include "config.h"
#include "lang.h"
#include "input/action.h"
#include "widgets/splash.h"
#include "settings/settings.h"
#include "widgets/menu.h"
#include "kernel.h"
#include "playlist/playlist.h"
#include "audio.h"
#include "metadata/cuesheet.h"
#include "system/volume.h"
#include "audio/playback.h"
#include "pcm_sampr.h"
#include "speech/talk.h"

static int setcrossfadeonexit_callback(int action,
                                       const struct menu_item_ex *this_item,
                                       struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM: /* on exit */
            audio_set_crossfade(global_settings.crossfade);
            break;
    }
    return action;
}


/** Playback menu **/
MENUITEM_SETTING(shuffle_item, &global_settings.playlist_shuffle, NULL);
MENUITEM_SETTING(repeat_mode, &global_settings.repeat_mode, NULL);
MENUITEM_SETTING(play_selected, &global_settings.play_selected, NULL);

MENUITEM_SETTING(ff_rewind_accel, &global_settings.ff_rewind_accel, NULL);
MENUITEM_SETTING(ff_rewind_min_step, &global_settings.ff_rewind_min_step, NULL);
MAKE_MENU(ff_rewind_settings_menu, ID2P(LANG_WIND_MENU), 0, Icon_NOICON,
          &ff_rewind_min_step, &ff_rewind_accel);
static int buffermargin_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM: /* on exit */
            audio_set_buffer_margin(global_settings.buffer_margin);
            break;
    }
    return action;
}
MENUITEM_SETTING(buffer_margin, &global_settings.buffer_margin,
                 buffermargin_callback);
MENUITEM_SETTING(fade_on_stop, &global_settings.fade_on_stop, NULL);
MENUITEM_SETTING(single_mode, &global_settings.single_mode, NULL);
MENUITEM_SETTING(party_mode, &global_settings.party_mode, NULL);

/* crossfade submenu */
MENUITEM_SETTING(crossfade, &global_settings.crossfade, setcrossfadeonexit_callback);
MENUITEM_SETTING(crossfade_fade_in_delay,
    &global_settings.crossfade_fade_in_delay, NULL);
MENUITEM_SETTING(crossfade_fade_in_duration,
    &global_settings.crossfade_fade_in_duration, NULL);
MENUITEM_SETTING(crossfade_fade_out_delay,
    &global_settings.crossfade_fade_out_delay, setcrossfadeonexit_callback);
MENUITEM_SETTING(crossfade_fade_out_duration,
    &global_settings.crossfade_fade_out_duration, setcrossfadeonexit_callback);
MENUITEM_SETTING(crossfade_fade_out_mixmode,
    &global_settings.crossfade_fade_out_mixmode,NULL);
MAKE_MENU(crossfade_settings_menu,ID2P(LANG_CROSSFADE),0, Icon_NOICON,
          &crossfade, &crossfade_fade_in_delay, &crossfade_fade_in_duration,
          &crossfade_fade_out_delay, &crossfade_fade_out_duration,
          &crossfade_fade_out_mixmode);

/* replay gain submenu */

static int replaygain_callback(int action,
                               const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM: /* on exit */
            replaygain_update();
            break;
    }
    return action;
}
MENUITEM_SETTING(replaygain_noclip,
                 &global_settings.replaygain_settings.noclip,
                 replaygain_callback);
MENUITEM_SETTING(replaygain_type,
                 &global_settings.replaygain_settings.type,
                 replaygain_callback);
MENUITEM_SETTING(replaygain_preamp,
                 &global_settings.replaygain_settings.preamp,
                 replaygain_callback);
MAKE_MENU(replaygain_settings_menu,ID2P(LANG_REPLAYGAIN),0, Icon_NOICON,
          &replaygain_type, &replaygain_noclip, &replaygain_preamp);

MENUITEM_SETTING(beep, &global_settings.beep ,NULL);

MENUITEM_SETTING(next_folder, &global_settings.next_folder, NULL);
MENUITEM_SETTING(constrain_next_folder,
                 &global_settings.constrain_next_folder, NULL);

static int cuesheet_callback(int action,
                             const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM: /* on exit */
            audio_set_cuesheet(global_settings.cuesheet);
    }
    return action;
}
MENUITEM_SETTING(cuesheet, &global_settings.cuesheet, cuesheet_callback);

MENUITEM_SETTING(unplug_mode, &global_settings.unplug_mode, NULL);
MENUITEM_SETTING(unplug_autoresume, &global_settings.unplug_autoresume, NULL);
MAKE_MENU(unplug_menu, ID2P(LANG_HEADPHONE_UNPLUG), 0, Icon_NOICON,
          &unplug_mode, &unplug_autoresume);

MENUITEM_SETTING(skip_length, &global_settings.skip_length, NULL);
MENUITEM_SETTING(prevent_skip, &global_settings.prevent_skip, NULL);
MENUITEM_SETTING(rewind_across_tracks, &global_settings.rewind_across_tracks, NULL);
MENUITEM_SETTING(resume_rewind, &global_settings.resume_rewind, NULL);
MENUITEM_SETTING(pause_rewind, &global_settings.pause_rewind, NULL);
MENUITEM_SETTING(play_frequency, &global_settings.play_frequency, NULL);
MENUITEM_SETTING(album_art, &global_settings.album_art, NULL);

MENUITEM_SETTING(playback_log, &global_settings.playback_log, NULL);

MAKE_MENU(playback_settings,ID2P(LANG_PLAYBACK),0,
          Icon_Playback_menu,
          &shuffle_item, &repeat_mode, &play_selected,
          &ff_rewind_settings_menu,
          &buffer_margin,
          &fade_on_stop, &single_mode, &party_mode,

          &crossfade_settings_menu,

          &replaygain_settings_menu, &beep,

          &next_folder, &constrain_next_folder, &cuesheet
         ,&unplug_menu
         ,&skip_length, &prevent_skip
          ,&rewind_across_tracks

          ,&resume_rewind
          ,&pause_rewind
          ,&play_frequency
          ,&album_art
        ,&playback_log
         );

