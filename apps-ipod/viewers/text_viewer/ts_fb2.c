/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * What an FB2 keeps outside its text: its title, author and cover for the
 * book index, and where each picture lies. An FB2 holds every picture as
 * base64 text in a <binary> element after the body, named by its id.
 *
 *   tag scanner   - tags, the text before each, and where each ends
 *   book details  - title, first author and cover, for the book index
 *   pictures      - every <binary> by id, found in one pass and kept
 ****************************************************************************/

#include "ts_internal.h"
#include "files/zip_member.h"       /* MEMBER_BASE64 */

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

/* ---- tag scanner ----------------------------------------------------- */

#define FB2_TAG  256
#define FB2_TEXT 160

typedef int (*fb2_cb)(void *ud, const char *tag, const char *text,
                      bs_off_t end);

/* Calls cb() with each tag's text, the text before it cut at FB2_TEXT, and
 * the offset just past its '>'. Non-zero from cb stops the scan. */
static int fb2_scan(const bs_io *io, fb2_cb cb, void *ud)
{
    char tag[FB2_TAG], text[FB2_TEXT];
    uint8_t buf[512];
    size_t n = 0, t = 0;
    int in_tag = 0, quote = 0;
    bs_off_t off = 0;

    if (io->seek(io->ctx, 0, SEEK_SET) < 0) return BS_ERR_IO;
    for (;;) {
        long got = io->read(io->ctx, buf, sizeof buf), i;

        if (got < 0) return BS_ERR_IO;
        if (got == 0) return BS_OK;
        for (i = 0; i < got; i++) {
            char c = (char)buf[i];

            if (!in_tag) {
                if (c == '<') { in_tag = 1; n = 0; quote = 0; }
                else if (t < FB2_TEXT - 1) text[t++] = c;
                continue;
            }
            /* a comment's apostrophe opens no quote */
            if (quote) { if (c == quote) quote = 0; }
            else if ((c == '"' || c == '\'') && tag[0] != '!') quote = c;
            else if (c == '>') {
                tag[n] = 0;
                text[t] = 0;
                in_tag = 0;
                t = 0;
                if (cb(ud, tag, text, off + i + 1)) return BS_OK;
                continue;
            }
            if (n < FB2_TAG - 1) tag[n++] = c;
        }
        off += got;
    }
}

/* Whether `tag`, opening or closing, is element `name` */
static int fb2_is(const char *tag, const char *name)
{
    size_t l = strlen(name);

    if (tag[0] == '/') tag++;
    return !ts_ascii_ncasecmp(tag, name, l) &&
           (tag[l] == 0 || tag[l] == ' ' || tag[l] == '/' ||
            tag[l] == '\t' || tag[l] == '\n' || tag[l] == '\r');
}

/* The value of the first attribute in `tag` named `name` or ending in
 * ":name", or 0 */
