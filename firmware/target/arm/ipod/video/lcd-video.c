/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * LCD driver for iPod Video
 *
 * Based on code from the ipodlinux project - http://ipodlinux.org/
 * Adapted for Rockbox in December 2005
 *
 * Original file: linux/arch/armnommu/mach-ipod/fb.c
 *
 * Copyright (c) 2003-2005 Bernard Leach (leachbj@bouncycastle.org)
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

#include <sys/types.h> /* off_t */
#include "config.h"
#include "cpu.h"
#include "lcd.h"
#include "kernel.h"
#include "system.h"
#ifdef HAVE_LCD_SLEEP
/* Included only for lcd_awake() prototype */
#include "backlight-target.h"
#endif
#ifdef HAVE_COMPOSITE_VIDEO_OUT
#include <string.h>
#include "core_alloc.h"
#include "videoout.h"
#endif

/* The BCM bus width is 16 bits. But since the low address bits aren't decoded
 * by the chip (the 3 BCM address bits are mapped to address bits 16..18 of the
 * PP5022), writing 32 bits (and even more, using 'stmia') at once works. */
#define BCM_DATA      (*(volatile unsigned short*)(0x30000000))
#define BCM_DATA32    (*(volatile unsigned long *)(0x30000000))
#define BCM_WR_ADDR   (*(volatile unsigned short*)(0x30010000))
#define BCM_WR_ADDR32 (*(volatile unsigned long *)(0x30010000))
#define BCM_RD_ADDR   (*(volatile unsigned short*)(0x30020000))
#define BCM_RD_ADDR32 (*(volatile unsigned long *)(0x30020000))
#define BCM_CONTROL   (*(volatile unsigned short*)(0x30030000))

#define BCM_ALT_DATA      (*(volatile unsigned short*)(0x30040000))
#define BCM_ALT_DATA32    (*(volatile unsigned long *)(0x30040000))
#define BCM_ALT_WR_ADDR   (*(volatile unsigned short*)(0x30050000))
#define BCM_ALT_WR_ADDR32 (*(volatile unsigned long *)(0x30050000))
#define BCM_ALT_RD_ADDR   (*(volatile unsigned short*)(0x30060000))
#define BCM_ALT_RD_ADDR32 (*(volatile unsigned long *)(0x30060000))
#define BCM_ALT_CONTROL   (*(volatile unsigned short*)(0x30070000))

/* Time until the BCM is considered stalled and will be re-kicked.
 * Must be guaranteed to be >~ 20ms. */
#define BCM_UPDATE_TIMEOUT (HZ/20)
/* An LCD update command done while the LCD is off needs >~ 200ms */
#define BCM_LCDINIT_TIMEOUT (HZ/2)

/* Addresses within BCM */
#define BCMA_SRAM_BASE   0
#define BCMA_COMMAND     0x1F8
#define BCMA_STATUS      0x1FC
#define BCMA_CMDPARAM    0xE0000    /* Parameters/data for commands */
#define BCMA_SDRAM_BASE  0xC0000000
#define BCMA_TV_FB       0xC0000000 /* TV out framebuffer */
#define BCMA_TV_BMPDATA  0xC0200000 /* BMP data for TV out functions */

/* BCM commands.  Write them to BCMA_COMMAND.  Note BCM_CMD encoding. */
#define BCM_CMD(x) ((~((unsigned long)x) << 16) | ((unsigned long)x))
#define BCMCMD_LCD_UPDATE     BCM_CMD(0)
/* Execute "M25 Diagnostics".  Status displayed on LCD.  Takes <40s */
#define BCMCMD_SELFTEST       BCM_CMD(1)
#define BCMCMD_TV_PALBMP      BCM_CMD(2)
#define BCMCMD_TV_NTSCBMP     BCM_CMD(3)
/* BCM_CMD(4) may be another TV-related command */
/* The following might do more depending on word at 0xE00000 */
#define BCMCMD_LCD_UPDATERECT BCM_CMD(5)
#define BCMCMD_LCD_SLEEP      BCM_CMD(8)
/* BCM_CMD(12) involved in shutdown */
/* Macrovision analog copy prevention is on by default on TV output.
   Execute this command after enabling TV out to turn it off.
 */
#define BCMCMD_TV_MVOFF       BCM_CMD(14)

enum lcd_status
{
    LCD_IDLE,
    LCD_INITIAL,
    LCD_NEED_UPDATE,
    LCD_UPDATING
};

