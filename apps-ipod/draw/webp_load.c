/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Reads a lossy WebP (VP8, RFC 6386) into a rockbox bitmap one row at a
 * time, scaling on load through resize.c, for album art, covers and book
 * pictures. A lossless one (VP8L) is refused, and an alpha channel is left
 * out: a transparent pixel shows the colour stored beneath it.
 *
 *   container      - RIFF chunks to the first VP8 data, inside an animated
 *                    file's first frame
 *   reading        - each partition through its own buffer on the file, and
 *                    the boolean decoder over it
 *   headers        - segments, loop filter, quantisers, token probabilities
 *   macroblocks    - the intra modes, then the coefficient tokens
 *   reconstruction - prediction and the inverse transforms in a work block
 *                    that holds the unfiltered edges prediction reads
 *   filter         - the loop filter over a row once all of it is built
 *   rows           - a row of macroblocks at a time, the rows its filter
 *                    can still change held back for the next; each row
 *                    handed out as RGB, its chroma taken from the nearest
 *                    sample
 *
 * Nothing is scaled before it is built, so memory grows with the image's
 * width: 36 bytes a pixel of it, taken from the room after the bitmap. The
 * arithmetic, down to the rounding, is libwebp's, so a decode matches
 * dwebp -nofancy.
 ****************************************************************************/

#include <stddef.h>
#include <string.h>
#include "kernel.h"
#include "system.h"
#include "file.h"
#include "bmp.h"
#include "resize.h"
#include "webp_load.h"
#include "vp8_tables.h"

/* The work block: a 16x16 luma block and two 8x8 chroma ones, each with the
 * row above and four columns to its left, 32 bytes to a row */
#define BPS      32
#define YUV_SIZE (BPS * 17 + BPS * 9)
#define Y_OFF    (BPS * 1 + 8)
#define U_OFF    (Y_OFF + BPS * 16 + BPS)
#define V_OFF    (U_OFF + 16)

#define MODES_BUF 1024      /* the first partition's reads */
#define TOKEN_BUF 4096      /* the token partitions', shared between them */

enum {
    B_DC_PRED, B_TM_PRED, B_VE_PRED, B_HE_PRED, B_RD_PRED,
    B_VR_PRED, B_LD_PRED, B_VL_PRED, B_HD_PRED, B_HU_PRED,
    /* DC without the edges a frame's first row or column lacks */
    B_DC_PRED_NOTOP, B_DC_PRED_NOLEFT, B_DC_PRED_NOTOPLEFT
};
#define DC_PRED B_DC_PRED
#define TM_PRED B_TM_PRED
#define V_PRED  B_VE_PRED
#define H_PRED  B_HE_PRED

/* ---- reading --------------------------------------------------------- */

/* One partition: a byte range of the file, read through its own buffer */
struct part {
    int fd;
    off_t pos, end;
    uint8_t *buf;
    int size, at, len;
};

static int part_byte(struct part *p)
{
    if (p->at == p->len)
    {
        off_t n = p->end - p->pos;

        if (n > p->size)
            n = p->size;
        if (n <= 0 || lseek(p->fd, p->pos, SEEK_SET) != p->pos)
            return -1;
        n = read(p->fd, p->buf, n);
        if (n <= 0)
            return -1;
        p->pos += n;
        p->at = 0;
        p->len = n;
    }
    return p->buf[p->at++];
}

/* The boolean decoder. value holds bits + 8 bits not yet decoded; range is
 * one less than the interval, 127 to 254 between bits. */
struct br {
    uint32_t value;
    int bits;
    uint32_t range;
    bool eof;
    struct part *src;
};

static void br_load(struct br *b)
{
    struct part *p = b->src;
    int c;

    if (p->len - p->at >= 3)
    {
        const uint8_t *s = p->buf + p->at;

        b->value = (b->value << 24) | (s[0] << 16) | (s[1] << 8) | s[2];
        p->at += 3;
        b->bits += 24;
        return;
    }
    c = part_byte(p);
    if (c >= 0)
    {
        b->value = (b->value << 8) | c;
        b->bits += 8;
    }
    else if (!b->eof)
    {   /* past the end the stream reads as zeros */
        b->value <<= 8;
        b->bits += 8;
        b->eof = true;
    }
    else
        b->bits = 0;
}

static void br_init(struct br *b, struct part *p)
{
    b->range = 254;
    b->value = 0;
    b->bits = -8;
    b->eof = false;
    b->src = p;
    br_load(b);
}

/* Inlined where tokens are read; a call from everywhere else */
static FORCE_INLINE int br_bit_in(struct br *b, int prob)
{
    uint32_t range = b->range, split, value;
    int bit;

    if (b->bits < 0)
        br_load(b);
    split = (range * prob) >> 8;
    value = b->value >> b->bits;
    if (value > split)
    {
        range -= split + 1;
        b->value -= (split + 1) << b->bits;
        bit = 1;
    }
    else
    {
        range = split;
        bit = 0;
    }
    if (range <= 0x7e)
    {
        b->bits -= vp8_range_shift[range];
        range = vp8_range_new[range];
    }
    b->range = range;
    return bit;
}

static int br_bit(struct br *b, int prob)
{
    return br_bit_in(b, prob);
}

/* v with a sign read at probability one half */
static FORCE_INLINE int br_signed(struct br *b, int v)
{
    uint32_t split, value;
    int32_t mask;
    int pos;

    if (b->bits < 0)
        br_load(b);
    pos = b->bits;
    split = b->range >> 1;
    value = b->value >> pos;
    mask = (int32_t)(split - value) >> 31;
    b->bits -= 1;
    b->range += (uint32_t)mask;
    b->range |= 1;
    b->value -= ((split + 1) & (uint32_t)mask) << pos;
    return (v ^ mask) - mask;
}

static uint32_t br_value(struct br *b, int n)
{
    uint32_t v = 0;

    while (n-- > 0)
        v |= (uint32_t)br_bit(b, 0x80) << n;
    return v;
}

static int br_svalue(struct br *b, int n)
{
    int v = br_value(b, n);

    return br_bit(b, 0x80) ? -v : v;
}

/* ---- the decoder ----------------------------------------------------- */

typedef uint8_t vp8_band[3][11];

struct finfo {
    uint8_t limit;          /* 0: not filtered */
    uint8_t ilevel, inner, hev;
};

struct nzctx {
    uint8_t nz;             /* luma columns' 4 bits, then 2 of U, 2 of V */
    uint8_t nz_dc;
};

struct top_samples {
    uint8_t y[16], u[8], v[8];
};

struct vp8 {
    int width, height, mb_w, mb_h;
    struct part parts[9];
    struct br br, tok[8];
    int ntok;

    bool update_map;
    uint8_t seg_proba[3];
    int filter_type;        /* 0 none, 1 simple, 2 normal */
    struct { int y1[2], y2[2], uv[2]; } dqm[4];
    uint8_t proba[4][8][3][11];
    const vp8_band *bands[4][17];
    bool use_skip;
    int skip_p;
    struct finfo fstr[4][2];

    /* by macroblock column */
    uint8_t *intra_t;
    struct nzctx *nz_t;
    struct top_samples *yuv_t;
    struct finfo *f_row;
    uint8_t intra_l[4];
    struct nzctx nz_l;

    /* the macroblock being decoded */
    int16_t coeffs[384];
    uint8_t imodes[16];
    int uvmode, segment;
    bool is_i4x4, skip;
    uint32_t nz_y, nz_uv;
    uint8_t *yuv_b;

