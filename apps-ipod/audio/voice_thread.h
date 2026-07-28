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

#ifndef VOICE_PLAY_CALLBACK_DEFINED
#define VOICE_PLAY_CALLBACK_DEFINED
typedef void (*voice_play_callback_t)(const void **start, size_t *size);
#endif

void voice_play_data(const void *start, size_t size,
                     voice_play_callback_t get_more);
void voice_play_stop(void);

void voice_wait(void);
void voice_stop(void);

void voice_thread_init(void);
void voice_thread_kill(void);

void voice_set_mixer_level(int percent);

void voice_thread_set_priority(int priority);

#endif /* VOICE_THREAD_H */
