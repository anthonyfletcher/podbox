/***************************************************************************
 * Original code from RockBox
 * was: apps/recorder/resize.c
 * Copyright (C) 2008 by Akio Idehara, Andrew Mahone
 * GNU General Public License (version 2+)
 *
 * Image scaler. Area-average and linear scaling between bitmaps, plus
 * recalc_dimension() for fitting an image to a target box, and the bitmap
 * sizing and row output the PNG, GIF and WebP readers share.
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
#include <stddef.h>

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


/* horizontal area average scaler. Each part is taken whole into locals; a
 * pixel inside an area is three adds, and one that ends it splits between
 * that area and the next. `alpha` and `rgb24` are constants in each of the
 * callers below, so the alpha channel costs nothing where nothing reads it,
 * and a pixel's layout is fixed offsets from one byte pointer. */
static FORCE_INLINE bool scale_h_area_row(void *out_line_ptr,
                                          struct scaler_context *ctx,
                                          bool accum, const bool alpha,
                                          const bool rgb24)
{
    /* a pixel's channels as byte offsets, and its size */
    const int ro = rgb24 ? 0 : offsetof(struct uint8_rgb, red),
              go = rgb24 ? 1 : offsetof(struct uint8_rgb, green),
              bo = rgb24 ? 2 : offsetof(struct uint8_rgb, blue),
              ao = offsetof(struct uint8_rgb, alpha),
              size = rgb24 ? 3 : sizeof(struct uint8_rgb);
    SDEBUGF("scale_h_area\n");
    const uint32_t h_i_val = ctx->h_i_val,
                   h_o_val = ctx->h_o_val;
    struct uint32_argb *out = (struct uint32_argb *)out_line_ptr;
    uint32_t r = 0, g = 0, b = 0, a = 0, tr = 0, tg = 0, tb = 0, ta = 0;
    uint32_t oxe = 0, mul = 0;
    int left = ctx->src->width;
    struct img_part *part;
    FILL_BUF_INIT(part,ctx->store_part,ctx->args);
    /* give other tasks a chance to run */
    yield();
    while (left > 0)
    {
        const uint8_t *px;
        int n;

        FILL_BUF(part,ctx->store_part,ctx->args);
        px = (const uint8_t *)part->buf;
        n = MIN(part->len, left);
        part->buf = (struct uint8_rgb *)(px + n * size);
        part->len -= n;
        left -= n;
        while (n--)
        {
            oxe += h_o_val;
            /* inside an area */
            if (oxe < h_i_val)
            {
                r += px[ro];
                g += px[go];
                b += px[bo];
                if (alpha)
                    a += px[ao];
                px += size;
                continue;
            }
            /* end of current area: the error now represents partial
               coverage of this pixel by the next area */
            oxe -= h_i_val;
            /* add saved partial pixel from start of area */
            r = r * h_o_val + tr * mul;
            g = g * h_o_val + tg * mul;
            b = b * h_o_val + tb * mul;
            if (alpha)
                a = a * h_o_val + ta * mul;
            /* then this pixel's partial coverage of this area */
            tr = px[ro];
            tg = px[go];
            tb = px[bo];
            if (alpha)
                ta = px[ao];
            mul = h_o_val - oxe;
            r = (r + tr * mul + (1 << 21)) >> 22;
            g = (g + tg * mul + (1 << 21)) >> 22;
            b = (b + tb * mul + (1 << 21)) >> 22;
            if (alpha)
                a = (a + ta * mul + (1 << 21)) >> 22;
            /* store or accumulate to output row */
            if (accum)
            {
                r += out->r;
                g += out->g;
                b += out->b;
                if (alpha)
                    a += out->a;
            }
            out->r = r;
            out->g = g;
            out->b = b;
            if (alpha)
                out->a = a;
            r = g = b = a = 0;
            mul = oxe;
            px += size;
            out++;
        }
    }
    return true;
}

#ifdef CPU_ARM
/* The same, with each part's pixels summed by image_arm.S, which reads its
 * state from this struct at fixed offsets */
struct area_run
{
    uint32_t r, g, b;
    uint32_t tr, tg, tb;        /* the partial pixel an area started with */
    uint32_t oxe, mul;
    struct uint32_argb *out;
    uint32_t h_i_val, h_o_val;
    int accum;
};
_Static_assert(offsetof(struct area_run, tr) == 12
               && offsetof(struct area_run, oxe) == 24
               && offsetof(struct area_run, accum) == 44,
               "image_arm.S reads struct area_run by these offsets");

void area_run_rgb24(struct area_run *s, const uint8_t *px, int n);
void area_run_rgb32(struct area_run *s, const uint8_t *px, int n);

