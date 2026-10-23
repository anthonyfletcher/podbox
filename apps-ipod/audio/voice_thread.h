/***************************************************************************
 * Original code from RockBox
 * was: apps/voice_thread.h
 * Copyright (C) 2007 Michael Sevakis
 * GNU General Public License (version 2+)
 *
 * Interface to voice_thread.c.
 ****************************************************************************/
#ifndef VOICE_THREAD_H
#define VOICE_THREAD_H

#include "config.h"

/* A clip is speex data at start, or with text >= 0 the speech.c item to
 * synthesise instead. */
#ifndef VOICE_PLAY_CALLBACK_DEFINED
#define VOICE_PLAY_CALLBACK_DEFINED
typedef void (*voice_play_callback_t)(const void **start, size_t *size,
                                      int *text);
#endif

void voice_play_data(const void *start, size_t size, int text,
                     voice_play_callback_t get_more);
void voice_play_stop(void);

void voice_wait(void);
/* True while a clip is being said, and for a moment after */
bool voice_is_playing(void);
/* Hold the next utterance for this many ticks from now: a value that is
 * still changing is said once it stops. */
void voice_settle(long ticks);
void voice_stop(void);

void voice_thread_init(void);
void voice_thread_kill(void);

void voice_set_mixer_level(int percent);

void voice_thread_set_priority(int priority);

#endif /* VOICE_THREAD_H */
