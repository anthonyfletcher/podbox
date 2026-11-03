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

/* Hang catcher. Threads are cooperative, so one that never gives up the
 * core starves every other: no sound, no backlight, no buttons, and no
 * panic. The scheduler handles timed waits each time it switches, so a
 * timed wait several seconds overdue says nothing has switched for that
 * long. The tick then sends that thread to a screen naming it, with a
 * backtrace from the instruction the tick interrupted. A hang with
 * interrupts off stops the tick too, and is not caught: a frozen player with
 * no such screen is evidence of that.
 *
 * A deadlock lets the core switch freely, so the tick watches for it apart:
 * the UI thread blocked with no time limit while key presses wait unread.
 * A wait for a key ends at the first one, so this is a wait on another
 * thread or a lock that has not come back, and the screen lists what every
 * thread was doing. */

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "power.h"
#include "rbversion.h"
#include "backtrace.h"
#include "safe_read.h"
#include "string-extra.h"
#include "thread-internal.h"
#include "usb_log.h"
#include "button.h"
#include "hang_catch.h"

#define HANG_SECS   5
/* Longer than the core check: the UI thread can wait this long on the
 * audio thread through slow disk work, where a thread that never yields
 * has no excuse at all. Long work done on the UI thread itself, such as a
 * playlist being built, keeps it running and never counts. */
#define STUCK_SECS  20

static const uint32_t *irq_stack_top;

/* Whether the word before a is a call: BL, BX or BLX by register, or a load
 * into pc after a "mov lr, pc" */
static bool after_call(uint32_t a)
{
    uint32_t i;

    /* past the exception vectors, whose loads into pc would pass */
    if ((a & 3) || a < 0x100 || !safe_read32((uint32_t *)(a - 4), &i))
        return false;
    return (i & 0x0f000000) == 0x0b000000 ||
           (i & 0x0fffffd0) == 0x012fff10 ||
           (i & 0x0c10f000) == 0x0410f000;
}

/* The unwinder steps forward to a function's return, which a loop that
 * never ends never reaches. The words on the stack that follow a call are
 * the callers, with some stale ones among them, from the innermost. */
#define STACK_CALLS 32
static int stack_scan(uint32_t sp, uint32_t top, uint32_t *found, int most)
{
    uint32_t w;
    int n = 0;

    for (; sp < top && n < most; sp += 4)
        if (safe_read32((uint32_t *)sp, &w) && after_call(w))
            found[n++] = w;
    return n;
}

/* Where the stuck thread was, taken by the tick */
static const struct thread_entry *hang_thread;
static uint32_t hang_pc, hang_sp, hang_lr;

/* The UI thread, which starts the catcher; when it was first seen blocked
 * with keys unread, or 0; and whether that, not the core, is the report */
static const struct thread_entry *ui_thread;
static long stuck_since;
static bool hang_stuck;

static char state_letter(unsigned char state)
{
    switch (state)
    {
        case STATE_RUNNING:       return 'R';
        case STATE_BLOCKED:       return 'B';
        case STATE_SLEEPING:      return 'S';
        case STATE_BLOCKED_W_TMO: return 'T';
        case STATE_FROZEN:        return 'F';
        default:                  return '?';
    }
}

static void name8(const struct thread_entry *t, char name[8])
{
    memset(name, 0, 8);
    strlcpy(name, t && t->name ? t->name : "?", 8);
}

/* Every live thread: its name and state, the return address its switch
 * saved -- inside the call it is waiting in -- and the first caller found
 * on its stack. The thread the tick interrupted is drawing this, so its
 * saved registers are stale; it is marked. */
static void stuck_log(void)
{
    char name[8];

    usb_log(USB_LOG_HANG, 3, 0, STUCK_SECS, 0);
    for (unsigned i = 0; i < MAXTHREADS; i++)
    {
        const struct thread_entry *t = __thread_slot_entry(i);
        uint32_t call = 0;

        if (t->state == STATE_KILLED)
            continue;
        stack_scan(t->context.sp, (uint32_t)t->stack + t->stack_size, &call,
                   1);
        name8(t, name);
        const uint8_t *c = (const uint8_t *)name;
        usb_log(USB_LOG_HANG, 4, state_letter(t->state),
                (uint32_t)c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3],
                (uint32_t)c[4] << 24 | c[5] << 16 | c[6] << 8 | c[7]);
        usb_log(USB_LOG_HANG, 5, t == hang_thread, t->context.lr, call);
    }
    usb_log_sync();
}

static void stuck_draw(unsigned int *y)
{
    lcd_putsf(1, (*y)++, "%s blocked %ds, keys unread",
              ui_thread && ui_thread->name ? ui_thread->name : "?",
              STUCK_SECS);
    lcd_puts(1, (*y)++, "thread  s lr       calls");
    for (unsigned i = 0; i < MAXTHREADS; i++)
    {
        const struct thread_entry *t = __thread_slot_entry(i);
        uint32_t calls[2] = { 0, 0 };
        char name[8];

        if (t->state == STATE_KILLED)
            continue;
        stack_scan(t->context.sp, (uint32_t)t->stack + t->stack_size,
                   calls, 2);
        name8(t, name);
        lcd_putsf(1, (*y)++, "%-7.7s %c %08lx %08lx %08lx", name,
                  t == hang_thread ? '*' : state_letter(t->state),
                  (unsigned long)t->context.lr, (unsigned long)calls[0],
                  (unsigned long)calls[1]);
    }
}

