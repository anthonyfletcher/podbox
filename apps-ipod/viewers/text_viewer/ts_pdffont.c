/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * PDF font handling: /ToUnicode CMaps and the page resources that name
 * them, so extracted glyphs map back to characters.
 ****************************************************************************/

#include "ts_internal.h"
#include "string-extra.h"   /* strlcpy (strncpy isn't linkable in -nostdlib core) */

/* Caps, not allocations: exceeded means the extra fonts fall back to the
 * single-byte path rather than the whole document failing. */
#define MAX_FONT 128
#define MAX_PAGE 64
#define MAX_RES  512
#define NAMELEN  8

typedef struct { uint32_t lo, hi, d0, d1; } bfr;   /* d1 = 0 unless a ligature */
typedef struct { int obj, nbytes, nr; bfr *r; } cmap_t;
typedef struct { int obj, tou, cmap; } font_t;
typedef struct { char name[NAMELEN]; int fobj; } res_t;
typedef struct { int contents, res0, resn; } page_t;

struct ts_pdffonts {
    font_t *f; int nf;
    page_t *p; int np;
    res_t  *r; int nr;
    cmap_t *c; int nc;
};

/* ---- buffered reader ------------------------------------------------- */

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

typedef struct {
    const ts_io *io;
    uint8_t  buf[512];
    size_t   pos, len;
    ts_off_t base;
    int      eof;
} rd;

static void rd_init(rd *r, const ts_io *io)
{
    memset(r, 0, sizeof *r);
    r->io = io;
}

static int rd_byte(rd *r)
{
    if (r->pos >= r->len) {
        long got;
        if (r->eof) return -1;
        r->base += (ts_off_t)r->len;
        r->pos = r->len = 0;
        if (r->io->seek(r->io->ctx, r->base, SEEK_SET) < 0) { r->eof = 1; return -1; }
        got = r->io->read(r->io->ctx, r->buf, sizeof r->buf);
        if (got <= 0) { r->eof = 1; return -1; }
        r->len = (size_t)got;
    }
    return r->buf[r->pos++];
}

static int rd_peek(rd *r)
{
    int c = rd_byte(r);
    if (c >= 0) r->pos--;
    return c;
}

static ts_off_t rd_tell(rd *r) { return r->base + (ts_off_t)r->pos; }

/* ---- tokenizer ------------------------------------------------------- */

enum { TK_EOF, TK_NUM, TK_NAME, TK_KW, TK_OPEN, TK_CLOSE, TK_AOPEN, TK_ACLOSE, TK_JUNK };

typedef struct {
    int  type;
    long num;
    char name[24];
} tok;

static int is_ws(int c)
{ return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0; }
static int is_delim(int c)
{ return c=='('||c==')'||c=='<'||c=='>'||c=='['||c==']'||c=='{'||c=='}'||c=='/'||c=='%'; }

/* Skips a literal string, honouring nesting and escapes: a /Title could
 * otherwise smuggle "obj" or "/Contents" into the scan. */
static void skip_string(rd *r)
{
    int depth = 1, c;

    while ((c = rd_byte(r)) >= 0) {
        if (c == '\\') { rd_byte(r); continue; }
        if (c == '(') depth++;
        else if (c == ')' && --depth == 0) return;
    }
}

