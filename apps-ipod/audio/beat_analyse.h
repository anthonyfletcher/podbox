/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to beat_analyse.c.
 ****************************************************************************/

#ifndef BEAT_ANALYSE_H
#define BEAT_ANALYSE_H

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
 * look-ahead: the analysis runs right up to the codec's write cursor, so the
 * newest hop sits up to 3s in the future, and a shorter history would never
 * reach back to what is being heard. */
#define BEAT_HISTORY  640

/* Audio time the debug trace spans, oldest at the left. */
#define BEAT_VIEW_MS  1800

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
};

/* Everything the debug screen reports, gathered in one call so the numbers
 * it prints all describe the same instant. */
struct beat_status
{
    unsigned int lookahead_ms;   /* Decoded audio the DAC has not reached */
    unsigned int lead_ms;        /* How far ahead of it this has analysed */
    unsigned long play_ms;       /* Track time being heard now, which is what
                                    a hop's own time is measured against */
    unsigned int samplerate;
    unsigned int windows;        /* Windows analysed since the last start */
    unsigned int reseeds;        /* Times the cursor went stale */
    int          flux[BEAT_GROUPS];
    int          threshold[BEAT_GROUPS];
    unsigned int onsets[BEAT_GROUPS];
    unsigned int rate10[BEAT_GROUPS]; /* Onsets per second, in tenths */
};

/* Seed the cursor at the play position and clear the history. */
void beat_analyse_start(void);
void beat_analyse_stop(void);

/* Analyse newly decoded audio, up to a bounded amount per call so a slow
 * caller cannot spend an unbounded time here. Call at least every 100ms or
 * the cursor falls behind the codec and has to be reseeded. */
void beat_analyse_poll(void);

void beat_analyse_status(struct beat_status *status);

/* Window 'back' places behind the newest, 0 being the newest. Returns NULL
 * past what has been analysed. */
const struct beat_window *beat_analyse_window(int back);

#endif /* BEAT_ANALYSE_H */
