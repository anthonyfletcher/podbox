/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Text extraction from PDF: object parsing, stream filters, content-stream
 * tokenising and text positioning.
 ****************************************************************************/

#include "ts_internal.h"

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

#define SBUF  512
#define DICT  768      /* bytes of context kept before the "stream" keyword */
#define DBUF  256
#define STRB  256

typedef struct {
    const ts_io *io;
    ts_stream   *win;      /* file window over the stream body   */
    ts_stream   *filt;     /* ASCII85 / ASCIIHex de-armouring    */
    ts_stream   *infl;

    /* raw scanner */
    uint8_t  sbuf[SBUF];
    ts_off_t sbase;
    size_t   slen, spos;
    int      s_eof;

    uint8_t  dict[DICT];
    size_t   dpos, dfill;

    /* current stream */
    int      in_stream, flate, tried_raw;
    ts_off_t data_off, data_len;
    uint8_t  dbuf[DBUF];
    size_t   dblen, dbpos;

    /* fonts */
    ts_pdffonts *fonts;
    const void  *cmap;          /* current font's /ToUnicode, or NULL */
    int          cur_obj;
    char         last_name[16];

    /* marked content: /Span <</ActualText (l)>> BDC ... EMC */
    int      dict_depth, pending_lt, pending_gt;
    int      mc_depth, at_depth, at_active;
    uint8_t  at_buf[64];
    size_t   at_len;

    /* tokenizer */
    int      in_text, depth, esc, hex_hi, tok_len, dirty;
    long     ops[6];            /* recent numeric operands, x1000 */
    int      nops;
    long     cur_x, cur_y, line_x, line_y, pen_x, scale, tm_a, tf_size;
    int      have_y;
    int      str_state;    /* 0 none, 1 literal, 2 hex */
    int      paren;
    char     tok[16];
    uint8_t  str[STRB];
    size_t   str_len;
    char     num[32];
    size_t   num_len;

    ts_pend  out;
} pdf_st;

/* ---- raw scanning ---------------------------------------------------- */

static int scan_refill(pdf_st *s)
{
    long got;

    if (s->spos < s->slen) return 1;
    if (s->s_eof) return 0;
    s->sbase += (ts_off_t)s->slen;
    s->spos = s->slen = 0;
    if (s->io->seek(s->io->ctx, s->sbase, SEEK_SET) < 0) return -1;
    got = s->io->read(s->io->ctx, s->sbuf, SBUF);
    if (got < 0) return -1;
    if (!got) { s->s_eof = 1; return 0; }
    s->slen = (size_t)got;
    return 1;
}

static int scan_byte(pdf_st *s, uint8_t *b)
{
    int rc = scan_refill(s);
    if (rc <= 0) return rc;
    *b = s->sbuf[s->spos++];
    s->dict[s->dpos++ % DICT] = *b;
    if (s->dfill < DICT) s->dfill++;
    return 1;
}

static ts_off_t scan_tell(pdf_st *s) { return s->sbase + (ts_off_t)s->spos; }

static void scan_seek(pdf_st *s, ts_off_t pos)
{
    s->sbase = pos;
    s->spos = s->slen = 0;
    s->s_eof = 0;
    s->dpos = s->dfill = 0;
}

/* Flattens the circular context buffer so plain string searches work. */
static void dict_linear(pdf_st *s, char *out)
{
    size_t i, start = (s->dpos >= s->dfill) ? s->dpos - s->dfill : 0;
    for (i = 0; i < s->dfill; i++)
        out[i] = (char)s->dict[(start + i) % DICT];
    out[s->dfill] = 0;
}

/* Narrows the raw context down to the dictionary of the object we are
 * standing in, and rejects anything that isn't one.
 *
 * Both halves matter. Without the trailing ">>" test, the literal "stream"
 * turns up inside ASCII85 bodies (they are printable text) and the scanner
 * chases a phantom object. Without cutting back to the enclosing "obj", a
 * "/Image" or "/FontFile" belonging to some *earlier* object drifts into the
 * window and vetoes a perfectly good content stream. */
