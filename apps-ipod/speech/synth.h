/***************************************************************************
 * GNU General Public License (version 2 only)
 * Port of NV Speech Player, Copyright (C) 2014 NV Access Limited
 *
 * Interface to synth.c, the formant speech generator.
 ****************************************************************************/

#ifndef __SYNTH_H
#define __SYNTH_H

#include <stdbool.h>
#include <stdint.h>

/* Frame parameters are fixed point with 16 fractional bits: Hz for the
 * frequencies, bandwidths and pitch, 0 to 1 (SYNTH_ONE) for amplitudes. */
typedef int32_t synth_param_t;
#define SYNTH_ONE (1 << 16)

/* One frame's parameters, in NV Speech Player's frame order. */
enum synth_param
{
    SP_VOICE_PITCH = 0,
    SP_VIBRATO_PITCH_OFFSET,    /* fraction of a semitone */
    SP_VIBRATO_SPEED,
    SP_VOICE_TURBULENCE,        /* breathiness */
    SP_GLOTTAL_OPEN_QUOTIENT,   /* fraction of a cycle the glottis is open */
    SP_VOICE_AMPLITUDE,
    SP_ASPIRATION_AMPLITUDE,
    SP_CF1, SP_CF2, SP_CF3, SP_CF4, SP_CF5, SP_CF6, SP_CF_N0, SP_CF_NP,
    SP_CB1, SP_CB2, SP_CB3, SP_CB4, SP_CB5, SP_CB6, SP_CB_N0, SP_CB_NP,
    SP_CA_NP,                   /* cascade nasal pole amplitude */
    SP_FRICATION_AMPLITUDE,
    SP_PF1, SP_PF2, SP_PF3, SP_PF4, SP_PF5, SP_PF6,
    SP_PB1, SP_PB2, SP_PB3, SP_PB4, SP_PB5, SP_PB6,
    SP_PA1, SP_PA2, SP_PA3, SP_PA4, SP_PA5, SP_PA6,
    SP_PARALLEL_BYPASS,
    SP_PRE_FORMANT_GAIN,
    SP_OUTPUT_GAIN,
    SP_END_VOICE_PITCH,         /* pitch at the end of the frame */
    SYNTH_PARAM_COUNT
};

struct synth_frame
{
    synth_param_t p[SYNTH_PARAM_COUNT];
};

/* Reset the generator and empty its queue. */
void synth_init(int sample_rate);

/* Queue a frame, or silence when frame is NULL: held for at least min_us,
 * reached by fading from the previous frame over fade_us, or its formants
 * over formant_fade_us when that is longer. False when full. */
bool synth_queue_frame(const struct synth_frame *frame, int32_t min_us,
                       int32_t fade_us, int32_t formant_fade_us);

/* Render up to count samples. Fewer means the queue ran dry: more frames may
 * still be queued and rendering resumed exactly where it stopped. */
int synth_generate(int16_t *buf, int count);

#endif /* __SYNTH_H */
