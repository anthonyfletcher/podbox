/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The engine core: format probing, the shared helpers, and the public entry
 * points from txt_source.h.
 ****************************************************************************/

#include "ts_internal.h"
#include "files/zip_member.h"       /* MEMBER_BASE64, MEMBER_STORED */

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* ---- misc helpers ---------------------------------------------------- */

const char *ts_strerror(int err)
{
    switch (err) {
    case BS_OK:         return "ok";
    case BS_ERR_IO:     return "I/O error";
    case BS_ERR_NOMEM:  return "arena exhausted";
    case BS_ERR_FORMAT: return "malformed file";
    case BS_ERR_UNSUP:  return "unsupported variant";
    case BS_ERR_INVAL:  return "invalid argument";
    case TS_ERR_LOCKED: return "locked (DRM)";
    }
    return "unknown error";
}

const char *ts_format_name(ts_format f)
{
    /* One entry per ts_format, in enum order. ts_format has no negative
     * members, so it is an unsigned type and the upper bound is the only
     * check needed. */
    static const char *n[] = { "unknown", "text", "markdown", "html", "rtf",
                               "fb2", "epub", "docx", "pdf", "lyrics",
                               "mobi" };
    return ((size_t)f < sizeof n / sizeof *n) ? n[f] : "unknown";
}

ts_config ts_config_default(void)
{
    ts_config c;
    c.force_format        = TS_FMT_UNKNOWN;
    c.charset             = TS_CS_AUTO;
    c.fallback            = TS_CS_CP1252;
    c.collapse_whitespace = 1;
    c.keep_soft_breaks    = 1;
    c.max_blank_lines     = 2;
    return c;
}

