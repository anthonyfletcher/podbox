/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.h (system sounds and keyclick)
 * GNU General Public License (version 2+)
 *
 * Interface to sound_feedback.c.
 ****************************************************************************/
#ifndef _SOUND_FEEDBACK_H_
#define _SOUND_FEEDBACK_H_

#include <stdbool.h>

enum system_sound
{
    SOUND_KEYCLICK = 0,
    SOUND_TRACK_SKIP,
    SOUND_TRACK_NO_MORE,
    SOUND_LIST_EDGE_BEEP_WRAP,
    SOUND_LIST_EDGE_BEEP_NOWRAP,
};

/* Play one of the standard UI sounds. */
void system_sound_play(enum system_sound sound);

/* A screen may intercept the keyclick to suppress or alter it; the callback
 * returns false to silence this one. */
typedef bool (*keyclick_callback)(int action, void* data);
void keyclick_set_callback(keyclick_callback cb, void* data);

/* Produce a keyclick for this button/action, honouring the settings. */
void keyclick_click(bool rawbutton, int action);

#endif /* _SOUND_FEEDBACK_H_ */
