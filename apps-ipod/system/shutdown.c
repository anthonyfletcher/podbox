/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.c (shutdown, events, car adapter)
 * Copyright (C) 2002 by Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * Orderly shutdown and system events: flushing state to disk, the poweroff
 * path, the default event handler, and the car-adapter and
 * headphone-unplug behaviours.
 ****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "pathfuncs.h"
#include "lang.h"
#include "power.h"
#include "powermgmt.h"
#include "backlight.h"
#include "audio.h"
#include "storage.h"
#include "usb_core.h"       /* usb_core_host_wrote_storage */
#include "ata_idle_notify.h"
#include "dircache.h"
#include "sound.h"
#include "debug.h"
#include "rolo.h"
#include "input/action.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"
#include "widgets/list.h"
#include "playlist/playlist.h"
#include "database/tagcache.h"
#include "screens/system/usb_screen.h"
#include "screens/bookmark.h"
#include "screens/playback/wps.h"      /* pause_action, unpause_action */
#include "screens/browse/browser.h"  /* browser_flush, browser_restore */
#include "audio/voice_thread.h" /* voice_wait */
#include "activity.h"
#include "app_util.h"
#include "shutdown.h"

#ifdef BOOTFILE
/* True once this cable session's "before" BOOTFILE mtime has been recorded. */
static bool bootfile_baseline_taken = false;
#endif

static void system_flush(void)
{
    playlist_shutdown();
    browser_flush();
    call_storage_idle_notifys(true); /*doesnt work on usb and shutdown from ata thread */
}

static void system_restore(void)
{
    browser_restore();
}

static bool clean_shutdown(enum shutdown_type sd_type,
                           void (*callback)(void *), void *parameter)
{
    long msg_id = -1;

    if (!global_settings.show_shutdown_message && get_sleep_timer_active())
    {
        talk_force_shutup();
        talk_disable(true);
    }

    status_save(true);

#if CONFIG_CHARGING && !defined(HAVE_POWEROFF_WHILE_CHARGING)
    if(!charger_inserted())
#endif
    {
        bool batt_safe = battery_level_safe();

        FOR_NB_SCREENS(i)
        {
            screens[i].clear_display();
            screens[i].update();
        }

        if (batt_safe)
        {
            int level;
            if (!tagcache_prepare_shutdown())
            {
                cancel_shutdown();
                splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
                return false;
            }
            level = battery_level();
            if (level > 10 || level < 0)
            {
                if (global_settings.show_shutdown_message)
                    splash(0, str(LANG_SHUTTINGDOWN));
            }
            else
            {
                msg_id = LANG_WARNING_BATTERY_LOW;
                splashf(0, "%s %s", str(LANG_WARNING_BATTERY_LOW),
                                    str(LANG_SHUTTINGDOWN));
            }
        }
        else
        {
            msg_id = LANG_WARNING_BATTERY_EMPTY;
            splashf(0, "%s %s", str(LANG_WARNING_BATTERY_EMPTY),
                                str(LANG_SHUTTINGDOWN));
        }

        if (batt_safe) /* do not save on critical battery */
        {
            bookmark_autobookmark(false);

            /* audio_stop_recording == audio_stop for HWCODEC */
            audio_stop();

            if (callback != NULL)
                callback(parameter);

            system_flush();
        }
        else
            dircache_disable();

        if(global_settings.talk_menu)
        {
            bool enqueue = false;
            if(msg_id != -1)
            {
                talk_id(msg_id, enqueue);
                enqueue = true;
            }
            talk_id(LANG_SHUTTINGDOWN, enqueue);
            voice_wait();
        }

        shutdown_hw(sd_type);
    }
    return false;
}

bool list_stop_handler(void)
{
    bool ret = false;


    /* Stop the music if it is playing */
    if(audio_status())
    {
        if (!global_settings.party_mode)
        {
            bookmark_autobookmark(true);
            audio_stop();
            ret = true;  /* bookmarking can make a refresh necessary */
        }
    }
#ifndef HAVE_POWEROFF_WHILE_CHARGING
    {
        static long last_off = 0;

        if (TIME_BEFORE(current_tick, last_off + HZ/2))
        {
            if (charger_inserted())
            {
                charging_splash();
                ret = true;  /* screen is dirty, caller needs to refresh */
            }
        }
        last_off = current_tick;
    }
#endif
    return ret;
}

static bool waiting_to_resume_play = false;
static bool paused_on_unplugged = false;
static long play_resume_tick;

static void car_adapter_mode_processing(bool inserted)
{
    if (global_settings.car_adapter_mode)
    {
        if(inserted)
        {
            /*
             * Just got plugged in, delay & resume if we were playing
             */
            if ((audio_status() & AUDIO_STATUS_PAUSE) && paused_on_unplugged)
            {
                /* delay resume a bit while the engine is cranking */
                play_resume_tick = current_tick + HZ*global_settings.car_adapter_mode_delay;
                waiting_to_resume_play = true;
            }
        }
        else
        {
            /*
             * Just got unplugged, pause if playing
             */
            if ((audio_status() & AUDIO_STATUS_PLAY) &&
                !(audio_status() & AUDIO_STATUS_PAUSE))
            {
                pause_action(true);
                paused_on_unplugged = true;
            }
            else if (!waiting_to_resume_play)
                paused_on_unplugged = false;
            waiting_to_resume_play = false;
        }
    }
}