size_t ts_utf8_put(uint8_t *dst, uint32_t cp)
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    if (cp < 0x80) { dst[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) {
        dst[0] = (uint8_t)(0xC0 | (cp >> 6));
        dst[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = (uint8_t)(0xE0 | (cp >> 12));
        dst[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    dst[0] = (uint8_t)(0xF0 | (cp >> 18));
    dst[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    dst[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int ts_ascii_casecmp(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

int ts_ascii_ncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int d = lower((unsigned char)*a) - lower((unsigned char)*b);
        if (d || !*a) return d;
    }
    return 0;
}

void ts_book_subject(ts_book *b, const char *s)
{
    size_t n = strlen(s), have = strlen(b->subjects);
    const char *p = b->subjects;
    int count = 0;

    if (!n || have + 1 + n >= sizeof b->subjects) return;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);

        if (l == n && !memcmp(p, s, n)) return;
        count++;
        p += l + (e != NULL);
    }
    if (count >= TS_BOOK_SUBJECT_MAX) return;
    if (have) b->subjects[have++] = '\n';
    memcpy(b->subjects + have, s, n + 1);
}

/* ---- pending ring ---------------------------------------------------- */

void ts_emit(ts_pend *p, const uint8_t *b, size_t n)
{
    while (n-- && ts_pend_free(p))
        p->buf[p->tail++ % TS_PEND] = *b++;
}

/* All or nothing, because ts_emit() stops at the byte the ring fills on.
 * A stage whose step outruns the headroom it reserved would otherwise leave
 * a half-written sequence in the stream, and ts_read() promises callers it
 * never splits one. Dropping the character loses the same text and keeps the
 * output well-formed; the stages that reserve four bytes per codepoint never
 * reach this. */
void ts_emit_cp(ts_pend *p, uint32_t cp)
{
    uint8_t t[4];
    size_t n = ts_utf8_put(t, cp);

    if (ts_pend_free(p) < n)
        return;
    ts_emit(p, t, n);
}

size_t ts_pend_take(ts_pend *p, uint8_t *dst, size_t n)
{
    size_t i = 0;
    while (i < n && p->head != p->tail)
        dst[i++] = p->buf[p->head++ % TS_PEND];
    if (p->head == p->tail) p->head = p->tail = 0;
    return i;
}

/* ---- probing --------------------------------------------------------- */

static const char *ext_of(const char *name)
{
    const char *dot = NULL;
    if (!name) return NULL;
    for (; *name; name++)
        if (*name == '.') dot = name + 1;
        else if (*name == '/' || *name == '\\') dot = NULL;
    return dot;
}

static int starts(const uint8_t *h, size_t n, const char *lit)
{
    size_t l = strlen(lit);
    return n >= l && memcmp(h, lit, l) == 0;
}

/* Case-insensitive substring search over the probe window. */
static int contains_ci(const uint8_t *h, size_t n, const char *needle)
{
    size_t l = strlen(needle);
    if (n < l) return 0;
    for (size_t i = 0; i + l <= n; i++)
        if (ts_ascii_ncasecmp((const char *)h + i, needle, l) == 0)
            return 1;
    return 0;
}

static ts_format probe_ext(const char *name)
{
    const char *e = ext_of(name);
    if (!e) return TS_FMT_UNKNOWN;
    if (!ts_ascii_casecmp(e, "md") || !ts_ascii_casecmp(e, "markdown") ||
        !ts_ascii_casecmp(e, "mdown"))                    return TS_FMT_MARKDOWN;
    if (!ts_ascii_casecmp(e, "html") || !ts_ascii_casecmp(e, "htm") ||
        !ts_ascii_casecmp(e, "xhtml"))                    return TS_FMT_HTML;
    if (!ts_ascii_casecmp(e, "rtf"))                      return TS_FMT_RTF;
    if (!ts_ascii_casecmp(e, "fb2"))                      return TS_FMT_FB2;
    if (!ts_ascii_casecmp(e, "epub"))                     return TS_FMT_EPUB;
    if (!ts_ascii_casecmp(e, "docx"))                     return TS_FMT_DOCX;
    if (!ts_ascii_casecmp(e, "pdf"))                      return TS_FMT_PDF;
    if (!ts_ascii_casecmp(e, "mobi") || !ts_ascii_casecmp(e, "prc") ||
        !ts_ascii_casecmp(e, "azw") || !ts_ascii_casecmp(e, "azw3") ||
        !ts_ascii_casecmp(e, "pdb"))                      return TS_FMT_MOBI;
    /* By extension only. A lyrics file is plain text with tags in it, so a
     * content sniff would claim .txt files that merely start with a bracket. */
    if (!ts_ascii_casecmp(e, "lrc") || !ts_ascii_casecmp(e, "lrc8") ||
        !ts_ascii_casecmp(e, "snc"))                      return TS_FMT_LYRICS;
    if (!ts_ascii_casecmp(e, "txt") || !ts_ascii_casecmp(e, "text") ||
        !ts_ascii_casecmp(e, "nfo") || !ts_ascii_casecmp(e, "log"))
        return TS_FMT_PLAIN;
    return TS_FMT_UNKNOWN;
}

/* UTF-16 markup looks like "<\0?\0x\0m\0l\0"; squeeze the padding out so the
 * content sniffers below see plain ASCII. */
static size_t denull(const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                     ts_charset cs)
{
    size_t j = 0, i = (cs == TS_CS_UTF16LE) ? 0 : 1;
    if (cs != TS_CS_UTF16LE && cs != TS_CS_UTF16BE) {
        n = n < cap ? n : cap;
        memcpy(out, in, n);
        return n;
    }
    for (; i < n && j < cap; i += 2) out[j++] = in[i];
    return j;
}

static ts_format probe(const uint8_t *raw, size_t rawn, const char *name,
                       ts_charset cs, uint8_t *scratch, size_t scap)
{
    size_t n = denull(raw, rawn, scratch, scap, cs);
    const uint8_t *h = scratch;
    ts_format e;

    if (starts(raw, rawn, "%PDF"))          return TS_FMT_PDF;
    if (rawn >= 68 && (!memcmp(raw + 60, "BOOKMOBI", 8) ||
                       !memcmp(raw + 60, "TEXtREAd", 8))) return TS_FMT_MOBI;
    if (starts(raw, rawn, "{\\rtf"))        return TS_FMT_RTF;
    if (starts(raw, rawn, "PK\x03\x04") ||
        starts(raw, rawn, "PK\x05\x06"))    return TS_FMT_EPUB; /* refined later */

    if (contains_ci(h, n, "<FictionBook"))  return TS_FMT_FB2;
    if (contains_ci(h, n, "<!DOCTYPE html") || contains_ci(h, n, "<html") ||
        contains_ci(h, n, "<head") || contains_ci(h, n, "<body"))
        return TS_FMT_HTML;

    e = probe_ext(name);
    if (e != TS_FMT_UNKNOWN) return e;

    /* An XML file we can't identify still reads better with tags stripped. */
    if (starts(h, n, "<?xml"))              return TS_FMT_HTML;
    return TS_FMT_PLAIN;
}

/* ---- source ---------------------------------------------------------- */

#define HEAD_MAX 4096

struct ts_source {
    bs_arena   arena;
    bs_io      io;
    ts_config  cfg;
    bs_stream *pipe;
    bs_stream *parts;        /* the zip formats' spine stream, else NULL */
    ts_fb2_pictures *fb2;    /* a loose FB2's pictures, once looked for */
    ts_format  fmt;
    ts_charset cs;
    uint8_t    hold[4];      /* partial UTF-8 carried between ts_read calls */
    size_t     hold_n;
    int        eof, err;
};

int ts_open(ts_source **out, const bs_io *io, const char *name,
            void *arena, size_t arena_size, const ts_config *cfg)
{
    ts_source *s;
    bs_arena a;
    uint8_t *head, *scratch;
    long got;
    size_t bom = 0;
    ts_ctx ctx;
    int rc;

    if (!out || !io || !arena) return BS_ERR_INVAL;
    *out = NULL;

    a.base = arena; a.size = arena_size; a.used = 0;
    s = bs_alloc(&a, sizeof *s);
    head = bs_alloc(&a, HEAD_MAX);
    scratch = bs_alloc(&a, HEAD_MAX);
    if (!s || !head || !scratch) return BS_ERR_NOMEM;

    s->io  = *io;
    s->cfg = cfg ? *cfg : ts_config_default();
    if (s->cfg.max_blank_lines < 1) s->cfg.max_blank_lines = 1;

    if (io->seek(io->ctx, 0, SEEK_SET) < 0) return BS_ERR_IO;
    got = io->read(io->ctx, head, HEAD_MAX);
    if (got < 0) return BS_ERR_IO;

    s->cs = (s->cfg.charset != TS_CS_AUTO)
          ? s->cfg.charset
          : ts_charset_sniff(head, (size_t)got, s->cfg.fallback, &bom);

    s->fmt = (s->cfg.force_format != TS_FMT_UNKNOWN)
           ? s->cfg.force_format
           : probe(head, (size_t)got, name, s->cs, scratch, HEAD_MAX);

    /* The probe scratch is dead now; hand its space back to the back ends. */
    a.used -= HEAD_MAX;

    ctx.arena = &a; ctx.io = &s->io; ctx.cfg = &s->cfg;
    ctx.head = head; ctx.hlen = (size_t)got; ctx.detected = s->cs;
    ctx.resolved = TS_FMT_UNKNOWN;
    ctx.parts = NULL;

    switch (s->fmt) {
    case TS_FMT_PLAIN:
    case TS_FMT_MARKDOWN:
    case TS_FMT_RTF:    rc = ts_open_text(&ctx, s->fmt, &s->pipe); break;
    case TS_FMT_HTML:   rc = ts_open_html(&ctx, &s->pipe);         break;
    case TS_FMT_FB2:    rc = ts_open_fb2(&ctx, &s->pipe);          break;
    case TS_FMT_PDF:    rc = ts_open_pdf(&ctx, &s->pipe);          break;
    case TS_FMT_LYRICS: rc = ts_open_lyrics(&ctx, &s->pipe);       break;
    case TS_FMT_MOBI:   rc = ts_open_mobi(&ctx, &s->pipe);         break;
    case TS_FMT_EPUB:
    case TS_FMT_DOCX:   rc = ts_open_epub(&ctx, &s->pipe);         break;
    default:            rc = BS_ERR_UNSUP;                         break;
    }
    if (rc != BS_OK) return rc;

    /* A container probe may have resolved EPUB vs DOCX vs zipped FB2, and a
     * markup back end may have found a declared encoding. */
    if (ctx.resolved != TS_FMT_UNKNOWN)
        s->fmt = ctx.resolved;
    s->cs = ctx.detected;
    s->parts = ctx.parts;

    s->arena = a;
    s->eof = s->err = 0;
    s->hold_n = 0;
    *out = s;
    return BS_OK;
}

/* Length of the UTF-8 sequence starting with `b`, or 0 if it's a stray
 * continuation byte. */
static size_t seqlen(uint8_t b)
{
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 0;
}

/* Number of trailing bytes that form an incomplete sequence. */
static size_t tail_partial(const uint8_t *b, size_t n)
{
    size_t back = 0;
    while (back < 4 && back < n) {
        size_t need;
        back++;
        need = seqlen(b[n - back]);
        if (need == 0) continue;            /* continuation byte, keep going */
        return (need > back) ? back : 0;    /* complete sequence => nothing held */
    }
    return 0;
}

long ts_read(ts_source *s, char *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    size_t total = 0, keep;
    int rc;

    if (!s || !buf || n < 8) return BS_ERR_INVAL;
    if (s->err) return s->err;

    if (s->hold_n) {
        memcpy(p, s->hold, s->hold_n);
        total = s->hold_n;
        s->hold_n = 0;
    }

    while (total < n) {
        size_t got = 0;
        if (s->eof) break;
        rc = bs_pull(s->pipe, p + total, n - total, &got);
        if (rc != BS_OK) { s->err = rc; return rc; }
        if (!got) { s->eof = 1; break; }
        total += got;
    }

    if (!s->eof) {
        keep = tail_partial(p, total);
        if (keep && keep < total) {
            memcpy(s->hold, p + total - keep, keep);
            s->hold_n = keep;
            total -= keep;
        }
    }
    return (long)total;
}

int ts_rewind(ts_source *s)
{
    int rc;
    if (!s) return BS_ERR_INVAL;
    if (!s->pipe->reset) return BS_ERR_UNSUP;
    rc = s->pipe->reset(s->pipe);
    if (rc != BS_OK) return rc;
    s->eof = s->err = 0;
    s->hold_n = 0;
    return BS_OK;
}

int ts_part_count(const ts_source *s)
{
    if (!s || !s->parts) return 0;
    return s->fmt == TS_FMT_MOBI ? ts_mobi_parts(s->parts)
                                 : ts_zipdoc_parts(s->parts);
}

long ts_part_size(const ts_source *s, int part)
{
    if (!s || !s->parts) return 0;
    return s->fmt == TS_FMT_MOBI ? ts_mobi_part_size(s->parts, part)
                                 : ts_zipdoc_part_size(s->parts, part);
}

int ts_picture_extent(ts_source *s, int part, const char *href, long *at,
                      uint32_t *csize, uint32_t *usize, int *method)
{
    bs_off_t data;
    int rc;

    if (s && !s->parts && s->fmt == TS_FMT_FB2) {
        if (!s->fb2 && !(s->fb2 = bs_alloc(&s->arena, sizeof *s->fb2)))
            return BS_ERR_NOMEM;
        rc = ts_fb2_picture(&s->io, s->fb2, href, &data, csize);
        *usize = 0;
        *method = MEMBER_BASE64;
        if (rc == BS_OK) *at = (long)data;
        return rc;
    }
    if (s && s->parts && s->fmt == TS_FMT_MOBI) {
        rc = ts_mobi_picture(s->parts, href, &data, csize);
        *usize = *csize;
        *method = MEMBER_STORED;
        if (rc == BS_OK) *at = (long)data;
        return rc;
    }
    if (!s || !s->parts || s->fmt != TS_FMT_EPUB) return BS_ERR_UNSUP;
    rc = ts_zipdoc_picture(s->parts, part, href, &data, csize, usize, method);
    if (rc == BS_OK) *at = (long)data;
    return rc;
}

int ts_seek_part(ts_source *s, int part)
{
    int rc;

    if (!s) return BS_ERR_INVAL;
    if (!s->parts) return BS_ERR_UNSUP;
    rc = s->fmt == TS_FMT_MOBI ? ts_mobi_seek_part(s->parts, part)
                               : ts_zipdoc_seek_part(s->parts, part);
    if (rc != BS_OK) return rc;
    s->eof = s->err = 0;
    s->hold_n = 0;
    return BS_OK;
}

int ts_toc(ts_source *s, int (*cb)(void *ud, const char *title, int part),
           void *ud)
{
    if (!s) return BS_ERR_INVAL;
    if (!s->parts) return BS_ERR_UNSUP;
    return s->fmt == TS_FMT_MOBI ? ts_mobi_toc(s->parts, cb, ud)
                                 : ts_zipdoc_toc(s->parts, cb, ud);
}

void ts_close(ts_source *s)
{
    if (!s) return;
    if (s->io.close) s->io.close(s->io.ctx);
    s->pipe = NULL;      /* arena belongs to the caller; nothing to free */
}

ts_format  ts_source_format (const ts_source *s) { return s ? s->fmt : TS_FMT_UNKNOWN; }
ts_charset ts_source_charset(const ts_source *s) { return s ? s->cs : TS_CS_AUTO; }
size_t     ts_arena_used    (const ts_source *s) { return s ? s->arena.used : 0; }
