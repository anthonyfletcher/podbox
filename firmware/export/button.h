/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Daniel Stenberg
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#ifndef _BUTTON_H_
#define _BUTTON_H_

#include <stdbool.h>
#include <inttypes.h>
#include "config.h"
#if defined(CHECKWPS) || !defined(__PCTOOL__)
#include "button-target.h"
#endif

#ifndef BUTTON_REMOTE
# define BUTTON_REMOTE 0
#endif

void button_init_device(void);
#ifdef HAVE_BUTTON_DATA
int button_read_device(int *);
#else
int button_read_device(void);
#endif

#ifdef HAS_BUTTON_HOLD
bool button_hold(void);
#endif
#ifdef HAS_REMOTE_BUTTON_HOLD
bool remote_button_hold(void);
#endif

void button_init (void) INIT_ATTR;
void button_close(void);

int button_status(void);
#ifdef HAVE_BUTTON_DATA
int button_status_wdata(int *pdata);
#endif

void button_set_flip(bool flip); /* turn 180 degrees */
#ifdef HAVE_BACKLIGHT
void set_backlight_filter_keypress(bool value);
#ifdef HAVE_REMOTE_LCD
void set_remote_backlight_filter_keypress(bool value);
#endif
#endif

/* button queue functions (in button_queue.c) */
void button_queue_init(void);
void button_queue_post(long id, intptr_t data);
void button_queue_post_remove_head(long id, intptr_t data);
bool button_queue_try_post(long button, int data);
int button_queue_count(void);
bool button_queue_empty(void);
bool button_queue_full(void);
void button_clear_queue(void);
void button_clear_pressed(void);
long button_get(bool block);
long button_get_w_tmo(int ticks);
intptr_t button_get_data(void);

#ifdef HAVE_HEADPHONE_DETECTION
bool headphones_inserted(void);
#endif
#ifdef HAVE_LINEOUT_DETECTION
bool lineout_inserted(void);
#endif
#ifdef HAVE_WHEEL_POSITION
int wheel_status(void);
void wheel_send_events(bool send);
#endif

#ifdef HAVE_WHEEL_ACCELERATION
int button_apply_acceleration(const unsigned int data);
#endif

#define BUTTON_NONE         0x00000000

/* Button modifiers */
#define BUTTON_REL          0x02000000
#define BUTTON_REPEAT       0x04000000
/* Special buttons */
#define BUTTON_TOUCHSCREEN  0x08000000
#define BUTTON_MULTIMEDIA   0x10000000
#define BUTTON_REDRAW       0x20000000

#define BUTTON_MULTIMEDIA_PLAYPAUSE (BUTTON_MULTIMEDIA|0x01)
#define BUTTON_MULTIMEDIA_STOP      (BUTTON_MULTIMEDIA|0x02)
#define BUTTON_MULTIMEDIA_PREV      (BUTTON_MULTIMEDIA|0x04)
#define BUTTON_MULTIMEDIA_NEXT      (BUTTON_MULTIMEDIA|0x08)
#define BUTTON_MULTIMEDIA_REW       (BUTTON_MULTIMEDIA|0x10)
#define BUTTON_MULTIMEDIA_FFWD      (BUTTON_MULTIMEDIA|0x20)
#define BUTTON_MULTIMEDIA_VOLUME_UP   (BUTTON_MULTIMEDIA|0x40)
#define BUTTON_MULTIMEDIA_VOLUME_DOWN (BUTTON_MULTIMEDIA|0x80)

#define BUTTON_MULTIMEDIA_ALL       (BUTTON_MULTIMEDIA_PLAYPAUSE| \
                                     BUTTON_MULTIMEDIA_STOP| \
                                     BUTTON_MULTIMEDIA_PREV| \
                                     BUTTON_MULTIMEDIA_NEXT| \
                                     BUTTON_MULTIMEDIA_REW | \
                                     BUTTON_MULTIMEDIA_FFWD| \
                                     BUTTON_MULTIMEDIA_VOLUME_UP| \
                                     BUTTON_MULTIMEDIA_VOLUME_DOWN)