static int fb2_attr(const char *tag, const char *name, char *out, size_t cap)
{
    size_t l = strlen(name);
    const char *p;

    for (p = tag + 1; *p; p++) {
        size_t j = 0;
        char q;

        if (p[-1] != ' ' && p[-1] != ':' && p[-1] != '\t' && p[-1] != '\n')
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

/* A picture's id from its href, "#id" */
static int fb2_href(const char *tag, char *out, size_t cap)
{
    if (!fb2_attr(tag, "href", out, cap) || out[0] != '#') return 0;
    memmove(out, out + 1, strlen(out));
    return out[0] != 0;
}

static uint32_t fb2_key(const char *id)
{
    uint32_t h = 2166136261u;

    for (; *id; id++) h = (h ^ (uint8_t)*id) * 16777619u;
    return h;
}

/* ---- book details ---------------------------------------------------- */

typedef struct {
    ts_book   *out;
    ts_charset cs;
    int        in_info, in_author, in_cover, authored;
    char       first[48], middle[48], last[48], nick[48];
    char       cover[64];
    bs_off_t   cover_at;
} fb2_book;

/* Text between tags into `dst` in UTF-8: entities decoded, a single-byte
 * charset converted, runs of space folded, trimmed */
static void fb2_text(char *dst, size_t cap, const char *src, ts_charset cs)
{
    size_t n = 0, len = strlen(src), i;
    int space = 0, single = cs != TS_CS_UTF8 && cs != TS_CS_AUTO;

    for (i = 0; i < len && n + 5 < cap; i++) {
        uint8_t c = (uint8_t)src[i];
        uint32_t cp;
        size_t used;

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = n > 0;
            continue;
        }
        if (space) { dst[n++] = ' '; space = 0; }
        if (c == '&' && (used = ts_entity(src + i + 1, len - i - 1, &cp))) {
            n += ts_utf8_put((uint8_t *)dst + n, cp);
            i += used;
            continue;
        }
        if (single && c >= 0x80)
            n += ts_utf8_put((uint8_t *)dst + n, ts_sb_to_cp(cs, c));
        else
            dst[n++] = (char)c;
    }
    dst[n] = 0;
}

/* `part` after `dst`'s text, a space between */
static void fb2_join(char *dst, size_t cap, const char *part)
{
    size_t n = strlen(dst), l = strlen(part);

    if (!l) return;
    if (n && n + 1 < cap) dst[n++] = ' ';
    if (n + l >= cap) l = cap - 1 - n;
    memcpy(dst + n, part, l);
    dst[n + l] = 0;
}

static int book_tag(void *ud, const char *tag, const char *text, bs_off_t end)
{
    fb2_book *b = ud;
    int close = tag[0] == '/';

    if (fb2_is(tag, "title-info")) { b->in_info = !close; return 0; }
    if (b->in_info) {
        if (fb2_is(tag, "book-title") && close && !b->out->title[0])
            fb2_text(b->out->title, sizeof b->out->title, text, b->cs);
        else if (fb2_is(tag, "author") && !b->authored) {
            if (!close) {
                b->in_author = 1;
                b->first[0] = b->middle[0] = b->last[0] = b->nick[0] = 0;
            } else if (b->in_author) {
                char *a = b->out->author;
                size_t cap = sizeof b->out->author;

                b->in_author = 0;
                fb2_join(a, cap, b->first);
                fb2_join(a, cap, b->middle);
                fb2_join(a, cap, b->last);
                if (!a[0]) fb2_join(a, cap, b->nick);
                b->authored = a[0] != 0;
            }
        } else if (b->in_author && close) {
            if (fb2_is(tag, "first-name"))
                fb2_text(b->first, sizeof b->first, text, b->cs);
            else if (fb2_is(tag, "middle-name"))
                fb2_text(b->middle, sizeof b->middle, text, b->cs);
            else if (fb2_is(tag, "last-name"))
                fb2_text(b->last, sizeof b->last, text, b->cs);
            else if (fb2_is(tag, "nickname"))
                fb2_text(b->nick, sizeof b->nick, text, b->cs);
        } else if (fb2_is(tag, "genre") && close) {
            /* a code, such as prose_classic */
            char s[TS_BOOK_SUBJECTS], *u;

            fb2_text(s, sizeof s, text, b->cs);
            for (u = s; *u; u++)
                if (*u == '_') *u = ' ';
            ts_book_subject(b->out, s);
        } else if (fb2_is(tag, "sequence") && !close &&
                   !b->out->series[0]) {
            char s[TS_BOOK_SERIES];

            if (fb2_attr(tag, "name", s, sizeof s))
                fb2_text(b->out->series, sizeof b->out->series, s, b->cs);
            if (fb2_attr(tag, "number", s, sizeof s))
                b->out->series_pos = ts_series_pos(s);
        } else if (fb2_is(tag, "coverpage")) {
            b->in_cover = !close;
        } else if (b->in_cover && !close && fb2_is(tag, "image") &&
                   !b->cover[0]) {
            fb2_href(tag, b->cover, sizeof b->cover);
        }
        return 0;
    }

    /* The cover's text, after the body */
    if (fb2_is(tag, "description") && close) return !b->cover[0];
    if (fb2_is(tag, "binary")) {
        char id[64];

        if (!close && fb2_attr(tag, "id", id, sizeof id) &&
            !strcmp(id, b->cover))
            b->cover_at = end;
        else if (close && b->cover_at >= 0) {
            b->out->cover_at = (long)b->cover_at;
            b->out->cover_csize =
                (uint32_t)(end - (bs_off_t)strlen(tag) - 2 - b->cover_at);
            b->out->cover_usize = 0;
            b->out->cover_method = MEMBER_BASE64;
            return 1;
        }
    }
    return 0;
}

/* The charset of the text, from its head: a UTF-16 FB2 is not read */
static int fb2_charset(const bs_io *io, ts_charset *cs)
{
    uint8_t head[512];
    size_t bom;
    long got;

    if (io->seek(io->ctx, 0, SEEK_SET) < 0) return BS_ERR_IO;
    got = io->read(io->ctx, head, sizeof head);
    if (got <= 0) return BS_ERR_IO;
    *cs = ts_charset_sniff(head, (size_t)got, TS_CS_AUTO, &bom);
    *cs = ts_charset_decl(head, (size_t)got, *cs);
    if (*cs == TS_CS_UTF16LE || *cs == TS_CS_UTF16BE) return BS_ERR_UNSUP;
    return BS_OK;
}

int ts_fb2_book(const bs_io *io, ts_book *out)
{
    fb2_book b;
    int rc;

    memset(out, 0, sizeof *out);
    out->cover_at = -1;
    out->parts = 1;
    memset(&b, 0, sizeof b);
    b.out = out;
    b.cover_at = -1;
    rc = fb2_charset(io, &b.cs);
    if (rc != BS_OK) return rc;
    return fb2_scan(io, book_tag, &b);
}

/* ---- pictures -------------------------------------------------------- */

typedef struct {
    ts_fb2_pictures *p;
    uint32_t key;
    bs_off_t at;
} pic_ctx;

static int pic_tag(void *ud, const char *tag, const char *text, bs_off_t end)
{
    pic_ctx *c = ud;
    ts_fb2_pictures *p = c->p;
    char id[64];

    (void)text;
    if (!fb2_is(tag, "binary")) return 0;
    if (tag[0] != '/') {
        c->at = -1;
        if (fb2_attr(tag, "id", id, sizeof id)) {
            c->key = fb2_key(id);
            c->at = end;
        }
    } else if (c->at >= 0 && p->n < TS_FB2_PICTURES) {
        p->pic[p->n].key = c->key;
        p->pic[p->n].at = c->at;
        p->pic[p->n].len = (uint32_t)(end - (bs_off_t)strlen(tag) - 2 - c->at);
        p->n++;
        c->at = -1;
    }
    return 0;
}

int ts_fb2_picture(const bs_io *io, ts_fb2_pictures *p, const char *href,
                   bs_off_t *at, uint32_t *len)
{
    uint32_t key;
    int i;

    if (href[0] != '#' || !href[1]) return BS_ERR_FORMAT;
    if (!p->scanned) {
        pic_ctx c = { p, 0, -1 };
        int rc = fb2_scan(io, pic_tag, &c);

        if (rc != BS_OK) return rc;
        p->scanned = 1;
    }
    key = fb2_key(href + 1);
    for (i = 0; i < p->n; i++)
        if (p->pic[i].key == key) {
            *at = p->pic[i].at;
            *len = p->pic[i].len;
            return BS_OK;
        }
    return BS_ERR_FORMAT;
}
