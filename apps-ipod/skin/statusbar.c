/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/statusbar.c
 * Copyright (C) Robert E. Hak (2002), Linus Nielsen Feltzing (2002)
 * GNU General Public License (version 2+)
 *
 * The built-in (non-skinned) status bar: battery, volume, playback mode,
 * time and disk activity drawn directly.
 ****************************************************************************/
#include <stdio.h>
#include "config.h"
#include "font.h"
#include "kernel.h"
#include "string.h" /* for memcmp oO*/
#include "string-extra.h" /* for itoa */
#include "sound.h"
#include "settings/settings.h"
#include "draw/viewport.h"
#include "metadata.h"
#include "draw/icon_bitmaps.h"
#include "powermgmt.h"
#include "usb.h"
#include "led.h"
#include "draw/screen_access.h"

#include "audio/play_status.h" /* needed for battery_state global var */
#include "input/action.h" /* for keys_locked */
#include "statusbar.h"
#include "system/appevents.h"
#include "timefuncs.h"

/* FIXME: should be removed from icon.h to avoid redefinition,
   but still needed for compatibility with old system */
#define ICONS_SPACING                           2
#define STATUSBAR_BATTERY_X_POS                 0*ICONS_SPACING
#define STATUSBAR_BATTERY_WIDTH                 (2+(2*SYSFONT_WIDTH))
#define STATUSBAR_PLUG_X_POS                    STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                ICONS_SPACING
#define STATUSBAR_BATTERY_HEIGHT                SB_ICON_HEIGHT - 1
#define STATUSBAR_PLUG_WIDTH                    7
#define STATUSBAR_VOLUME_X_POS                  STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                2*ICONS_SPACING
#define STATUSBAR_VOLUME_WIDTH                  (2+(2*SYSFONT_WIDTH))
#define STATUSBAR_ENCODER_X_POS                 STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                2*ICONS_SPACING - 1
#define STATUSBAR_ENCODER_WIDTH                 18
#define STATUSBAR_PLAY_STATE_X_POS              STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                STATUSBAR_VOLUME_WIDTH + \
                                                3*ICONS_SPACING
#define STATUSBAR_PLAY_STATE_WIDTH              7
#define STATUSBAR_PLAY_MODE_X_POS               STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                STATUSBAR_VOLUME_WIDTH + \
                                                STATUSBAR_PLAY_STATE_WIDTH + \
                                                4*ICONS_SPACING
#define STATUSBAR_PLAY_MODE_WIDTH               7
#define STATUSBAR_RECFREQ_X_POS                 STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                STATUSBAR_VOLUME_WIDTH + \
                                                STATUSBAR_PLAY_STATE_WIDTH + \
                                                3*ICONS_SPACING
#define STATUSBAR_RECFREQ_WIDTH                 12
#define STATUSBAR_RECCHANNELS_X_POS             STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                STATUSBAR_VOLUME_WIDTH + \
                                                STATUSBAR_PLAY_STATE_WIDTH + \
                                                STATUSBAR_RECFREQ_WIDTH + \
                                                4*ICONS_SPACING
#define STATUSBAR_RECCHANNELS_WIDTH             5
#define STATUSBAR_SHUFFLE_X_POS                 STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                STATUSBAR_VOLUME_WIDTH + \
                                                STATUSBAR_PLAY_STATE_WIDTH + \
                                                STATUSBAR_PLAY_MODE_WIDTH + \
                                                5*ICONS_SPACING
#define STATUSBAR_SHUFFLE_WIDTH                 7
#define STATUSBAR_LOCKM_X_POS                   STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                STATUSBAR_VOLUME_WIDTH + \
                                                STATUSBAR_PLAY_STATE_WIDTH + \
                                                STATUSBAR_PLAY_MODE_WIDTH + \
                                                STATUSBAR_SHUFFLE_WIDTH + \
                                                6*ICONS_SPACING
#define STATUSBAR_LOCKM_WIDTH                   5
#define STATUSBAR_LOCKR_X_POS                   STATUSBAR_X_POS + \
                                                STATUSBAR_BATTERY_WIDTH + \
                                                STATUSBAR_PLUG_WIDTH + \
                                                STATUSBAR_VOLUME_WIDTH + \
                                                STATUSBAR_PLAY_STATE_WIDTH + \
                                                STATUSBAR_PLAY_MODE_WIDTH + \
                                                STATUSBAR_SHUFFLE_WIDTH + \
                                                STATUSBAR_LOCKM_WIDTH + \
                                                7*ICONS_SPACING
#define STATUSBAR_LOCKR_WIDTH                   5

#define STATUSBAR_DISK_WIDTH                    12
#define STATUSBAR_DISK_X_POS(statusbar_width)   statusbar_width - \
                                                STATUSBAR_DISK_WIDTH
