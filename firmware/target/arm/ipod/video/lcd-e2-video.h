/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Video: screen pushes by the PP's second DMA engine, and the lock
 * that keeps every other BCM port user out while one is under way.
 *
 * Copyright (C) 2026 prokopto-dev
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
#ifndef LCD_E2_VIDEO_H
#define LCD_E2_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include "lcd.h"

#ifndef BOOTLOADER
void lcd_port_init(void);

/* Holds the BCM port for the caller's thread; recursive. false, and nothing
 * held, with interrupts off, on the COP or before lcd_port_init(): the
 * caller then copies by CPU without yielding, and a piece the engine is
 * moving is stopped first. */
bool lcd_port_lock(void);
void lcd_port_unlock(bool locked);
/* True while another thread holds the port, when lcd_port_lock() would
 * wait for it. */
bool lcd_port_held_elsewhere(void);
/* True when lcd_port_unlock(locked) lets go of the port. */
bool lcd_port_outermost(bool locked);

/* Sends a contiguous run to the BCM at bcmaddr by the engine, yielding while
 * each piece moves, and returns the bytes it sent, in whole pieces; the
 * caller sends the rest by CPU. Sends nothing unless playback is playing and
 * the caller is the thread that ran lcd_port_init(), the UI thread: no other
 * thread's LCD update may yield. The port lock is held and the tick
 * blocked. */
size_t lcd_e2_push(unsigned bcmaddr, const fb_data *src, size_t bytes);

/* The same push without the wait, whether or not playback plays: begin
 * starts it, false and nothing sent when the engine cannot take it; poll
 * starts each next piece, true while one moves; end waits, yielding, and
 * returns the bytes sent, in whole pieces. The port lock and the tick are
 * held from begin to end, and src stays as it is. */
bool lcd_e2_begin(unsigned bcmaddr, const fb_data *src, size_t bytes);
bool lcd_e2_poll(void);
size_t lcd_e2_end(void);
#else
#define lcd_port_init()         do { } while (0)
#define lcd_port_lock()         false
#define lcd_port_unlock(l)      ((void)(l))
#define lcd_port_held_elsewhere() false
#define lcd_port_outermost(l)   ((void)(l), false)
#define lcd_e2_push(a, s, n)    ((void)(a), (void)(s), (size_t)0)
#endif

#endif /* LCD_E2_VIDEO_H */
