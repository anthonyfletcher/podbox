/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Perceptual bass enhancement's band delays. Each sample is split by three
 * scalings into four bands; three of them are delayed, each by its own
 * ring buffer, and the four summed. A ring reads and writes at the same
 * place, so one head serves both, and it follows enqueue()/dequeue()'s rule
 * when a delay shortens under it: it reads where it is, then wraps.
 * pbe_delay_arm.S stands in for this on ARM, with the same output.
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
#include "rbcodecconfig.h"
#include "fracmul.h"
#include "pbe_delay.h"

#if !defined(CPU_ARM) || defined(CPU_ARM_MICRO)
void pbe_delay_process(int32_t *buf, int count, const int32_t coefs[3],
                       int32_t * const ring[3], int head[3],
                       const int len[3])
{
    const int32_t c1 = coefs[0], c2 = coefs[1], c3 = coefs[2];
    int32_t * const s0 = ring[0], * const s1 = ring[1], * const s2 = ring[2];
    int32_t * const e0 = s0 + len[0], * const e1 = s1 + len[1],
            * const e2 = s2 + len[2];
    int32_t *d0 = s0 + head[0], *d1 = s1 + head[1], *d2 = s2 + head[2];

    for (int i = 0; i < count; i++)
    {
        int32_t x = buf[i];
        int32_t f1 = FRACMUL(x, c1);
        int32_t f2 = FRACMUL(x, c2);
        int32_t f3 = FRACMUL(x, c3);
        /* 160-500 Hz goes straight through; below 160 Hz, 500-1150 Hz and
           above 1150 Hz are delayed */
        int32_t y = f1 - f2 + *d0 + *d1 + *d2;
        *d0++ = x - f1;
        *d1++ = f2 - f3;
        *d2++ = f3;
        buf[i] = y;
        if (d0 >= e0)
            d0 = s0;
        if (d1 >= e1)
            d1 = s1;
        if (d2 >= e2)
            d2 = s2;
    }

    head[0] = d0 - s0;
    head[1] = d1 - s1;
    head[2] = d2 - s2;
}
#endif /* CPU */
