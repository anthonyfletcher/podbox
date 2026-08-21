/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/bmp/bmp.c
 * BMP image viewer (core build, ported from the imageviewer plugin).
 * GNU General Public License (version 2+)
 *
 * BMP back end for the image viewer, wrapping the core BMP reader in the
 * decoder vtable.
 ****************************************************************************/

#include <string.h>
#include "system.h"
#include "lcd.h"
#include "file.h"
#include "kernel.h"
#include "logf.h"
#include "draw/bmp.h"        /* struct bitmap, read_bmp_fd, FORMAT_* */
#include "draw/resize.h"     /* format_native */
#include "../image_viewer.h"

/************************* Types ***************************/

struct t_disp
{
    unsigned char* bitmap;
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

static struct bitmap bmp;

/************************* Implementation ***************************/

static void draw_image_rect(struct image_info *info,
                            int x, int y, int width, int height)
{
    struct t_disp* pdisp = (struct t_disp*)info->data;
    lcd_bitmap_part(
        (fb_data*)pdisp->bitmap, info->x + x, info->y + y,
        STRIDE(SCREEN_MAIN, info->width, info->height),
        x + MAX(0, (LCD_WIDTH-info->width)/2),
        y + MAX(0, (LCD_HEIGHT-info->height)/2),
        width, height);
}

static int img_mem(int ds)
{
    return (bmp.width/ds) * (bmp.height/ds) * sizeof (fb_data);
}

static int load_image(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size,
                      int offset, int filesize)
{
    (void)filesize;
    int fd;
    int size;
    int format = FORMAT_NATIVE;
    const struct custom_format *cformat = &format_native;

    memset(&disp, 0, sizeof(disp));
    memset(&bmp, 0, sizeof(bmp)); /* clear info struct */

    if ((intptr_t)buf & 3)
    {
        *buf_size -= 4-((intptr_t)buf&3);
        buf += 4-((intptr_t)buf&3);
    }
    fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        logf("bmp: open failed, %d", fd);
        splashf(HZ * 3, "Could not open %s", filename);
        return PLUGIN_ERROR;
    }
    if (offset)
    {
        lseek(fd, offset, SEEK_SET);
    }

    int ds = 1;
    /* check size of image needed to load image. */
    size = read_bmp_fd(fd, &bmp, 0, format | FORMAT_RETURN_SIZE, cformat);
    while (size > *buf_size && bmp.width >= 2 && bmp.height >= 2 && ds < 8)
    {
        /* too large for the buffer. resize on load. */
        format |= FORMAT_RESIZE|FORMAT_KEEP_ASPECT;
        ds *= 2;
        bmp.width /= 2;
        bmp.height /= 2;
        lseek(fd, 0, SEEK_SET);
        size = read_bmp_fd(fd, &bmp, 0, format | FORMAT_RETURN_SIZE, cformat);
    }
    if (size <= 0)
    {
        close(fd);
        logf("bmp: size probe failed, %d", size);
        splashf(HZ * 3, "Could not read %s", filename);
        return PLUGIN_ERROR;
    }

    if (size > *buf_size)
    {
        close(fd);
        return PLUGIN_OUTOFMEM;
    }

    /* allocate bitmap buffer */
    bmp.data = buf;

    /* actual loading */
    lseek(fd, 0, SEEK_SET);
    cpu_boost(true);
    size = read_bmp_fd(fd, &bmp, *buf_size, format, cformat);
    cpu_boost(false);
    close(fd);

    if (size <= 0)
    {
        logf("bmp: decode failed, %d", size);
        splashf(HZ * 3, "Could not decode %s", filename);
        return PLUGIN_ERROR;
    }

    /* we can start the resized images behind it */
    buf_images = buf_root = buf + size;
    buf_images_size = root_size = *buf_size - size;

    info->x_size = bmp.width;
    info->y_size = bmp.height;
    *buf_size = buf_images_size;
    return PLUGIN_OK;
}

static int get_image(struct image_info *info, int frame, int ds)
{
    (void)frame;
    struct t_disp* p_disp = &disp[ds]; /* short cut */

    info->width = bmp.width/ds;
    info->height = bmp.height/ds;
    info->data = p_disp;

    if (p_disp->bitmap != NULL)
    {
        /* we still have it */
        return PLUGIN_OK;
    }

    /* assign image buffer */
    if (ds > 1)
    {
        int size; /* resized image size */
        struct bitmap bmp_dst;

        size = img_mem(ds);
        if (buf_images_size <= size)
        {
            /* have to discard the current */
            int i;
            for (i=1; i<=8; i++)
                disp[i].bitmap = NULL; /* invalidate all bitmaps */
            buf_images = buf_root; /* start again from the beginning of the buffer */
            buf_images_size = root_size;
        }

        p_disp->bitmap = buf_images;
        buf_images += size;
        buf_images_size -= size;

        bmp_dst.width = info->width;
        bmp_dst.height = info->height;
        bmp_dst.data = p_disp->bitmap;
        cpu_boost(true);
        smooth_resize_bitmap(&bmp, &bmp_dst);
        cpu_boost(false);
    }
    else
    {
        p_disp->bitmap = bmp.data;
    }

    return PLUGIN_OK;
}

const struct image_decoder bmp_decoder = {
    true,
    img_mem,
    load_image,
    get_image,
    draw_image_rect,
};