    /* rows */
    int extra;              /* luma rows held back for the next filter */
    uint8_t *cache_y, *cache_u, *cache_v;
    int ys, uvs;
    int mb_y;               /* the next macroblock row */
    int ready;              /* image rows below this are final */
    int cache_top;          /* the image row at the cache's first */
    int y;                  /* the next row handed out */
    struct uint8_rgb *rgb;
    struct img_part part;
};

static FORCE_INLINE int clip8(int v)
{
    return (v & ~0xff) == 0 ? v : v < 0 ? 0 : 255;
}

/* ---- headers --------------------------------------------------------- */

static const uint8_t bands_of[17] = {
    0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0
};

static int clipq(int v, int m)
{
    return v < 0 ? 0 : v > m ? m : v;
}

/* Everything in the first partition before the macroblocks; the token
 * partitions' sizes follow it in the file at `after` */
static bool read_headers(struct vp8 *d, int fd, off_t after, off_t end,
                         uint8_t *tokbuf)
{
    struct br *b = &d->br;
    bool use_segment, absolute = true;
    int quant[4] = { 0 }, fstrength[4] = { 0 };
    int level, sharpness, ref_delta = 0, mode_delta = 0;
    bool use_lf_delta;
    int base_q, dqy1_dc, dqy2_dc, dqy2_ac, dquv_dc, dquv_ac;
    uint8_t sizes[21];
    off_t at;
    int i, t, s, c, p;

    br_value(b, 2);                     /* colour space, clamping */

    memset(d->seg_proba, 255, sizeof d->seg_proba);
    use_segment = br_bit(b, 0x80);
    d->update_map = false;
    if (use_segment)
    {
        d->update_map = br_bit(b, 0x80);
        if (br_bit(b, 0x80))
        {
            absolute = br_bit(b, 0x80);
            for (s = 0; s < 4; s++)
                quant[s] = br_bit(b, 0x80) ? br_svalue(b, 7) : 0;
            for (s = 0; s < 4; s++)
                fstrength[s] = br_bit(b, 0x80) ? br_svalue(b, 6) : 0;
        }
        if (d->update_map)
            for (s = 0; s < 3; s++)
                d->seg_proba[s] = br_bit(b, 0x80) ? br_value(b, 8) : 255;
    }

    d->filter_type = br_bit(b, 0x80) ? 1 : 2;
    level = br_value(b, 6);
    sharpness = br_value(b, 3);
    use_lf_delta = br_bit(b, 0x80);
    if (use_lf_delta && br_bit(b, 0x80))
    {   /* a key frame's macroblocks are all intra: only delta 0 counts */
        for (i = 0; i < 4; i++)
            if (br_bit(b, 0x80))
            {
                int v = br_svalue(b, 6);
                if (i == 0)
                    ref_delta = v;
            }
        for (i = 0; i < 4; i++)
            if (br_bit(b, 0x80))
            {
                int v = br_svalue(b, 6);
                if (i == 0)
                    mode_delta = v;
            }
    }
    if (level == 0)
        d->filter_type = 0;

    /* the token partitions, one after another past the sizes of all but
     * the last */
    d->ntok = 1 << br_value(b, 2);
    if (d->ntok > 1)
    {
        int n = 3 * (d->ntok - 1);
        if (lseek(fd, after, SEEK_SET) != after || read(fd, sizes, n) != n)
            return false;
    }
    at = after + 3 * (d->ntok - 1);
    for (p = 0; p < d->ntok; p++)
    {
        struct part *tp = &d->parts[1 + p];
        off_t size = end - at;

        if (p < d->ntok - 1)
        {
            off_t want = sizes[3 * p] | sizes[3 * p + 1] << 8
                         | sizes[3 * p + 2] << 16;
            if (want < size)
                size = want;
        }
        if (size < 0)
            size = 0;
        tp->fd = fd;
        tp->pos = at;
        tp->end = at + size;
        tp->size = TOKEN_BUF / d->ntok;
        tp->buf = tokbuf + p * tp->size;
        tp->at = tp->len = 0;
        at += size;
        br_init(&d->tok[p], tp);
    }

    base_q = br_value(b, 7);
    dqy1_dc = br_bit(b, 0x80) ? br_svalue(b, 4) : 0;
    dqy2_dc = br_bit(b, 0x80) ? br_svalue(b, 4) : 0;
    dqy2_ac = br_bit(b, 0x80) ? br_svalue(b, 4) : 0;
    dquv_dc = br_bit(b, 0x80) ? br_svalue(b, 4) : 0;
    dquv_ac = br_bit(b, 0x80) ? br_svalue(b, 4) : 0;
    for (s = 0; s < 4; s++)
    {
        int q = use_segment ? quant[s] + (absolute ? 0 : base_q) : base_q;

        d->dqm[s].y1[0] = vp8_dc_q[clipq(q + dqy1_dc, 127)];
        d->dqm[s].y1[1] = vp8_ac_q[clipq(q, 127)];
        d->dqm[s].y2[0] = vp8_dc_q[clipq(q + dqy2_dc, 127)] * 2;
        d->dqm[s].y2[1] = (vp8_ac_q[clipq(q + dqy2_ac, 127)] * 101581) >> 16;
        if (d->dqm[s].y2[1] < 8)
            d->dqm[s].y2[1] = 8;
        d->dqm[s].uv[0] = vp8_dc_q[clipq(q + dquv_dc, 117)];
        d->dqm[s].uv[1] = vp8_ac_q[clipq(q + dquv_ac, 127)];
    }

    br_bit(b, 0x80);                    /* refresh entropy probs */
    for (t = 0; t < 4; t++)
    {
        for (i = 0; i < 8; i++)
            for (c = 0; c < 3; c++)
                for (p = 0; p < 11; p++)
                    d->proba[t][i][c][p] =
                        br_bit(b, vp8_coef_update[t][i][c][p])
                            ? br_value(b, 8) : vp8_coef_proba[t][i][c][p];
        for (i = 0; i < 17; i++)
            d->bands[t][i] = &d->proba[t][bands_of[i]];
    }
    d->use_skip = br_bit(b, 0x80);
    if (d->use_skip)
        d->skip_p = br_value(b, 8);

    /* each segment's filter, with and without 4x4 modes */
    for (s = 0; s < 4 && d->filter_type; s++)
    {
        int base = use_segment ? fstrength[s] + (absolute ? 0 : level)
                               : level;
        for (i = 0; i < 2; i++)
        {
            struct finfo *f = &d->fstr[s][i];
            int l = base;

            if (use_lf_delta)
                l += ref_delta + (i ? mode_delta : 0);
            l = clipq(l, 63);
            f->limit = 0;
            f->inner = i;
            if (l > 0)
            {
                int il = l;
                if (sharpness > 0)
                {
                    il >>= sharpness > 4 ? 2 : 1;
                    if (il > 9 - sharpness)
                        il = 9 - sharpness;
                }
                if (il < 1)
                    il = 1;
                f->ilevel = il;
                f->limit = 2 * l + il;
                f->hev = l >= 40 ? 2 : l >= 15 ? 1 : 0;
            }
        }
    }
    return !b->eof;
}

/* ---- macroblocks ----------------------------------------------------- */

