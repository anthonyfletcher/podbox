/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Back end for MOBI books (.mobi, .prc, .azw): a Palm database whose first
 * record holds the PalmDOC and MOBI headers, then the text in records that
 * each decompress alone, then the images.
 *
 *   container - the record table, the headers, EXTH
 *   records   - a text record read, its trailing entries cut, decompressed
 *   loading   - every text record once: where each starts in the text, the
 *               parts (split at <mbp:pagebreak>) and the guide's contents
 *   stream    - the text of one part, or the whole book, through the
 *               charset and HTML stages
 *   contents  - the guide's toc page, a list of filepos links
 *   details   - title, author and cover for the book index
 *
 * A locked book is refused (TS_ERR_LOCKED), as is HUFF/CDIC compression and
 * a KF8-only book; a joint MOBI/KF8 file is read through its MOBI half.
 ****************************************************************************/

#include "ts_internal.h"
#include "files/zip_member.h"       /* MEMBER_STORED */

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

#define REC_MAX     8192    /* a record as stored, or record 0's headers */
#define TEXT_MAX    8192    /* a text record decompressed */
#define PARTS_MAX   1024
#define TAG_MAX     256

typedef struct {
    const bs_io *io;
    uint32_t *rec;          /* each record's offset, and the file's end */
    int       nrec;
    int       comp;         /* 1 none, 2 PalmDOC */
    int       ntext;        /* records 1..ntext */
    uint16_t  extra;        /* the trailing-entry flags */
    uint32_t  first_image;  /* the first image record, or ~0 */
    ts_charset cs;
    uint32_t *text_at;      /* where text record i starts in the text */
    uint32_t *part_at;      /* where part i starts; part_at[nparts] = end */
    int       nparts;
    uint32_t  toc_at;       /* the guide's toc page, or ~0 */

    int       part;         /* the part read, or -1 for the whole book */
    uint32_t  pos, end;     /* the text left to read */
    int       cur;          /* the record in dec, or 0 */
    uint32_t  dec_len;
    uint8_t   raw[REC_MAX];
    uint8_t   dec[TEXT_MAX];
    bs_stream text;         /* [pos, end) of the text */
    bs_stream *cs_st, *tags;
} mobi_st;

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | p[2] << 8 | p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

/* Bytes of record i into m->raw, at most REC_MAX; -1 on a read error */
static long read_rec(mobi_st *m, int i)
{
    uint32_t len = m->rec[i + 1] - m->rec[i];

    if (len > REC_MAX)
        len = REC_MAX;
    if (m->io->seek(m->io->ctx, m->rec[i], SEEK_SET) < 0)
        return -1;
    return m->io->read(m->io->ctx, m->raw, len) == (long)len ? (long)len : -1;
}

/* ---- container ------------------------------------------------------- */

/* The record table, and the headers of record 0. BS_OK, or why the book
 * cannot be read. Record 0 is left in m->raw, its length in *r0len. */
static int open_container(mobi_st *m, bs_arena *a, const bs_io *io,
                          long *r0len)
{
    uint8_t h[78];
    uint8_t *r0;
    long len;
    int i;

    m->io = io;
    m->first_image = ~0u;
    m->toc_at = ~0u;
    m->cs = TS_CS_CP1252;
    if (io->seek(io->ctx, 0, SEEK_SET) < 0 || io->read(io->ctx, h, 78) != 78)
        return BS_ERR_IO;
    if (memcmp(h + 60, "BOOKMOBI", 8))
        return BS_ERR_FORMAT;
    m->nrec = be16(h + 76);
    if (m->nrec < 2)
        return BS_ERR_FORMAT;
    m->rec = bs_alloc(a, (m->nrec + 1) * sizeof *m->rec);
    if (!m->rec)
        return BS_ERR_NOMEM;
    for (i = 0; i < m->nrec; i++)
    {
        uint8_t e[8];

        if (io->read(io->ctx, e, 8) != 8)
            return BS_ERR_IO;
        m->rec[i] = be32(e);
    }
    m->rec[m->nrec] = (uint32_t)io->size(io->ctx);
    for (i = 0; i < m->nrec; i++)
        if (m->rec[i] > m->rec[i + 1])
            return BS_ERR_FORMAT;

    len = read_rec(m, 0);
    if (len < 16)
        return BS_ERR_FORMAT;
    r0 = m->raw;
    m->comp = be16(r0);
    m->ntext = be16(r0 + 8);
    if (be16(r0 + 12))
        return TS_ERR_LOCKED;
    if (m->comp != 1 && m->comp != 2)
        return BS_ERR_UNSUP;                    /* HUFF/CDIC */
    if (m->ntext < 1 || m->ntext >= m->nrec)
        return BS_ERR_FORMAT;

    if (len >= 132 && !memcmp(r0 + 16, "MOBI", 4))
    {
        uint32_t hl = be32(r0 + 20);

        if (be32(r0 + 36) >= 8)
            return BS_ERR_UNSUP;                /* KF8 alone */
        m->cs = be32(r0 + 28) == 65001 ? TS_CS_UTF8 : TS_CS_CP1252;
        m->first_image = be32(r0 + 108);
        if (hl >= 0xe4 && 16 + 0xe4 <= len)
            m->extra = be16(r0 + 16 + 0xe2);
    }
    *r0len = len;
    return BS_OK;
}

