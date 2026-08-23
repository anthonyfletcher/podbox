/***************************************************************************
 * Original code from RockBox
 * was: apps/recorder/resize.c
 * Copyright (C) 2008 by Akio Idehara, Andrew Mahone
 * GNU General Public License (version 2+)
 *
 * Image scaler. Area-average and linear scaling between bitmaps, plus
 * recalc_dimension() for fitting an image to a target box.
 ****************************************************************************/

/*
 * Implementation of area average and linear row and vertical scalers, and
 * nearest-neighbor grey scaler (C) 2008 Andrew Mahone
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "general.h"
#include "kernel.h"
#include "system.h"
#include "debug.h"
#include "lcd.h"
#include "file.h"
#ifdef ROCKBOX_DEBUG_SCALERS
#define SDEBUGF DEBUGF
#else
#define SDEBUGF(...)
#endif
#include "config.h"
#include "system.h"
#include "bmp.h"
#include "resize.h"
#include "jpeg_load.h"

#define MULUQ(a, b) ((a) * (b))
#define MULQ(a, b) ((a) * (b))

#define CHANNEL_BYTES (sizeof(struct uint32_argb)/sizeof(uint32_t))

/* calculate the maximum dimensions which will preserve the aspect ration of
   src while fitting in the constraints passed in dst, and store result in dst,
   returning 0 if rounding and 1 if not rounding.
*/
int recalc_dimension(struct dim *dst, struct dim *src)
{
    /* This only looks backwards. The input image size is being pre-scaled by
     * the inverse of the pixel aspect ratio, so that once the size it scaled
     * to meet the output constraints, the scaled image will have appropriate
     * proportions.
     */
    int sw = src->width * LCD_PIXEL_ASPECT_HEIGHT;
    int sh = src->height * LCD_PIXEL_ASPECT_WIDTH;
    int tmp;
    if (dst->width <= 0)
        dst->width = LCD_WIDTH;
    if (dst->height <= 0)
        dst->height = LCD_HEIGHT;
    tmp = (sw * dst->height + (sh >> 1)) / sh;
    if (tmp > dst->width)
        dst->height = (sh * dst->width + (sw >> 1)) / sw;
    else
        dst->width = tmp;
    return src->width == dst->width && src->height == dst->height;
}

/* All of these scalers use variations of Bresenham's algorithm to convert from
   their input to output coordinates.  The error value is shifted from the
   "classic" version such that it is a useful input to the scaling calculation.
*/

/* dither + pack on channel of RGB565, R an B share a packing macro */
#define PACKRB(v, delta)  ((31 * v + (v >> 3) + delta) >> 8)
#define PACKG(g, delta) ((63 * g + (g >> 2) + delta) >> 8)

/* read new img_part unconditionally, return false on failure */
#define FILL_BUF_INIT(img_part, store_part, args) { \
    img_part = store_part(args); \
    if (img_part == NULL) \
        return false; \
}

/* read new img_part if current one is empty, return false on failure */
#define FILL_BUF(img_part, store_part, args) { \
    if (img_part->len == 0) \
        img_part = store_part(args); \
    if (img_part == NULL) \
        return false; \
}