static NO_INLINE void read_modes(struct vp8 *d, int mb_x)
{
    struct br *b = &d->br;
    uint8_t *top = d->intra_t + 4 * mb_x, *left = d->intra_l;

    d->segment = 0;
    if (d->update_map)
        d->segment = !br_bit(b, d->seg_proba[0])
                     ? br_bit(b, d->seg_proba[1])
                     : br_bit(b, d->seg_proba[2]) + 2;
    d->skip = d->use_skip ? br_bit(b, d->skip_p) : false;
    d->is_i4x4 = !br_bit(b, 145);
    if (!d->is_i4x4)
    {
        int m = br_bit(b, 156) ? (br_bit(b, 128) ? TM_PRED : H_PRED)
                               : (br_bit(b, 163) ? V_PRED : DC_PRED);
        d->imodes[0] = m;
        memset(top, m, 4);
        memset(left, m, 4);
    }
    else
    {
        uint8_t *modes = d->imodes;

        for (int y = 0; y < 4; y++)
        {
            int m = left[y];

            for (int x = 0; x < 4; x++)
            {
                const uint8_t *p = vp8_bmode_proba[top[x]][m];

                m = !br_bit(b, p[0]) ? B_DC_PRED
                    : !br_bit(b, p[1]) ? B_TM_PRED
                    : !br_bit(b, p[2]) ? B_VE_PRED
                    : !br_bit(b, p[3])
                      ? (!br_bit(b, p[4]) ? B_HE_PRED
                         : !br_bit(b, p[5]) ? B_RD_PRED : B_VR_PRED)
                    : !br_bit(b, p[6]) ? B_LD_PRED
                    : !br_bit(b, p[7]) ? B_VL_PRED
                    : !br_bit(b, p[8]) ? B_HD_PRED : B_HU_PRED;
                top[x] = m;
            }
            memcpy(modes, top, 4);
            modes += 4;
            left[y] = m;
        }
    }
    d->uvmode = !br_bit(b, 142) ? DC_PRED
                : !br_bit(b, 114) ? V_PRED
                : br_bit(b, 183) ? TM_PRED : H_PRED;
}

static const uint8_t cat3[] = { 173, 148, 140, 0 };
static const uint8_t cat4[] = { 176, 155, 140, 135, 0 };
static const uint8_t cat5[] = { 180, 157, 141, 134, 130, 0 };
static const uint8_t cat6[] = { 254, 254, 243, 230, 196, 177,
                                153, 140, 133, 130, 129, 0 };
