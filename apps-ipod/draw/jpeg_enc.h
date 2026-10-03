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
#ifndef _JPEG_ENC_H_
#define _JPEG_ENC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lcd.h"

/* Native pixels in, a baseline JPEG out: YCbCr 4:2:0 with the standard
 * tables scaled to a quality of 1-100. The pixels arrive a band of 16 rows
 * at a time: read() fills band, width pixels a row, with rows y to y+rows-1
 * (fewer than 16 only at the bottom), and returns false to give up. */
struct jpeg_enc_src
{
    int width, height;
    fb_data *band;              /* 16 * width pixels */
    bool (*read)(void *ctx, fb_data *band, int y, int rows);
    void *ctx;
};

/* The JPEG's length in out, or -1 when read() gave up or the JPEG would
 * not fit in size bytes. */
int jpeg_encode(const struct jpeg_enc_src *src, int quality,
                uint8_t *out, size_t size);

#endif
