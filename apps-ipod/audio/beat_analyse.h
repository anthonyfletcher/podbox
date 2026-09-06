/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to beat_analyse.c: the hop machine driven from playback.
 ****************************************************************************/

#ifndef BEAT_ANALYSE_H
#define BEAT_ANALYSE_H

#include <stdbool.h>
#include <stdint.h>
#include "audio/beat_hops.h"

/* Audio time the debug trace spans, oldest at the left. */
#define BEAT_VIEW_MS  1800

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
