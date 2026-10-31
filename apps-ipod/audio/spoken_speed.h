/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to spoken_speed.c: Spoken Word Speed.
 ****************************************************************************/
#ifndef _SPOKEN_SPEED_H_
#define _SPOKEN_SPEED_H_

#include <stdbool.h>

/* Once, at boot, after the settings are read and before the audio buffer
 * takes the memory */
void spoken_speed_init(void);
/* Spoken Word Speed's callback: 'pct' is the new setting */
void spoken_speed_set(int pct);
/* Whether the playing track is spoken word, which the speed applies to */
bool spoken_speed_playing(void);

struct mp3entry;
/* Whether 'id3' is spoken word, by its genre: what Spoken Word Speed and
 * Spoken Word Skip Length apply to */
bool spoken_word_track(const struct mp3entry *id3);
/* The audio thread, before the codec decodes 'id3': its speed */
void spoken_speed_track(const struct mp3entry *id3);

#endif /* _SPOKEN_SPEED_H_ */
