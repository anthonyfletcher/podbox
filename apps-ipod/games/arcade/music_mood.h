/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * What the music is doing, in three numbers, for beat.mood().
 *
 * The source is the playback mixer's own peak reading -- the same one the
 * peak meter draws -- taken once a beat and gathered into bars. It has an
 * envelope and nothing spectral, so 'flux' is how much the envelope moved
 * within the bar rather than how the spectrum changed; it cannot tell a
 * busy bar from a sustained one. It is close to free, which the analyser is
 * not: the analyser costs about a third of the frame budget on the 5G.
 *
 * Everything here trails by a bar: a cart choosing what comes next from it
 * chooses a bar or more ahead, so no forward view into the audio is
 * needed.
 ****************************************************************************/

#ifndef MUSIC_MOOD_H
#define MUSIC_MOOD_H

#include <stdbool.h>

struct music_mood
{
    int energy;     /* 0-100 against the loudest the track has been */
    int flux;       /* 0-100, movement within the bar against its own level */
    int trend;      /* signed: this bar against the one before it */
};

void music_mood_reset(void);

/* Sample the mixer, once per beat boundary and nowhere else.
 *
 * The reading is a peak since the last call, so calling it only here makes
 * each one the peak over exactly one beat -- a function of the audio and of
 * the grid rather than of the frame rate. Sampling per frame is biased and
 * not merely noisy: fewer frames means a longer window, so a slow stretch
 * reads louder. */
void music_mood_beat(void);

/* The bar just heard. Neutral until two bars have been gathered, so the
 * opening phrases are chosen on nothing rather than on noise. */
void music_mood_get(struct music_mood *mood);

#endif /* MUSIC_MOOD_H */
