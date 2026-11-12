/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Reads a GIF's first frame into a rockbox bitmap one row at a time, scaling
 * on load through resize.c, for album art, covers and book pictures. The
 * image viewer has giflib for the frames of an animated one.
 *
 *   header - the screen, its colour table, extensions up to the first image
 *            (a graphic control extension's transparent colour kept)
 *   LZW    - the image's codes into palette indices, across sub-blocks
 *   rows   - each row of indices as RGB, handed to the scaler; an
 *            interlaced image is put together first, in the room after the
 *            bitmap, and when it is too big for that, a band of rows at a
 *            time, the image read again from its start for each band
 *
 * The frame is read at its own size, wherever it sits on the screen. A
 * transparent pixel is drawn black, as a PNG's is.
 ****************************************************************************/

#include <string.h>
#include "system.h"
#include "file.h"
#include "bmp.h"
#include "resize.h"
#include "gif_load.h"

#define LZW_MAX 4096

struct gif {
    const struct custom_format *format;
    bs_stream *in;
    int skip;                   /* bytes before the signature, after a reset */
    int w, h;
    bool interlaced;
    int trns;                   /* the transparent index, or -1 */
    uint8_t pal[256][3];
    /* LZW */
    uint16_t prefix[LZW_MAX];
    uint8_t suffix[LZW_MAX];
    uint8_t stack[LZW_MAX];
    int sp;                     /* indices waiting on the stack */
    int min_size, size, clear, next, prev;
    uint8_t first;
    uint32_t bits;
    int nbits;
    uint8_t block[255];
    int blen, bpos;
    bool ended;                 /* the end code, or the data's end */
    /* rows */
    uint8_t *idx;               /* one row, or interlaced rows band0 on */
    int band0, nband;           /* interlaced: the rows idx holds */
    struct uint8_rgb *rgb;
    int y;                      /* the next row handed out */
    struct img_part part;
};

/* ---- header ---------------------------------------------------------- */

static bool read_table(struct gif *g, int flags)
{
    int n = 2 << (flags & 7);

    memset(g->pal, 0, sizeof g->pal);
    return bs_read(g->in, g->pal, n * 3) == BS_OK;
}

static bool skip_blocks(bs_stream *in)
{
    uint8_t n;

    do
        if (bs_read(in, &n, 1) != BS_OK || bs_skip(in, n) != BS_OK)
            return false;
    while (n);
    return true;
}

/* Up to the first image's data: its size, colour table and LZW code size */
static bool read_header(struct gif *g)
{
    uint8_t b[10];

    g->trns = -1;
    if (bs_read(g->in, b, 6) != BS_OK || memcmp(b, "GIF8", 4)
        || bs_read(g->in, b, 7) != BS_OK
        || ((b[4] & 0x80) && !read_table(g, b[4])))
        return false;
    for (;;)
    {
        if (bs_read(g->in, b, 1) != BS_OK)
            return false;
        if (b[0] == 0x21)                       /* an extension */
        {
            if (bs_read(g->in, b, 1) != BS_OK)
                return false;
            if (b[0] == 0xf9)                   /* graphic control */
            {
                if (bs_read(g->in, b, 6) != BS_OK || b[0] != 4)
                    return false;
                g->trns = (b[1] & 1) ? b[4] : -1;
                /* b[5] ends it, or is the length of more */
                if (b[5] && (bs_skip(g->in, b[5]) != BS_OK
                             || !skip_blocks(g->in)))
                    return false;
            }
            else if (!skip_blocks(g->in))
                return false;
        }
        else if (b[0] == 0x2c)                  /* the image */
        {
            if (bs_read(g->in, b, 9) != BS_OK)
                return false;
            g->w = b[4] | b[5] << 8;
            g->h = b[6] | b[7] << 8;
            g->interlaced = b[8] & 0x40;
            /* 32767 keeps w * h, an interlaced image's indices, in an int */
            if (((b[8] & 0x80) && !read_table(g, b[8]))
                || bs_read(g->in, b, 1) != BS_OK
                || b[0] < 2 || b[0] > 8 || g->w < 1 || g->h < 1
                || g->w > 32767 || g->h > 32767)
                return false;
            g->min_size = b[0];
            return true;
        }
        else
            return false;                       /* the trailer, or worse */
    }
}

/* ---- LZW ------------------------------------------------------------- */

static void lzw_reset(struct gif *g)
{
    g->clear = 1 << g->min_size;
    g->size = g->min_size + 1;
    g->next = g->clear + 2;
    g->prev = -1;
}

/* The next code, or -1 at the end of the data */
static int lzw_code(struct gif *g)
{
    int code;

    while (g->nbits < g->size)
    {
        if (g->bpos == g->blen)
        {
            uint8_t n;

            if (bs_read(g->in, &n, 1) != BS_OK || !n
                || bs_read(g->in, g->block, n) != BS_OK)
                return -1;
            g->blen = n;
            g->bpos = 0;
        }
        g->bits |= (uint32_t)g->block[g->bpos++] << g->nbits;
        g->nbits += 8;
    }
    code = g->bits & ((1 << g->size) - 1);
    g->bits >>= g->size;
    g->nbits -= g->size;
    return code;
}

/* n indices into dst; past the end of the data, or a bad code, zeros */
static void lzw_pixels(struct gif *g, uint8_t *dst, int n)
{
    while (n > 0)
    {
        int code, c;

        if (g->sp)
        {
            *dst++ = g->stack[--g->sp];
            n--;
            continue;
        }
        if (g->ended || (code = lzw_code(g)) < 0 || code == g->clear + 1)
        {
            g->ended = true;
            memset(dst, 0, n);
            return;
        }
        if (code == g->clear)
        {
            lzw_reset(g);
            continue;
        }
        if (g->prev < 0)
        {
            if (code >= g->clear)
            {
                g->ended = true;
                continue;
            }
            g->prev = code;
            g->first = code;
            g->stack[g->sp++] = code;
            continue;
        }
        if (code > g->next)
        {
            g->ended = true;
            continue;
        }
        /* the string a code stands for, last index first; the code not yet
         * in the table is the last string with its own first index added */
        c = code;
        if (code == g->next)
        {
            g->stack[g->sp++] = g->first;
            c = g->prev;
        }
        while (c >= g->clear && g->sp < LZW_MAX - 1)
        {
            g->stack[g->sp++] = g->suffix[c];
            c = g->prefix[c];
        }
        g->stack[g->sp++] = c;
        g->first = c;
        if (g->next < LZW_MAX)
        {
            g->prefix[g->next] = g->prev;
            g->suffix[g->next] = g->first;
            if (++g->next == (1 << g->size) && g->size < 12)
                g->size++;
        }
        g->prev = code;
    }
}

/* ---- rows ------------------------------------------------------------ */

static void to_rgb(const struct gif *g, const uint8_t *idx)
{
    struct uint8_rgb *o = g->rgb;

    for (int x = 0; x < g->w; x++, o++)
    {
        const uint8_t *c = g->pal[idx[x]];

        o->alpha = 255;
        if (idx[x] == g->trns)
            o->red = o->green = o->blue = 0;
        else
        {
            o->red = c[0];
            o->green = c[1];
            o->blue = c[2];
        }
    }
}

static bool load_band(struct gif *g, int y0);

static struct img_part *store_part_gif(void *args)
{
    struct gif *g = args;

    if (g->y >= g->h)
        return NULL;
    if (g->interlaced)
    {
        if (g->y >= g->band0 + g->nband && !load_band(g, g->y))
            return NULL;
        to_rgb(g, g->idx + (size_t)(g->y - g->band0) * g->w);
    }
    else
    {
        lzw_pixels(g, g->idx, g->w);
        to_rgb(g, g->idx);
    }
    g->y++;
    g->part.len = g->w;
    g->part.buf = g->rgb;
    return &g->part;
}

/* Rows y0 on of an interlaced image, as many as idx holds, through its four
 * passes: each of them to its place in idx, the rest into the RGB row,
 * unused until the rows are handed out. Past the first band the image is
 * read again from its start. */
static bool load_band(struct gif *g, int y0)
{
    static const uint8_t start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
    int end = MIN(y0 + g->nband, g->h), done = 0;

    if (y0 > 0)
    {
        if (!g->in->reset || g->in->reset(g->in) != BS_OK
            || bs_skip(g->in, g->skip) != BS_OK || !read_header(g))
            return false;
        g->sp = g->nbits = g->blen = g->bpos = 0;
        g->bits = 0;
        g->ended = false;
        lzw_reset(g);
    }
    g->band0 = y0;
    for (int pass = 0; pass < 4 && done < end - y0; pass++)
        for (int y = start[pass]; y < g->h && done < end - y0;
             y += step[pass])
        {
            bool mine = y >= y0 && y < end;

            lzw_pixels(g, mine ? g->idx + (size_t)(y - y0) * g->w
                               : (uint8_t *)g->rgb, g->w);
            if (!mine)
                continue;
            done++;
            if (!image_progress(g->format, y0 + done, g->h))
                return false;
        }
    return true;
}

int read_gif_stream(bs_stream *in, int skip, bs_arena *a, struct bitmap *bm,
                    int maxsize, int format,
                    const struct custom_format *cformat)
{
    struct gif *g = bs_alloc(a, sizeof *g);
    struct dim src_dim;
    unsigned char *free_start;
    int bm_size, free_len, lines;

    if (!g)
        return -1;
    g->format = cformat;
    g->in = in;
    g->skip = skip;
    if (!read_header(g))
        return -3;

    src_dim.width = g->w;
    src_dim.height = g->h;
    bm_size = image_fit_bitmap(bm, &src_dim, format, cformat);

    /* the scaler's line buffers; an interlaced image's indices after them */
    lines = bm->width != src_dim.width || bm->height != src_dim.height
            ? (int)sizeof(struct uint32_argb) * 3 * bm->width : 0;
    if (format & FORMAT_RETURN_SIZE)
        return bm_size + lines + sizeof(long) + (int)a->size
               + (g->interlaced ? g->w * g->h : 0);

    free_start = (unsigned char *)bm->data + bm_size;
    free_len = maxsize - bm_size;
    ALIGN_BUFFER(free_start, free_len, sizeof(uint32_t));
    g->rgb = bs_alloc(a, g->w * sizeof(struct uint8_rgb));
    if (g->interlaced)
    {
        long rows = (free_len - lines) / g->w;

        if (rows < 1)
            return -6;
        g->nband = rows < g->h ? (int)rows : g->h;
        g->idx = free_start + lines;
    }
    else
        g->idx = bs_alloc(a, g->w);
    if (free_len < lines || !g->rgb || !g->idx)
        return -6;

    lzw_reset(g);
    if (g->interlaced && !load_band(g, 0))
        return -9;

    if (!image_store_rows(bm, format & FORMAT_DITHER, &src_dim, free_start,
                          free_len, cformat, 0, store_part_gif, g))
        return -9;
    return bm_size;
}

/* Past a colour table whose size `flags` gives, if it has one */
static bool skip_table(int fd, int flags)
{
    return !(flags & 0x80) || lseek(fd, 3 * (2 << (flags & 7)), SEEK_CUR) >= 0;
}

/* Past an extension's sub-blocks, up to the empty one that ends them */
static bool skip_fd_blocks(int fd)
{
    uint8_t n;

    do
        if (read(fd, &n, 1) != 1 || lseek(fd, n, SEEK_CUR) < 0)
            return false;
    while (n);
    return true;
}

/* read_header()'s walk to the first image, by seeking past what it would
 * read */
bool gif_usable(int fd)
{
    uint8_t b[13];

    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, b, 13) != 13
        || memcmp(b, "GIF8", 4) || !skip_table(fd, b[10]))
        return false;
    for (;;)
    {
        if (read(fd, b, 1) != 1)
            return false;
        if (b[0] == 0x21)                       /* an extension */
        {
            if (read(fd, b, 2) != 2 || (b[0] == 0xf9 && b[1] != 4))
                return false;
            if (lseek(fd, -1, SEEK_CUR) < 0 || !skip_fd_blocks(fd))
                return false;
        }
        else if (b[0] == 0x2c)                  /* the image */
        {
            int w, h;

            if (read(fd, b, 9) != 9)
                return false;
            w = b[4] | b[5] << 8;
            h = b[6] | b[7] << 8;
            return skip_table(fd, b[8]) && read(fd, b, 1) == 1
                   && b[0] >= 2 && b[0] <= 8 && w >= 1 && h >= 1
                   && w <= 32767 && h <= 32767;
        }
        else
            return false;                       /* the trailer, or worse */
    }
}
