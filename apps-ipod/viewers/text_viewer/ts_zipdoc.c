/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The zip-backed documents: EPUB, DOCX and .fb2.zip. Finds the content
 * parts inside the container and feeds them through the markup path.
 *
 *   tag scanner   - tags, and the text between them, from a member
 *   OPF parsing   - the manifest, the spine, and where the contents are
 *   spine stream  - every part in order, or one part alone
 *   contents      - an EPUB's nav document or NCX, as (title, part) pairs
 *   book details  - title, author and cover, for the book index
 ****************************************************************************/

#include "ts_internal.h"

#define MAX_SPINE 2048
#define TAGBUF    1024

/* ---- tiny XML tag scanner -------------------------------------------- */

#define TEXTBUF   256

/* Calls cb() with the raw text of each tag (without the angle brackets) and
 * the text before it since the last tag, cut at TEXTBUF. Returning non-zero
 * from cb stops the scan early. */
static int scan_markup(bs_stream *src,
                       int (*cb)(void *, const char *, const char *), void *ud)
{
    char tag[TAGBUF], text[TEXTBUF];
    uint8_t buf[256];
    size_t n = 0, t = 0, i;
    int in_tag = 0, quote = 0, stop = 0;

    for (;;) {
        size_t got = 0;
        int rc = bs_pull(src, buf, sizeof buf, &got);
        if (rc != BS_OK) return rc;
        if (!got) break;

        for (i = 0; i < got && !stop; i++) {
            char c = (char)buf[i];
            if (!in_tag) {
                if (c == '<') { in_tag = 1; n = 0; quote = 0; }
                else if (t < TEXTBUF - 1) text[t++] = c;
                continue;
            }
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '"' || c == '\'') quote = c;
            else if (c == '>') {
                tag[n] = 0;
                text[t] = 0;
                in_tag = 0;
                t = 0;
                if (cb(ud, tag, text)) stop = 1;
                continue;
            }
            if (n < TAGBUF - 1) tag[n++] = c;
        }
        if (stop) break;
    }
    return BS_OK;
}

typedef struct {
    int (*cb)(void *, const char *);
    void *ud;
} tags_only;

static int tags_only_cb(void *ud, const char *tag, const char *text)
{
    tags_only *t = ud;
    (void)text;
    return t->cb(t->ud, tag);
}

/* scan_markup() for a caller that wants the tags alone */
static int scan_tags(bs_stream *src, int (*cb)(void *, const char *), void *ud)
{
    tags_only t = { cb, ud };
    return scan_markup(src, tags_only_cb, &t);
}

static int tag_is(const char *tag, const char *name)
{
    size_t l = strlen(name);
    /* accept both "item" and "opf:item" */
    const char *colon = NULL, *p;

    for (p = tag; *p && *p != ' ' && *p != '\t' && *p != '/'; p++)
        if (*p == ':') colon = p + 1;
    if (colon) tag = colon;
    if (ts_ascii_ncasecmp(tag, name, l)) return 0;
    return tag[l] == 0 || tag[l] == ' ' || tag[l] == '\t' ||
           tag[l] == '/' || tag[l] == '\n' || tag[l] == '\r';
}

/* Copies the value of attribute `name` out of a tag body. */
static int attr(const char *tag, const char *name, char *out, size_t cap)
{
    size_t l = strlen(name);
    const char *p = tag;

    for (; *p; p++) {
        size_t j = 0;
        char q;
        if (p != tag && p[-1] != ' ' && p[-1] != '\t') continue;
        if (ts_ascii_ncasecmp(p, name, l)) continue;
        if (p[l] != '=') continue;
        q = p[l + 1];
        if (q != '"' && q != '\'') continue;
        p += l + 2;
        while (*p && *p != q && j < cap - 1) out[j++] = *p++;
        out[j] = 0;
        return 1;
    }
    return 0;
}

