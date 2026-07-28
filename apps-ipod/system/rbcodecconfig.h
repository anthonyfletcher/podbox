/***************************************************************************
 * was: apps/rbcodecconfig.h
 * GNU General Public License (version 2+)
 *
 * Configuration the shared rbcodec library expects from the application.
 ****************************************************************************/
#ifndef RBCODECCONFIG_H_INCLUDED
#define RBCODECCONFIG_H_INCLUDED

/* Explicit path to avoid issues with name clashes (libopus) */
#include "../firmware/export/config.h"

#ifndef __ASSEMBLER__

/* NULL, offsetof, size_t */
#include <stddef.h>

/* ssize_t, off_t, open, close, read, lseek, SEEK_SET, SEEK_CUR, SEEK_END,
 * O_RDONLY, O_WRONLY, O_CREAT, O_APPEND, MAX_PATH, filesize */
#include "file.h"

/* {,u}int{8,16,32,64}_t, , intptr_t, uintptr_t, bool, true, false, swap16,
 * swap32, hto{be,le}{16,32}, {be,le}toh{16,32}, ROCKBOX_{BIG,LITTLE}_ENDIAN,
 * {,U}INT{8,16,32,64}_{MIN,MAX} */
#include "system.h"

/* HZ, TIME_AFTER, current_tick */
#include "kernel.h"

/* MAX_PATH */
#include "fs_defines.h"

/* Structure to record some info during processing call */
struct dsp_loop_context
{
    long last_yield;
};

static inline void dsp_process_start(struct dsp_loop_context *ctx, bool thread_yield)
{
    /* At least perform one yield before starting */
    ctx->last_yield = current_tick;
    if (thread_yield)
    {
        yield();
    }
}

static inline void dsp_process_loop(struct dsp_loop_context *ctx, bool thread_yield)
{
    /* Yield at least once each tick */
    long tick = current_tick;
    if (TIME_AFTER(tick, ctx->last_yield))
    {
        ctx->last_yield = tick;
        if (thread_yield)
        {
            yield();
        }
    }
}

static inline void dsp_process_end(struct dsp_loop_context *ctx)
{
    (void)ctx;
}

#define DSP_PROCESS_START(yield) \
    struct dsp_loop_context __ctx; \
    dsp_process_start(&__ctx, yield)

#define DSP_PROCESS_LOOP(yield) \
    dsp_process_loop(&__ctx, yield)

#define DSP_PROCESS_END() \
    dsp_process_end(&__ctx)

#endif

#define DSP_OUT_MIN_HZ      PLAY_SAMPR_HW_MIN
#define DSP_OUT_MAX_HZ      PLAY_SAMPR_MAX
#define DSP_OUT_DEFAULT_HZ  PLAY_SAMPR_DEFAULT

#endif
