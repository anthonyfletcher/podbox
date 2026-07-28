/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/backdrop.c
 * Copyright (C) 2006 Dave Chapman
 * GNU General Public License (version 2+)
 *
 * Loads a backdrop bitmap from disk and pushes it to the LCD. The plain,
 * non-skin backdrop path.
 ****************************************************************************/

#include <stdio.h>
#include "config.h"
#include "lcd.h"
#include "backdrop.h"

bool backdrop_load(const char* filename, char *backdrop_buffer)
{
    struct bitmap bm;
    int ret;

    /* load the image */
    bm.data = backdrop_buffer;
    ret = read_bmp_file(filename, &bm, LCD_BACKDROP_BYTES,
                        FORMAT_NATIVE | FORMAT_DITHER, NULL);

    return ((ret > 0)
            && (bm.width == LCD_WIDTH) && (bm.height == LCD_HEIGHT));
}
  
  
void backdrop_show(char *backdrop_buffer)
{
    lcd_set_backdrop((fb_data*)backdrop_buffer);
}
  