/* ---- path handling --------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_unescape(char *s)
{
    char *d = s;
    while (*s) {
        if (s[0] == '%' && s[1] && s[2]) {
            int hi = hexval(s[1]), lo = hexval(s[2]);
            if (hi >= 0 && lo >= 0) { *d++ = (char)(hi * 16 + lo); s += 3; continue; }
        }
        *d++ = *s++;
    }
    *d = 0;
}

/* Resolves `href` against the directory of `base`, in place into `out`. */
static void path_join(const char *base, const char *href, char *out, size_t cap)
{
    const char *slash = strrchr(base, '/');
    size_t n = 0;
    char *frag;

    if (slash && href[0] != '/') {
        size_t dl = (size_t)(slash - base) + 1;
        if (dl > cap - 1) dl = cap - 1;
        memcpy(out, base, dl);
        n = dl;
    }
    while (*href && n < cap - 1) out[n++] = *href++;
    out[n] = 0;

    frag = strchr(out, '#');
    if (frag) *frag = 0;
    url_unescape(out);

    /* collapse "dir/../" */
    for (;;) {
        char *up = strstr(out, "../");
        char *prev;
        if (!up || up == out) break;
        prev = up - 1;
        if (prev > out) {
            char *sl;
            *prev = 0;
            sl = strrchr(out, '/');
            *prev = '/';
            if (!sl) { memmove(out, up + 3, strlen(up + 3) + 1); continue; }
            memmove(sl + 1, up + 3, strlen(up + 3) + 1);
        } else break;
    }
}

/* ---- opf parsing ----------------------------------------------------- */

/* A manifest entry costs one short id string and one int. The href is
 * resolved to a zip index the moment it is parsed and then discarded: on a
 * 2000-chapter book, keeping the hrefs instead would cost a quarter of a
 * megabyte, which is more than some targets can spare for the whole plugin. */
typedef struct mitem {
    char *id;
    int   zi;
    struct mitem *next;
} mitem;

typedef struct {
    bs_arena *a;
    ts_zip   *z;
    const char *opf;
    mitem    *manifest;
    int      *spine;
    int       nspine;
    int       in_spine;
    int       nomem;
    int       nav_zi;         /* EPUB 3's contents, or -1 */
    char      toc_id[64];     /* the spine's NCX, by manifest id */
} opf_ctx;

static char *dup_str(bs_arena *a, const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = bs_alloc(a, n);
    if (p) memcpy(p, s, n);
    return p;
}

static int opf_tag(void *ud, const char *tag)
{
    opf_ctx *o = ud;
    char id[64], href[256], type[64];

    if (tag_is(tag, "manifest") && tag[0] != '/') return 0;

    if (tag_is(tag, "item") && attr(tag, "href", href, sizeof href)) {
        /* Only the documents that can hold text are worth remembering. */
        if (attr(tag, "media-type", type, sizeof type) &&
            !strstr(type, "xhtml") && !strstr(type, "html") &&
            !strstr(type, "xml"))
            return 0;
        if (!attr(tag, "id", id, sizeof id)) return 0;
        {
            char full[320];
            mitem *m;
            int zi;

            path_join(o->opf, href, full, sizeof full);
            zi = ts_zip_find(o->z, full);
            if (zi < 0) return 0;               /* manifest lies; skip it */

            m = bs_alloc(o->a, sizeof *m);
            if (!m) { o->nomem = 1; return 1; }
            m->id = dup_str(o->a, id);
            if (!m->id) { o->nomem = 1; return 1; }
            m->zi = zi;
            m->next = o->manifest;
            o->manifest = m;
            if (attr(tag, "properties", type, sizeof type) &&
                strstr(type, "nav"))
                o->nav_zi = zi;
        }
        return 0;
    }

    if (tag_is(tag, "spine")) {
        o->in_spine = (tag[0] != '/');
        if (o->in_spine) attr(tag, "toc", o->toc_id, sizeof o->toc_id);
        return 0;
    }

    if (o->in_spine && tag_is(tag, "itemref") &&
        attr(tag, "idref", id, sizeof id)) {
        mitem *m;
        for (m = o->manifest; m; m = m->next) {
            if (strcmp(m->id, id)) continue;
            if (o->nspine < MAX_SPINE) o->spine[o->nspine++] = m->zi;
            break;
        }
    }
    return 0;
}

static int container_tag(void *ud, const char *tag)
{
    char *out = ud;
    if (tag_is(tag, "rootfile") && attr(tag, "full-path", out, 256)) {
        url_unescape(out);
        return 1;
    }
    return 0;
}

/* ---- spine stream ---------------------------------------------------- */

typedef struct {
    ts_zip    *z;
    bs_stream *cs, *tags;
    int       *spine;
    int        nspine, cur;
    int        opened;
    int        part;          /* the one spine item read, or -1 for all */
    int        toc_zi;        /* the contents' member, or -1 */
    int        toc_nav;       /* it is a nav document, not an NCX */
} sp_st;