struct
{
    long update_timeout;  /* also used to ensure BCM stays off for >= 50 ms */
    enum lcd_status state;
    bool blocked;
#if NUM_CORES > 1
    struct corelock cl;   /* inter-core sync */
#endif
#ifdef HAVE_LCD_SLEEP
    bool display_on;
    bool waking;
    struct semaphore initwakeup;
#endif
} lcd_state IBSS_ATTR;

#ifdef HAVE_COMPOSITE_VIDEO_OUT
/* True while the TV is kept current: LCD updates then go to the TV. */
static bool tv_on;
/* True once the BCM has been put in TV mode, until the next boot. Taking it
 * out again hangs the player, whether by the LCD sleep path or by cutting
 * its power and booting it, so the LCD does not sleep after. */
static bool tv_bcm_mode;
static void tv_mark_dirty(int y, int height);
#endif

#ifdef HAVE_LCD_SLEEP
const fb_data *flash_vmcs_offset;
unsigned flash_vmcs_length;

#define ROM_BASE        0x20000000
#define ROM_ID(a,b,c,d) (unsigned int)(  ((unsigned int)(d))        | \
                                        (((unsigned int)(c)) << 8)  | \
                                        (((unsigned int)(b)) << 16) | \
                                        (((unsigned int)(a)) << 24) )

/* Get address and length of iPod flash section.
   Based on part of FS#6721.  This may belong elsewhere.
   (BCM initialization uploads the vmcs section to the BCM.)
 */
static bool flash_get_section(const unsigned int imageid,
                              void **offset,
                              unsigned int *length)
{
    unsigned long *p = (unsigned long*)(ROM_BASE + 0xffe00);
    unsigned char *csp, *csend;
    unsigned long checksum;

    /* Find the image in the directory */
    while (1)
    {
        if (p[0] != ROM_ID('f','l','s','h'))
            return false;
        if (p[1] == imageid)
            break;
        p += 10;
    }

    *offset = (void *)(ROM_BASE + p[3]);
    *length = p[4];

    /* Verify checksum.  Probably unnecessary, but it's fast. */
    checksum = 0;
    csend = (unsigned char *)(ROM_BASE + p[3] + p[4]);
    for(csp = (unsigned char *)(ROM_BASE + p[3]); csp < csend; csp++)
    {
        checksum += *csp;
    }

    return checksum == p[7];
}
#endif /* HAVE_LCD_SLEEP */

static inline void bcm_write_addr(unsigned address)
{
    BCM_WR_ADDR32 = address;       /* write destination address */

    while (!(BCM_CONTROL & 0x2));  /* wait for it to be write ready */
}

static inline void bcm_write32(unsigned address, unsigned value)
{

    bcm_write_addr(address);       /* set destination address */

    BCM_DATA32 = value;            /* write value */
}

static inline unsigned bcm_read32(unsigned address)
{
    while (!(BCM_RD_ADDR & 1));

    BCM_RD_ADDR32 = address;       /* write source address */

    while (!(BCM_CONTROL & 0x10)); /* wait for it to be read ready */

    return BCM_DATA32;             /* read value */
}

#ifdef HAVE_LCD_SLEEP
static void continue_lcd_awake(void)
{
    lcd_state.waking = false;
    semaphore_release(&(lcd_state.initwakeup));
}
#endif

#ifndef BOOTLOADER
static void lcd_tick(void)
{
    /* No core level interrupt mask - already in interrupt context */
#if NUM_CORES > 1
    corelock_lock(&lcd_state.cl);
#endif

    if (!lcd_state.blocked && lcd_state.state >= LCD_NEED_UPDATE)
    {
        unsigned data = bcm_read32(BCMA_COMMAND);
        bool bcm_is_busy = (data == BCMCMD_LCD_UPDATE || data == 0xFFFF);

        if (((lcd_state.state == LCD_NEED_UPDATE) && !bcm_is_busy)
            /* Update requested and BCM is no longer busy. */
         || (TIME_AFTER(current_tick, lcd_state.update_timeout) && bcm_is_busy))
            /* BCM still busy after timeout, i.e. stalled. */
        {
            bcm_write32(BCMA_COMMAND, BCMCMD_LCD_UPDATE);  /* Kick off update */
            BCM_CONTROL = 0x31;
            lcd_state.update_timeout = current_tick + BCM_UPDATE_TIMEOUT;
            lcd_state.state = LCD_UPDATING;
#ifdef HAVE_LCD_SLEEP
            if (lcd_state.waking)
                continue_lcd_awake();
#endif
        }
        else if ((lcd_state.state == LCD_UPDATING) && !bcm_is_busy)
        {
            /* Update finished properly and no new update pending. */
            lcd_state.state = LCD_IDLE;
#ifdef HAVE_LCD_SLEEP
            if (lcd_state.waking)
                continue_lcd_awake();
#endif
        }
    }
#if NUM_CORES > 1
    corelock_unlock(&lcd_state.cl);
#endif
}

