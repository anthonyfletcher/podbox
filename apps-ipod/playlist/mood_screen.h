/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to mood_screen.c: choosing a mood or a journey.
 ****************************************************************************/

#ifndef _MOOD_SCREEN_H
#define _MOOD_SCREEN_H

#include <stdbool.h>

/* Offer the moods, or the journeys, and play what is chosen. True if a
 * playlist started, which is the caller's cue to leave for the playing
 * screen; false if nothing was chosen or nothing could be built. */
bool mood_screen_pick(bool journey);

/* The nth journey offered, in menu order: its name and the two moods it runs
 * between. False past the last. */
bool mood_screen_journey(int n, int *lang, int *from, int *to);

#endif /* _MOOD_SCREEN_H */