/* ---- records --------------------------------------------------------- */

/* The bytes at the end of a text record that are not text: one entry for
 * each of extra's bits above the first, each sized by a backwards varint,
 * then for bit 0 the bytes of a character the next record starts with */
static uint32_t trailing(const uint8_t *r, uint32_t n, uint16_t extra)
{
    uint32_t cut = 0;
    int bit;

    for (bit = 1; bit < 16; bit++)
    {
        uint32_t v = 0, size = n - cut;
        int shift = 0;

        if (!(extra & (1u << bit)))
            continue;
        while (size > 0)
        {
            uint8_t b = r[--size];

            v |= (uint32_t)(b & 0x7f) << shift;
            shift += 7;
            if ((b & 0x80) || shift >= 28)
                break;
        }
        cut += v;
        if (cut > n)
            return n;
    }
    if ((extra & 1) && cut < n)
        cut += (r[n - cut - 1] & 3) + 1;
    return cut > n ? n : cut;
}

/* PalmDOC: literals, runs of 1-8 copied bytes, a space and a letter in one
 * byte, and back-references within the record. Stops short at a bad one. */
static uint32_t palmdoc(const uint8_t *in, uint32_t n, uint8_t *out,
                        uint32_t cap)
{
    uint32_t i = 0, o = 0;

    while (i < n && o < cap)
    {
        unsigned c = in[i++];

        if (c >= 1 && c <= 8)
        {
            while (c-- && i < n && o < cap)
                out[o++] = in[i++];
        }
        else if (c < 0x80)
            out[o++] = c;
        else if (c >= 0xc0)
        {
            out[o++] = ' ';
            if (o < cap)
                out[o++] = c ^ 0x80;
        }
        else
        {
            unsigned dist, len;

            if (i >= n)
                break;
            c = c << 8 | in[i++];
            dist = (c >> 3) & 0x7ff;
            len = (c & 7) + 3;
            if (!dist || dist > o)
                break;
            while (len-- && o < cap)
            {
                out[o] = out[o - dist];
                o++;
            }
        }
    }
    return o;
}

/* Text record i into m->dec */
static int load_text(mobi_st *m, int i)
{
    long len;
    uint32_t n;

    if (m->cur == i)
        return BS_OK;
    m->cur = 0;
    len = read_rec(m, i);
    if (len < 0)
        return BS_ERR_IO;
    n = (uint32_t)len - trailing(m->raw, (uint32_t)len, m->extra);
    if (m->comp == 2)
        m->dec_len = palmdoc(m->raw, n, m->dec, TEXT_MAX);
    else
    {
        m->dec_len = n < TEXT_MAX ? n : TEXT_MAX;
        memcpy(m->dec, m->raw, m->dec_len);
    }
    m->cur = i;
    return BS_OK;
}

/* ---- loading --------------------------------------------------------- */

static int match(const char *pat, int *k, uint8_t c)
{
    if ((c | 0x20) == (pat[*k] | 0x20) || c == pat[*k])
        (*k)++;
    else
        *k = c == '<';
    if (pat[*k])
        return 0;
    *k = 0;
    return 1;
}

/* The guide's toc page, from a <reference> tag: type="toc", filepos=N */
static void guide_tag(mobi_st *m, const char *tag)
{
    const char *p;
    uint32_t v = 0;

    for (p = tag; *p; p++)
        if (!ts_ascii_ncasecmp(p, "type=", 5))
            break;
    if (!*p || ts_ascii_ncasecmp(p + 5 + (p[5] == '"' || p[5] == '\''),
                                 "toc", 3))
        return;
    for (p = tag; *p; p++)
        if (!ts_ascii_ncasecmp(p, "filepos=", 8))
            break;
    if (!*p)
        return;
    for (p += 8; *p == '"' || *p == '\''; p++)
        ;
    for (; *p >= '0' && *p <= '9'; p++)
        v = v * 10 + (*p - '0');
    m->toc_at = v;
}