static inline void lcd_block_tick(void)
{
    int oldlevel = disable_irq_save();

#if NUM_CORES > 1
    corelock_lock(&lcd_state.cl);
    lcd_state.blocked = true;
    corelock_unlock(&lcd_state.cl);
#else
    lcd_state.blocked = true;
#endif
    restore_irq(oldlevel);
}

#ifdef HAVE_COMPOSITE_VIDEO_OUT
/* Lets the tick run again without starting an LCD update. */
static inline void lcd_unblock_tick(void)
{
    int oldlevel = disable_irq_save();

#if NUM_CORES > 1
    corelock_lock(&lcd_state.cl);
    lcd_state.blocked = false;
    corelock_unlock(&lcd_state.cl);
#else
    lcd_state.blocked = false;
#endif
    restore_irq(oldlevel);
}
#endif

static void lcd_unblock_and_update(void)
{
    unsigned data;
    bool bcm_is_busy;
    int oldlevel = disable_irq_save();

#if NUM_CORES > 1
    corelock_lock(&lcd_state.cl);
#endif
    data = bcm_read32(BCMA_COMMAND);
    bcm_is_busy = (data == BCMCMD_LCD_UPDATE || data == 0xFFFF);

    if (!bcm_is_busy || (lcd_state.state == LCD_INITIAL) ||
        TIME_AFTER(current_tick, lcd_state.update_timeout))
    {
        bcm_write32(BCMA_COMMAND, BCMCMD_LCD_UPDATE);  /* Kick off update */
        BCM_CONTROL = 0x31;
        lcd_state.update_timeout = current_tick + BCM_UPDATE_TIMEOUT;
        lcd_state.state = LCD_UPDATING;
#ifdef HAVE_LCD_SLEEP
        if (lcd_state.waking)
            continue_lcd_awake();
#endif
    }
    else
    {
         lcd_state.state = LCD_NEED_UPDATE; /* Post update request */
    }
    lcd_state.blocked = false;

#if NUM_CORES > 1
    corelock_unlock(&lcd_state.cl);
#endif
    restore_irq(oldlevel);
}

#else /* BOOTLOADER */

#define lcd_block_tick()

static void lcd_unblock_and_update(void)
{
    unsigned data;

    if (lcd_state.state != LCD_INITIAL)
    {
        data = bcm_read32(BCMA_COMMAND);
        while (data == BCMCMD_LCD_UPDATE || data == 0xFFFF)
        {
            yield();
            data = bcm_read32(BCMA_COMMAND);
        }
    }
    bcm_write32(BCMA_COMMAND, BCMCMD_LCD_UPDATE);  /* Kick off update */
    BCM_CONTROL = 0x31;
    lcd_state.state = LCD_IDLE;
}
#endif /* BOOTLOADER */

/*** hardware configuration ***/

void lcd_set_contrast(int val)
{
  /* TODO: Implement lcd_set_contrast() */
  (void)val;
}

void lcd_set_invert_display(bool yesno)
{
  /* TODO: Implement lcd_set_invert_display() */
  (void)yesno;
}

/* turn the display upside down (call lcd_update() afterwards) */
void lcd_set_flip(bool yesno)
{
  /* TODO: Implement lcd_set_flip() */
  (void)yesno;
}

/* LCD init */
void lcd_init_device(void)
{
    /* These port initializations are supposed to be done when initializing
       the BCM.  None of it is changed when shutting down the BCM.
     */
    GPO32_ENABLE |= 0xC000;
    GPIO_CLEAR_BITWISE(GPIOC_ENABLE, 0x80);
    /* This pin is used for BCM interrupts */
    GPIOC_ENABLE |= 0x40;
    GPIOC_OUTPUT_EN &= ~0x40;
    GPO32_ENABLE &= ~1;

    lcd_state.blocked = false;
    lcd_state.state = LCD_INITIAL;
#ifndef BOOTLOADER
#if NUM_CORES > 1
    corelock_init(&lcd_state.cl);
#endif
#ifdef HAVE_LCD_SLEEP
    if (!flash_get_section(ROM_ID('v', 'm', 'c', 's'),
                           (void **)(&flash_vmcs_offset), &flash_vmcs_length))
        /* BCM cannot be shut down because firmware wasn't found */
        flash_vmcs_length = 0;
    else
    {
        /* lcd_write_data needs an even number of 16 bit values */
        flash_vmcs_length = ((flash_vmcs_length + 3) >> 1) & ~1;
    }
    semaphore_init(&(lcd_state.initwakeup), 1, 0);
    lcd_state.waking = false;

    if (GPO32_VAL & 0x4000)
    {
        /* BCM is powered.  Assume it is initialized. */
        lcd_state.display_on = true;
        tick_add_task(&lcd_tick);
    }
    else
    {
        /* BCM is not powered, so it needs to be initialized.
           This can only happen when loading Rockbox via ROLO.
         */
        lcd_state.update_timeout = current_tick;
        lcd_state.display_on = false;
        lcd_awake();
    }
#else /* !HAVE_LCD_SLEEP */
    tick_add_task(&lcd_tick);
#endif
#endif /* !BOOTLOADER */
}