#define STATUSBAR_TIME_X_END(statusbar_width)   statusbar_width - 1 - \
                                                STATUSBAR_DISK_WIDTH
struct gui_syncstatusbar statusbars;

/* Prototypes */
static void gui_statusbar_icon_battery(struct screen * display, int percent,
                                       int batt_charge_step);
static bool gui_statusbar_icon_volume(struct gui_statusbar * bar, int volume);
static void gui_statusbar_icon_play_state(struct screen * display, int state);
static void gui_statusbar_icon_play_mode(struct screen * display, int mode);
static void gui_statusbar_icon_shuffle(struct screen * display);
static void gui_statusbar_icon_lock(struct screen * display);
static void gui_statusbar_led(struct screen * display);
static void gui_statusbar_time(struct screen * display, struct tm *time);

/* End prototypes */


/*
 * Initializes a status bar
 *  - bar : the bar to initialize
 */
static void gui_statusbar_init(struct screen * display, struct gui_statusbar * bar)
{
    bar->display = display;
    bar->redraw_volume = true;
    bar->volume_icon_switch_tick = bar->battery_icon_switch_tick = current_tick;
    memset((void*)&(bar->lastinfo), 0, sizeof(struct status_info));
    bar->last_tm_min = 0;
}

static struct screen * sb_fill_bar_info(struct gui_statusbar * bar)
{
    struct screen *display = bar->display;

    if (!display)
        return display;

    bar->info.battlevel = battery_level();
    bar->info.usb_inserted = usb_inserted();
    bar->info.inserted = (charger_input_state == CHARGER);
    if (bar->info.inserted)
    {
        bar->info.battery_state = true;


        /* zero battery run time if charging */
        if (charge_state > DISCHARGING)
            zero_runtime();

        /* animate battery if charging */
        if ((charge_state == DISCHARGING) || (charge_state == TRICKLE))
        {
            bar->info.batt_charge_step = -1;
        }
        else
        {
            /* animate in (max.) 4 steps, starting near the current charge level */
            if (TIME_AFTER(current_tick, bar->battery_icon_switch_tick))
            {
                if (++bar->info.batt_charge_step > 3)
                    bar->info.batt_charge_step = bar->info.battlevel / 34;
                bar->battery_icon_switch_tick = current_tick + HZ;
            }
        }
    }
    else
    {
        bar->info.batt_charge_step = -1;
        if (battery_level_safe())
            bar->info.battery_state = true;
        else
            /* blink battery if level is low */
            if (TIME_AFTER(current_tick, bar->battery_icon_switch_tick) &&
               (bar->info.battlevel > -1))
            {
                bar->info.battery_state = !bar->info.battery_state;
                bar->battery_icon_switch_tick = current_tick + HZ;
            }
    }
    bar->info.volume = global_status.volume;
    bar->info.shuffle = global_settings.playlist_shuffle;
    bar->info.keylock = button_hold();
    bar->info.repeat = global_settings.repeat_mode;
    bar->info.playmode = current_playmode();
    bar->time = get_time();
    if(!display->has_disk_led)
        bar->info.led = led_read(HZ/2); /* delay should match polling interval */

    return display;
}

void gui_statusbar_draw(struct gui_statusbar * bar, bool force_redraw, struct viewport *vp)
{
    struct viewport *last_vp = NULL;
    struct screen * display = sb_fill_bar_info(bar);
    if (!display)
        return;

    /* only redraw if forced to, or info has changed */
    if (force_redraw || bar->redraw_volume ||
        (bar->time->tm_min != bar->last_tm_min) ||
        memcmp(&(bar->info), &(bar->lastinfo), sizeof(struct status_info)))
    {
        last_vp = display->set_viewport(vp);
        display->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
        display->fill_viewport();
        display->set_drawmode(DRMODE_SOLID);
        display->setfont(FONT_SYSFIXED);

        if (bar->info.battery_state)
            gui_statusbar_icon_battery(display, bar->info.battlevel,
                                       bar->info.batt_charge_step);
        if (bar->info.usb_inserted)
            display->mono_bitmap(bitmap_icons_7x8[Icon_USBPlug],
                                 STATUSBAR_PLUG_X_POS,
                                 STATUSBAR_Y_POS, STATUSBAR_PLUG_WIDTH,
                                 SB_ICON_HEIGHT);
        else
        /* draw power plug if charging */
        if (bar->info.inserted)
            display->mono_bitmap(bitmap_icons_7x8[Icon_Plug],
                                    STATUSBAR_PLUG_X_POS,
                                    STATUSBAR_Y_POS, STATUSBAR_PLUG_WIDTH,
                                    SB_ICON_HEIGHT);
        bar->redraw_volume = gui_statusbar_icon_volume(bar, bar->info.volume);
        gui_statusbar_icon_play_state(display, current_playmode() + Icon_Play);

        {
            gui_statusbar_icon_play_mode(display, bar->info.repeat);

            if (bar->info.shuffle)
                gui_statusbar_icon_shuffle(display);
        }
        if (bar->info.keylock)
            gui_statusbar_icon_lock(display);
        gui_statusbar_time(display, bar->time);
        bar->last_tm_min = bar->time->tm_min;
        if(!display->has_disk_led && bar->info.led)
        {
            gui_statusbar_led(display);
        }
        display->setfont(FONT_UI);
        display->update_viewport();
        display->set_viewport(last_vp);
        bar->lastinfo = bar->info;
    }
}