static const uint8_t *const cat3456[] = { cat3, cat4, cat5, cat6 };
static const uint8_t zigzag[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

/* A token above one: its value, from the tree past the first three nodes */
static int large_value(struct br *b, const uint8_t *p)
{
    int v;

    if (!br_bit_in(b, p[3]))
        v = !br_bit_in(b, p[4]) ? 2 : 3 + br_bit_in(b, p[5]);
    else if (!br_bit(b, p[6]))
    {
        if (!br_bit(b, p[7]))
            v = 5 + br_bit(b, 159);
        else
        {
            v = 7 + 2 * br_bit(b, 165);
            v += br_bit(b, 145);
        }
    }
    else
    {
        int bit1 = br_bit(b, p[8]);
        int bit0 = br_bit(b, p[9 + bit1]);
        int cat = 2 * bit1 + bit0;

        v = 0;
        for (const uint8_t *tab = cat3456[cat]; *tab; tab++)
            v += v + br_bit(b, *tab);
        v += 3 + (8 << cat);
    }
    return v;
}

/* One block's coefficients from position n, dequantised into out in raster
 * order. Returns one past the last that is not zero. */
static int get_coeffs(struct br *b, const vp8_band *const *prob, int ctx,
                      const int *dq, int n, int16_t *out)
{
    const uint8_t *p = (*prob[n])[ctx];

    for (; n < 16; n++)
    {
        if (!br_bit_in(b, p[0]))
            return n;
        while (!br_bit_in(b, p[1]))
        {
            p = (*prob[++n])[0];
            if (n == 16)
                return 16;
        }
        {
            const vp8_band *pc = prob[n + 1];
            int v;

            if (!br_bit_in(b, p[2]))
            {
                v = 1;
                p = (*pc)[1];
            }
            else
            {
                v = large_value(b, p);
                p = (*pc)[2];
            }
            out[zigzag[n]] = br_signed(b, v) * dq[n > 0];
        }
    }
    return 16;
}

/* The inverse Walsh-Hadamard transform of the luma DCs, into each block */
static void transform_wht(const int16_t *in, int16_t *out)
{
    int tmp[16];

    for (int i = 0; i < 4; i++)
    {
        int a0 = in[0 + i] + in[12 + i];
        int a1 = in[4 + i] + in[8 + i];
        int a2 = in[4 + i] - in[8 + i];
        int a3 = in[0 + i] - in[12 + i];

        tmp[0 + i] = a0 + a1;
        tmp[8 + i] = a0 - a1;
        tmp[4 + i] = a3 + a2;
        tmp[12 + i] = a3 - a2;
    }
    for (int i = 0; i < 4; i++)
    {
        int dc = tmp[0 + i * 4] + 3;
        int a0 = dc + tmp[3 + i * 4];
        int a1 = tmp[1 + i * 4] + tmp[2 + i * 4];
        int a2 = tmp[1 + i * 4] - tmp[2 + i * 4];
        int a3 = dc - tmp[3 + i * 4];

        out[0] = (a0 + a1) >> 3;
        out[16] = (a3 + a2) >> 3;
        out[32] = (a0 - a1) >> 3;
        out[48] = (a3 - a2) >> 3;
        out += 64;
    }
}

/* Two bits a block: 3 any coefficient, 2 only the first three, 1 the DC */
static inline uint32_t nz_code(uint32_t bits, int nz, int dc_nz)
{
    return bits << 2 | (nz > 3 ? 3 : nz > 1 ? 2 : dc_nz);
}

/* The macroblock's coefficients, and the non-zero contexts it leaves.
 * True when every one is zero. */
static NO_INLINE bool read_residuals(struct vp8 *d, struct nzctx *mb,
                                     struct br *tb)
{
    const vp8_band *const *ac;
    struct nzctx *left = &d->nz_l;
    const int *y1 = d->dqm[d->segment].y1;
    int16_t *dst = d->coeffs;
    uint8_t tnz, lnz;
    uint32_t nz_y = 0, nz_uv = 0, out_t, out_l;
    int first;

    memset(dst, 0, sizeof d->coeffs);
    if (!d->is_i4x4)
    {
        int16_t dc[16];
        int ctx = mb->nz_dc + left->nz_dc;
        int nz;

        memset(dc, 0, sizeof dc);
        nz = get_coeffs(tb, d->bands[1], ctx, d->dqm[d->segment].y2, 0, dc);
        mb->nz_dc = left->nz_dc = nz > 0;
        if (nz > 1)
            transform_wht(dc, dst);
        else
        {
            int dc0 = (dc[0] + 3) >> 3;

            for (int i = 0; i < 16 * 16; i += 16)
                dst[i] = dc0;
        }
        first = 1;
        ac = d->bands[0];
    }
    else
    {
        first = 0;
        ac = d->bands[3];
    }

    tnz = mb->nz & 0x0f;
    lnz = left->nz & 0x0f;
    for (int y = 0; y < 4; y++)
    {
        int l = lnz & 1;
        uint32_t bits = 0;

        for (int x = 0; x < 4; x++)
        {
            int ctx = l + (tnz & 1);
            int nz = get_coeffs(tb, ac, ctx, y1, first, dst);

            l = nz > first;
            tnz = (tnz >> 1) | (l << 7);
            bits = nz_code(bits, nz, dst[0] != 0);
            dst += 16;
        }
        tnz >>= 4;
        lnz = (lnz >> 1) | (l << 7);
        nz_y = (nz_y << 8) | bits;
    }
    out_t = tnz;
    out_l = lnz >> 4;

    for (int ch = 0; ch < 4; ch += 2)
    {
        uint32_t bits = 0;

        tnz = mb->nz >> (4 + ch);
        lnz = left->nz >> (4 + ch);
        for (int y = 0; y < 2; y++)
        {
            int l = lnz & 1;

            for (int x = 0; x < 2; x++)
            {
                int ctx = l + (tnz & 1);
                int nz = get_coeffs(tb, d->bands[2], ctx,
                                    d->dqm[d->segment].uv, 0, dst);

                l = nz > 0;
                tnz = (tnz >> 1) | (l << 3);
                bits = nz_code(bits, nz, dst[0] != 0);
                dst += 16;
            }
            tnz >>= 2;
            lnz = (lnz >> 1) | (l << 5);
        }
        nz_uv |= bits << (4 * ch);
        out_t |= (tnz << 4) << ch;
        out_l |= (lnz & 0xf0) << ch;
    }
    mb->nz = out_t;
    left->nz = out_l;
    d->nz_y = nz_y;
    d->nz_uv = nz_uv;
    return !(nz_y | nz_uv);
}

/* ---- reconstruction -------------------------------------------------- */

#define MUL1(a) ((((a) * 20091) >> 16) + (a))
#define MUL2(a) (((a) * 35468) >> 16)
#define STORE(x, y, v) \
    dst[(x) + (y) * BPS] = clip8(dst[(x) + (y) * BPS] + ((v) >> 3))

static void transform(const int16_t *in, uint8_t *dst)
{
    int c[16], *tmp = c;

    for (int i = 0; i < 4; i++, in++, tmp += 4)
    {
        int a = in[0] + in[8];
        int b = in[0] - in[8];
        int cc = MUL2(in[4]) - MUL1(in[12]);
        int dd = MUL1(in[4]) + MUL2(in[12]);

        tmp[0] = a + dd;
        tmp[1] = b + cc;
        tmp[2] = b - cc;
        tmp[3] = a - dd;
    }
    tmp = c;
    for (int i = 0; i < 4; i++, tmp++, dst += BPS)
    {
        int dc = tmp[0] + 4;
        int a = dc + tmp[8];
        int b = dc - tmp[8];
        int cc = MUL2(tmp[4]) - MUL1(tmp[12]);
        int dd = MUL1(tmp[4]) + MUL2(tmp[12]);

        STORE(0, 0, a + dd);
        STORE(1, 0, b + cc);
        STORE(2, 0, b - cc);
        STORE(3, 0, a - dd);
    }
}

/* Only in[0], in[1] and in[4] set */
static void transform_ac3(const int16_t *in, uint8_t *dst)
{
    int a = in[0] + 4;
    int c4 = MUL2(in[4]), d4 = MUL1(in[4]);
    int c1 = MUL2(in[1]), d1 = MUL1(in[1]);
    int row[4] = { a + d4, a + c4, a - c4, a - d4 };

    for (int y = 0; y < 4; y++)
    {
        STORE(0, y, row[y] + d1);
        STORE(1, y, row[y] + c1);
        STORE(2, y, row[y] - c1);
        STORE(3, y, row[y] - d1);
    }
}

static void transform_dc(const int16_t *in, uint8_t *dst)
{
    int dc = in[0] + 4;

    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            STORE(x, y, dc);
}

static void do_transform(uint32_t bits, const int16_t *in, uint8_t *dst)
{
    switch (bits >> 30)
    {
        case 3: transform(in, dst);     break;
        case 2: transform_ac3(in, dst); break;
        case 1: transform_dc(in, dst);  break;
    }
}

static void do_uv_transform(uint32_t bits, const int16_t *in, uint8_t *dst)
{
    if (!(bits & 0xff))
        return;
    if (bits & 0xaa)
    {
        transform(in, dst);
        transform(in + 16, dst + 4);
        transform(in + 32, dst + 4 * BPS);
        transform(in + 48, dst + 4 * BPS + 4);
    }
    else
    {
        if (in[0])  transform_dc(in, dst);
        if (in[16]) transform_dc(in + 16, dst + 4);
        if (in[32]) transform_dc(in + 32, dst + 4 * BPS);
        if (in[48]) transform_dc(in + 48, dst + 4 * BPS + 4);
    }
}

#undef STORE

#define DST(x, y) dst[(x) + (y) * BPS]
#define AVG3(a, b, c) ((uint8_t)(((a) + 2 * (b) + (c) + 2) >> 2))
#define AVG2(a, b) (((a) + (b) + 1) >> 1)

static void true_motion(uint8_t *dst, int size)
{
    const uint8_t *top = dst - BPS;
    int tl = top[-1];

    for (int y = 0; y < size; y++, dst += BPS)
    {
        int l = dst[-1] - tl;

        for (int x = 0; x < size; x++)
            dst[x] = clip8(top[x] + l);
    }
}

/* dst is word-aligned, as every block in the work block is */
static void fill(uint8_t *dst, int size, int v)
{
    uint32_t w = v * 0x01010101u;

    for (int y = 0; y < size; y++, dst += BPS)
        for (int x = 0; x < size; x += 4)
            *(uint32_t *)(dst + x) = w;
}

#define COPY4(d, s) (*(uint32_t *)(d) = *(const uint32_t *)(s))

/* A 16x16 luma or 8x8 chroma block's prediction, `size` its width */
static void predict_block(uint8_t *dst, int size, int mode)
{
    int shift = size == 16 ? 5 : 4, dc = 0;

    switch (mode)
    {
        case V_PRED:
            for (int y = 0; y < size; y++)
                memcpy(dst + y * BPS, dst - BPS, size);
            break;
        case H_PRED:
            for (int y = 0; y < size; y++)
                memset(dst + y * BPS, dst[y * BPS - 1], size);
            break;
        case TM_PRED:
            true_motion(dst, size);
            break;
        case DC_PRED:
            for (int i = 0; i < size; i++)
                dc += dst[i - BPS] + dst[-1 + i * BPS];
            fill(dst, size, (dc + (1 << (shift - 1))) >> shift);
            break;
        case B_DC_PRED_NOTOP:
            for (int i = 0; i < size; i++)
                dc += dst[-1 + i * BPS];
            fill(dst, size, (dc + (1 << (shift - 2))) >> (shift - 1));
            break;
        case B_DC_PRED_NOLEFT:
            for (int i = 0; i < size; i++)
                dc += dst[i - BPS];
            fill(dst, size, (dc + (1 << (shift - 2))) >> (shift - 1));
            break;
        default:
            fill(dst, size, 0x80);
            break;
    }
}

static void predict4(uint8_t *dst, int mode)
{
    const uint8_t *top = dst - BPS;
    int X = top[-1], A = top[0], B = top[1], C = top[2], D = top[3];
    int E = top[4], F = top[5], G = top[6], H = top[7];
    int I = dst[-1], J = dst[-1 + BPS], K = dst[-1 + 2 * BPS];
    int L = dst[-1 + 3 * BPS];

    switch (mode)
    {
        case B_DC_PRED:
        {
            int dc = 4;

            for (int i = 0; i < 4; i++)
                dc += top[i] + dst[-1 + i * BPS];
            fill(dst, 4, dc >> 3);
            break;
        }
        case B_TM_PRED:
            true_motion(dst, 4);
            break;
        case B_VE_PRED:
        {
            uint8_t v[4] = { AVG3(X, A, B), AVG3(A, B, C), AVG3(B, C, D),
                             AVG3(C, D, E) };

            for (int y = 0; y < 4; y++)
                memcpy(dst + y * BPS, v, 4);
            break;
        }
        case B_HE_PRED:
            memset(dst, AVG3(X, I, J), 4);
            memset(dst + BPS, AVG3(I, J, K), 4);
            memset(dst + 2 * BPS, AVG3(J, K, L), 4);
            memset(dst + 3 * BPS, AVG3(K, L, L), 4);
            break;
        case B_RD_PRED:
            DST(0, 3) = AVG3(J, K, L);
            DST(1, 3) = DST(0, 2) = AVG3(I, J, K);
            DST(2, 3) = DST(1, 2) = DST(0, 1) = AVG3(X, I, J);
            DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = AVG3(A, X, I);
            DST(3, 2) = DST(2, 1) = DST(1, 0) = AVG3(B, A, X);
            DST(3, 1) = DST(2, 0) = AVG3(C, B, A);
            DST(3, 0) = AVG3(D, C, B);
            break;
        case B_LD_PRED:
            DST(0, 0) = AVG3(A, B, C);
            DST(1, 0) = DST(0, 1) = AVG3(B, C, D);
            DST(2, 0) = DST(1, 1) = DST(0, 2) = AVG3(C, D, E);
            DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = AVG3(D, E, F);
            DST(3, 1) = DST(2, 2) = DST(1, 3) = AVG3(E, F, G);
            DST(3, 2) = DST(2, 3) = AVG3(F, G, H);
            DST(3, 3) = AVG3(G, H, H);
            break;
        case B_VR_PRED:
            DST(0, 0) = DST(1, 2) = AVG2(X, A);
            DST(1, 0) = DST(2, 2) = AVG2(A, B);
            DST(2, 0) = DST(3, 2) = AVG2(B, C);
            DST(3, 0) = AVG2(C, D);
            DST(0, 3) = AVG3(K, J, I);
            DST(0, 2) = AVG3(J, I, X);
            DST(0, 1) = DST(1, 3) = AVG3(I, X, A);
            DST(1, 1) = DST(2, 3) = AVG3(X, A, B);
            DST(2, 1) = DST(3, 3) = AVG3(A, B, C);
            DST(3, 1) = AVG3(B, C, D);
            break;
        case B_VL_PRED:
            DST(0, 0) = AVG2(A, B);
            DST(1, 0) = DST(0, 2) = AVG2(B, C);
            DST(2, 0) = DST(1, 2) = AVG2(C, D);
            DST(3, 0) = DST(2, 2) = AVG2(D, E);
            DST(0, 1) = AVG3(A, B, C);
            DST(1, 1) = DST(0, 3) = AVG3(B, C, D);
            DST(2, 1) = DST(1, 3) = AVG3(C, D, E);
            DST(3, 1) = DST(2, 3) = AVG3(D, E, F);
            DST(3, 2) = AVG3(E, F, G);
            DST(3, 3) = AVG3(F, G, H);
            break;
        case B_HD_PRED:
            DST(0, 0) = DST(2, 1) = AVG2(I, X);
            DST(0, 1) = DST(2, 2) = AVG2(J, I);
            DST(0, 2) = DST(2, 3) = AVG2(K, J);
            DST(0, 3) = AVG2(L, K);
            DST(3, 0) = AVG3(A, B, C);
            DST(2, 0) = AVG3(X, A, B);
            DST(1, 0) = DST(3, 1) = AVG3(I, X, A);
            DST(1, 1) = DST(3, 2) = AVG3(J, I, X);
            DST(1, 2) = DST(3, 3) = AVG3(K, J, I);
            DST(1, 3) = AVG3(L, K, J);
            break;
        default:                        /* B_HU_PRED */
            DST(0, 0) = AVG2(I, J);
            DST(2, 0) = DST(0, 1) = AVG2(J, K);
            DST(2, 1) = DST(0, 2) = AVG2(K, L);
            DST(1, 0) = AVG3(I, J, K);
            DST(3, 0) = DST(1, 1) = AVG3(J, K, L);
            DST(3, 1) = DST(1, 2) = AVG3(K, L, L);
            DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3)
                = DST(3, 3) = L;
            break;
    }
}