/*** update functions ***/

/* Update a fraction of the display. */
void lcd_update_rect(int x, int y, int width, int height)
{
    const fb_data *addr;
    unsigned bcmaddr;

#ifdef HAVE_LCD_SLEEP
    if (!lcd_state.display_on)
        return;
#endif

    if (x + width >= LCD_WIDTH)
        width = LCD_WIDTH - x;
    if (y + height >= LCD_HEIGHT)
        height = LCD_HEIGHT - y;

    if ((width <= 0) || (height <= 0))
        return; /* Nothing left to do. */

#ifdef HAVE_COMPOSITE_VIDEO_OUT
    if (tv_on)
    {
        tv_mark_dirty(y, height);
        return;
    }
#endif

    /* Ensure x and width are both even. The BCM doesn't like small unaligned
     * writes and would just ignore them. */
    width = (width + (x & 1) + 1) & ~1;
    x &= ~1;

    /* Prevent the tick from triggering BCM updates while we're writing. */
    lcd_block_tick();

    addr = FBADDR(x, y);
    bcmaddr = BCMA_CMDPARAM + (LCD_WIDTH*2) * y + (x << 1);

    if (width == LCD_WIDTH)
    {
        bcm_write_addr(bcmaddr);
        lcd_write_data(addr, width * height);
    }
    else
    {
        do
        {
            bcm_write_addr(bcmaddr);
            bcmaddr += (LCD_WIDTH*2);
            lcd_write_data(addr, width);
            addr += LCD_WIDTH;
        }
        while (--height > 0);
    }
    lcd_unblock_and_update();
}

/* Update the display.
   This must be called after all other LCD functions that change the display. */
