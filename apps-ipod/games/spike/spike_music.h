/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * What the music is doing, in three numbers, for the generator to choose
 * patterns by.
 *
 * The source is the playback mixer's own peak reading -- the same one the
 * peak meter draws -- taken once a beat and gathered into bars.
 * That is a coarser instrument than §9.3 asks for: it has an envelope and
 * nothing spectral, so 'flux' here is how much the envelope moved within the
 * bar rather than how the spectrum changed. It buys the whole of §9.3's
 * table except the one row that separates a busy bar from a sustained one,
 * and it is close to free, which the analyser is not: the analyser costs
 * about a third of the frame budget on the 5G and the game stops polling it
 * the moment the tempo is latched.
 *
 * Everything here trails by a bar, which §9.4 says is exactly right: a
 * pattern is committed two phrases before it is reached, so no forward view
 * into the audio is needed at all.
 ****************************************************************************/

#ifndef SPIKE_MUSIC_H
#define SPIKE_MUSIC_H

#include <stdbool.h>

struct spk_mood
{
    int energy;     /* 0-100 against the loudest the track has been */
    int flux;       /* 0-100, movement within the bar against its own level */
    int trend;      /* signed: this bar against the one before it */
};

void spk_music_reset(void);

/* Sample the mixer, once per beat boundary and nowhere else.
 *
 * The reading is a peak since the last call, so calling it only here makes
 * each one the peak over exactly one beat -- a function of the audio and of
 * the grid rather than of the frame rate. Sampling per frame is biased and
 * not merely noisy: fewer frames means a longer window, so a slow stretch
 * reads louder, and a course chosen from it stops being the same course
 * twice. */
void spk_music_beat(void);

/* The bar just heard. Neutral until two bars have been gathered, so the
 * opening phrases are chosen on nothing rather than on noise. */
void spk_music_get(struct spk_mood *mood);

#endif /* SPIKE_MUSIC_H */
