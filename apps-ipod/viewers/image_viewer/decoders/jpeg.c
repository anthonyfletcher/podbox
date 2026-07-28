/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/jpeg/jpeg.c
 * JPEG image viewer (baseline decoder, core build).
 * Ported from the imageviewer plugin.
 *
 * Copyright (C) 2004 Jörg Hohensohn aka [IDC]Dragon
 * Heavily borrowed from the IJG implementation (C) Thomas G. Lane
 * GNU General Public License (version 2+)
 *
 * Baseline JPEG back end for the image viewer, separate from the album-art
 * decoder in draw/.
 ****************************************************************************/

#include <string.h>
#include "system.h"
#include "lcd.h"
#include "file.h"
#include "kernel.h"
#include "../image_viewer.h"
#include "jpeg_decoder.h"
#include "yuv2rgb.h"

/************************* Types ***************************/

struct t_disp
{
    unsigned char* bitmap[3]; /* Y, Cr, Cb */
    int csub_x, csub_y;
    int stride;
};

/************************* Globals ***************************/

/* decompressed image in the possible sizes (1,2,4,8), wasting the other */
static struct t_disp disp[9];

/* the root of the images, hereafter are decompresed ones */
static unsigned char* buf_root;
static int root_size;

/* up to here currently used by image(s) */
static unsigned char* buf_images;
static ssize_t buf_images_size;

static struct jpeg jpg; /* too large for stack */

/************************* Implementation ***************************/

static void draw_image_rect(struct image_info *info,
                            int x, int y, int width, int height)
{
    struct t_disp* pdisp = (struct t_disp*)info->data;
    yuv_bitmap_part(
        pdisp->bitmap, pdisp->csub_x, pdisp->csub_y,
        info->x + x, info->y + y, pdisp->stride,
        x + MAX(0, (LCD_WIDTH - info->width) / 2),
        y + MAX(0, (LCD_HEIGHT - info->height) / 2),
        width, height,
        iv_settings.jpeg_colour_mode, iv_settings.jpeg_dither_mode);
}

static int img_mem(int ds)
{
    int size;
    struct jpeg *p_jpg = &jpg;

    size = (p_jpg->x_phys/ds/p_jpg->subsample_x[0])
         * (p_jpg->y_phys/ds/p_jpg->subsample_y[0]);
    if (p_jpg->blocks > 1) /* colour, add requirements for chroma */
    {
        size += (p_jpg->x_phys/ds/p_jpg->subsample_x[1])
              * (p_jpg->y_phys/ds/p_jpg->subsample_y[1]);
        size += (p_jpg->x_phys/ds/p_jpg->subsample_x[2])
              * (p_jpg->y_phys/ds/p_jpg->subsample_y[2]);
    }
    return size;
}

static int load_image(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size,
                      int offset, int file_size)
{
    int fd;
    unsigned char* buf_jpeg; /* compressed JPEG image */
    int status;
    struct jpeg *p_jpg = &jpg;

    memset(&disp, 0, sizeof(disp));
    memset(&jpg, 0, sizeof(jpg));

    fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        splashf(HZ, "err opening %s: %d", filename, fd);
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

    /* allocate JPEG buffer */
    buf_jpeg = buf;

    /* we can start the decompressed images behind it */
    buf_images = buf_root = buf + file_size;
    buf_images_size = root_size = *buf_size - file_size;

    if (buf_images_size <= 0)
    {
        close(fd);
        return PLUGIN_OUTOFMEM;
    }

    read(fd, buf_jpeg, file_size);
    close(fd);

    /* process markers, unstuffing */
    status = process_markers(buf_jpeg, file_size, p_jpg);

    if (status < 0 || (status & (DQT | SOF0)) != (DQT | SOF0))
    {   /* bad format or minimum components not contained.
         * on colour targets, retry with the progressive decoder. */
        return PLUGIN_JPEG_PROGRESSIVE;
    }

    if (!(status & DHT)) /* if no Huffman table present: */
        default_huff_tbl(p_jpg); /* use default */
    build_lut(p_jpg); /* derive Huffman and other lookup-tables */

    info->x_size = p_jpg->x_size;
    info->y_size = p_jpg->y_size;
    *buf_size = buf_images_size;
    return PLUGIN_OK;
}

static int get_image(struct image_info *info, int frame, int ds)
{
    (void)frame;
    int size; /* decompressed image size */
    int status;
    struct jpeg* p_jpg = &jpg;
    struct t_disp* p_disp = &disp[ds]; /* short cut */

    info->width = p_jpg->x_size / ds;
    info->height = p_jpg->y_size / ds;
    info->data = p_disp;

    if (p_disp->bitmap[0] != NULL)
    {
        /* we still have it */
        return PLUGIN_OK;
    }

    /* assign image buffer */

    /* physical size needed for decoding */
    size = img_mem(ds);
    if (buf_images_size <= size)
    {   /* have to discard the current */
        int i;
        for (i=1; i<=8; i++)
            disp[i].bitmap[0] = NULL; /* invalidate all bitmaps */
        buf_images = buf_root; /* start again from the beginning of the buffer */
        buf_images_size = root_size;
    }

    if (p_jpg->blocks > 1) /* colour jpeg */
    {
        int i;

        for (i = 1; i < 3; i++)
        {
            size = (p_jpg->x_phys / ds / p_jpg->subsample_x[i])
                 * (p_jpg->y_phys / ds / p_jpg->subsample_y[i]);
            p_disp->bitmap[i] = buf_images;
            buf_images += size;
            buf_images_size -= size;
        }
        p_disp->csub_x = p_jpg->subsample_x[1];
        p_disp->csub_y = p_jpg->subsample_y[1];
    }
    else
    {
        p_disp->csub_x = p_disp->csub_y = 0;
        p_disp->bitmap[1] = p_disp->bitmap[2] = buf_images;
    }
    /* size may be less when decoded (if height is not block aligned) */
    size = (p_jpg->x_phys/ds) * (p_jpg->y_size/ds);
    p_disp->bitmap[0] = buf_images;
    buf_images += size;
    buf_images_size -= size;

    /* update image properties */
    p_disp->stride = p_jpg->x_phys / ds; /* use physical size for stride */

    /* the actual decoding */
    cpu_boost(true);
    status = jpeg_decode(p_jpg, p_disp->bitmap, ds, cb_progress);
    cpu_boost(false);
    if (status)
    {
        splashf(HZ, "decode error %d", status);
        return PLUGIN_ERROR;
    }

    return PLUGIN_OK;
}

const struct image_decoder jpeg_decoder = {
    false,
    img_mem,
    load_image,
    get_image,
    draw_image_rect,
};
