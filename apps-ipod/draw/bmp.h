/***************************************************************************
 * Original code from RockBox
 * was: apps/recorder/bmp.h
 * Copyright (C) 2002 by Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * Interface to bmp.c, plus the shared image types (uint8_rgb, dim, rowset)
 * and IMG_* load flags used by every image path.
 ****************************************************************************/
#ifndef _BMP_H_
#define _BMP_H_

#include "config.h"
#include "lcd.h"
#include "inttypes.h"
#include "resize.h"

#define ARRAY_SIZE(array)    (int)(sizeof(array)/(sizeof(array[0])))

#define IMG_NORESIZE           0
#define IMG_RESIZE             1
#define BM_MAX_WIDTH (((LCD_WIDTH) + 7) & ~7)

struct uint8_rgb {
    /* Little endian */
    uint8_t blue;
    uint8_t green;
    uint8_t red;
    uint8_t alpha;
};

struct dim {
    short width;
    short height;
};

struct rowset {
    short rowstep;
    short rowstart;
    short rowstop;
};

extern const unsigned char dither_table[16];
#define DITHERY(y) (dither_table[(y) & 15] & 0xAA)
#define DITHERX(x) (dither_table[(x) & 15])
#define DITHERXDY(x,dy) (DITHERX(x) ^ dy)
#define DITHERDXY(dx,y) (dx ^ DITHERY(y))
#define DITHERXY(x,y) (DITHERX(x) ^ DITHERY(y))

/* The /256 version has a mean squared variance from YUV luma of <1 grey level.
   The /8 version is a good deal less accurate, but sufficient on mono as we
   don't support HQ output or dithering there, yet.
*/
static inline unsigned brightness(struct uint8_rgb color)
{
    return (77 * (unsigned)color.red + 150 * (unsigned)color.green
              + 29 * (unsigned)color.blue) / 256;
}


/* Number of rows of data in a mono bitmap height pixels tall */
#define MONO_BM_HEIGHT(height) (((height) + 7) >> 3)

/* Number of rows of datain a LCD native bitmap height pixels tall */
#define LCD_BM_HEIGHT(height) (height)

#define NATIVE_BM_HEIGHT(height,remote) LCD_BM_HEIGHT(height)

/* Convenience macro to calculate rows based on height, remote vs main LCD,
   and format
*/
#define BM_HEIGHT(height,format,remote) ((format) == FORMAT_MONO ? \
    MONO_BM_HEIGHT(height) : NATIVE_BM_HEIGHT(height,remote))

/* Number of data elements in a mono bitmap width pixels wide */
#define MONO_BM_WIDTH(width) (width)

/* Number of data elements in a LCD native bitmap width pixels wide */
#define LCD_BM_WIDTH(width) (width)

#define NATIVE_BM_WIDTH(width,remote) LCD_BM_WIDTH(width)

/* Convenience macro to calculate elements based on height, remote vs native
   main LCD, and format
*/
#define BM_WIDTH(width,format,remote) ((format) == FORMAT_MONO ? \
    MONO_BM_WIDTH(width) : NATIVE_BM_WIDTH(width,remote))

/* Size in bytes of a mono bitmap of dimensions width*height */
#define MONO_BM_SIZE(width,height) (MONO_BM_WIDTH(width) * \
    MONO_BM_HEIGHT(height) * FB_DATA_SZ)

/* Size in bytes of a native bitmap of dimensions width*height */
#define NATIVE_BM_SIZE(width,height,format,remote) \
    (FB_DATA_SZ * BM_WIDTH(width,format,remote) * \
    BM_HEIGHT(height,format,remote))

/* Convenience macro to calculate size in bytes based on height, remote vs
   main LCD, and format
*/
#define BM_SIZE(width,height,format,remote) (((format) == FORMAT_MONO) ? \
    MONO_BM_SIZE(width,height) : NATIVE_BM_SIZE(width,height,format,remote))

/* Size in bytes needed to load and scale a bitmap with target size up to
   width*height, including overhead to allow for buffer alignment.
*/
#define BM_SCALED_SIZE(width,height,format,remote) \
    (BM_SIZE(width,height,format,remote) + \
    (remote ? 0 : BM_WIDTH(width,format,remote) * sizeof(uint32_t) * 9 + 3))

/*********************************************************************
 * read_bmp_file()
 *
 * Reads a 8bit BMP file and puts the data in a 1-pixel-per-byte
 * array.
 * Returns < 0 for error, or number of bytes used from the bitmap buffer
 *
 **********************************************/
int read_bmp_file(const char* filename,
                  struct bitmap *bm,
                  int maxsize,
                  int format,
                  const struct custom_format *cformat);

int read_bmp_fd(int fd,
                struct bitmap *bm,
                int maxsize,
                int format,
                const struct custom_format *cformat);

void output_row_8_native(uint32_t row, void * row_in,
                         struct scaler_context *ctx);
#endif
