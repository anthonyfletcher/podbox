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
#include "logf.h"
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

/* decompressed image in the possible sizes: DS_FIT and 1,2,4,8, wasting the
 * other slots */
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

/* Horizontal and vertical chroma subsampling of the fit rendering, 1 when the
 * picture is greyscale and has no chroma planes at all. */
static int fit_csub_x(void)
{
    struct jpeg *p_jpg = &jpg;
    return (p_jpg->blocks > 1) ? p_jpg->subsample_x[1] : 1;
}

static int fit_csub_y(void)
{
    struct jpeg *p_jpg = &jpg;
    return (p_jpg->blocks > 1) ? p_jpg->subsample_y[1] : 1;
}

/* Line length of the fit rendering. yuv_bitmap_part() addresses the chroma
 * planes as stride/csub_x, so the stride has to be a whole number of chroma
 * samples -- up to one pixel wider than the picture. */
static int fit_stride(void)
{
    int csub_x = fit_csub_x();
    return (iv_fit_width + csub_x - 1) / csub_x * csub_x;
}

static int img_mem(int ds)
{
    int size;
    struct jpeg *p_jpg = &jpg;

    if (ds == DS_FIT)
    {
        int stride = fit_stride();

        size = stride * iv_fit_height;
        if (p_jpg->blocks > 1)
            size += 2 * (stride / fit_csub_x())
                      * ((iv_fit_height + fit_csub_y() - 1) / fit_csub_y());
        return size;
    }

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
        logf("jpeg: open failed, %d", fd);
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

static int get_image(struct image_info *info, int frame, int ds);

/* Box-average an 8-bit plane down to dst_w x dst_h. */
static void resample_plane(const unsigned char *src, int src_stride,
                           int src_w, int src_h,
                           unsigned char *dst, int dst_stride,
                           int dst_w, int dst_h)
{
    int dy, dx, y, x;

    for (dy = 0; dy < dst_h; dy++)
    {
        int y0 = dy * src_h / dst_h;
        int y1 = (dy + 1) * src_h / dst_h;

        if (y1 <= y0)
            y1 = y0 + 1;

        for (dx = 0; dx < dst_w; dx++)
        {
            int x0 = dx * src_w / dst_w;
            int x1 = (dx + 1) * src_w / dst_w;
            int sum = 0;

            if (x1 <= x0)
                x1 = x0 + 1;

            for (y = y0; y < y1; y++)
                for (x = x0; x < x1; x++)
                    sum += src[y * src_stride + x];

            dst[dy * dst_stride + dx] = sum / ((y1 - y0) * (x1 - x0));
        }
    }
}

/* Render the fit view.
 *
 * The IDCT scales by 1, 2, 4 or 8 and refuses anything else, and the result is
 * three subsampled YUV planes that yuv_bitmap_part() converts at 1:1 -- so
 * neither stage can land on an arbitrary size. Decode the smallest integer
 * rung that is still at least as large as the fit view, then box-average each
 * plane down to it. Colour mode and dithering are untouched by this: the
 * conversion still happens where it always did, at draw time.
 *
 * Both allocations are made after one reset up front, because the bump
 * allocator's own out-of-room path empties the whole cache -- which would take
 * away the source this is reading from. */
static int get_image_fit(struct image_info *info, int frame)
{
    struct jpeg* p_jpg = &jpg;
    struct t_disp* p_fit = &disp[DS_FIT];
    struct t_disp* p_src;
    int src_ds = iv_fit_source_ds(p_jpg->x_size, p_jpg->y_size);
    int stride = fit_stride();
    int csub_x = fit_csub_x(), csub_y = fit_csub_y();
    int src_w, src_h, status, i;

    if (buf_images_size <= img_mem(src_ds) + img_mem(DS_FIT))
    {
        for (i = 0; i <= 8; i++)
            disp[i].bitmap[0] = NULL; /* invalidate all bitmaps */
        buf_images = buf_root;
        buf_images_size = root_size;
    }

    status = get_image(info, frame, src_ds);
    if (status != PLUGIN_OK)
        return status;

    p_src = &disp[src_ds];
    src_w = info->width;   /* get_image() left the source's size on info */
    src_h = info->height;

    p_fit->csub_x = p_src->csub_x;
    p_fit->csub_y = p_src->csub_y;
    p_fit->stride = stride;

    p_fit->bitmap[0] = buf_images;
    buf_images += stride * iv_fit_height;
    buf_images_size -= stride * iv_fit_height;

    resample_plane(p_src->bitmap[0], p_src->stride, src_w, src_h,
                   p_fit->bitmap[0], stride, iv_fit_width, iv_fit_height);

    if (p_jpg->blocks > 1) /* colour */
    {
        int cw = stride / csub_x;
        int ch = (iv_fit_height + csub_y - 1) / csub_y;

        for (i = 1; i < 3; i++)
        {
            p_fit->bitmap[i] = buf_images;
            buf_images += cw * ch;
            buf_images_size -= cw * ch;

            resample_plane(p_src->bitmap[i], p_src->stride / csub_x,
                           src_w / csub_x, src_h / csub_y,
                           p_fit->bitmap[i], cw, cw, ch);
        }
    }
    else
    {
        p_fit->bitmap[1] = p_fit->bitmap[2] = buf_images;
    }

    info->width = iv_fit_width;
    info->height = iv_fit_height;
    info->data = p_fit;
    return PLUGIN_OK;
}

static int get_image(struct image_info *info, int frame, int ds)
{
    (void)frame;
    int size; /* decompressed image size */
    int status;
    struct jpeg* p_jpg = &jpg;
    struct t_disp* p_disp = &disp[ds]; /* short cut */

    if (ds == DS_FIT)
    {
        info->width = iv_fit_width;
        info->height = iv_fit_height;
        info->data = p_disp;

        if (p_disp->bitmap[0] != NULL)
            return PLUGIN_OK;

        return get_image_fit(info, frame);
    }

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
        for (i=0; i<=8; i++)
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
        logf("jpeg: decode failed, %d", status);
        splash(HZ * 2, "Could not decode the image");
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