#undef DST
#undef AVG3
#undef AVG2

/* DC prediction for what the frame's edges leave it */
static int dc_mode(int mb_x, int mb_y, int mode)
{
    if (mode != B_DC_PRED)
        return mode;
    if (mb_x == 0)
        return mb_y == 0 ? B_DC_PRED_NOTOPLEFT : B_DC_PRED_NOLEFT;
    return mb_y == 0 ? B_DC_PRED_NOTOP : B_DC_PRED;
}

static const uint16_t scan[16] = {
    0 + 0 * BPS,  4 + 0 * BPS,  8 + 0 * BPS,  12 + 0 * BPS,
    0 + 4 * BPS,  4 + 4 * BPS,  8 + 4 * BPS,  12 + 4 * BPS,
    0 + 8 * BPS,  4 + 8 * BPS,  8 + 8 * BPS,  12 + 8 * BPS,
    0 + 12 * BPS, 4 + 12 * BPS, 8 + 12 * BPS, 12 + 12 * BPS
};

/* The work block's edges for the row's first macroblock */
static void begin_row(struct vp8 *d)
{
    uint8_t *y = d->yuv_b + Y_OFF, *u = d->yuv_b + U_OFF;
    uint8_t *v = d->yuv_b + V_OFF;

    for (int j = 0; j < 16; j++)
        y[j * BPS - 1] = 129;
    for (int j = 0; j < 8; j++)
        u[j * BPS - 1] = v[j * BPS - 1] = 129;
    if (d->mb_y > 0)
        y[-1 - BPS] = u[-1 - BPS] = v[-1 - BPS] = 129;
    else
    {
        memset(y - BPS - 1, 127, 16 + 4 + 1);
        memset(u - BPS - 1, 127, 8 + 1);
        memset(v - BPS - 1, 127, 8 + 1);
    }
}

