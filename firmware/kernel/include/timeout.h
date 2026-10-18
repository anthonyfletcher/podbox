/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Björn Stenberg
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
#ifndef _KERNEL_H_
#define _KERNEL_H_

#include "config.h"
#include <stdint.h>

struct timeout;

/* timeout callback type
 * tmo - pointer to struct timeout associated with event
 * return next interval or <= 0 to stop event
 */
/* A full table drops a registration without a word, so the timeout never
 * fires: with the fork's USB, iAP, DAC search and log timeouts there are
 * about 15 that can be registered at once */
#define MAX_NUM_TIMEOUTS 24
typedef int (* timeout_cb_type)(struct timeout *tmo);

struct timeout
{
    timeout_cb_type callback;/* callback - returning false cancels */
    intptr_t        data;    /* data passed to callback */
    long            expires; /* expiration tick */
};

void timeout_register(struct timeout *tmo, timeout_cb_type callback,
                      int ticks, intptr_t data);
void timeout_cancel(struct timeout *tmo);

#endif /* _KERNEL_H_ */
