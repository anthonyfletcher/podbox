/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to beat_hops.c: PCM in, onsets out.
 ****************************************************************************/

#ifndef BEAT_HOPS_H
#define BEAT_HOPS_H

#include <stdbool.h>
#include <stdint.h>

/* The three bands onsets are reported in. Low is kick and bass, mid carries
 * vocals and most lead instruments, high is hats and cymbals. */
enum beat_group
{
    BEAT_LOW = 0,
    BEAT_MID,
    BEAT_HIGH,
    BEAT_GROUPS
};

/* Hops kept. At 172 a second this is 3.7s, which has to cover the whole
 * look-ahead: a live caller analyses right up to the codec's write cursor,
 * so the newest hop sits up to 3s in the future, and a shorter history would
 * never reach back to what is being heard. */
#define BEAT_HISTORY  640

/* One analysis window's result. */
struct beat_window
{
    uint32_t time_ms;             /* Track time of the audio it covers */
    uint8_t  level[BEAT_GROUPS]; /* 0-100, log-compressed for display */
    uint8_t  onset;               /* Bit per group that fired: 1 << group */

    /* How hard the onset hit, as its flux against the decaying peak of the
     * onsets around it: 128 is level with that peak and 192 is half again
     * above it, which is section 4.1's accent. Level cannot answer this --
     * a loud passage has a high level throughout, and what marks an accent
     * is standing out from its neighbours rather than being loud. Zero
     * where nothing fired. */
    uint8_t  strength;

    /* How far the hop's energy leans on one of the envelope's six wide
     * bands, as the gap between their arithmetic and geometric means in
     * hundredths of a nat, capped at 255. Zero is level bands -- noise-like
     * -- and it climbs as one band takes over, which is what a tonal
     * passage does.
     *
     * A measured unit rather than a 0-255 scale of its own: real music
     * occupies a small part of the possible range, and a scale invented to
     * span the range put every track within a few points of the top. */
    uint8_t  peakiness;
};

/* Per-group read-out, for a caller reporting what the analysis is doing. */
struct beat_bands
{
    int          flux[BEAT_GROUPS];
    int          threshold[BEAT_GROUPS];
    unsigned int onsets[BEAT_GROUPS];
    unsigned int rate10[BEAT_GROUPS]; /* Onsets per second, in tenths */
};

/* Begin a new analysis at 'sampr': clears the history and the running means,
 * and resets the beat tracker with them. */
void beat_hops_reset(unsigned int sampr);

/* The samples about to be fed do not run on from the last ones -- a seek, a
 * new track, or a cursor that lost its place. The running means survive:
 * they describe the mix rather than the position in it, and rebuilding them
 * from nothing costs a burst of false onsets. */
void beat_hops_resync(void);

/* A rate change invalidates every Goertzel coefficient and every frame count
 * derived from the old rate, so this resyncs as well. */
void beat_hops_set_rate(unsigned int sampr);
unsigned int beat_hops_rate(void);

/* Track time of the next frame to be fed. Nothing is analysed until a caller
 * has said this at least once: hops are placed on the track's own timeline
 * rather than on the caller's, so that two runs over one track analyse the
 * same samples. */
void beat_hops_set_pos(unsigned long ms);

/* Interleaved stereo frames, analysed a hop at a time. Feed them in the
 * order they play; the caller is free to stop between calls. */
void beat_hops_feed(const int16_t *pcm, int frames);

/* Window 'back' places behind the newest, 0 being the newest. Returns NULL
 * past what has been analysed. */
const struct beat_window *beat_hops_window(int back);

/* Track time of the newest window, or 0 before there is one. */
unsigned long beat_hops_newest_ms(void);

/* Windows analysed since the last reset. */
unsigned int beat_hops_windows(void);

void beat_hops_bands(struct beat_bands *out);

#endif /* BEAT_HOPS_H */
