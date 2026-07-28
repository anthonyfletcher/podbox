/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/backdrop.h
 * Copyright (C) 2006 Dave Chapman
 * GNU General Public License (version 2+)
 *
 * Interface to backdrop.c.
 ****************************************************************************/

#ifndef _BACKDROP_H
#define _BACKDROP_H


#include "lcd.h"
#include "draw/bmp.h"

#define LCD_BACKDROP_BYTES (LCD_FBHEIGHT*LCD_FBWIDTH*sizeof(fb_data))
bool backdrop_load(const char *filename, char* backdrop_buffer);
void backdrop_show(char* backdrop_buffer);



#endif /* _BACKDROP_H */