static int sp_next(sp_st *s)
{
    bs_stream *member;
    int rc;

    if (s->cur >= s->nspine) return 0;
    rc = ts_zip_member(s->z, s->spine[s->cur++], &member);
    if (rc != BS_OK) return rc;               /* negative, or 0 */
    ts_charset_restart(s->cs, member, TS_CS_AUTO);
    ts_tags_restart(s->tags, s->cs);
    s->opened = 1;
    return 1;
}

static int sp_pull(bs_stream *st, uint8_t *buf, size_t n, size_t *out)
{
    sp_st *s = st->st;
    size_t done = 0;

    while (done < n) {
        size_t got = 0;
        int rc;

        if (!s->opened) {
            if (s->part >= 0 && s->cur > s->part)
                break;                        /* end of the chapter */
            rc = sp_next(s);
            /* A member that will not open is skipped, like a damaged one. */
            if (rc == BS_ERR_FORMAT || rc == BS_ERR_UNSUP) continue;
            if (rc < 0) return rc;
            if (rc == 0) break;               /* spine exhausted */
        }
        rc = bs_pull(s->tags, buf + done, n - done, &got);
        if (rc != BS_OK) {
            /* A damaged chapter shouldn't cost the reader the whole book. */
            if (rc == BS_ERR_FORMAT) { s->opened = 0; continue; }
            return rc;
        }
        if (!got) { s->opened = 0; continue; }
        done += got;
    }
    *out = done;
    return BS_OK;
}

/* Back to the start of the chapter being read, or of the book */
static int sp_reset(bs_stream *st)
{
    sp_st *s = st->st;

    s->cur = s->part >= 0 ? s->part : 0;
    s->opened = 0;
    ts_tags_rewind(s->tags);
    return BS_OK;
}

int ts_zipdoc_seek_part(bs_stream *st, int part)
{
    sp_st *s = st->st;

    if (part < 0 || part >= s->nspine) return BS_ERR_INVAL;
    s->part = part;
    return sp_reset(st);
}

int ts_zipdoc_parts(const bs_stream *st)
{
    return ((const sp_st *)st->st)->nspine;
}

long ts_zipdoc_part_size(const bs_stream *st, int part)
{
    const sp_st *s = st->st;
    return (part >= 0 && part < s->nspine)
           ? (long)ts_zip_size(s->z, s->spine[part]) : 0;
}

int ts_zipdoc_picture(const bs_stream *st, int part, const char *href,
                      bs_off_t *at, uint32_t *csize, uint32_t *usize,
                      int *method)
{
    const sp_st *s = st->st;
    char unescaped[256], full[320];
    size_t n = strlen(href);
    int idx;

    if (part < 0 || part >= s->nspine || n >= sizeof unescaped)
        return BS_ERR_INVAL;
    memcpy(unescaped, href, n + 1);
    url_unescape(unescaped);
    path_join(ts_zip_name(s->z, s->spine[part]), unescaped, full, sizeof full);
    idx = ts_zip_find(s->z, full);
    if (idx < 0) return BS_ERR_FORMAT;
    return ts_zip_extent(s->z, idx, at, csize, usize, method);
}

/* ---- contents -------------------------------------------------------- */

#define TITLE_MAX 128

typedef struct {
    sp_st *sp;
    const char *base;         /* the contents member's own path */
    int (*cb)(void *, const char *, int);
    void *ud;
    int in_toc;               /* inside <nav epub:type="toc"> */
    int in_title;             /* gathering a title */
    char href[256];
    char title[TITLE_MAX];
    size_t tlen;
    int stop;
} toc_ctx;

/* Appends `text` to the title, decoding entities and folding white space */
static void title_add(toc_ctx *t, const char *text)
{
    size_t n = strlen(text), i = 0;

    while (i < n) {
        uint32_t cp = (unsigned char)text[i];
        uint8_t u[4];
        size_t ul, used = 1;

        if (cp == '&') {
            size_t e = ts_entity(text + i + 1, n - i - 1, &cp);
            if (e) used = e + 1;
        }
        i += used;
        /* A new character only with room for all of it */
        if (t->tlen >= TITLE_MAX - 5 && !(used == 1 && cp >= 0x80 && cp < 0xc0))
            return;
        if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n') {
            if (t->tlen && t->title[t->tlen - 1] != ' ' &&
                t->tlen < TITLE_MAX - 1)
                t->title[t->tlen++] = ' ';
            continue;
        }
        if (cp >= 0x80 && used == 1) {
            /* a UTF-8 byte: copied as it is */
            if (t->tlen < TITLE_MAX - 1) t->title[t->tlen++] = (char)cp;
            continue;
        }
        ul = ts_utf8_put(u, cp);
        if (t->tlen + ul < TITLE_MAX) {
            memcpy(t->title + t->tlen, u, ul);
            t->tlen += ul;
        }
    }
}

