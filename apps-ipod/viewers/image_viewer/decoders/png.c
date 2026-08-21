/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/png/png.c
 * PNG image viewer (core build, ported from the imageviewer plugin).
 *
 * Copyright (C) 2009 by Christophe Gouiran
 * Based on lodepng (c) 2005-2008 Lode Vandevenne
 * GNU General Public License (version 2+)
 *
 * PNG back end for the image viewer.
 ****************************************************************************/

#include <string.h>
#include "system.h"
#include "lcd.h"
#include "file.h"
#include "kernel.h"
#include "logf.h"
#include "draw/bmp.h"
#include "tinf.h"
#include "png_decoder.h"
#include "../image_viewer.h"

/* decoder context struct */
static LodePNG_Decoder decoder;

/* decompressed image in the possible sizes (1,2,4,8), wasting the other */
static unsigned char *disp[9];
static unsigned char *disp_buf;

static void draw_image_rect(struct image_info *info,
                            int x, int y, int width, int height)
{
    unsigned char **pdisp = (unsigned char **)info->data;

    lcd_bitmap_part((fb_data *)*pdisp, info->x + x, info->y + y,
                    STRIDE(SCREEN_MAIN, info->width, info->height),
                    x + MAX(0, (LCD_WIDTH-info->width)/2),
                    y + MAX(0, (LCD_HEIGHT-info->height)/2),
                    width, height);
}

static int img_mem(int ds)
{
    LodePNG_Decoder *p_decoder = &decoder;

    if (ds == DS_FIT)
        return iv_fit_width * iv_fit_height * FB_DATA_SZ;

    return (p_decoder->infoPng.width/ds) *
           (p_decoder->infoPng.height/ds) *
           FB_DATA_SZ;
}

static int load_image(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size,
                      int offset, int file_size)
{
    int fd;
    LodePNG_Decoder *p_decoder = &decoder;

    unsigned char *memory, *memory_max, *image;
    size_t memory_size;

    /* cleanup */
    memset(&disp, 0, sizeof(disp));

    /* align buffer */
    memory = (unsigned char *)((intptr_t)(buf + 3) & ~3);
    memory_max = (unsigned char *)((intptr_t)(memory + *buf_size) & ~3);
    memory_size = memory_max - memory;

    fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        logf("png: open failed, %d", fd);
        splashf(HZ * 3, "Could not open %s", filename);
        return PLUGIN_ERROR;
    }

    if (offset)
    {
        lseek(fd, offset, SEEK_SET);
    }
    else
    {
        file_size = filesize(fd);
    }

    if ((size_t)file_size > memory_size) {
        p_decoder->error = FILE_TOO_LARGE;
        close(fd);

    } else {
        /* load file to the end of the buffer */
        image = memory_max - file_size;
        read(fd, image, file_size);
        close(fd);

        /* Initialize decoder context struct, set buffer decoder is free
         * to use. Decoder assumes that raw image file is loaded at the very
         * end of the allocated buffer */
        LodePNG_Decoder_init(p_decoder, memory, memory_size);

        /* read file header; file is loaded at the end of the buffer */
        LodePNG_inspect(p_decoder, image, file_size);

        if (!p_decoder->error) {
            /* the actual decoding */
            cpu_boost(true);
            LodePNG_decode(p_decoder, image, file_size, cb_progress);
            cpu_boost(false);
        }
    }

    if (p_decoder->error) {
        if (p_decoder->error == FILE_TOO_LARGE ||
            p_decoder->error == OUT_OF_MEMORY)
        {
            return PLUGIN_OUTOFMEM;
        }

        logf("png: decode failed, %ld", p_decoder->error);
        splashf(HZ * 3, "Could not decode %s", filename);

        return PLUGIN_ERROR;
    }

    info->x_size = p_decoder->infoPng.width;
    info->y_size = p_decoder->infoPng.height;

    p_decoder->native_img_size = (p_decoder->native_img_size + 3) & ~3;
    disp_buf = p_decoder->buf + p_decoder->native_img_size;
    *buf_size = memory_max - disp_buf;

    return PLUGIN_OK;
}

static int get_image(struct image_info *info, int frame, int ds)
{
    (void)frame;
    unsigned char **p_disp = &disp[ds]; /* short cut */
    LodePNG_Decoder *p_decoder = &decoder;

    if (ds == DS_FIT)
    {
        info->width = iv_fit_width;
        info->height = iv_fit_height;
    }
    else
    {
        info->width = p_decoder->infoPng.width / ds;
        info->height = p_decoder->infoPng.height / ds;
    }
    info->data = p_disp;

    if (*p_disp != NULL)
    {
        /* we still have it */
        return PLUGIN_OK;
    }

    /* assign image buffer */
    if (ds != 1) {
        struct bitmap bmp_src, bmp_dst;

        int size = img_mem(ds);

        if (disp_buf + size >= p_decoder->buf + p_decoder->buf_size) {
            /* have to discard the current */
            int i;
            for (i=0; i<=8; i++)
                disp[i] = NULL; /* invalidate all bitmaps */

            /* start again from the beginning of the buffer */
            disp_buf = p_decoder->buf + p_decoder->native_img_size;
        }

        *p_disp = disp_buf;
        disp_buf += size;

        bmp_src.width = p_decoder->infoPng.width;
        bmp_src.height = p_decoder->infoPng.height;
        bmp_src.data = p_decoder->buf;

        bmp_dst.width = info->width;
        bmp_dst.height = info->height;
        bmp_dst.data = *p_disp;
        cpu_boost(true);
        smooth_resize_bitmap(&bmp_src, &bmp_dst);
        cpu_boost(false);
    } else {
        *p_disp = p_decoder->buf;
    }

    return PLUGIN_OK;
}

const struct image_decoder png_decoder = {
    true,
    img_mem,
    load_image,
    get_image,
    draw_image_rect,
};