void lcd_update(void)
{
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

/* Line write helper function for lcd_yuv_blit. Writes two lines of yuv420. */
extern void lcd_write_yuv420_lines(unsigned char const * const src[3],
                                   unsigned bcmaddr,
                                   int width,
                                   int stride);

/* Performance function to blit a YUV bitmap directly to the LCD */
void lcd_blit_yuv(unsigned char * const src[3],
                  int src_x, int src_y, int stride,
                  int x, int y, int width, int height)
{
    unsigned bcmaddr;
    off_t z;
    unsigned char const * yuv_src[3];

#ifdef HAVE_LCD_SLEEP
    if (!lcd_state.display_on)
        return;
#endif

    /* Sorry, but width and height must be >= 2 or else */
    width &= ~1;

    z = stride * src_y;
    yuv_src[0] = src[0] + z + src_x;
    yuv_src[1] = src[1] + (z >> 2) + (src_x >> 1);
    yuv_src[2] = src[2] + (yuv_src[1] - src[1]);

    /* Prevent the tick from triggering BCM updates while we're writing. */
    lcd_block_tick();

    bcmaddr = BCMA_CMDPARAM + (LCD_WIDTH*2) * y + (x << 1);
    height >>= 1;

    do
    {
        lcd_write_yuv420_lines(yuv_src, bcmaddr, width, stride);
        bcmaddr += (LCD_WIDTH*4);  /* Skip up two lines */
        yuv_src[0] += stride << 1;
        yuv_src[1] += stride >> 1; /* Skip down one chroma line */
        yuv_src[2] += stride >> 1;
    }
    while (--height > 0);

    lcd_unblock_and_update();
}

#ifdef HAVE_LCD_SLEEP
/* Executes a BCM command immediately and waits for it to complete.
   Other BCM commands (eg. LCD updates or lcd_tick) must not interfere.
 */
static void bcm_command(unsigned cmd)
{
    unsigned status;

    bcm_write32(BCMA_COMMAND,  cmd);

    BCM_CONTROL = 0x31;

    while (1)
    {
        status = bcm_read32(BCMA_COMMAND);
        if (status != cmd && status != 0xFFFF)
            break;
        yield();
    }
}

#ifdef HAVE_COMPOSITE_VIDEO_OUT
/*** TV output ***
 * One bitmap command puts the BCM's TV encoder in PAL or NTSC mode. From
 * then on whatever is in its TV framebuffer is on the TV, so the picture is
 * kept current by writing that memory; another command would drop the
 * signal. The framebuffer is 24-bit BGR at TV levels (16-235), one line of
 * 704 pixels every 2112 bytes, whatever the bitmap's width. An LCD update
 * command disturbs the TV, so the LCD is blanked while the TV runs.
 *
 * Parts: format and state; RGB565 conversion; the writer thread; start and
 * stop; the videoout interface.
 */
#define TV_LINE           (704 * 3)
#define TV_BMP_WIDTH      720
/* The BCM holds back about the last 16 KB written until more arrives. */
#define TV_FLUSH_WORDS    4096

static bool tv_double, tv_pal;          /* the format the next start uses */
static unsigned tv_scale;               /* 1 or 2, while tv_on */
static unsigned tv_first_line, tv_left; /* picture origin: line, bytes */
static unsigned tv_height;              /* lines in the running TV frame */
/* A copy of the LCD framebuffer, taken from core memory while the TV runs:
 * taking it rebuffers playback once. NULL sends from the LCD's own. */
static fb_data *tv_snapshot;
static int tv_snapshot_handle;
/* Rows changed since the writer last ran, [top, bottom). */
static int tv_dirty_top = LCD_HEIGHT, tv_dirty_bottom;
static struct semaphore tv_wake;
/* Held while the TV framebuffer is written or the TV mode is changed. */
static struct mutex tv_lock;
static long tv_stack[DEFAULT_STACK_SIZE / sizeof(long)];
static bool tv_thread_running;

/* RGB565 channels to 8 bits at TV levels. */
static unsigned char tv_lvl5[32] IBSS_ATTR, tv_lvl6[64] IBSS_ATTR;

/* One LCD row, 4 pixels to 3 words. */
static void ICODE_ATTR __attribute__((noinline))
tv_write_row_1x(const fb_data *p)
{
    const unsigned long *in = (const unsigned long *)p;
    const unsigned char *l5 = tv_lvl5, *l6 = tv_lvl6;

    for (int n = LCD_WIDTH / 4; n > 0; n--)
    {
        unsigned long a = *in++, b = *in++;
        BCM_DATA32 = l5[a & 31] | l6[(a >> 5) & 63] << 8 |
                     l5[(a >> 11) & 31] << 16 | l5[(a >> 16) & 31] << 24;
        BCM_DATA32 = l6[(a >> 21) & 63] | l5[a >> 27] << 8 |
                     l5[b & 31] << 16 | l6[(b >> 5) & 63] << 24;
        BCM_DATA32 = l5[(b >> 11) & 31] | l5[(b >> 16) & 31] << 8 |
                     l6[(b >> 21) & 63] << 16 | l5[b >> 27] << 24;
    }
}

/* One LCD row with every pixel doubled, 2 pixels to 3 words. */
static void ICODE_ATTR __attribute__((noinline))
tv_write_row_2x(const fb_data *p)
{
    const unsigned long *in = (const unsigned long *)p;
    const unsigned char *l5 = tv_lvl5, *l6 = tv_lvl6;

    for (int n = LCD_WIDTH / 2; n > 0; n--)
    {
        unsigned long a = *in++;
        unsigned b0 = l5[a & 31], g0 = l6[(a >> 5) & 63],
                 r0 = l5[(a >> 11) & 31];
        unsigned b1 = l5[(a >> 16) & 31], g1 = l6[(a >> 21) & 63],
                 r1 = l5[a >> 27];
        BCM_DATA32 = b0 | g0 << 8 | r0 << 16 | b0 << 24;
        BCM_DATA32 = g0 | r0 << 8 | b1 << 16 | g1 << 24;
        BCM_DATA32 = r1 | b1 << 8 | g1 << 16 | r1 << 24;
    }
}

/* Copies LCD rows to the TV, from the snapshot when there is one. At 2x it
 * lets other threads run every 16 TV lines, since threads switch only when
 * one yields and a full frame takes over 100 ms. A 1x pass runs straight
 * through, to finish inside the two fields that keep it from tearing. */
static void tv_write_rows(int y, int height)
{
    unsigned line = tv_first_line + y * tv_scale;

    for (int row = y; row < y + height; row++)
    {
        const fb_data *p = tv_snapshot ? tv_snapshot + row * LCD_WIDTH
                                       : FBADDR(0, row);
        for (unsigned k = 0; k < tv_scale; k++, line++)
        {
            bcm_write_addr(BCMA_TV_FB + line * TV_LINE + tv_left);
            if (tv_scale == 2)
                tv_write_row_2x(p);
            else
                tv_write_row_1x(p);
        }
        if (tv_scale == 2 && (line & 15) < tv_scale)
            yield();
    }

    bcm_write_addr(BCMA_TV_FB +
                   (tv_first_line + LCD_HEIGHT * tv_scale) * TV_LINE);
    for (int n = 0; n < TV_FLUSH_WORDS; n++)
        BCM_DATA32 = 0;
}

static void tv_mark_dirty(int y, int height)
{
    int oldlevel = disable_irq_save();
    if (y < tv_dirty_top)
        tv_dirty_top = y;
    if (y + height > tv_dirty_bottom)
        tv_dirty_bottom = y + height;
    restore_irq(oldlevel);
    semaphore_release(&tv_wake);
}

/* The BCM's TV scan line is in bits 16-25 of this register. It counts up
 * through each field and starts again at 0 with the next. */
#define BCMA_TV_SCANLINE  0x100008ac

/* Returns at the start of a field, or after a field's time if the scan line
 * never wraps. A pass started here and finished within two fields does not
 * tear: the field being drawn stays ahead of the writing and shows only the
 * old picture, and the next field starts behind it and shows only the new.
 * Other threads run while it waits. */
static void tv_wait_field_start(void)
{
    unsigned prev = (bcm_read32(BCMA_TV_SCANLINE) >> 16) & 0x3ff;
    long end = current_tick + HZ/20 + 1;

    while (TIME_BEFORE(current_tick, end))
    {
        unsigned line = (bcm_read32(BCMA_TV_SCANLINE) >> 16) & 0x3ff;
        if (line < prev)
            return;
        prev = line;
        yield();
    }
}

/* Copies changed rows to the TV, boosted. A pass waits a tick for the UI to
 * finish a burst of updates, and after it the thread rests at least as long
 * as the pass took, and to a 30th of a second: a screen that is still being
 * built, like the WPS, updates over and over, and back-to-back passes
 * starve it. Changes that arrive meanwhile are merged into the next pass.
 *
 * Each pass first copies the changed rows into the snapshot without
 * yielding, so it sends one whole UI frame. Sent straight from the LCD
 * framebuffer, a pass that yields while the carousel animates puts bands of
 * several frames on the TV at once. */
static void tv_thread(void)
{
    while (1)
    {
        semaphore_wait(&tv_wake, TIMEOUT_BLOCK);
        sleep(1);

        long start = current_tick;
        mutex_lock(&tv_lock);
        if (tv_on && tv_scale == 1)
            tv_wait_field_start();

        int oldlevel = disable_irq_save();
        int top = tv_dirty_top, bottom = tv_dirty_bottom;
        tv_dirty_top = LCD_HEIGHT;
        tv_dirty_bottom = 0;
        restore_irq(oldlevel);

        if (tv_on && top < bottom)
        {
            if (tv_snapshot)
                for (int row = top; row < bottom; row++)
                    memcpy(tv_snapshot + row * LCD_WIDTH, FBADDR(0, row),
                           LCD_WIDTH * sizeof(fb_data));
            cpu_boost(true);
            lcd_block_tick();
            tv_write_rows(top, bottom - top);
            lcd_unblock_tick();
            cpu_boost(false);
        }
        mutex_unlock(&tv_lock);

        long took = current_tick - start;
        long wait = MAX(took, start + HZ/30 - current_tick);
        if (wait > 0)
            sleep(wait);
    }
}

/* A black 24-bit bitmap of the TV's size at BCMA_TV_BMPDATA: the bitmap
 * command reads its size and standard from here. */
static void tv_write_black_bmp(int height)
{
    unsigned size = TV_BMP_WIDTH * height * 3;
    const unsigned hdr[14] =
    {
        /* "BM", file size, reserved, data offset 54, header size 40, width,
         * height (positive: bottom-up; top-down crashes the BCM), 1 plane,
         * 24 bpp, uncompressed, data size, 2835 px/m twice, no palette.
         * Packed little-endian into words, with 2 bytes of the data. */
        0x4d42 | (54 + size) << 16, (54 + size) >> 16, 54 << 16,
        40 << 16, TV_BMP_WIDTH << 16, height << 16, 1 << 16,
        24, size << 16, size >> 16 | 2835 << 16, 2835 << 16, 0, 0, 0
    };

    bcm_write_addr(BCMA_TV_BMPDATA);
    for (unsigned i = 0; i < ARRAYLEN(hdr); i++)
        BCM_DATA32 = hdr[i];
    for (unsigned i = 56; i < 54 + size; i += 4)
        BCM_DATA32 = 0;
}

/* Black over the whole TV frame. The bitmap command leaves stray lines
 * outside the picture, and only a 2x picture covers them. Rewriting the
 * frame's first 16 KB, already black, pushes its last lines out of the BCM's
 * cache. */
static void tv_wipe(unsigned height)
{
    bcm_write_addr(BCMA_TV_FB);
    for (unsigned i = 0; i < height * TV_LINE / 4; i++)
        BCM_DATA32 = 0x10101010;
    bcm_write_addr(BCMA_TV_FB);
    for (int n = 0; n < TV_FLUSH_WORDS; n++)
        BCM_DATA32 = 0x10101010;
}

static void tv_start(void)
{
    unsigned height = tv_pal ? 576 : 480;

    if (!tv_thread_running)
    {
        for (int i = 0; i < 32; i++)
            tv_lvl5[i] = 16 + (i * 219 + 15) / 31;
        for (int i = 0; i < 64; i++)
            tv_lvl6[i] = 16 + (i * 219 + 31) / 63;
        semaphore_init(&tv_wake, 1, 0);
        mutex_init(&tv_lock);
        create_thread(tv_thread, tv_stack, sizeof(tv_stack), 0, "tv out"
                      IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
        tv_thread_running = true;
    }

    if (tv_snapshot == NULL)
    {
        int handle = core_alloc(LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data));
        if (handle > 0)
        {
            core_pin(handle);
            tv_snapshot_handle = handle;
            tv_snapshot = core_get_data_pinned(handle);
        }
    }

    /* A sleeping BCM is powered down and would never answer a command. */
    if (!lcd_state.display_on)
        lcd_awake();

    mutex_lock(&tv_lock);
    lcd_block_tick();
    while (lcd_state.state == LCD_UPDATING &&
           !TIME_AFTER(current_tick, lcd_state.update_timeout))
    {
        unsigned d = bcm_read32(BCMA_COMMAND);
        if (d != BCMCMD_LCD_UPDATE && d != 0xFFFF)
            break;
        yield();
    }
    lcd_state.state = LCD_IDLE;

    /* The LCD keeps its last picture, so give it a black one first: once
     * the TV runs, an LCD update would disturb it. BCMCMD_LCD_SLEEP is no
     * substitute: it turns the panel white. */
    bcm_write_addr(BCMA_CMDPARAM);
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT / 2; i++)
        BCM_DATA32 = 0;
    bcm_command(BCMCMD_LCD_UPDATE);

    tv_write_black_bmp(height);
    bcm_command(tv_pal ? BCMCMD_TV_PALBMP : BCMCMD_TV_NTSCBMP);
    tv_wipe(height);
    tv_height = height;

    tv_bcm_mode = true;
    tv_scale = tv_double ? 2 : 1;
    tv_first_line = (height - LCD_HEIGHT * tv_scale) / 2;
    tv_left = (704 - LCD_WIDTH * tv_scale) / 2 * 3;
    tv_on = true;
    lcd_unblock_tick();
    mutex_unlock(&tv_lock);

    tv_mark_dirty(0, LCD_HEIGHT);
}

