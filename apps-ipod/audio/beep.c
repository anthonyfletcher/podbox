/***************************************************************************
 * Original code from RockBox
 * was: apps/beep.c
 * Copyright (c) 2011 Michael Sevakis
 * GNU General Public License (version 2+)
 *
 * Software beep generation for keyclicks and edge tones, mixed into the
 * PCM stream. Only built where there is no hardware beeper.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "settings/settings.h"
#include "pcm.h"
#include "pcm_mixer.h"
#include "system/app_util.h"
#include "fixedpoint.h"

/** Beep generation, CPU optimized **/
#include "asm/beep.c"

/* Phase accumulator: beep_phase is a 32-bit fixed-point angle that wraps
 * naturally on overflow, and beep_step is how far it advances per sample.
 * The square wave is then just the sign bit of the phase. */
static uint32_t beep_phase;     /* Phase of square wave generator */
static uint32_t beep_step;      /* Step of square wave generator on each sample */
static uint32_t beep_amplitude; /* Amplitude of square wave generator */
static int beep_count;          /* Number of samples remaining to generate */

#define BEEP_COUNT(fs, duration) ((fs) / 1000 * (duration))

/* Reserve enough static space for keyclick to fit in worst case */
#define BEEP_BUF_COUNT  BEEP_COUNT(PLAY_SAMPR_MAX, KEYCLICK_DURATION)
static int16_t beep_buf[BEEP_BUF_COUNT*2] IBSS_ATTR __attribute__((aligned(4)));

/* Callback to generate the beep frames - also don't want inlining of
   call below in beep_play */
static void __attribute__((noinline))
beep_get_more(const void **start, size_t *size)
{
    int count = beep_count;

    if (count > 0)
    {
        count = MIN(count, BEEP_BUF_COUNT);
        beep_count -= count;
        *start = beep_buf;
        *size = count * 2 * sizeof (int16_t);
        beep_generate((void *)beep_buf, count, &beep_phase,
                      beep_step, beep_amplitude);
    }
}

/* Generates a constant square wave sound with a given frequency in Hertz for
   a duration in milliseconds */
void beep_play(unsigned int frequency, unsigned int duration,
               unsigned int amplitude)
{
    mixer_channel_stop(PCM_MIXER_CHAN_BEEP);

    if (frequency == 0 || duration == 0 || amplitude == 0)
        return;

    if (amplitude > INT16_MAX)
        amplitude = INT16_MAX;

    /* Setup the parameters for the square wave generator */
    uint32_t fout = mixer_get_frequency();
    beep_phase = 0;
    /* frequency/fout as a 32-bit fraction: one full turn of phase per wave */
    beep_step = fp_div(frequency, fout, 32);
    beep_count = BEEP_COUNT(fout, duration);

    /* Optimized routines do XOR with phase sign bit in both channels at once */
    beep_amplitude = amplitude | (amplitude << 16); /* Word:|AMP16|AMP16| */

    /* If it fits - avoid cb overhead */
    const void *start;
    size_t size;

    /* Generate first frame here */
    beep_get_more(&start, &size);

    mixer_channel_set_amplitude(PCM_MIXER_CHAN_BEEP, MIX_AMP_UNITY);
    static struct mixer_play_cbs cbs;
    cbs.get_more = beep_count ? beep_get_more : NULL;
    mixer_channel_play_data(PCM_MIXER_CHAN_BEEP, &cbs, start, size);
}
