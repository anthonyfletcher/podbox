/***************************************************************************
 * Original code from RockBox
 * was: apps/recorder/resize.h
 * Copyright (C) 2008 by Akio Idehara
 * GNU General Public License (version 2+)
 *
 * Interface to resize.c.
 ****************************************************************************/
#ifndef _RESIZE_H_
#define _RESIZE_H_
#include "config.h"
#include "lcd.h"
#include "inttypes.h"

/****************************************************************
 * resize_on_load()
 *
 * resize bitmap on load with scaling
 *
 * If HAVE_LCD_COLOR then this func use smooth scaling algorithm
 * - downscaling both way use "Area Sampling"
 *   if IMG_RESIZE_BILINER or IMG_RESIZE_NEAREST is NOT set
 * - otherwise "Bilinear" or "Nearest Neighbour"
 *
 * If !(HAVE_LCD_COLOR) then use simple scaling algorithm "Nearest Neighbour"
 * 
 * return -1 for error
 ****************************************************************/

/* nothing needs the on-stack buffer right now */
#define MAX_SC_STACK_ALLOC 0
#define HAVE_UPSCALER 1

#define SC_OUT(n, c) (((n) + (1 << 23)) >> 24)
#ifndef SC_OUT
#define SC_OUT(n, c) (sc_mul_u32_rnd(n, (c)->recip))
#endif

struct img_part {
    int len;
    struct uint8_rgb* buf;
};

/* intermediate type used by the scaler for color output. greyscale version
   uses uint32_t
*/
struct uint32_argb {
    uint32_t r;
    uint32_t g;
    uint32_t b;
    uint32_t a;
};

/* struct which contains various parameters shared between vertical scaler,
   horizontal scaler, and row output
*/
struct scaler_context {
    const struct custom_format *format;
    uint32_t h_i_val;
    uint32_t h_o_val;
    uint32_t v_i_val;
    uint32_t v_o_val;
    struct bitmap *bm;
    struct dim *src;
    unsigned char *buf;
    bool dither;
    int len;
    void *args;
    struct img_part* (*store_part)(void *);
    void (*output_row)(uint32_t,void*,struct scaler_context*);
    bool (*h_scaler)(void*,struct scaler_context*, bool);
};

#define IF_PIX_FMT(...) __VA_ARGS__

struct custom_format {
    void (*output_row_8)(uint32_t,void*, struct scaler_context*);
    void (*output_row_32[2])(uint32_t,void*, struct scaler_context*);
    unsigned int (*get_size)(struct bitmap *bm);
    bool (*progress)(int done, int rows);
};

struct rowset;

extern const struct custom_format format_native;

/* A decode owns its progress callback. No callback is shared between jobs. */
static inline bool image_progress(const struct custom_format *format,
                                  int done, int rows)
{
    return !format || !format->progress || format->progress(done, rows);
}

int recalc_dimension(struct dim *dst, struct dim *src);

/* Added to format_index: each part's buf is packed R, G, B bytes rather than
 * struct uint8_rgb, and its len counts pixels. Only for a source wider than
 * the bitmap, which the area scaler takes; resize_on_load() refuses it
 * otherwise. */
#define RESIZE_RGB24 2

int resize_on_load(struct bitmap *bm, bool dither,
                   struct dim *src, struct rowset *tmp_row,
                   unsigned char *buf, unsigned int len,
                   const struct custom_format *cformat,
                   IF_PIX_FMT(int format_index,)
                   struct img_part* (*store_part)(void *args),
                   void *args);

/* The shared ends of the PNG, GIF and WebP readers. image_fit_bitmap() sizes
 * the bitmap for a source of `src` as `format` asks and returns its size in
 * bytes. image_store_rows() writes every row from store_part(args): through
 * the scaler, in `buf`, when the sizes differ, else straight out. False if a
 * row fails or the request callback stops it. */
int image_fit_bitmap(struct bitmap *bm, struct dim *src, int format,
                     const struct custom_format *cformat);
bool image_store_rows(struct bitmap *bm, bool dither, struct dim *src,
                      unsigned char *buf, unsigned int len,
                      const struct custom_format *cformat, int format_index,
                      struct img_part *(*store_part)(void *args), void *args);

#endif /* _RESIZE_H_ */