/* horizontal area average scaler */
static bool scale_h_area(void *out_line_ptr,
                         struct scaler_context *ctx, bool accum)
{
    SDEBUGF("scale_h_area\n");
    unsigned int ix, ox, oxe, mul;
    const uint32_t h_i_val = ctx->h_i_val,
                   h_o_val = ctx->h_o_val;
    struct uint32_argb rgbvalacc = { 0, 0, 0, 0 },
                       rgbvaltmp = { 0, 0, 0, 0 },
                      *out_line = (struct uint32_argb *)out_line_ptr;
    struct img_part *part;
    FILL_BUF_INIT(part,ctx->store_part,ctx->args);
    ox = 0;
    oxe = 0;
    mul = 0;
    /* give other tasks a chance to run */
    yield();
    for (ix = 0; ix < (unsigned int)ctx->src->width; ix++)
    {
        oxe += h_o_val;
        /* end of current area has been reached */
        /* fill buffer if needed */
        FILL_BUF(part,ctx->store_part,ctx->args);
        if (oxe >= h_i_val)
        {
            /* "reset" error, which now represents partial coverage of next
               pixel by the next area
            */
            oxe -= h_i_val;

/* generic C math */
            /* add saved partial pixel from start of area */
            rgbvalacc.r = rgbvalacc.r * h_o_val + rgbvaltmp.r * mul;
            rgbvalacc.g = rgbvalacc.g * h_o_val + rgbvaltmp.g * mul;
            rgbvalacc.b = rgbvalacc.b * h_o_val + rgbvaltmp.b * mul;
            rgbvalacc.a = rgbvalacc.a * h_o_val + rgbvaltmp.a * mul;

            /* get new pixel , then add its partial coverage to this area */
            rgbvaltmp.r = part->buf->red;
            rgbvaltmp.g = part->buf->green;
            rgbvaltmp.b = part->buf->blue;
            rgbvaltmp.a = part->buf->alpha;
            mul = h_o_val - oxe;
            rgbvalacc.r += rgbvaltmp.r * mul;
            rgbvalacc.g += rgbvaltmp.g * mul;
            rgbvalacc.b += rgbvaltmp.b * mul;
            rgbvalacc.a += rgbvaltmp.a * mul;
            rgbvalacc.r = (rgbvalacc.r + (1 << 21)) >> 22;
            rgbvalacc.g = (rgbvalacc.g + (1 << 21)) >> 22;
            rgbvalacc.b = (rgbvalacc.b + (1 << 21)) >> 22;
            rgbvalacc.a = (rgbvalacc.a + (1 << 21)) >> 22;
            /* store or accumulate to output row */
            if (accum)
            {
                rgbvalacc.r += out_line[ox].r;
                rgbvalacc.g += out_line[ox].g;
                rgbvalacc.b += out_line[ox].b;
                rgbvalacc.a += out_line[ox].a;
            }
            out_line[ox].r = rgbvalacc.r;
            out_line[ox].g = rgbvalacc.g;
            out_line[ox].b = rgbvalacc.b;
            out_line[ox].a = rgbvalacc.a;
            /* reset accumulator */
            rgbvalacc.r = 0;
            rgbvalacc.g = 0;
            rgbvalacc.b = 0;
            rgbvalacc.a = 0;
            mul = oxe;
            ox += 1;
        /* inside an area */
        } else {
            /* add pixel value to accumulator */
            rgbvalacc.r += part->buf->red;
            rgbvalacc.g += part->buf->green;
            rgbvalacc.b += part->buf->blue;
            rgbvalacc.a += part->buf->alpha;
        }
        part->buf++;
        part->len--;
    }
    return true;
}

/* vertical area average scaler */
static inline bool scale_v_area(struct rowset *rset, struct scaler_context *ctx)
{
    uint32_t mul, oy, iy, oye;
    const uint32_t v_i_val = ctx->v_i_val,
                   v_o_val = ctx->v_o_val;

    /* Set up rounding and scale factors */
    mul = 0;
    oy = rset->rowstart;
    oye = 0;
    uint32_t *rowacc = (uint32_t *) ctx->buf,
             *rowtmp = rowacc + ctx->bm->width * CHANNEL_BYTES,
             *rowacc_px, *rowtmp_px;
    memset((void *)ctx->buf, 0, ctx->bm->width * 2 * sizeof(uint32_t)*CHANNEL_BYTES);
    SDEBUGF("scale_v_area\n");
    /* zero the accumulator and temp rows */
    for (iy = 0; iy < (unsigned int)ctx->src->height; iy++)
    {
        oye += v_o_val;
        /* end of current area has been reached */
        if (oye >= v_i_val)
        {
            /* "reset" error, which now represents partial coverage of the next
               row by the next area
            */
            oye -= v_i_val;
            /* add stored partial row to accumulator */
            for(rowacc_px = rowacc, rowtmp_px = rowtmp; rowacc_px != rowtmp;
                rowacc_px++, rowtmp_px++)
                *rowacc_px = *rowacc_px * v_o_val + *rowtmp_px * mul;
            /* store new scaled row in temp row */
            if(!ctx->h_scaler(rowtmp, ctx, false))
                return false;
            /* add partial coverage by new row to this area, then round and
               scale to final value
            */
            mul = v_o_val - oye;
            for(rowacc_px = rowacc, rowtmp_px = rowtmp; rowacc_px != rowtmp;
                rowacc_px++, rowtmp_px++)
                *rowacc_px += mul * *rowtmp_px;
            ctx->output_row(oy, (void*)rowacc, ctx);
            /* clear accumulator row, store partial coverage for next row */
            memset((void *)rowacc, 0, ctx->bm->width * sizeof(uint32_t) * CHANNEL_BYTES);
            mul = oye;
            oy += rset->rowstep;
        /* inside an area */
        } else {
            /* accumulate new scaled row to rowacc */
            if (!ctx->h_scaler(rowacc, ctx, true))
                return false;
        }
    }
    return true;
}

