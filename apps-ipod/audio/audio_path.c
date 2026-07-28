/***************************************************************************
 * Original code from RockBox
 * was: apps/audio_path.c
 * Audio signal path management APIs
 *
 * Copyright (C) 2007 by Michael Sevakis
 * GNU General Public License (version 2+)
 *
 * Selects the audio input source. Vestigial on these targets, which have
 * no recording or line-in.
 ****************************************************************************/
#include "system.h"
#include "cpu.h"
#include "audio.h"
#include "general.h"
#include "settings/settings.h"

/* With no recording, line-in or SPDIF on these targets there is nothing to
 * power up or monitor, so this reduces to pointing the codec's input mux at
 * the requested source. */
void audio_set_input_source(int source, unsigned flags)
{
    audio_input_mux(source, flags);
}



