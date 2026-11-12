/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/bmp/bmp.c
 * BMP image viewer (core build, ported from the imageviewer plugin).
 * GNU General Public License (version 2+)
 *
 * The image viewer's back end for every image the core readers decode
 * through image_load(): JPEG, BMP, PNG and WebP files, and now-playing art
 * embedded in a track, whatever its format, with ordered dithering.
 *
 * Opening reads only the image's size. Each zoom level is decoded from the
 * file at its own size when it is first shown, so a large photo opens at the
 * cost of a screenful -- a JPEG decodes at 1/8 scale, an interlaced PNG reads
 * its first pass -- and only a zoom pays for detail. The fit rendering has a
 * slot of its own at the front of the buffer and is never discarded; the
 * other levels share the rest, and all of them go when a new one does not
 * fit beside them.
 ****************************************************************************/

#include <string.h>
#include "system.h"
#include "lcd.h"
#include "file.h"
#include "kernel.h"
#include "logf.h"
#include "string-extra.h"    /* strmemccpy */
#include "draw/bmp.h"        /* struct bitmap, FORMAT_* */
#include "draw/resize.h"     /* format_native */
#include "draw/image_load.h"
#include "../image_viewer.h"

/************************* Types ***************************/

struct t_disp
{
    unsigned char* bitmap;
};

/************************* Globals ***************************/

/* the renderings: DS_FIT and 1,2,4,8, wasting the other slots */
static struct t_disp disp[9];

/* The fit rendering's slot, then the space the other levels share */
#define FIT_SLOT (LCD_WIDTH * LCD_HEIGHT * (int)sizeof (fb_data))
static unsigned char* buf_fit;
static unsigned char* buf_root;
static int root_size;

/* up to here currently used by the other levels */
static unsigned char* buf_images;
static ssize_t buf_images_size;

/* where the image is, to read it again for each level */
static char src_name[MAX_PATH];
static struct image_src src;
static int src_width, src_height;

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
    if (ds == DS_FIT)
        return iv_fit_width * iv_fit_height * sizeof (fb_data);

    return (src_width/ds) * (src_height/ds) * sizeof (fb_data);
}

/* Decodes the image at w x h into 'out', with 'size' bytes there for the
 * bitmap and the decode's working memory, and puts the bitmap's size in
 * *used. PLUGIN_OK, or the status that stopped it. */
static int decode(unsigned char *out, int size, int w, int h, int *used)
{
    int format = FORMAT_NATIVE | FORMAT_DITHER;
    struct bitmap bm = { .width = w, .height = h, .data = out };
    int ret;

    if (w != src_width || h != src_height)
        format |= FORMAT_RESIZE;

    src.fd = open(src_name, O_RDONLY);
    if (src.fd < 0)
        return PLUGIN_ERROR;
    if (image_load(&src, &bm, size, format | FORMAT_RETURN_SIZE,
                   &format_native) > size)
    {
        close(src.fd);
        return PLUGIN_OUTOFMEM;
    }

    iv_decode_begin();
    struct custom_format decode_format = format_native;
    decode_format.progress = iv_decode_row;
    cpu_boost(true);
    ret = image_load(&src, &bm, size, format, &decode_format);
    cpu_boost(false);

    close(src.fd);

    if (iv_decode_stop)
        return iv_decode_stop;
    if (ret <= 0)
    {
        logf("scaled: decode failed, %d", ret);
        splashf(HZ * 3, "Could not decode %s", src_name);
        return PLUGIN_ERROR;
    }
    *used = ret;
    return PLUGIN_OK;
}

static int load_image(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size,
                      int offset, int filesize)
{
    struct bitmap bm = { .data = buf };
    int size;

    memset(&disp, 0, sizeof(disp));

    if ((intptr_t)buf & 3)
    {
        *buf_size -= 4-((intptr_t)buf&3);
        buf += 4-((intptr_t)buf&3);
    }
    strmemccpy(src_name, filename, sizeof(src_name));
    src.pos = offset;
    src.len = offset ? filesize : -1;
    src.flags = iv_src_flags;

    /* The image's own size. The readers work in the buffer even to answer
     * this, so it is passed. */
    src.fd = open(filename, O_RDONLY);
    if (src.fd < 0)
    {
        logf("scaled: open failed, %d", src.fd);
        splashf(HZ * 3, "Could not open %s", filename);
        return PLUGIN_ERROR;
    }
    bm.data = buf;
    size = image_load(&src, &bm, *buf_size, FORMAT_NATIVE | FORMAT_RETURN_SIZE,
                      &format_native);
    close(src.fd);
    if (size <= 0 || bm.width < 1 || bm.height < 1)
    {
        logf("scaled: size probe failed, %d", size);
        splashf(HZ * 3, "Could not read %s", filename);
        return PLUGIN_ERROR;
    }
    src_width = bm.width;
    src_height = bm.height;

    if (*buf_size <= FIT_SLOT)
        return PLUGIN_OUTOFMEM;
    buf_fit = buf;
    buf_images = buf_root = buf + FIT_SLOT;
    buf_images_size = root_size = *buf_size - FIT_SLOT;

    info->x_size = src_width;
    info->y_size = src_height;

    /* What the levels may take, less what decoding the widest one needs
     * beside its bitmap */
    *buf_size = root_size - IMAGE_DECODE_OVERHEAD
                - 3 * (int)sizeof(struct uint32_argb) * src_width;
    return *buf_size > 0 ? PLUGIN_OK : PLUGIN_OUTOFMEM;
}

static int get_image(struct image_info *info, int frame, int ds)
{
    (void)frame;
    struct t_disp* p_disp = &disp[ds]; /* short cut */
    int status, size;

    if (ds == DS_FIT)
    {
        info->width = iv_fit_width;
        info->height = iv_fit_height;
    }
    else
    {
        info->width = src_width/ds;
        info->height = src_height/ds;
    }
    info->data = p_disp;

    if (p_disp->bitmap != NULL)
        return PLUGIN_OK;   /* we still have it */

    if (ds == DS_FIT)
    {
        /* Its working memory runs on into the shared space */
        for (int i = 1; i <= 8; i++)
            disp[i].bitmap = NULL;
        buf_images = buf_root;
        buf_images_size = root_size;
        status = decode(buf_fit, FIT_SLOT + root_size,
                        info->width, info->height, &size);
        if (status == PLUGIN_OK)
            p_disp->bitmap = buf_fit;
        return status;
    }

    status = decode(buf_images, buf_images_size, info->width, info->height,
                    &size);
    if (status == PLUGIN_OUTOFMEM && buf_images != buf_root)
    {
        /* no room beside the others: drop them and use it all */
        for (int i = 1; i <= 8; i++)
            disp[i].bitmap = NULL;
        buf_images = buf_root;
        buf_images_size = root_size;
        status = decode(buf_images, buf_images_size,
                        info->width, info->height, &size);
    }
    if (status != PLUGIN_OK)
        return status;

    p_disp->bitmap = buf_images;
    size = ALIGN_UP(size, 4);
    buf_images += size;
    buf_images_size -= size;
    return PLUGIN_OK;
}

const struct image_decoder scaled_decoder = {
    false,
    img_mem,
    load_image,
    get_image,
    draw_image_rect,
};
