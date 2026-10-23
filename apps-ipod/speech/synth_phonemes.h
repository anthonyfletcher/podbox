/***************************************************************************
 * GNU General Public License (version 2 only)
 * Port of NV Speech Player, Copyright (C) 2014 NV Access Limited
 *
 * The phoneme table: what each IPA symbol sets in a frame. synth_phonemes.c
 * is written by tools/speech/gen_phonemes.py from NV Speech Player's data.py.
 ****************************************************************************/

#ifndef __SYNTH_PHONEMES_H
#define __SYNTH_PHONEMES_H

#include "synth.h"

#define PH_NASAL         0x01
#define PH_STOP          0x02
#define PH_LIQUID        0x04
#define PH_VOWEL         0x08
#define PH_VOICED        0x10
#define PH_SEMIVOWEL     0x20
#define PH_AFRICATE      0x40
#define PH_COPY_ADJACENT 0x80   /* takes what it lacks from a neighbour */

struct synth_phoneme
{
    uint16_t key[3];            /* IPA code points, zero-padded */
    uint16_t flags;
    uint64_t present;           /* bit n set: p[n] is this phoneme's */
    synth_param_t p[SYNTH_PARAM_COUNT];
};

extern const struct synth_phoneme synth_phonemes[];
extern const int synth_phoneme_count;

/* Bit k set: some phoneme gives parallel formant k+1 an amplitude. The
 * others never reach the output, so they are not run. */
extern const unsigned int synth_parallel_used;

#endif /* __SYNTH_PHONEMES_H */