/* Hands the gathered title on, with the part its href names */
static void title_emit(toc_ctx *t, const char *href)
{
    char full[320];
    int zi, part;

    while (t->tlen && t->title[t->tlen - 1] == ' ') t->tlen--;
    t->title[t->tlen] = 0;
    path_join(t->base, href, full, sizeof full);
    zi = ts_zip_find(t->sp->z, full);
    for (part = 0; zi >= 0 && part < t->sp->nspine; part++)
        if (t->sp->spine[part] == zi) {
            if (t->tlen && t->cb(t->ud, t->title, part)) t->stop = 1;
            break;
        }
    t->tlen = 0;
}

static int toc_tag(void *ud, const char *tag, const char *text)
{
    toc_ctx *t = ud;
    int end = tag[0] == '/';
    const char *name = tag + end;     /* tag_is() takes no closing slash */
    char type[64];

    if (t->in_title) title_add(t, text);

    if (t->sp->toc_nav) {
        if (tag_is(name, "nav")) {
            if (end) t->in_toc = 0;
            else t->in_toc = attr(tag, "epub:type", type, sizeof type) &&
                             strstr(type, "toc");
        } else if (t->in_toc && tag_is(name, "a")) {
            if (!end) {
                t->in_title = attr(tag, "href", t->href, sizeof t->href);
                t->tlen = 0;
            } else if (t->in_title) {
                t->in_title = 0;
                title_emit(t, t->href);
            }
        }
    } else {
        if (tag_is(name, "text")) {
            if (!end) { t->in_title = 1; t->tlen = 0; }
            else t->in_title = 0;
        } else if (!end && tag_is(name, "content") &&
                   attr(tag, "src", t->href, sizeof t->href)) {
            title_emit(t, t->href);
        }
    }
    return t->stop;
}

int ts_zipdoc_toc(bs_stream *st, int (*cb)(void *, const char *, int),
                  void *ud)
{
    sp_st *s = st->st;
    bs_stream *member;
    toc_ctx t;
    int rc;

    if (s->toc_zi < 0) return BS_ERR_UNSUP;
    memset(&t, 0, sizeof t);
    t.sp = s;
    t.base = ts_zip_name(s->z, s->toc_zi);
    t.cb = cb;
    t.ud = ud;
    rc = ts_zip_member(s->z, s->toc_zi, &member);
    /* The member the spine was reading is gone: a read now ends the part */
    s->opened = 0;
    if (s->part >= 0) s->cur = s->part + 1;
    if (rc != BS_OK) return rc;
    return scan_markup(member, toc_tag, &t);
}

/* ---- entry point ----------------------------------------------------- */

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && ts_ascii_casecmp(s + a - b, suf) == 0;
}