static int next_tok(rd *r, tok *t)
{
    int c;

    t->type = TK_EOF; t->num = 0; t->name[0] = 0;
    for (;;) {
        do { c = rd_byte(r); } while (c >= 0 && is_ws(c));
        if (c < 0) return TK_EOF;
        if (c != '%') break;
        while ((c = rd_byte(r)) >= 0 && c != '\n' && c != '\r') ;
    }

    if (c == '(') { skip_string(r); t->type = TK_JUNK; return t->type; }
    if (c == '[') { t->type = TK_AOPEN; return t->type; }
    if (c == ']') { t->type = TK_ACLOSE; return t->type; }
    if (c == '>') {
        if (rd_peek(r) == '>') rd_byte(r);
        t->type = TK_CLOSE;
        return t->type;
    }
    if (c == '<') {
        if (rd_peek(r) == '<') { rd_byte(r); t->type = TK_OPEN; return t->type; }
        while ((c = rd_byte(r)) >= 0 && c != '>') ;      /* hex string */
        t->type = TK_JUNK;
        return t->type;
    }
    if (c == '/') {
        size_t n = 0;
        while ((c = rd_peek(r)) >= 0 && !is_ws(c) && !is_delim(c)) {
            rd_byte(r);
            if (n < sizeof t->name - 1) t->name[n++] = (char)c;
        }
        t->name[n] = 0;
        t->type = TK_NAME;
        return t->type;
    }
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
        long v = 0;
        int neg = (c == '-'), seen = 0;

        if (c >= '0' && c <= '9') { v = c - '0'; seen = 1; }
        while ((c = rd_peek(r)) >= 0 && ((c >= '0' && c <= '9') || c == '.')) {
            rd_byte(r);
            if (c != '.' && seen >= 0) { v = v * 10 + (c - '0'); seen = 1; }
            if (c == '.') seen = -1;                     /* ignore the fraction */
        }
        t->num = neg ? -v : v;
        t->type = TK_NUM;
        return t->type;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'' || c == '"') {
        size_t n = 0;
        t->name[n++] = (char)c;
        while ((c = rd_peek(r)) >= 0 && !is_ws(c) && !is_delim(c)) {
            rd_byte(r);
            if (n < sizeof t->name - 1) t->name[n++] = (char)c;
        }
        t->name[n] = 0;
        t->type = TK_KW;
        return t->type;
    }
    t->type = TK_JUNK;
    return t->type;
}

/* Steps over a stream body. /Length is often an indirect reference we can't
 * resolve here, so hunt the keyword instead; a false "endstream" inside
 * compressed data is possible but vanishingly rare. */
static void skip_stream(rd *r)
{
    static const char kw[] = "endstream";
    int match = 0, c;

    while ((c = rd_byte(r)) >= 0) {
        if (c == kw[match]) { if (++match == 9) return; }
        else match = (c == kw[0]) ? 1 : 0;
    }
}

/* ---- pass 1: structure ----------------------------------------------- */

enum { K_NONE, K_TOU, K_CONTENTS, K_TYPE, K_FONT };

static void add_res(ts_pdffonts *F, const char *name, int fobj)
{
    res_t *e;
    if (F->nr >= MAX_RES) return;
    e = &F->r[F->nr++];
    strlcpy(e->name, name, NAMELEN);
    e->fobj = fobj;
}

static int scan_structure(ts_pdffonts *F, const ts_io *io)
{
    rd r;
    tok t;
    long n1 = 0, n2 = 0;
    int cur = -1, key = K_NONE, in_font = 0, is_page = 0, tou = -1;
    int res0 = 0;
    char pend[NAMELEN];

    rd_init(&r, io);
    pend[0] = 0;

    while (next_tok(&r, &t) != TK_EOF) {
        switch (t.type) {

        case TK_NUM:
            n2 = n1; n1 = t.num;
            continue;

        case TK_NAME:
            if (in_font) {                       /* inside /Font << ... >> */
                strlcpy(pend, t.name, NAMELEN);
                continue;
            }
            if (key == K_TYPE) {
                if (!strcmp(t.name, "Page")) is_page = 1;
                key = K_NONE;
                continue;
            }
            if (!strcmp(t.name, "ToUnicode"))      key = K_TOU;
            else if (!strcmp(t.name, "Contents"))  key = K_CONTENTS;
            else if (!strcmp(t.name, "Type"))      key = K_TYPE;
            else if (!strcmp(t.name, "Font"))      key = K_FONT;
            else if (key != K_FONT)                key = K_NONE;
            continue;

        case TK_OPEN:
            if (key == K_FONT) { in_font = 1; key = K_NONE; res0 = F->nr; }
            continue;

        case TK_CLOSE:
            in_font = 0;
            continue;

        case TK_KW:
            if (!strcmp(t.name, "obj")) {
                cur = (int)n2;
                key = K_NONE; in_font = 0; is_page = 0; tou = -1;
                if (res0 > F->nr) res0 = F->nr;
                continue;
            }
            if (!strcmp(t.name, "stream")) { skip_stream(&r); continue; }
            if (!strcmp(t.name, "R")) {
                if (in_font && pend[0]) { add_res(F, pend, (int)n2); pend[0] = 0; }
                else if (key == K_TOU)      { tou = (int)n2; key = K_NONE; }
                else if (key == K_CONTENTS) {
                    /* /Contents may be a single ref or an array of them. */
                    if (F->np < MAX_PAGE) {
                        page_t *p = &F->p[F->np++];
                        p->contents = (int)n2;
                        p->res0 = res0;
                        p->resn = F->nr - res0;
                    }
                }
                continue;
            }
            if (!strcmp(t.name, "endobj")) {
                if (tou >= 0 && F->nf < MAX_FONT) {
                    font_t *f = &F->f[F->nf++];
                    f->obj = cur; f->tou = tou; f->cmap = -1;
                }
                if (is_page) {
                    /* fix up the resource span now that it is complete */
                    int i;
                    for (i = F->np - 1; i >= 0; i--) {
                        if (F->p[i].res0 != res0) break;
                        F->p[i].resn = F->nr - res0;
                    }
                }
                cur = -1; is_page = 0; tou = -1;
                continue;
            }
            continue;

        default:
            continue;
        }
    }
    return TS_OK;
}

