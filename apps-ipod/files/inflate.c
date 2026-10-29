/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Streaming DEFLATE and zlib decompressor, used by the zip-backed document
 * formats, the PDF reader and PNG.
 *
 * A code is found by one lookup of the next TBITS bits in a table built for
 * each block; only a code longer than that is read a bit at a time, against
 * the canonical counts that also validate the block's code lengths. On the
 * players inflate_fast()'s loop is inflate_arm.S.
 ****************************************************************************/

#include <stddef.h>   /* offsetof */
#include <string.h>
#include "config.h"   /* CPU_ARM */
#include "byte_stream.h"

#define WBITS 15
#define WSIZE (1u << WBITS)
#define WMASK (WSIZE - 1)
#define INBUF 4096

enum { ST_HDR, ST_STORED, ST_BLOCK, ST_DONE };

/* A table entry: the symbol << 4 | its code's length, or 0 for a code
 * longer than TBITS */
#define TBITS 9
#define TSIZE (1u << TBITS)

typedef struct {
    short count[16];
    short symbol[288];
    uint16_t table[TSIZE];
} huff;

typedef struct {
    bs_stream *src;
    int wrapper;

    uint8_t  in[INBUF];
    size_t   ipos, ilen;
    int      src_eof;
    bs_off_t pulled;

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
        rc = bs_pull(s->src, s->in, INBUF, &got);
        if (rc != BS_OK) { s->err = rc; return -1; }
        if (!got) { s->src_eof = 1; return -1; }
        s->ipos = 0; s->ilen = got; s->pulled += (bs_off_t)got;
    }
    return s->in[s->ipos++];
}

static int bits(inf_st *s, int need)
{
    uint32_t val = s->bitbuf;

    while (s->bitcnt < need) {
        int b = nextbyte(s);
        if (b < 0) { if (!s->err) s->err = BS_ERR_FORMAT; return -1; }
        val |= (uint32_t)b << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

/* At least `need` bits in bitbuf where the input has them; at its end,
 * fewer, and no error */
static inline void fill(inf_st *s, int need)
{
    while (s->bitcnt < need) {
        int b;

        if (s->ipos < s->ilen) b = s->in[s->ipos++];
        else if ((b = nextbyte(s)) < 0) return;
        s->bitbuf |= (uint32_t)b << s->bitcnt;
        s->bitcnt += 8;
    }
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

/* The lookup table for the codes construct() has just counted: each code of
 * TBITS or fewer bits, reversed as it arrives, fills every entry whose low
 * bits it matches */
static void build_table(huff *h, const short *length, int n)
{
    unsigned next[16];
    unsigned code = 0;
    int len, symbol;

    memset(h->table, 0, sizeof h->table);
    for (len = 1; len < 16; len++) {
        next[len] = code;
        code = (code + h->count[len]) << 1;
    }
    for (symbol = 0; symbol < n; symbol++) {
        unsigned c, rev = 0, i;
        int l = length[symbol];

        if (!l) continue;
        c = next[l]++;
        if (l > TBITS) continue;
        for (i = 0; i < (unsigned)l; i++) rev |= ((c >> i) & 1) << (l - 1 - i);
        for (i = rev; i < TSIZE; i += 1u << l)
            h->table[i] = (uint16_t)(symbol << 4 | l);
    }
}

/* A code a bit at a time: one longer than TBITS, or one at the end of the
 * input */
static int decode_slow(inf_st *s, const huff *h)
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
    s->err = BS_ERR_FORMAT;
    return -1;
}

static inline int decode(inf_st *s, const huff *h)
{
    unsigned e;

    fill(s, TBITS);
    e = h->table[s->bitbuf & (TSIZE - 1)];
    if (e && (int)(e & 15) <= s->bitcnt) {
        s->bitbuf >>= e & 15;
        s->bitcnt -= e & 15;
        return (int)(e >> 4);
    }
    return decode_slow(s, h);
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
    build_table(&s->lencode, len, 288);

    for (i = 0; i < 30; i++) len[i] = 5;
    construct(&s->distcode, len, 30);
    build_table(&s->distcode, len, 30);
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
    if (nlen > 286 || ndist > 30) { s->err = BS_ERR_FORMAT; return -1; }

    for (index = 0; index < ncode; index++) {
        int v = bits(s, 3);
        if (v < 0) return -1;
        lengths[order[index]] = (short)v;
    }
    for (; index < 19; index++) lengths[order[index]] = 0;

    err = construct(&s->lencode, lengths, 19);
    if (err) { s->err = BS_ERR_FORMAT; return -1; }
    build_table(&s->lencode, lengths, 19);

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
            if (!index) { s->err = BS_ERR_FORMAT; return -1; }
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
        if (index + symbol > nlen + ndist) { s->err = BS_ERR_FORMAT; return -1; }
        while (symbol--) lengths[index++] = (short)len;
    }
    if (!lengths[256]) { s->err = BS_ERR_FORMAT; return -1; }

    err = construct(&s->lencode, lengths, nlen);
    if (err && (err < 0 || nlen != s->lencode.count[0] + s->lencode.count[1]))
        { s->err = BS_ERR_FORMAT; return -1; }
    build_table(&s->lencode, lengths, nlen);
    err = construct(&s->distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != s->distcode.count[0] + s->distcode.count[1]))
        { s->err = BS_ERR_FORMAT; return -1; }
    build_table(&s->distcode, lengths + nlen, ndist);
    return 0;
}

