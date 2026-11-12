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
#include <stdio.h>
#include <string.h>
#include "podlink.h"
#include "kernel.h"
#include "button.h"
#include "lcd.h"
#include "powermgmt.h"
#include "core_alloc.h"
#include "version.h"
#include "system.h"
#include "widgets/splash.h"
#include "system/shutdown.h"
#include "podlink_screen.h"

#define MAX_BENCH (16u * 1024u * 1024u)
static unsigned char reply[PODLINK_MAX_MESSAGE];
static unsigned int reply_len, reply_type;
static unsigned int goal, offset, errors;
static unsigned long received, sent, closes;
static enum
{
    ECHO,
    UPLOAD,
    DOWNLOAD
} operation;

static void text(const char *s)
{
    reply_len = snprintf((char *)reply, sizeof(reply), "%s", s);
    reply_type = PODLINK_TEXT;
}
static void message(const struct podlink_message *m)
{
    if (m->type == PODLINK_CLOSE)
    {
        ++closes;
        operation = ECHO;
        reply_type = 0;
        return;
    }
    if (m->type == PODLINK_PONG)
        return;
    received += m->length;
    if (operation == UPLOAD && m->type == PODLINK_BINARY)
    {
        if (m->length > goal - offset)
        {
            text("ERR upload length");
            operation = ECHO;
            return;
        }
        for (unsigned int i = 0; i < m->length; ++i)
            if (m->data[i] != (offset + i) % 251)
                ++errors;
        offset += m->length;
        if (offset == goal)
        {
            reply_len =
                snprintf((char *)reply, sizeof(reply), "%s TX %u errors=%u",
                         errors ? "ERR" : "OK", goal, errors);
            reply_type = PODLINK_TEXT;
            operation = ECHO;
        }
        return;
    }
    if (m->type == PODLINK_TEXT)
    {
        if (m->length == 4 && !memcmp(m->data, "PING", 4))
        {
            text("PONG");
            return;
        }
        if (m->length == 4 && !memcmp(m->data, "INFO", 4))
        {
            reply_len = snprintf(
                (char *)reply, sizeof(reply),
                "{\"firmware\":\"%s\",\"battery_percent\":%d,"
                "\"free_ram_bytes\":%lu,\"free_ram_scope\":\"core_allocator\","
                "\"rx_bytes\":%lu,\"tx_bytes\":%lu}",
                rbversion, battery_level(), (unsigned long)core_available(),
                received, sent);
            reply_type = PODLINK_TEXT;
            return;
        }
        if (m->length > 9 && (!memcmp(m->data, "BENCH TX ", 9) ||
                              !memcmp(m->data, "BENCH RX ", 9)))
        {
            unsigned int candidate = 0;
            bool valid = true;
            for (unsigned int i = 9; i < m->length; ++i)
            {
                if (m->data[i] < '0' || m->data[i] > '9' ||
                    candidate > MAX_BENCH / 10)
                {
                    valid = false;
                    break;
                }
                candidate = candidate * 10 + m->data[i] - '0';
            }
            if (!valid || !candidate || candidate > MAX_BENCH)
            {
                text("ERR benchmark size");
                return;
            }
            /* A valid command starts a new transfer, replacing any active one.
             */
            goal = candidate;
            operation = m->data[6] == 'T' ? UPLOAD : DOWNLOAD;
            offset = errors = 0;
            reply_len = snprintf((char *)reply, sizeof(reply), "READY %cX %u",
                                 m->data[6], goal);
            reply_type = PODLINK_TEXT;
            return;
        }
    }
    /* One message in, one message out. No terminator, file, or text conversion.
     */
    memcpy(reply, m->data, m->length);
    reply_len = m->length;
    reply_type = m->type;
}

int podlink_screen(void)
{
    if (!podlink_open())
    {
        splash(HZ * 3, "Unplug USB, then start PodLink");
        return 0;
    }
    long draw_at = 0;
    bool exiting = false;
    received = sent = closes = 0;
    reply_type = 0;
    operation = ECHO;
    while (true)
    {
        bool progress = false;
        uint32_t turn = USEC_TIMER;
        for (int step = 0; step < 16; ++step)
        {
            if (step && (uint32_t)USEC_TIMER - turn >= 2000)
                break;
            if (reply_type)
            {
                int rc = podlink_send(reply_type, reply, reply_len);
                if (rc == PODLINK_SENT)
                {
                    sent += reply_len;
                    reply_type = 0;
                    progress = true;
                }
                else if (rc == PODLINK_INVALID ||
                         podlink_state() != PODLINK_CONNECTED)
                    reply_type = 0;
            }
            struct podlink_message m;
            if (podlink_poll(&m))
            {
                message(&m);
                progress = true;
            }
            if (!reply_type && operation == DOWNLOAD && !exiting &&
                podlink_state() == PODLINK_CONNECTED)
            {
                if (offset == goal)
                {
                    reply_len = snprintf((char *)reply, sizeof(reply),
                                         "OK RX %u", goal);
                    reply_type = PODLINK_TEXT;
                    operation = ECHO;
                }
                else
                {
                    reply_len = MIN(podlink_max_message(), goal - offset);
                    for (unsigned int i = 0; i < reply_len; ++i)
                        reply[i] = (offset + i) % 251;
                    offset += reply_len;
                    reply_type = PODLINK_BINARY;
                }
            }
        }
        enum podlink_connection state = podlink_state();
        if (exiting && state == PODLINK_CLOSED)
            break;
        if (TIME_AFTER(current_tick, draw_at))
        {
            static const char *const names[] = {
                "Closed", "Waiting: plug USB in", "USB up / waiting OPEN",
                "Open / messages", "Closing"};
            lcd_clear_display();
            lcd_puts(0, 0, "PodLink");
            lcd_puts(0, 2, names[state]);
            lcd_putsf(0, 4, "RX: %lu  TX: %lu", received, sent);
            lcd_putsf(0, 5, "Close events: %lu", closes);
            lcd_puts(0, 7, "MENU: close and exit");
            lcd_update();
            draw_at = current_tick + HZ / 4;
        }
        int button = button_get(false);
        if (button == BUTTON_MENU && !exiting)
        {
            exiting = true;
            reply_type = 0;
            operation = ECHO;
            podlink_close();
        }
        if (default_event_handler(button) == SYS_USB_CONNECTED)
        {
            podlink_close();
            return 1;
        }
        if (progress)
            yield();
        else
            podlink_wait(1);
    }
    return 0;
}
