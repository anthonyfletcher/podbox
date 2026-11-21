/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * The equalizer's bands run on the second core where there is one: the
 * caller hands a block over and sleeps until it is done, so its own core
 * runs other threads meanwhile.
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
#ifndef EQ_COP_H
#define EQ_COP_H

#include <stdbool.h>
#include <stdint.h>
#include "platform.h"

typedef void (*eq_cop_bands_fn)(int32_t * const buf[], int count,
                                unsigned int channels);

enum eq_cop_state
{
    EQ_COP_NONE,        /* No second core in this build */
    EQ_COP_RUNNING,     /* The bands run on the COP */
    EQ_COP_NO_THREAD,   /* The worker could not be created */
    EQ_COP_TIMED_OUT,   /* The COP stopped answering; the CPU has the bands */
};

#if NUM_CORES > 1 && (CONFIG_PLATFORM & PLATFORM_NATIVE)
/* Create the worker; once, from a thread on the CPU */
void eq_cop_init(void);
/* Run bands(buf, count, channels) on the COP and wait for it. False when
   the caller must run it itself: no worker, or an interrupt. True with an
   interrupt's block unfiltered while the COP has a thread's. A block the
   COP is late with is still waited for, and the next go to the CPU. */
bool eq_cop_run(eq_cop_bands_fn bands, int32_t * const buf[], int count,
                unsigned int channels);
/* Held around a run and around every change to the bands, so a block is
   never filtered with half-changed coefficients */
void eq_cop_lock(void);
void eq_cop_unlock(void);
enum eq_cop_state eq_cop_state(void);

/* For Debug > EQ on COP Check (eq.c): count stereo frames of buf filtered
   with the current bands from a cleared history, on the COP or on the
   caller's core, leaving the playing EQ as it was. False if the COP was
   asked for and did not take it. */
#define EQ_CHECK_BLOCK 128      /* frames a call, as the DSP's sample input */
bool dsp_eq_check_run(int32_t *buf[2], int count, bool on_cop);
#else
static inline void eq_cop_init(void) {}
static inline bool eq_cop_run(eq_cop_bands_fn bands, int32_t * const buf[],
                              int count, unsigned int channels)
    { (void)bands; (void)buf; (void)count; (void)channels; return false; }
static inline void eq_cop_lock(void) {}
static inline void eq_cop_unlock(void) {}
static inline enum eq_cop_state eq_cop_state(void) { return EQ_COP_NONE; }
#endif

#endif /* EQ_COP_H */
