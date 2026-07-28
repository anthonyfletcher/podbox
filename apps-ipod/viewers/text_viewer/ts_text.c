/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Back ends for the formats that need no container: plain text, markdown
 * and RTF.
 ****************************************************************************/

#include "ts_internal.h"

#define TIN 256

/* ===================== shared filter scaffolding ====================== */

typedef struct flt_st flt_st;
struct flt_st {
    ts_stream *src;
    uint8_t in[TIN];
    size_t ipos, ilen;
    int    src_eof;
    ts_pend out;
    void (*feed)(flt_st *, uint8_t);
    void (*eos)(flt_st *);          /* optional flush at end of input */
    int  started, eos_done;
    /* markdown */
    int at_bol, fence, tick, link, esc;
    /* rtf */
    int depth, skip_depth, ctrl, ctrl_len, uc_skip, hexd, hexv;
    char ctrl_word[32];
    ts_charset cp;
};

static int flt_fill(flt_st *s)
{
    size_t got = 0;
    int rc;

    if (s->ipos < s->ilen || s->src_eof) return TS_OK;
    s->ipos = s->ilen = 0;
    rc = ts_pull(s->src, s->in, TIN, &got);
    if (rc != TS_OK) return rc;
    if (!got) s->src_eof = 1;
    s->ilen = got;
    return TS_OK;
}

static int flt_pull(ts_stream *st, uint8_t *buf, size_t n, size_t *out)
{
    flt_st *s = st->st;
    size_t done = 0;

    while (done < n) {
        int rc;
        done += ts_pend_take(&s->out, buf + done, n - done);
        if (done == n) break;
        if (ts_pend_free(&s->out) < 16) continue;

        rc = flt_fill(s);
        if (rc != TS_OK) return rc;
        if (s->ipos >= s->ilen) {
            if (s->eos && !s->eos_done) { s->eos_done = 1; s->eos(s); continue; }
            done += ts_pend_take(&s->out, buf + done, n - done);
            break;
        }
        s->feed(s, s->in[s->ipos++]);
    }
    *out = done;
    return TS_OK;
}

static int flt_reset(ts_stream *st)
{
    flt_st *s = st->st;
    ts_stream *src = s->src;
    void (*feed)(flt_st *, uint8_t) = s->feed;
    void (*eos)(flt_st *) = s->eos;
    ts_charset cp = s->cp;
    int rc = src->reset ? src->reset(src) : TS_ERR_UNSUP;

    if (rc != TS_OK) return rc;
    memset(s, 0, sizeof *s);
    s->src = src; s->feed = feed; s->eos = eos; s->cp = cp;
    s->at_bol = 1; s->uc_skip = 1;
    return TS_OK;
}

static ts_stream *flt_new(ts_arena *a, ts_stream *src,
                          void (*feed)(flt_st *, uint8_t), flt_st **sp)
{
    ts_stream *st = ts_alloc(a, sizeof *st);
    flt_st *s = ts_alloc(a, sizeof *s);

    if (!st || !s || !src) return NULL;
    s->src = src;
    s->feed = feed;
    s->at_bol = 1;
    s->uc_skip = 1;
    st->pull = flt_pull; st->reset = flt_reset; st->st = s;
    if (sp) *sp = s;
    return st;
}

/* ===================== plain text ===================================== */

/* Only normalisation: CR/CRLF become LF. Everything else is the author's. */
static void plain_feed(flt_st *s, uint8_t c)
{
    if (c == '\r') return;
    ts_emit(&s->out, &c, 1);
}

/* ===================== markdown ======================================= */

/* Light-touch: drop the marks that only make sense to a renderer, keep the
 * prose and the layout. Code fences pass through verbatim. */
enum { LK_NONE = 0, LK_AFTER_BRACKET, LK_IN_TARGET };

static void md_emit(flt_st *s, uint8_t c)
{
    ts_emit(&s->out, &c, 1);
    s->at_bol = (c == '\n');
}

