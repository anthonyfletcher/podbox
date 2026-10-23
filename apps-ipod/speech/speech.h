/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to speech.c: the synthesiser as talk.c and the voice thread use
 * it.
 ****************************************************************************/

#ifndef __SPEECH_H
#define __SPEECH_H

#include <stdbool.h>
#include <stdint.h>

#define SPEECH_RATE 16000       /* Hz, the voice thread's */

/* Load what the synthesiser needs, or free it, as Voice Menus, Speech
 * Synthesiser and the language now say; nothing may be queued. Loading
 * takes 1.5 MB, which stops and rebuffers playback if the audio buffer
 * already holds the memory. */
void speech_apply_settings(void);

/* True while the synthesiser can speak. */
bool speech_enabled(void);

/* The voice text of a language string, copied into buf; false if the
 * string has none. */
bool speech_voice_string(int32_t id, char *buf, int size);

/* Turn UTF-8 text into phonemes and keep them as an item for the voice
 * thread: appended to item append_to while that is the newest and its
 * rendering has not begun, so words queued one at a time are said as one
 * phrase, or else as a new item. Returns the item, or -1 if there is
 * nothing to say or no room. */
int speech_queue(const char *utf8, int append_to);

/* An item has been rendered or dropped. Items go in the order queued. */
void speech_release(int item);

/* Drop every item. */
void speech_clear(void);

/* On the voice thread: start rendering an item, then fill buf with up to
 * count samples of it at SPEECH_RATE. Returns 0 once the item is done. */
void speech_render_begin(int item);
int speech_render(int16_t *buf, int count);

/* The item rendered last, or -1. */
int speech_rendering(void);

#endif /* __SPEECH_H */