/* from icon.c */
/*
 * Print battery icon to status bar
 */
static void gui_statusbar_icon_battery(struct screen * display, int percent,
                                       int batt_charge_step)
{
    int fill, endfill;
    char buffer[5];
    unsigned int width, height;
    unsigned int prevfg = 0;

    if (batt_charge_step >= 0)
    {
        fill = percent * (STATUSBAR_BATTERY_WIDTH-3) / 100;
        endfill = 34 * batt_charge_step * (STATUSBAR_BATTERY_WIDTH-3) / 100;
    }
    else
    {
        fill = endfill = (percent * (STATUSBAR_BATTERY_WIDTH-3) + 50) / 100;
    }

    /* Certain charge controlled targets */
    /* show graphical animation when charging instead of numbers */
    if ((global_settings.battery_display) &&
        (charge_state != CHARGING) &&
        (percent > -1) &&
        (percent <= 100)) {
        /* Numeric display */
        snprintf(buffer, sizeof(buffer), "%3d", percent);
        font_getstringsize(buffer, &width, &height, FONT_SYSFIXED);
        if (height <= STATUSBAR_HEIGHT) {
             display->putsxy(STATUSBAR_BATTERY_X_POS
                             + STATUSBAR_BATTERY_WIDTH / 2
                             - width/2, STATUSBAR_Y_POS, buffer);
        }

    }
    else {
        /* draw battery */
        display->drawrect(STATUSBAR_BATTERY_X_POS, STATUSBAR_Y_POS,
                          STATUSBAR_BATTERY_WIDTH - 1, STATUSBAR_BATTERY_HEIGHT);
        display->vline(STATUSBAR_BATTERY_X_POS + STATUSBAR_BATTERY_WIDTH - 1,
                       STATUSBAR_Y_POS + 2, STATUSBAR_Y_POS + 4);

        display->fillrect(STATUSBAR_BATTERY_X_POS + 1, STATUSBAR_Y_POS + 1,
                          fill, STATUSBAR_BATTERY_HEIGHT - 2);
        if (display->depth > 1)
        {
            prevfg = display->get_foreground();
            display->set_foreground(LCD_DARKGRAY);
        }
        display->fillrect(STATUSBAR_BATTERY_X_POS + 1 + fill, STATUSBAR_Y_POS + 1,
                          endfill - fill, STATUSBAR_BATTERY_HEIGHT - 2);
        if (display->depth > 1)
            display->set_foreground(prevfg);
    }

    if (percent == -1 || percent > 100) {
        display->putsxy(STATUSBAR_BATTERY_X_POS + STATUSBAR_BATTERY_WIDTH / 2
                          - 4, STATUSBAR_Y_POS, "?");
    }
}

/*
 * Print volume gauge to status bar
 */
static bool gui_statusbar_icon_volume(struct gui_statusbar * bar, int volume)
{
    int i;
    int vol;
    char buffer[4];
    unsigned int width, height;
    bool needs_redraw = false;
    int type = global_settings.volume_type;
    struct screen * display=bar->display;
    const int minvol = sound_min(SOUND_VOLUME);

    if (volume < minvol)
        volume = minvol;

    if (volume == minvol) {
        display->mono_bitmap(bitmap_icons_7x8[Icon_Mute],
                    STATUSBAR_VOLUME_X_POS + STATUSBAR_VOLUME_WIDTH / 2 - 4,
                    STATUSBAR_Y_POS, 7, SB_ICON_HEIGHT);
    }
    else {
        const int maxvol = sound_max(SOUND_VOLUME);

        if (volume > maxvol)
            volume = maxvol;
        /* We want to redraw the icon later on */
        if (bar->last_volume != volume && bar->last_volume >= minvol) {
            bar->volume_icon_switch_tick = current_tick + HZ;
        }

        /* If the timeout hasn't yet been reached, we show it numerically
           and tell the caller that we want to be called again */
        if (TIME_BEFORE(current_tick,bar->volume_icon_switch_tick)) {
            type = 1;
            needs_redraw = true;
        }

        /* display volume level numerical? */
        if (type)
        {
            const int num_decimals = sound_numdecimals(SOUND_VOLUME);
            if (num_decimals)
                volume /= 10 * num_decimals;

            snprintf(buffer, sizeof(buffer), "%2d", volume);
            font_getstringsize(buffer, &width, &height, FONT_SYSFIXED);
            if (height <= STATUSBAR_HEIGHT) {
                display->putsxy(STATUSBAR_VOLUME_X_POS
                                  + STATUSBAR_VOLUME_WIDTH / 2
                                  - width/2, STATUSBAR_Y_POS, buffer);
            }
        } else {
            /* display volume bar */
            vol = (volume - minvol) * 14 / (maxvol - minvol);
            for(i=0; i < vol; i++) {
                display->vline(STATUSBAR_VOLUME_X_POS + i,
                               STATUSBAR_Y_POS + 6 - i / 2,
                               STATUSBAR_Y_POS + 6);
            }
        }
    }
    bar->last_volume = volume;

    return needs_redraw;
}

