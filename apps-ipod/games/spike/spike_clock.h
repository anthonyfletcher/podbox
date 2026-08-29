/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to spike_clock.c: the clock a grid is cut from, and the
 * mapping from a track's tempo to the grid's own.
 ****************************************************************************/

#ifndef SPIKE_CLOCK_H
#define SPIKE_CLOCK_H

#include <stdbool.h>

/* What the grid aims for, and the range an octave shift of a real tempo can
 * actually land in. Choosing the shift that minimises the distance to the
 * target always lands within a factor of root two of it, so the bounds
 * cannot be reached from any plausible tempo -- they are here to report a
 * tempo estimate that was never usable, not to fix one. Clamping instead
 * would leave the grid at a tempo that is not an integer ratio of the
 * track's, and it would then drift out of time in silence. */
#define SPK_BEAT_TARGET      500
#define SPK_BEAT_MIN         330
#define SPK_BEAT_MAX         720

/* Seed the clock at the play position. */
void spk_clock_reset(void);

/* Carry the clock to now and let the audio steer it. False where playback
 * moved underneath -- a seek, a skip or a track change -- in which case it
 * has been reseeded and whatever was cut from it must start again.
 *
 * That is judged on the position report and not on pcmbuf's key, so it
 * arrives when the new audio reaches the speaker rather than when it starts
 * being written a buffer earlier. */
bool spk_clock_tick(void);

/* Track time as of the last tick, which is what a frame should be drawn
 * from: stable for the whole of it. */
unsigned long spk_clock_ms(void);

/* How much of the track is left, or a very large number where its length is
 * not known. The run bows out on this: obstacles stop being laid while there
 * is still a phrase or two to cross, and the triangle walks off the right
 * before the music stops rather than being cut off mid-stride. */
unsigned long spk_clock_left_ms(void);

/* Track time at this instant, extrapolated past the last tick. This is what
 * a press is timestamped against, so that it is judged where it fell rather
 * than where the next frame happened to be. */
unsigned long spk_clock_now(void);

/* game_beat = track_beat times a power of two, chosen for the smallest
 * distance from the target. A 60 BPM track runs at double time and a 200 BPM
 * one at half, and anything between roughly 90 and 150 runs as it is --
 * which is what keeps the grid an exact ratio of the music at every tempo. */
int spk_octave(unsigned int track_ms);

#endif /* SPIKE_CLOCK_H */
