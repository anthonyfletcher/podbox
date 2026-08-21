/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/jpegp/jpegp.c
 * Progressive JPEG image viewer (RAINBOW decoder, core build).
 * Ported from the imageviewer plugin.
 * GNU General Public License (version 2+)
 *
 * Progressive JPEG back end, using the vendored jpeg81 coefficient
 * decoder.
 ****************************************************************************/

#include <string.h>
#include "system.h"
#include "lcd.h"
#include "kernel.h"
#include "logf.h"
#include "../image_viewer.h"

#include "jpeg81.h"
#include "idct.h"
#include "GETC.h"
#include "jpegp_glue.h"   /* malloc->jpegp_malloc; keep after the core headers */

/************************* Types ***************************/

struct t_disp
{
    unsigned char* bitmap;
};

/************************* Globals ***************************/

/* decompressed image in the possible sizes: DS_FIT and 1,2,4,8, wasting the
 * other slots */
static struct t_disp disp[9];

static struct JPEGD jpg; /* too large for stack */

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
    struct JPEGD* j = &jpg;

    if (ds == DS_FIT)
        return iv_fit_width * iv_fit_height * sizeof(fb_data);

    return j->Y/ds * j->X/ds*sizeof(fb_data);
}

static void scaled_dequantization_and_idct(void)
{
    struct JPEGD* j = &jpg;
    /* Based on RAINBOW lib jpeg2bmp example. */
    int c, i, n;

    /* Pre-scale quant-tables */
    int SQ[4][64];
    for (c=0; c<4 && j->QT[c][0]; c++)
    {
        int *q= j->QT[c], *sq= SQ[c];
        for (i=0; i<64; i++) sq[i]= q[i] * SCALEM[zigzag[i]];
    }

    /* DEQUANT + IDCT */
    for (c=0; c<j->Nf; c++)
    {
        struct COMP *C= j->Components+c;
        int *sq= SQ[C->Qi];

        for (n=0; n < C->du_size; n++)
        {
            /* scaled idct with dequant */
            idct_sq( C->du[ (n / C->du_w) * C->du_width +  n % C->du_w ], sq );
        }
    }
}

static int load_image(char *filename, struct image_info *info,
                      unsigned char *buf, ssize_t *buf_size,
                      int offset, int filesize)
{
    (void)filesize;
    int status;
    struct JPEGD *p_jpg = &jpg;

    memset(&disp, 0, sizeof(disp));
    memset(&jpg, 0, sizeof(jpg));

    init_mem_pool(buf, *buf_size);

    if (!OPEN(filename))
    {
        return PLUGIN_ERROR;
    }
    if (offset)
    {
        POS(offset);
    }

    /* the actual decoding */
    status = JPEGDecode(p_jpg);

    CLOSE();

    if (status < 0)
    {   /* bad format or minimum components not contained */
        if (status == JPEGENUMERR_MALLOC)
        {
            return PLUGIN_OUTOFMEM;
        }
        logf("jpegp: decode failed, %d", status);
        splashf(HZ * 3, "Could not decode %s", filename);
        return  PLUGIN_ERROR;
    }

    info->x_size = p_jpg->X;
    info->y_size = p_jpg->Y;

    if ( 3 != p_jpg->Nf )
        return PLUGIN_ERROR;

    scaled_dequantization_and_idct();

    *buf_size = freeze_mem_pool();
    return PLUGIN_OK;
}

static int get_image(struct image_info *info, int frame, int ds);

/* Render the fit view: the RAINBOW loop below steps the source in whole
 * pixels, so it cannot land on an arbitrary size in one pass. Take the
 * smallest integer rung that is still at least as large and shrink that.
 *
 * Trap: the pool allocator empties itself and clears every cached rendering
 * when it runs out, so a second allocation can take the first one's source
 * away. That is what the retry is for -- the second pass starts from an empty
 * pool, where both fit. */
static int get_image_fit(struct image_info *info, int frame)
{
    struct JPEGD* p_jpg = &jpg;
    struct t_disp* p_fit = &disp[DS_FIT];
    int src_ds = iv_fit_source_ds(p_jpg->X, p_jpg->Y);
    struct bitmap bmp_src, bmp_dst;
    int status, tries;

    for (tries = 0; tries < 2; tries++)
    {
        status = get_image(info, frame, src_ds);
        if (status != PLUGIN_OK)
            return status;

        p_fit->bitmap = malloc(img_mem(DS_FIT));
        if (p_fit->bitmap != NULL)
            break;

        /* the pool is full of other renderings: empty it and go round once
         * more, which re-renders the source into a pool that now has room */
        clear_mem_pool();
        memset(&disp, 0, sizeof(disp));
    }

    if (p_fit->bitmap == NULL)
        return PLUGIN_ERROR;

    /* get_image() left the source's size on info */
    bmp_src.width = info->width;
    bmp_src.height = info->height;
    bmp_src.data = disp[src_ds].bitmap;

    bmp_dst.width = iv_fit_width;
    bmp_dst.height = iv_fit_height;
    bmp_dst.data = p_fit->bitmap;

    cpu_boost(true);
    smooth_resize_bitmap(&bmp_src, &bmp_dst);
    cpu_boost(false);

    info->width = iv_fit_width;
    info->height = iv_fit_height;
    info->data = p_fit;
    return PLUGIN_OK;
}