/* Every text record once: where each starts, the parts at each
 * <mbp:pagebreak>, and the guide's toc */
static int load_text_map(mobi_st *m, bs_arena *a)
{
    static const char brk[] = "<mbp:pagebreak", ref[] = "<reference";
    char tag[TAG_MAX];
    uint32_t at = 0;
    int i, kb = 0, kr = 0, t = -1;

    m->text_at = bs_alloc(a, (m->ntext + 2) * sizeof *m->text_at);
    m->part_at = bs_alloc(a, (PARTS_MAX + 1) * sizeof *m->part_at);
    if (!m->text_at || !m->part_at)
        return BS_ERR_NOMEM;
    m->part_at[0] = 0;
    m->nparts = 1;
    for (i = 1; i <= m->ntext; i++)
    {
        uint32_t j;
        int rc = load_text(m, i);

        if (rc != BS_OK)
            return rc;
        m->text_at[i] = at;
        for (j = 0; j < m->dec_len; j++)
        {
            uint8_t c = m->dec[j];

            if (t >= 0)
            {
                if (c == '>' || t == TAG_MAX - 1)
                {
                    tag[t] = 0;
                    guide_tag(m, tag);
                    t = -1;
                }
                else
                    tag[t++] = c;
            }
            if (match(brk, &kb, c))
            {
                uint32_t start = at + j + 1 - (sizeof brk - 1);

                if (start > m->part_at[m->nparts - 1]
                    && m->nparts < PARTS_MAX)
                    m->part_at[m->nparts++] = start;
            }
            if (match(ref, &kr, c))
                t = 0;
        }
        at += m->dec_len;
    }
    m->text_at[m->ntext + 1] = at;
    m->part_at[m->nparts] = at;
    return BS_OK;
}

/* The part text offset `at` falls in */
static int part_of(const mobi_st *m, uint32_t at)
{
    int p = 0;

    while (p + 1 < m->nparts && m->part_at[p + 1] <= at)
        p++;
    return p;
}

/* ---- stream ---------------------------------------------------------- */

static int text_pull(bs_stream *st, uint8_t *buf, size_t n, size_t *out)
{
    mobi_st *m = st->st;
    size_t done = 0;

    while (done < n && m->pos < m->end)
    {
        int i = m->cur;
        uint32_t off, k;

        if (!i || m->pos < m->text_at[i] || m->pos >= m->text_at[i + 1])
        {
            for (i = 1; i < m->ntext && m->text_at[i + 1] <= m->pos; i++)
                ;
            if (load_text(m, i) != BS_OK)
                return BS_ERR_IO;
        }
        off = m->pos - m->text_at[i];
        if (off >= m->dec_len)
        {
            m->pos = m->text_at[i + 1];         /* a record that came short */
            continue;
        }
        k = m->dec_len - off;
        if (k > m->end - m->pos)
            k = m->end - m->pos;
        if (k > n - done)
            k = n - done;
        memcpy(buf + done, m->dec + off, k);
        done += k;
        m->pos += k;
    }
    *out = done;
    return BS_OK;
}

/* From `from` to `to` of the text, through fresh charset and HTML stages */
static void read_range(mobi_st *m, uint32_t from, uint32_t to)
{
    m->pos = from;
    m->end = to;
    ts_charset_restart(m->cs_st, &m->text, m->cs);
    ts_tags_restart(m->tags, m->cs_st);
    ts_tags_rewind(m->tags);
}

static int mobi_pull(bs_stream *st, uint8_t *buf, size_t n, size_t *out)
{
    mobi_st *m = st->st;

    return bs_pull(m->tags, buf, n, out);
}

static int mobi_reset(bs_stream *st)
{
    mobi_st *m = st->st;

    if (m->part >= 0)
        read_range(m, m->part_at[m->part], m->part_at[m->part + 1]);
    else
        read_range(m, 0, m->part_at[m->nparts]);
    return BS_OK;
}

int ts_mobi_seek_part(bs_stream *st, int part)
{
    mobi_st *m = st->st;

    if (part < 0 || part >= m->nparts)
        return BS_ERR_INVAL;
    m->part = part;
    return mobi_reset(st);
}

