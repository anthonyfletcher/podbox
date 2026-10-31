/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to spike_tap.c.
 ****************************************************************************/

#ifndef SPIKE_TAP_H
#define SPIKE_TAP_H

#include <stdbool.h>

/* Tap the beat you hear and see what the tracker made of the same music.
 * Requires playback. True if USB took the screen, as the debug menu's other
 * screens return for the root menu. */
bool spike_tap_screen(void);

#endif /* SPIKE_TAP_H */
