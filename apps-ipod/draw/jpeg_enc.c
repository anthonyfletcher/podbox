/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* A baseline JPEG encoder, small rather than fast: one 16x16 macroblock at a
 * time -- four luminance blocks and one of each chrominance -- through a
 * fixed-point DCT, quantisation and the standard Huffman tables. No floating
 * point and no division per coefficient.
 *
 * Parts: the tables; the bit writer; the DCT and block coder; the headers;
 * jpeg_encode(). */

#include <string.h>
#include "jpeg_common.h"
#include "jpeg_enc.h"

/* ---- tables ------------------------------------------------------------- */

/* Zigzag position -> row-major position in a block */
static const uint8_t zigzag[64] =
{
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* The standard's example quantisation tables (Annex K), row-major */
static const uint8_t std_quant[2][64] =
{
    {
        16, 11, 10, 16,  24,  40,  51,  61,
        12, 12, 14, 19,  26,  58,  60,  55,
        14, 13, 16, 24,  40,  57,  69,  56,
        14, 17, 22, 29,  51,  87,  80,  62,
        18, 22, 37, 56,  68, 109, 103,  77,
        24, 35, 55, 64,  81, 104, 113,  92,
        49, 64, 78, 87, 103, 121, 120, 101,
        72, 92, 95, 98, 112, 100, 103,  99,
    },
    {
        17, 18, 24, 47, 99, 99, 99, 99,
        18, 21, 26, 66, 99, 99, 99, 99,
        24, 26, 56, 99, 99, 99, 99, 99,
        47, 66, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99,
    },
};

/* C(u)/2 * cos((2x+1)u*pi/16) in 2.14 fixed point, x = 0..3; x = 4..7 mirror
 * them, negated for odd u */
static const int16_t dct_cos[8][4] =
{
    {  5793,  5793,  5793,  5793 },
    {  8035,  6811,  4551,  1598 },
    {  7568,  3135, -3135, -7568 },
    {  6811, -1598, -8035, -4551 },
    {  5793, -5793, -5793,  5793 },
    {  4551, -8035,  1598,  6811 },
    {  3135, -7568,  7568, -3135 },
    {  1598, -4551,  6811, -8035 },
};

struct huff
{
    uint16_t code[256];
    uint8_t len[256];
};

/* DC and AC, luminance then chrominance */
static struct huff tables[2][2];
static bool huff_built;

/* Canonical codes from a JFIF table: 16 counts by length, then the values */
static void huff_build(struct huff *h, const int *table)
{
    const int *val = table + 16;
    unsigned code = 0;
    for (int len = 1; len <= 16; len++)
    {
        for (int i = 0; i < table[len - 1]; i++)
        {
            h->code[*val] = code++;
            h->len[*val++] = len;
        }
        code <<= 1;
    }
}

/* ---- bit writer --------------------------------------------------------- */

static struct
{
    uint8_t *p, *end;
    uint32_t acc;       /* the low n bits are pending */
    int n;
    bool full;
} bw;

static void put_byte(int c)
{
    if (bw.p < bw.end)
        *bw.p++ = c;
    else
        bw.full = true;
}

static void put_bits(uint32_t bits, int len)
{
    bw.acc = bw.acc << len | (bits & ((1u << len) - 1));
    bw.n += len;
    while (bw.n >= 8)
    {
        bw.n -= 8;
        const int c = (bw.acc >> bw.n) & 0xff;
        put_byte(c);
        if (c == 0xff)
            put_byte(0);    /* stuffed, so data never reads as a marker */
    }
}

/* ---- DCT and block coder ------------------------------------------------ */

/* blk holds samples less 128, row-major; out the coefficients, row-major,
 * scaled as the standard's DCT. Each pass folds the row in half first:
 * even frequencies need only the sums of mirrored samples, odd ones only
 * the differences. */
static void fdct(const int16_t *blk, int32_t *out)
{
    int32_t tmp[64];    /* [row][u], 3 bits more than the samples */

    for (int y = 0; y < 8; y++)
    {
        const int16_t *f = blk + y * 8;
        int32_t s[4], d[4];
        for (int x = 0; x < 4; x++)
        {
            s[x] = f[x] + f[7 - x];
            d[x] = f[x] - f[7 - x];
        }
        for (int u = 0; u < 8; u++)
        {
            const int32_t *a = (u & 1) ? d : s;
            const int16_t *c = dct_cos[u];
            int32_t sum = a[0] * c[0] + a[1] * c[1] + a[2] * c[2] + a[3] * c[3];
            tmp[y * 8 + u] = (sum + (1 << 10)) >> 11;
        }
    }
    for (int u = 0; u < 8; u++)
    {
        int32_t s[4], d[4];
        for (int y = 0; y < 4; y++)
        {
            s[y] = tmp[y * 8 + u] + tmp[(7 - y) * 8 + u];
            d[y] = tmp[y * 8 + u] - tmp[(7 - y) * 8 + u];
        }
        for (int v = 0; v < 8; v++)
        {
            const int32_t *a = (v & 1) ? d : s;
            const int16_t *c = dct_cos[v];
            int32_t sum = a[0] * c[0] + a[1] * c[1] + a[2] * c[2] + a[3] * c[3];
            out[v * 8 + u] = (sum + (1 << 16)) >> 17;
        }
    }
}

/* Bits needed for |v|, the coefficient's category */
static int category(int v)
{
    unsigned a = v < 0 ? -v : v;
    int n = 0;
    while (a)
    {
        n++;
        a >>= 1;
    }
    return n;
}

/* A value as its category's code then its bits: a negative one is sent as
 * its ones' complement */
static void put_value(const struct huff *h, int sym, int v, int cat)
{
    put_bits(h->code[sym], h->len[sym]);
    if (cat)
        put_bits(v < 0 ? v - 1 : v, cat);
}

/* quant: the table divisors, row-major; recip 65536 over each, which for a
 * divisor of 1 needs 17 bits */
static void code_block(const int16_t *blk, const uint8_t *quant,
                       const uint32_t *recip, int *dc_prev,
                       const struct huff *dc, const struct huff *ac)
{
    int32_t coef[64];
    int zz[64];

    fdct(blk, coef);
    for (int i = 0; i < 64; i++)
    {
        const int k = zigzag[i];
        const int32_t c = coef[k];
        const int32_t q = ((c < 0 ? -c : c) + quant[k] / 2) * recip[k] >> 16;
        zz[i] = c < 0 ? -q : q;
    }

    const int diff = zz[0] - *dc_prev;
    *dc_prev = zz[0];
    const int dcat = category(diff);
    put_value(dc, dcat, diff, dcat);

    int run = 0;
    for (int i = 1; i < 64; i++)
    {
        if (!zz[i])
        {
            run++;
            continue;
        }
        while (run > 15)
        {
            put_bits(ac->code[0xf0], ac->len[0xf0]);   /* sixteen zeros */
            run -= 16;
        }
        const int cat = category(zz[i]);
        put_value(ac, run << 4 | cat, zz[i], cat);
        run = 0;
    }
    if (run)
        put_bits(ac->code[0x00], ac->len[0x00]);       /* end of block */
}

/* ---- headers ------------------------------------------------------------ */

static void put_word(int w)
{
    put_byte(w >> 8);
    put_byte(w & 0xff);
}

/* One table a segment, as libjpeg and an iPhone write them, which is what
 * a car's decoder is made against */
static void put_huff_table(int class_id, const int *table, int nvals)
{
    put_word(0xffc4);
    put_word(2 + 1 + 16 + nvals);
    put_byte(class_id);
    for (int i = 0; i < 16 + nvals; i++)
        put_byte(table[i]);
}

static void put_headers(int width, int height, const uint8_t quant[2][64])
{
    static const uint8_t jfif[] =
    {
        0xff, 0xd8,                                 /* start of image */
        0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00,
        0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
    };
    for (size_t i = 0; i < sizeof(jfif); i++)
        put_byte(jfif[i]);

    for (int t = 0; t < 2; t++)                     /* quantisation */
    {
        put_word(0xffdb);
        put_word(2 + 65);
        put_byte(t);
        for (int i = 0; i < 64; i++)
            put_byte(quant[t][zigzag[i]]);
    }

    put_word(0xffc0);                               /* baseline frame */
    put_word(17);
    put_byte(8);
    put_word(height);
    put_word(width);
    put_byte(3);
    put_byte(1); put_byte(0x22); put_byte(0);       /* Y, 2x2, table 0 */
    put_byte(2); put_byte(0x11); put_byte(1);       /* Cb */
    put_byte(3); put_byte(0x11); put_byte(1);       /* Cr */

    put_huff_table(0x00, jpeg_std_luma.huffmancodes_dc, DC_LEN - 16);
    put_huff_table(0x10, jpeg_std_luma.huffmancodes_ac, AC_LEN - 16);
    put_huff_table(0x01, jpeg_std_chroma.huffmancodes_dc, DC_LEN - 16);
    put_huff_table(0x11, jpeg_std_chroma.huffmancodes_ac, AC_LEN - 16);

    put_word(0xffda);                               /* start of scan */
    put_word(12);
    put_byte(3);
    put_byte(1); put_byte(0x00);
    put_byte(2); put_byte(0x11);
    put_byte(3); put_byte(0x11);
    put_byte(0); put_byte(63); put_byte(0);
}

/* ---- jpeg_encode() ------------------------------------------------------ */

int jpeg_encode(const struct jpeg_enc_src *src, int quality,
                uint8_t *out, size_t size)
{
    const int w = src->width, h = src->height;
    uint8_t quant[2][64];
    uint32_t recip[2][64];
    int dc_prev[3] = { 0, 0, 0 };
    int16_t y_blk[4][64], cb_blk[64], cr_blk[64];
    int32_t cb_sum[64], cr_sum[64];

    if (w <= 0 || h <= 0 || w > 0xffff || h > 0xffff)
        return -1;
    if (!huff_built)
    {
        huff_build(&tables[0][0], jpeg_std_luma.huffmancodes_dc);
        huff_build(&tables[0][1], jpeg_std_luma.huffmancodes_ac);
        huff_build(&tables[1][0], jpeg_std_chroma.huffmancodes_dc);
        huff_build(&tables[1][1], jpeg_std_chroma.huffmancodes_ac);
        huff_built = true;
    }

    /* The standard's scaling of its tables to a quality */
    quality = quality < 1 ? 1 : quality > 100 ? 100 : quality;
    const int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < 64; i++)
        {
            int q = (std_quant[t][i] * scale + 50) / 100;
            q = q < 1 ? 1 : q > 255 ? 255 : q;
            quant[t][i] = q;
            recip[t][i] = (65536 + q - 1) / q;
        }

    bw.p = out;
    bw.end = out + size;
    bw.acc = 0;
    bw.n = 0;
    bw.full = false;
    put_headers(w, h, quant);

    for (int y0 = 0; y0 < h; y0 += 16)
    {
        const int rows = h - y0 < 16 ? h - y0 : 16;
        if (!src->read(src->ctx, src->band, y0, rows))
            return -1;

        for (int x0 = 0; x0 < w; x0 += 16)
        {
            memset(cb_sum, 0, sizeof(cb_sum));
            memset(cr_sum, 0, sizeof(cr_sum));
            for (int j = 0; j < 16; j++)
            {
                /* Past the edge, the last row and column repeat */
                const fb_data *row = src->band + (j < rows ? j : rows - 1) * w;
                for (int i = 0; i < 16; i++)
                {
                    const fb_data p = row[x0 + i < w ? x0 + i : w - 1];
                    const int r = RGB_UNPACK_RED(p);
                    const int g = RGB_UNPACK_GREEN(p);
                    const int b = RGB_UNPACK_BLUE(p);
                    const int y = (19595 * r + 38470 * g + 7471 * b + 32768)
                                  >> 16;
                    y_blk[(j >> 3) * 2 + (i >> 3)][(j & 7) * 8 + (i & 7)] =
                        y - 128;
                    const int c = (j >> 1) * 8 + (i >> 1);
                    cb_sum[c] += -11059 * r - 21709 * g + 32768 * b;
                    cr_sum[c] += 32768 * r - 27439 * g - 5329 * b;
                }
            }
            for (int i = 0; i < 64; i++)
            {
                /* four pixels' worth, 16 fraction bits */
                cb_blk[i] = (cb_sum[i] + (1 << 17)) >> 18;
                cr_blk[i] = (cr_sum[i] + (1 << 17)) >> 18;
            }

            for (int k = 0; k < 4; k++)
                code_block(y_blk[k], quant[0], recip[0], &dc_prev[0],
                           &tables[0][0], &tables[0][1]);
            code_block(cb_blk, quant[1], recip[1], &dc_prev[1],
                       &tables[1][0], &tables[1][1]);
            code_block(cr_blk, quant[1], recip[1], &dc_prev[2],
                       &tables[1][0], &tables[1][1]);
            if (bw.full)
                return -1;
        }
    }

    if (bw.n)
        put_bits(0x7f, 8 - bw.n);   /* the last byte padded with ones */
    put_word(0xffd9);               /* end of image */
    return bw.full ? -1 : bw.p - out;
}