/* No command ends TV output, so the TV is blacked out and the LCD is
 * updated again: it takes updates in TV mode, at the cost of disturbing a
 * picture that is now black. */
static void tv_stop(void)
{
    mutex_lock(&tv_lock);
    lcd_block_tick();
    tv_wipe(tv_height);
    tv_on = false;
    if (tv_snapshot)
    {
        tv_snapshot = NULL;
        core_free(tv_snapshot_handle);
    }
    lcd_unblock_tick();
    mutex_unlock(&tv_lock);

    lcd_update();
}

void videoout_set_mode(enum videoout_mode mode, const void *framebuffer,
                       int width, int height)
{
    (void)framebuffer;
    (void)width;
    (void)height;

    if (mode != VIDEOOUT_OFF && !tv_on)
        tv_start();
    else if (mode == VIDEOOUT_OFF && tv_on)
        tv_stop();
}

void videoout_set_format(bool double_size, bool pal)
{
    bool changed = double_size != tv_double || pal != tv_pal;

    tv_double = double_size;
    tv_pal = pal;
    /* A bitmap command switches a running TV to the new standard. */
    if (changed && tv_on)
        tv_start();
}

bool videoout_active(void)
{
    return tv_on;
}

bool videoout_disable(void)
{
    if (tv_on)
        tv_stop();
    return true;
}
#endif /* HAVE_COMPOSITE_VIDEO_OUT */