int ts_open_epub(ts_ctx *c, bs_stream **out)
{
    ts_zip *z;
    bs_stream *st, *member;
    sp_st *sp;
    int rc, i, idx;
    char opf[256];
    ts_tag_profile prof;
    int *spine;

    rc = ts_zip_open(&z, c->arena, c->io);
    if (rc != BS_OK) return rc;

    spine = bs_alloc(c->arena, sizeof(int) * MAX_SPINE);
    st = bs_alloc(c->arena, sizeof *st);
    sp = bs_alloc(c->arena, sizeof *sp);
    if (!spine || !st || !sp) return BS_ERR_NOMEM;

    sp->z = z; sp->spine = spine; sp->nspine = 0; sp->cur = 0; sp->opened = 0;
    sp->part = -1;
    sp->toc_zi = -1;

    /* --- which flavour of zip is this? --- */
    idx = ts_zip_find(z, "word/document.xml");
    if (idx >= 0) {
        prof = TS_TAGS_DOCX;
        spine[sp->nspine++] = idx;
        c->resolved = TS_FMT_DOCX;
    } else if ((idx = ts_zip_find(z, "META-INF/container.xml")) >= 0) {
        opf_ctx o;

        prof = TS_TAGS_HTML;
        c->resolved = TS_FMT_EPUB;

        opf[0] = 0;
        rc = ts_zip_member(z, idx, &member);
        if (rc != BS_OK) return rc;
        rc = scan_tags(member, container_tag, opf);
        if (rc != BS_OK) return rc;
        if (!opf[0]) return BS_ERR_FORMAT;

        idx = ts_zip_find(z, opf);
        if (idx < 0) return BS_ERR_FORMAT;
        rc = ts_zip_member(z, idx, &member);
        if (rc != BS_OK) return rc;

        memset(&o, 0, sizeof o);
        o.a = c->arena; o.z = z; o.opf = opf; o.spine = spine;
        o.nav_zi = -1;
        rc = scan_tags(member, opf_tag, &o);
        if (rc != BS_OK) return rc;
        if (o.nomem) return BS_ERR_NOMEM;
        sp->nspine = o.nspine;

        /* The contents: EPUB 3's nav document, else the spine's NCX */
        if (o.nav_zi >= 0) {
            sp->toc_zi = o.nav_zi;
            sp->toc_nav = 1;
        } else if (o.toc_id[0]) {
            mitem *m;
            for (m = o.manifest; m; m = m->next)
                if (!strcmp(m->id, o.toc_id)) sp->toc_zi = m->zi;
        }

        /* No usable spine: fall back to every xhtml member, in zip order. */
        if (!sp->nspine) {
            for (i = 0; i < ts_zip_count(z) && sp->nspine < MAX_SPINE; i++) {
                const char *nm = ts_zip_name(z, i);
                if (ends_with(nm, ".xhtml") || ends_with(nm, ".html") ||
                    ends_with(nm, ".htm"))
                    spine[sp->nspine++] = i;
            }
        }
    } else {
        prof = TS_TAGS_FB2;
        for (i = 0; i < ts_zip_count(z); i++)
            if (ends_with(ts_zip_name(z, i), ".fb2")) {
                spine[sp->nspine++] = i;
                c->resolved = TS_FMT_FB2;
                break;
            }
        if (!sp->nspine) return BS_ERR_UNSUP;
    }

    if (!sp->nspine) return BS_ERR_FORMAT;

    /* One charset+markup pipeline, re-pointed at each spine document. The
     * inner charset stage is fed by ts_tags_restart via the tags stage. */
    sp->cs = ts_charset_stream(c->arena, NULL, TS_CS_UTF8);
    if (!sp->cs) return BS_ERR_NOMEM;
    sp->tags = ts_tags_stream(c->arena, sp->cs, prof, c->cfg);
    if (!sp->tags) return BS_ERR_NOMEM;
    if (c->resolved == TS_FMT_EPUB) ts_tags_pictures(sp->tags, 1);

    st->pull = sp_pull; st->reset = sp_reset; st->st = sp;
    c->parts = st;
    c->detected = TS_CS_UTF8;      /* XML in OCF/OOXML containers is Unicode */
    *out = st;
    return BS_OK;
}

int ts_open_docx(ts_ctx *c, bs_stream **out) { return ts_open_epub(c, out); }

/* ---- book details ---------------------------------------------------- */

#define BOOK_IMAGES 32      /* image items remembered for a cover lookup */

typedef struct {
    char id[64];
    char href[256];
} book_img;

typedef struct {
    ts_book  *out;
    int       in_meta;
    char      cover_meta[64];     /* <meta name="cover" content=...> */
    char      cover_prop[256];    /* the item marked cover-image */
    char      cover_named[256];   /* an image item called cover */
    book_img *img;
    int       nimg;
} book_ctx;

/* `src` into `dst` of `cap` bytes, cut short if it must be */
static void book_copy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);

    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Text between tags into `dst`: entities decoded, runs of space folded,
 * trimmed */
static void book_text(char *dst, size_t cap, const char *src)
{
    size_t n = 0, len = strlen(src);
    int space = 0;
    size_t i;

    for (i = 0; i < len && n + 5 < cap; i++) {
        uint32_t cp;
        size_t used;
        char c = src[i];

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = n > 0;
            continue;
        }
        if (space) { dst[n++] = ' '; space = 0; }
        if (c == '&' && (used = ts_entity(src + i + 1, len - i - 1, &cp))) {
            n += ts_utf8_put((uint8_t *)dst + n, cp);
            i += used + 1;
            continue;
        }
        dst[n++] = c;
    }
    dst[n] = 0;
}

