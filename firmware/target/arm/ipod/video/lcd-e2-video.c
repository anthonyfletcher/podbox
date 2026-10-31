/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * iPod Video: screen pushes by the PP's second DMA engine (0x6000_8000),
 * which writes the BCM's host port while the CPU runs other threads. The
 * general engine at 0x6000_A000 cannot reach the port. The registers and
 * sequence are ProkBox5g's, from Apple's driver and run on a 5.5G.
 *
 * Parts: the port lock; the engine's registers; one piece; the push.
 *
 * Copyright (C) 2026 prokopto-dev
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
#include "cpu.h"
#include "system.h"
#include "kernel.h"
#include "pcm_mixer.h"
#include "lcd-e2-video.h"

/*** the port lock ***
 * The port has one auto-incrementing address register, and a push now
 * yields, so every thread-level port user in lcd-video.c holds this. The
 * tick's reads are kept out by lcd_block_tick(), not by this. All the
 * holders run on the CPU, so it lives in ordinary memory. The mutex's own
 * owner says whether it is held, including by a waiter it was just handed
 * to; the owner and depth here say whether by the caller. */
static struct mutex lcd_port_mtx;
static bool lcd_port_ready;
static unsigned int lcd_port_owner;
static int lcd_port_depth;
/* The thread that set up the LCD, the UI thread: only its updates push. */
static unsigned int lcd_port_pusher;

/* Set while a piece may be on its way, from just before e2_start() to the
 * end of its wait; e2_cut, when a CPU writer stopped that piece. */
static bool e2_moving, e2_cut;
static void e2_stop(void);

void lcd_port_init(void)
{
    mutex_init(&lcd_port_mtx);
    lcd_port_pusher = thread_self();
    lcd_port_ready = true;
}

static bool lcd_port_usable(void)
{
    return lcd_port_ready && irq_enabled() && CURRENT_CORE == CPU;
}

bool lcd_port_lock(void)
{
    if (!lcd_port_usable())
    {
        /* A panic, say, while the holder waits on a piece: the caller's
         * writes would otherwise land in the middle of it */
        if (e2_moving)
        {
            e2_stop();
            e2_cut = true;
        }
        return false;
    }
    mutex_lock(&lcd_port_mtx);
    if (lcd_port_depth++ == 0)
        lcd_port_owner = thread_self();
    return true;
}

void lcd_port_unlock(bool locked)
{
    if (locked)
    {
        lcd_port_depth--;
        mutex_unlock(&lcd_port_mtx);
    }
}

bool lcd_port_held_elsewhere(void)
{
    if (!lcd_port_usable() || lcd_port_mtx.blocker.thread == NULL)
        return false;
    return lcd_port_depth == 0 || lcd_port_owner != thread_self();
}

bool lcd_port_outermost(bool locked)
{
    return locked && lcd_port_depth == 1;
}

/*** the engine's registers ***
 * Channel 0. Bit 27 of the command and the configuration code 2 are
 * Apple's values; what they mean is unknown. */
#define E2_MASTER     (*(volatile unsigned long *)0x60008000)
#define E2_CMD        (*(volatile unsigned long *)0x60009000)
#define E2_STATUS     (*(volatile unsigned long *)0x60009004)
#define E2_SRC        (*(volatile unsigned long *)0x60009010)
#define E2_SRC_CFG    (*(volatile unsigned long *)0x60009014)
#define E2_DST        (*(volatile unsigned long *)0x60009018)
#define E2_DST_CFG    (*(volatile unsigned long *)0x6000901C)

#define E2_ENABLE     (1ul << 31)       /* in E2_MASTER */
#define E2_START      (1ul << 31)       /* in E2_CMD, clear again at the end */
#define E2_INTR       (1ul << 30)
#define E2_BUSY       (1ul << 31)       /* in E2_STATUS */
#define E2_CFG_MASK   (7ul << 24)

#define PORT_DATA     0x30000000ul
#define PORT_ADDR_LO  (*(volatile unsigned short *)0x30010000)
#define PORT_ADDR_HI  (*(volatile unsigned short *)0x30010002)
#define PORT_CONTROL  (*(volatile unsigned short *)0x30030000)
#define PORT_WR_READY 0x2

