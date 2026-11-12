/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to shell_tracks.c: the owner's music and library, as a cart's
 * playback and tracks calls reach them.
 ****************************************************************************/
#ifndef SHELL_TRACKS_H
#define SHELL_TRACKS_H

#include <stdbool.h>
#include <stdint.h>
#include "games/arcade/arcade.h"

struct sound_record;

extern const struct arcade_tracks shell_tracks;

/* A cart starting, each tick (whether the track is another since the last,
 * and the duck's fade a step on), and ending: the music as the owner had
 * it, undone of the duck and of a playlist of the cart's own */
void shell_tracks_start(void);
int shell_tracks_tick(void);
void shell_tracks_end(void);

/* Whether a cart is playing from a playlist of its own, the owner's set
 * aside: a dock or a car must not browse or change the queue meanwhile */
bool shell_tracks_owned(void);

/* Whether music the cart owns is held still by a pause */
bool shell_tracks_held(void);

/* The game paused (true) and playing on: music the cart owns held still
 * meanwhile, so a round timed against it loses nothing */
void shell_tracks_hold(bool hold);

/* A Sound Index record as playback.sound() and tracks.sound() hand it over,
 * in the order of SOUND.* */
void shell_sound_out(const struct sound_record *s,
                     int32_t out[ARCADE_SOUND_FIELDS]);

#endif
