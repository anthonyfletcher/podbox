/***************************************************************************
 * Original code from RockBox
 * was: apps/alarm_menu.c
 * Copyright (C) 2003 Uwe Freese
 * GNU General Public License (version 2+)
 *
 * The RTC wake-up alarm screen. Sets the alarm time via the shared time
 * picker and arms the hardware RTC, plus the full-screen image shown on the
 * boot that alarm goes on to wake us for.
 ****************************************************************************/
#include "config.h"

#include <stdbool.h>

#include "backlight.h"
#include "bitmaps/podboxalarm.h"
#include "lcd.h"
#include "timefuncs.h"
#include "input/action.h"
#include "kernel.h"
#include <string.h>
#include "settings/settings.h"
#include "power.h"
#include "draw/icon_bitmaps.h"
#include "rtc.h"
#include "time_set.h"
#include "speech/talk.h"
#include "lang.h"
#include "alarm.h"
#include "widgets/splash.h"
#include "draw/viewport.h"

bool alarm_woke_us = false;

/* Called at the top of gui_wps_show(), before the skin renders anything.
 * Playback has already been started by the caller and runs on its own thread,
 * so the music is the alarm and this is just the face on it: the image sits
 * over the screen until a button arrives, then the WPS draws as usual.
 *
 * The wait has to happen here rather than after the WPS is up, because
 * entering the WPS repaints the whole display and would wipe the image.
 * action_userabort() suppresses the status-bar skin flush while it blocks,
 * which is what keeps the image on screen. */
void alarm_show_wake_image(void)
{
    if (!alarm_woke_us)
        return;
    alarm_woke_us = false;

    backlight_on();
    lcd_bmp(&bm_podboxalarm, (LCD_WIDTH - BMPWIDTH_podboxalarm) / 2,
                              (LCD_HEIGHT - BMPHEIGHT_podboxalarm) / 2);
    lcd_update();

    action_userabort(TIMEOUT_BLOCK);
}

int alarm_screen(void)
{
    bool usb, update;
    struct tm *now = get_time();
    struct tm atm;
    memcpy(&atm, now, sizeof(struct tm));
    rtc_get_alarm(&atm.tm_hour, &atm.tm_min);

    /* After a battery change the RTC values are out of range */
    if (!valid_time(&atm))
        memcpy(&atm, now, sizeof(struct tm));
    atm.tm_sec = 0;

    usb = set_time_screen(str(LANG_ALARM_MOD_TIME), &atm, false);
    update = valid_time(&atm); /* set_time returns invalid time if canceled */

    if (!usb && update)
    {

            now = get_time();
            int nmins = now->tm_min + (now->tm_hour * 60);
            int amins = atm.tm_min + (atm.tm_hour * 60);
            int mins_togo = (amins - nmins + 1440) % 1440;
            /* prevent that an alarm occurs in the shutdown procedure */
            /* accept alarms only if they are in 2 minutes or more */
            if (mins_togo > 1) {
                rtc_init();
                rtc_set_alarm(atm.tm_hour,atm.tm_min);
                rtc_enable_alarm(true);
                if (global_settings.talk_menu)
                {
                    talk_id(LANG_ALARM_MOD_TIME_TO_GO, true);
                    talk_value(mins_togo / 60, UNIT_HOUR, true);
                    talk_value(mins_togo % 60, UNIT_MIN, true);
                    talk_force_enqueue_next();
                }
                /* (voiced above) */
                splashf(HZ*2, str(LANG_ALARM_MOD_TIME_TO_GO),
                               mins_togo / 60, mins_togo % 60);
            } else {
                splash(HZ, ID2P(LANG_ALARM_MOD_ERROR));
                update = false;
            }
    }

    if (usb || !update)
    {
        if (!usb)
            splash(HZ*2, ID2P(LANG_ALARM_MOD_DISABLE));
        rtc_enable_alarm(false);
        return 1;
    }
    return 0;
}