#define PIECE_MAX     0x10000           /* the command's 16-bit count */
#define PIECE_US      100000            /* 64 KB takes under 6 ms */
#define PORT_US       20000             /* the port answers in microseconds */
/* Shorter runs go by CPU: 16 whole rows */
#define PUSH_MIN      (LCD_WIDTH * 2 * 16)

/* Off for the rest of the boot once a piece never ends. */
static bool e2_on = true;

static bool port_ready(void)
{
    unsigned long t0 = USEC_TIMER;

    while (!(PORT_CONTROL & PORT_WR_READY))
        if (USEC_TIMER - t0 > PORT_US)
            return false;
    return true;
}

/*** one piece ***/
static bool e2_start(unsigned bcmaddr, const void *src, size_t bytes)
{
    unsigned long cmd;

    PORT_ADDR_LO = bcmaddr & 0xffff;
    PORT_ADDR_HI = bcmaddr >> 16;
    if (!port_ready())
        return false;

    commit_dcache();                    /* the engine reads memory */
    E2_MASTER |= E2_ENABLE;
    cmd = E2_CMD;
    cmd &= ~(E2_START | E2_INTR | 1ul << 28 | 1ul << 25 | 1ul << 24 |
             0xfful << 16 | 0xfffful);
    cmd |= 1ul << 27 | 1ul << 26 | (bytes - 4);
    E2_SRC_CFG = (E2_SRC_CFG & ~E2_CFG_MASK) | 2ul << 24;
    E2_DST_CFG = (E2_DST_CFG & ~E2_CFG_MASK) | 2ul << 24;
    E2_SRC = (unsigned long)src | UNCACHED_BASE_ADDR;
    E2_DST = PORT_DATA;
    E2_CMD = cmd;
    (void)E2_STATUS;                    /* acknowledges a stale event */
    E2_CMD = cmd | E2_START;
    return true;
}

static bool e2_busy(void)
{
    return (E2_STATUS & E2_BUSY) || (E2_CMD & E2_START);
}

/* Returns once the engine has let go of the port, or after PORT_US */
static void e2_stop(void)
{
    int oldlevel = disable_interrupt_save(IRQ_FIQ_STATUS);
    unsigned long t0;

    E2_CMD &= ~E2_INTR;
    E2_CMD &= ~E2_START;
    restore_interrupt(oldlevel);
    (void)E2_STATUS;
    t0 = USEC_TIMER;
    while ((E2_STATUS & E2_BUSY) && USEC_TIMER - t0 <= PORT_US)
        ;
}

/*** the push ***/
size_t lcd_e2_push(unsigned bcmaddr, const fb_data *src, size_t bytes)
{
    size_t done = 0;

    /* IRAM is unproven as a source */
    if (bytes < PUSH_MIN || (bytes & 3) || (bcmaddr & 3) ||
        ((uintptr_t)src & 3) || (uintptr_t)src >= 0x40000000)
        return 0;
    /* The engine lands a frame about half as late again, so it pays only
     * while the codec can use the time it frees */
    if (!e2_on || thread_self() != lcd_port_pusher ||
        mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) != CHANNEL_PLAYING)
        return 0;

    while (done < bytes)
    {
        size_t n = MIN(bytes - done, PIECE_MAX);
        unsigned long t0;

        e2_moving = true;
        if (!e2_start(bcmaddr + done, (const char *)src + done, n))
        {
            e2_moving = false;
            break;
        }
        t0 = USEC_TIMER;
        /* Unboosted, a big piece outlasts a tick, and yield() returns at
         * once when nothing else is runnable: sleeping lets the core idle. */
        if (n >= PIECE_MAX / 2 && cpu_frequency < CPUFREQ_MAX)
            sleep(1);
        while (e2_busy())
        {
            if (USEC_TIMER - t0 > PIECE_US)
            {
                /* The CPU sends this piece again, and every one after */
                e2_stop();
                e2_on = false;
                e2_moving = false;
                return done;
            }
            yield();
        }
        e2_moving = false;
        /* The CPU sends a piece that was cut short, and the rest */
        if (e2_cut)
        {
            e2_cut = false;
            return done;
        }
        done += n;
        if (!port_ready())
            break;
    }
    return done;
}
