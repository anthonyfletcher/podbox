/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Whether the DSP passes a 16-bit source through bit for bit, and dithers
 * what it must. Noise goes through the real chain, every stage at its
 * default, in the two shapes the codecs hand it: 16-bit interleaved, as
 * the PCM codecs do, and 28-bit words, as FLAC does. Exit status 0 when
 * every case passes.
 *
 * The C output path only: the player's own runs dither_arm.S and
 * dsp_arm.S, which are checked against the C separately.
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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "platform.h"
#include "dsp_core.h"
#include "dsp_proc_entry.h"
#include "dsp_misc.h"

/* What the firmware supplies to the library, as warble.c does */
void panicf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}

void debugf(const char *fmt, ...)
{
    (void)fmt;
}

int find_first_set_bit(uint32_t value)
{
    return value ? __builtin_ctz(value) : 32;
}

off_t ffilesize(int fd)
{
    struct stat st;
    return fstat(fd, &st) ? -1 : st.st_size;
}

#define FRAMES  (3 * 44100)
#define CHUNK   1152        /* frames the codec hands over at a time */

static int16_t in16[2 * FRAMES];    /* the source, as 16-bit */
static int32_t in32[2][FRAMES];     /* the same, as the codec's words */
static int16_t out[3][2 * FRAMES];

/* What a run hands the DSP: a stage can work in its input buffer */
static int16_t work16[2 * FRAMES];
static int32_t work32[2][FRAMES];

static uint32_t seed;

static int32_t noise(int bits)
{
    seed = seed * 1664525u + 1013904223u;
    return (int32_t)seed >> (32 - bits);
}

/* A third of the stream, in whole chunks */
#define THIRD   (FRAMES / 3 / CHUNK * CHUNK)

/* bits of noise in a depth-bit word, the codec's shape for bits > 16;
   bits2 in the middle third */
static void make_source2(int bits, int bits2, int depth)
{
    for (int i = 0; i < FRAMES; i++)
        for (int ch = 0; ch < 2; ch++)
        {
            int b = i >= THIRD && i < 2 * THIRD ? bits2 : bits;
            int32_t v = noise(b) >> 2;                  /* -12 dBFS */
            in32[ch][i] = v << (depth + 1 - b);
            in16[2 * i + ch] = b == 16 ? v : 0;
        }
}

static void make_source(int bits, int depth)
{
    make_source2(bits, bits, depth);
}

/* The stream through the DSP from a fresh start; returns frames out */
static int run(int16_t *dst, int mode, int depth, int freq, long gain)
{
    struct dsp_config *dsp = dsp_get_config(CODEC_IDX_AUDIO);
    struct replaygain_settings rg = { false, REPLAYGAIN_TRACK, 0 };
    struct dsp_replay_gains gains = { gain, 0, 0, 0 };
    int done = 0;

    memcpy(work16, in16, sizeof work16);
    memcpy(work32, in32, sizeof work32);
    dsp_configure(dsp, DSP_RESET, 0);
    dsp_configure(dsp, DSP_SET_OUT_FREQUENCY, 44100);
    dsp_configure(dsp, DSP_SET_FREQUENCY, freq);
    dsp_configure(dsp, DSP_SET_SAMPLE_DEPTH, depth);
    dsp_configure(dsp, DSP_SET_STEREO_MODE,
                  depth > 16 ? STEREO_NONINTERLEAVED : STEREO_INTERLEAVED);
    dsp_replaygain_set_settings(&rg);
    dsp_configure(dsp, REPLAYGAIN_SET_GAINS, (intptr_t)&gains);
    dsp_dither_enable(mode == DITHER_OFF ? DITHER_ALWAYS : DITHER_OFF);
    dsp_dither_enable(mode);            /* a change: the history is flushed */
    dsp_configure(dsp, DSP_FLUSH, 0);

    for (int pos = 0; pos < FRAMES; pos += CHUNK)
    {
        struct dsp_buffer src;
        int n = FRAMES - pos < CHUNK ? FRAMES - pos : CHUNK;

        src.remcount = n;
        if (depth > 16)
        {
            src.pin[0] = &work32[0][pos];
            src.pin[1] = &work32[1][pos];
        }
        else
            src.pin[0] = &work16[2 * pos];
        src.proc_mask = 0;

        while (1)
        {
            struct dsp_buffer d;

            d.remcount = 0;
            d.p16out = dst + 2 * done;
            d.bufcount = FRAMES - done;
            dsp_process(dsp, &src, &d, true);
            done += d.remcount;
            if (d.remcount <= 0 && src.remcount <= 0)
                break;
        }
    }
    return done;
}