static NO_INLINE void reconstruct(struct vp8 *d, int mb_x)
{
    uint8_t *y = d->yuv_b + Y_OFF, *u = d->yuv_b + U_OFF;
    uint8_t *v = d->yuv_b + V_OFF;
    struct top_samples *top = d->yuv_t + mb_x;
    const int16_t *coeffs = d->coeffs;
    uint32_t bits = d->nz_y;
    int mb_y = d->mb_y, j, mode;

    if (mb_x > 0)
    {   /* the last macroblock's right columns become this one's left */
        for (j = -1; j < 16; j++)
            COPY4(y + j * BPS - 4, y + j * BPS + 12);
        for (j = -1; j < 8; j++)
        {
            COPY4(u + j * BPS - 4, u + j * BPS + 4);
            COPY4(v + j * BPS - 4, v + j * BPS + 4);
        }
    }
    if (mb_y > 0)
    {
        memcpy(y - BPS, top->y, 16);
        memcpy(u - BPS, top->u, 8);
        memcpy(v - BPS, top->v, 8);
    }

    if (d->is_i4x4)
    {
        uint8_t *tr = y - BPS + 16;

        /* above-right of the right column's blocks: the next macroblock's
         * top, the same four for every row of blocks */
        if (mb_y > 0)
        {
            if (mb_x >= d->mb_w - 1)
                memset(tr, top->y[15], 4);
            else
                memcpy(tr, top[1].y, 4);
        }
        COPY4(tr + 4 * BPS, tr);
        COPY4(tr + 8 * BPS, tr);
        COPY4(tr + 12 * BPS, tr);
        for (int n = 0; n < 16; n++, bits <<= 2)
        {
            uint8_t *dst = y + scan[n];

            predict4(dst, d->imodes[n]);
            do_transform(bits, coeffs + n * 16, dst);
        }
    }
    else
    {
        predict_block(y, 16, dc_mode(mb_x, mb_y, d->imodes[0]));
        for (int n = 0; bits && n < 16; n++, bits <<= 2)
            do_transform(bits, coeffs + n * 16, y + scan[n]);
    }

    mode = dc_mode(mb_x, mb_y, d->uvmode);
    predict_block(u, 8, mode);
    predict_block(v, 8, mode);
    do_uv_transform(d->nz_uv, coeffs + 16 * 16, u);
    do_uv_transform(d->nz_uv >> 8, coeffs + 20 * 16, v);

    if (mb_y < d->mb_h - 1)
    {
        memcpy(top->y, y + 15 * BPS, 16);
        memcpy(top->u, u + 7 * BPS, 8);
        memcpy(top->v, v + 7 * BPS, 8);
    }

    {
        uint8_t *yo = d->cache_y + d->extra * d->ys + mb_x * 16;
        uint8_t *uo = d->cache_u + d->extra / 2 * d->uvs + mb_x * 8;
        uint8_t *vo = d->cache_v + d->extra / 2 * d->uvs + mb_x * 8;

        for (j = 0; j < 16; j++)
            for (int k = 0; k < 16; k += 4)
                COPY4(yo + j * d->ys + k, y + j * BPS + k);
        for (j = 0; j < 8; j++)
        {
            COPY4(uo + j * d->uvs, u + j * BPS);
            COPY4(uo + j * d->uvs + 4, u + j * BPS + 4);
            COPY4(vo + j * d->uvs, v + j * BPS);
            COPY4(vo + j * d->uvs + 4, v + j * BPS + 4);
        }
    }
}

/* ---- filter ---------------------------------------------------------- */

static FORCE_INLINE int sclip1(int v)         /* to [-128, 127] */
{
    return v < -128 ? -128 : v > 127 ? 127 : v;
}

static FORCE_INLINE int sclip2(int v)         /* to [-16, 15] */
{
    return v < -16 ? -16 : v > 15 ? 15 : v;
}

static FORCE_INLINE int iabs(int v)
{
    return v < 0 ? -v : v;
}

static FORCE_INLINE void filter2(uint8_t *p, int step)
{
    int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    int a = 3 * (q0 - p0) + sclip1(p1 - q1);
    int a1 = sclip2((a + 4) >> 3);
    int a2 = sclip2((a + 3) >> 3);

    p[-step] = clip8(p0 + a2);
    p[0] = clip8(q0 - a1);
}

static FORCE_INLINE void filter4(uint8_t *p, int step)
{
    int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    int a = 3 * (q0 - p0);
    int a1 = sclip2((a + 4) >> 3);
    int a2 = sclip2((a + 3) >> 3);
    int a3 = (a1 + 1) >> 1;

    p[-2 * step] = clip8(p1 + a3);
    p[-step] = clip8(p0 + a2);
    p[0] = clip8(q0 - a1);
    p[step] = clip8(q1 - a3);
}

static FORCE_INLINE void filter6(uint8_t *p, int step)
{
    int p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
    int q0 = p[0], q1 = p[step], q2 = p[2 * step];
    int a = sclip1(3 * (q0 - p0) + sclip1(p1 - q1));
    int a1 = (27 * a + 63) >> 7;
    int a2 = (18 * a + 63) >> 7;
    int a3 = (9 * a + 63) >> 7;

    p[-3 * step] = clip8(p2 + a3);
    p[-2 * step] = clip8(p1 + a2);
    p[-step] = clip8(p0 + a1);
    p[0] = clip8(q0 - a1);
    p[step] = clip8(q1 - a2);
    p[2 * step] = clip8(q2 - a3);
}

static FORCE_INLINE bool hev(const uint8_t *p, int step, int thresh)
{
    return iabs(p[-2 * step] - p[-step]) > thresh
           || iabs(p[step] - p[0]) > thresh;
}

static FORCE_INLINE bool needs_filter(const uint8_t *p, int step, int t)
{
    return 4 * iabs(p[-step] - p[0]) + iabs(p[-2 * step] - p[step]) <= t;
}

static FORCE_INLINE bool needs_filter2(const uint8_t *p, int step, int t,
                                        int it)
{
    int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    int p3, p2, q2, q3;

    if (4 * iabs(p0 - q0) + iabs(p1 - q1) > t)
        return false;
    p3 = p[-4 * step];
    p2 = p[-3 * step];
    q2 = p[2 * step];
    q3 = p[3 * step];
    return iabs(p3 - p2) <= it && iabs(p2 - p1) <= it
           && iabs(p1 - p0) <= it && iabs(q3 - q2) <= it
           && iabs(q2 - q1) <= it && iabs(q1 - q0) <= it;
}

/* The simple filter along `size` pixels of one edge: hstride across it,
 * vstride along it */
static void simple_edge(uint8_t *p, int hstride, int vstride, int thresh)
{
    int t = 2 * thresh + 1;

    for (int i = 0; i < 16; i++, p += vstride)
        if (needs_filter(p, hstride, t))
            filter2(p, hstride);
}

/* The normal filter along one edge, six taps on a macroblock's edge and
 * four inside one */
#ifdef CPU_ARM
/* The same in webp_arm.S, which reads its limits from this struct at fixed
 * offsets */
struct edge_limits {
    int t, ithresh, hev;
};
_Static_assert(offsetof(struct edge_limits, ithresh) == 4
               && offsetof(struct edge_limits, hev) == 8,
               "webp_arm.S reads struct edge_limits by these offsets");

void webp_edge_mb_arm(uint8_t *p, int hstride, int vstride, int size,
                      const struct edge_limits *l);
void webp_edge_inner_arm(uint8_t *p, int hstride, int vstride, int size,
                         const struct edge_limits *l);

static void normal_edge(uint8_t *p, int hstride, int vstride, int size,
                        int thresh, int ithresh, int hev_t, bool mb_edge)
{
    struct edge_limits l = { 2 * thresh + 1, ithresh, hev_t };

    if (mb_edge)
        webp_edge_mb_arm(p, hstride, vstride, size, &l);
    else
        webp_edge_inner_arm(p, hstride, vstride, size, &l);
}
#else
static void normal_edge(uint8_t *p, int hstride, int vstride, int size,
                        int thresh, int ithresh, int hev_t, bool mb_edge)
{
    int t = 2 * thresh + 1;

    for (; size > 0; size--, p += vstride)
    {
        if (!needs_filter2(p, hstride, t, ithresh))
            continue;
        if (hev(p, hstride, hev_t))
            filter2(p, hstride);
        else if (mb_edge)
            filter6(p, hstride);
        else
            filter4(p, hstride);
    }
}
#endif