/* horizontal linear scaler */
static bool scale_h_linear(void *out_line_ptr, struct scaler_context *ctx,
                           bool accum)
{
    unsigned int ix, ox, ixe;
    const uint32_t h_i_val = ctx->h_i_val,
                   h_o_val = ctx->h_o_val;
    /* type x = x is an ugly hack for hiding an unitialized data warning. The
       values are conditionally initialized before use, but other values are
       set such that this will occur before these are used.
    */
    struct uint32_argb rgbval=rgbval, rgbinc=rgbinc,
                      *out_line = (struct uint32_argb*)out_line_ptr;
    struct img_part *part;
    SDEBUGF("scale_h_linear\n");
    FILL_BUF_INIT(part,ctx->store_part,ctx->args);
    ix = 0;
    /* The error is set so that values are initialized on the first pass. */
    ixe = h_o_val;
    /* give other tasks a chance to run */
    yield();
    for (ox = 0; ox < (uint32_t)ctx->bm->width; ox++)
    {
        if (ixe >= h_o_val)
        {
            /* Store the new "current" pixel value in rgbval, and the color
               step value in rgbinc.
            */
            ixe -= h_o_val;
            rgbinc.r = -(part->buf->red);
            rgbinc.g = -(part->buf->green);
            rgbinc.b = -(part->buf->blue);
            rgbinc.a = -(part->buf->alpha);
/* generic C math */
            rgbval.r = (part->buf->red) * h_o_val;
            rgbval.g = (part->buf->green) * h_o_val;
            rgbval.b = (part->buf->blue) * h_o_val;
            rgbval.a = (part->buf->alpha) * h_o_val;
            ix += 1;
            /* If this wasn't the last pixel, add the next one to rgbinc. */
            if (LIKELY(ix < (uint32_t)ctx->src->width)) {
                part->buf++;
                part->len--;
                /* Fetch new pixels if needed */
                FILL_BUF(part,ctx->store_part,ctx->args);
                rgbinc.r += part->buf->red;
                rgbinc.g += part->buf->green;
                rgbinc.b += part->buf->blue;
                rgbinc.a += part->buf->alpha;
                /* Add a partial step to rgbval, in this pixel isn't precisely
                   aligned with the new source pixel
                */
/* generic C math */
                rgbval.r += rgbinc.r * ixe;
                rgbval.g += rgbinc.g * ixe;
                rgbval.b += rgbinc.b * ixe;
                rgbval.a += rgbinc.a * ixe;
            }
            /* Now multiply the color increment to its proper value */
            rgbinc.r *= h_i_val;
            rgbinc.g *= h_i_val;
            rgbinc.b *= h_i_val;
            rgbinc.a *= h_i_val;
        } else {
            rgbval.r += rgbinc.r;
            rgbval.g += rgbinc.g;
            rgbval.b += rgbinc.b;
            rgbval.a += rgbinc.a;
        }
        /* round and scale values, and accumulate or store to output */
        if (accum)
        {
            out_line[ox].r += (rgbval.r + (1 << 21)) >> 22;
            out_line[ox].g += (rgbval.g + (1 << 21)) >> 22;
            out_line[ox].b += (rgbval.b + (1 << 21)) >> 22;
            out_line[ox].a += (rgbval.a + (1 << 21)) >> 22;
        } else {
            out_line[ox].r = (rgbval.r + (1 << 21)) >> 22;
            out_line[ox].g = (rgbval.g + (1 << 21)) >> 22;
            out_line[ox].b = (rgbval.b + (1 << 21)) >> 22;
            out_line[ox].a = (rgbval.a + (1 << 21)) >> 22;
        }
        ixe += h_i_val;
    }
    return true;
}

