/***************************************************************************
 * Original code from RockBox
 * was: apps/audio_thread.h
 * Copyright (C) 2005-2007 Miika Pekkarinen
 * Copyright (C) 2007-2008 Nicolas Pennequin
 * Copyright (C) 2011-2013 Michael Sevakis
 * GNU General Public License (version 2+)
 *
 * Interface to audio_thread.c, plus audio_init()/playback_init().
 ****************************************************************************/
#ifndef AUDIO_THREAD_H
#define AUDIO_THREAD_H

enum
{
    Q_NULL = 0,                 /* reserved */

    /* -> audio */
    Q_AUDIO_PLAY,
    Q_AUDIO_STOP,
    Q_AUDIO_PAUSE,
    Q_AUDIO_SKIP,
    Q_AUDIO_PRE_FF_REWIND,
    Q_AUDIO_FF_REWIND,
    Q_AUDIO_FLUSH,
    Q_AUDIO_DIR_SKIP,

    /* pcmbuf -> audio */
    Q_AUDIO_TRACK_CHANGED,

    /* audio -> audio */
    Q_AUDIO_FILL_BUFFER,        /* continue buffering next track */

    /* buffering -> audio */
    Q_AUDIO_BUFFERING,          /* some buffer event */
    Q_AUDIO_FINISH_LOAD_TRACK,  /* metadata is buffered */
    Q_AUDIO_HANDLE_FINISHED,    /* some other type is buffered */

    /* codec -> audio (*) */
    Q_AUDIO_CODEC_SEEK_COMPLETE,
    Q_AUDIO_CODEC_COMPLETE,

    /* audio -> codec */
    Q_CODEC_LOAD,
    Q_CODEC_RUN,
    Q_CODEC_PAUSE,
    Q_CODEC_SEEK,
    Q_CODEC_STOP,
    Q_CODEC_UNLOAD,

    /* -> codec */
    Q_CODEC_DO_CALLBACK,

    /*- settings -*/

    /* -> audio */
    Q_AUDIO_UPDATE_WATERMARK,   /* buffering watermark needs updating */
    /* -> audio */
    Q_AUDIO_REMAKE_AUDIO_BUFFER, /* buffer needs to be reinitialized */
};

/* (*) If you change these, you must check audio_clear_track_notifications
       in playback.c for correctness */

void audio_init(void);
void playback_init(void);
unsigned int playback_status(void);

void audio_playback_handler(struct queue_event *ev);

/** --- audio_queue helpers --- **/
void audio_queue_post(long id, intptr_t data);
intptr_t audio_queue_send(long id, intptr_t data);

#endif /* AUDIO_THREAD_H */
