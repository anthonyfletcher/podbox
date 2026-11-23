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

/* Threads for the simulator compiled to WebAssembly: each Rockbox thread is
 * an Emscripten fiber running on the thread's own stack buffer. A browser
 * page has one host thread, so a switch is the only point at which the tick,
 * input and the page itself get a turn; sim_emscripten_poll() in
 * kernel-emscripten.c gives them it. */

#include <stdlib.h>
#include <emscripten/fiber.h>
#include "system.h"

/* Room for a fiber's call stack while Asyncify has it unwound. */
#define ASYNCIFY_STACK_SIZE 0x20000

/* The bottom of a thread's stack stays outside the fiber's: Rockbox's
 * overflow check reads the first word, and Emscripten's stack cookie
 * (an ASSERTIONS build) is written at the fiber's limit. */
#define STACK_GUARD 16

void sim_emscripten_poll(void);

struct fiber
{
    emscripten_fiber_t f;
    char asyncify_stack[ASYNCIFY_STACK_SIZE];
};

/* The fiber being switched away from, or NULL when an exiting thread has
 * no context to keep; it is then saved into the throwaway one. */
static struct fiber *switch_from;
static struct fiber discarded;

#define INIT_MAIN_THREAD

#define THREAD_STARTUP_INIT(core, thread, function) \
    ({ (thread)->context.stack_size = (thread)->stack_size, \
       (thread)->context.stack = (uintptr_t)(thread)->stack; \
       (thread)->context.start = function; })

static void init_main_thread(void *addr)
{
    static struct fiber main_fiber;
    struct regs *context = (struct regs*)addr;
    emscripten_fiber_init_from_current_context(&main_fiber.f,
        main_fiber.asyncify_stack, sizeof (main_fiber.asyncify_stack));
    emscripten_fiber_init_from_current_context(&discarded.f,
        discarded.asyncify_stack, sizeof (discarded.asyncify_stack));
    context->uc = &main_fiber;
    context->stack_size = 0;
}

static void start_thread(void *arg)
{
    void (*func)(void) = arg;
    func();
    thread_exit();
}

static inline void store_context(void* addr)
{
    switch_from = ((struct regs*)addr)->uc;
}

static inline void load_context(const void* addr)
{
    struct regs *context = (struct regs*)addr;
    struct fiber *from = switch_from ? switch_from : &discarded;
    switch_from = NULL;

    if (UNLIKELY(context->start))
    {
        /* A reused thread slot keeps the fiber its last thread had. */
        struct fiber *fiber = context->uc;
        if (fiber == NULL)
            fiber = malloc(sizeof (*fiber));
        emscripten_fiber_init(&fiber->f, start_thread, context->start,
                              (char *)context->stack + STACK_GUARD,
                              context->stack_size - STACK_GUARD,
                              fiber->asyncify_stack,
                              sizeof (fiber->asyncify_stack));
        context->uc = fiber;
        context->start = NULL;
    }

    sim_emscripten_poll();

    if (context->uc != from)
        emscripten_fiber_swap(&from->f, &((struct fiber *)context->uc)->f);
}