int ts_mobi_parts(const bs_stream *st)
{
    return ((const mobi_st *)st->st)->nparts;
}

long ts_mobi_part_size(const bs_stream *st, int part)
{
    const mobi_st *m = st->st;

    return part >= 0 && part < m->nparts
           ? (long)(m->part_at[part + 1] - m->part_at[part]) : 0;
}

/* A picture's href is its recindex: the image record, counted from 1 */
int ts_mobi_picture(const bs_stream *st, const char *href, bs_off_t *at,
                    uint32_t *csize)
{
    const mobi_st *m = st->st;
    uint32_t n = 0, r;

    for (; *href >= '0' && *href <= '9'; href++)
        n = n * 10 + (*href - '0');
    if (*href || !n || m->first_image == ~0u)
        return BS_ERR_FORMAT;
    r = m->first_image + n - 1;
    if (r >= (uint32_t)m->nrec)
        return BS_ERR_FORMAT;
    *at = m->rec[r];
    *csize = m->rec[r + 1] - m->rec[r];
    return BS_OK;
}

int ts_open_mobi(ts_ctx *c, bs_stream **out)
{
    mobi_st *m = bs_alloc(c->arena, sizeof *m);
    bs_stream *st = bs_alloc(c->arena, sizeof *st);
    long r0len;
    int rc;

    if (!m || !st)
        return BS_ERR_NOMEM;
    rc = open_container(m, c->arena, c->io, &r0len);
    if (rc == BS_OK)
        rc = load_text_map(m, c->arena);
    if (rc != BS_OK)
        return rc;

    m->text.pull = text_pull;
    m->text.reset = NULL;
    m->text.st = m;
    m->cs_st = ts_charset_stream(c->arena, &m->text, m->cs);
    if (!m->cs_st)
        return BS_ERR_NOMEM;
    m->tags = ts_tags_stream(c->arena, m->cs_st, TS_TAGS_HTML, c->cfg);
    if (!m->tags)
        return BS_ERR_NOMEM;
    ts_tags_pictures(m->tags, 1);
    m->part = -1;

    st->pull = mobi_pull;
    st->reset = mobi_reset;
    st->st = m;
    mobi_reset(st);
    c->parts = st;
    c->detected = m->cs;
    *out = st;
    return BS_OK;
}

/* ---- contents -------------------------------------------------------- */

#define TITLE_MAX 128

/* The guide's toc page: each <a filepos=N> on it, by its text, at the part
 * N falls in. Reading it ends the part being read. */
int ts_mobi_toc(bs_stream *st, int (*cb)(void *, const char *, int),
                void *ud)
{
    mobi_st *m = st->st;
    char tag[TAG_MAX], title[TITLE_MAX];
    uint8_t buf[256];
    int t = -1, tl = -1, stop = 0;
    uint32_t target = 0;

    if (m->toc_at == ~0u || m->toc_at >= m->part_at[m->nparts])
        return BS_ERR_UNSUP;
    m->pos = m->toc_at;
    m->end = m->part_at[part_of(m, m->toc_at) + 1];
    ts_charset_restart(m->cs_st, &m->text, m->cs);
    while (!stop)
    {
        size_t got = 0, i;
        int rc = bs_pull(m->cs_st, buf, sizeof buf, &got);

        if (rc != BS_OK)
            return rc;
        if (!got)
            break;
        for (i = 0; i < got && !stop; i++)
        {
            uint8_t c = buf[i];

            if (t >= 0)
            {
                const char *p;

                if (c != '>')
                {
                    if (t < TAG_MAX - 1)
                        tag[t++] = c;
                    continue;
                }
                tag[t] = 0;
                t = -1;
                if (tag[0] == '/' && (tag[1] | 0x20) == 'a' && tl >= 0)
                {
                    while (tl > 0 && title[tl - 1] == ' ')
                        tl--;
                    title[tl] = 0;
                    if (tl && cb(ud, title, part_of(m, target)))
                        stop = 1;
                    tl = -1;
                }
                else if ((tag[0] | 0x20) == 'a' && tag[1] == ' ')
                {
                    for (p = tag; *p; p++)
                        if (!ts_ascii_ncasecmp(p, "filepos=", 8))
                            break;
                    if (*p)
                    {
                        for (p += 8, target = 0; *p == '"' || *p == '\''; p++)
                            ;
                        for (; *p >= '0' && *p <= '9'; p++)
                            target = target * 10 + (*p - '0');
                        tl = 0;
                    }
                }
                continue;
            }
            if (c == '<')
                t = 0;
            else if (tl >= 0)
            {
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
                {
                    if (tl && title[tl - 1] != ' ' && tl < TITLE_MAX - 1)
                        title[tl++] = ' ';
                }
                else if (tl < TITLE_MAX - 5 || (c & 0xc0) == 0x80)
                {
                    if (tl < TITLE_MAX - 1)
                        title[tl++] = c;
                }
            }
        }
    }
    m->pos = m->end;
    return BS_OK;
}

