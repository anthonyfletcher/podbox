/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Streaming DEFLATE and zlib decompressor, used by the zip-backed document
 * formats.
 ****************************************************************************/

#include "ts_internal.h"
#include <stddef.h>   /* offsetof */

#define WBITS 15
#define WSIZE (1u << WBITS)
#define WMASK (WSIZE - 1)
#define INBUF 512

enum { ST_HDR, ST_STORED, ST_BLOCK, ST_DONE };

typedef struct {
    short count[16];
    short symbol[288];
} huff;

typedef struct {
    ts_stream *src;
    int wrapper;

    uint8_t  in[INBUF];
    size_t   ipos, ilen;
    int      src_eof;
    ts_off_t pulled;

    uint32_t bitbuf;
    int      bitcnt;

    uint8_t  win[WSIZE];
    uint32_t wpos;

    int      state, last, started, err;
    unsigned stored_left;
    unsigned copy_len, copy_dist;
    huff     lencode, distcode;
} inf_st;

/* ---- bit input ------------------------------------------------------- */

static int nextbyte(inf_st *s)
{
    if (s->ipos >= s->ilen) {
        size_t got = 0;
        int rc;
        if (s->src_eof) return -1;
        rc = ts_pull(s->src, s->in, INBUF, &got);
        if (rc != TS_OK) { s->err = rc; return -1; }
        if (!got) { s->src_eof = 1; return -1; }
        s->ipos = 0; s->ilen = got; s->pulled += (ts_off_t)got;
    }
    return s->in[s->ipos++];
}

static int bits(inf_st *s, int need)
{
    uint32_t val = s->bitbuf;

    while (s->bitcnt < need) {
        int b = nextbyte(s);
        if (b < 0) { if (!s->err) s->err = TS_ERR_FORMAT; return -1; }
        val |= (uint32_t)b << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

/* ---- huffman --------------------------------------------------------- */

static int construct(huff *h, const short *length, int n)
{
    int symbol, len, left;
    short offs[16];

    for (len = 0; len < 16; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;

    left = 1;
    for (len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;            /* over-subscribed */
    }

    offs[1] = 0;
    for (len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol]) h->symbol[offs[length[symbol]]++] = (short)symbol;
    return left;
}

static int decode(inf_st *s, const huff *h)
{
    int code = 0, first = 0, index = 0, len;

    for (len = 1; len < 16; len++) {
        int b = bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        {
            int count = h->count[len];
            if (code - count < first) return h->symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
    }
    s->err = TS_ERR_FORMAT;
    return -1;
}

static void fixed_tables(inf_st *s)
{
    short len[288];
    int i;

    for (i = 0;   i < 144; i++) len[i] = 8;
    for (; i < 256; i++)        len[i] = 9;
    for (; i < 280; i++)        len[i] = 7;
    for (; i < 288; i++)        len[i] = 8;
    construct(&s->lencode, len, 288);

    for (i = 0; i < 30; i++) len[i] = 5;
    construct(&s->distcode, len, 30);
}

static int dynamic_tables(inf_st *s)
{
    static const short order[19] =
        { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    short lengths[288 + 30];
    int nlen, ndist, ncode, index, err;

    nlen  = bits(s, 5); ndist = bits(s, 5); ncode = bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
    nlen += 257; ndist += 1; ncode += 4;
    if (nlen > 286 || ndist > 30) { s->err = TS_ERR_FORMAT; return -1; }

    for (index = 0; index < ncode; index++) {
        int v = bits(s, 3);
        if (v < 0) return -1;
        lengths[order[index]] = (short)v;
    }
    for (; index < 19; index++) lengths[order[index]] = 0;

    err = construct(&s->lencode, lengths, 19);
    if (err) { s->err = TS_ERR_FORMAT; return -1; }

    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &s->lencode), len = 0;
        if (symbol < 0) return -1;
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
            continue;
        }
        if (symbol == 16) {
            int r;
            if (!index) { s->err = TS_ERR_FORMAT; return -1; }
            len = lengths[index - 1];
            r = bits(s, 2); if (r < 0) return -1;
            symbol = 3 + r;
        } else if (symbol == 17) {
            int r = bits(s, 3); if (r < 0) return -1;
            symbol = 3 + r;
        } else {
            int r = bits(s, 7); if (r < 0) return -1;
            symbol = 11 + r;
        }
        if (index + symbol > nlen + ndist) { s->err = TS_ERR_FORMAT; return -1; }
        while (symbol--) lengths[index++] = (short)len;
    }
    if (!lengths[256]) { s->err = TS_ERR_FORMAT; return -1; }

    err = construct(&s->lencode, lengths, nlen);
    if (err && (err < 0 || nlen != s->lencode.count[0] + s->lencode.count[1]))
        { s->err = TS_ERR_FORMAT; return -1; }
    err = construct(&s->distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != s->distcode.count[0] + s->distcode.count[1]))
        { s->err = TS_ERR_FORMAT; return -1; }
    return 0;
}

/* ---- output ---------------------------------------------------------- */

static void put(inf_st *s, uint8_t b, uint8_t *out, size_t *n)
{
    s->win[s->wpos & WMASK] = b;
    s->wpos++;
    out[(*n)++] = b;
}

/* ---- main loop ------------------------------------------------------- */

static const short lbase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
    67,83,99,115,131,163,195,227,258 };
