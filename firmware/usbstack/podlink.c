/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * PodLink USB messaging
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
#include "config.h"
#include "podlink.h"
#include "usb.h"
#include "usb_serial.h"
#include "kernel.h"
#include <string.h>
#include "system.h"
/* Two application messages and one reserved control frame. */
#define QUEUE_SIZE 3
struct tx_frame
{
    unsigned int length, sent;
    unsigned char data[PODLINK_FRAME_MAX];
};
static struct tx_frame tx[QUEUE_SIZE];
static unsigned int head, count;
static struct podlink_parser parser;
static unsigned char input[4096];
static unsigned int input_pos, input_length;
static unsigned char event_data[PODLINK_MAX_MESSAGE];
static enum podlink_connection state;
static uint32_t next_seq, ping_seq, peer_max;
static uint32_t transport_generation;
static long last_rx, ping_at, close_at;
static bool leased, had_usb, ping_pending, release_requested;
static const unsigned char hello[] = {1, 0, 0, 16, 0, 0};

static int enqueue(unsigned int type, uint32_t seq, const void *data,
                   unsigned int length, bool control)
{
    if (count >= (control ? QUEUE_SIZE : QUEUE_SIZE - 1))
        return PODLINK_BUSY;
    struct tx_frame *f = &tx[(head + count) % QUEUE_SIZE];
    f->length = podlink_encode(f->data, type, seq, data, length);
    if (!f->length)
        return PODLINK_INVALID;
    f->sent = 0;
    ++count;
    return PODLINK_SENT;
}
static void pump(void)
{
    if (!count)
        return;
    struct tx_frame *f = &tx[head];
    f->sent += usb_serial_write(f->data + f->sent, f->length - f->sent);
    if (f->sent == f->length)
    {
        head = (head + 1) % QUEUE_SIZE;
        --count;
    }
}
static void reset_session(void)
{
    head = count = 0;
    memset(&parser, 0, sizeof(parser));
    input_pos = input_length = 0;
    ping_pending = false;
    next_seq = 1;
    peer_max = PODLINK_MAX_MESSAGE;
}
static void finish(void)
{
    usb_podlink_close();
    leased = false;
    state = PODLINK_CLOSED;
    reset_session();
}
static int close_event(struct podlink_message *m, unsigned int code)
{
    event_data[0] = code;
    event_data[1] = code >> 8;
    m->type = PODLINK_CLOSE;
    m->flags = 0;
    m->seq = 0;
    m->length = 2;
    m->data = event_data;
    return 1;
}
static void send_close(unsigned int code)
{
    unsigned char data[2] = {code, code >> 8};
    enqueue(PODLINK_CLOSE, next_seq++, data, sizeof(data), true);
}

bool podlink_open(void)
{
    if (leased || !usb_podlink_open())
        return false;
    leased = true;
    had_usb = release_requested = false;
    reset_session();
    state = PODLINK_WAIT_USB;
    return true;
}
unsigned int podlink_max_message(void)
{
    return peer_max;
}
enum podlink_connection podlink_state(void)
{
    return state;
}
int podlink_send(unsigned int type, const void *data, unsigned int length)
{
    if (type < PODLINK_TEXT || type > PODLINK_PONG || length > peer_max ||
        (length && !data))
        return PODLINK_INVALID;
    if (state != PODLINK_CONNECTED)
        return PODLINK_BUSY;
    int rc = enqueue(type, next_seq, data, length, false);
    if (rc == PODLINK_SENT)
        ++next_seq;
    return rc;
}
void podlink_close(void)
{
    if (!leased || release_requested)
        return;
    release_requested = true;
    if (!had_usb)
    {
        finish();
        return;
    }
    send_close(1000);
    state = PODLINK_CLOSING;
    close_at = current_tick;
}