/* vertical linear scaler */
static inline bool scale_v_linear(struct rowset *rset,
                                  struct scaler_context *ctx)
{
    uint32_t iy, iye;
    int32_t oy;
    const uint32_t v_i_val = ctx->v_i_val,
                   v_o_val = ctx->v_o_val;
    /* Set up our buffers, to store the increment and current value for each
       column, and one temp buffer used to read in new rows.
    */
    uint32_t *rowinc = (uint32_t *)(ctx->buf),
             *rowval = rowinc + ctx->bm->width * CHANNEL_BYTES,
             *rowtmp = rowval + ctx->bm->width * CHANNEL_BYTES,
             *rowinc_px, *rowval_px, *rowtmp_px;

    SDEBUGF("scale_v_linear\n");
    iy = 0;
    iye = v_o_val;
    /* get first scaled row in rowtmp */
    if(!ctx->h_scaler((void*)rowtmp, ctx, false))
        return false;
    for (oy = rset->rowstart; oy != rset->rowstop; oy += rset->rowstep)
    {
        if (iye >= v_o_val)
        {
            iye -= v_o_val;
            iy += 1;
            for(rowinc_px = rowinc, rowtmp_px = rowtmp, rowval_px = rowval;
                rowinc_px < rowval; rowinc_px++, rowtmp_px++, rowval_px++)
            {
                *rowinc_px = -*rowtmp_px;
                *rowval_px = *rowtmp_px * v_o_val;
            }
            if (iy < (uint32_t)ctx->src->height)
            {
                if (!ctx->h_scaler((void*)rowtmp, ctx, false))
                    return false;
                for(rowinc_px = rowinc, rowtmp_px = rowtmp, rowval_px = rowval;
                    rowinc_px < rowval; rowinc_px++, rowtmp_px++, rowval_px++)
                {
                    *rowinc_px += *rowtmp_px;
                    *rowval_px += *rowinc_px * iye;
                    *rowinc_px *= v_i_val;
                }
            }
        } else
            for(rowinc_px = rowinc, rowval_px = rowval; rowinc_px < rowval;
                rowinc_px++, rowval_px++)
                *rowval_px += *rowinc_px;
        ctx->output_row(oy, (void*)rowval, ctx);
        iye += v_i_val;
    }
    return true;
}

static void output_row_32_native_fromyuv(uint32_t row, void * row_in,
                               struct scaler_context *ctx)
{
#define DEST_STEP   (1)
#define Y_STEP      (BM_WIDTH(ctx->bm->width,FORMAT_NATIVE,0))

    int col;
    uint8_t dy = DITHERY(row);
    struct uint32_argb *qp = (struct uint32_argb *)row_in;
    SDEBUGF("output_row: y: %lu in: %p\n",row, row_in);
    fb_data *dest = (fb_data *)ctx->bm->data + Y_STEP * row;
    int delta = 127;
    unsigned r, g, b, y, u, v;
    
    for (col = 0; col < ctx->bm->width; col++) {
        (void) delta;
        if (ctx->dither)
            delta = DITHERXDY(col,dy);
        y = SC_OUT(qp->b, ctx);
        u = SC_OUT(qp->g, ctx);
        v = SC_OUT(qp->r, ctx);
        qp++;
        yuv_to_rgb(y, u, v, &r, &g, &b);
        r = (31 * r + (r >> 3) + delta) >> 8;
        g = (63 * g + (g >> 2) + delta) >> 8;
        b = (31 * b + (b >> 3) + delta) >> 8;
        *dest = FB_RGBPACK_LCD(r, g, b);
        dest += DEST_STEP;
    }
}

static void output_row_32_native(uint32_t row, void * row_in,
                              struct scaler_context *ctx)
{
    int col;
    int fb_width = BM_WIDTH(ctx->bm->width,FORMAT_NATIVE,0);
    uint8_t dy = DITHERY(row);
    struct uint32_argb *qp = (struct uint32_argb*)row_in;
    SDEBUGF("output_row: y: %lu in: %p\n",row, row_in);
                /* iriver h300, colour iPods, X5 */
                (void)fb_width;
                fb_data *dest = STRIDE_MAIN((fb_data *)ctx->bm->data + fb_width * row,
                                            (fb_data *)ctx->bm->data + row);
                int delta = 127;
                unsigned r, g, b;
                struct uint32_argb q0;
                /* setup alpha channel buffer */
                unsigned char *bm_alpha = NULL;
                if (ctx->bm->alpha_offset > 0)
                    bm_alpha = ctx->bm->data + ctx->bm->alpha_offset;
                if (bm_alpha)
                    bm_alpha += ALIGN_UP(ctx->bm->width, 2)*row/2;

