/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2008 by Miika Pekkarinen
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

#include <stdio.h>
#include "events.h"
#include "panic.h"

/* Leaving the Now Playing screen alone turns on four handlers, on top of the
 * fork's resident ones and any storage-idle callbacks queued; a full table
 * panics */
#define MAX_SYS_EVENTS 48

/* the runs of a oneshot in one send, the first and those asked for during
   one: a callback asked for again at every run gives its sender back then */
#define ONESHOT_RUNS 4

struct sysevent {
    unsigned short id;
    /* the registration's number in its slot, new at each: a run's return
       knows its own from a later one's that took the slot meanwhile */
    unsigned short gen;
    bool oneshot;
    /* a oneshot's handler is running, as a storage idle callback that
       yields in its writes, and was asked for again meanwhile */
    bool running, again;
    union {
        void (*cb)(unsigned short id, void *event_data);
        void (*cb_ex)(unsigned short id, void *event_data, void *user_data);
    } handler;
    void *user_data;
};

static struct sysevent events[MAX_SYS_EVENTS];
static int invalid_userdata;

static bool do_add_event(unsigned short id, bool oneshot,
                         void *handler, void *user_data)
{
    size_t free = MAX_SYS_EVENTS;
    struct sysevent *ev;
    /* Check if the event already exists. & lowest free slot available */
    for (size_t i = MAX_SYS_EVENTS - 1; i < MAX_SYS_EVENTS; i--)
    {
        ev = &events[i];
        if (ev->handler.cb == NULL)
            free = i;

        if (ev->id == id && ev->handler.cb == handler && user_data == ev->user_data)
        {
            /* a oneshot asked for while it runs, as a mark changed during
               its save: it runs again after it, instead of being lost as
               its slot is freed */
            if (ev->running)
            {
                ev->again = true;
                return true;
            }
            return false;
        }
    }

    /* is there a free slot? */
    if (free < MAX_SYS_EVENTS)
    {
        ev = &events[free];
        ev->id = id;
        ev->handler.cb = handler;
        ev->user_data = user_data;
        ev->oneshot = oneshot;
        ev->gen++;
        ev->running = false;
        ev->again = false;

        return true;
    }

    panicf("event line full");
    return false;
}

bool add_event(unsigned short id, void (*handler)(unsigned short id, void *data))
{
    return do_add_event(id, false, handler, &invalid_userdata);
}

bool add_event_ex(unsigned short id, bool oneshot,
                  void (*handler)(unsigned short id, void *event_data, void *user_data), void *user_data)
{
    return do_add_event(id, oneshot, handler, user_data);
}

static void do_remove_event(unsigned short id, void *handler, void *user_data)
{
    for (size_t i = 0; i < MAX_SYS_EVENTS; i++)
    {
        struct sysevent *ev = &events[i];
        if (ev->id == id && ev->handler.cb == handler && user_data == ev->user_data)
        {
            ev->handler.cb = NULL;
            ev->running = false; /* its run, if any, then leaves the slot */
            ev->again = false;
            return;
        }
    }
}

void remove_event(unsigned short id, void (*handler)(unsigned short id, void *data))
{
    do_remove_event(id, handler, &invalid_userdata);
}

void remove_event_ex(unsigned short id,
                     void (*handler)(unsigned short id, void *event_data, void *user_data),
                     void *user_data)
{
    do_remove_event(id, handler, user_data);
}

/* A oneshot (only _ex events have one) runs once at a time: a send while it
   runs, as another thread's call_storage_idle_notifys(true) during its
   writes' yields, passes it by. Asked for again meanwhile, it runs again at
   once from the send that runs it, which completes the request before it
   returns: the storage thread's, before the disk sleeps or USB is answered.
   A forced send that passed it by leaves no request for an idle that may
   not come. Then it is freed, unless still asked for after ONESHOT_RUNS:
   then at the next send. The senders are threads of one core, which switch
   only at a yield: no lock, as before. */
void send_event(unsigned short id, void *data)
{
    for (size_t i = 0; i < MAX_SYS_EVENTS; i++)
    {
        struct sysevent *ev = &events[i];
        if (ev->id != id || ev->handler.cb == NULL)
            continue;

        if (ev->user_data == &invalid_userdata)
            ev->handler.cb(id, data);
        else if (!ev->oneshot)
            ev->handler.cb_ex(id, data, ev->user_data);
        else if (!ev->running)
        {
            const unsigned short gen = ev->gen;
            ev->running = true;
            for (int run = 1; ; run++)
            {
                ev->again = false;
                ev->handler.cb_ex(id, data, ev->user_data);
                /* removed meanwhile, and the slot maybe another's: left */
                if (ev->gen != gen || !ev->running)
                    break;
                if (ev->again && run < ONESHOT_RUNS)
                    continue;
                ev->running = false;
                if (!ev->again)
                    ev->handler.cb = NULL;
                ev->again = false;
                break;
            }
        }
    }
}
