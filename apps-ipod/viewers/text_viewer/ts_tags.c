/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Turns tagged markup into flowing text: strips tags, resolves entities,
 * and applies block and whitespace rules.
 ****************************************************************************/

#include "ts_internal.h"

#define R_BLOCK 0x01    /* break the line before and after      */
#define R_BR    0x02    /* break the line, empty element        */
#define R_SKIP  0x04    /* drop this element's entire contents  */
#define R_PRE   0x08    /* preserve whitespace inside           */
#define R_TAB   0x10    /* emit a tab                           */
#define R_TEXT  0x20    /* DOCX: the element that carries text  */
#define R_BLANK 0x40    /* leave a blank line here              */
#define R_HEAD  0x80    /* a heading: its text is marked        */

typedef struct { const char *name; unsigned flags; } rule;

static const rule html_rules[] = {
    { "script", R_SKIP }, { "style", R_SKIP }, { "head", R_SKIP },
    { "noscript", R_SKIP }, { "svg", R_SKIP }, { "iframe", R_SKIP },
    { "pre", R_BLOCK | R_PRE },
    { "br", R_BR }, { "hr", R_BLANK },
    { "p", R_BLOCK }, { "div", R_BLOCK }, { "li", R_BLOCK },
    { "ul", R_BLOCK }, { "ol", R_BLOCK }, { "dl", R_BLOCK },
    { "dt", R_BLOCK }, { "dd", R_BLOCK },
    { "h1", R_BLOCK | R_HEAD }, { "h2", R_BLOCK | R_HEAD },
    { "h3", R_BLOCK | R_HEAD }, { "h4", R_BLOCK | R_HEAD },
    { "h5", R_BLOCK | R_HEAD }, { "h6", R_BLOCK | R_HEAD },
    { "tr", R_BLOCK }, { "table", R_BLOCK }, { "td", R_TAB }, { "th", R_TAB },
    { "blockquote", R_BLOCK }, { "section", R_BLOCK }, { "article", R_BLOCK },
    { "header", R_BLOCK }, { "footer", R_BLOCK }, { "nav", R_BLOCK },
    { "figure", R_BLOCK }, { "figcaption", R_BLOCK }, { "body", R_BLOCK },
    { NULL, 0 }
};

static const rule fb2_rules[] = {
    { "binary", R_SKIP }, { "description", R_SKIP }, { "stylesheet", R_SKIP },
    { "p", R_BLOCK }, { "v", R_BLOCK }, { "subtitle", R_BLOCK | R_HEAD },
    { "title", R_BLOCK | R_HEAD }, { "empty-line", R_BLANK },
    { "section", R_BLOCK },
    { "td", R_TAB }, { "th", R_TAB }, { "tr", R_BLOCK },
    { "text-author", R_BLOCK }, { "epigraph", R_BLOCK }, { "poem", R_BLOCK },
    { "cite", R_BLOCK }, { "annotation", R_BLOCK }, { "code", R_PRE },
    { NULL, 0 }
};

static const rule docx_rules[] = {
    { "w:t", R_TEXT },
    { "w:p", R_BLOCK }, { "w:br", R_BR }, { "w:cr", R_BR },
    { "w:tab", R_TAB }, { "w:tr", R_BLOCK },
    { "w:instrText", R_SKIP }, { "w:delText", R_SKIP },
    { "w:proofErr", R_SKIP }, { "w:bookmarkStart", R_SKIP },
    { NULL, 0 }
};

/* ---- entities -------------------------------------------------------- */

