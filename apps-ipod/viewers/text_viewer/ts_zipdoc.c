/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The zip-backed documents: EPUB, DOCX and .fb2.zip. Finds the content
 * parts inside the container and feeds them through the markup path.
 ****************************************************************************/

#include "ts_internal.h"

#define MAX_SPINE 2048
#define TAGBUF    1024

/* ---- tiny XML tag scanner -------------------------------------------- */

/* Calls cb() with the raw text of each tag (without the angle brackets).
 * Returning non-zero from cb stops the scan early. */
static int scan_tags(ts_stream *src, int (*cb)(void *, const char *), void *ud)
{
    char tag[TAGBUF];
    uint8_t buf[256];
    size_t n = 0, i;
    int in_tag = 0, quote = 0, stop = 0;

    for (;;) {
        size_t got = 0;
        int rc = ts_pull(src, buf, sizeof buf, &got);
        if (rc != TS_OK) return rc;
        if (!got) break;

        for (i = 0; i < got && !stop; i++) {
            char c = (char)buf[i];
            if (!in_tag) {
                if (c == '<') { in_tag = 1; n = 0; quote = 0; }
                continue;
            }
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '"' || c == '\'') quote = c;
            else if (c == '>') {
                tag[n] = 0;
                in_tag = 0;
                if (cb(ud, tag)) stop = 1;
                continue;
            }
            if (n < TAGBUF - 1) tag[n++] = c;
        }
        if (stop) break;
    }
    return TS_OK;
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

static void url_unescape(char *s)
{
    char *d = s;
    while (*s) {
        if (s[0] == '%' && s[1] && s[2]) {
            int hi = -1, lo = -1, i;
            for (i = 0; i < 2; i++) {
                char c = s[1 + i];
                int v = (c >= '0' && c <= '9') ? c - '0'
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (i == 0) hi = v; else lo = v;
            }
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
    ts_arena *a;
    ts_zip   *z;
    const char *opf;
    mitem    *manifest;
    int      *spine;
    int       nspine;
    int       in_spine;
    int       nomem;
} opf_ctx;

static char *dup_str(ts_arena *a, const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = ts_alloc(a, n);
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

            m = ts_alloc(o->a, sizeof *m);
            if (!m) { o->nomem = 1; return 1; }
            m->id = dup_str(o->a, id);
            if (!m->id) { o->nomem = 1; return 1; }
            m->zi = zi;
            m->next = o->manifest;
            o->manifest = m;
        }
        return 0;
    }

    if (tag_is(tag, "spine")) { o->in_spine = (tag[0] != '/'); return 0; }

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
    ts_stream *cs, *tags;
    int       *spine;
    int        nspine, cur;
    int        opened;
} sp_st;

static int sp_next(sp_st *s)
{
    ts_stream *member;
    int rc;

    if (s->cur >= s->nspine) return 0;
    rc = ts_zip_member(s->z, s->spine[s->cur++], &member);
    if (rc != TS_OK) return rc;               /* negative, or 0 */
    ts_charset_restart(s->cs, member, TS_CS_AUTO);
    ts_tags_restart(s->tags, s->cs);
    s->opened = 1;
    return 1;
}

static int sp_pull(ts_stream *st, uint8_t *buf, size_t n, size_t *out)
{
    sp_st *s = st->st;
    size_t done = 0;

    while (done < n) {
        size_t got = 0;
        int rc;

        if (!s->opened) {
            rc = sp_next(s);
            if (rc < 0) return rc;
            if (rc == 0) break;               /* spine exhausted */
        }
        rc = ts_pull(s->tags, buf + done, n - done, &got);
        if (rc != TS_OK) {
            /* A damaged chapter shouldn't cost the reader the whole book. */
            if (rc == TS_ERR_FORMAT) { s->opened = 0; continue; }
            return rc;
        }
        if (!got) { s->opened = 0; continue; }
        done += got;
    }
    *out = done;
    return TS_OK;
}

static int sp_reset(ts_stream *st)
{
    sp_st *s = st->st;

    s->cur = 0;
    s->opened = 0;
    ts_tags_rewind(s->tags);
    return TS_OK;
}

/* ---- entry point ----------------------------------------------------- */

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && ts_ascii_casecmp(s + a - b, suf) == 0;
}

int ts_open_epub(ts_ctx *c, ts_stream **out)
{
    ts_zip *z;
    ts_stream *st, *member;
    sp_st *sp;
    int rc, i, idx;
    char opf[256];
    ts_tag_profile prof;
    int *spine;

    rc = ts_zip_open(&z, c->arena, c->io);
    if (rc != TS_OK) return rc;

    spine = ts_alloc(c->arena, sizeof(int) * MAX_SPINE);
    st = ts_alloc(c->arena, sizeof *st);
    sp = ts_alloc(c->arena, sizeof *sp);
    if (!spine || !st || !sp) return TS_ERR_NOMEM;

    sp->z = z; sp->spine = spine; sp->nspine = 0; sp->cur = 0; sp->opened = 0;

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
        if (rc != TS_OK) return rc;
        rc = scan_tags(member, container_tag, opf);
        if (rc != TS_OK) return rc;
        if (!opf[0]) return TS_ERR_FORMAT;

        idx = ts_zip_find(z, opf);
        if (idx < 0) return TS_ERR_FORMAT;
        rc = ts_zip_member(z, idx, &member);
        if (rc != TS_OK) return rc;

        memset(&o, 0, sizeof o);
        o.a = c->arena; o.z = z; o.opf = opf; o.spine = spine;
        rc = scan_tags(member, opf_tag, &o);
        if (rc != TS_OK) return rc;
        if (o.nomem) return TS_ERR_NOMEM;
        sp->nspine = o.nspine;

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
        if (!sp->nspine) return TS_ERR_UNSUP;
    }

    if (!sp->nspine) return TS_ERR_FORMAT;

    /* One charset+markup pipeline, re-pointed at each spine document. The
     * inner charset stage is fed by ts_tags_restart via the tags stage. */
    sp->cs = ts_charset_stream(c->arena, NULL, TS_CS_UTF8);
    if (!sp->cs) return TS_ERR_NOMEM;
    sp->tags = ts_tags_stream(c->arena, sp->cs, prof, c->cfg);
    if (!sp->tags) return TS_ERR_NOMEM;

    st->pull = sp_pull; st->reset = sp_reset; st->st = sp;
    c->detected = TS_CS_UTF8;      /* XML in OCF/OOXML containers is Unicode */
    *out = st;
    return TS_OK;
}

int ts_open_docx(ts_ctx *c, ts_stream **out) { return ts_open_epub(c, out); }
