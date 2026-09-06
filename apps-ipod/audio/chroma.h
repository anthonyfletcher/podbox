/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to chroma.c: which notes a track is built from, and what key
 * that makes it.
 ****************************************************************************/

#ifndef CHROMA_H
#define CHROMA_H

#include <stdbool.h>
#include <stdint.h>

/* What the pitch content says.
 *
 * Everything else the analysis measures is energy, timing and spectral
 * balance, none of which can tell a major track from a minor one -- and major
 * against minor is the strongest content cue there is for whether music reads
 * as bright or dark. This is the only part that hears notes. */
struct chroma_result
{
    /* The twelve pitch classes, C first, scaled so the largest is 255. Kept
     * whole rather than only the key derived from it: two tracks in
     * neighbouring keys share most of their notes, and a reader comparing
     * these directly can see that where a key name cannot. */
    uint8_t pitch[12];

    uint8_t key;        /* 0 = C .. 11 = B */
    bool    minor;

    /* How far the winning mode beat the other, and the whole of what makes
     * the mode usable.
     *
     * A key and its relative minor contain the same twelve notes -- A minor
     * and C major are the same pitch set -- so pitch content alone cannot
     * always separate them, and music that never leans on its tonic genuinely
     * has no answer. Measured on real tracks, eight of eighteen came out
     * ambiguous, and every one of those was instrumental math rock with no
     * settled key. A reader must treat a low margin as "no mode" rather than
     * as a quiet yes: a mode reported wrong flips the sign of the axis it
     * feeds, which is worse than not having it. */
    uint8_t margin;

    /* How far the strongest pitch class stands above the average of them.
     * Low is atonal, percussive or noise; high is a clear tonal centre. Works
     * on everything, unlike the mode. */
    uint8_t clarity;

    /* How much the pitch content moves between frames. Static drones and
     * one-chord vamps read low, music that changes chord often reads high. */
    uint8_t change;

    uint16_t frames;    /* Analysed. Zero means nothing above is meaningful */
};

/* Below this the mode is not worth reporting. Named here so a reader does not
 * invent its own threshold: this one is the value that abstained on exactly
 * the tracks a listener also called ambiguous. */
#define CHROMA_MARGIN_MIN  10

void chroma_reset(unsigned int samplerate);

/* Interleaved stereo, as beat_hops_feed() takes. */
void chroma_feed(const int16_t *pcm, int frames);

void chroma_get(struct chroma_result *out);

#endif /* CHROMA_H */