static void md_feed(flt_st *s, uint8_t c)
{
    if (c == '\r') return;

    /* Backtick runs: three or more at the start of a line open or close a
     * fenced block, anything shorter is an inline code mark and vanishes. */
    if (c == '`') { s->tick++; return; }
    if (s->tick) {
        int run = s->tick;
        s->tick = 0;
        if (run >= 3 && s->at_bol) {
            s->fence = !s->fence;
            if (c != '\n') return;              /* drop the info string */
        }
    }

    if (s->fence) { md_emit(s, c); return; }

    if (s->esc) { s->esc = 0; md_emit(s, c); return; }
    if (c == '\n') { s->link = LK_NONE; md_emit(s, c); return; }

    if (s->at_bol) {
        if (c == '#' || c == '>' || c == ' ' || c == '\t') return;
        if (c == '=' || c == '-') { /* setext rules pass through as-is */ }
        s->at_bol = 0;
    }

    if (s->link == LK_IN_TARGET) {              /* swallow (url) or [ref] */
        if (c == ')' || c == ']') s->link = LK_NONE;
        return;
    }

    if (s->link == LK_AFTER_BRACKET && (c == '(' || c == '[')) {
        s->link = LK_IN_TARGET;                 /* [label](url) or [label][ref] */
        return;
    }

    switch (c) {
    case '\\': s->esc = 1; return;
    case '*': case '_': return;                 /* emphasis marks */
    case '[':  s->link = LK_NONE; return;       /* keep the label only */
    case ']':  s->link = LK_AFTER_BRACKET; return;
    case '!':  s->link = LK_NONE; return;       /* image marker */
    default:   break;
    }
    s->link = LK_NONE;
    md_emit(s, c);
}

/* ===================== rtf ============================================ */

static const struct { const char *w; ts_charset cs; } rtf_cp[] = {
    { "ansicpg1250", TS_CS_ISO8859_2 }, { "ansicpg1251", TS_CS_CP1251 },
    { "ansicpg1252", TS_CS_CP1252 },    { "ansicpg1253", TS_CS_ISO8859_7 },
    { "ansicpg1254", TS_CS_ISO8859_9 }, { "ansicpg65001", TS_CS_UTF8 },
    { NULL, TS_CS_AUTO }
};

static const char *rtf_skip_dest[] = {
    "fonttbl", "colortbl", "stylesheet", "info", "pict", "object",
    "themedata", "colorschememapping", "latentstyles", "datastore",
    "generator", "listtable", "listoverridetable", "rsidtbl", "xmlnstbl",
    "header", "footer", "footnote", "filetbl", "upr", NULL
};

static int is_skip_dest(const char *w)
{
    int i;
    for (i = 0; rtf_skip_dest[i]; i++)
        if (strcmp(rtf_skip_dest[i], w) == 0) return 1;
    return 0;
}

static void rtf_char(flt_st *s, uint32_t cp)
{
    if (s->skip_depth) return;
    if (s->uc_skip < 0) { s->uc_skip++; return; }   /* swallowed by \uN */
    ts_emit_cp(&s->out, cp);
}

static void rtf_ctrl_done(flt_st *s, int has_arg, long arg)
{
    const char *w = s->ctrl_word;
    int i;

    if (!strcmp(w, "par") || !strcmp(w, "line") || !strcmp(w, "sect")) {
        if (!s->skip_depth) ts_emit(&s->out, (const uint8_t *)"\n", 1);
        return;
    }
    if (!strcmp(w, "tab")) { if (!s->skip_depth) ts_emit(&s->out, (const uint8_t *)"\t", 1); return; }
    if (!strcmp(w, "emdash")) { rtf_char(s, 0x2014); return; }
    if (!strcmp(w, "endash")) { rtf_char(s, 0x2013); return; }
    if (!strcmp(w, "lquote")) { rtf_char(s, 0x2018); return; }
    if (!strcmp(w, "rquote")) { rtf_char(s, 0x2019); return; }
    if (!strcmp(w, "ldblquote")) { rtf_char(s, 0x201C); return; }
    if (!strcmp(w, "rdblquote")) { rtf_char(s, 0x201D); return; }
    if (!strcmp(w, "bullet")) { rtf_char(s, 0x2022); return; }
    if (!strcmp(w, "uc") && has_arg) { s->uc_skip = (int)arg; return; }
    if (!strcmp(w, "u") && has_arg) {
        uint32_t cp = (arg < 0) ? (uint32_t)(arg + 65536) : (uint32_t)arg;
        rtf_char(s, cp);
        s->uc_skip = -s->uc_skip;               /* negative = swallow N chars */
        if (!s->uc_skip) s->uc_skip = 1;
        return;
    }
    for (i = 0; rtf_cp[i].w; i++)
        if (!strcmp(rtf_cp[i].w, w)) { s->cp = rtf_cp[i].cs; return; }
    if (is_skip_dest(w) && !s->skip_depth) s->skip_depth = s->depth;
}