/* ---- pass 2: cmaps --------------------------------------------------- */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* A CMap hex string is both a value and a width: <08> is a one-byte code,
 * <0008> a two-byte one, and <00660069> the two characters "fi". */
typedef struct { uint32_t v[4]; int nv, digits; } hexs;

typedef struct {
    ts_stream *src;
    uint8_t buf[256];
    size_t  pos, len;
    int     eof;
} cs;

static int cs_byte(cs *c)
{
    if (c->pos >= c->len) {
        size_t got = 0;
        if (c->eof) return -1;
        if (ts_pull(c->src, c->buf, sizeof c->buf, &got) != TS_OK || !got)
            { c->eof = 1; return -1; }
        c->pos = 0; c->len = got;
    }
    return c->buf[c->pos++];
}

/* Reads one token of a CMap: a hex string, a keyword, or a delimiter. */
static int cmap_tok(cs *c, hexs *h, char *kw, size_t kwcap)
{
    int ch;

    kw[0] = 0;
    h->nv = h->digits = 0;
    do { ch = cs_byte(c); } while (ch >= 0 && (is_ws(ch) || ch == '\r'));
    if (ch < 0) return 0;

    if (ch == '<') {
        uint32_t acc = 0;
        int nib = 0;
        h->v[0] = 0;
        while ((ch = cs_byte(c)) >= 0 && ch != '>') {
            int d = hexval(ch);
            if (d < 0) continue;
            acc = (acc << 4) | (uint32_t)d;
            h->digits++;
            if (++nib == 4) {                    /* one UTF-16 unit */
                if (h->nv < 4) h->v[h->nv++] = acc;
                acc = 0; nib = 0;
            }
        }
        if (nib) {                               /* <08>: a short code */
            if (h->nv < 4) h->v[h->nv++] = acc;
        }
        return 1;
    }
    if (ch == '[' || ch == ']') { kw[0] = (char)ch; kw[1] = 0; return 2; }
    {
        size_t n = 0;
        do {
            if (n < kwcap - 1) kw[n++] = (char)ch;
            ch = cs_byte(c);
        } while (ch >= 0 && !is_ws(ch) && ch != '<' && ch != '[' && ch != ']');
        kw[n] = 0;
        if (ch == '<' || ch == '[' || ch == ']') c->pos--;   /* push back */
        return 2;
    }
}

/* Surrogate pairs are one character; anything else is a ligature expansion. */
static void hex_to_uni(const hexs *h, uint32_t *d0, uint32_t *d1)
{
    *d0 = h->nv ? h->v[0] : 0;
    *d1 = 0;
    if (h->nv >= 2) {
        if (h->v[0] >= 0xD800 && h->v[0] <= 0xDBFF &&
            h->v[1] >= 0xDC00 && h->v[1] <= 0xDFFF)
            *d0 = 0x10000 + ((h->v[0] - 0xD800) << 10) + (h->v[1] - 0xDC00);
        else
            *d1 = h->v[1];
    }
}

