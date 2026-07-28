/***************************************************************************
 * Original code from RockBox
 * was: apps/audio_thread.c
 * Copyright (C) 2005-2007 Miika Pekkarinen
 * Copyright (C) 2007-2008 Nicolas Pennequin
 * Copyright (C) 2011-2013 Michael Sevakis
 * GNU General Public License (version 2+)
 *
 * The audio thread's event loop. Receives queued playback commands and
 * dispatches them into playback.c; owns the audio status.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "logf.h"
#include "usb.h"
#include "pcm.h"
#include "sound.h"
#include "pcmbuf.h"
#include "system/appevents.h"
#include "audio_thread.h"
#include "codec_thread.h"
#include "voice_thread.h"
#include "speech/talk.h"
#include "settings/settings.h"

/* Macros to enable logf for queues
   logging on SYS_TIMEOUT can be disabled */

#ifdef AUDIO_LOGQUEUES
#define LOGFQUEUE logf
#else
#define LOGFQUEUE(...)
#endif

bool audio_is_initialized = false;

/* Event queues */
struct event_queue audio_queue SHAREDBSS_ATTR;
static struct queue_sender_list audio_queue_sender_list SHAREDBSS_ATTR;

/* Audio thread */
static long audio_stack[(DEFAULT_STACK_SIZE + 0x1000)/sizeof(long)];
static const char audio_thread_name[] = "audio";
unsigned int audio_thread_id = 0;

static void NORETURN_ATTR audio_thread(void)
{
    struct queue_event ev;
    /* The loop dispatches before it waits, so seed ev with an id no case
     * matches: the first pass falls straight through to queue_wait(). Cases
     * that `continue` re-dispatch the same event without waiting. */
    ev.id = Q_NULL;

    pcm_postinit();

    while (1)
    {
        switch (ev.id)
        {
        /* Starts the playback engine branch */
        case Q_AUDIO_PLAY:
            LOGFQUEUE("audio < Q_AUDIO_PLAY");
            audio_playback_handler(&ev);
            continue;

        /* Playback has to handle these, even if not playing */
        case Q_AUDIO_REMAKE_AUDIO_BUFFER:
        case Q_AUDIO_UPDATE_WATERMARK:
            audio_playback_handler(&ev);
            break;

        /* All return upon USB */
        case SYS_USB_CONNECTED:
            LOGFQUEUE("audio < SYS_USB_CONNECTED");
            voice_stop();
            usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
            usb_wait_for_disconnect(&audio_queue);
            break;
        }

        queue_wait(&audio_queue, &ev);
    }
}

void audio_voice_event(unsigned short id, void *data)
{
    (void)id;
    /* Make audio play softly while voice is speaking */
    pcmbuf_soft_mode(*(bool *)data);
}

void audio_queue_post(long id, intptr_t data)
{
    queue_post(&audio_queue, id, data);
}

intptr_t audio_queue_send(long id, intptr_t data)
{
    return queue_send(&audio_queue, id, data);
}

int audio_status(void)
{
    return playback_status();
}

/* Empty: the error state this cleared belonged to recording, which these
 * targets do not have. Declared in firmware/export/audio.h; no callers remain. */
void audio_error_clear(void)
{
}

/** Startup **/

/* Initialize the audio system - called from init() in main.c */
void INIT_ATTR audio_init(void)
{
    /* Can never do this twice */
    if (audio_is_initialized)
    {
        logf("audio: already initialized");
        return;
    }

    logf("audio: initializing");

    /* Initialize queues before giving control elsewhere in case it likes
       to send messages. Thread creation will be delayed however so nothing
       starts running until ready if something yields such as talk_init. */
    queue_init(&audio_queue, true);
    codec_thread_init();

    /* This thread does buffer, so match its priority */
    audio_thread_id = create_thread(audio_thread, audio_stack,
                  sizeof(audio_stack), 0, audio_thread_name
                  IF_PRIO(, MIN(PRIORITY_BUFFERING, PRIORITY_USER_INTERFACE))
                  IF_COP(, CPU));

    queue_enable_queue_send(&audio_queue, &audio_queue_sender_list,
                            audio_thread_id);

    playback_init();

    add_event(VOICE_EVENT_IS_PLAYING, audio_voice_event);

   /* Probably safe to say */
    audio_is_initialized = true;

    sound_settings_apply();
}
