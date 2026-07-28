/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.c (system sounds and keyclick)
 * GNU General Public License (version 2+)
 *
 * The sounds the UI makes: the standard system sounds and the keyclick, both
 * produced through the beep generator. Input triggers them, audio produces
 * them, which is why they live here rather than with the widgets.
 ****************************************************************************/

#include <stdlib.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "sound.h"
#include "settings/settings.h"
#include "input/action.h"
#include "system/app_util.h"
#include "piezo.h"
#include "beep.h"
#include "sound_feedback.h"

void system_sound_play(enum system_sound sound)
{
    static const struct beep_params
    {
        int *setting;
        unsigned short frequency;
        unsigned short duration;
        unsigned short amplitude;
    } beep_params[] =
    {
        [SOUND_KEYCLICK] =
        { &global_settings.keyclick,
          4000, KEYCLICK_DURATION, 2500 },
        [SOUND_TRACK_SKIP] =
        { &global_settings.beep,
          2000, 100, 2500 },
        [SOUND_TRACK_NO_MORE] =
        { &global_settings.beep,
          1000, 100, 1500 },
        [SOUND_LIST_EDGE_BEEP_NOWRAP] =
        { &global_settings.keyclick,
          1000, 40, 1500 },
        [SOUND_LIST_EDGE_BEEP_WRAP] =
        { &global_settings.keyclick,
          2000, 20, 1500 },

    };

    const struct beep_params *params = &beep_params[sound];

    /* setting points at the global that controls this sound; it is both the
     * on/off test and a 1-N loudness multiplier, so zero means silent. */
    if (*params->setting)
    {
        beep_play(params->frequency, params->duration,
                  params->amplitude * *params->setting);
    }
}

static keyclick_callback keyclick_current_callback = NULL;
static void* keyclick_data = NULL;

void keyclick_set_callback(keyclick_callback cb, void* data)
{
    keyclick_current_callback = cb;
    keyclick_data = data;
}

/* Produce keyclick based upon button and global settings */
void keyclick_click(bool rawbutton, int action)
{
    int button = action;
    static long last_button = BUTTON_NONE;
    bool do_beep = false;

    if (!rawbutton)
        get_action_statuscode(&button);

    /* Settings filters */
    if (
        (global_settings.keyclick || global_settings.keyclick_hardware)
        )
    {
        if (global_settings.keyclick_repeats || !(button & BUTTON_REPEAT))
        {
            /* Button filters */
            if (button != BUTTON_NONE && !(button & BUTTON_REL)
                && !(button & (SYS_EVENT|BUTTON_MULTIMEDIA)) )
            {
                do_beep = true;
            }
        }
        else if ((button & BUTTON_REPEAT) && (last_button == BUTTON_NONE))
        {
            do_beep = true;
        }
        else if (button & (BUTTON_SCROLL_BACK | BUTTON_SCROLL_FWD))
        {
            do_beep  = true;
        }
    }
    if (button&BUTTON_REPEAT)
        last_button = button;
    else
        last_button = BUTTON_NONE;

    if (do_beep && keyclick_current_callback)
        do_beep = keyclick_current_callback(action, keyclick_data);
    keyclick_current_callback = NULL;

    if (do_beep)
    {
        if (global_settings.keyclick)
        {
            system_sound_play(SOUND_KEYCLICK);
        }
        if (global_settings.keyclick_hardware)
        {
            piezo_button_beep(false, false);
        }
    }
}
