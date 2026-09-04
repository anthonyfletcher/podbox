/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to pv_play.c.
 ****************************************************************************/
#ifndef _PV_PLAY_H
#define _PV_PLAY_H

#include "pv_tiles.h"

/* Play what a card is about.
 *
 * Returns the number of tracks queued, 0 when the library holds nothing under
 * that name, and -1 when the attempt could not be made at all -- the database
 * was busy, the playlist refused, or the user declined to lose the current
 * one. Only 0 needs the caller to say anything: the other two have already
 * put their own message on screen or are the user's own choice.
 *
 * Nothing is queued until a match is certain, so a miss leaves whatever is
 * playing exactly as it was.
 *
 * Safe to call from inside a screen holding the app buffer: an album is put
 * in track order in a small static array rather than in scratch from there,
 * which is the one thing that forces the carousel to defer its own play until
 * after teardown. */
int pv_play_target(const struct pv_target *t);

#endif /* _PV_PLAY_H */
