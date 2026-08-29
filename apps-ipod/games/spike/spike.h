/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to spike.c.
 ****************************************************************************/

#ifndef SPIKE_H
#define SPIKE_H

#include <stdbool.h>

/* The two ways to play. They differ in exactly one rule -- what a death
 * costs -- and everything else about them follows from it, which is why
 * they are a pair rather than a difficulty setting.
 *
 * Run is the playlist and the long game: a death is survivable, and the
 * score and the distance carry across track boundaries until the playlist
 * ends. Song is one track: a death restarts it and the attempt is thrown
 * away, and finishing the track is the win. */
enum spike_mode
{
    SPIKE_MODE_RUN,
    SPIKE_MODE_SONG
};

/* Runs the game until the user leaves it, or until the mode says the run is
 * over. Requires playback to be under way: Spike has no clock of its
 * own and is meaningless without a track. Returns false, so it can sit in a
 * menu table beside the rest. */
bool spike_screen(enum spike_mode mode);

/* The two the menus call, since a menu table wants a function of no
 * arguments. */
bool spike_run_screen(void);
bool spike_song_screen(void);

#endif /* SPIKE_H */
