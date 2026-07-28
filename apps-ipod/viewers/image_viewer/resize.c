/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/lib/bmp_smooth_scale.c
 * Bilinear image scaler (Imlib2-derived), used for zooming BMP images.
 * Ported verbatim from apps/plugins/lib/bmp_smooth_scale.c -- it uses no
 * plugin API, only the core FB_UNPACK / FB_RGBPACK pixel macros, so the body
 * is unchanged.
 *
 * Code for the scaling algorithm:
 * Imlib2 is (C) Carsten Haitzler and various contributors. The MMX code
 * is by Willem Monsuwe <willem@stack.nl>. Additional modifications are by
 * (C) Daniel M. Duley.
 *
 * Port to Rockbox
 * Copyright (C) 2007 Jonas Hurrelmann (j@outpo.st)
 * GNU General Public License (version 2+)
 *
 * Smooth (area-average) bitmap scaler used when fitting a decoded image to
 * the screen. Separate from draw/resize.c, which serves album art.
 ****************************************************************************/

#include "lcd.h"
#include "image_viewer.h"

void smooth_resize_bitmap(struct bitmap *src_bmp,  struct bitmap *dest_bmp)
{
    fb_data *sptr, *dptr;
    int x, y, end;
    int val_y = 0, val_x;
    const int sw = src_bmp->width;
    const int sh = src_bmp->height;
    const int dw = dest_bmp->width;
    const int dh = dest_bmp->height;
    const int inc_x = (sw << 16) / dw;
    const int inc_y = (sh << 16) / dh;
    const int Cp_x = ((dw << 14) / sw) + 1;
    const int Cp_y = ((dh << 14) / sh) + 1;
    const int xup_yup = (dw >= sw) + ((dh >= sh) << 1);
    const int dow = dw;
    const int sow = sw;
    fb_data *src = (fb_data*)src_bmp->data;
    fb_data *dest = (fb_data*)dest_bmp->data;
    int XAP, YAP, INV_YAP, INV_XAP;
    int xpoint;
    fb_data *ypoint;

    end = dw;
    /* scaling up both ways */
    if (xup_yup == 3) {
        /* go through every scanline in the output buffer */
        for (y = 0; y < dh; y++) {
            /* calculate the source line we'll scan from */
            ypoint = src + ((val_y >> 16) * sw);
            YAP = ((val_y >> 16) >= (sh - 1)) ? 0 : (val_y >> 8) - ((val_y >> 8) & 0xffffff00);
            INV_YAP = 256 - YAP;

            val_y += inc_y;
            val_x = 0;

            dptr = dest + (y * dow);
            sptr = ypoint;
            if (YAP > 0) {
                for (x = 0; x < end; x++) {
                    int r = 0, g = 0, b = 0;
                    int rr = 0, gg = 0, bb = 0;
                    fb_data *pix;

                    xpoint = (val_x >> 16);
                    XAP = ((val_x >> 16) >= (sw - 1)) ? 0 : (val_x >> 8) - ((val_x >> 8) & 0xffffff00);
                    INV_XAP = 256 - XAP;
                    val_x += inc_x;

                    if (XAP > 0) {
                        pix = ypoint + xpoint;
                        r = FB_UNPACK_RED(*pix) * INV_XAP;
                        g = FB_UNPACK_GREEN(*pix) * INV_XAP;
                        b = FB_UNPACK_BLUE(*pix) * INV_XAP;
                        pix++;
                        r += FB_UNPACK_RED(*pix) * XAP;
                        g += FB_UNPACK_GREEN(*pix) * XAP;
                        b += FB_UNPACK_BLUE(*pix) * XAP;
                        pix += sow;
                        rr = FB_UNPACK_RED(*pix) * XAP;
                        gg = FB_UNPACK_GREEN(*pix) * XAP;
                        bb = FB_UNPACK_BLUE(*pix) * XAP;
                        pix--;
                        rr += FB_UNPACK_RED(*pix) * INV_XAP;
                        gg += FB_UNPACK_GREEN(*pix) * INV_XAP;
                        bb += FB_UNPACK_BLUE(*pix) * INV_XAP;
                        r = ((rr * YAP) + (r * INV_YAP)) >> 16;
                        g = ((gg * YAP) + (g * INV_YAP)) >> 16;
                        b = ((bb * YAP) + (b * INV_YAP)) >> 16;
                        *dptr++ = FB_RGBPACK(r, g, b);
                    } else {
                        pix = ypoint + xpoint;
                        r = FB_UNPACK_RED(*pix) * INV_YAP;
                        g = FB_UNPACK_GREEN(*pix) * INV_YAP;
                        b = FB_UNPACK_BLUE(*pix) * INV_YAP;
                        pix += sow;
                        r += FB_UNPACK_RED(*pix) * YAP;
                        g += FB_UNPACK_GREEN(*pix) * YAP;
                        b += FB_UNPACK_BLUE(*pix) * YAP;
                        r >>= 8;
                        g >>= 8;
                        b >>= 8;
                        *dptr++ = FB_RGBPACK(r, g, b);
                    }
                }
            } else {
                for (x = 0; x < end; x++) {
                    int r = 0, g = 0, b = 0;
                    fb_data *pix;

                    xpoint = (val_x >> 16);
                    XAP = ((val_x >> 16) >= (sw - 1)) ? 0 : (val_x >> 8) - ((val_x >> 8) & 0xffffff00);
                    INV_XAP = 256 - XAP;
                    val_x += inc_x;

                    if (XAP > 0) {
                        pix = ypoint + xpoint;
                        r = FB_UNPACK_RED(*pix) * INV_XAP;
                        g = FB_UNPACK_GREEN(*pix) * INV_XAP;
                        b = FB_UNPACK_BLUE(*pix) * INV_XAP;
                        pix++;
                        r += FB_UNPACK_RED(*pix) * XAP;
                        g += FB_UNPACK_GREEN(*pix) * XAP;
                        b += FB_UNPACK_BLUE(*pix) * XAP;
                        r >>= 8;
                        g >>= 8;
                        b >>= 8;
                        *dptr++ = FB_RGBPACK(r, g, b);
                    } else
                        *dptr++ = sptr[xpoint];
                }
            }
        }
    }
    /* if we're scaling down vertically */
    else if (xup_yup == 1) {
        /*\ 'Correct' version, with math units prepared for MMXification \ */
        int Cy, j;
        fb_data *pix;
        int r, g, b, rr, gg, bb;
        int yap;

        /* go through every scanline in the output buffer */
        for (y = 0; y < dh; y++) {
            ypoint = src + ((val_y >> 16) * sw);
            YAP = (((0x100 - ((val_y >> 8) & 0xff)) * Cp_y) >> 8) | (Cp_y << 16);
            INV_YAP = 256 - YAP;
            val_y += inc_y;
            val_x = 0;

            Cy = YAP >> 16;
            yap = YAP & 0xffff;


            dptr = dest + (y * dow);
            for (x = 0; x < end; x++) {
                xpoint = (val_x >> 16);
                XAP = ((val_x >> 16) >= (sw - 1)) ? 0 : (val_x >> 8) - ((val_x >> 8) & 0xffffff00);
                INV_XAP = 256 - XAP;
                val_x += inc_x;

                pix = ypoint + xpoint;
                r = (FB_UNPACK_RED(*pix) * yap) >> 10;
                g = (FB_UNPACK_GREEN(*pix) * yap) >> 10;
                b = (FB_UNPACK_BLUE(*pix) * yap) >> 10;
                pix += sow;
                for (j = (1 << 14) - yap; j > Cy; j -= Cy) {
                    r += (FB_UNPACK_RED(*pix) * Cy) >> 10;
                    g += (FB_UNPACK_GREEN(*pix) * Cy) >> 10;
                    b += (FB_UNPACK_BLUE(*pix) * Cy) >> 10;
                    pix += sow;
                }
                if (j > 0) {
                    r += (FB_UNPACK_RED(*pix) * j) >> 10;
                    g += (FB_UNPACK_GREEN(*pix) * j) >> 10;
                    b += (FB_UNPACK_BLUE(*pix) * j) >> 10;
                }
                if (XAP > 0) {
                    pix = ypoint + xpoint + 1;
                    rr = (FB_UNPACK_RED(*pix) * yap) >> 10;
                    gg = (FB_UNPACK_GREEN(*pix) * yap) >> 10;
                    bb = (FB_UNPACK_BLUE(*pix) * yap) >> 10;
                    pix += sow;
                    for (j = (1 << 14) - yap; j > Cy; j -= Cy) {
                        rr += (FB_UNPACK_RED(*pix) * Cy) >> 10;
                        gg += (FB_UNPACK_GREEN(*pix) * Cy) >> 10;
                        bb += (FB_UNPACK_BLUE(*pix) * Cy) >> 10;
                        pix += sow;
                    }
                    if (j > 0) {
                        rr += (FB_UNPACK_RED(*pix) * j) >> 10;
                        gg += (FB_UNPACK_GREEN(*pix) * j) >> 10;
                        bb += (FB_UNPACK_BLUE(*pix) * j) >> 10;
                    }
                    r = r * INV_XAP;
                    g = g * INV_XAP;
                    b = b * INV_XAP;
                    r = (r + ((rr * XAP))) >> 12;
                    g = (g + ((gg * XAP))) >> 12;
                    b = (b + ((bb * XAP))) >> 12;
                } else {
                    r >>= 4;
                    g >>= 4;
                    b >>= 4;
                }
                *dptr = FB_RGBPACK(r, g, b);
                dptr++;
            }
        }
    }
    /* if we're scaling down horizontally */
    else if (xup_yup == 2) {
        /*\ 'Correct' version, with math units prepared for MMXification \ */
        int Cx, j;
        fb_data *pix;
        int r, g, b, rr, gg, bb;
        int xap;

        /* go through every scanline in the output buffer */
        for (y = 0; y < dh; y++) {
            ypoint = src + ((val_y >> 16) * sw);
            YAP = ((val_y >> 16) >= (sh - 1)) ? 0 : (val_y >> 8) - ((val_y >> 8) & 0xffffff00);
            INV_YAP = 256 - YAP;
            val_y += inc_y;
            val_x = 0;

            dptr = dest + (y * dow);
            for (x = 0; x < end; x++) {
                xpoint = (val_x >> 16);
                XAP = (((0x100 - ((val_x >> 8) & 0xff)) * Cp_x) >> 8) | (Cp_x << 16);
                INV_XAP = 256 - XAP;

                val_x += inc_x;

                Cx = XAP >> 16;
                xap = XAP & 0xffff;

                pix = ypoint + xpoint;
                r = (FB_UNPACK_RED(*pix) * xap) >> 10;
                g = (FB_UNPACK_GREEN(*pix) * xap) >> 10;
                b = (FB_UNPACK_BLUE(*pix) * xap) >> 10;
                pix++;
                for (j = (1 << 14) - xap; j > Cx; j -= Cx) {
                    r += (FB_UNPACK_RED(*pix) * Cx) >> 10;
                    g += (FB_UNPACK_GREEN(*pix) * Cx) >> 10;
                    b += (FB_UNPACK_BLUE(*pix) * Cx) >> 10;
                    pix++;
                }
                if (j > 0) {
                    r += (FB_UNPACK_RED(*pix) * j) >> 10;
                    g += (FB_UNPACK_GREEN(*pix) * j) >> 10;
                    b += (FB_UNPACK_BLUE(*pix) * j) >> 10;
                }
                if (YAP > 0) {
                    pix = ypoint + xpoint + sow;
                    rr = (FB_UNPACK_RED(*pix) * xap) >> 10;
                    gg = (FB_UNPACK_GREEN(*pix) * xap) >> 10;
                    bb = (FB_UNPACK_BLUE(*pix) * xap) >> 10;
                    pix++;
                    for (j = (1 << 14) - xap; j > Cx; j -= Cx) {
                        rr += (FB_UNPACK_RED(*pix) * Cx) >> 10;
                        gg += (FB_UNPACK_GREEN(*pix) * Cx) >> 10;
                        bb += (FB_UNPACK_BLUE(*pix) * Cx) >> 10;
                        pix++;
                    }
                    if (j > 0) {
                        rr += (FB_UNPACK_RED(*pix) * j) >> 10;
                        gg += (FB_UNPACK_GREEN(*pix) * j) >> 10;
                        bb += (FB_UNPACK_BLUE(*pix) * j) >> 10;
                    }
                    r = r * INV_YAP;
                    g = g * INV_YAP;
                    b = b * INV_YAP;
                    r = (r + ((rr * YAP))) >> 12;
                    g = (g + ((gg * YAP))) >> 12;
                    b = (b + ((bb * YAP))) >> 12;
                } else {
                    r >>= 4;
                    g >>= 4;
                    b >>= 4;
                }
                *dptr = FB_RGBPACK(r, g, b);
                dptr++;
            }
        }
    }
    /* fully optimized (i think) - only change of algorithm can help */
    /* if we're scaling down horizontally & vertically */
    else {
        /*\ 'Correct' version, with math units prepared for MMXification \ */
        int Cx, Cy, i, j;
        fb_data *pix;
        int r, g, b, rx, gx, bx;
        int xap, yap;

        for (y = 0; y < dh; y++) {
            ypoint = src + ((val_y >> 16) * sw);
            YAP = (((0x100 - ((val_y >> 8) & 0xff)) * Cp_y) >> 8) | (Cp_y << 16);
            INV_YAP = 256 - YAP;
            val_y += inc_y;
            val_x = 0;

            Cy = YAP >> 16;
            yap = YAP & 0xffff;

            dptr = dest + (y * dow);
            for (x = 0; x < end; x++) {
                xpoint = (val_x >> 16);
                XAP = (((0x100 - ((val_x >> 8) & 0xff)) * Cp_x) >> 8) | (Cp_x << 16);
                INV_XAP = 256 - XAP;
                val_x += inc_x;

                Cx = XAP >> 16;
                xap = XAP & 0xffff;

                sptr = ypoint + xpoint;

                pix = sptr;
                sptr += sow;
                rx = (FB_UNPACK_RED(*pix) * xap) >> 9;
                gx = (FB_UNPACK_GREEN(*pix) * xap) >> 9;
                bx = (FB_UNPACK_BLUE(*pix) * xap) >> 9;
                pix++;
                for (i = (1 << 14) - xap; i > Cx; i -= Cx) {
                    rx += (FB_UNPACK_RED(*pix) * Cx) >> 9;
                    gx += (FB_UNPACK_GREEN(*pix) * Cx) >> 9;
                    bx += (FB_UNPACK_BLUE(*pix) * Cx) >> 9;
                    pix++;
                }
                if (i > 0) {
                    rx += (FB_UNPACK_RED(*pix) * i) >> 9;
                    gx += (FB_UNPACK_GREEN(*pix) * i) >> 9;
                    bx += (FB_UNPACK_BLUE(*pix) * i) >> 9;
                }

                r = (rx * yap) >> 14;
                g = (gx * yap) >> 14;
                b = (bx * yap) >> 14;

                for (j = (1 << 14) - yap; j > Cy; j -= Cy) {
                    pix = sptr;
                    sptr += sow;
                    rx = (FB_UNPACK_RED(*pix) * xap) >> 9;
                    gx = (FB_UNPACK_GREEN(*pix) * xap) >> 9;
                    bx = (FB_UNPACK_BLUE(*pix) * xap) >> 9;
                    pix++;
                    for (i = (1 << 14) - xap; i > Cx; i -= Cx) {
                        rx += (FB_UNPACK_RED(*pix) * Cx) >> 9;
                        gx += (FB_UNPACK_GREEN(*pix) * Cx) >> 9;
                        bx += (FB_UNPACK_BLUE(*pix) * Cx) >> 9;
                        pix++;
                    }
                    if (i > 0) {
                        rx += (FB_UNPACK_RED(*pix) * i) >> 9;
                        gx += (FB_UNPACK_GREEN(*pix) * i) >> 9;
                        bx += (FB_UNPACK_BLUE(*pix) * i) >> 9;
                    }

                    r += (rx * Cy) >> 14;
                    g += (gx * Cy) >> 14;
                    b += (bx * Cy) >> 14;
                }
                if (j > 0) {
                    pix = sptr;
                    sptr += sow;
                    rx = (FB_UNPACK_RED(*pix) * xap) >> 9;
                    gx = (FB_UNPACK_GREEN(*pix) * xap) >> 9;
                    bx = (FB_UNPACK_BLUE(*pix) * xap) >> 9;
                    pix++;
                    for (i = (1 << 14) - xap; i > Cx; i -= Cx) {
                        rx += (FB_UNPACK_RED(*pix) * Cx) >> 9;
                        gx += (FB_UNPACK_GREEN(*pix) * Cx) >> 9;
                        bx += (FB_UNPACK_BLUE(*pix) * Cx) >> 9;
                        pix++;
                    }
                    if (i > 0) {
                        rx += (FB_UNPACK_RED(*pix) * i) >> 9;
                        gx += (FB_UNPACK_GREEN(*pix) * i) >> 9;
                        bx += (FB_UNPACK_BLUE(*pix) * i) >> 9;
                    }

                    r += (rx * j) >> 14;
                    g += (gx * j) >> 14;
                    b += (bx * j) >> 14;
                }

                *dptr = FB_RGBPACK(r >> 5, g >> 5, b >> 5);
                dptr++;
            }
        }
    }
}