                for (col = 0; col < ctx->bm->width; col++) {
                    (void) delta;
                    if (ctx->dither)
                        delta = DITHERXDY(col,dy);
                    q0 = *qp++;
                    r = SC_OUT(q0.r, ctx);
                    g = SC_OUT(q0.g, ctx);
                    b = SC_OUT(q0.b, ctx);
                    r = (31 * r + (r >> 3) + delta) >> 8;
                    g = (63 * g + (g >> 2) + delta) >> 8;
                    b = (31 * b + (b >> 3) + delta) >> 8;
                    *dest = FB_RGBPACK_LCD(r, g, b);
                    dest += STRIDE_MAIN(1, ctx->bm->height);
                    if (bm_alpha) {
                        /* pack alpha channel for 2 pixels into 1 byte */
                        unsigned alpha = SC_OUT(q0.a, ctx);
                        if (col%2)
                            *bm_alpha++ |= alpha&0xf0;
                        else
                            *bm_alpha = alpha>>4;
                    }
                }
}

/* Also built for the core image viewer (colour targets), not just plugins. */
unsigned int get_size_native(struct bitmap *bm)
{
    return BM_SIZE(bm->width,bm->height,FORMAT_NATIVE,0);
}

const struct custom_format format_native = {
    .output_row_8 = output_row_8_native,
    .output_row_32 = {
        output_row_32_native,
        output_row_32_native_fromyuv
    },
    .get_size = get_size_native
};

int resize_on_load(struct bitmap *bm, bool dither, struct dim *src,
                   struct rowset *rset, unsigned char *buf, unsigned int len,
                   const struct custom_format *format,
                   IF_PIX_FMT(int format_index,)
                   struct img_part* (*store_part)(void *args),
                   void *args)
{
    const int sw = src->width;
    const int sh = src->height;
    const int dw = bm->width;
    const int dh = bm->height;
    int ret;
    /* buffer for 1 line + 2 spare lines */
    unsigned int needed = sizeof(struct uint32_argb) * 3 * bm->width;
    ALIGN_BUFFER(buf, len, sizeof(uint32_t));
    if (needed > len)
    {
        DEBUGF("unable to allocate required buffer: %d needed, "
               "%d available\n", needed, len);
        return 0;
    }

    struct scaler_context ctx;
    cpu_boost(true);
    ctx.store_part = store_part;
    ctx.args = args;
    ctx.buf = buf;
    ctx.len = len;
    ctx.bm = bm;
    ctx.src = src;
    ctx.dither = dither;
    ctx.output_row = format_index ? output_row_32_native_fromyuv
                                  : output_row_32_native;
    if (format)
        ctx.output_row = format->output_row_32[format_index];
    /* dw < 2 joins the downscale branch because the linear scaler divides
     * by dw - 1: with one output pixel there is no pair to interpolate
     * between, and a one-pixel-wide picture fitted to a box reaches here
     * with sw and dw both 1. The area scaler divides by sw instead, which
     * the decoders now guarantee is at least 1. */
    if (sw > dw || dw < 2)
    {
        ctx.h_scaler = scale_h_area;
        uint32_t h_div = (1U << 24) / sw;
        ctx.h_i_val = sw * h_div;
        ctx.h_o_val = dw * h_div;
    } else {
        ctx.h_scaler = scale_h_linear;
        uint32_t h_div = (1U << 24) / (dw - 1);
        ctx.h_i_val = (sw - 1) * h_div;
        ctx.h_o_val = (dw - 1) * h_div;
    }
    if (sh > dh)
    {
        uint32_t v_div = (1U << 22) / sh;
        ctx.v_i_val = sh * v_div;
        ctx.v_o_val = dh * v_div;
        ret = scale_v_area(rset, &ctx);
    }
    else
    {
        uint32_t v_div = (1U << 22) / dh;
        ctx.v_i_val = (sh - 1) * v_div;
        ctx.v_o_val = (dh - 1) * v_div;
        ret = scale_v_linear(rset, &ctx);
    }
    cpu_boost(false);
    if (!ret)
        return 0;
    return 1;
}
