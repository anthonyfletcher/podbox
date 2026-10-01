/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025 by Sho Tanimoto
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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "logf.h"
#include "system.h"
#include "tick.h"

#include "font.h"
#include "lcd.h"
#include "usb_log.h"

#include "debug.h"
#include "libiap/iap.h"
#include "libiap/spec/hid.h"
#include "libiap/spec/iap.h"

#define MAX_COLS 64

static int      rows;
static int      columns;
static unsigned count;

static void update_color(void) {
    unsigned avail = rows - 1;
    if((count % (avail * 2)) > avail) {
        lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
    } else {
        lcd_set_drawinfo(DRMODE_SOLID, LCD_WHITE, LCD_BLACK);
    }
}

void iap_lcd_scatter(const char* fmt, ...) {
    if(rows == 0) {
        int w, h;
        font_getstringsize((unsigned char*)"A", &w, &h, FONT_SYSFIXED);
        columns = MIN(LCD_WIDTH / w, MAX_COLS);
        rows    = LCD_HEIGHT / h;
    }

    va_list ap;
    char    buf[256];
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), (char*)fmt, ap);
    va_end(ap);

    logf("%s", buf);

    lcd_set_backdrop(NULL);
    lcd_setfont(FONT_SYSFIXED);

    update_color();
    lcd_putsf(0, 0, (unsigned char*)"count %u", count);
    for(int i = 0; i < len;) {
        char line[MAX_COLS];
        int  copy = MIN(len - i, columns - 1);
        memcpy(line, buf + i, copy);
        memset(line + copy, ' ', columns - 1 - copy);
        line[columns - 1] = '\0';
        update_color();
        lcd_puts(0, 1 + count % (rows - 1), (unsigned char*)line);
        count += 1;
        i += copy;
    }
    lcd_update();
}

static unsigned long timestamp_epoch;

unsigned long iap_debug_timestamp(void) {
    return current_tick - timestamp_epoch;
}

void iap_debug_reset_timestamp(void) {
    timestamp_epoch = current_tick;
}

/* libiap's TransIDSupported; iap.c keeps the enum private */
#define TRANS_ID_SUPPORTED 1

void iap_log_report(struct IAPContext* ctx, const void* report, size_t size, bool from_player) {
    const struct IAPHIDReport* r = report;
    if(size < sizeof(*r) || (r->link_control & IAPHIDReportLinkControlBits_Continue)) {
        return;
    }

    /* The header and the first payload bytes, zero-padded: a small report
     * can end before the payload starts. */
    uint8_t      buf[16] = {0};
    const size_t avail   = size - sizeof(*r);
    memcpy(buf, r->data, MIN(avail, sizeof(buf)));

    const uint8_t* p = buf;
    if(*p == IAP_SYNC_BYTE) {
        p += 1;
    }
    if(*p++ != IAP_SOF_BYTE) {
        return;
    }
    unsigned length = *p++;
    if(length == 0) {
        length = p[0] << 8 | p[1];
        p += 2;
    }
    const uint8_t lingo   = *p++;
    unsigned      command = *p++;
    unsigned      header  = 2;
    if(lingo == IAPLingoID_ExtendedInterface) {
        command = command << 8 | *p++;
        header += 1;
    }
    /* The accessory's first packet is what decides whether packets carry
     * a transaction ID, so it has one only if it is StartIDPS. */
    unsigned trans = 0xffff;
    if(ctx->trans_id_support == TRANS_ID_SUPPORTED ||
       (lingo == IAPLingoID_General && command == IAPGeneralCommandID_StartIDPS)) {
        trans = p[0] << 8 | p[1];
        p += 2;
        header += 2;
    }
    const unsigned payload = length > header ? length - header : 0;
    uint32_t       first   = 0;
    for(unsigned i = 0; i < 4; i += 1) {
        first = first << 8 | (i < payload ? p[i] : 0);
    }
    usb_log(USB_LOG_IAP, lingo | (from_player ? USB_LOG_IAP_FROM_PLAYER : 0), command,
            payload | trans << 16, first);
}

const char* usb_log_iap_lingo(int lingo) {
    return _iap_lingo_str(lingo);
}

const char* usb_log_iap_command(int lingo, int command) {
    return _iap_command_str(lingo, command);
}
