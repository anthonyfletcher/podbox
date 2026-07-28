/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.h (declaration only; beep.c itself was never split)
 * GNU General Public License (version 2+)
 *
 * Interface to beep.c: software tone generation mixed into the PCM stream.
 * Only built where the target has no hardware beeper, which is the case for
 * both iPods this fork supports.
 ****************************************************************************/
#ifndef _BEEP_H_
#define _BEEP_H_

/* Generate a tone and mix it into the PCM output. Duration is in ticks,
 * amplitude scales the waveform. */
void beep_play(unsigned int frequency, unsigned int duration,
                unsigned int amplitude);

#endif /* _BEEP_H_ */
