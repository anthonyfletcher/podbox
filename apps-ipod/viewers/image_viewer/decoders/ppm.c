/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/ppm/ppm.c
 * PPM image viewer (core build, ported from the imageviewer plugin).
 *
 * Copyright (C) 2010 Marcin Bukat
 * GNU General Public License (version 2+)
 *
 * PPM/PGM back end for the image viewer.
 ****************************************************************************/

#include <string.h>
#include "system.h"
#include "lcd.h"
#include "file.h"
#include "kernel.h"
#include "draw/bmp.h"        /* struct bitmap */
#include "ppm_decoder.h"
#include "../image_viewer.h"

/* decompressed image in the possible sizes (1,2,4,8), wasting the other */
static unsigned char *disp[9];
static unsigned char *disp_buf;
static struct ppm_info ppm;

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
    return (ppm.x/ds) * (ppm.y/ds) * FB_DATA_SZ;
}

static int load_image(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size,
                      int offset, int filesize)
{
    (void)filesize;
    int fd;
    int rc = PLUGIN_OK;

    unsigned char *memory, *memory_max;
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
        splashf(HZ, "err opening %s: %d", filename, fd);
        return PLUGIN_ERROR;
    }

    if (offset)
    {
        lseek(fd, offset, SEEK_SET);
    }

    /* init decoder struct */
    ppm.buf = memory;
    ppm.buf_size = memory_size;

    /* the actual decoding */
    cpu_boost(true);
    rc = read_ppm(fd, &ppm);
    cpu_boost(false);

    close(fd);

    if (rc != PLUGIN_OK)
    {
        return rc;
    }

    info->x_size = ppm.x;
    info->y_size = ppm.y;

    ppm.native_img_size = (ppm.native_img_size + 3) & ~3;
    disp_buf = buf + ppm.native_img_size;
    *buf_size = memory_max - disp_buf;

    return PLUGIN_OK;
}

static int get_image(struct image_info *info, int frame, int ds)
{
    (void)frame;
    unsigned char **p_disp = &disp[ds]; /* short cut */
    struct ppm_info *p_ppm = &ppm;

    info->width = ppm.x / ds;
    info->height = ppm.y / ds;
    info->data = p_disp;

    if (*p_disp != NULL)
    {
        /* we still have it */
        return PLUGIN_OK;
    }

    /* assign image buffer */
    if (ds > 1)
    {
        struct bitmap bmp_src, bmp_dst;
        int size = img_mem(ds);

        if (disp_buf + size >= p_ppm->buf + p_ppm->buf_size)
        {
            /* have to discard the current */
            int i;
            for (i=1; i<=8; i++)
                disp[i] = NULL; /* invalidate all bitmaps */

            /* start again from the beginning of the buffer */
            disp_buf = p_ppm->buf + p_ppm->native_img_size;
        }

        *p_disp = disp_buf;
        disp_buf += size;

        bmp_src.width = ppm.x;
        bmp_src.height = ppm.y;
        bmp_src.data = ppm.buf;

        bmp_dst.width = info->width;
        bmp_dst.height = info->height;
        bmp_dst.data = *p_disp;
        cpu_boost(true);
        smooth_resize_bitmap(&bmp_src, &bmp_dst);
        cpu_boost(false);
    }
    else
    {
        *p_disp = p_ppm->buf;
    }

    return PLUGIN_OK;
}

const struct image_decoder ppm_decoder = {
    true,
    img_mem,
    load_image,
    get_image,
    draw_image_rect,
};
