/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Reads a PNG into a rockbox bitmap one row at a time, scaling on load
 * through resize.c, so its memory does not grow with the image.
 *
 *   header      - the chunks up to the first IDAT: IHDR, PLTE, tRNS
 *   IDAT stage  - a stream of the IDAT payloads alone, across chunks, which
 *                 the inflater reads
 *   rows        - each row inflated, unfiltered against the one before, and
 *                 handed to the scaler as RGB in short parts
 *
 * Chunk CRCs, the zlib checksum, gamma, colour profiles and text are not
 * read. An animated PNG shows its IDAT image, which is its first frame.
 ****************************************************************************/

#include <string.h>
#include "system.h"
#include "file.h"
#include "bmp.h"
#include "resize.h"
#include "png_load.h"

/* Pixels converted per part handed to the scaler */
#define PART_PIXELS 64

enum {
    CT_GREY       = 0,
    CT_RGB        = 2,
    CT_PALETTE    = 3,
    CT_GREY_ALPHA = 4,
    CT_RGBA       = 6,
};

static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

struct png_hdr {
    int    width, height;
    int    depth, ctype;
    int    channels;
    size_t rowbytes;        /* one row's samples, without the filter byte */
    int    bpp;             /* bytes per pixel for the filters, at least 1 */
};

struct png {
    struct png_hdr h;
    bs_stream *in;          /* the file's bytes */
    bs_stream *z;           /* the inflated rows */
    uint32_t idat_left;     /* payload left in the current IDAT */
    bool     idat_end;
    uint8_t *cur, *prev;    /* a filter byte and a row, each */
    int      x;             /* pixels of cur handed out */
    bool     has_trns;
    unsigned trns[3];       /* the transparent grey or RGB, at the depth */
    int      npal;
    uint8_t  pal_alpha[256];
    struct uint8_rgb pal[256];
    struct uint8_rgb *out;
    int      out_len;
    struct img_part part;
};

static uint32_t get32(const uint8_t *b)
{
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | b[2] << 8 | b[3];
}

/* c drawn over black at opacity a, rounded */
static inline unsigned over_black(unsigned c, unsigned a)
{
    unsigned t = c * a + 128;
    return (t + (t >> 8)) >> 8;
}

/* IHDR's 13 bytes. False for anything read_png_stream() does not decode. */
static bool parse_ihdr(const uint8_t *d, struct png_hdr *h)
{
    uint32_t w = get32(d), ht = get32(d + 4);
    int depth = d[8], ctype = d[9], bits;

    /* the scaler's dimensions are shorts */
    if (w < 1 || ht < 1 || w > 32767 || ht > 32767)
        return false;
    if (d[10] != 0 || d[11] != 0 || d[12] != 0) /* deflate, filter, no Adam7 */
        return false;
    switch (ctype)
    {
    case CT_GREY:
        if (depth != 1 && depth != 2 && depth != 4 && depth != 8
            && depth != 16)
            return false;
        h->channels = 1;
        break;
    case CT_PALETTE:
        if (depth != 1 && depth != 2 && depth != 4 && depth != 8)
            return false;
        h->channels = 1;
        break;
    case CT_RGB:
    case CT_GREY_ALPHA:
    case CT_RGBA:
        if (depth != 8 && depth != 16)
            return false;
        h->channels = ctype == CT_RGB ? 3 : ctype == CT_RGBA ? 4 : 2;
        break;
    default:
        return false;
    }
    bits = depth * h->channels;
    h->width = w;
    h->height = ht;
    h->depth = depth;
    h->ctype = ctype;
    h->rowbytes = (w * bits + 7) / 8;
    h->bpp = bits >= 8 ? bits / 8 : 1;
    return 1 + h->rowbytes <= PNG_MAX_ROW_BYTES;
}

bool png_usable(int fd)
{
    uint8_t b[29];
    struct png_hdr h;

    return lseek(fd, 0, SEEK_SET) == 0
           && read(fd, b, sizeof b) == (ssize_t)sizeof b
           && !memcmp(b, png_sig, 8)
           && get32(b + 8) == 13 && !memcmp(b + 12, "IHDR", 4)
           && parse_ihdr(b + 16, &h);
}