/* ---- details --------------------------------------------------------- */

/* EXTH value `v` of `n` bytes into dst as UTF-8, whole characters only */
static void book_str(char *dst, size_t cap, const uint8_t *v, uint32_t n,
                     ts_charset cs)
{
    size_t o = 0;
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        uint8_t u[4];
        size_t ul;

        if (cs == TS_CS_UTF8)
        {
            size_t len = v[i] < 0x80 ? 1 : (v[i] & 0xe0) == 0xc0 ? 2
                       : (v[i] & 0xf0) == 0xe0 ? 3 : 4;

            if (i + len > n || o + len >= cap)
                break;
            memcpy(dst + o, v + i, len);
            o += len;
            i += len - 1;
            continue;
        }
        ul = ts_utf8_put(u, ts_sb_to_cp(cs, v[i]));
        if (o + ul >= cap)
            break;
        memcpy(dst + o, u, ul);
        o += ul;
    }
    dst[o] = 0;
}

/* A MOBI's title (EXTH 503, else the header's full name), first author
 * (EXTH 100), subjects (EXTH 105), parts, and cover image (EXTH 201, from the first image
 * record) unless it is a GIF. Reads every text record for the parts. */
int ts_mobi_book(const bs_io *io, void *arena, size_t arena_size,
                 ts_book *out)
{
    bs_arena a = { arena, arena_size, 0 };
    mobi_st *m = bs_alloc(&a, sizeof *m);
    uint32_t cover = ~0u;
    const uint8_t *r0;
    long len;
    int rc;

    out->title[0] = out->author[0] = out->subjects[0] = 0;
    out->parts = 0;
    out->cover_at = -1;
    if (!m)
        return BS_ERR_NOMEM;
    rc = open_container(m, &a, io, &len);
    if (rc != BS_OK)
        return rc;
    r0 = m->raw;
    if (len >= 132 && !memcmp(r0 + 16, "MOBI", 4))
    {
        uint32_t hl = be32(r0 + 20), name_at = be32(r0 + 84),
                 name_len = be32(r0 + 88);

        if (name_at < (uint32_t)len && name_len <= (uint32_t)len - name_at)
            book_str(out->title, TS_BOOK_TITLE, r0 + name_at, name_len,
                     m->cs);
        if ((be32(r0 + 128) & 0x40) && 16 + hl + 12 <= (uint32_t)len
            && !memcmp(r0 + 16 + hl, "EXTH", 4))
        {
            uint32_t p = 16 + hl + 12, cnt = be32(r0 + 16 + hl + 8);

            while (cnt-- && p + 8 <= (uint32_t)len)
            {
                uint32_t type = be32(r0 + p), l = be32(r0 + p + 4);

                if (l < 8 || l > (uint32_t)len - p)
                    break;
                if (type == 503)
                    book_str(out->title, TS_BOOK_TITLE, r0 + p + 8, l - 8,
                             m->cs);
                else if (type == 100 && !out->author[0])
                    book_str(out->author, TS_BOOK_AUTHOR, r0 + p + 8, l - 8,
                             m->cs);
                else if (type == 105)
                {
                    char s[TS_BOOK_SUBJECTS];

                    book_str(s, sizeof s, r0 + p + 8, l - 8, m->cs);
                    ts_book_subject(out, s);
                }
                else if (type == 201 && l == 12)
                    cover = be32(r0 + p + 8);
                p += l;
            }
        }
    }
    if (cover != ~0u && m->first_image != ~0u
        && m->first_image + cover < (uint32_t)m->nrec)
    {
        int r = m->first_image + cover;

        if (read_rec(m, r) >= 4 && memcmp(m->raw, "GIF8", 4))
        {
            out->cover_at = m->rec[r];
            out->cover_csize = out->cover_usize = m->rec[r + 1] - m->rec[r];
            out->cover_method = MEMBER_STORED;
        }
    }
    rc = load_text_map(m, &a);
    if (rc != BS_OK)
        return rc;
    out->parts = m->nparts;
    return BS_OK;
}