static bfr *cmap_add(ts_arena *a, cmap_t *cm)
{
    bfr *e = ts_alloc(a, sizeof *e);
    if (!e) return NULL;
    if (!cm->nr) cm->r = e;      /* bump allocation keeps these contiguous */
    cm->nr++;
    return e;
}

static int parse_cmap(ts_arena *a, cmap_t *cm, ts_stream *src)
{
    cs c;
    hexs h, lo;
    char kw[32];
    int mode = 0, phase = 0, arr = 0;
    uint32_t rlo = 0, rhi = 0;

    memset(&c, 0, sizeof c);
    c.src = src;
    cm->nbytes = 1;
    cm->nr = 0;
    cm->r = NULL;

    for (;;) {
        int t = cmap_tok(&c, &h, kw, sizeof kw);
        if (!t) break;

        if (t == 2) {
            if (!strcmp(kw, "begincodespacerange")) { mode = 1; phase = 0; }
            else if (!strcmp(kw, "beginbfchar"))    { mode = 2; phase = 0; }
            else if (!strcmp(kw, "beginbfrange"))   { mode = 3; phase = 0; }
            else if (!strncmp(kw, "end", 3))        { mode = 0; phase = 0; arr = 0; }
            else if (kw[0] == '[')                  { arr = 1; }
            else if (kw[0] == ']')                  { arr = 0; phase = 0; }
            continue;
        }

        switch (mode) {
        case 1:                                   /* codespacerange */
            if (phase == 0) { cm->nbytes = h.digits / 2; if (cm->nbytes < 1) cm->nbytes = 1; }
            phase ^= 1;
            break;

        case 2:                                   /* bfchar: <code> <uni> */
            if (phase == 0) { lo = h; phase = 1; }
            else {
                bfr *e = cmap_add(a, cm);
                if (!e) return TS_ERR_NOMEM;
                e->lo = e->hi = lo.v[0];
                hex_to_uni(&h, &e->d0, &e->d1);
                phase = 0;
            }
            break;

        case 3:                                   /* bfrange: <lo> <hi> <dst|[..]> */
            if (arr) {                            /* one destination per code */
                bfr *e = cmap_add(a, cm);
                if (!e) return TS_ERR_NOMEM;
                e->lo = e->hi = rlo++;
                hex_to_uni(&h, &e->d0, &e->d1);
                break;
            }
            if (phase == 0) { rlo = h.v[0]; phase = 1; }
            else if (phase == 1) { rhi = h.v[0]; phase = 2; }
            else {
                bfr *e = cmap_add(a, cm);
                if (!e) return TS_ERR_NOMEM;
                e->lo = rlo; e->hi = rhi;
                hex_to_uni(&h, &e->d0, &e->d1);
                phase = 0;
            }
            break;

        default:
            break;
        }
    }
    return TS_OK;
}

static int needs_cmap(ts_pdffonts *F, int obj)
{
    int i;
    for (i = 0; i < F->nf; i++)
        if (F->f[i].tou == obj) return 1;
    return 0;
}

static void attach_cmap(ts_pdffonts *F, int obj, int idx)
{
    int i;
    for (i = 0; i < F->nf; i++)
        if (F->f[i].tou == obj) F->f[i].cmap = idx;
}

