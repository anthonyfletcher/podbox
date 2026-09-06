/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to beat_track.c.
 ****************************************************************************/

#ifndef BEAT_TRACK_H
#define BEAT_TRACK_H

#include <stdbool.h>
#include <stdint.h>

struct beat_track
{
    bool          locked;      /* Confidence is high enough to trust these */
    unsigned int  period_ms;   /* One beat */
    unsigned int  bpm;
    unsigned int  confidence;  /* 0-100 */
    unsigned long last_ms;     /* Track time of the most recent beat */
    unsigned int  count;       /* Beats since the lock, for a 1-2-3-4 */
};

void beat_track_reset(void);

/* One hop's onset strength, and the track time it covers. Called for every
 * hop the analyser runs; the tempo estimate is recomputed inside, about once
 * a second. */
void beat_track_push(int flux_sum, unsigned long ms, unsigned int sampr);

/* A beat-carrying onset, by its detected time. The envelope finds the tempo
 * but is decimated to 23ms bins and cannot place a beat more accurately than
 * that; these are measured to a few milliseconds, so they are what the phase
 * is finally snapped to. */
void beat_track_onset(unsigned long ms);

void beat_track_get(struct beat_track *beat);

/* How full the onset envelope is, 0-100. The tempo is read out of that
 * envelope, so an estimate taken below 100 rests on less history than the
 * search was tuned for and is the less steady for it. A caller keeping a
 * tempo rather than following one should wait for a full envelope. */
unsigned int beat_track_fill(void);

/* Track time of the nth beat at or after 'from_ms', and its position in the
 * bar. Returns false when there is no lock to project from. Beats are
 * projected rather than waited for, which is what lets a caller act on a
 * beat that has not been heard yet.
 *
 * n counts from zero: 0 is the first beat at or after 'from_ms', 1 the one
 * after that. Passing 1 for "the next beat" is a whole period late, and on
 * a track the grid runs at half time that is half a beat of phase error. */
bool beat_track_next(unsigned long from_ms, int n,
                        unsigned long *beat_ms, int *in_bar);

#endif /* BEAT_TRACK_H */