static void filter_mb(struct vp8 *d, int mb_x)
{
    const struct finfo *f = d->f_row + mb_x;
    int ys = d->ys, uvs = d->uvs;
    uint8_t *y = d->cache_y + d->extra * ys + mb_x * 16;
    int limit = f->limit, il = f->ilevel, hv = f->hev;

    if (!limit)
        return;
    if (d->filter_type == 1)
    {
        if (mb_x > 0)
            simple_edge(y, 1, ys, limit + 4);
        if (f->inner)
            for (int k = 4; k < 16; k += 4)
                simple_edge(y + k, 1, ys, limit);
        if (d->mb_y > 0)
            simple_edge(y, ys, 1, limit + 4);
        if (f->inner)
            for (int k = 4; k < 16; k += 4)
                simple_edge(y + k * ys, ys, 1, limit);
    }
    else
    {
        uint8_t *u = d->cache_u + d->extra / 2 * uvs + mb_x * 8;
        uint8_t *v = d->cache_v + d->extra / 2 * uvs + mb_x * 8;

        if (mb_x > 0)
        {
            normal_edge(y, 1, ys, 16, limit + 4, il, hv, true);
            normal_edge(u, 1, uvs, 8, limit + 4, il, hv, true);
            normal_edge(v, 1, uvs, 8, limit + 4, il, hv, true);
        }
        if (f->inner)
        {
            for (int k = 4; k < 16; k += 4)
                normal_edge(y + k, 1, ys, 16, limit, il, hv, false);
            normal_edge(u + 4, 1, uvs, 8, limit, il, hv, false);
            normal_edge(v + 4, 1, uvs, 8, limit, il, hv, false);
        }
        if (d->mb_y > 0)
        {
            normal_edge(y, ys, 1, 16, limit + 4, il, hv, true);
            normal_edge(u, uvs, 1, 8, limit + 4, il, hv, true);
            normal_edge(v, uvs, 1, 8, limit + 4, il, hv, true);
        }
        if (f->inner)
        {
            for (int k = 4; k < 16; k += 4)
                normal_edge(y + k * ys, ys, 1, 16, limit, il, hv, false);
            normal_edge(u + 4 * uvs, uvs, 1, 8, limit, il, hv, false);
            normal_edge(v + 4 * uvs, uvs, 1, 8, limit, il, hv, false);
        }
    }
}

/* ---- rows ------------------------------------------------------------ */

/* The next row of macroblocks into the cache, filtered, and the image rows
 * it makes final */
static NO_INLINE void decode_row(struct vp8 *d)
{
    struct br *tb = &d->tok[d->mb_y & (d->ntok - 1)];
    int extra = d->extra;

    if (d->mb_y > 0 && extra)
    {   /* the rows the last one held back go to the top */
        memmove(d->cache_y, d->cache_y + 16 * d->ys, extra * d->ys);
        memmove(d->cache_u, d->cache_u + 8 * d->uvs, extra / 2 * d->uvs);
        memmove(d->cache_v, d->cache_v + 8 * d->uvs, extra / 2 * d->uvs);
    }
    d->cache_top = 16 * d->mb_y - extra;

    memset(d->intra_l, B_DC_PRED, sizeof d->intra_l);
    d->nz_l.nz = d->nz_l.nz_dc = 0;
    begin_row(d);
    for (int mb_x = 0; mb_x < d->mb_w; mb_x++)
    {
        struct nzctx *mb = d->nz_t + mb_x;
        bool skip;

        read_modes(d, mb_x);
        skip = d->skip;
        if (!skip)
            skip = read_residuals(d, mb, tb);
        else
        {
            d->nz_l.nz = mb->nz = 0;
            if (!d->is_i4x4)
                d->nz_l.nz_dc = mb->nz_dc = 0;
            d->nz_y = d->nz_uv = 0;
        }
        if (d->filter_type)
        {
            d->f_row[mb_x] = d->fstr[d->segment][d->is_i4x4];
            d->f_row[mb_x].inner |= !skip;
        }
        reconstruct(d, mb_x);
        if ((mb_x & 15) == 15)
            yield();
    }
    if (d->filter_type)
        for (int mb_x = 0; mb_x < d->mb_w; mb_x++)
            filter_mb(d, mb_x);

    d->mb_y++;
    d->ready = d->mb_y == d->mb_h ? d->height : 16 * d->mb_y - extra;
    if (d->ready > d->height)
        d->ready = d->height;
}

/* A value with six fraction bits, to 0-255 */
static FORCE_INLINE uint32_t clip6(int v)
{
    if ((uint32_t)v >= 0x4000)
        v = ~(v >> 31);                 /* 0 below, all ones above */
    return ((uint32_t)v >> 6) & 0xff;
}

/* RGB from YUV as libwebp converts it -- BT.601, video range -- each term
 * looked up: luma's; U's green term in the low half and blue in the high;
 * V's green term low and red high. The two green halves add without a
 * carry between them, the sum fitting 16 bits. */
static int16_t yuv_y[256];
static int32_t yuv_u[256], yuv_v[256];

static void yuv_tables(void)
{
    for (int i = 0; i < 256; i++)
    {
        yuv_y[i] = (i * 19077) >> 8;
        yuv_u[i] = (uint16_t)-((i * 6419) >> 8)
                   | (uint32_t)(((i * 33050) >> 8) - 17685) << 16;
        yuv_v[i] = (uint16_t)(8708 - ((i * 13320) >> 8))
                   | (uint32_t)(((i * 26149) >> 8) - 14234) << 16;
    }
}

/* One pixel as a little-endian word: blue, green, red, alpha */
static FORCE_INLINE uint32_t yuv_px(int y, int32_t tu, int32_t tv)
{
    int yy = yuv_y[y];
    int g = (int32_t)(((uint32_t)tu << 16) + ((uint32_t)tv << 16)) >> 16;

    return clip6(yy + (tu >> 16)) | clip6(yy + g) << 8
           | clip6(yy + (tv >> 16)) << 16 | 0xff000000u;
}

#ifdef CPU_ARM
void webp_yuv_row_arm(uint32_t *out, const uint8_t *y, const uint8_t *u,
                      const uint8_t *v, int pairs, const void *tabs[3]);
#endif

static struct img_part *store_part_webp(void *args)
{
    struct vp8 *d = args;
    const uint8_t *y, *u, *v;
    int row;

    if (d->y >= d->height)
        return NULL;
    while (d->y >= d->ready)
        decode_row(d);
    row = d->y - d->cache_top;
    y = d->cache_y + row * d->ys;
    u = d->cache_u + (row >> 1) * d->uvs;
    v = d->cache_v + (row >> 1) * d->uvs;
    {
        uint32_t *o = (uint32_t *)d->rgb;
        int x = d->width & ~1;

#ifdef CPU_ARM
        static const void *tabs[3] = { yuv_y, yuv_u, yuv_v };

        webp_yuv_row_arm(o, y, u, v, x >> 1, tabs);
#else
        for (int i = 0; i < x; i += 2)
        {
            int32_t tu = yuv_u[u[i >> 1]], tv = yuv_v[v[i >> 1]];

            o[i] = yuv_px(y[i], tu, tv);
            o[i + 1] = yuv_px(y[i + 1], tu, tv);
        }
#endif
        if (x < d->width)
            o[x] = yuv_px(y[x], yuv_u[u[x >> 1]], yuv_v[v[x >> 1]]);
    }
    d->y++;
    d->part.len = d->width;
    d->part.buf = d->rgb;
    return &d->part;
}

/* ---- container ------------------------------------------------------- */

static uint32_t le32(const uint8_t *b)
{
    return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}

/* Where the first VP8 data lies: through the RIFF chunks, and into the
 * first frame of an animation. -4 for a lossless image. */
