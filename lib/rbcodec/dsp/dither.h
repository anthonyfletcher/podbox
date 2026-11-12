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
#ifndef DITHER_H
#define DITHER_H

#include <stdbool.h>
#include <stdint.h>

/* One channel's history. dither_arm.S reads it as four words in this
   order. */
struct dither_state
{
    long error[3];  /* 00h: error term history */
    long random;    /* 0ch: last random value */
};

/* count samples of one channel quantised to 16 bits by dropping scale
   bits, with noise-shaped triangular dither; dst steps by two, for
   interleaved stereo. */
void dither_channel(struct dither_state *st, const int32_t *src,
                    int16_t *dst, int count, int scale);

/* Whether no sample of the block has a bit set below the output's LSB, so
   that dropping scale bits loses nothing */
bool dither_block_exact(int32_t * const p32[], int channels, int count,
                        int scale);

#endif /* DITHER_H */
