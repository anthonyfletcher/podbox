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
 * no such screen is evidence of that. */

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "rbversion.h"
#include "backtrace.h"
#include "safe_read.h"
#include "thread-internal.h"
#include "hang_catch.h"

#define HANG_SECS   5

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
 * the callers, with some stale ones among them: four to a line, from the
 * innermost. */
static void stack_scan(uint32_t sp, uint32_t top, unsigned int *y)
{
    uint32_t found[4], w;
    int n = 0, lines = 0;

    lcd_puts(1, (*y)++, "stack:");
    for (; sp < top && lines < 8; sp += 4)
    {
        if (!safe_read32((uint32_t *)sp, &w) || !after_call(w))
            continue;
        found[n++] = w;
        if (n == 4 || sp + 4 >= top)
        {
            lcd_putsf(1, (*y)++, "%08lx %08lx %08lx %08lx",
                      (unsigned long)found[0],
                      (unsigned long)(n > 1 ? found[1] : 0),
                      (unsigned long)(n > 2 ? found[2] : 0),
                      (unsigned long)(n > 3 ? found[3] : 0));
            n = 0;
            lines++;
        }
    }
    if (n)
        lcd_putsf(1, (*y)++, "%08lx %08lx %08lx", (unsigned long)found[0],
                  (unsigned long)(n > 1 ? found[1] : 0),
                  (unsigned long)(n > 2 ? found[2] : 0));
}

/* Where the stuck thread was, taken by the tick */
static const struct thread_entry *hang_thread;
static uint32_t hang_pc, hang_sp, hang_lr;

/* Entered by the stuck thread itself, as the tick returned it here rather
 * than to its loop: drawing from the tick would wait on the 6G's LCD DMA,
 * which does not finish inside an interrupt. */
static void NORETURN_ATTR hang_show(void)
{
    const struct thread_entry *t = hang_thread;
    unsigned int y = 1;

    disable_interrupt(IRQ_FIQ_STATUS);
    lcd_set_viewport(NULL);
#if LCD_DEPTH > 1
    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
#endif
    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);
    lcd_puts(1, y++, "*HANG* (" RBVERSION ")");
    lcd_putsf(1, y++, "%s held the core %ds", t && t->name ? t->name : "?",
              HANG_SECS);
    lcd_putsf(1, y++, "lr:%08lx", (unsigned long)hang_lr);
    rb_backtrace(hang_pc, hang_sp, &y);
    if (t)
        stack_scan(hang_sp, (uint32_t)t->stack + t->stack_size, &y);
    lcd_update();

    system_exception_wait();
    system_reboot();
    while (1);
}

static void hang_tick(void)
{
    struct core_entry *corep = __core_id_entry(CURRENT_CORE);
    uint32_t *ret = (uint32_t *)irq_stack_top - 1;

    if (hang_thread ||
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
    tick_add_task(hang_tick);
}
