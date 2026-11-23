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
 * kernel-sdl.c for the simulator compiled to WebAssembly. Nothing runs
 * beside the Rockbox threads, so the "interrupts" -- the tick and the SDL
 * events -- are delivered at sim_emscripten_poll(), which every thread
 * switch and every idle wait calls with interrupts enabled. The page itself
 * draws and takes input only while the module sleeps, so the poll sleeps
 * when the module has not let the page run for a frame.
 *
 ****************************************************************************/

#include <emscripten.h>
#include <SDL.h>
#include "system.h"
#include "kernel.h"
#include "button-sdl.h"
#include "system-sdl.h"

#define FRAME_MS 16

long start_tick;
static int interrupt_level = HIGHEST_IRQ_LEVEL;
static bool ticking;
static double last_yield;

int set_irq_level(int level)
{
    int oldlevel = interrupt_level;
    interrupt_level = level;
    return oldlevel;
}

/* Nothing preempts a handler here, so a handler needs no exclusion. */
void sim_enter_irq_handler(void)
{
}

void sim_exit_irq_handler(void)
{
}

bool sim_kernel_init(void)
{
    return true;
}

void sim_kernel_shutdown(void)
{
    ticking = false;
}

void tick_start(unsigned int interval_in_ms)
{
    (void)interval_in_ms;
    start_tick = SDL_GetTicks();
    ticking = true;
}

/* Lets the page run and comes straight back. A message rather than
 * emscripten_sleep(0): a timer set from inside a timer waits at least 4 ms. */
EM_ASYNC_JS(void, yield_to_page, (void), {
    await new Promise(resolve => {
        const channel = new MessageChannel();
        channel.port1.onmessage = () => resolve();
        channel.port2.postMessage(0);
    });
});

void sim_emscripten_poll(void)
{
    if (interrupt_level != 0)
        return;

    double now = emscripten_get_now();
    if (now - last_yield >= FRAME_MS)
    {
        yield_to_page();
        last_yield = emscripten_get_now();
    }

    interrupt_level = HIGHEST_IRQ_LEVEL;

    SDL_Event event;
    while (SDL_PollEvent(&event))
        sdl_event_filter(NULL, &event);

    if (ticking)
    {
        long due = (SDL_GetTicks() - start_tick) / (1000/HZ);
        while (current_tick != due)
            call_tick_tasks();
    }

    interrupt_level = 0;
}

void wait_for_interrupt(void)
{
    long ms = (1000/HZ) - (SDL_GetTicks() - start_tick) % (1000/HZ);
    emscripten_sleep(ms);
    last_yield = emscripten_get_now();
    sim_emscripten_poll();
}