/* The report goes to the USB log first, and waits up to three seconds for
 * the log's writer: once interrupts are off for the screen, nothing else
 * runs. */
static void hang_log(const struct thread_entry *t, const uint32_t *calls,
                     int n)
{
    char name[8] = "?";

    if (t && t->name)
        strlcpy(name, t->name, sizeof name);
    const uint8_t *c = (const uint8_t *)name;

    usb_log(USB_LOG_HANG, 0, 0,
            (uint32_t)c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3],
            (uint32_t)c[4] << 24 | c[5] << 16 | c[6] << 8 | c[7]);
    usb_log(USB_LOG_HANG, 1, 0, hang_pc, hang_lr);
    for (int i = 0; i < n; i += 2)
        usb_log(USB_LOG_HANG, 2, i / 2, calls[i],
                i + 1 < n ? calls[i + 1] : 0);
    usb_log_sync();
}

/* Entered by the stuck thread itself, as the tick returned it here rather
 * than to its loop: the report waits on the log's writer, which an
 * interrupt cannot. */
static void NORETURN_ATTR hang_show(void)
{
    const struct thread_entry *t = hang_thread;
    uint32_t calls[STACK_CALLS];
    unsigned int y = 1;
    int n = t ? stack_scan(hang_sp, (uint32_t)t->stack + t->stack_size,
                           calls, STACK_CALLS) : 0;

    if (hang_stuck)
        stuck_log();
    else
        hang_log(t, calls, n);
    disable_interrupt(IRQ_FIQ_STATUS);
    lcd_set_viewport(NULL);
#if LCD_DEPTH > 1
    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
#endif
    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);
    lcd_puts(1, y++, "*HANG* (" RBVERSION ")");
    if (hang_stuck)
        stuck_draw(&y);
    else
    {
        lcd_putsf(1, y++, "%s held the core %ds",
                  t && t->name ? t->name : "?", HANG_SECS);
        lcd_putsf(1, y++, "lr:%08lx", (unsigned long)hang_lr);
        rb_backtrace(hang_pc, hang_sp, &y);
        lcd_puts(1, y++, "stack:");
        for (int i = 0; i < n; i += 4)
            lcd_putsf(1, y++, "%08lx %08lx %08lx %08lx",
                      (unsigned long)calls[i],
                      (unsigned long)(i + 1 < n ? calls[i + 1] : 0),
                      (unsigned long)(i + 2 < n ? calls[i + 2] : 0),
                      (unsigned long)(i + 3 < n ? calls[i + 3] : 0));
    }
    lcd_update();

    /* Waiting for a button, as a panic leaves it */
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (cpu_boost_lock())
    {
        set_cpu_frequency(0);
        cpu_boost_unlock();
    }
#endif
#ifdef HAVE_ATA_POWER_OFF
    ide_power_enable(false);
#endif
    system_exception_wait();
    system_reboot();
    while (1);
}

static void hang_tick(void)
{
    struct core_entry *corep = __core_id_entry(CURRENT_CORE);
    uint32_t *ret = (uint32_t *)irq_stack_top - 1;

    if (hang_thread)
        return;
    if (ui_thread && ui_thread->state == STATE_BLOCKED
        && button_queue_count() > 0)
    {
        if (!stuck_since)
            stuck_since = current_tick;
        else if (TIME_AFTER(current_tick, stuck_since + STUCK_SECS * HZ))
            hang_stuck = true;
    }
    else
        stuck_since = 0;
    if (!hang_stuck &&
        !TIME_AFTER(current_tick, corep->next_tmo_check + HANG_SECS * HZ))
        return;
    /* The IRQ handler's prologue pushes lr, already the interrupted pc,
     * highest of its registers, so at the top of the IRQ stack; its return
     * loads pc from there. The thread ran in system mode, whose sp and lr
     * are banked away from IRQ mode's. */
    hang_thread = corep->running;
    hang_pc = *ret;
    asm volatile ("mrs r2, cpsr      \n"
                  "msr cpsr_c, #0xdf \n"
                  "mov %0, sp        \n"
                  "mov %1, lr        \n"
                  "msr cpsr_c, r2    \n"
                  : "=r" (hang_sp), "=r" (hang_lr) : : "r2");
    *ret = (uint32_t)hang_show;
}

void hang_catch_init(void)
{
    int oldlevel = disable_irq_save();

    /* Outside an interrupt, IRQ mode's banked sp rests at its stack's top */
    asm volatile ("mrs r2, cpsr      \n"
                  "msr cpsr_c, #0xd2 \n"
                  "mov %0, sp        \n"
                  "msr cpsr_c, r2    \n"
                  : "=r" (irq_stack_top) : : "r2");
    restore_irq(oldlevel);
    ui_thread = __core_id_entry(CURRENT_CORE)->running;
    tick_add_task(hang_tick);
}

void hang_catch_stop(void)
{
    tick_remove_task(hang_tick);
}
