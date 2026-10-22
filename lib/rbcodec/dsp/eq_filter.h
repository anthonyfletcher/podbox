/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * The equalizer's band filters: the cookbook peak and shelves, each run as
 * the input plus a difference, y = x + e, in fewer multiplies than the
 * general biquad of dsp_filter.c.
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
#ifndef EQ_FILTER_H
#define EQ_FILTER_H

#include "dsp_filter.h"
#include "eq.h"

void eq_filter_coefs(enum eq_filter_type type, unsigned long cutoff,
                     unsigned long Q, long db, struct dsp_filter *f);
/* A first-order shelf, from 0 dB to db at the shelf's end, all of it then
   scaled by gain dB for eq_high_shelf1_gain_process() */
void eq_shelf1_coefs(bool low, unsigned long cutoff, long db, long gain,
                     struct dsp_filter *f);
void eq_filter_convert(struct dsp_filter *f, enum eq_filter_type from,
                       enum eq_filter_type to);
void eq_filter_process(struct dsp_filter *f, enum eq_filter_type type,
                       int32_t * const buf[], int count,
                       unsigned int channels);

/* One per filter type, in eq_filter_arm.S on ARM */
void eq_peak_process(struct dsp_filter *f, int32_t * const buf[], int count,
                     unsigned int channels);
void eq_low_shelf_process(struct dsp_filter *f, int32_t * const buf[],
                          int count, unsigned int channels);
void eq_high_shelf_process(struct dsp_filter *f, int32_t * const buf[],
                           int count, unsigned int channels);
void eq_low_shelf1_process(struct dsp_filter *f, int32_t * const buf[],
                           int count, unsigned int channels);
void eq_high_shelf1_process(struct dsp_filter *f, int32_t * const buf[],
                            int count, unsigned int channels);
void eq_high_shelf1_gain_process(struct dsp_filter *f, int32_t * const buf[],
                                 int count, unsigned int channels);

#endif /* EQ_FILTER_H */
