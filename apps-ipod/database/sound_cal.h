/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to sound_cal.c: where this library sits on each axis.
 ****************************************************************************/

#ifndef _SOUND_CAL_H
#define _SOUND_CAL_H

#include <stdbool.h>
#include <stddef.h>

/* The axes a calibration covers -- the ones the read-out puts words on.
 *
 * Pace is absent on purpose. It is beats per minute, and a listener who knows
 * what 90 BPM feels like is right about it whatever else is on the player, so
 * there is nothing for a library to say about it. */
enum {
    CAL_ENERGY = 0, CAL_BRIGHT, CAL_DENS, CAL_PEAK, CAL_CLARITY,
    CAL_WIDTH, CAL_CREST, CAL_CHANGE, CAL_LOW, CAL_MID,
    CAL_LOUD, CAL_TEMPO, CAL_SPEED, CAL_AXES
};

/* Every percentile from 0 to 100, rather than the handful the band tables
 * happen to be written against.
 *
 * Measured over the 63 mood targets of a 3,439-track library, 17 of them sit
 * at percentiles no ladder would have named -- p41, p50, p53, p56, p57, p59,
 * p73, p77, p79. A table of named points cannot key those, and rounding them
 * to the nearest named one moves a target that was placed deliberately. The
 * whole curve is 2.6K and the file is rebuilt whenever it does not match the
 * index, so storing it costs a version number and nothing that matters. */
#define CAL_PCOUNT  101

/* The axis value at 'permille' of the way up this library's distribution --
 * 0 to 1000, so 250 is the lower quartile. Or -1 where the library cannot
 * say: no calibration loaded, too few records on that axis to take a
 * percentile from, or an axis whose whole library lands in one place.
 *
 * Asked in per mille rather than percent because whole percent is not fine
 * enough to put a target back where it was taken from; see the interpolation
 * in sound_cal.c. The stored curve is still one point per percent.
 *
 * -1 is an answer, not an error. Every caller has a shipped number behind it
 * and falls back to that, which is also what a player that has never
 * calibrated does. */
int sound_cal_at(int axis, int permille);

/* Where that axis sits in struct sound_axes, so that naming an axis for a
 * percentile also says how to read it out of a track. One name per axis
 * rather than an id and an offset side by side, which could disagree. */
size_t sound_cal_offset(int axis);

/* Load the calibration, building it from the index first where the file is
 * missing or does not match. Anything about to read the bands calls this;
 * after the first time it is a flag test, and without an index it does
 * nothing at all. */
void sound_cal_ensure(void);

/* Build from the index on disk and put the file in place, whatever is there
 * already. Called where a scan has just written an index, so that the first
 * screen to ask is not the one that pays for the pass.
 *
 * False where it could not be written, which costs the shipped numbers and
 * nothing else. */
bool sound_cal_update(void);

#endif /* _SOUND_CAL_H */
