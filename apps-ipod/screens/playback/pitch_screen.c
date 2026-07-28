/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/pitchscreen.c
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * Pitch and speed adjustment screen. Now a thin stub -- the interactive UI
 * was a plugin and is gone; this resets pitch to normal.
 ****************************************************************************/
#include "pitch_screen.h"
#include "sound.h"
#include "dsp_proc_settings.h"

int gui_syncpitchscreen_run(void)
{
    /* The pitch screen UI was a plugin (pitch_screen.rock); with the plugin
     * system gone it is not available. reset_pitch() below still works. */
    return 0;
}

int reset_pitch(void)
{
    sound_set_pitch(PITCH_SPEED_100);
    dsp_set_timestretch(PITCH_SPEED_100);
    return 0;
}