/*
 * Print play state to status bar
 */
static void gui_statusbar_icon_play_state(struct screen * display, int state)
{
    display->mono_bitmap(bitmap_icons_7x8[state], STATUSBAR_PLAY_STATE_X_POS,
                    STATUSBAR_Y_POS, STATUSBAR_PLAY_STATE_WIDTH,
                    SB_ICON_HEIGHT);
}

/*
 * Print play mode to status bar
 */
static void gui_statusbar_icon_play_mode(struct screen * display, int mode)
{
    switch (mode) {
        case REPEAT_AB:
            display->mono_bitmap(bitmap_icons_7x8[Icon_RepeatAB],
                                 STATUSBAR_PLAY_MODE_X_POS,
                                 STATUSBAR_Y_POS, STATUSBAR_PLAY_MODE_WIDTH,
                                 SB_ICON_HEIGHT);
            break;

        case REPEAT_ONE:
            display->mono_bitmap(bitmap_icons_7x8[Icon_RepeatOne],
                                 STATUSBAR_PLAY_MODE_X_POS,
                                 STATUSBAR_Y_POS, STATUSBAR_PLAY_MODE_WIDTH,
                                 SB_ICON_HEIGHT);
            break;

        case REPEAT_ALL:
        case REPEAT_SHUFFLE:
            display->mono_bitmap(bitmap_icons_7x8[Icon_Repeat],
                                 STATUSBAR_PLAY_MODE_X_POS,
                                 STATUSBAR_Y_POS, STATUSBAR_PLAY_MODE_WIDTH,
                                 SB_ICON_HEIGHT);
            break;
    }
}
/*
 * Print shuffle mode to status bar
 */
static void gui_statusbar_icon_shuffle(struct screen * display)
{
    display->mono_bitmap(bitmap_icons_7x8[Icon_Shuffle],
                    STATUSBAR_SHUFFLE_X_POS, STATUSBAR_Y_POS,
                    STATUSBAR_SHUFFLE_WIDTH, SB_ICON_HEIGHT);
}

/*
 * Print lock when keys are locked
 */
static void gui_statusbar_icon_lock(struct screen * display)
{
    display->mono_bitmap(bitmap_icons_5x8[Icon_Lock_Main],
                         STATUSBAR_LOCKM_X_POS, STATUSBAR_Y_POS,
                         STATUSBAR_LOCKM_WIDTH, SB_ICON_HEIGHT);
}


/*
 * no real LED: disk activity in status bar
 */
static void gui_statusbar_led(struct screen * display)
{
    display->mono_bitmap(bitmap_icon_disk,
                         STATUSBAR_DISK_X_POS(display->getwidth()),
                         STATUSBAR_Y_POS, STATUSBAR_DISK_WIDTH,
                         SB_ICON_HEIGHT);
}

/*
 * Print time to status bar
 */
static void gui_statusbar_time(struct screen * display, struct tm *time)
{
    unsigned char buffer[6];
    const unsigned char *p = buffer;
    unsigned int width, height;
    int hour, minute;
    if ( valid_time(time) ) {
        hour = time->tm_hour;
        minute = time->tm_min;
        if ( global_settings.timeformat ) { /* 12 hour clock */
            hour %= 12;
            if ( hour == 0 ) {
                hour += 12;
            }
        }
        snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);
    }
    else {
        p = "--:--";
    }

    font_getstringsize(p, &width, &height, FONT_SYSFIXED);
    if (height <= STATUSBAR_HEIGHT) {
        display->putsxy(STATUSBAR_TIME_X_END(display->getwidth()) - width,
                        STATUSBAR_Y_POS, p);
    }

}


void gui_syncstatusbar_init(struct gui_syncstatusbar * bars)
{
    FOR_NB_SCREENS(i) {
        gui_statusbar_init(&(screens[i]), &(bars->statusbars[i]));
    }
}


