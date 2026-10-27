/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The byte-stream primitives from byte_stream.h, except inflate
 * (inflate.c): the arena, the file stream, and the ID3 unsync and base64
 * decoding stages.
 ****************************************************************************/

#include <string.h>
#include "byte_stream.h"

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

/* ---- arena ----------------------------------------------------------- */

void *bs_alloc(bs_arena *a, size_t n)
{
    size_t align = sizeof(void *);
    size_t start = (a->used + align - 1) & ~(align - 1);

    if (n > a->size || start > a->size - n)
        return NULL;
    a->used = start + n;
    memset(a->base + start, 0, n);
    return a->base + start;
}

/* ---- file stream ----------------------------------------------------- */

typedef struct {
    const bs_io *io;
    bs_off_t start, len, pos;
} file_st;

static int file_pull(bs_stream *s, uint8_t *buf, size_t n, size_t *out)
{
    file_st *f = s->st;
    long got;

    if (f->len >= 0 && f->pos >= f->len) return BS_OK;
    if (f->len >= 0 && (bs_off_t)n > f->len - f->pos)
        n = (size_t)(f->len - f->pos);
    if (!n) return BS_OK;

    /* Always seek: several readers share one descriptor (the scanner, the
     * font pre-pass and this window all move it), so no stream may assume it
     * knows where the file is positioned. Eliding the redundant seeks is the
     * io backend's job -- it is the only layer that sees every one. */
    if (f->io->seek(f->io->ctx, f->start + f->pos, SEEK_SET) < 0)
        return BS_ERR_IO;
    got = f->io->read(f->io->ctx, buf, n);
    if (got < 0) return BS_ERR_IO;
    f->pos += got;
    *out = (size_t)got;
    return BS_OK;
}

static int file_reset(bs_stream *s)
{
    ((file_st *)s->st)->pos = 0;
    return BS_OK;
}

bs_stream *bs_file_stream(bs_arena *a, const bs_io *io,
                          bs_off_t start, bs_off_t len)
{
    bs_stream *s = bs_alloc(a, sizeof *s);
    file_st *f = bs_alloc(a, sizeof *f);

    if (!s || !f) return NULL;
    f->io = io; f->start = start; f->len = len; f->pos = 0;
    s->pull = file_pull; s->reset = file_reset; s->st = f;
    return s;
}

int bs_file_stream_retarget(bs_stream *s, bs_off_t start, bs_off_t len)
{
    file_st *f = s->st;
    if (s->pull != file_pull) return BS_ERR_INVAL;
    f->start = start; f->len = len; f->pos = 0;
    return BS_OK;
}

/* ---- ID3 unsynchronisation ------------------------------------------- */

/* Decoded in place in the caller's buffer, since output never outgrows the
 * input. The FF that ends one pull still drops a 00 that starts the next. */
typedef struct {
    bs_stream *src;
    int ff;
} unsync_st;

static int unsync_pull(bs_stream *s, uint8_t *buf, size_t n, size_t *out)
{
    unsync_st *u = s->st;
    size_t got, i, w;
    int rc;

    /* A pull of a lone 00 decodes to nothing, and an empty pull means EOF */
    do {
        rc = bs_pull(u->src, buf, n, &got);
        if (rc != BS_OK) return rc;
        if (!got) return BS_OK;
        for (i = w = 0; i < got; i++) {
            uint8_t c = buf[i];
            if (u->ff && c == 0) { u->ff = 0; continue; }
            u->ff = (c == 0xFF);
            buf[w++] = c;
        }
    } while (!w);
    *out = w;
    return BS_OK;
}

static int unsync_reset(bs_stream *s)
{
    unsync_st *u = s->st;
    int rc = u->src->reset ? u->src->reset(u->src) : BS_ERR_UNSUP;

    if (rc != BS_OK) return rc;
    u->ff = 0;
    return BS_OK;
}

bs_stream *bs_unsync_stream(bs_arena *a, bs_stream *src)
{
    bs_stream *s = bs_alloc(a, sizeof *s);
    unsync_st *u = bs_alloc(a, sizeof *u);

    if (!s || !u || !src) return NULL;
    u->src = src;
    s->pull = unsync_pull; s->reset = unsync_reset; s->st = u;
    return s;
}

/* ---- base64 ---------------------------------------------------------- */

#define B64_INBUF 256
#define B64_SKIP  -1    /* whitespace */
#define B64_PAD   -2
#define B64_BAD   -3

typedef struct {
    bs_stream *src;
    uint8_t  in[B64_INBUF];
    size_t   ipos, ilen;
    uint32_t acc;           /* sextets of the group so far */
    int      nchars;        /* how many, 0..3 */
    uint8_t  pend[3];       /* a decoded group the caller had no room for */
    int      npend, ppos;
    int      done;
} b64_st;

static int b64_value(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return B64_PAD;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return B64_SKIP;
    return B64_BAD;
}

/* The end of the data: a group of two or three characters still holds one
 * or two bytes. A lone character holds none. */
static void b64_finish(b64_st *b)
{
    b->ppos = 0;
    b->npend = 0;
    if (b->nchars == 2) {
        b->pend[b->npend++] = (uint8_t)(b->acc >> 4);
    } else if (b->nchars == 3) {
        b->pend[b->npend++] = (uint8_t)(b->acc >> 10);
        b->pend[b->npend++] = (uint8_t)(b->acc >> 2);
    }
    b->nchars = 0;
    b->done = 1;
}

static int b64_pull(bs_stream *s, uint8_t *buf, size_t n, size_t *out)
{
    b64_st *b = s->st;
    size_t w = 0;

    while (w < n) {
        int v;

        if (b->ppos < b->npend) {
            buf[w++] = b->pend[b->ppos++];
            continue;
        }
        if (b->done) break;
        if (b->ipos >= b->ilen) {
            size_t got;
            int rc = bs_pull(b->src, b->in, sizeof b->in, &got);
            if (rc != BS_OK) return rc;
            if (!got) { b64_finish(b); continue; }
            b->ipos = 0;
            b->ilen = got;
        }
        v = b64_value(b->in[b->ipos++]);
        if (v == B64_SKIP) continue;
        if (v == B64_BAD) return BS_ERR_FORMAT;
        if (v == B64_PAD) { b64_finish(b); continue; }
        b->acc = (b->acc << 6) | (uint32_t)v;
        if (++b->nchars == 4) {
            b->pend[0] = (uint8_t)(b->acc >> 16);
            b->pend[1] = (uint8_t)(b->acc >> 8);
            b->pend[2] = (uint8_t)b->acc;
            b->npend = 3;
            b->ppos = 0;
            b->nchars = 0;
            b->acc = 0;
        }
    }
    *out = w;
    return BS_OK;
}

static int b64_reset(bs_stream *s)
{
    b64_st *b = s->st;
    bs_stream *src = b->src;
    int rc = src->reset ? src->reset(src) : BS_ERR_UNSUP;

    if (rc != BS_OK) return rc;
    memset(b, 0, sizeof *b);
    b->src = src;
    return BS_OK;
}

bs_stream *bs_base64_stream(bs_arena *a, bs_stream *src)
{
    bs_stream *s = bs_alloc(a, sizeof *s);
    b64_st *b = bs_alloc(a, sizeof *b);

    if (!s || !b || !src) return NULL;
    b->src = src;
    s->pull = b64_pull; s->reset = b64_reset; s->st = b;
    return s;
}