static void bcm_powerdown(void)
{
    /* Immediately switch off the backlight to avoid flashing. */
    _backlight_hw_enable(false);
    
    /* Not sure what this does. */
    bcm_write32(0x10001400, bcm_read32(0x10001400) & ~0xF0);

    /* Blanks the LCD and decreases power consumption
       below what clearing the LCD would achieve.
       Executing an LCD update command wakes it.
     */
    bcm_command(BCMCMD_LCD_SLEEP);

    /* Not sure if this does anything */
    bcm_command(BCM_CMD(0xC));

    /* Further cuts power use, probably by powering down BCM.
       After this point, BCM needs to be bootstrapped
     */
    GPO32_VAL &= ~0x4000;
}

/* Data written to BCM_CONTROL and BCM_ALT_CONTROL */
const unsigned char bcm_bootstrapdata[] =
{
    0xA1, 0x81, 0x91, 0x02, 0x12, 0x22, 0x72, 0x62
};

static void bcm_init(void)
{
    int i;

    /* Power up BCM */
    GPO32_VAL |= 0x4000;
    sleep(HZ/20);

    /* Bootstrap stage 1 */

    STRAP_OPT_A &= ~0xF00;
    outl(0x1313, 0x70000040);

    /* Interrupt-related code for future use
       GPIOC_INT_LEV |= 0x40;
       GPIOC_INT_EN |= 0x40;
       CPU_HI_INT_EN |= 0x40000;
    */

    /* Bootstrap stage 2 */

    while (BCM_ALT_CONTROL & 0x80);
    while (!(BCM_ALT_CONTROL & 0x40));

    for (i = 0; i < 8; i++)
    {
        BCM_CONTROL = bcm_bootstrapdata[i];
    }

    for (i = 3; i < 8; i++)
    {
        BCM_ALT_CONTROL = bcm_bootstrapdata[i];
    }

    while ((BCM_RD_ADDR & 1) == 0 || (BCM_ALT_RD_ADDR & 1) == 0);

    (void)BCM_WR_ADDR;
    (void)BCM_ALT_WR_ADDR;

    /* Bootstrap stage 3: upload firmware */

    while (BCM_ALT_CONTROL & 0x80);
    while (!(BCM_ALT_CONTROL & 0x40));

    /* Upload firmware to BCM SRAM */
    bcm_write_addr(BCMA_SRAM_BASE);
    lcd_write_data(flash_vmcs_offset, flash_vmcs_length);

    bcm_write32(BCMA_COMMAND,  0);
    bcm_write32(0x10000C00, 0xC0000000);

    while (!(bcm_read32(0x10000C00) & 1));

    bcm_write32(0x10000C00, 0);
    bcm_write32(0x10000400, 0xA5A50002);

    while (bcm_read32(BCMA_COMMAND) == 0)
        yield();

    /* sleep(HZ/2) apparently unneeded */
}