#ifdef HAVE_MIKEY_REMOTE
/* The inline earphone remote's centre button. On, two clicks report
 * BUTTON_MULTIMEDIA_NEXT and three BUTTON_MULTIMEDIA_PREV, and a single
 * click waits out the multi-click window before reporting PLAYPAUSE; off,
 * every click reports PLAYPAUSE at once. Declared here rather than in
 * mikey-target.h because the caller is settings_apply(). */
void mikey_set_track_skip(bool enable);

/* Whether this board can have a remote at all. False on capture hardware
 * version 0 -- the 80GB and fat 160GB -- which has no jack microphone line
 * and carries no Mikey on the bus, so the poller is never even started
 * there. Answers from boot, unlike mikey_present(), which cannot latch
 * until something occupies the jack. */
bool mikey_supported(void);

/* The register scope behind Debug > Mikey remote. While it is on, the polling
 * thread reads every register each poll instead of just reg0, reg4 and reg5,
 * and decodes the buttons from that same read -- a second reader of reg5
 * would take its events away from the remote. Each changed value goes into
 * the log ring, as does each change in what the driver reports, under
 * MIKEY_SCOPE_OUT with the MIKEY_OUT_* bits. Off, regs[] still carries the
 * thread's own reg0, reg4 and reg5. */
#define MIKEY_SCOPE_REGS    8       /* the chip mirrors these every 8 */
#define MIKEY_SCOPE_LOG     128     /* a power of two */
#define MIKEY_SCOPE_OUT     0xff

#define MIKEY_OUT_PLAY      0x01
#define MIKEY_OUT_NEXT      0x02
#define MIKEY_OUT_PREV      0x04
#define MIKEY_OUT_VOLUP     0x08
#define MIKEY_OUT_VOLDN     0x10

struct mikey_scope_change {
    long tick;
    unsigned char reg, from, to;
};

struct mikey_scope {
    bool on;
    unsigned char regs[MIKEY_SCOPE_REGS];
    unsigned char seen[MIKEY_SCOPE_REGS];   /* OR of every value since clear */
    unsigned long nak;                      /* bit per reg: last read NAKed */
    unsigned char out;                      /* MIKEY_OUT_* reported now */
    unsigned long sweeps;
    long sweep_ticks;                       /* how long the last one took */
    unsigned long head;                     /* changes ever logged */
    struct mikey_scope_change log[MIKEY_SCOPE_LOG];
};

void mikey_scope_enable(bool on);
void mikey_scope_clear(void);              /* seen[] only */
const struct mikey_scope *mikey_scope_get(void);
#endif

#ifdef HAVE_TOUCHSCREEN
long touchscreen_last_touch(void);

#if (!defined(BUTTON_TOPLEFT) || !defined(BUTTON_TOPMIDDLE) \
 || !defined(BUTTON_TOPRIGHT) || !defined(BUTTON_MIDLEFT) \
 || !defined(BUTTON_CENTER) || !defined(BUTTON_MIDRIGHT) \
 || !defined(BUTTON_BOTTOMLEFT) || !defined(BUTTON_BOTTOMMIDDLE) \
 || !defined(BUTTON_BOTTOMRIGHT)) && !defined(__PCTOOL__)
#error Touchscreen button mode BUTTON_* defines not set up correctly
#endif

#include "touchscreen.h"
#endif

#ifdef HAVE_TOUCHPAD
#include "touchpad.h"
#endif

#if (defined(HAVE_TOUCHPAD) || defined(HAVE_TOUCHSCREEN)) && !defined(HAS_BUTTON_HOLD)
void button_enable_touch(bool en);
#endif

#ifdef HAVE_SW_POWEROFF
void button_set_sw_poweroff_state(bool en);
bool button_get_sw_poweroff_state(void);
#endif

#endif /* _BUTTON_H_ */
