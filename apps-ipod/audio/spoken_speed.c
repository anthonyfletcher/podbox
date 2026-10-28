/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Spoken Word Speed: books and podcasts play at the speed the setting
 * names, through the DSP's time-stretch, and music at its own speed. A
 * track is spoken word by its genre, as the shelf decides it.
 *
 * The stretch's buffers come from core_alloc, which restarts the playing
 * track when the audio buffer holds the memory. So they are taken only once
 * a speed other than 100% is chosen -- at boot, before playback, if it was
 * chosen before, or when the setting is changed -- and never from the audio
 * thread. A track change only sets the factor.
 ****************************************************************************/
#include <stdbool.h>
#include "config.h"
#include "audio.h"
#include "metadata.h"
#include "system/appevents.h"
#include "events.h"
#include "dsp_core.h"
#include "tdspeed.h"
#include "settings/settings.h"
#include "database/db_spoken.h"
#include "spoken_speed.h"

static bool is_spoken(const struct mp3entry *id3)
{
    return id3 != NULL && db_spoken_is_spoken_genre(id3->genre_string);
}

bool spoken_speed_playing(void)
{
    return (audio_status() & AUDIO_STATUS_PLAY)
           && is_spoken(audio_current_track());
}

/* The factor for 'id3': a no-op until the buffers are taken */
static void apply(const struct mp3entry *id3)
{
    int pct = is_spoken(id3) ? global_settings.spoken_speed : 100;

    dsp_set_timestretch(pct * PITCH_SPEED_PRECISION);
}

static void track_change(unsigned short id, void *data)
{
    (void)id;
    apply(((struct track_event *)data)->id3);
}

void spoken_speed_set(int pct)
{
    if (pct != 100)
        dsp_timestretch_enable(true);
    apply(audio_current_track());
}

void spoken_speed_init(void)
{
    if (global_settings.spoken_speed != 100)
        dsp_timestretch_enable(true);
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, track_change);
}