static int scan_cmaps(ts_pdffonts *F, ts_arena *a, const ts_io *io,
                      ts_stream *win, ts_stream *filt, ts_stream *infl)
{
    rd r;
    tok t;
    long n1 = 0, n2 = 0;
    int cur = -1, want = 0, flate = 0, armour = TS_FILT_RAW;

    rd_init(&r, io);

    while (next_tok(&r, &t) != TK_EOF) {
        if (t.type == TK_NUM) { n2 = n1; n1 = t.num; continue; }

        if (t.type == TK_NAME) {
            if (!strcmp(t.name, "FlateDecode"))          flate = 1;
            else if (!strcmp(t.name, "ASCII85Decode"))   armour = TS_FILT_A85;
            else if (!strcmp(t.name, "ASCIIHexDecode"))  armour = TS_FILT_AHX;
            continue;
        }

        if (t.type != TK_KW) continue;

        if (!strcmp(t.name, "obj")) {
            cur = (int)n2;
            want = needs_cmap(F, cur);
            flate = 0; armour = TS_FILT_RAW;
            continue;
        }
        if (!strcmp(t.name, "stream")) {
            ts_off_t data;
            int c;

            if (!want) { skip_stream(&r); continue; }

            /* the keyword is followed by CRLF or LF */
            c = rd_byte(&r);
            if (c == '\r') c = rd_byte(&r);
            if (c != '\n') { skip_stream(&r); continue; }
            data = rd_tell(&r);

            if (F->nc < MAX_FONT) {
                cmap_t *cm = &F->c[F->nc];
                int rc;

                ts_file_stream_retarget(win, data, -1);
                ts_filt_restart(filt, armour);
                if (flate) ts_inflate_restart(infl, TS_INFLATE_ZLIB);

                cm->obj = cur;
                rc = parse_cmap(a, cm, flate ? infl : filt);
                if (rc != TS_OK) return rc;
                if (cm->nr) { attach_cmap(F, cur, F->nc); F->nc++; }
            }
            /* the reader's buffer is stale after the stream was consumed */
            rd_init(&r, io);
            r.base = data;
            skip_stream(&r);
            want = 0;
            continue;
        }
        if (!strcmp(t.name, "endobj")) { cur = -1; want = 0; continue; }
    }
    return TS_OK;
}

/* ---- api ------------------------------------------------------------- */

int ts_pdffonts_build(ts_pdffonts **out, ts_arena *a, const ts_io *io,
                      ts_stream *win, ts_stream *filt, ts_stream *infl)
{
    ts_pdffonts *F = ts_alloc(a, sizeof *F);
    int rc;

    *out = NULL;
    if (!F) return TS_ERR_NOMEM;

    F->f = ts_alloc(a, sizeof(font_t) * MAX_FONT);
    F->p = ts_alloc(a, sizeof(page_t) * MAX_PAGE);
    F->r = ts_alloc(a, sizeof(res_t)  * MAX_RES);
    F->c = ts_alloc(a, sizeof(cmap_t) * MAX_FONT);
    if (!F->f || !F->p || !F->r || !F->c) return TS_ERR_NOMEM;

    rc = scan_structure(F, io);
    if (rc != TS_OK) return rc;
    if (F->nf) {
        rc = scan_cmaps(F, a, io, win, filt, infl);
        if (rc != TS_OK) return rc;
    }
    *out = F;
    return TS_OK;
}

const void *ts_pdffonts_cmap(ts_pdffonts *F, int content_obj, const char *name)
{
    int i, j, fobj = -1;

    if (!F || content_obj < 0) return NULL;

    for (i = 0; i < F->np; i++) {
        if (F->p[i].contents != content_obj) continue;
        for (j = F->p[i].res0; j < F->p[i].res0 + F->p[i].resn && j < F->nr; j++)
            if (!strcmp(F->r[j].name, name)) { fobj = F->r[j].fobj; break; }
        if (fobj >= 0) break;
    }
    if (fobj < 0) return NULL;

    for (i = 0; i < F->nf; i++)
        if (F->f[i].obj == fobj && F->f[i].cmap >= 0)
            return &F->c[F->f[i].cmap];
    return NULL;
}

/* The pre-pass already learned which objects the pages point at, so the
 * extractor need not inflate every stream in the file just to discover it
 * holds no text. A Chrome PDF stores each Type3 glyph as its own stream:
 * decompressing all of them costs far more than reading the pages. */
int ts_pdffonts_is_content(ts_pdffonts *F, int obj)
{
    int i;
    if (!F || !F->np) return 1;              /* no pages found: try them all */
    for (i = 0; i < F->np; i++)
        if (F->p[i].contents == obj) return 1;
    return 0;
}

int ts_cmap_bytes(const void *cmap)
{
    const cmap_t *cm = cmap;
    return cm ? cm->nbytes : 1;
}

int ts_cmap_map(const void *cmap, uint32_t code, uint32_t *u0, uint32_t *u1)
{
    const cmap_t *cm = cmap;
    int i;

    if (!cm) return 0;
    for (i = 0; i < cm->nr; i++) {
        const bfr *e = &cm->r[i];
        if (code < e->lo || code > e->hi) continue;
        *u0 = e->d0 + (code - e->lo);
        *u1 = e->d1;
        return 1;
    }
    return 0;
}
