/***************************************************************************
 * GNU General Public License (version 2 only)
 * Port of NV Speech Player, Copyright (C) 2014 NV Access Limited
 *
 * Interface to synth_frames.c, which turns IPA into frames for synth.c.
 ****************************************************************************/

#ifndef __SYNTH_FRAMES_H
#define __SYNTH_FRAMES_H

#include <stdbool.h>
#include <stdint.h>

/* IPA code points the prosody reads besides the phoneme table's own */
#define IPA_STRESS_PRIMARY   0x02c8
#define IPA_STRESS_SECONDARY 0x02cc
#define IPA_LENGTH           0x02d0
#define IPA_TIE              0x0361

/* Prepare one clause: ipa is code points, words separated by spaces, and
 * clause its closing punctuation ('.', ',', '?', '!') or 0 for none. speed
 * is in thousandths of NV Speech Player's normal rate. Returns false if the
 * clause was longer than can be held, in which case its end is dropped. */
bool synth_clause_begin(const uint16_t *ipa, int len, int clause,
                        int speed);

/* Queue as much of the clause as synth.c has room for, ending with the pause
 * after it. True once all of it is queued. */
bool synth_clause_feed(void);

#endif /* __SYNTH_FRAMES_H */
