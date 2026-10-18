/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Pause on Unplug (unplug_mode: off, pause, pause and resume) for every
 * output. The firmware sends SYS_EVENT_OUTPUT_GONE before playback falls
 * back to the player's own output and SYS_EVENT_OUTPUT_BACK once it is on an
 * external one again; the headphones come from the UI's plug events. A
 * pause made here records which output's going made it and where the track
 * stood, and only that output's return finds playback still paused there and
 * resumes it -- a pause the user made, or one played past since, stays.
 *
 * A going pauses at once, in the sender's thread, before the switch, so no
 * sound reaches the jack; a return resumes through the UI, as the car
 * adapter's resume does.
 ****************************************************************************/
#include <stdint.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "events.h"
#include "audio.h"
#include "pcm.h"
#include "pcm_sink.h"
#include "settings/settings.h"
#include "playlist/playlist.h"
#include "screens/playback/wps.h"   /* pause_action, unpause_action */
#include "backlight.h"
#include "usb_log.h"
#include "output_pause.h"

enum paused_by { BY_NONE, BY_JACK, BY_DAC, BY_ACCESSORY };

static enum paused_by paused_by;
static int paused_index;
static unsigned long paused_elapsed;

static bool playing(void)
{
    int st = audio_status();

    return (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
}

static unsigned long elapsed(void)
{
    const struct mp3entry *id3 = audio_current_track();

    return id3 ? id3->elapsed : 0;
}

static void note_pause(enum paused_by by)
{
    paused_by = by;
    paused_index = playlist_get_display_index();
    paused_elapsed = elapsed();
}

/* Still paused where this output's going left it, give or take Pause
 * Rewind, which may land after the position was noted */
static bool still_ours(enum paused_by by)
{
    unsigned long now = elapsed();
    unsigned long slack = 2000 + global_settings.pause_rewind * 1000;

    return paused_by == by && (audio_status() & AUDIO_STATUS_PAUSE) &&
           playlist_get_display_index() == paused_index &&
           now + slack > paused_elapsed && now < paused_elapsed + slack;
}

/* In the USB log beside the accessories' own lines */
static void note(int what, enum paused_by by, bool acted)
{
    usb_log(USB_LOG_OUTPUT, what, by - BY_JACK, acted,
            global_settings.unplug_mode | audio_status() << 8);
}

static void output_event(unsigned short id, void *data)
{
    enum paused_by by = (intptr_t)data == OUTPUT_DAC ? BY_DAC : BY_ACCESSORY;
    bool acted = false;

    if (id == SYS_EVENT_OUTPUT_GONE)
    {
        if (global_settings.unplug_mode && playing())
        {
            audio_pause();
            note_pause(by);
            acted = true;
        }
        note(USB_LOG_OUTPUT_GONE, by, acted);
    }
    else
    {
        if (global_settings.unplug_mode > 1 && still_ours(by))
        {
            queue_broadcast(SYS_OUTPUT_RESUME, by);
            acted = true;
        }
        note(USB_LOG_OUTPUT_BACK, by, acted);
    }
}

void output_pause_resume(void)
{
    enum paused_by by = paused_by;
    bool acted = by != BY_NONE && still_ours(by);

    if (acted)
        unpause_action(true);
    paused_by = BY_NONE;
    if (by != BY_NONE)
        note(USB_LOG_OUTPUT_RESUME, by, acted);
}

/* Only while the player's own output is playing: a DAC or dock goes on
 * without the headphones */
void output_pause_headphones(bool inserted)
{
    bool acted = false;

    if (!global_settings.unplug_mode)
        return;
    if (inserted)
    {
        backlight_on();
        if (global_settings.unplug_mode > 1 && still_ours(BY_JACK))
        {
            unpause_action(true);
            acted = true;
        }
        if (paused_by == BY_JACK)
            paused_by = BY_NONE;
        note(USB_LOG_OUTPUT_BACK, BY_JACK, acted);
    }
    else
    {
        if (pcm_current_sink() == PCM_SINK_BUILTIN && playing())
        {
            pause_action(false);
            note_pause(BY_JACK);
            acted = true;
        }
        note(USB_LOG_OUTPUT_GONE, BY_JACK, acted);
    }
}

void output_pause_init(void)
{
    add_event(SYS_EVENT_OUTPUT_GONE, output_event);
    add_event(SYS_EVENT_OUTPUT_BACK, output_event);
}