/* ---- IDAT stage ------------------------------------------------------ */

static int idat_pull(bs_stream *s, uint8_t *buf, size_t n, size_t *out)
{
    struct png *p = s->st;
    uint8_t hdr[12];
    int rc;

    while (!p->idat_left)
    {
        if (p->idat_end)
            return BS_OK;
        /* the CRC of the chunk just read, and the next chunk's header */
        rc = bs_read(p->in, hdr, sizeof hdr);
        if (rc != BS_OK)
            return rc;
        if (memcmp(hdr + 8, "IDAT", 4))
        {
            p->idat_end = true;
            return BS_OK;
        }
        p->idat_left = get32(hdr + 4);
    }
    if (n > p->idat_left)
        n = p->idat_left;
    rc = bs_pull(p->in, buf, n, out);
    if (rc != BS_OK)
        return rc;
    if (!*out)
        return BS_ERR_FORMAT;                   /* ends inside a chunk */
    p->idat_left -= *out;
    return BS_OK;
}

/* ---- rows ------------------------------------------------------------ */

static bool unfilter(uint8_t *cur, const uint8_t *prev, size_t len, int bpp)
{
    uint8_t *r = cur + 1;
    const uint8_t *u = prev + 1;
    size_t i;

    switch (cur[0])
    {
    case 0:
        break;
    case 1:                                     /* Sub */
        for (i = bpp; i < len; i++)
            r[i] += r[i - bpp];
        break;
    case 2:                                     /* Up */
        for (i = 0; i < len; i++)
            r[i] += u[i];
        break;
    case 3:                                     /* Average */
        for (i = 0; i < (size_t)bpp; i++)
            r[i] += u[i] >> 1;
        for (; i < len; i++)
            r[i] += (r[i - bpp] + u[i]) >> 1;
        break;
    case 4:                                     /* Paeth */
        for (i = 0; i < (size_t)bpp; i++)
            r[i] += u[i];
        for (; i < len; i++)
        {
            int a = r[i - bpp], b = u[i], c = u[i - bpp];
            int pa = b - c, pb = a - c, pc = pa + pb;

            if (pa < 0) pa = -pa;
            if (pb < 0) pb = -pb;
            if (pc < 0) pc = -pc;
            r[i] += (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
        }
        break;
    default:
        return false;
    }
    return true;
}

static bool next_row(struct png *p)
{
    uint8_t *t = p->prev;

    p->prev = p->cur;
    p->cur = t;
    p->x = 0;
    return bs_read(p->z, p->cur, 1 + p->h.rowbytes) == BS_OK
           && unfilter(p->cur, p->prev, p->h.rowbytes, p->h.bpp);
}

/* Sample c of pixel x at the image's depth: 1, 2 and 4 bits have one */
static unsigned sample(const struct png *p, int x, int c)
{
    const uint8_t *r = p->cur + 1;
    int depth = p->h.depth;

    if (depth == 8)
        return r[x * p->h.channels + c];
    if (depth == 16)
    {
        r += 2 * (x * p->h.channels + c);
        return r[0] << 8 | r[1];
    }
    x *= depth;
    return (r[x >> 3] >> (8 - depth - (x & 7))) & ((1 << depth) - 1);
}

static unsigned to8(unsigned v, int depth)
{
    switch (depth)
    {
    case 1:  return v * 255;
    case 2:  return v * 85;
    case 4:  return v * 17;
    case 16: return v >> 8;
    default: return v;
    }
}

/* n pixels of the current row from x, as RGB over black */
static void convert(const struct png *p, int x, int n, struct uint8_rgb *o)
{
    const uint8_t *r = p->cur + 1;
    int depth = p->h.depth;

    for (; n--; x++, o++)
    {
        unsigned red, green, blue, a = 255, v;

        o->alpha = 255;
        if (p->h.ctype == CT_PALETTE)
        {
            v = sample(p, x, 0);
            if ((int)v < p->npal)
                *o = p->pal[v];
            else
                o->red = o->green = o->blue = 0;
            continue;
        }
        if (depth == 8 && p->h.ctype == CT_RGB)
        {
            red = r[3 * x];
            green = r[3 * x + 1];
            blue = r[3 * x + 2];
            if (p->has_trns && red == p->trns[0] && green == p->trns[1]
                && blue == p->trns[2])
                a = 0;
        }
        else if (depth == 8 && p->h.ctype == CT_RGBA)
        {
            red = r[4 * x];
            green = r[4 * x + 1];
            blue = r[4 * x + 2];
            a = r[4 * x + 3];
        }
        else if (p->h.ctype == CT_RGB)
        {
            red = sample(p, x, 0);
            green = sample(p, x, 1);
            blue = sample(p, x, 2);
            if (p->has_trns && red == p->trns[0] && green == p->trns[1]
                && blue == p->trns[2])
                a = 0;
            red = to8(red, depth);
            green = to8(green, depth);
            blue = to8(blue, depth);
        }
        else if (p->h.ctype == CT_RGBA)
        {
            red = to8(sample(p, x, 0), depth);
            green = to8(sample(p, x, 1), depth);
            blue = to8(sample(p, x, 2), depth);
            a = to8(sample(p, x, 3), depth);
        }
        else
        {
            v = sample(p, x, 0);
            if (p->h.ctype == CT_GREY_ALPHA)
                a = to8(sample(p, x, 1), depth);
            else if (p->has_trns && v == p->trns[0])
                a = 0;
            red = green = blue = to8(v, depth);
        }
        if (a != 255)
        {
            red = over_black(red, a);
            green = over_black(green, a);
            blue = over_black(blue, a);
        }
        o->red = red;
        o->green = green;
        o->blue = blue;
    }
}

static struct img_part *store_part_png(void *args)
{
    struct png *p = args;
    int n;

    if (p->x == p->h.width && !next_row(p))
        return NULL;
    n = MIN(p->out_len, p->h.width - p->x);
    convert(p, p->x, n, p->out);
    p->x += n;
    p->part.len = n;
    p->part.buf = p->out;
    return &p->part;
}

/* ---- header ---------------------------------------------------------- */

/* The chunks before the first IDAT. Leaves p->in at its payload. */
static bool read_header(struct png *p)
{
    uint8_t b[13];
    bool ihdr = false;
    int i;

    if (bs_read(p->in, b, 8) != BS_OK || memcmp(b, png_sig, 8))
        return false;
    memset(p->pal_alpha, 255, sizeof p->pal_alpha);
    for (;;)
    {
        uint32_t len;

        if (bs_read(p->in, b, 8) != BS_OK)
            return false;
        len = get32(b);
        if (!memcmp(b + 4, "IDAT", 4))
        {
            p->idat_left = len;
            break;
        }
        if (!memcmp(b + 4, "IHDR", 4))
        {
            if (ihdr || len != 13 || bs_read(p->in, b, 13) != BS_OK
                || !parse_ihdr(b, &p->h))
                return false;
            ihdr = true;
            len = 0;
        }
        else if (!ihdr || !memcmp(b + 4, "IEND", 4))
            return false;
        else if (!memcmp(b + 4, "PLTE", 4) && len % 3 == 0 && len <= 768)
        {
            p->npal = len / 3;
            for (i = 0; i < p->npal; i++)
            {
                if (bs_read(p->in, b, 3) != BS_OK)
                    return false;
                p->pal[i].red = b[0];
                p->pal[i].green = b[1];
                p->pal[i].blue = b[2];
                p->pal[i].alpha = 255;
            }
            len = 0;
        }
        else if (!memcmp(b + 4, "tRNS", 4))
        {
            if (p->h.ctype == CT_PALETTE && len <= 256)
            {
                if (bs_read(p->in, p->pal_alpha, len) != BS_OK)
                    return false;
                len = 0;
            }
            else if ((p->h.ctype == CT_GREY && len == 2)
                     || (p->h.ctype == CT_RGB && len == 6))
            {
                if (bs_read(p->in, b, len) != BS_OK)
                    return false;
                for (i = 0; i < (int)len / 2; i++)
                    p->trns[i] = b[2 * i] << 8 | b[2 * i + 1];
                p->has_trns = true;
                len = 0;
            }
        }
        /* the rest of the chunk, and its CRC */
        if (bs_skip(p->in, (unsigned long)len + 4) != BS_OK)
            return false;
    }
    if (!ihdr || (p->h.ctype == CT_PALETTE && !p->npal))
        return false;
    for (i = 0; i < p->npal; i++)
    {
        unsigned a = p->pal_alpha[i];
        p->pal[i].red = over_black(p->pal[i].red, a);
        p->pal[i].green = over_black(p->pal[i].green, a);
        p->pal[i].blue = over_black(p->pal[i].blue, a);
    }
    return true;
}

int read_png_stream(bs_stream *in, bs_arena *a, struct bitmap *bm,
                    int maxsize, int format,
                    const struct custom_format *cformat)
{
    struct png *p = bs_alloc(a, sizeof *p);
    bs_stream *idat = bs_alloc(a, sizeof *idat);
    bool resize = format & FORMAT_RESIZE;
    bool dither = format & FORMAT_DITHER;
    struct dim src_dim;
    struct rowset rset;
    unsigned char *free_start;
    int bm_size, free_len, lines;

    if (!p || !idat)
        return -1;
    p->in = in;
    if (!read_header(p))
        return -3;

    src_dim.width = p->h.width;
    src_dim.height = p->h.height;
    if (resize)
    {
        struct dim d = { .width = bm->width, .height = bm->height };
        if (format & FORMAT_KEEP_ASPECT)
            recalc_dimension(&d, &src_dim);
        bm->width = d.width;
        bm->height = d.height;
    }
    else
    {
        bm->width = src_dim.width;
        bm->height = src_dim.height;
    }
    if (bm->width == src_dim.width && bm->height == src_dim.height)
        resize = false;
    bm->alpha_offset = 0;
    if (cformat)
        bm_size = cformat->get_size(bm);
    else
        bm_size = BM_SIZE(bm->width, bm->height, FORMAT_NATIVE, false);

    /* The scaler's line buffers, or one whole row of RGB without it */
    lines = resize ? (int)sizeof(struct uint32_argb) * 3 * bm->width
                   : (int)sizeof(struct uint8_rgb) * bm->width;
    if (format & FORMAT_RETURN_SIZE)
        return bm_size + lines + sizeof(long) + (int)a->size;

    free_start = (unsigned char *)bm->data + bm_size;
    free_len = maxsize - bm_size;
    ALIGN_BUFFER(free_start, free_len, sizeof(uint32_t));
    if (free_len < lines)
        return -6;

    idat->pull = idat_pull;
    idat->st = p;
    p->z = bs_inflate_stream(a, idat, BS_INFLATE_ZLIB);
    p->cur = bs_alloc(a, 1 + p->h.rowbytes);
    p->prev = bs_alloc(a, 1 + p->h.rowbytes);
    if (resize)
    {
        p->out = bs_alloc(a, PART_PIXELS * sizeof(struct uint8_rgb));
        p->out_len = PART_PIXELS;
    }
    else
    {
        p->out = (struct uint8_rgb *)free_start;
        p->out_len = bm->width;
    }
    if (!p->z || !p->cur || !p->prev || !p->out)
        return -1;
    p->x = p->h.width;

    if (resize)
    {
        rset.rowstart = 0;
        rset.rowstop = bm->height;
        rset.rowstep = 1;
        if (!resize_on_load(bm, dither, &src_dim, &rset, free_start, free_len,
                            cformat, IF_PIX_FMT(0,) store_part_png, p))
            return -9;
    }
    else
    {
        struct scaler_context ctx = { .bm = bm, .dither = dither };
        void (*output_row_8)(uint32_t, void *, struct scaler_context *) =
            cformat ? cformat->output_row_8 : output_row_8_native;
        int row;

        for (row = 0; row < bm->height; row++)
        {
            struct img_part *part = store_part_png(p);
            if (!part)
                return -9;
            output_row_8(row, part->buf, &ctx);
        }
    }
    return bm_size;
}