int podlink_poll(struct podlink_message *m)
{
    if (!m || !leased)
        return 0;
    uint32_t generation;
    bool connected = usb_serial_get_state(&generation);
    if (!connected || (had_usb && generation != transport_generation))
    {
        bool notify = had_usb;
        had_usb = false;
        reset_session();
        state = PODLINK_WAIT_USB;
        if (release_requested)
            finish();
        return notify ? close_event(m, 1006) : 0;
    }
    if (!had_usb)
    {
        had_usb = true;
        transport_generation = generation;
        reset_session();
        state = PODLINK_OPENING;
        last_rx = current_tick;
        enqueue(PODLINK_HELLO, next_seq++, hello, sizeof(hello), true);
    }
    pump();
    if (release_requested && TIME_AFTER(current_tick, close_at + HZ))
    {
        finish();
        return close_event(m, 1000);
    }
    if (state == PODLINK_CONNECTED)
    {
        if (ping_pending && TIME_AFTER(current_tick, ping_at + 15 * HZ))
        {
            send_close(1001);
            state = PODLINK_OPENING;
            ping_pending = false;
            return close_event(m, 1001);
        }
        if (!ping_pending && TIME_AFTER(current_tick, last_rx + 5 * HZ))
        {
            ping_seq = next_seq++;
            if (enqueue(PODLINK_PING, ping_seq, NULL, 0, true) == PODLINK_SENT)
            {
                ping_pending = true;
                ping_at = current_tick;
            }
        }
    }
    /* Drain TX before RX so control replies cannot be dropped. Backpressure
     * delays RX; local close still releases the lease after one second. */
    if (count)
        return 0;
    for (int work = 0; work < 64; ++work)
    {
        if (input_pos == input_length)
        {
            input_length = usb_serial_read(input, sizeof(input));
            input_pos = 0;
        }
        struct podlink_message incoming;
        unsigned int consumed;
        int rc = podlink_decode_bytes(&parser, input + input_pos,
                                      input_length - input_pos, &consumed,
                                      &incoming);
        input_pos += consumed;
        if (rc == -2)
        {
            send_close(1009);
            state = PODLINK_OPENING;
            return close_event(m, 1009);
        }
        if (rc <= 0)
        {
            if (!rc && !input_length)
                break;
            continue; /* bad CRC/magic: scan for the next frame */
        }
        last_rx = current_tick;
        if (incoming.type == PODLINK_OPEN)
        {
            const unsigned char *d = incoming.data;
            if (release_requested)
                continue;
            if (incoming.length != sizeof(hello) || d[0] != PODLINK_VERSION ||
                d[1])
            {
                send_close(1002);
                state = PODLINK_OPENING;
                return close_event(m, 1002);
            }
            uint32_t limit = (uint32_t)d[2] | (uint32_t)d[3] << 8 |
                             (uint32_t)d[4] << 16 | (uint32_t)d[5] << 24;
            if (!limit)
            {
                send_close(1002);
                continue;
            }
            peer_max = MIN(limit, PODLINK_MAX_MESSAGE);
            unsigned char answer[6] = {1, 0, peer_max, peer_max >> 8, 0, 0};
            enqueue(PODLINK_OPEN, incoming.seq, answer, sizeof(answer), true);
            state = PODLINK_CONNECTED;
            ping_pending = false;
        }
        else if (incoming.type == PODLINK_CLOSE)
        {
            if (incoming.length < 2)
            {
                send_close(1002);
                if (!release_requested)
                {
                    state = PODLINK_OPENING;
                    ping_pending = false;
                }
                return close_event(m, 1002);
            }
            memcpy(event_data, incoming.data, incoming.length);
            *m = incoming;
            m->data = event_data;
            if (release_requested)
                finish();
            else
            {
                enqueue(PODLINK_CLOSE, incoming.seq, event_data,
                        incoming.length, true);
                state = PODLINK_OPENING;
                ping_pending = false;
            }
            return 1;
        }
        else if (state == PODLINK_CONNECTED)
        {
            if (incoming.type == PODLINK_PING)
                enqueue(PODLINK_PONG, incoming.seq, incoming.data,
                        incoming.length, true);
            else if (incoming.type == PODLINK_PONG && incoming.seq == ping_seq)
                ping_pending = false;
            if (incoming.type == PODLINK_TEXT ||
                incoming.type == PODLINK_BINARY ||
                incoming.type == PODLINK_PONG)
            {
                memcpy(event_data, incoming.data, incoming.length);
                *m = incoming;
                m->data = event_data;
                return 1;
            }
        }
        /* Once a response filled the control slot, stop before another frame.
         */
        if (count)
            break;
    }
    pump();
    return 0;
}

void podlink_wait(int ticks)
{
    if (input_pos == input_length)
        usb_serial_wait(ticks);
}
