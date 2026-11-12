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
#ifndef PODLINK_H
#define PODLINK_H
#include <stdbool.h>
#include <stdint.h>

#define PODLINK_VERSION 1
#define PODLINK_MAX_MESSAGE 4096
#define PODLINK_HEADER 16
#define PODLINK_FRAME_MAX (PODLINK_HEADER + PODLINK_MAX_MESSAGE + 4)
enum podlink_type
{
    PODLINK_OPEN = 1,
    PODLINK_TEXT,
    PODLINK_BINARY,
    PODLINK_PING,
    PODLINK_PONG,
    PODLINK_CLOSE,
    PODLINK_HELLO
};
enum podlink_connection
{
    PODLINK_CLOSED,
    PODLINK_WAIT_USB,
    PODLINK_OPENING,
    PODLINK_CONNECTED,
    PODLINK_CLOSING
};
enum podlink_result
{
    PODLINK_BUSY = 0,
    PODLINK_SENT = 1,
    PODLINK_INVALID = -1
};
struct podlink_message
{
    uint8_t type, flags;
    uint32_t seq, length;
    /* Valid until the next poll. CLOSE begins with a little-endian u16 code. */
    const unsigned char *data;
};

/* Single screen/UI-thread owner. No disk I/O, allocation, sleeps or USB waits.
 * Poll frequently (at least once per second), including while closing.
 * A received CLOSE ends the message session, not the USB lease: a new OPEN
 * can reconnect. Unplug reports CLOSE 1006 once; replug works in the same
 * screen. close() requests clean CLOSE, then releases the lease on ACK or after
 * 1 s. Send copies payload; BUSY means retry later, with no bytes accepted. */
bool podlink_open(void);
int podlink_send(unsigned int type, const void *data, unsigned int length);
int podlink_poll(struct podlink_message *message);
/* Maximum outgoing payload negotiated with the peer. */
unsigned int podlink_max_message(void);
enum podlink_connection podlink_state(void);
void podlink_close(void);
/* Optional screen idle wait; wakes on transport progress, bounded by ticks. */
void podlink_wait(int ticks);

/* Portable wire codec, also built by the host tests. */
struct podlink_parser
{
    unsigned char bytes[PODLINK_FRAME_MAX];
    unsigned int used, consumed;
};
uint32_t podlink_crc32(const unsigned char *data, unsigned int length);
unsigned int podlink_encode(unsigned char *dest, unsigned int type,
                            uint32_t seq, const void *data,
                            unsigned int length);
/* Feed a chunk, stopping at a message/error. Always report consumed bytes.
 * A result can consume zero bytes when draining buffered recovery data.
 * Call with length zero to drain without input. Payload lasts until next call.
 */
int podlink_decode_bytes(struct podlink_parser *parser,
                         const unsigned char *data, unsigned int length,
                         unsigned int *consumed,
                         struct podlink_message *message);
/* Feed one byte. 1=message, 0=more, -1=bad CRC/header, -2=oversize. */
int podlink_decode(struct podlink_parser *parser, unsigned char byte,
                   struct podlink_message *message);
#endif
