/***************************************************************************
 * Original code from RockBox
 * was: apps/abrepeat.h
 * Copyright (C) 2005 Ray Lambert
 * GNU General Public License (version 2+)
 *
 * Interface to abrepeat.c.
 ****************************************************************************/
#ifndef _AB_REPEAT_H_
#define _AB_REPEAT_H_
#include <stdbool.h>

#include "audio.h"
#include "kernel.h" /* needed for HZ */

#define AB_MARKER_NONE 0

#include "settings/settings.h"

bool ab_before_A_marker(unsigned int song_position);
bool ab_after_A_marker(unsigned int song_position);
void ab_jump_to_A_marker(void);
void ab_reset_markers(void);
void ab_set_A_marker(unsigned int song_position);
void ab_set_B_marker(unsigned int song_position);
/* These return whether the marker are actually set.
 * The actual positions are returned via output parameter */
bool ab_get_A_marker(unsigned int *song_position);
bool ab_get_B_marker(unsigned int *song_position);
void ab_end_of_track_report(void);

/* These functions really need to be inlined for speed */
extern unsigned int ab_A_marker;
extern unsigned int ab_B_marker;

static inline bool ab_repeat_mode_enabled(void)
{
    return global_settings.repeat_mode == REPEAT_AB;
}

static inline bool ab_reached_B_marker(unsigned int song_position)
{
/* following is the size of the window in which we'll detect that the B marker
was hit; it must be larger than the frequency (in milliseconds) at which this 
function is called otherwise detection of the B marker will be unreliable */
/* On swcodec, the worst case seems to be 9600kHz with 1024 samples between
 * calls, meaning ~9 calls per second, look within 1/5 of a second */
#define B_MARKER_DETECT_WINDOW 200
    if (ab_B_marker != AB_MARKER_NONE)
    {
        if ( (song_position >= ab_B_marker) 
        && (song_position <= (ab_B_marker+B_MARKER_DETECT_WINDOW)) )
            return true;
    }
    return false;
}

static inline void ab_position_report(unsigned long position)
{
    if (ab_repeat_mode_enabled())
    {
        if ( !(audio_status() & AUDIO_STATUS_PAUSE) && 
                ab_reached_B_marker(position) )
        {
            ab_jump_to_A_marker();
        }
    }
}


#endif /* _AB_REPEAT_H_ */
