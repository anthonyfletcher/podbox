/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to beat_probe.c: what one track sounds like, measured from its
 * own audio.
 ****************************************************************************/

#ifndef BEAT_PROBE_H
#define BEAT_PROBE_H

#include <stdbool.h>
#include <stdint.h>
#include "audio/beat_hops.h"
#include "audio/chroma.h"
#include "audio/track_decode.h"

/* One track's measurements.
 *
 * Every field describes the window that was analysed, not the whole track --
 * see beat_probe_start(). Fields are independent axes on purpose: a mix wants
 * to hold a character, and tempo alone is a poor description of one. Two
 * tracks at 120 BPM can be a folk ballad and a techno record, and it is the
 * level balance and the onset rate that say so. */
struct track_sound
{
    /* ---- tempo ---- */

    /* The tracker's own period, unfolded. A caller comparing tempi folds it
     * into the range a listener would tap -- doubling below 70 BPM and
     * halving above 140 -- rather than this being stored folded, because the
     * raw period is also what a later run wants as its prior. */
    unsigned int  period_ms;      /* 0 where it never locked */
    unsigned int  confidence;     /* Reported, never thresholded on: it is a
                                     correlation ratio in the tens, not a
                                     probability, and beat_track applies its
                                     own floor already */
    unsigned int  tempo_spread;   /* Slowest locked period minus the fastest.
                                     Small on a track that keeps time and
                                     large on one that does not */

    /* ---- level ---- */

    int           loudness_db10;  /* dBFS x10, so negative */
    unsigned int  crest_db10;     /* Peak over RMS, dB x10. Low is a
                                     loudness-compressed master, high is a
                                     recording that kept its transients */
    unsigned int  width;          /* Side against mid energy, 0-255. Near
                                     zero is mono or nearly so */
    unsigned char level_spread;   /* How much the level moved across the
                                     window, on the same 0-100 scale as
                                     level[]. Quiet-then-loud reads high */

    /* ---- character ---- */

    unsigned char level[BEAT_GROUPS];  /* Mean band level, 0-100. The balance
                                          between the three is the closest
                                          thing here to timbre */
    unsigned int  rate10[BEAT_GROUPS]; /* Onsets a second, in tenths */
    unsigned char strength;            /* Mean onset strength, 0-255: how far
                                          a hit stands out from its
                                          neighbours */
    unsigned char peakiness;           /* Mean. Level bands 0, one band
                                          dominating higher; see
                                          struct beat_window */

    /* ---- pitch ---- */

    /* The only part of this that hears notes. Its mode is the one axis here
     * that separates bright from dark; everything else measures how much is
     * going on rather than what kind. Read chroma_result.margin before the
     * mode: it abstains, and an abstention is not a quiet "major". */
    struct chroma_result chroma;

    /* ---- what it was measured from ---- */

    unsigned int  samplerate;
    unsigned long analysed_ms;
    unsigned int  windows;
    bool          settled;        /* The tempo met beat_probe_settled() */
};

/* Begin measuring. The caller then decodes a window of the track and hands
 * every block to beat_probe_sink().
 *
 * Feed one contiguous window rather than several short probes: the tempo is
 * read from an envelope covering the last seventeen seconds or so, and
 * restarting it throws that away. Anything else measured here has no memory
 * and would not care. */
void beat_probe_start(void);

/* A track_decode_sink. */
void beat_probe_sink(const void *ch1, const void *ch2, int count,
                     const struct track_pcm *fmt, unsigned long track_ms);

/* Whether the tempo is worth keeping and the decode may stop early: the
 * envelope is full and the period has held steady across several seconds of
 * it.
 *
 * Not simply "has it locked". A lock can arrive six seconds in, from an
 * envelope a quarter full, which is the least steady reading the tracker
 * produces -- fine for a game following the music, wrong for a number being
 * written down and kept. Nor a full envelope, which costs half as long again
 * and changes the answer on one track in eighteen; see beat_probe.c.
 *
 * Shaped to be passed straight to track_decode_run() as its 'enough'. */
bool beat_probe_settled(void);

void beat_probe_result(struct track_sound *out);

#endif /* BEAT_PROBE_H */