static void car_adapter_tick(void)
{
    if (waiting_to_resume_play)
    {
        if ((audio_status() & AUDIO_STATUS_PLAY) &&
                !(audio_status() & AUDIO_STATUS_PAUSE))
                waiting_to_resume_play = false;
        if (TIME_AFTER(current_tick, play_resume_tick))
        {
            if (audio_status() & AUDIO_STATUS_PAUSE)
            {
                queue_broadcast(SYS_CAR_ADAPTER_RESUME, 0);
            }
            waiting_to_resume_play = false;
        }
    }
}

void car_adapter_mode_init(void)
{
    tick_add_task(car_adapter_tick);
}

static void hp_unplug_change(bool inserted)
{
    static bool headphone_caused_pause = true;

    if (global_settings.unplug_mode)
    {
        int audio_stat = audio_status();
        if (inserted)
        {
            backlight_on();
            if ((audio_stat & AUDIO_STATUS_PLAY) &&
                    headphone_caused_pause &&
                    global_settings.unplug_mode > 1 )
            {
                unpause_action(true);
            }
            headphone_caused_pause = false;
        } else {
            if ((audio_stat & AUDIO_STATUS_PLAY) &&
                    !(audio_stat & AUDIO_STATUS_PAUSE))
            {
                headphone_caused_pause = true;
                pause_action(false);
            }
        }
    }

}

long default_event_handler_ex(long event, void (*callback)(void *), void *parameter)
{

    switch(event)
    {
        case SYS_BATTERY_UPDATE:
            if(global_settings.talk_battery_level)
            {
                talk_ids(true, VOICE_PAUSE, VOICE_PAUSE,
                         LANG_BATTERY_TIME,
                         TALK_ID(battery_level(), UNIT_PERCENT),
                         VOICE_PAUSE);
                talk_force_enqueue_next();
            }
            break;
        case SYS_USB_CONNECTED:
        {
            intptr_t seqnum = button_get_data();
            if (callback != NULL)
                callback(parameter);
            system_flush();
#ifdef BOOTFILE
            /* Take the "before" mtime once per cable session. Windows
             * unconfigures and reconfigures us mid-connect, so this runs several
             * times per plug-in; re-recording it would move the baseline past a
             * firmware file the host had already replaced, and the comparison
             * below would then see no change. */
            if (!bootfile_baseline_taken)
            {
                bootfile_baseline_taken = true;
                check_bootfile(false); /* gets initial size */
            }
#endif
            gui_usb_screen_run(false, seqnum);
#ifdef BOOTFILE
            /* Only once the cable is really out, and only if the host could have
             * replaced the firmware. Otherwise a mid-connect blip scans
             * /.rockbox for nothing -- and can pop the "Boot changed, reboot
             * now?" prompt while the device is still mounted. */
            if (!usb_inserted())
            {
                bootfile_baseline_taken = false;
                if (usb_core_host_wrote_storage())
                    check_bootfile(true);
            }
#endif
            system_restore();
            return SYS_USB_CONNECTED;
        }

        case SYS_POWEROFF:
        case SYS_REBOOT:
        {
            enum shutdown_type sd_type;
            if (event == SYS_POWEROFF)
                sd_type = SHUTDOWN_POWER_OFF;
            else
                sd_type = SHUTDOWN_REBOOT;

            if (!clean_shutdown(sd_type, callback, parameter))
                return event;
        }
            break;
        case SYS_CHARGER_CONNECTED:
            car_adapter_mode_processing(true);
            return SYS_CHARGER_CONNECTED;

        case SYS_CHARGER_DISCONNECTED:
            car_adapter_mode_processing(false);
            reset_runtime();
            return SYS_CHARGER_DISCONNECTED;

        case SYS_CAR_ADAPTER_RESUME:
            unpause_action(true);
            return SYS_CAR_ADAPTER_RESUME;
        case SYS_PHONE_PLUGGED:
            hp_unplug_change(true);
            return SYS_PHONE_PLUGGED;

        case SYS_PHONE_UNPLUGGED:
            hp_unplug_change(false);
            return SYS_PHONE_UNPLUGGED;
    }
    return 0;
}

long default_event_handler(long event)
{
    return default_event_handler_ex(event, NULL, NULL);
}

#ifdef BOOTFILE
/*
    memorize/compare details about the BOOTFILE
    we don't use dircache because it may not be up to date after
    USB disconnect (scanning in the background)
*/
void check_bootfile(bool do_rolo)
{
    static time_t mtime = 0;
    DIR* dir = NULL;
    struct dirent* entry = NULL;

    /* 1. open BOOTDIR and find the BOOTFILE dir entry */
    dir = opendir(BOOTDIR);

    if(!dir) return; /* do we want an error splash? */

    /* loop all files in BOOTDIR */
    while(0 != (entry = readdir(dir)))
    {
        if(!strcasecmp(entry->d_name, BOOTFILE))
        {
            struct dirinfo info = dir_get_info(dir, entry);
            /* found the bootfile */
            if(mtime && do_rolo)
            {
                if(info.mtime != mtime)
                {
                    static const char *lines[] = { ID2P(LANG_BOOT_CHANGED),
                                                   ID2P(LANG_REBOOT_NOW) };
                    static const struct text_message message={ lines, 2 };
                    button_clear_queue(); /* Empty the keyboard buffer */
                    if(gui_syncyesno_run(&message, NULL, NULL) == YESNO_YES)
                    {
                        audio_hard_stop();
                        rolo_load(BOOTDIR "/" BOOTFILE);
                    }
                }
            }
            mtime = info.mtime;
            break;
        }
    }
    closedir(dir);
}
#endif