static int find_vp8(int fd, off_t pos, off_t len, off_t *data, off_t *size)
{
    uint8_t h[12];
    off_t at, end;

    if (lseek(fd, pos, SEEK_SET) != pos || read(fd, h, 12) != 12
        || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WEBP", 4))
        return -3;
    end = pos + 8 + le32(h + 4);
    if (len >= 0 && end > pos + len)
        end = pos + len;
    at = pos + 12;
    for (int n = 0; n < 64 && at + 8 <= end; n++)
    {
        uint32_t csize;

        if (lseek(fd, at, SEEK_SET) != at || read(fd, h, 8) != 8)
            return -3;
        csize = le32(h + 4);
        if (!memcmp(h, "VP8 ", 4))
        {
            *data = at + 8;
            *size = MIN((off_t)csize, end - *data);
            return 0;
        }
        if (!memcmp(h, "VP8L", 4))
            return -4;
        if (!memcmp(h, "ANMF", 4))
            at += 8 + 16;               /* its frame's own chunks follow */
        else
            at += 8 + csize + (csize & 1);
    }
    return -3;
}

/* ---- the way in ------------------------------------------------------ */

/* A bump allocation from *at, word-aligned */
static void *take(uint8_t **at, uint8_t *end, size_t n)
{
    uintptr_t p = ((uintptr_t)*at + 3) & ~(uintptr_t)3;

    if (p + n > (uintptr_t)end)
        return NULL;
    *at = (uint8_t *)(p + n);
    return (void *)p;
}

/* The decoder's memory for an image mb_w macroblocks wide, beside the
 * struct, with every alignment allowed for */
static size_t state_bytes(int mb_w, int width)
{
    return sizeof(struct vp8) + YUV_SIZE + MODES_BUF + TOKEN_BUF
           + (size_t)mb_w * (4 + sizeof(struct nzctx)
                             + sizeof(struct top_samples)
                             + sizeof(struct finfo))
           + (size_t)(16 + 8) * mb_w * 16 + 2 * (size_t)(8 + 4) * mb_w * 8
           + (size_t)width * sizeof(struct uint8_rgb) + 16 * 4;
}

int read_webp_fd(int fd, off_t pos, off_t len, struct bitmap *bm,
                 int maxsize, int format, const struct custom_format *cformat)
{
    bool resize;
    struct dim src_dim;
    struct vp8 *d;
    uint8_t h[10], *at, *end, *free_start, *tokbuf;
    off_t data, size;
    int rc, bm_size, lines, width, height, mb_w, mb_h, free_len;
    uint32_t tag;

    rc = find_vp8(fd, pos, len, &data, &size);
    if (rc < 0)
        return rc;
    if (size < 10 || lseek(fd, data, SEEK_SET) != data || read(fd, h, 10) != 10)
        return -3;
    tag = h[0] | h[1] << 8 | h[2] << 16;
    if ((tag & 1) || ((tag >> 1) & 7) > 3 || !((tag >> 4) & 1)
        || h[3] != 0x9d || h[4] != 0x01 || h[5] != 0x2a)
        return -3;                      /* a key frame, shown, signed */
    width = (h[6] | h[7] << 8) & 0x3fff;
    height = (h[8] | h[9] << 8) & 0x3fff;
    if (width < 1 || height < 1 || (off_t)(tag >> 5) > size - 10)
        return -3;
    mb_w = (width + 15) >> 4;
    mb_h = (height + 15) >> 4;

    src_dim.width = width;
    src_dim.height = height;
    bm_size = image_fit_bitmap(bm, &src_dim, format, cformat);
    resize = bm->width != width || bm->height != height;
    lines = resize ? (int)sizeof(struct uint32_argb) * 3 * bm->width : 0;
    if (format & FORMAT_RETURN_SIZE)
        return bm_size + lines + sizeof(long) + state_bytes(mb_w, width);

    /* the scaler's lines first, then the decoder */
    free_start = (unsigned char *)bm->data + bm_size;
    free_len = maxsize - bm_size;
    ALIGN_BUFFER(free_start, free_len, sizeof(uint32_t));
    if (free_len < lines)
        return -6;
    at = free_start + lines;
    end = free_start + free_len;
    d = take(&at, end, sizeof *d);
    if (!d)
        return -6;
    memset(d, 0, sizeof *d);
    d->width = width;
    d->height = height;
    d->mb_w = mb_w;
    d->mb_h = mb_h;
    d->yuv_b = take(&at, end, YUV_SIZE);
    d->parts[0].buf = take(&at, end, MODES_BUF);
    tokbuf = take(&at, end, TOKEN_BUF);
    d->intra_t = take(&at, end, 4 * mb_w);
    d->nz_t = take(&at, end, mb_w * sizeof *d->nz_t);
    d->yuv_t = take(&at, end, mb_w * sizeof *d->yuv_t);
    d->f_row = take(&at, end, mb_w * sizeof *d->f_row);
    d->rgb = take(&at, end, width * sizeof *d->rgb);
    if (!d->yuv_b || !d->parts[0].buf || !tokbuf || !d->intra_t || !d->nz_t
        || !d->yuv_t || !d->f_row || !d->rgb)
        return -6;
    memset(d->yuv_b, 0, YUV_SIZE);
    yuv_tables();
    memset(d->intra_t, B_DC_PRED, 4 * mb_w);
    memset(d->nz_t, 0, mb_w * sizeof *d->nz_t);

    d->parts[0].fd = fd;
    d->parts[0].pos = data + 10;
    d->parts[0].end = data + 10 + (tag >> 5);
    d->parts[0].size = MODES_BUF;
    br_init(&d->br, &d->parts[0]);
    if (!read_headers(d, fd, data + 10 + (tag >> 5), data + size, tokbuf))
        return -3;
    /* Shrunk to half or less, the loop filter's smoothing of block edges is
     * averaged away by the scaler, and it is a quarter of the decode:
     * passed over, as libwebp's bypass_filtering does */
    if (resize && width >= 2 * bm->width && height >= 2 * bm->height)
        d->filter_type = 0;

    d->extra = d->filter_type == 2 ? 8 : d->filter_type == 1 ? 2 : 0;
    d->ys = mb_w * 16;
    d->uvs = mb_w * 8;
    d->cache_y = take(&at, end, (size_t)(16 + d->extra) * d->ys);
    d->cache_u = take(&at, end, (size_t)(8 + d->extra / 2) * d->uvs);
    d->cache_v = take(&at, end, (size_t)(8 + d->extra / 2) * d->uvs);
    if (!d->cache_y || !d->cache_u || !d->cache_v)
        return -6;

    if (!image_store_rows(bm, format & FORMAT_DITHER, &src_dim, free_start,
                          free_len, cformat, 0, store_part_webp, d))
        return -9;
    return bm_size;
}

bool webp_usable(int fd)
{
    uint8_t h[10];
    off_t data, size;
    uint32_t tag;

    if (find_vp8(fd, 0, -1, &data, &size) < 0 || size < 10
        || lseek(fd, data, SEEK_SET) != data || read(fd, h, 10) != 10)
        return false;
    tag = h[0] | h[1] << 8 | h[2] << 16;
    return !(tag & 1) && ((tag >> 1) & 7) <= 3 && ((tag >> 4) & 1)
           && h[3] == 0x9d && h[4] == 0x01 && h[5] == 0x2a
           && (h[6] | h[7] << 8) & 0x3fff && (h[8] | h[9] << 8) & 0x3fff;
}
