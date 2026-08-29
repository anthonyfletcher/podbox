/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to spike_bar.c: which of four beats is the music's downbeat.
 ****************************************************************************/

#ifndef SPIKE_BAR_H
#define SPIKE_BAR_H

/* Forget every onset heard. A new track is a new bar. */
void spk_bar_reset(void);

/* Take in whatever the analyser has finished since the last call. Cheap
 * enough for every frame, and only meaningful while the analyser is
 * running -- which is the wait, and nothing else. */
void spk_bar_listen(void);

/* Which of the four beat positions carries the downbeat, on a grid whose
 * beat zero falls at track time 'zero_ms' and whose beats are 'beat_ms'
 * apart. Returns 0..3, or -1 where nothing wins by a margin.
 *
 * Answered from onsets already recorded, so it costs one pass and no
 * waiting: the beats it needs were heard while the tracker was still
 * making up its mind. Ask it once, with the tempo, and never again. */
int spk_bar_downbeat(long zero_ms, int beat_ms);

/* What the last decision was taken on: onsets binned, and the winner
 * against the mean of the other three in tenths. Both zero where it has not
 * been asked yet. It is here because "is the downbeat working" cannot be
 * answered by a screen that only says found or not found -- a margin of 11
 * and a margin of 40 are the same word and very different evidence. */
void spk_bar_working(int *hits, int *margin10);

#endif /* SPIKE_BAR_H */
