/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to spike.c.
 ****************************************************************************/

#ifndef SPIKE_H
#define SPIKE_H

#include <stdbool.h>

/* Runs the game until the player leaves it or the playlist ends. Requires
 * playback to be under way: Spike has no clock of its own and is meaningless
 * without a track.
 *
 * One run, over whatever is playing, across every track boundary it meets.
 * There was a per-track mode beside this and it is gone: three and a half
 * minutes is not long enough to learn the game in, so the difficulty never
 * climbed anywhere worth reaching and every track handed the player the
 * opening again. The unit is the run.
 *
 * True when USB or the game's menu sent the player to the root, which
 * the caller passes on. */
bool spike_screen(void);

/* The best run there has been, on the same screen a finished one is shown
 * on. 'font' is the face the track names are set in: the game has one open
 * and the menu this is reached from is inside the game. */
void spike_best_screen(int font);

#endif /* SPIKE_H */
