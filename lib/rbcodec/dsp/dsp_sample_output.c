/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 Miika Pekkarinen
 * Copyright (C) 2012 Michael Sevakis
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
#include "dsp_core.h"
#include "dsp_sample_io.h"
#include "dsp_proc_entry.h"
#include "dsp-util.h"
#include "dsp_misc.h"
#include "dither.h"
#include <string.h>

#if 0
#include <debug.h>
#else
#undef DEBUGF
#define DEBUGF(...)
#endif

/* May be implemented in here or externally.*/
void sample_output_mono(struct sample_io_data *this,
                        struct dsp_buffer *src, struct dsp_buffer *dst);
void sample_output_stereo(struct sample_io_data *this,
                          struct dsp_buffer *src, struct dsp_buffer *dst);
void sample_output_dithered(struct sample_io_data *this,
                            struct dsp_buffer *src, struct dsp_buffer *dst);

/** Sample output **/

#if !defined(CPU_COLDFIRE) && !defined(CPU_ARM)
/* write mono internal format to output format */
void sample_output_mono(struct sample_io_data *this,
                        struct dsp_buffer *src, struct dsp_buffer *dst)
{
    int count = this->outcount;
    const int32_t *s0 = src->p32[0];
    int16_t *d = dst->p16out;
    int scale = src->format.output_scale;
    int32_t dc_bias = 1L << (scale - 1);

    do
    {
        int32_t lr = clip_sample_16((*s0++ + dc_bias) >> scale);
        *d++ = lr;
        *d++ = lr;
    }
    while (--count > 0);
}
#endif /* CPU */

#if (!defined(CPU_COLDFIRE) && !defined(CPU_ARM)) || defined(CPU_ARM_MICRO)
/* write stereo internal format to output format */
void sample_output_stereo(struct sample_io_data *this,
                          struct dsp_buffer *src, struct dsp_buffer *dst)
{
    int count = this->outcount;
    const int32_t *s0 = src->p32[0];
    const int32_t *s1 = src->p32[1];
    int16_t *d = dst->p16out;
    int scale = src->format.output_scale;
    int32_t dc_bias = 1L << (scale - 1);

    do
    {
        *d++ = clip_sample_16((*s0++ + dc_bias) >> scale);
        *d++ = clip_sample_16((*s1++ + dc_bias) >> scale);
    }
    while (--count > 0);
}
#endif /* CPU */

/**
 * The "dither" code to convert the 24-bit samples produced by libmad was
 * taken from the coolplayer project - coolplayer.sourceforge.net
 *
 * This function handles mono and stereo outputs.
 */
static struct dither_data
{
    struct dither_state state[2]; /* 0=left, 1=right */
    int mode;           /* 20h: enum dither_modes */
    bool dithering;     /* 24h: automatic: the last block was dithered */
                        /* 28h */
} dither_data IBSS_ATTR;

void sample_output_dithered(struct sample_io_data *this,
                            struct dsp_buffer *src, struct dsp_buffer *dst)
{
    int count = this->outcount;
    int channels = src->format.num_channels;
    int scale = src->format.output_scale;

    for (int ch = 0; ch < channels; ch++)
        dither_channel(&dither_data.state[ch], src->p32[ch],
                       &dst->p16out[ch], count, scale);

    if (channels > 1)
        return;

    /* Have to duplicate left samples into the right channel since
       output is interleaved stereo */
    int16_t *d = dst->p16out;

    do
    {
        int16_t s = *d++;
        *d++ = s;
    }
    while (--count > 0);
}

/* Automatic: a block with no bits below the output's LSB goes through the
   plain copy, which quantises it without error */
static void sample_output_auto(struct sample_io_data *this,
                               struct dsp_buffer *src, struct dsp_buffer *dst)
{
    int channels = src->format.num_channels;

    if (!dither_block_exact(src->p32, channels, this->outcount,
                            src->format.output_scale))
    {
        dither_data.dithering = true;
        sample_output_dithered(this, src, dst);
        return;
    }

    if (dither_data.dithering)
    {
        dither_data.dithering = false;
        dsp_sample_output_flush(this);
    }

    if (channels > 1)
        sample_output_stereo(this, src, dst);
    else
        sample_output_mono(this, src, dst);
}

/* Initialize the output function for settings and format */
void dsp_sample_output_format_change(struct sample_io_data *this,
                                     struct sample_format *format)
{
    static const sample_output_fn_type fns[2][2] =
    {
        { sample_output_mono,        /* DC-biased quantizing */
          sample_output_stereo },
        { sample_output_dithered,    /* Tri-PDF dithering */
          sample_output_dithered },
    };

    int mode = dsp_get_id((void *)this) == CODEC_IDX_AUDIO ?
               dither_data.mode : DITHER_OFF;
    int channels = format->num_channels;

    DSP_PRINT_FORMAT(DSP Output, *format);

    if (mode == DITHER_AUTO)
        this->output_samples = sample_output_auto;
    else
        this->output_samples = fns[mode == DITHER_ALWAYS ? 1 : 0][channels - 1];
    this->output_version = format->version;
}

void dsp_sample_output_init(struct sample_io_data *this)
{
    this->output_version = 0;
    this->output_samples = sample_output_stereo;
}

/* Flush the dither history */
void dsp_sample_output_flush(struct sample_io_data *this)
{
    if (dsp_get_id((void *)this) == CODEC_IDX_AUDIO)
        memset(dither_data.state, 0, sizeof (dither_data.state));
}


/** Output settings **/

/* Set the tri-pdf dithered output */
void dsp_dither_enable(int mode)
{
    if (mode == dither_data.mode)
        return;

    dither_data.mode = mode;
    dither_data.dithering = false;
    struct sample_io_data *data = (void *)dsp_get_config(CODEC_IDX_AUDIO);

    if (mode != DITHER_OFF)
        dsp_sample_output_flush(data);

    data->output_version = 0; /* Force format update */
}
