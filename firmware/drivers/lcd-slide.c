/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
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

/* The hold behind a screen transition (lcd-slide.h): while it is in force,
 * every update the main core asks for is folded into one deferred rectangle
 * and nothing reaches the panel. Only the main core is held, and nothing
 * drawn with interrupts off, so a panic or crash screen always shows. */
#include "config.h"
#include "kernel.h"
#include "system.h"
#include "lcd-slide.h"

static bool held;
static long held_until;
/* What was deferred, [x0,x1) by [y0,y1); empty while x0 >= x1 */
static int x0 = LCD_WIDTH, y0 = LCD_HEIGHT, x1, y1;

static void drop_deferred(void)
{
    x0 = LCD_WIDTH;
    y0 = LCD_HEIGHT;
    x1 = y1 = 0;
}

void lcd_hold_updates(long ticks)
{
    held_until = current_tick + ticks;
    held = true;
    drop_deferred();
}

bool lcd_updates_held(void)
{
    return held && TIME_BEFORE(current_tick, held_until);
}

/* The hold is over before this runs, so the update goes through. From the
 * framebuffer, as the thread whose update ended the hold may have a viewport
 * with a buffer of its own set. */
static void send_deferred(void)
{
    if (x0 >= x1)
        return;
    int x = x0, y = y0, width = x1 - x0, height = y1 - y0;
    drop_deferred();
    struct viewport *vp = lcd_set_viewport(NULL);
    lcd_update_rect(x, y, width, height);
    lcd_set_viewport(vp);
}

void lcd_release_updates(void)
{
    held = false;
    send_deferred();
}

bool lcd_hold_rect(int x, int y, int width, int height)
{
    if (!held)
        return false;
#if NUM_CORES > 1
    if (CURRENT_CORE != CPU)
        return false;
#endif
#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
    if (!irq_enabled())
        return false;
#endif
    if (!TIME_BEFORE(current_tick, held_until))
    {
        lcd_release_updates();
        return false;
    }
    if (width <= 0 || height <= 0)
        return true;
    x0 = MIN(x0, MAX(x, 0));
    y0 = MIN(y0, MAX(y, 0));
    x1 = MAX(x1, MIN(x + width, LCD_WIDTH));
    y1 = MAX(y1, MIN(y + height, LCD_HEIGHT));
    return true;
}