void lcd_awake(void)
{
    if (!lcd_state.display_on && flash_vmcs_length != 0)
    {
        /* Ensure BCM has been off for >= 50 ms */
        long sleepwait = lcd_state.update_timeout + HZ/20 - current_tick;
        if (sleepwait > 0 && sleepwait < HZ/20)
            sleep(sleepwait);

        bcm_init();

        /* Start the first LCD update, which also initializes the LCD */
        lcd_state.state = LCD_INITIAL;
        lcd_state.display_on = true;
        lcd_update();
        lcd_state.update_timeout = current_tick + BCM_LCDINIT_TIMEOUT;

        /* Wait for end of first LCD update, so LCD isn't white
           when the backlight turns on.
         */
        lcd_state.waking = true;
        tick_add_task(&lcd_tick);
        semaphore_wait(&(lcd_state.initwakeup), TIMEOUT_BLOCK);

        send_event(LCD_EVENT_ACTIVATION, NULL);
    }
}

void lcd_sleep(void)
{
#ifdef HAVE_COMPOSITE_VIDEO_OUT
    if (tv_bcm_mode)
        return;
#endif
    if (lcd_state.display_on && flash_vmcs_length != 0)
    {
        lcd_state.display_on = false;

        /* Wait for BCM to finish work */
        while (lcd_state.state != LCD_INITIAL && lcd_state.state != LCD_IDLE)
            yield();

        tick_remove_task(&lcd_tick);
        bcm_powerdown();

        /* Remember time to ensure BCM stays off for >= 50 ms */
        lcd_state.update_timeout = current_tick;
    }
}

bool lcd_active(void)
{
    return lcd_state.display_on;
}

#ifdef HAVE_LCD_SHUTDOWN
void lcd_shutdown(void)
{
    lcd_sleep();
}
#endif /* HAVE_LCD_SHUTDOWN */
#endif /* HAVE_LCD_SLEEP */