static int book_tag(void *ud, const char *tag, const char *text)
{
    book_ctx *b = ud;
    char id[64], href[256], type[64], value[64];

    if (tag_is(tag, "metadata")) { b->in_meta = tag[0] != '/'; return 0; }

    if (b->in_meta && tag[0] == '/') {
        const char *name = tag + 1;
        if (tag_is(name, "title") && !b->out->title[0])
            book_text(b->out->title, sizeof b->out->title, text);
        else if (tag_is(name, "creator") && !b->out->author[0])
            book_text(b->out->author, sizeof b->out->author, text);
        return 0;
    }

    if (tag_is(tag, "meta") && attr(tag, "name", value, sizeof value) &&
        !ts_ascii_casecmp(value, "cover"))
        attr(tag, "content", b->cover_meta, sizeof b->cover_meta);

    if (tag_is(tag, "item") && attr(tag, "href", href, sizeof href) &&
        attr(tag, "media-type", type, sizeof type) &&
        !ts_ascii_ncasecmp(type, "image/", 6)) {
        if (!attr(tag, "id", id, sizeof id)) id[0] = 0;
        if (attr(tag, "properties", type, sizeof type) &&
            strstr(type, "cover-image") && !b->cover_prop[0])
            book_copy(b->cover_prop, href, sizeof b->cover_prop);
        if (!b->cover_named[0] && (strstr(id, "cover") || strstr(href, "cover")))
            book_copy(b->cover_named, href, sizeof b->cover_named);
        if (b->nimg < BOOK_IMAGES) {
            book_copy(b->img[b->nimg].id, id, sizeof b->img[0].id);
            book_copy(b->img[b->nimg].href, href, sizeof b->img[0].href);
            b->nimg++;
        }
    }

    if (tag_is(tag, "itemref") && tag[0] != '/')
        b->out->parts++;
    return 0;
}

int ts_book_read(const bs_io *io, void *arena, size_t arena_size,
                 ts_book *out)
{
    bs_arena a;
    ts_zip *z;
    bs_stream *member;
    book_ctx b;
    char opf[256], full[320];
    const char *href = NULL;
    bs_off_t at;
    int rc, idx, i;

    memset(out, 0, sizeof *out);
    out->cover_at = -1;
    a.base = arena; a.size = arena_size; a.used = 0;

    rc = ts_zip_open(&z, &a, io);
    if (rc != BS_OK) return rc;
    idx = ts_zip_find(z, "META-INF/container.xml");
    if (idx < 0) return BS_ERR_FORMAT;
    opf[0] = 0;
    rc = ts_zip_member(z, idx, &member);
    if (rc != BS_OK) return rc;
    rc = scan_tags(member, container_tag, opf);
    if (rc != BS_OK) return rc;
    if (!opf[0] || (idx = ts_zip_find(z, opf)) < 0) return BS_ERR_FORMAT;

    memset(&b, 0, sizeof b);
    b.out = out;
    b.img = bs_alloc(&a, sizeof(book_img) * BOOK_IMAGES);
    if (!b.img) return BS_ERR_NOMEM;
    rc = ts_zip_member(z, idx, &member);
    if (rc != BS_OK) return rc;
    rc = scan_markup(member, book_tag, &b);
    if (rc != BS_OK) return rc;

    /* EPUB 3's cover-image, then EPUB 2's meta, then an image called cover */
    if (b.cover_prop[0])
        href = b.cover_prop;
    else if (b.cover_meta[0]) {
        for (i = 0; i < b.nimg; i++)
            if (!strcmp(b.img[i].id, b.cover_meta)) href = b.img[i].href;
    }
    if (!href && b.cover_named[0])
        href = b.cover_named;
    if (!href)
        return BS_OK;

    {
        char unescaped[256];
        book_copy(unescaped, href, sizeof unescaped);
        url_unescape(unescaped);
        path_join(opf, unescaped, full, sizeof full);
    }
    idx = ts_zip_find(z, full);
    if (idx < 0 ||
        ts_zip_extent(z, idx, &at, &out->cover_csize, &out->cover_usize,
                      &out->cover_method) != BS_OK)
        return BS_OK;
    out->cover_at = (long)at;
    return BS_OK;
}
