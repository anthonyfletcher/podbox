/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to spike_tap.c.
 ****************************************************************************/

#ifndef SPIKE_TAP_H
#define SPIKE_TAP_H

#include <stdbool.h>

/* Tap the beat you hear and see what the tracker made of the same music.
 * Requires playback, and returns false so it can sit in a menu table. */
bool spike_tap_screen(void);

#endif /* SPIKE_TAP_H */