/* ---- output ---------------------------------------------------------- */

static inline void put(inf_st *s, uint8_t b, uint8_t *out, size_t *n)
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

#ifdef CPU_ARM
/* The loop below in inflate_arm.S, which reads and writes this struct at
 * fixed offsets */
struct inflate_fast_regs
{
    uint32_t bb;
    int bc;
    const uint8_t *in, *in_end;
    const uint16_t *lt, *dt;
    uint8_t *win;
    uint32_t wp, end;
    const short *lbase, *lext, *dbase, *dext;
};
_Static_assert(offsetof(struct inflate_fast_regs, wp) == 28
               && offsetof(struct inflate_fast_regs, lbase) == 36
               && offsetof(struct inflate_fast_regs, dext) == 48,
               "inflate_arm.S reads struct inflate_fast_regs by these offsets");

int inflate_fast_arm(struct inflate_fast_regs *f);
#endif

/* Literals and whole matches while there is room in `out` for the longest
 * match and input for the longest symbol, with the decoder's state in
 * locals. It stops before a code longer than TBITS, the end of the block or
 * a bad symbol, leaving that one to the loop in inf_pull(). The bytes go to
 * the window alone, and are copied out at the end, so a call writes less
 * than half a window. BS_OK, or an error. */
static int inflate_fast(inf_st *s, uint8_t *out, size_t *outn, size_t cap)
{
    const uint16_t *lt = s->lencode.table, *dt = s->distcode.table;
    const uint8_t *in = s->in + s->ipos, *in_end = s->in + s->ilen - 8;
    uint8_t *win = s->win;
    uint32_t bb = s->bitbuf, wp = s->wpos, start = wp, end;
    int bc = s->bitcnt, rc = BS_OK;
    size_t room = cap - *outn;

    if (room > WSIZE / 2)
        room = WSIZE / 2;
    end = start + (uint32_t)room - 258;
#ifdef CPU_ARM
    {
        struct inflate_fast_regs f = {
            bb, bc, in, in_end, lt, dt, win, wp, end,
            lbase, lext, dbase, dext,
        };

        rc = inflate_fast_arm(&f);
        bb = f.bb;
        bc = f.bc;
        in = f.in;
        wp = f.wp;
    }
#else
    while ((int32_t)(end - wp) >= 0 && in <= in_end) {
        unsigned e, sym, l, le, len, d, dl, de, dist;

        while (bc <= 24) {
            bb |= (uint32_t)*in++ << bc;
            bc += 8;
        }
        e = lt[bb & (TSIZE - 1)];
        if (!e)
            break;
        sym = e >> 4;
        l = e & 15;
        if (sym < 256) {
            bb >>= l;
            bc -= l;
            win[wp++ & WMASK] = (uint8_t)sym;
            continue;
        }
        if (sym == 256 || sym - 257 >= 29)
            break;
        sym -= 257;

        /* the distance code is looked at before the length is taken, so a
         * long one stops the loop with nothing consumed */
        le = lext[sym];
        e = dt[(bb >> (l + le)) & (TSIZE - 1)];
        if (!e)
            break;
        len = lbase[sym] + ((bb >> l) & ((1u << le) - 1));
        d = e >> 4;
        dl = e & 15;
        if (d >= 30) {
            rc = BS_ERR_FORMAT;
            break;
        }
        bb >>= l + le + dl;
        bc -= l + le + dl;
        while (bc <= 24) {
            bb |= (uint32_t)*in++ << bc;
            bc += 8;
        }
        de = dext[d];
        dist = dbase[d] + (bb & ((1u << de) - 1));
        bb >>= de;
        bc -= de;
        if (dist > wp) {
            rc = BS_ERR_FORMAT;
            break;
        }
        {
            unsigned to = wp & WMASK, from = (wp - dist) & WMASK;

            wp += len;
            if (to + len <= WSIZE && from + len <= WSIZE) {
                uint8_t *dst = win + to;
                const uint8_t *src = win + from;

                /* forwards a byte at a time, so a match that overlaps the
                 * bytes it is writing repeats them */
                do *dst++ = *src++; while (--len);
            } else {
                do {
                    win[to] = win[from];
                    to = (to + 1) & WMASK;
                    from = (from + 1) & WMASK;
                } while (--len);
            }
        }
    }
#endif

    /* out to the caller, in two pieces where the window wrapped */
    {
        unsigned a = start & WMASK, total = wp - start;
        unsigned first = total < WSIZE - a ? total : WSIZE - a;

        memcpy(out + *outn, win + a, first);
        memcpy(out + *outn + first, win, total - first);
        *outn += total;
    }
    s->bitbuf = bb;
    s->bitcnt = bc;
    s->wpos = wp;
    s->ipos = (size_t)(in - s->in);
    return rc;
}

