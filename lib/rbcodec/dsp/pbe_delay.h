/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Perceptual bass enhancement's band delays, for one channel.
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
#ifndef PBE_DELAY_H
#define PBE_DELAY_H

#include <stdint.h>

/* buf through the three delay lines in place. coefs are the band splits
   in s0.31; ring[k] delays by len[k] samples, at least one, and head[k] is
   where it is up to. */
void pbe_delay_process(int32_t *buf, int count, const int32_t coefs[3],
                       int32_t * const ring[3], int head[3],
                       const int len[3]);

#endif /* PBE_DELAY_H */