static FORCE_INLINE bool scale_h_area_arm(void *out_line_ptr,
                                          struct scaler_context *ctx,
                                          bool accum, const bool rgb24)
{
    struct area_run s = {
        .out = out_line_ptr,
        .h_i_val = ctx->h_i_val,
        .h_o_val = ctx->h_o_val,
        .accum = accum,
    };
    const int size = rgb24 ? 3 : sizeof(struct uint8_rgb);
    int left = ctx->src->width;
    struct img_part *part;
    FILL_BUF_INIT(part,ctx->store_part,ctx->args);
    /* give other tasks a chance to run */
    yield();
    while (left > 0)
    {
        const uint8_t *px;
        int n;

        FILL_BUF(part,ctx->store_part,ctx->args);
        px = (const uint8_t *)part->buf;
        n = MIN(part->len, left);
        part->buf = (struct uint8_rgb *)(px + n * size);
        part->len -= n;
        left -= n;
        if (rgb24)
            area_run_rgb24(&s, px, n);
        else
            area_run_rgb32(&s, px, n);
    }
    return true;
}
#endif

static bool scale_h_area(void *out_line_ptr, struct scaler_context *ctx,
                         bool accum)
{
#ifdef CPU_ARM
    return scale_h_area_arm(out_line_ptr, ctx, accum, false);
#else
    return scale_h_area_row(out_line_ptr, ctx, accum, false, false);
#endif
}

/* For a bitmap with an alpha plane: only output_row_32_native() reads the
 * alpha it accumulates */
static bool scale_h_area_alpha(void *out_line_ptr, struct scaler_context *ctx,
                               bool accum)
{
    return scale_h_area_row(out_line_ptr, ctx, accum, true, false);
}

/* For parts of packed R, G, B bytes (RESIZE_RGB24) */
static bool scale_h_area_rgb24(void *out_line_ptr, struct scaler_context *ctx,
                               bool accum)
{
#ifdef CPU_ARM
    return scale_h_area_arm(out_line_ptr, ctx, accum, true);
#else
    return scale_h_area_row(out_line_ptr, ctx, accum, false, true);
#endif
}

/* vertical area average scaler */
static inline bool scale_v_area(struct rowset *rset, struct scaler_context *ctx)
{
    uint32_t mul, oy, iy, oye;
    const uint32_t v_i_val = ctx->v_i_val,
                   v_o_val = ctx->v_o_val;
    int done = 0;

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
            if (!image_progress(ctx->format, ++done, ctx->bm->height))
                return false;
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
    int done = 0;
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
        if (!image_progress(ctx->format, ++done, ctx->bm->height))
            return false;
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

/* Bytes a native-format bitmap of bm's size takes. */
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
    ctx.format = format;
    ctx.store_part = store_part;
    ctx.args = args;
    ctx.buf = buf;
    ctx.len = len;
    ctx.bm = bm;
    ctx.src = src;
    ctx.dither = dither;
    bool rgb24 = format_index & RESIZE_RGB24;

    format_index &= ~RESIZE_RGB24;
    ctx.output_row = format_index ? output_row_32_native_fromyuv
                                  : output_row_32_native;
    if (format)
        ctx.output_row = format->output_row_32[format_index];
    /* dw < 2 joins the downscale branch because the linear scaler divides
     * by dw - 1: with one output pixel there is no pair to interpolate
     * between, and a one-pixel-wide picture fitted to a box reaches here
     * with sw and dw both 1. The area scaler divides by sw instead, which
     * the decoders guarantee is at least 1. */
    if (sw > dw || dw < 2)
    {
        ctx.h_scaler = rgb24 ? scale_h_area_rgb24
                     : bm->alpha_offset > 0 ? scale_h_area_alpha
                     : scale_h_area;
        uint32_t h_div = (1U << 24) / sw;
        ctx.h_i_val = sw * h_div;
        ctx.h_o_val = dw * h_div;
    } else {
        if (rgb24)
        {
            cpu_boost(false);
            return 0;
        }
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

int image_fit_bitmap(struct bitmap *bm, struct dim *src, int format,
                     const struct custom_format *cformat)
{
    if (format & FORMAT_RESIZE)
    {
        struct dim d = { .width = bm->width, .height = bm->height };
        if (format & FORMAT_KEEP_ASPECT)
            recalc_dimension(&d, src);
        bm->width = d.width;
        bm->height = d.height;
    }
    else
    {
        bm->width = src->width;
        bm->height = src->height;
    }
    bm->alpha_offset = 0;
    if (cformat)
        return cformat->get_size(bm);
    return BM_SIZE(bm->width, bm->height, FORMAT_NATIVE, false);
}

bool image_store_rows(struct bitmap *bm, bool dither, struct dim *src,
                      unsigned char *buf, unsigned int len,
                      const struct custom_format *cformat, int format_index,
                      struct img_part *(*store_part)(void *args), void *args)
{
    if (bm->width != src->width || bm->height != src->height)
    {
        struct rowset rset = {
            .rowstart = 0, .rowstop = bm->height, .rowstep = 1
        };
        return resize_on_load(bm, dither, src, &rset, buf, len, cformat,
                              format_index, store_part, args);
    }

    struct scaler_context ctx = { .bm = bm, .dither = dither };
    void (*output_row_8)(uint32_t, void *, struct scaler_context *) =
        cformat ? cformat->output_row_8 : output_row_8_native;

    for (int row = 0; row < bm->height; row++)
    {
        struct img_part *part;

        if (!image_progress(cformat, row, bm->height))
            return false;
        part = store_part(args);
        if (!part)
            return false;
        output_row_8(row, part->buf, &ctx);
    }
    return true;
}
