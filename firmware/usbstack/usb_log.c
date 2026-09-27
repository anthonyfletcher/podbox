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

/* The USB event ring. Writers are the USB interrupt and the USB thread; the
 * one reader is the USB Log screen on the UI thread, which owns the file. */

#include "system.h"
#include "kernel.h"
#include "usb.h"
#include "usb_log.h"

static struct usb_log_entry ring[USB_LOG_SIZE];
static unsigned long head;          /* next sequence number to write */
static volatile unsigned long written;
static volatile bool attached;

void usb_log(int type, int a, int b, uint32_t c, uint32_t d)
{
    int oldlevel = disable_irq_save();
    struct usb_log_entry *e = &ring[head % USB_LOG_SIZE];
    e->us = (uint32_t)USEC_TIMER;
    e->type = type;
    e->a = a;
    e->b = b;
    e->c = c;
    e->d = d;
    head++;
    restore_irq(oldlevel);
}

void usb_log_sync(void)
{
    unsigned long target = head;
    uint32_t start = USEC_TIMER;
    bool timed_out = false;

    if (!attached || usb_get_insert_record()->storage_handed_over)
        return;

    while (written < target)
    {
        if ((uint32_t)USEC_TIMER - start > 3000000)
        {
            timed_out = true;
            break;
        }
        sleep(1);
    }
    usb_log(USB_LOG_SYNC, timed_out, 0, (uint32_t)USEC_TIMER - start, 0);
}

unsigned long usb_log_head(void)
{
    return head;
}

/* False when the entry has been overwritten, or is being. */
bool usb_log_read(unsigned long seq, struct usb_log_entry *e)
{
    int oldlevel = disable_irq_save();
    bool ok = seq < head && head - seq <= USB_LOG_SIZE;
    if (ok)
        *e = ring[seq % USB_LOG_SIZE];
    restore_irq(oldlevel);
    return ok;
}

void usb_log_attach(bool on)
{
    attached = on;
}

void usb_log_set_written(unsigned long seq)
{
    written = seq;
}

unsigned long usb_log_written(void)
{
    return written;
}