static char *dict_of(char *lin, int *obj_no)
{
    char *end = lin + strlen(lin);
    char *q, *start = lin;

    *obj_no = -1;

    while (end > lin && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
        end--;
    if (end - lin < 6 || memcmp(end - 6, "stream", 6)) return NULL;
    end -= 6;
    while (end > lin && (end[-1] == ' ' || end[-1] == '\t' ||
                         end[-1] == '\n' || end[-1] == '\r'))
        end--;
    if (end - lin < 2 || end[-1] != '>' || end[-2] != '>') return NULL;
    *end = 0;

    for (q = lin; (q = strstr(q, "obj")) != NULL; q += 3) {
        char *p;
        long n = 0, mul = 1;

        if (q != lin && q[-1] == 'd') continue;  /* the "obj" of "endobj" */
        start = q + 3;

        /* walk back over " 0 obj" to recover the object number, which is how
         * a content stream is matched to the page that references it */
        if (q == lin) continue;              /* nothing in front of "obj" */
        p = q - 1;
        while (p > lin && (*p == ' ' || *p == '\r' || *p == '\n')) p--;
        while (p > lin && *p >= '0' && *p <= '9') p--;       /* generation */
        while (p > lin && (*p == ' ' || *p == '\r' || *p == '\n')) p--;
        if (p >= lin && *p >= '0' && *p <= '9') {
            while (p >= lin && *p >= '0' && *p <= '9') {
                n += (long)(*p - '0') * mul;
                mul *= 10;
                p--;
            }
            *obj_no = (int)n;
        }
    }
    return strstr(start, "<<") ? start : NULL;
}

static long dict_int(const char *d, const char *key)
{
    const char *p = strstr(d, key);
    long v = 0;
    int seen = 0;

    if (!p) return -1;
    p += strlen(key);
    while (*p == ' ' || *p == '\r' || *p == '\n') p++;
    for (; *p >= '0' && *p <= '9'; p++) { v = v * 10 + (*p - '0'); seen = 1; }
    /* an indirect reference ("/Length 12 0 R") is no use to us here */
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') return -1;
    return seen ? v : -1;
}

/* ---- pre-filter stage ------------------------------------------------
 *
 * Content streams are often wrapped in an ASCII armour before compression
 * ("/Filter [ /ASCII85Decode /FlateDecode ]" is what reportlab emits by
 * default), so the bytes reaching the inflater must be de-armoured first.
 * This stage also counts how much of the file it has eaten, which is how the
 * scanner knows where to resume when a stream's /Length was an indirect
 * reference. */

typedef struct {
    ts_stream *src;
    int      mode, eof, begun, held_lt;
    uint8_t  in[256];
    size_t   ipos, ilen;
    ts_off_t pulled;
    uint32_t tuple;
    int      count;
    ts_pend  out;
} filt_st;

static int filt_byte(filt_st *s, uint8_t *b)
{
    if (s->ipos >= s->ilen) {
        size_t got = 0;
        int rc;
        s->ipos = s->ilen = 0;
        rc = ts_pull(s->src, s->in, sizeof s->in, &got);
        if (rc != TS_OK) return rc;
        if (!got) { s->eof = 1; return 0; }
        s->ilen = got;
        s->pulled += (ts_off_t)got;
    }
    *b = s->in[s->ipos++];
    return 1;
}

static void a85_flush(filt_st *s, int partial)
{
    uint8_t t[4];
    int i;

    if (s->count <= 1) { s->count = 0; return; }
    for (i = s->count; i < 5; i++) s->tuple = s->tuple * 85 + 84;  /* pad 'u' */
    t[0] = (uint8_t)(s->tuple >> 24); t[1] = (uint8_t)(s->tuple >> 16);
    t[2] = (uint8_t)(s->tuple >> 8);  t[3] = (uint8_t)s->tuple;
    ts_emit(&s->out, t, partial ? (size_t)(s->count - 1) : 4);
    s->tuple = 0;
    s->count = 0;
}

static int filt_pull(ts_stream *st, uint8_t *buf, size_t n, size_t *out)
{
    filt_st *s = st->st;
    size_t done = 0;

    while (done < n) {
        uint8_t c = 0;
        int rc;

        done += ts_pend_take(&s->out, buf + done, n - done);
        if (done == n) break;
        if (s->eof) break;

        if (s->mode == TS_FILT_RAW) {              /* straight copy, no buffering */
            size_t got = 0;
            rc = ts_pull(s->src, buf + done, n - done, &got);
            if (rc != TS_OK) return rc;
            if (!got) { s->eof = 1; break; }
            s->pulled += (ts_off_t)got;
            done += got;
            continue;
        }

        if (ts_pend_free(&s->out) < 8) continue;

        rc = filt_byte(s, &c);
        if (rc < 0) return rc;
        if (rc == 0) { a85_flush(s, 1); break; }

        if (s->mode == TS_FILT_A85) {
            /* A leading "<~" is optional armour. Only the very first byte can
             * start one: '<' is 0x3C, a perfectly ordinary ASCII85 data
             * character, so it must not be dropped anywhere else. */
            if (s->held_lt) {
                s->held_lt = 0;
                if (c == '~') continue;               /* it was "<~" after all */
                s->tuple = s->tuple * 85 + (uint32_t)('<' - '!');
                if (++s->count == 5) a85_flush(s, 0);
            }
            if (!s->begun && c == '<') { s->begun = 1; s->held_lt = 1; continue; }
            s->begun = 1;

            if (c == '~') { a85_flush(s, 1); s->eof = 1; continue; }
            if (c == 'z' && !s->count) { uint8_t z[4] = {0,0,0,0}; ts_emit(&s->out, z, 4); continue; }
            if (c < '!' || c > 'u') continue;         /* whitespace and junk */
            s->tuple = s->tuple * 85 + (uint32_t)(c - '!');
            if (++s->count == 5) a85_flush(s, 0);
        } else {                                       /* TS_FILT_AHX */
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (c == '>') {
                if (s->count) { uint8_t b = (uint8_t)(s->tuple << 4); ts_emit(&s->out, &b, 1); }
                s->eof = 1;
                continue;
            }
            if (d < 0) continue;
            if (!s->count) { s->tuple = (uint32_t)d; s->count = 1; }
            else { uint8_t b = (uint8_t)((s->tuple << 4) | (uint32_t)d);
                   ts_emit(&s->out, &b, 1); s->count = 0; }
        }
    }
    *out = done;
    return TS_OK;
}

void ts_filt_restart(ts_stream *st, int mode)
{
    filt_st *s = st->st;
    ts_stream *src = s->src;

    memset(s, 0, sizeof *s);
    s->src = src;
    s->mode = mode;
}

/* Bytes of the file consumed, ignoring what is still sitting in our buffer. */
ts_off_t ts_filt_used(ts_stream *st)
{
    filt_st *s = st->st;
    return s->pulled - (ts_off_t)(s->ilen - s->ipos);
}

ts_stream *ts_filt_stream(ts_arena *a, ts_stream *src)
{
    ts_stream *st = ts_alloc(a, sizeof *st);
    filt_st *s = ts_alloc(a, sizeof *s);

    if (!st || !s) return NULL;
    s->src = src;
    st->pull = filt_pull; st->reset = NULL; st->st = s;
    return st;
}

/* ---- text encoding --------------------------------------------------- */

/* Advances the estimated pen by a drawn run, so the next repositioning can be
 * judged as "follows on" or "new run over there". */
static void pen_advance(pdf_st *s, size_t nglyphs)
{
    s->pen_x = s->cur_x + (long)nglyphs * s->scale / 2;
}

/* ActualText is the spec's own escape hatch: "whatever glyphs follow, the
 * text is really this". Generators reach for it exactly when their own
 * encoding can't express a character -- Chrome wraps every glyph its CMap
 * fails to map, so honouring it recovers text that is otherwise lost. */
static void emit_actual(pdf_st *s)
{
    size_t i;

    if (!s->at_len) return;

    if (s->at_len >= 2 && s->at_buf[0] == 0xFE && s->at_buf[1] == 0xFF) {
        for (i = 2; i + 1 < s->at_len; i += 2) {
            uint32_t u = (uint32_t)(s->at_buf[i] << 8 | s->at_buf[i + 1]);
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < s->at_len) {
                uint32_t lo = (uint32_t)(s->at_buf[i + 2] << 8 | s->at_buf[i + 3]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            ts_emit_cp(&s->out, u);
        }
    } else {
        for (i = 0; i < s->at_len; i++)
            ts_emit_cp(&s->out, ts_sb_to_cp(TS_CS_CP1252, s->at_buf[i]));
    }
    s->dirty = 1;
}

static void put_str(pdf_st *s)
{
    size_t i = 0;

    /* Inside << >> the string is a dictionary value, not page text. */
    if (s->dict_depth) {
        if (!strcmp(s->last_name, "ActualText")) {
            s->at_len = s->str_len < sizeof s->at_buf ? s->str_len : sizeof s->at_buf;
            memcpy(s->at_buf, s->str, s->at_len);
        }
        s->str_len = 0;
        return;
    }

    if (!s->in_text || !s->str_len) { s->str_len = 0; return; }

    /* The span's ActualText has already been emitted; these glyphs are the
     * unreliable rendering of it, so skip them -- but still let the pen
     * advance, or the word-gap logic loses its place. */
    if (s->at_active) {
        int w = s->cmap ? ts_cmap_bytes(s->cmap) : 1;
        pen_advance(s, s->str_len / (size_t)(w < 1 ? 1 : w));
        s->str_len = 0;
        return;
    }

    /* Subset font: the codes mean nothing without the font's own CMap. */
    if (s->cmap) {
        int w = ts_cmap_bytes(s->cmap);
        for (i = 0; i + (size_t)w <= s->str_len; i += (size_t)w) {
            uint32_t code = s->str[i], u0, u1;
            if (w == 2) code = (code << 8) | s->str[i + 1];
            /* An unmapped code is a hole in the font's CMap -- the character
             * simply isn't recoverable, so emit nothing rather than noise. */
            if (ts_cmap_map(s->cmap, code, &u0, &u1)) {
                if (u0) ts_emit_cp(&s->out, u0);
                if (u1) ts_emit_cp(&s->out, u1);
            }
        }
        pen_advance(s, s->str_len / (size_t)(w < 1 ? 1 : w));
        s->dirty = 1;
        s->str_len = 0;
        return;
    }

    pen_advance(s, s->str_len);

    if (s->str_len >= 2 && s->str[0] == 0xFE && s->str[1] == 0xFF) {
        for (i = 2; i + 1 < s->str_len; i += 2) {
            uint32_t u = (uint32_t)(s->str[i] << 8 | s->str[i + 1]);
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < s->str_len) {
                uint32_t lo = (uint32_t)(s->str[i + 2] << 8 | s->str[i + 3]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            ts_emit_cp(&s->out, u);
        }
    } else {
        for (i = 0; i < s->str_len; i++)
            ts_emit_cp(&s->out, ts_sb_to_cp(TS_CS_CP1252, s->str[i]));
    }
    s->dirty = 1;
    s->str_len = 0;
}

static void str_put(pdf_st *s, uint8_t b)
{
    if (s->str_len < STRB) s->str[s->str_len++] = b;
}

/* ---- content tokenizer ----------------------------------------------- */

/* Only break a line that has something on it: text objects are full of
 * positioning operators that would otherwise each cost a blank line. */
static void nl(pdf_st *s)
{
    if (!s->dirty) return;
    ts_emit(&s->out, (const uint8_t *)"\n", 1);
    s->dirty = 0;
}

/* Text position, in thousandths of a text-space unit. Chrome places every
 * glyph with its own Td, so breaking the line on each one would give a
 * character per line: only a change of *row* is a line break. */
#define Y_EPS 500

static long yabs(long v) { return v < 0 ? -v : v; }

/* A new row is a new line. So is a jump back to the left at the same row,
 * which is how table cells and columns present themselves.
 *
 * Staying on the row but skipping ahead of where the pen should be is a word
 * gap: plenty of generators place words by coordinate and never emit a space
 * glyph at all. The pen is estimated at half the font scale per glyph, which
 * is crude, but it only has to separate "the next glyph follows on" (a gap of
 * a fraction of a scale) from "a new run starts over there" (many scales). */
static void line_at(pdf_st *s, long x, long y, int absolute)
{
    if (s->have_y) {
        if (yabs(y - s->line_y) > Y_EPS) nl(s);
        else if (absolute && x + Y_EPS < s->line_x) nl(s);
        else if (s->dirty && s->scale > 0 && x > s->pen_x + s->scale * 6 / 10)
            ts_emit(&s->out, (const uint8_t *)" ", 1);
    }
    s->line_x = x;
    s->line_y = y;
    s->have_y = 1;
}

static void op_done(pdf_st *s)
{
    s->tok[s->tok_len] = 0;
    if (!s->tok_len) return;

    if (!strcmp(s->tok, "BT")) {
        s->in_text = 1;
        /* have_y deliberately survives: generators split a single visual line
         * into several text objects, one per styled run, and breaking at each
         * BT/ET would strand punctuation on its own line. */
    } else if (!strcmp(s->tok, "ET")) {
        s->in_text = 0;
    } else if (!strcmp(s->tok, "BDC") || !strcmp(s->tok, "BMC")) {
        s->mc_depth++;
        if (s->at_len && !s->at_active) {
            emit_actual(s);
            s->at_active = 1;
            s->at_depth = s->mc_depth;
        }
        s->at_len = 0;
    } else if (!strcmp(s->tok, "EMC")) {
        if (s->at_active && s->mc_depth == s->at_depth) s->at_active = 0;
        if (s->mc_depth) s->mc_depth--;
    } else if (!strcmp(s->tok, "Tf")) {
        s->cmap = ts_pdffonts_cmap(s->fonts, s->cur_obj, s->last_name);
        if (s->nops >= 1) s->tf_size = s->ops[0];
        s->scale = (long)(((long long)s->tm_a * s->tf_size) / 1000);
        if (s->scale < 0) s->scale = -s->scale;
    } else if (!strcmp(s->tok, "Tm")) {
        if (s->nops >= 6) {                       /* a b c d e f: x=e, y=f */
            s->tm_a = s->ops[5];
            s->scale = (long)(((long long)s->tm_a * s->tf_size) / 1000);
            if (s->scale < 0) s->scale = -s->scale;
            s->cur_y = s->ops[0];
            s->cur_x = s->ops[1];
            line_at(s, s->cur_x, s->cur_y, 1);
            s->pen_x = s->cur_x;
        }
    } else if (!strcmp(s->tok, "Td") || !strcmp(s->tok, "TD")) {
        if (s->nops >= 2) {                       /* tx ty */
            s->cur_y += s->ops[0];
            s->cur_x += s->ops[1];
            line_at(s, s->cur_x, s->cur_y, 0);
            if (s->cur_x > s->pen_x) s->pen_x = s->cur_x;
        }
    } else if (!strcmp(s->tok, "T*") || !strcmp(s->tok, "'") ||
               !strcmp(s->tok, "\"")) {
        if (s->in_text) nl(s);
        s->have_y = 0;
    }
    s->tok_len = 0;
    s->nops = 0;
}

/* Parses to thousandths, so positioning stays exact without floating point
 * (many Rockbox targets have no FPU). */
static long num_parse(const char *p, size_t n)
{
    long v = 0, frac = 0, scale = 1000;
    size_t i = 0;
    int neg = 0, dot = 0;

    if (i < n && (p[i] == '-' || p[i] == '+')) neg = (p[i++] == '-');
    for (; i < n; i++) {
        if (p[i] == '.') { dot = 1; continue; }
        if (p[i] < '0' || p[i] > '9') break;
        /* Clamped, not wrapped: a damaged file can hand us a hundred digits,
         * and no page coordinate needs more than six. `long` is 32 bits on
         * most targets, so the result must stay well inside it. */
        if (!dot) { if (v < 100000) v = v * 10 + (p[i] - '0'); }
        else if (scale > 1) { scale /= 10; frac += (p[i] - '0') * scale; }
    }
    v = v * 1000 + frac;
    return neg ? -v : v;
}

static void num_done(pdf_st *s)
{
    s->num[s->num_len] = 0;
    if (!s->num_len) return;

    /* Inside a TJ array a large negative kern is a word gap. */
    if (s->depth && s->in_text && s->num[0] == '-') {
        long v = 0;
        size_t i;
        for (i = 1; i < s->num_len && s->num[i] >= '0' && s->num[i] <= '9'; i++)
            v = v * 10 + (s->num[i] - '0');
        if (v >= 100) ts_emit(&s->out, (const uint8_t *)" ", 1);
    } else if (!s->depth) {
        /* ops[0] is the most recent operand: Td is "tx ty", so ty=ops[0] and
         * tx=ops[1]; Tm is "a b c d e f", so f=ops[0] ... a=ops[5]. */
        int i;
        for (i = 5; i > 0; i--) s->ops[i] = s->ops[i - 1];
        s->ops[0] = num_parse(s->num, s->num_len);
        if (s->nops < 6) s->nops++;
    }
    s->num_len = 0;
}

static int is_delim(uint8_t c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
           c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}
static int is_ws(uint8_t c)
{ return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0; }

static void tok_feed(pdf_st *s, uint8_t c)
{
    /* "<" is ambiguous until the next byte: "<<" opens a dictionary, anything
     * else starts a hex string. */
    if (s->pending_lt) {
        s->pending_lt = 0;
        if (c == '<') {
            if (!s->dict_depth) s->at_len = 0;      /* a fresh properties dict */
            s->dict_depth++;
            return;
        }
        s->str_state = 2; s->str_len = 0; s->hex_hi = -1;
        /* fall through: c is the first hex digit */
    }
    if (s->pending_gt) {
        s->pending_gt = 0;
        if (c == '>') {
            if (s->dict_depth) s->dict_depth--;
            return;
        }
    }

    /* --- inside a literal string --- */
    if (s->str_state == 1) {
        if (s->esc == 1) {
            s->esc = 0;
            switch (c) {
            case 'n': str_put(s, '\n'); return;
            case 'r': str_put(s, '\r'); return;
            case 't': str_put(s, '\t'); return;
            case 'b': case 'f': return;
            case '\n': case '\r': return;              /* line continuation */
            default:
                if (c >= '0' && c <= '7') { s->hex_hi = c - '0'; s->esc = 2; return; }
                str_put(s, c);
                return;
            }
        }
        if (s->esc == 2) {                              /* octal escape */
            if (c >= '0' && c <= '7') { s->hex_hi = s->hex_hi * 8 + (c - '0'); return; }
            str_put(s, (uint8_t)s->hex_hi);
            s->esc = 0;
            /* fall through to normal handling of c */
        }
        if (c == '\\') { s->esc = 1; return; }
        if (c == '(') { s->paren++; str_put(s, c); return; }
        if (c == ')') {
            if (s->paren) { s->paren--; str_put(s, c); return; }
            s->str_state = 0;
            put_str(s);
            return;
        }
        str_put(s, c);
        return;
    }

    /* --- inside a hex string --- */
    if (s->str_state == 2) {
        int d = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (c == '>') {
            if (s->hex_hi >= 0) str_put(s, (uint8_t)(s->hex_hi << 4));
            s->hex_hi = -1;
            s->str_state = 0;
            put_str(s);
            return;
        }
        if (d < 0) return;
        if (s->hex_hi < 0) s->hex_hi = d;
        else { str_put(s, (uint8_t)((s->hex_hi << 4) | d)); s->hex_hi = -1; }
        return;
    }

    /* --- collecting a /Name --- */
    if (s->str_state == 3) {
        if (!is_ws(c) && !is_delim(c)) {
            if (s->str_len < sizeof s->last_name - 1)
                s->last_name[s->str_len++] = (char)c;
            return;
        }
        s->last_name[s->str_len] = 0;
        s->str_state = 0;
        s->str_len = 0;
        /* fall through so the delimiter is handled normally */
    }

    /* --- outside strings --- */
    if (c == '(') { s->str_state = 1; s->str_len = 0; s->paren = 0; s->esc = 0;
                    op_done(s); num_done(s); return; }
    if (c == '<') { op_done(s); num_done(s); s->pending_lt = 1; return; }
    if (c == '>') { op_done(s); num_done(s); s->pending_gt = 1; return; }
    if (c == '[') { op_done(s); num_done(s); s->depth++; return; }
    if (c == ']') { op_done(s); num_done(s); if (s->depth) s->depth--; return; }

    if ((c >= '0' && c <= '9') || c == '-' || c == '.' || c == '+') {
        if (s->tok_len) op_done(s);
        if (s->num_len < sizeof s->num - 1) s->num[s->num_len++] = (char)c;
        return;
    }
    if (c == '/') {                 /* a name operand: /F4 12 Tf */
        op_done(s); num_done(s);
        s->str_state = 3;
        s->str_len = 0;
        return;
    }
    if (is_ws(c) || is_delim(c)) { op_done(s); num_done(s); return; }

    num_done(s);
    if (s->tok_len < (int)sizeof s->tok - 1) s->tok[s->tok_len++] = (char)c;
}

/* ---- stream discovery ------------------------------------------------ */

/* Returns 1 when a content stream has been opened, 0 at end of file. */
static int next_stream(pdf_st *s)
{
    static const char kw[] = "stream";
    char lin[DICT + 1], *dict;
    int match = 0, armour, obj_no = -1;
    uint8_t c = 0, prev = 0;

    for (;;) {
        uint8_t p;
        int rc = scan_byte(s, &c);
        if (rc < 0) return TS_ERR_IO;
        if (rc == 0) return 0;
        p = prev;
        prev = c;

        if (c == kw[match] && (match || !((p >= 'A' && p <= 'Z') ||
                                          (p >= 'a' && p <= 'z')))) {
            if (++match == 6) {
                ts_off_t data;
                long len;
                int flate;

                match = 0;
                /* the keyword is followed by CRLF or LF, never CR alone */
                rc = scan_byte(s, &c);
                if (rc <= 0) return rc < 0 ? TS_ERR_IO : 0;
                if (c == '\r') { rc = scan_byte(s, &c); if (rc <= 0) return 0; }
                if (c != '\n') continue;
                data = scan_tell(s);

                dict_linear(s, lin);
                dict = dict_of(lin, &obj_no);
                if (!dict) continue;

                /* Not page content: images, embedded fonts, xref and object
                 * streams, metadata. */
                /* Not referenced by any page: a glyph program, an annotation
                 * appearance, a CMap. Decoding it would cost real time on a
                 * slow target and yield nothing. */
                if (!ts_pdffonts_is_content(s->fonts, obj_no)) continue;

                if (strstr(dict, "/Image") || strstr(dict, "/Length1") ||
                    strstr(dict, "/XRef") || strstr(dict, "/ObjStm") ||
                    strstr(dict, "/Metadata") || strstr(dict, "/FontFile"))
                    continue;

                /* Filters may be chained: [ /ASCII85Decode /FlateDecode ]. */
                flate = strstr(dict, "/FlateDecode") != NULL;
                armour = strstr(dict, "/ASCII85Decode") ? TS_FILT_A85
                       : strstr(dict, "/ASCIIHexDecode") ? TS_FILT_AHX : TS_FILT_RAW;

                if (strstr(dict, "/Filter") && !flate && armour == TS_FILT_RAW)
                    continue;                   /* LZW, DCT, CCITT, JPX... */

                len = dict_int(dict, "/Length");
                if (!flate && armour == TS_FILT_RAW && len <= 0)
                    continue;                   /* no filter and no extent */

                /* Compressed and armoured streams end on their own marker, so
                 * they don't need a length -- which is just as well, because
                 * /Length is often an indirect reference we can't resolve. */
                ts_file_stream_retarget(s->win, data,
                                        (!flate && armour == TS_FILT_RAW) ? len : -1);
                ts_filt_restart(s->filt, armour);
                if (flate) ts_inflate_restart(s->infl, TS_INFLATE_ZLIB);

                s->flate = flate;
                s->tried_raw = 0;
                s->data_off = data;
                s->data_len = (len > 0) ? len : -1;
                s->in_stream = 1;
                s->dblen = s->dbpos = 0;
                s->in_text = s->depth = s->tok_len = 0;
                s->num_len = s->str_len = 0;
                s->str_state = 0;
                s->dict_depth = s->pending_lt = s->pending_gt = 0;
                s->mc_depth = s->at_depth = s->at_active = 0;
                s->at_len = 0;
                s->cur_obj = obj_no;
                s->cmap = NULL;
                s->nops = 0;
                s->have_y = 0;
                s->cur_x = s->cur_y = s->line_x = s->line_y = s->pen_x = 0;
                s->tm_a = 1000; s->tf_size = 0; s->scale = 0;
                return 1;
            }
        } else {
            match = (c == kw[0]) ? 1 : 0;
        }
    }
}

/* ---- pull ------------------------------------------------------------ */

static int pdf_pull(ts_stream *st, uint8_t *buf, size_t n, size_t *out)
{
    pdf_st *s = st->st;
    size_t done = 0;

    while (done < n) {
        int rc;

        done += ts_pend_take(&s->out, buf + done, n - done);
        if (done == n) break;
        if (ts_pend_free(&s->out) < 16) continue;

        if (!s->in_stream) {
            rc = next_stream(s);
            if (rc < 0) return rc;
            if (rc == 0) {                      /* end of file */
                done += ts_pend_take(&s->out, buf + done, n - done);
                break;
            }
        }

        if (s->dbpos >= s->dblen) {
            size_t got = 0;
            ts_stream *src = s->flate ? s->infl : s->filt;
            ts_off_t used;

            s->dbpos = s->dblen = 0;
            rc = ts_pull(src, s->dbuf, DBUF, &got);

            /* Some generators write bare deflate where the spec demands a
             * zlib wrapper. If nothing at all came out, try again raw. */
            if (s->flate && !s->tried_raw && rc != TS_OK && !got) {
                s->tried_raw = 1;
                ts_file_stream_retarget(s->win, s->data_off, -1);
                ts_filt_restart(s->filt, ((filt_st *)s->filt->st)->mode);
                ts_inflate_restart(s->infl, TS_INFLATE_RAW);
                continue;
            }

            if (rc != TS_OK || !got) {
                /* Truncated, encrypted or otherwise undecodable: resume the
                 * scan past whatever the decoder managed to read. */
                /* Resume past a stream we decoded cleanly; one that failed
                 * might never have been a stream at all, so resume inside it
                 * rather than risk skipping the real thing.
                 *
                 * Prefer the dictionary's /Length. The decoder reads ahead,
                 * so ts_filt_used() overshoots the true end by up to a buffer
                 * -- enough to step over the whole of the next object and
                 * silently lose a page. */
                if (rc != TS_OK) used = 0;
                else if (s->data_len > 0) used = s->data_len;
                else used = ts_filt_used(s->filt);
                if (used < 1) used = 1;
                scan_seek(s, s->data_off + used);
                s->in_stream = 0;
                if (s->in_text) { nl(s); s->in_text = 0; }
                continue;
            }
            s->dblen = got;
        }
        tok_feed(s, s->dbuf[s->dbpos++]);
    }
    *out = done;
    return TS_OK;
}

static int pdf_reset(ts_stream *st)
{
    pdf_st *s = st->st;
    const ts_io *io = s->io;
    ts_stream *win = s->win, *filt = s->filt, *infl = s->infl;
    ts_pdffonts *fonts = s->fonts;

    memset(s, 0, sizeof *s);
    s->io = io; s->win = win; s->filt = filt; s->infl = infl;
    s->fonts = fonts;
    s->hex_hi = -1;
    s->cur_obj = -1;
    return TS_OK;
}

int ts_open_pdf(ts_ctx *c, ts_stream **out)
{
    ts_stream *st = ts_alloc(c->arena, sizeof *st);
    pdf_st *s = ts_alloc(c->arena, sizeof *s);

    if (!st || !s) return TS_ERR_NOMEM;

    s->io = c->io;
    s->win = ts_file_stream(c->arena, c->io, 0, -1);
    if (!s->win) return TS_ERR_NOMEM;
    s->filt = ts_filt_stream(c->arena, s->win);
    if (!s->filt) return TS_ERR_NOMEM;
    s->infl = ts_inflate_stream(c->arena, s->filt, TS_INFLATE_ZLIB);
    if (!s->infl) return TS_ERR_NOMEM;
    s->hex_hi = -1;
    s->cur_obj = -1;

    /* Two scans up front to learn the fonts. Documents with no /ToUnicode
     * anywhere cost only the scan; documents with subset fonts are
     * unreadable without it. */
    {
        int rc = ts_pdffonts_build(&s->fonts, c->arena, c->io,
                                   s->win, s->filt, s->infl);
        if (rc != TS_OK) return rc;
    }

    st->pull = pdf_pull; st->reset = pdf_reset; st->st = s;
    c->detected = TS_CS_UTF8;
    *out = st;
    return TS_OK;
}