static const short lext[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short dbase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short dext[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int inf_pull(ts_stream *st, uint8_t *out, size_t cap, size_t *outn)
{
    inf_st *s = st->st;
    size_t n = 0;

    if (s->err) return s->err;

    if (!s->started) {
        s->started = 1;
        if (s->wrapper == TS_INFLATE_ZLIB) {
            int cmf = nextbyte(s), flg = nextbyte(s);
            if (cmf < 0 || flg < 0) return s->err ? s->err : TS_ERR_FORMAT;
            if ((cmf & 0x0F) != 8 || ((cmf << 8 | flg) % 31)) return TS_ERR_FORMAT;
            if (flg & 0x20) { int i; for (i = 0; i < 4; i++) nextbyte(s); }
        }
    }

    while (n < cap && s->state != ST_DONE) {
        switch (s->state) {

        case ST_HDR: {
            int type;
            s->last = bits(s, 1);
            type = bits(s, 2);
            if (s->last < 0 || type < 0) return s->err ? s->err : TS_ERR_FORMAT;

            if (type == 0) {
                int i, b[4], len, nlen;
                s->bitbuf = 0; s->bitcnt = 0;       /* discard to byte edge */
                /* Each byte is checked before use: nextbyte() returns -1 at
                 * EOF, and shifting that left is undefined. A truncated file
                 * must be an error, not a leap into the unknown. */
                for (i = 0; i < 4; i++)
                    if ((b[i] = nextbyte(s)) < 0)
                        return s->err ? s->err : TS_ERR_FORMAT;
                len  = b[0] | (b[1] << 8);
                nlen = b[2] | (b[3] << 8);
                if (s->err) return s->err;
                if ((len & 0xFFFF) != ((~nlen) & 0xFFFF)) return TS_ERR_FORMAT;
                s->stored_left = (unsigned)(len & 0xFFFF);
                s->state = ST_STORED;
                (void)i;
            } else if (type == 1) {
                fixed_tables(s);
                s->state = ST_BLOCK;
            } else if (type == 2) {
                if (dynamic_tables(s) < 0) return s->err ? s->err : TS_ERR_FORMAT;
                s->state = ST_BLOCK;
            } else {
                return TS_ERR_FORMAT;
            }
            break;
        }

        case ST_STORED:
            while (n < cap && s->stored_left) {
                int b = nextbyte(s);
                if (b < 0) return s->err ? s->err : TS_ERR_FORMAT;
                put(s, (uint8_t)b, out, &n);
                s->stored_left--;
            }
            if (!s->stored_left) s->state = s->last ? ST_DONE : ST_HDR;
            break;

        case ST_BLOCK:
            while (n < cap) {
                int symbol;

                if (s->copy_len) {              /* resume an interrupted match */
                    while (n < cap && s->copy_len) {
                        put(s, s->win[(s->wpos - s->copy_dist) & WMASK], out, &n);
                        s->copy_len--;
                    }
                    if (s->copy_len) break;
                    continue;
                }

                symbol = decode(s, &s->lencode);
                if (symbol < 0) return s->err ? s->err : TS_ERR_FORMAT;

                if (symbol < 256) {
                    put(s, (uint8_t)symbol, out, &n);
                    continue;
                }
                if (symbol == 256) {
                    s->state = s->last ? ST_DONE : ST_HDR;
                    break;
                }
                symbol -= 257;
                if (symbol >= 29) return TS_ERR_FORMAT;
                {
                    int e = bits(s, lext[symbol]);
                    int d;
                    if (e < 0) return s->err ? s->err : TS_ERR_FORMAT;
                    s->copy_len = (unsigned)(lbase[symbol] + e);

                    d = decode(s, &s->distcode);
                    if (d < 0 || d >= 30) return s->err ? s->err : TS_ERR_FORMAT;
                    e = bits(s, dext[d]);
                    if (e < 0) return s->err ? s->err : TS_ERR_FORMAT;
                    s->copy_dist = (unsigned)(dbase[d] + e);
                    if (s->copy_dist > s->wpos) return TS_ERR_FORMAT;
                }
            }
            break;
        }
    }

    *outn = n;
    return TS_OK;
}

static int inf_reset(ts_stream *st)
{
    inf_st *s = st->st;
    int rc = s->src->reset ? s->src->reset(s->src) : TS_ERR_UNSUP;

    if (rc != TS_OK) return rc;
    ts_inflate_restart(st, s->wrapper);
    return TS_OK;
}

ts_stream *ts_inflate_stream(ts_arena *a, ts_stream *src, int wrapper)
{
    ts_stream *st = ts_alloc(a, sizeof *st);
    inf_st *s = ts_alloc(a, sizeof *s);

    if (!st || !s || !src) return NULL;
    s->src = src;
    s->wrapper = wrapper;
    st->pull = inf_pull; st->reset = inf_reset; st->st = s;
    return st;
}

int ts_inflate_finished(const ts_stream *st)
{
    const inf_st *s = st->st;
    return s->state == ST_DONE || s->err;
}

ts_off_t ts_inflate_src_used(const ts_stream *st)
{
    const inf_st *s = st->st;
    ts_off_t buffered = (ts_off_t)(s->ilen - s->ipos) + s->bitcnt / 8;
    return s->pulled - buffered;
}

/* Re-point an existing inflate stream at a fresh deflate stream without
 * touching the arena. Used by the PDF scanner, which walks many streams.
 *
 * The 32 KiB window is deliberately left alone: a match can only reach back
 * over bytes this stream has already written (copy_dist > wpos is rejected),
 * so stale history is unreachable. Clearing it would memset 32 KiB per
 * stream, which on a document with hundreds of streams is pure waste. */
void ts_inflate_restart(ts_stream *st, int wrapper)
{
    inf_st *s = st->st;
    ts_stream *src = s->src;

    memset(s, 0, offsetof(inf_st, win));
    s->wpos = 0;
    s->state = ST_HDR;
    s->last = s->started = s->err = 0;
    s->stored_left = s->copy_len = s->copy_dist = 0;
    s->src = src;
    s->wrapper = wrapper;
}
