/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * The dithered output's per-channel loop, from dsp_sample_output.c, where
 * the rest of the dither still lives, and automatic dither's test of each
 * block. dither_arm.S stands in for the loop on ARM, with the same output.
 *
 * The "dither" code to convert the 24-bit samples produced by libmad was
 * taken from the coolplayer project - coolplayer.sourceforge.net
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
#include "rbcodecconfig.h"
#include "platform.h"
#include "dsp-util.h"
#include "dither.h"

/* Eight at a time, so a source that is not exact, which shows on almost its
   first samples, costs almost nothing */
bool dither_block_exact(int32_t * const p32[], int channels, int count,
                        int scale)
{
    const int32_t mask = (1L << scale) - 1;

    for (int ch = 0; ch < channels; ch++)
    {
        const int32_t *s = p32[ch];
        int i = 0;

        for (; i + 8 <= count; i += 8)
            if ((s[i] | s[i + 1] | s[i + 2] | s[i + 3] |
                 s[i + 4] | s[i + 5] | s[i + 6] | s[i + 7]) & mask)
                return false;

        for (; i < count; i++)
            if (s[i] & mask)
                return false;
    }

    return true;
}

#if !defined(CPU_ARM) || defined(CPU_ARM_MICRO)
void dither_channel(struct dither_state *dither, const int32_t *s,
                    int16_t *d, int count, int scale)
{
    int32_t dc_bias = 1L << (scale - 1); /* 1/2 bit of significance */
    int32_t mask = (1L << scale) - 1; /* Mask of bits quantized away */

    for (int i = 0; i < count; i++, s++, d += 2)
    {
        /* Noise shape and bias (for correct rounding later) */
        int32_t sample = *s;

        sample += dither->error[0] - dither->error[1] + dither->error[2];
        dither->error[2] = dither->error[1];
        dither->error[1] = dither->error[0] / 2;

        int32_t output = sample + dc_bias;

        /* Dither, highpass triangle PDF */
        int32_t random = dither->random*0x0019660dL + 0x3c6ef35fL;
        output += (random & mask) - (dither->random & mask);
        dither->random = random;

        /* Quantize sample to output range */
        output >>= scale;

        /* Error feedback of quantization */
        dither->error[0] = sample - (output << scale);

        /* Clip and store */
        *d = clip_sample_16(output);
    }
}
#endif /* CPU */
