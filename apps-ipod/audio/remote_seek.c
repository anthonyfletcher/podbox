/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A seek an accessory drives, over USB iAP or the dock's serial line: the
 * WPS progress bar moves, accelerating as the WPS's own seek does, and one
 * codec seek is made when it ends. A seek per step, or WPS keys, would stop
 * working once the user left the WPS. From Paul Sauro's Gerrit 7906.
 ****************************************************************************/
#include <stdint.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "audio.h"
#include "metadata.h"
#include "settings/settings.h"
#include "playlist/playlist.h"
#include "screens/playback/wps.h"
#include "remote_seek.h"

static struct {
    int direction;          /* 1 forward, -1 back, 0 none */
    int track;              /* playlist_get_display_index() at the start */
    unsigned long position; /* ms */
    unsigned long length;
    long updated;           /* ticks */
    long started;
    unsigned long step;     /* ms */
    long interval;          /* ticks */
} seek;

/* The button repeat rate the WPS sees, 160ms falling to 50ms */
#define FIRST_INTERVAL  (HZ * 16 / 100)
#define LAST_INTERVAL   (HZ * 5 / 100)
#define INTERVAL_STEP   (HZ / 100)

bool remote_seek_start(int direction)
{
    struct mp3entry *id3 = audio_current_track();

    if (!(audio_status() & AUDIO_STATUS_PLAY) || !id3 || !id3->length)
        return false;
    remote_seek_tick();
    if (!seek.direction)
    {
        seek.position = MIN(id3->elapsed, id3->length - 1);
        seek.length = id3->length;
        seek.track = playlist_get_display_index();
        audio_pre_ff_rewind();
    }
    if (seek.direction != direction)
    {
        seek.step = 1000 * global_settings.ff_rewind_min_step;
        seek.interval = FIRST_INTERVAL;
        seek.updated = current_tick;
    }
    seek.direction = direction;
    /* a repeated start renews the watchdog, not the acceleration */
    seek.started = current_tick;
    wps_set_seek_preview(id3, seek.position);
    return true;
}

void remote_seek_tick(void)
{
    if (!seek.direction)
        return;
    if (!(audio_status() & AUDIO_STATUS_PLAY)
        || playlist_get_display_index() != seek.track)
    {
        seek.direction = 0;
        wps_set_seek_preview(NULL, 0);
        return;
    }
    /* A lost end must not leave playback paused for ever */
    if (TIME_AFTER(current_tick, seek.started + 60 * HZ))
    {
        remote_seek_stop();
        return;
    }
    /* Each step 1 + 2^-(accel+3) larger, at most 3% of the way to the end
     * and at least 500ms */
    while (TIME_AFTER(current_tick, seek.updated + seek.interval - 1))
    {
        unsigned long distance = seek.direction > 0 ?
            seek.length - seek.position : seek.position;
        unsigned long max_step = MAX((uint64_t)distance * 3 / 100, 500);
        unsigned long step = MIN(seek.step, max_step);
        long long pos = (long long)seek.position + seek.direction * (long long)step;

        seek.updated += seek.interval;
        if (pos < 0)
            pos = 0;
        if (pos >= (long long)seek.length)
            pos = seek.length - 1;
        seek.position = pos;
        seek.step = step + (step >> (global_settings.ff_rewind_accel + 3));
        if (seek.interval > LAST_INTERVAL)
            seek.interval -= INTERVAL_STEP;
    }
    wps_set_seek_preview(audio_current_track(), seek.position);
}

bool remote_seek_stop(void)
{
    if (!seek.direction)
        return false;
    seek.direction = 0;
    if ((audio_status() & AUDIO_STATUS_PLAY)
        && playlist_get_display_index() == seek.track)
    {
        /* as a WPS seek does, the position is set before the preview goes */
        struct mp3entry *id3 = audio_current_track();
        if (id3)
            id3->elapsed = seek.position;
        audio_ff_rewind(seek.position);
    }
    wps_set_seek_preview(NULL, 0);
    return true;
}

int remote_seek_direction(void)
{
    return seek.direction;
}

unsigned long remote_seek_position(void)
{
    return seek.position;
}
