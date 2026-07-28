/***************************************************************************
 * Original code from RockBox
 * was: apps/pcmbuf.h
 * Copyright (C) 2005 by Miika Pekkarinen
 * GNU General Public License (version 2+)
 *
 * Interface to pcmbuf.c.
 ****************************************************************************/
#ifndef PCMBUF_H
#define PCMBUF_H

#include <sys/types.h>

/* Commit PCM data */
void *pcmbuf_request_buffer(int *count);
void pcmbuf_write_complete(int count, unsigned long elapsed, off_t offset);

/* Init */
size_t pcmbuf_size_reqd(void);
size_t pcmbuf_init(void *bufend);

/* Playback */
void pcmbuf_play_start(void);
void pcmbuf_play_stop(void);
void pcmbuf_pause(bool pause);

/* Track change */

/* Track change origin type */
enum pcm_track_change_type
{
    TRACK_CHANGE_NONE = 0,     /* No track change pending */
    TRACK_CHANGE_MANUAL,       /* Manual change (from user) */
    TRACK_CHANGE_AUTO,         /* Automatic change (from codec) */
    TRACK_CHANGE_AUTO_PILEUP,  /* Auto change during pending change */
    TRACK_CHANGE_END_OF_DATA,  /* Expect no more data (from codec) */
};
void pcmbuf_monitor_track_change(bool monitor);
void pcmbuf_start_track_change(enum pcm_track_change_type type);

/* Crossfade */
void pcmbuf_request_crossfade_enable(int setting);
bool pcmbuf_is_same_size(void);

/* Debug menu, other metrics */
size_t pcmbuf_free(void);
size_t pcmbuf_get_bufsize(void);
int pcmbuf_used_descs(void);
int pcmbuf_descs(void);

/* Fading and channel volume control */
void pcmbuf_fade(bool fade, bool in);
bool pcmbuf_fading(void);
void pcmbuf_soft_mode(bool shhh);

/* Time and position */
unsigned int pcmbuf_get_position_key(void);
void pcmbuf_sync_position_update(void);

/* Misc */
bool pcmbuf_is_lowdata(void);
void pcmbuf_set_low_latency(bool state);
void pcmbuf_update_frequency(void);
unsigned int pcmbuf_get_frequency(void);

#endif /* PCMBUF_H */
