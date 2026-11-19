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

/* The simulator's lcd_slide_rect() (lcd-slide.h): the two parts of the
 * rectangle blitted into the LCD surface from their own screens, then the
 * rectangle shown, as lcd_update_rect() does from the framebuffer. */
#include "config.h"
#include "lcd.h"
#include "lcd-slide.h"
#include "lcd-sdl.h"
#include "sim-ui-defines.h"
#include "system.h"

extern SDL_Surface *lcd_surface;   /* lcd-bitmap.c */

static void blit(const fb_data *screen, int src_x, int x, int y,
                 int width, int height)
{
    if (width <= 0)
        return;
    SDL_Surface *s = SDL_CreateRGBSurfaceFrom((void *)screen, LCD_WIDTH,
                                              LCD_HEIGHT, LCD_DEPTH,
                                              LCD_WIDTH * LCD_DEPTH / 8,
                                              0, 0, 0, 0);
    SDL_Rect src = { src_x, y, width, height };
    SDL_Rect dest = { x, y, width, height };
    SDL_BlitSurface(s, &src, lcd_surface, &dest);
    SDL_FreeSurface(s);
}

bool lcd_slide_rect(int x, int y, int width, int height, int split,
                    const fb_data *left, int left_x,
                    const fb_data *right, int right_x)
{
    if (width <= 0 || height <= 0)
        return true;
    blit(left, left_x, x, y, split, height);
    blit(right, right_x, x + split, y, width - split, height);
    sdl_gui_update(lcd_surface, x, y, width, height + LCD_SPLIT_LINES,
                   SIM_LCD_WIDTH, SIM_LCD_HEIGHT,
                   background ? UI_LCD_POSX : 0, background ? UI_LCD_POSY : 0);
    return true;
}