static int failures;

static void report(const char *name, bool ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failures += !ok;
}

/* The share of the first n frames that differ, in percent */
static int differ(const int16_t *a, const int16_t *b, int n)
{
    int d = 0;
    for (int i = 0; i < 2 * n; i++)
        d += a[i] != b[i];
    return (int)(100LL * d / (2 * n));
}

static void exact_cases(int depth, const char *shape)
{
    char name[96];
    int n;

    make_source(16, depth);
    n = run(out[0], DITHER_AUTO, depth, 44100, 0);
    snprintf(name, sizeof name, "%s: Auto passes it bit for bit", shape);
    report(name, n == FRAMES && !memcmp(out[0], in16, sizeof in16));

    n = run(out[1], DITHER_OFF, depth, 44100, 0);
    snprintf(name, sizeof name, "%s: Off passes it bit for bit", shape);
    report(name, n == FRAMES && !memcmp(out[1], in16, sizeof in16));

    run(out[2], DITHER_ALWAYS, depth, 44100, 0);
    snprintf(name, sizeof name, "%s: Always changes it", shape);
    report(name, differ(out[2], in16, FRAMES) > 30);
}

/* Something the DSP must requantise: Auto dithers it as Always does */
static void dithered_case(const char *name, int bits, int depth, int freq,
                          long gain)
{
    int a, b;

    make_source(bits, depth);
    a = run(out[0], DITHER_AUTO, depth, freq, gain);
    b = run(out[1], DITHER_ALWAYS, depth, freq, gain);
    run(out[2], DITHER_OFF, depth, freq, gain);
    report(name, a == b && a > 0 && !memcmp(out[0], out[1], 4 * a) &&
                 differ(out[0], out[2], a) > 30);
}

/* 16-bit, then 24-bit, then 16-bit again: Auto is exact again after it
   has dithered, from the first block that is exact */
static void switching_case(void)
{
    int n;

    make_source2(16, 24, 28);
    n = run(out[0], DITHER_AUTO, 28, 44100, 0);
    run(out[1], DITHER_OFF, 28, 44100, 0);
    report("16-bit, 24-bit, 16-bit: Auto exact, dithered, exact",
           n == FRAMES &&
           !memcmp(out[0], in16, 4 * THIRD) &&
           differ(out[0] + 2 * THIRD, out[1] + 2 * THIRD, THIRD) > 30 &&
           !memcmp(out[0] + 4 * THIRD, in16 + 4 * THIRD,
                   4 * (FRAMES - 2 * THIRD)));
}

int main(void)
{
    dsp_init();
    seed = 1;

    exact_cases(16, "16-bit PCM");
    exact_cases(28, "16-bit in 28-bit words");

    dithered_case("24-bit: Auto dithers it", 24, 28, 44100, 0);
    dithered_case("16-bit at 48 kHz, resampled: Auto dithers it",
                  16, 28, 48000, 0);
    dithered_case("16-bit with Replaygain -6 dB: Auto dithers it",
                  16, 28, 44100, 1L << 23);
    switching_case();

    printf(failures ? "bitperfect: %d FAILED\n" : "bitperfect: all passed\n",
           failures);
    return failures != 0;
}