static void rtf_feed(flt_st *s, uint8_t c)
{
    /* \'hh -- a raw byte in the document code page */
    if (s->hexd) {
        int d = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) { s->hexd = 0; }
        else {
            s->hexv = s->hexv * 16 + d;
            if (--s->hexd == 0)
                rtf_char(s, ts_sb_to_cp(s->cp, (uint8_t)s->hexv));
            return;
        }
    }

    if (s->ctrl) {
        int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        int digit = (c >= '0' && c <= '9') || (c == '-' && s->ctrl == 1);

        if (alpha && s->ctrl == 1) {
            if (s->ctrl_len < (int)sizeof s->ctrl_word - 1)
                s->ctrl_word[s->ctrl_len++] = (char)c;
            return;
        }
        if (digit) {
            if (s->ctrl_len < (int)sizeof s->ctrl_word - 1)
                s->ctrl_word[s->ctrl_len++] = (char)c;
            s->ctrl = 2;
            return;
        }
        {   /* control word finished */
            char *p = s->ctrl_word;
            long arg = 0;
            int has_arg = 0, neg = 0, i;

            s->ctrl_word[s->ctrl_len] = 0;
            for (i = 0; p[i]; i++)
                if (p[i] == '-' || (p[i] >= '0' && p[i] <= '9')) break;
            if (p[i]) {
                const char *q = p + i;
                has_arg = 1;
                if (*q == '-') { neg = 1; q++; }
                for (; *q >= '0' && *q <= '9'; q++) arg = arg * 10 + (*q - '0');
                if (neg) arg = -arg;
                p[i] = 0;
            }
            s->ctrl = 0; s->ctrl_len = 0;
            rtf_ctrl_done(s, has_arg, arg);
            if (c == ' ') return;               /* delimiter is consumed */
        }
        /* fall through: c still needs handling */
    }

    switch (c) {
    case '{':
        s->depth++;
        return;
    case '}':
        if (s->skip_depth && s->depth <= s->skip_depth) s->skip_depth = 0;
        if (s->depth) s->depth--;
        return;
    case '\\':
        s->ctrl = 1; s->ctrl_len = 0;
        return;
    case '\r': case '\n':
        return;                                 /* layout only */
    default:
        break;
    }

    if (s->ctrl_len == 0 && s->ctrl == 0) rtf_char(s, c);
}

/* The escape forms \\ \{ \} \* \'hh are one-character control symbols; they
 * arrive here as ctrl==1 with a non-alpha first byte. */
static void rtf_feed_outer(flt_st *s, uint8_t c)
{
    if (s->ctrl == 1 && s->ctrl_len == 0) {
        switch (c) {
        case '\\': case '{': case '}':
            s->ctrl = 0; rtf_char(s, c); return;
        case '*':
            s->ctrl = 0;
            if (!s->skip_depth) s->skip_depth = s->depth;   /* \*\dest */
            return;
        case '\'':
            s->ctrl = 0; s->hexd = 2; s->hexv = 0; return;
        case '~':
            s->ctrl = 0; rtf_char(s, 0x00A0); return;
        case '-':
            s->ctrl = 0; rtf_char(s, 0x00AD); return;
        case '\n': case '\r':
            s->ctrl = 0;
            if (!s->skip_depth) ts_emit(&s->out, (const uint8_t *)"\n", 1);
            return;
        default:
            break;
        }
    }
    rtf_feed(s, c);
}

/* ===================== entry point ==================================== */

int ts_open_text(ts_ctx *c, ts_format fmt, ts_stream **out)
{
    ts_stream *file = ts_file_stream(c->arena, c->io, 0, -1);
    ts_stream *st;
    flt_st *s = NULL;

    if (!file) return TS_ERR_NOMEM;

    if (fmt == TS_FMT_RTF) {
        /* RTF is 7-bit ASCII on the wire; the filter does its own decoding. */
        st = flt_new(c->arena, file, rtf_feed_outer, &s);
        if (!st) return TS_ERR_NOMEM;
        s->cp = (c->cfg->charset != TS_CS_AUTO) ? c->cfg->charset : TS_CS_CP1252;
        *out = st;
        return TS_OK;
    }

    st = ts_charset_stream(c->arena, file, c->detected);
    if (!st) return TS_ERR_NOMEM;

    st = flt_new(c->arena, st,
                 (fmt == TS_FMT_MARKDOWN) ? md_feed : plain_feed, &s);
    if (!st) return TS_ERR_NOMEM;
    *out = st;
    return TS_OK;
}
