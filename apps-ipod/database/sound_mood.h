/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to sound_mood.c: named places in the sound index.
 ****************************************************************************/

#ifndef _SOUND_MOOD_H
#define _SOUND_MOOD_H

#include <stdbool.h>
#include "database/sound_mix.h"

enum {
    MOOD_CALM = 0, MOOD_ENERGETIC, MOOD_DARK, MOOD_BRIGHT, MOOD_WARM,
    MOOD_RAW, MOOD_LUSH, MOOD_PUNCHY, MOOD_SMOOTH, MOOD_SPARSE,
    MOOD_DENSE, MOOD_HYPNOTIC, MOOD_SLOW, MOOD_FAST, MOOD_MELANCHOLY,
    MOOD_UPLIFTING, MOOD_COUNT
};

/* How unlike the mood a track is, 0 upwards, on the same scale as the
 * track-to-track distance. Negative where the mood cannot judge the track at
 * all -- Slow and Fast have nothing to say about a track whose tempo the
 * analysis never settled on, and a guess there is worse than an omission. */
int sound_mood_score(const struct sound_axes *a, int mood);

/* The same, against a point 't' of the way from one mood to another, where t
 * runs 0..SOUND_AX.
 *
 * The targets and their weights are interpolated, not the two scores. Blending
 * scores does not describe anything in between: a weighted sum of two
 * distances is smallest at whichever end is nearer, so a journey scored that
 * way is its first mood until halfway and its second thereafter, with no
 * traversal at all. Moving the target is what makes the middle a middle. */
int sound_mood_score_between(const struct sound_axes *a, int from, int to,
                             int t);

#endif /* _SOUND_MOOD_H */
