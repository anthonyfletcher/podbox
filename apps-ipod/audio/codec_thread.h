/***************************************************************************
 * Original code from RockBox
 * was: apps/codec_thread.h
 * Copyright (C) 2005 Miika Pekkarinen
 * GNU General Public License (version 2+)
 *
 * Interface to codec_thread.c.
 ****************************************************************************/

#ifndef _CODEC_THREAD_H
#define _CODEC_THREAD_H

#include <stdbool.h>
#include "config.h"

/* codec identity */
const char *get_codec_filename(int cod_spec);

/* codec thread */
void codec_thread_init(void) INIT_ATTR;

/* Audio MUST be stopped before requesting callback! */
void codec_thread_do_callback(void (*fn)(void),
                              unsigned int *codec_thread_id);

int codec_thread_get_priority(void);
int codec_thread_set_priority(int priority);

/* codec commands - on audio thread only! */
bool codec_load(int hid, int cod_spec);
void codec_go(void);
bool codec_pause(void);
void codec_seek(long time);
void codec_stop(void);
void codec_unload(void);
int codec_loaded(void);

/* */

#endif /* _CODEC_THREAD_H */