static const struct { const char *n; uint16_t cp; } ents[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
    { "apos", '\'' }, { "nbsp", 0x00A0 }, { "shy", 0x00AD },
    { "copy", 0x00A9 }, { "reg", 0x00AE }, { "trade", 0x2122 },
    { "deg", 0x00B0 }, { "plusmn", 0x00B1 }, { "middot", 0x00B7 },
    { "laquo", 0x00AB }, { "raquo", 0x00BB },
    { "ndash", 0x2013 }, { "mdash", 0x2014 },
    { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "sbquo", 0x201A },
    { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bdquo", 0x201E },
    { "dagger", 0x2020 }, { "Dagger", 0x2021 }, { "bull", 0x2022 },
    { "hellip", 0x2026 }, { "prime", 0x2032 }, { "Prime", 0x2033 },
    { "euro", 0x20AC }, { "pound", 0x00A3 }, { "yen", 0x00A5 },
    { "cent", 0x00A2 }, { "sect", 0x00A7 }, { "para", 0x00B6 },
    { "frac12", 0x00BD }, { "frac14", 0x00BC }, { "frac34", 0x00BE },
    { "times", 0x00D7 }, { "divide", 0x00F7 },
    { "eacute", 0x00E9 }, { "egrave", 0x00E8 }, { "agrave", 0x00E0 },
    { "ccedil", 0x00E7 }, { "uuml", 0x00FC }, { "ouml", 0x00F6 },
    { "auml", 0x00E4 }, { "szlig", 0x00DF }, { "ntilde", 0x00F1 },
    { "aacute", 0x00E1 }, { "iacute", 0x00ED }, { "oacute", 0x00F3 },
    { "uacute", 0x00FA }, { "ae", 0x00E6 }, { "oslash", 0x00F8 },
    { "aring", 0x00E5 }, { "emsp", 0x2003 }, { "ensp", 0x2002 },
    { "thinsp", 0x2009 }, { "larr", 0x2190 }, { "rarr", 0x2192 },
    { NULL, 0 }
};

size_t ts_entity(const char *s, size_t n, uint32_t *cp)
{
    size_t i;
    char name[16];

    for (i = 0; i < n && i < 12; i++)
        if (s[i] == ';') break;
    if (i == 0 || i >= n || i >= 12 || s[i] != ';') return 0;

    if (s[0] == '#') {
        uint32_t v = 0;
        size_t k = 1;
        int hex = (k < i && (s[k] == 'x' || s[k] == 'X'));

        if (hex) k++;
        if (k >= i) return 0;
        for (; k < i; k++) {
            char c = s[k];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            v = v * (hex ? 16 : 10) + (uint32_t)d;
            if (v > 0x10FFFF) return 0;
        }
        *cp = v ? v : 0xFFFD;
        return i + 1;
    }

    memcpy(name, s, i);
    name[i] = 0;
    for (size_t k = 0; ents[k].n; k++)
        if (strcmp(ents[k].n, name) == 0) { *cp = ents[k].cp; return i + 1; }
    return 0;
}

/* ---- state machine --------------------------------------------------- */

enum { ST_TEXT, ST_TAG, ST_COMMENT, ST_CDATA, ST_ENT };

#define TAGBUF 384      /* an <img> carries a path, and often alt text */
#define ENTBUF 16
#define TIN    256

typedef struct {
    bs_stream    *src;
    ts_tag_profile prof;
    const rule   *rules;
    int           collapse, max_nl;

    uint8_t in[TIN];
    size_t  ipos, ilen;
    int     src_eof;

    int   state;
    char  tag[TAGBUF];
    size_t tlen;
    int   quote, match;            /* match: run length of terminator seen */
    char  ent[ENTBUF];
    size_t elen;

    int   skip_depth, pre_depth, text_depth;
    char  skip_name[TAGBUF];

    int   nl_pending, sp_pending, started;
    int   pictures;                /* TS_PICTURE_OPEN lines for pictures */
    int   cover_done;              /* an FB2's coverpage picture is out */
    int   head_depth;              /* headings open */
    int   head_wait, head_out;     /* TS_HEADING_OPEN due, and written */
    char  anchor[64];              /* the id watched for, or "" */
    long  anchor_at;               /* where it was seen, or -1 */
    long  anchor_end;              /* where its element closed, or -1 */
    char  anchor_name[16];         /* that element, and how deep in it */
    int   anchor_depth, anchor_block;
    long  taken;                   /* bytes handed out since the rewind */
    int   notes;                   /* references marked, notes left out */
    int   show_notes;              /* notes kept in: one is being read */
    int   ref, ref_wait, ref_close; /* in a reference; its bracket due */
    char  ref_href[TS_NOTE_HREF];
    ts_pend out;
} tg_st;

static const rule *lookup(const rule *r, const char *name)
{
    for (; r->name; r++)
        if (ts_ascii_casecmp(r->name, name) == 0) return r;
    return NULL;
}

static int emitting(tg_st *s)
{
    if (s->skip_depth) return 0;
    if (s->prof == TS_TAGS_DOCX && !s->text_depth) return 0;
    return 1;
}

/* Deferred whitespace keeps runs of markup from turning into runs of blank
 * lines, and drops leading whitespace at the top of the document. */
static void flush_ws(tg_st *s)
{
    int n = s->nl_pending;

    if (!s->started) { s->nl_pending = s->sp_pending = 0; return; }
    if (n > s->max_nl) n = s->max_nl;
    while (n--) ts_emit(&s->out, (const uint8_t *)"\n", 1);
    if (!s->nl_pending && s->sp_pending)
        ts_emit(&s->out, (const uint8_t *)" ", 1);
    s->nl_pending = s->sp_pending = 0;
}

/* A heading's opening mark goes just before its first character, so it
 * shares that character's line */
static void head_text(tg_st *s)
{
    if (!s->head_wait) return;
    s->head_wait = 0;
    s->head_out = 1;
    ts_emit_cp(&s->out, TS_HEADING_OPEN);
}

/* A note reference is bracketed unless its text already is, so a
 * superscript 1 does not read as part of the word before it */
static void ref_text(tg_st *s, uint32_t c)
{
    if (!s->ref_wait || c >= 0xE000) return;
    s->ref_wait = 0;
    if (c != '[' && c != '(') {
        ts_emit(&s->out, (const uint8_t *)"[", 1);
        s->ref_close = 1;
    }
}

/* Entity replacements arrive as codepoints and must be encoded... */
static void out_cp(tg_st *s, uint32_t cp)
{
    flush_ws(s);
    s->started = 1;
    head_text(s);
    ref_text(s, cp);
    ts_emit_cp(&s->out, cp);
}

/* ...but the document body is already UTF-8 from the charset stage, so its
 * bytes pass through untouched. Encoding them again would double-encode
 * every non-ASCII character. */
static void out_byte(tg_st *s, uint8_t b)
{
    flush_ws(s);
    s->started = 1;
    ref_text(s, b);
    head_text(s);
    ts_emit(&s->out, &b, 1);
}

static void want_nl(tg_st *s, int count)
{
    if (!s->started) return;
    if (count > s->nl_pending) s->nl_pending = count;
    s->sp_pending = 0;
}

/* The value of attribute `name` in tag body `t`, or 0 */
static int tag_attr(const char *t, const char *name, char *out, size_t cap)
{
    size_t l = strlen(name);
    const char *p;

    for (p = t; *p; p++) {
        size_t j = 0;
        char q;

        if (p != t && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n')
            continue;
        if (ts_ascii_ncasecmp(p, name, l) || p[l] != '=') continue;
        q = p[l + 1];
        if (q != '"' && q != '\'') continue;
        for (p += l + 2; *p && *p != q && j < cap - 1; p++) out[j++] = *p;
        out[j] = 0;
        return 1;
    }
    return 0;
}

/* A picture, on a line of its own */
static void out_picture(tg_st *s, const char *href)
{
    int wait = s->head_wait;            /* a picture line takes no mark */

    s->head_wait = 0;
    if (s->started && s->nl_pending < 1) s->nl_pending = 1;
    s->sp_pending = 0;
    out_cp(s, TS_PICTURE_OPEN);
    for (; *href; href++) out_byte(s, (uint8_t)*href);
    out_cp(s, TS_PICTURE_CLOSE);
    want_nl(s, 1);
    s->head_wait = wait;
}

static int has_word(const char *v, const char *w)
{
    size_t l = strlen(w);

    for (; *v; v++)
        if (ts_ascii_ncasecmp(v, w, l) == 0) return 1;
    return 0;
}

/* Whether <a> body `t` is a note reference, its target in href */
static int note_ref(const tg_st *s, const char *t, char *href, size_t cap)
{
    char v[64];
    const char *frag;

    if (!tag_attr(t, "href", href, cap) && !tag_attr(t, "l:href", href, cap)
        && !tag_attr(t, "xlink:href", href, cap))
        return 0;
    frag = strchr(href, '#');
    if (!frag || !frag[1]) return 0;
    frag++;
    /* a note's link back to its reference */
    if (!ts_ascii_ncasecmp(frag, "fnanchor", 8)
        || !ts_ascii_ncasecmp(frag, "noteref", 7)
        || !ts_ascii_ncasecmp(frag, "ref", 3))
        return 0;
    if (tag_attr(t, "epub:type", v, sizeof v) && has_word(v, "noteref"))
        return 1;
    if (s->prof == TS_TAGS_FB2)
        return tag_attr(t, "type", v, sizeof v)
               && !ts_ascii_casecmp(v, "note");
    if (tag_attr(t, "class", v, sizeof v)
        && (has_word(v, "noteref") || has_word(v, "fnanchor")))
        return 1;
    return !ts_ascii_ncasecmp(frag, "footnote", 8)
           || !ts_ascii_ncasecmp(frag, "note", 4)
           || (!ts_ascii_ncasecmp(frag, "fn", 2)
               && frag[2] >= '0' && frag[2] <= '9');
}

/* Whether the element is a note to leave out of the flow: an EPUB's
 * footnote, an FB2's notes */
static int note_body(const tg_st *s, const char *t, const char *name)
{
    char v[64];

    if (s->prof == TS_TAGS_FB2)
        return ts_ascii_casecmp(name, "body") == 0
               && tag_attr(t, "name", v, sizeof v)
               && !ts_ascii_casecmp(v, "notes");
    return tag_attr(t, "epub:type", v, sizeof v) && has_word(v, "footnote");
}

/* The end of a reference: its closing bracket, then the mark that carries
 * its target */
static void ref_end(tg_st *s)
{
    const char *h;

    if (s->ref_wait) out_byte(s, '*');      /* no text: a marker of its own */
    if (s->ref_close) out_byte(s, ']');
    out_cp(s, TS_NOTE_OPEN);
    for (h = s->ref_href; *h; h++) {     /* no space: a line breaks there */
        if (*h == ' ') {
            out_byte(s, '%'); out_byte(s, '2'); out_byte(s, '0');
        } else
            out_byte(s, (uint8_t)*h);
    }
    out_cp(s, TS_NOTE_CLOSE);
    s->ref = s->ref_wait = s->ref_close = 0;
}

static void process_tag(tg_st *s)
{
    char name[TAGBUF];
    size_t i = 0, n = 0;
    int close = 0, self = 0;
    const rule *r;
    const char *t = s->tag;

    if (s->tlen && t[s->tlen - 1] == '/') self = 1;
    if (s->tlen && t[0] == '/') { close = 1; i = 1; }
    if (s->tlen && (t[0] == '!' || t[0] == '?')) return;   /* decl / PI */

    while (i < s->tlen && n < sizeof name - 1) {
        char c = t[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/' ) break;
        name[n++] = c;
        i++;
    }
    name[n] = 0;
    if (!n) return;

    if (s->anchor[0] && s->anchor_at < 0 && !close) {
        char id[64];

        if ((tag_attr(t, "id", id, sizeof id) ||
             (ts_ascii_casecmp(name, "a") == 0 &&
              tag_attr(t, "name", id, sizeof id))) &&
            !strcmp(id, s->anchor)) {
            const rule *r = lookup(s->rules, name);

            s->anchor_at = s->taken + (long)(s->out.tail - s->out.head);
            strncpy(s->anchor_name, name, sizeof s->anchor_name - 1);
            s->anchor_name[sizeof s->anchor_name - 1] = 0;
            s->anchor_depth = self ? 0 : 1;
            s->anchor_block = (r && (r->flags & R_BLOCK))
                              || ts_ascii_casecmp(name, "aside") == 0;
            if (self) s->anchor_end = s->anchor_at;
        }
    } else if (s->anchor_at >= 0 && s->anchor_end < 0 && !self
               && ts_ascii_casecmp(name, s->anchor_name) == 0) {
        if (!close)
            s->anchor_depth++;
        else if (--s->anchor_depth == 0)
            s->anchor_end = s->taken + (long)(s->out.tail - s->out.head);
    }

    /* note references, and the notes left out of the flow */
    if (s->notes && !s->skip_depth) {
        if (ts_ascii_casecmp(name, "a") == 0) {
            if (close && s->ref)
                ref_end(s);
            else if (!close && !self && !s->ref
                     && note_ref(s, t, s->ref_href, sizeof s->ref_href))
                s->ref = s->ref_wait = 1;
        } else if (!close && !self && !s->show_notes
                   && note_body(s, t, name)) {
            s->skip_depth = 1;
            memcpy(s->skip_name, name, n + 1);
            return;
        }
    }

    /* A picture: <img src> (a MOBI's <img recindex>), or the <image> an
     * <svg> wraps; in an FB2, an
     * <image> of the text, and the first of its coverpage, which is in
     * the skipped <description> */
    if (s->pictures && !close) {
        char href[256];
        int fb2 = s->prof == TS_TAGS_FB2 &&
                  ts_ascii_casecmp(name, "image") == 0 &&
                  (!s->skip_depth ||
                   (!s->cover_done &&
                    ts_ascii_casecmp(s->skip_name, "description") == 0));

        if ((!s->skip_depth && ts_ascii_casecmp(name, "img") == 0 &&
             (tag_attr(t, "src", href, sizeof href) ||
              tag_attr(t, "recindex", href, sizeof href))) ||
            (s->skip_depth && ts_ascii_casecmp(s->skip_name, "svg") == 0 &&
             ts_ascii_casecmp(name, "image") == 0 &&
             (tag_attr(t, "xlink:href", href, sizeof href) ||
              tag_attr(t, "href", href, sizeof href))) ||
            (fb2 && (tag_attr(t, "l:href", href, sizeof href) ||
                     tag_attr(t, "xlink:href", href, sizeof href) ||
                     tag_attr(t, "href", href, sizeof href)))) {
            int depth = s->skip_depth;

            if (depth && fb2) s->cover_done = 1;
            s->skip_depth = 0;          /* out_picture writes */
            out_picture(s, href);
            s->skip_depth = depth;
            return;
        }
    }

    /* Inside a skipped element only its own close tag matters. */
    if (s->skip_depth) {
        if (ts_ascii_casecmp(name, s->skip_name) == 0)
            s->skip_depth += close ? -1 : (self ? 0 : 1);
        return;
    }

    r = lookup(s->rules, name);
    if (!r) return;

    if (r->flags & R_SKIP) {
        if (!close && !self) {
            s->skip_depth = 1;
            memcpy(s->skip_name, name, n + 1);
        }
        return;
    }
    if (r->flags & R_TEXT) {
        if (self) return;
        s->text_depth += close ? -1 : 1;
        if (s->text_depth < 0) s->text_depth = 0;
        return;
    }
    /* The closing mark goes before the line break the close asks for */
    if ((r->flags & R_HEAD) && !self) {
        if (!close) {
            if (s->head_depth++ == 0) s->head_wait = 1;
        } else if (s->head_depth > 0 && --s->head_depth == 0) {
            if (s->head_out) ts_emit_cp(&s->out, TS_HEADING_CLOSE);
            s->head_wait = s->head_out = 0;
        }
    }
    /* An empty <pre/> holds nothing to keep the spacing of */
    if ((r->flags & R_PRE) && !self) s->pre_depth += close ? -1 : 1;
    if (s->pre_depth < 0) s->pre_depth = 0;

    if (r->flags & R_BLANK) { want_nl(s, 2); return; }

    if (r->flags & (R_BLOCK | R_BR)) {
        if (s->prof == TS_TAGS_DOCX && !close && !self &&
            ts_ascii_casecmp(name, "w:p") == 0)
            return;                          /* break after the paragraph only */
        /* a heading stands apart: a blank line before it and after */
        want_nl(s, (r->flags & R_HEAD) && !self ? 2 : 1);
    }
    /* A tab separates cells, so it is pointless at the head of a line. */
    if ((r->flags & R_TAB) && !close && s->started && !s->nl_pending)
        out_cp(s, '\t');
}

static void feed(tg_st *s, uint8_t c)
{
    switch (s->state) {

    case ST_TEXT:
        if (c == '<') { s->state = ST_TAG; s->tlen = 0; s->quote = 0; s->match = 0; break; }
        if (c == '&') { s->state = ST_ENT; s->elen = 0; break; }
        if (!emitting(s)) break;
        if ((c == ' ' || c == '\t' || c == '\n' || c == '\r') &&
            s->collapse && !s->pre_depth) {
            s->sp_pending = 1;
            break;
        }
        if (!s->collapse || s->pre_depth) {
            if (c == '\n') { want_nl(s, 1); break; }
            if (c == '\r') break;
        }
        out_byte(s, c);
        break;

    case ST_TAG:
        if (s->quote) {
            if (c == s->quote) s->quote = 0;
        } else if (c == '"' || c == '\'') {
            s->quote = c;
        } else if (c == '>') {
            s->tag[s->tlen < TAGBUF ? s->tlen : TAGBUF - 1] = 0;
            process_tag(s);
            s->state = ST_TEXT;
            break;
        }
        if (s->tlen < TAGBUF - 1) s->tag[s->tlen++] = (char)c;
        if (s->tlen == 3 && !memcmp(s->tag, "!--", 3)) {
            s->state = ST_COMMENT; s->match = 0;
        } else if (s->tlen == 8 && !memcmp(s->tag, "![CDATA[", 8)) {
            s->state = ST_CDATA; s->match = 0;
        }
        break;

    case ST_COMMENT:
        if (c == '-') { if (s->match < 2) s->match++; }
        else if (c == '>' && s->match >= 2) s->state = ST_TEXT;
        else s->match = 0;
        break;

    case ST_CDATA:
        if (c == ']') { if (s->match < 2) s->match++; break; }
        if (c == '>' && s->match >= 2) { s->state = ST_TEXT; s->match = 0; break; }
        while (s->match) { if (emitting(s)) out_byte(s, ']'); s->match--; }
        if (emitting(s)) out_byte(s, c);
        break;

    case ST_ENT: {
        uint32_t cp;
        size_t used;

        if (s->elen < ENTBUF - 1) s->ent[s->elen++] = (char)c;
        if (c == ';' || s->elen >= ENTBUF - 1 ||
            c == '<' || c == ' ' || c == '&') {
            used = ts_entity(s->ent, s->elen, &cp);
            if (used) {
                if (emitting(s)) out_cp(s, cp);
            } else if (emitting(s)) {
                size_t k;
                out_byte(s, '&');
                for (k = 0; k < s->elen; k++) out_byte(s, (uint8_t)s->ent[k]);
            }
            s->state = ST_TEXT;
            if (!used && (c == '<' || c == '&')) {
                /* the terminator was really the start of something else */
                feed(s, c);
            }
        }
        break;
    }
    }
}

static int fill(tg_st *s)
{
    if (s->ipos < s->ilen) return BS_OK;
    if (s->src_eof) return BS_OK;
    s->ipos = s->ilen = 0;
    {
        size_t got = 0;
        int rc = bs_pull(s->src, s->in, TIN, &got);
        if (rc != BS_OK) return rc;
        if (!got) s->src_eof = 1;
        s->ilen = got;
    }
    return BS_OK;
}

/* Counted as it goes: a tag fed mid-pull reads the count for an anchor */
static size_t take(tg_st *s, uint8_t *buf, size_t n)
{
    size_t got = ts_pend_take(&s->out, buf, n);

    s->taken += (long)got;
    return got;
}

static int tg_pull(bs_stream *st, uint8_t *buf, size_t n, size_t *out)
{
    tg_st *s = st->st;
    size_t done = 0;

    while (done < n) {
        int rc;
        done += take(s, buf + done, n - done);
        if (done == n) break;
        if (ts_pend_free(&s->out) < 16) continue;

        rc = fill(s);
        if (rc != BS_OK) return rc;
        if (s->ipos >= s->ilen) {           /* end of input */
            done += take(s, buf + done, n - done);
            break;
        }
        feed(s, s->in[s->ipos++]);
    }
    *out = done;
    return BS_OK;
}

static int tg_reset(bs_stream *st)
{
    tg_st *s = st->st;
    bs_stream *src = s->src;
    ts_tag_profile prof = s->prof;
    const rule *rules = s->rules;
    int collapse = s->collapse, max_nl = s->max_nl;
    int rc = src->reset ? src->reset(src) : BS_ERR_UNSUP;

    int pictures = s->pictures, notes = s->notes;

    if (rc != BS_OK) return rc;
    memset(s, 0, sizeof *s);
    s->src = src; s->prof = prof; s->rules = rules;
    s->collapse = collapse; s->max_nl = max_nl;
    s->pictures = pictures; s->notes = notes;
    return BS_OK;
}

/* Lets a container back end point the same filter at the next document in a
 * spine without reallocating (and without losing the emitted-text state). */
void ts_tags_restart(bs_stream *st, bs_stream *src)
{
    tg_st *s = st->st;

    s->src = src;
    s->ipos = s->ilen = 0;
    s->src_eof = 0;
    s->state = ST_TEXT;
    s->tlen = s->elen = 0;
    s->skip_depth = s->pre_depth = s->text_depth = 0;
    s->head_depth = s->head_wait = s->head_out = 0;
    s->quote = s->match = 0;
    s->ref = s->ref_wait = s->ref_close = 0;
    if (s->started) s->nl_pending = 2;      /* blank line between documents */
}

void ts_tags_pictures(bs_stream *st, int on)
{
    ((tg_st *)st->st)->pictures = on;
}

void ts_tags_notes(bs_stream *st, int on)
{
    ((tg_st *)st->st)->notes = on;
}

void ts_tags_show_notes(bs_stream *st, int on)
{
    ((tg_st *)st->st)->show_notes = on;
}

/* Clears the between-documents state so a container back end can start its
 * spine over from the top without reallocating. */
void ts_tags_rewind(bs_stream *st)
{
    tg_st *s = st->st;

    s->started = 0;
    s->nl_pending = s->sp_pending = 0;
    s->taken = 0;
    ts_pend_reset(&s->out);     /* text not yet taken would shift offsets */
}

void ts_tags_anchor(bs_stream *st, const char *anchor)
{
    tg_st *s = st->st;

    strncpy(s->anchor, anchor, sizeof s->anchor - 1);
    s->anchor[sizeof s->anchor - 1] = 0;
    s->anchor_at = s->anchor_end = -1;
    s->anchor_depth = 0;
}

long ts_tags_anchor_at(const bs_stream *st)
{
    return ((const tg_st *)st->st)->anchor_at;
}

long ts_tags_anchor_end(const bs_stream *st, int *block)
{
    const tg_st *s = st->st;

    *block = s->anchor_block;
    return s->anchor_end;
}

bs_stream *ts_tags_stream(bs_arena *a, bs_stream *src, ts_tag_profile p,
                          const ts_config *cfg)
{
    bs_stream *st = bs_alloc(a, sizeof *st);
    tg_st *s = bs_alloc(a, sizeof *s);

    if (!st || !s) return NULL;
    s->src = src;
    s->prof = p;
    s->rules = (p == TS_TAGS_FB2) ? fb2_rules
             : (p == TS_TAGS_DOCX) ? docx_rules : html_rules;
    s->collapse = cfg->collapse_whitespace;
    s->max_nl = cfg->max_blank_lines;
    if (s->max_nl > 8) s->max_nl = 8;
    st->pull = tg_pull; st->reset = tg_reset; st->st = s;
    return st;
}