static int get_image(struct image_info *info, int frame, int ds)
{
    (void)frame;
    struct JPEGD* p_jpg = &jpg;
    struct t_disp* p_disp = &disp[ds]; /* short cut */

    if (ds == DS_FIT)
    {
        info->width = iv_fit_width;
        info->height = iv_fit_height;
        info->data = p_disp;

        if (p_disp->bitmap != NULL)
            return PLUGIN_OK;

        return get_image_fit(info, frame);
    }

    info->width = p_jpg->X / ds;
    info->height = p_jpg->Y / ds;
    info->data = p_disp;

    if (p_disp->bitmap != NULL)
    {
        /* we still have it */
        return PLUGIN_OK;
    }

    struct JPEGD* j = p_jpg;
    int mem = img_mem(ds);

    p_disp->bitmap = malloc(mem);

    if (!p_disp->bitmap)
    {
        clear_mem_pool();
        memset(&disp, 0, sizeof(disp));
        p_disp->bitmap = malloc(mem);
        if (!p_disp->bitmap)
            return PLUGIN_ERROR;
    }

    fb_data *bmp = (fb_data *)p_disp->bitmap;

    /* Based on RAINBOW lib jpeg2bmp example.
     * Primitive yuv-rgb converter for all sub-sampling types. */
    int h0 = j->Hmax / j->Components[0].Hi;
    int v0 = j->Vmax / j->Components[0].Vi;
    int h1 = j->Hmax / j->Components[1].Hi;
    int v1 = j->Vmax / j->Components[1].Vi;
    int h2 = j->Hmax / j->Components[2].Hi;
    int v2 = j->Vmax / j->Components[2].Vi;

    int x, y;
    int max_y = info->height * ds;
    int max_x = info->width * ds;
    for (y = 0; y < max_y; y += ds)
    {
        TCOEF *C0 =
                j->Components[0].du[j->Components[0].du_width * ((y / v0) / 8)] + 8 * ((y / v0) & 7);
        TCOEF *C1 =
                j->Components[1].du[j->Components[1].du_width * ((y / v1) / 8)] + 8 * ((y / v1) & 7);
        TCOEF *C2 =
                j->Components[2].du[j->Components[2].du_width * ((y / v2) / 8)] + 8 * ((y / v2) & 7);
        for (x = 0; x < max_x; x += ds)
        {
            int c0, c1, c2;
            if (ds == 1)
            {
                c0 = C0[(x / h0 / 8) * 64 + ((x / h0) & 7)];
                c1 = C1[(x / h1 / 8) * 64 + ((x / h1) & 7)];
                c2 = C2[(x / h2 / 8) * 64 + ((x / h2) & 7)];
            }
            else
            {
                /* Box filter downsampling (area averaging):
                   Average a ds*ds block of Y/U/V samples per output pixel
                   for simple anti-aliasing without extra buffers */
                int sumY = 0, sumU = 0, sumV = 0;
                int dx, dy;
                int area = ds * ds;
                for (dy = 0; dy < ds; dy++)
                {
                    TCOEF *C0_dy = j->Components[0].du[j->Components[0].du_width * (((y + dy) / v0) / 8)] + 8 * (((y + dy) / v0) & 7);
                    TCOEF *C1_dy = j->Components[1].du[j->Components[1].du_width * (((y + dy) / v1) / 8)] + 8 * (((y + dy) / v1) & 7);
                    TCOEF *C2_dy = j->Components[2].du[j->Components[2].du_width * (((y + dy) / v2) / 8)] + 8 * (((y + dy) / v2) & 7);
                    for (dx = 0; dx < ds; dx++)
                    {
                        int sx = x + dx;
                        sumY += C0_dy[((sx / h0 / 8) * 64) + ((sx / h0) & 7)];
                        sumU += C1_dy[((sx / h1 / 8) * 64) + ((sx / h1) & 7)];
                        sumV += C2_dy[((sx / h2 / 8) * 64) + ((sx / h2) & 7)];
                    }
                }
                c0 = (sumY + area/2) / area;
                c1 = (sumU + area/2) / area;
                c2 = (sumV + area/2) / area;
            }

            /* ITU BT.601 full-range YUV-to-RGB integer approximation */
            {
                int yy = (c0 << 5) + 16;
                int u = c1 - 128;
                int v = c2 - 128;

                int b = CLIP[(yy + 57 * u)>>5];         /* B */
                int g = CLIP[(yy - 11 * u - 23 * v)>>5];/* G */
                int r = CLIP[(yy + 45 * v)>>5];         /* R */
                *bmp++= FB_RGBPACK(r,g,b);
            }
        }
    }
    return 0;
}

const struct image_decoder jpegp_decoder = {
    false,
    img_mem,
    load_image,
    get_image,
    draw_image_rect,
};
