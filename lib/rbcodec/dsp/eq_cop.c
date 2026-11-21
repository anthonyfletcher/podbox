/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * The equalizer's worker on the COP. One job at a time: the caller fills
 * the job, releases the worker and waits on it. The job and the semaphores
 * are uncached SDRAM, and the filters are in IRAM, which neither core
 * caches. A block in IRAM or uncached SDRAM needs nothing more; one in
 * cached SDRAM (a codec run on the COP hands the DSP such a block) is
 * written back and dropped by the CPU, and dropped, filtered and written
 * back by the COP: whole-cache operations, the only kind the PP5022 has.
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
#include "eq_cop.h"

#if NUM_CORES > 1 && (CONFIG_PLATFORM & PLATFORM_NATIVE)
#include "system.h"
#include "kernel.h"

/* PP502x: IRAM sits at 0x40000000; SDRAM's uncached alias at
   UNCACHED_BASE_ADDR */
#define IRAM_BASE   0x40000000u
#define IRAM_END    0x40020000u

/* A healthy block takes well under a millisecond */
#define JOB_TIMEOUT HZ

static struct
{
    eq_cop_bands_fn bands;
    int32_t *buf[2];
    int count;
    unsigned int channels;
    bool cached;    /* the block is in cached SDRAM */
} job SHAREDBSS_ATTR;

static struct semaphore job_sem SHAREDBSS_ATTR;
static struct semaphore done_sem SHAREDBSS_ATTR;

/* The COP's alone once created (create_thread writes the CPU's cache back) */
static long worker_stack[DEFAULT_STACK_SIZE / sizeof(long)];

/* The CPU's alone */
static struct mutex eq_mutex;
static bool mutex_ready;
static enum eq_cop_state state = EQ_COP_NO_THREAD;
static volatile bool job_out;   /* given to the COP and not yet back */

static void eq_cop_worker(void)
{
    while (1)
    {
        semaphore_wait(&job_sem, TIMEOUT_BLOCK);
        /* A block in cached SDRAM: the CPU wrote it back and dropped it;
           this core drops any older copy of its lines, and writes the
           filtered block back for the CPU to read */
        if (job.cached)
            commit_discard_dcache();
        job.bands(job.buf, job.count, job.channels);
        if (job.cached)
            commit_dcache();
        semaphore_release(&done_sem);
    }
}

void eq_cop_init(void)
{
    semaphore_init(&job_sem, 1, 0);
    semaphore_init(&done_sem, 1, 0);

    if (create_thread(eq_cop_worker, worker_stack, sizeof(worker_stack), 0,
                      "eq cop" IF_PRIO(, PRIORITY_PLAYBACK), COP) != 0)
        state = EQ_COP_RUNNING;
}

static bool coherent(const void *p)
{
    uintptr_t a = (uintptr_t)p;

    return (a >= IRAM_BASE && a < IRAM_END)
        || (a & 0xf0000000u) == UNCACHED_BASE_ADDR;
}

bool eq_cop_run(eq_cop_bands_fn bands, int32_t * const buf[], int count,
                unsigned int channels)
{
    /* While the COP still has a block, the filters are its: anything else
       passes unfiltered, as from the USB sound card's interrupt, or after a
       timeout until the late block comes back */
    if (job_out)
    {
        if (!is_thread_context() ||
            semaphore_wait(&done_sem, 0) != OBJ_WAIT_SUCCEEDED)
            return true;
        job_out = false;
    }

    if (state != EQ_COP_RUNNING || !is_thread_context())
        return false;

    job.bands = bands;
    job.buf[0] = buf[0];
    job.buf[1] = buf[1];
    job.count = count;
    job.channels = channels;
    /* A codec run on the COP hands the DSP its PCM in cached SDRAM (the
       host's bounce buffer, codec_cop.c), filtered there in place */
    job.cached = !coherent(buf[0]) || !coherent(buf[1]);
    if (job.cached)
        commit_discard_dcache();

    job_out = true;
    semaphore_release(&job_sem);
    if (semaphore_wait(&done_sem, JOB_TIMEOUT) == OBJ_WAIT_SUCCEEDED)
        job_out = false;
    else
        state = EQ_COP_TIMED_OUT;   /* the next blocks are the CPU's */

    return true;
}

/* Lazily: the settings reach the EQ before the codec thread starts. The USB
   sound card runs the DSP in its interrupt, where no mutex may be taken and
   nothing may wait: there the bands run on the CPU, unlocked, unless the
   COP has a block (eq_cop_run()). */
void eq_cop_lock(void)
{
    if (!is_thread_context())
        return;
    if (!mutex_ready)
    {
        mutex_init(&eq_mutex);
        mutex_ready = true;
    }
    mutex_lock(&eq_mutex);
}

void eq_cop_unlock(void)
{
    if (is_thread_context())
        mutex_unlock(&eq_mutex);
}

enum eq_cop_state eq_cop_state(void)
{
    return state;
}
#endif /* NUM_CORES > 1 && PLATFORM_NATIVE */
