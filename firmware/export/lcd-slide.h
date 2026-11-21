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

/* What a screen transition needs from the LCD driver: updates held back
 * while the next screen draws, and a frame sent from two screens at once.
 * The hold is drivers/lcd-slide.c's; lcd_slide_rect() is each driver's. */
#ifndef LCD_SLIDE_H
#define LCD_SLIDE_H

#include <stdbool.h>
#include "lcd.h"

/* Defers every update from the main core for the next `ticks` ticks, or
 * until lcd_release_updates(). Called again, it moves the end and drops
 * what was deferred: the caller is about to send the whole screen. A driver
 * waking its panel ends the hold first. */
void lcd_hold_updates(long ticks);
/* True while a hold is in force and its time is not up. */
bool lcd_updates_held(void);
/* Ends a hold and sends what it deferred, from the framebuffer. */
void lcd_release_updates(void);
/* Ends a hold and drops what it deferred, for a caller about to send the
 * whole screen. */
void lcd_end_hold(void);

/* The driver's hook, first thing in each of its update calls: true when
 * the rectangle was deferred and the caller is to send nothing. A hold
 * whose time is up ends here, sending what it deferred first. */
bool lcd_hold_rect(int x, int y, int width, int height);

/* Sends the rectangle at (x, y), whatever is held, each row's first `split`
 * pixels from `left` at column left_x and the rest from `right` at column
 * right_x. Both are whole screens, rows LCD_WIDTH apart, read on the row
 * the panel's row is on. x, width, split and both columns are even. False
 * when the panel cannot show it, as with the LCD off or the 5G's TV out. */
bool lcd_slide_rect(int x, int y, int width, int height, int split,
                    const fb_data *left, int left_x,
                    const fb_data *right, int right_x);

#endif /* LCD_SLIDE_H */