static int inf_pull(bs_stream *st, uint8_t *out, size_t cap, size_t *outn)
{
    inf_st *s = st->st;
    size_t n = 0;

    if (s->err) return s->err;

    if (!s->started) {
        s->started = 1;
        if (s->wrapper == BS_INFLATE_ZLIB) {
            int cmf = nextbyte(s), flg = nextbyte(s);
            if (cmf < 0 || flg < 0) return s->err ? s->err : BS_ERR_FORMAT;
            if ((cmf & 0x0F) != 8 || ((cmf << 8 | flg) % 31)) return BS_ERR_FORMAT;
            if (flg & 0x20) { int i; for (i = 0; i < 4; i++) nextbyte(s); }
        }
    }

    while (n < cap && s->state != ST_DONE) {
        switch (s->state) {

        case ST_HDR: {
            int type;
            s->last = bits(s, 1);
            type = bits(s, 2);
            if (s->last < 0 || type < 0) return s->err ? s->err : BS_ERR_FORMAT;

            if (type == 0) {
                int i, b[4], len, nlen;
                /* to the byte edge: whole bytes read ahead stay in bitbuf */
                s->bitbuf >>= s->bitcnt & 7;
                s->bitcnt &= ~7;
                /* Each byte is checked before use: bits() returns -1 at
                 * EOF, and shifting that left is undefined. A truncated file
                 * must be an error, not a leap into the unknown. */
                for (i = 0; i < 4; i++)
                    if ((b[i] = bits(s, 8)) < 0)
                        return s->err ? s->err : BS_ERR_FORMAT;
                len  = b[0] | (b[1] << 8);
                nlen = b[2] | (b[3] << 8);
                if (s->err) return s->err;
                if ((len & 0xFFFF) != ((~nlen) & 0xFFFF)) return BS_ERR_FORMAT;
                s->stored_left = (unsigned)(len & 0xFFFF);
                s->state = ST_STORED;
                (void)i;
            } else if (type == 1) {
                fixed_tables(s);
                s->state = ST_BLOCK;
            } else if (type == 2) {
                if (dynamic_tables(s) < 0) return s->err ? s->err : BS_ERR_FORMAT;
                s->state = ST_BLOCK;
            } else {
                return BS_ERR_FORMAT;
            }
            break;
        }

        case ST_STORED:
            while (n < cap && s->stored_left) {
                int b = s->bitcnt ? bits(s, 8) : nextbyte(s);
                if (b < 0) return s->err ? s->err : BS_ERR_FORMAT;
                put(s, (uint8_t)b, out, &n);
                s->stored_left--;
            }
            if (!s->stored_left) s->state = s->last ? ST_DONE : ST_HDR;
            break;

        case ST_BLOCK:
            while (n < cap) {
                int symbol;

                if (!s->copy_len && cap - n >= 258 && s->ilen - s->ipos >= 8) {
                    int rc = inflate_fast(s, out, &n, cap);
                    if (rc < 0) return rc;
                    if (n >= cap) break;
                }
                if (s->copy_len) {              /* resume an interrupted match */
                    while (n < cap && s->copy_len) {
                        put(s, s->win[(s->wpos - s->copy_dist) & WMASK], out, &n);
                        s->copy_len--;
                    }
                    if (s->copy_len) break;
                    continue;
                }

                symbol = decode(s, &s->lencode);
                if (symbol < 0) return s->err ? s->err : BS_ERR_FORMAT;

                if (symbol < 256) {
                    put(s, (uint8_t)symbol, out, &n);
                    continue;
                }
                if (symbol == 256) {
                    s->state = s->last ? ST_DONE : ST_HDR;
                    break;
                }
                symbol -= 257;
                if (symbol >= 29) return BS_ERR_FORMAT;
                {
                    int e = bits(s, lext[symbol]);
                    int d;
                    if (e < 0) return s->err ? s->err : BS_ERR_FORMAT;
                    s->copy_len = (unsigned)(lbase[symbol] + e);

                    d = decode(s, &s->distcode);
                    if (d < 0 || d >= 30) return s->err ? s->err : BS_ERR_FORMAT;
                    e = bits(s, dext[d]);
                    if (e < 0) return s->err ? s->err : BS_ERR_FORMAT;
                    s->copy_dist = (unsigned)(dbase[d] + e);
                    if (s->copy_dist > s->wpos) return BS_ERR_FORMAT;
                }
            }
            break;
        }
    }

    *outn = n;
    return BS_OK;
}

