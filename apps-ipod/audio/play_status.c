/***************************************************************************
 * Original code from RockBox
 * was: apps/status.c
 * Copyright (C) 2002 by Linus Nielsen Feltzing
 * GNU General Public License (version 2+)
 *
 * The current playback mode (play, pause, ff/rewind) as the status bar and
 * skin engine read it.
 ****************************************************************************/
#include <string.h>
#include <stdbool.h>
#include "config.h"
#include "play_status.h"
#include "audio.h"

static enum playmode ff_mode = 0;

void play_status_set_ffmode(enum playmode mode)
{
    ff_mode = mode; /* Either STATUS_FASTFORWARD or STATUS_FASTBACKWARD */
}

enum playmode play_status_get_ffmode(void)
{
    /* only use this function for STATUS_FASTFORWARD or STATUS_FASTBACKWARD */
    /* use audio_status() for other modes */
    return ff_mode;
}

int current_playmode(void)
{
    int audio_stat = audio_status();

    /* ff_mode can be either STATUS_FASTFORWARD or STATUS_FASTBACKWARD
       and that supercedes the other modes */
    if(ff_mode)
        return ff_mode;
    
    if(audio_stat & AUDIO_STATUS_PLAY)
    {
        if(audio_stat & AUDIO_STATUS_PAUSE)
            return STATUS_PAUSE;
        else
            return STATUS_PLAY;
    }

    return STATUS_STOP;
}
