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

#include <stdbool.h>
#include <stdint.h>
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

/* Analysis tap: read committed-but-unplayed PCM without consuming it. What
 * the codec has decoded but the DAC has not reached yet is the only source
 * of audio the player has not heard, so this is where anything wanting to
 * react ahead of the sound reads. */
struct pcmbuf_peek
{
    const int16_t *pcm;     /* Interleaved stereo, two int16 per frame */
    int            frames;  /* Frames available at pcm */
    unsigned long  elapsed; /* Track time of the first frame, carried only */
    unsigned int   pos_key; /* where pos_key is non-zero (see stamp_chunk) */
};

/* Seed a peek cursor at the play position. */
size_t pcmbuf_peek_start(void);

/* Hand back the committed chunk at *index and step *index past it. False
 * once the cursor reaches the codec, and false if it no longer points into
 * committed data -- playback moved, or the codec lapped it -- in which case
 * reseed rather than trusting the cursor. */
bool pcmbuf_peek_next(size_t *index, struct pcmbuf_peek *peek);

/* Whether a cursor that returned nothing is merely caught up with the codec
 * rather than stale. This is the difference between waiting and reseeding,
 * and pcmbuf_peek_next() alone cannot say which. */
bool pcmbuf_peek_valid(size_t index);

/* Milliseconds of audio between the play position and 'index'. */
unsigned int pcmbuf_peek_lead_ms(size_t index);

/* Milliseconds of committed-but-unplayed audio: the look-ahead available. */
unsigned int pcmbuf_lookahead_ms(void);

/* Misc */
bool pcmbuf_is_lowdata(void);
void pcmbuf_set_low_latency(bool state);
void pcmbuf_update_frequency(void);
unsigned int pcmbuf_get_frequency(void);

#endif /* PCMBUF_H */