static int inf_reset(bs_stream *st)
{
    inf_st *s = st->st;
    int rc = s->src->reset ? s->src->reset(s->src) : BS_ERR_UNSUP;

    if (rc != BS_OK) return rc;
    bs_inflate_restart(st, s->wrapper);
    return BS_OK;
}

bs_stream *bs_inflate_stream(bs_arena *a, bs_stream *src, int wrapper)
{
    bs_stream *st = bs_alloc(a, sizeof *st);
    inf_st *s = bs_alloc(a, sizeof *s);

    if (!st || !s || !src) return NULL;
    s->src = src;
    s->wrapper = wrapper;
    st->pull = inf_pull; st->reset = inf_reset; st->st = s;
    return st;
}

int bs_inflate_finished(const bs_stream *st)
{
    const inf_st *s = st->st;
    return s->state == ST_DONE || s->err;
}

bs_off_t bs_inflate_src_used(const bs_stream *st)
{
    const inf_st *s = st->st;
    bs_off_t buffered = (bs_off_t)(s->ilen - s->ipos) + s->bitcnt / 8;
    return s->pulled - buffered;
}

/* Re-point an existing inflate stream at a fresh deflate stream without
 * touching the arena. Used by the PDF scanner, which walks many streams.
 *
 * The 32 KiB window is deliberately left alone: a match can only reach back
 * over bytes this stream has already written (copy_dist > wpos is rejected),
 * so stale history is unreachable. Clearing it would memset 32 KiB per
 * stream, which on a document with hundreds of streams is pure waste. */
void bs_inflate_restart(bs_stream *st, int wrapper)
{
    inf_st *s = st->st;
    bs_stream *src = s->src;

    memset(s, 0, offsetof(inf_st, win));
    s->wpos = 0;
    s->state = ST_HDR;
    s->last = s->started = s->err = 0;
    s->stored_left = s->copy_len = s->copy_dist = 0;
    s->src = src;
    s->wrapper = wrapper;
}
