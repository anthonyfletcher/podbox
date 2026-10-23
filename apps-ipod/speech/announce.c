/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Saying each new track while Announce Track Change is on: over the music,
 * or pausing it for as long as the announcement lasts. Only a track that
 * follows on by itself is paused for: a manual skip is still under way when
 * its track change is heard, and pausing into it is left alone.
 *
 * The track change and the end of speech are both noticed on other threads
 * (the audio and voice threads), neither of which may pause playback or run
 * the speech front end, so each posts an event to the UI thread and the
 * work happens there.
 ****************************************************************************/

#include <stdio.h>

#include "config.h"
#include "button.h"
#include "audio.h"
#include "metadata.h"
#include "settings/settings.h"
#include "system/appevents.h"
#include "metadata/tag_trim.h"
#include "audio/voice_thread.h"
#include "talk.h"
#include "announce.h"

static bool paused_for_us;      /* this announcement paused the music */

void announce_track(bool enqueue)
{
    struct mp3entry *id3 = audio_current_track();
    char trimmed[MAX_PATH], text[2 * MAX_PATH];
    const char *title;

    if (!global_settings.talk_menu || !id3)
        return;
    title = id3->title ? id3->title : id3->path;
    title = tag_trim(title, trimmed, sizeof(trimmed));
    if (id3->artist && id3->artist[0])
        snprintf(text, sizeof(text), "%s, %s", title, id3->artist);
    else
        snprintf(text, sizeof(text), "%s", title);
    talk_name(text, enqueue);
}

/* On the audio thread */
static void track_changed(unsigned short id, void *data)
{
    const struct track_event *te = data;
    (void)id;
    if (global_settings.talk_track_change != ANNOUNCE_OFF
        && global_settings.talk_menu)
        button_queue_post(SYS_ANNOUNCE_TRACK,
                          (te->flags & TEF_AUTO_SKIP) ? 1 : 0);
}

/* On the voice thread */
static void voice_changed(unsigned short id, void *data)
{
    (void)id;
    if (paused_for_us && !*(bool *)data)
        button_queue_post(SYS_ANNOUNCE_DONE, 0);
}

void announce_event(long event, intptr_t data)
{
    if (event == SYS_ANNOUNCE_TRACK)
    {
        int status = audio_status();
        if (global_settings.talk_track_change == ANNOUNCE_PAUSE_MUSIC
            && data /* followed on by itself */
            && (status & AUDIO_STATUS_PLAY) && !(status & AUDIO_STATUS_PAUSE))
        {
            audio_pause();
            paused_for_us = true;
        }
        announce_track(true);
        if (paused_for_us && !voice_is_playing())
        {
            /* nothing could be said, so nothing will end */
            paused_for_us = false;
            audio_resume();
        }
    }
    else if (event == SYS_ANNOUNCE_DONE && paused_for_us)
    {
        /* resumed only if nobody has touched the pause since */
        paused_for_us = false;
        if (audio_status() & AUDIO_STATUS_PAUSE)
            audio_resume();
    }
}

void announce_init(void)
{
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, track_changed);
    add_event(VOICE_EVENT_IS_PLAYING, voice_changed);
}
