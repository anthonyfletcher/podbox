/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Back end for lyrics files: .lrc, .lrc8 and .snc read as plain words, with
 * the timing stripped out.
 *
 * The synchronised viewer (viewers/lyric_viewer/) wants the timestamps; the
 * text viewer wants the song. So this drops every time tag, drops the
 * id-tag lines that carry no lyrics, and leaves everything else exactly as
 * the author wrote it -- including brackets that are not tags, because
 * "[chorus]" is words.
 *
 * It sits after the charset stage, so a UTF-16 lyrics file reads correctly
 * here even though the synchronised viewer refuses one. Time tags are ASCII,
 * which survives that ordering; the .snc marker is not, and is matched in its
 * decoded form -- see LY_SNC_OPEN.
 ****************************************************************************/

#include "ts_internal.h"

#define LIN     256     /* input taken from the source stream at a time */
#define TAGMAX  64      /* longest tag held while deciding what it is */

/* The .snc time marker is \xa2\xe2hhmmssxx\xa2\xd0 in the file. Once the
 * charset stage has run those bytes are U+00A2 U+00E2 and U+00A2 U+00D0, i.e.
 * the UTF-8 below -- which holds for the Latin-1/CP1252 family an .snc
 * actually is. Under a codepage that maps 0xA2 elsewhere the marker simply
 * will not match and its line reads as the two odd characters it decoded to,
 * which is the same thing any other reader would show. */
static const uint8_t LY_SNC_OPEN[4]  = { 0xc2, 0xa2, 0xc3, 0xa2 };
static const uint8_t LY_SNC_CLOSE[4] = { 0xc2, 0xa2, 0xc3, 0x90 };

/* Tags that identify the file rather than say anything. Their whole line
 * goes, newline included, so the lyrics do not open on a run of blanks. */

enum {
    LY_BOL = 0,     /* start of a line: [..] tags and the .snc marker strip */
    LY_BODY,        /* in the words */
    LY_SKIP,        /* an id-tag line; drop the rest of it */
    LY_LTAG,        /* holding a [..] while deciding */
    LY_WTAG,        /* holding a <..> while deciding */
    LY_SNCTRY,      /* holding what may be the .snc open marker */
    LY_SNC,         /* inside a .snc tag, waiting for the close marker */
    LY_SNCEOL       /* just past one; its own newline is not a blank line */
};

typedef struct {
    ts_stream *src;
    uint8_t in[LIN];
    size_t   ipos, ilen;
    int      src_eof;
    ts_pend  out;
    int      state;
    char     tag[TAGMAX];
    size_t   taglen;
} ly_st;

/* ---- tag recognition ------------------------------------------------- */

/* mm:ss, mm:ss.xx or mm:ss.xxx, with the fraction also allowed after a second
 * colon. Minutes may be one or two digits and are not capped at 60 -- a long
 * track's tags run past [60:00]. */
static int ly_is_time(const char *t, size_t n)
{
    size_t i = 0, d = 0;

    while (i < n && t[i] >= '0' && t[i] <= '9') { i++; d++; }
    if (d < 1 || d > 2 || i >= n || t[i] != ':')
        return 0;
    i++;

    for (d = 0; i < n && t[i] >= '0' && t[i] <= '9'; i++)
        d++;
    if (d != 2)
        return 0;
    if (i == n)
        return 1;                       /* mm:ss */
    if (t[i] != '.' && t[i] != ':')
        return 0;
    i++;

    for (d = 0; i < n && t[i] >= '0' && t[i] <= '9'; i++)
        d++;
    return (d >= 2 && d <= 3 && i == n);
}

/* A property rather than a lyric: a bracketed group at the head of a line
 * whose key is letters followed by a colon. Its whole line goes.
 *
 * A rule rather than a list of known keys, because taggers invent their own --
 * [ti:] [ar:] [al:] [by:] [re:] [ve:] [offset:] [length:] are the documented
 * ones, but [id:], [au:], [tool:] and others turn up in real files and a list
 * only ever finds the tags someone thought of.
 *
 * Letters only, so "[Verse 1: Adele]" is words and stays; and a time tag is
 * tested before this, so "[00:12.00]" never reaches it. */
static int ly_is_id(const char *t, size_t n)
{
    size_t i = 0;

    while (i < n && ((t[i] >= 'a' && t[i] <= 'z')
                     || (t[i] >= 'A' && t[i] <= 'Z')))
        i++;
    return (i > 0 && i < n && t[i] == ':');
}

/* ---- the filter ------------------------------------------------------ */

static void ly_feed(ly_st *s, uint8_t c);

/* Put back a tag that turned out not to be one, then re-read `c` in the
 * state that follows it. The recursion is one level deep: the state has
 * already changed, so the second call cannot come back here. */
static void ly_literal(ly_st *s, const char *open, uint8_t c)
{
    if (open)
        ts_emit(&s->out, (const uint8_t *)open, 1);
    ts_emit(&s->out, (const uint8_t *)s->tag, s->taglen);
    s->taglen = 0;
    s->state = LY_BODY;
    ly_feed(s, c);
}

static void ly_feed(ly_st *s, uint8_t c)
{
    if (c == '\r')
        return;

    switch (s->state) {
    case LY_BOL:
        if (c == '[') {
            s->state = LY_LTAG;
            s->taglen = 0;
            return;
        }
        if (c == LY_SNC_OPEN[0]) {
            s->state = LY_SNCTRY;
            s->taglen = 0;
            s->tag[s->taglen++] = (char)c;
            return;
        }
        if (c == '\n') {                /* a blank line is the author's */
            ts_emit(&s->out, &c, 1);
            return;
        }
        s->state = LY_BODY;
        /* fall through */

    case LY_BODY:
        if (c == '<') {
            s->state = LY_WTAG;
            s->taglen = 0;
            return;
        }
        if (c == '\n')
            s->state = LY_BOL;
        ts_emit(&s->out, &c, 1);
        return;

    case LY_LTAG:
        if (c == ']') {
            int time = ly_is_time(s->tag, s->taglen);
            int id = !time && ly_is_id(s->tag, s->taglen);

            if (!time && !id) {
                ts_emit(&s->out, (const uint8_t *)"[", 1);
                ts_emit(&s->out, (const uint8_t *)s->tag, s->taglen);
                ts_emit(&s->out, (const uint8_t *)"]", 1);
            }
            /* A consumed tag must leave the buffer empty: what is in it is
             * what the end of the file flushes back out. */
            s->taglen = 0;
            /* after a time tag, LY_BOL not LY_BODY -- [t1][t2]words repeats */
            s->state = time? LY_BOL: id? LY_SKIP: LY_BODY;
            return;
        }
        if (c == '\n' || s->taglen >= TAGMAX) {
            ly_literal(s, "[", c);      /* unclosed, or far too long */
            return;
        }
        s->tag[s->taglen++] = (char)c;
        return;

    case LY_WTAG:
        if (c == '>') {
            if (!ly_is_time(s->tag, s->taglen)) {
                ts_emit(&s->out, (const uint8_t *)"<", 1);
                ts_emit(&s->out, (const uint8_t *)s->tag, s->taglen);
                ts_emit(&s->out, (const uint8_t *)">", 1);
            }
            s->taglen = 0;
            s->state = LY_BODY;
            return;
        }
        if (c == '\n' || s->taglen >= TAGMAX) {
            ly_literal(s, "<", c);
            return;
        }
        s->tag[s->taglen++] = (char)c;
        return;

    case LY_SKIP:
        if (c == '\n')
            s->state = LY_BOL;          /* the newline goes with the line */
        return;

    case LY_SNCTRY:
        if (c == LY_SNC_OPEN[s->taglen]) {
            s->tag[s->taglen++] = (char)c;
            if (s->taglen == sizeof LY_SNC_OPEN) {
                s->state = LY_SNC;
                s->taglen = 0;
            }
            return;
        }
        ly_literal(s, NULL, c);         /* just a character that looked close */
        return;

    case LY_SNC:
        if (c == LY_SNC_CLOSE[s->taglen]) {
            if (++s->taglen == sizeof LY_SNC_CLOSE) {
                s->state = LY_SNCEOL;
                s->taglen = 0;
            }
            return;
        }
        s->taglen = 0;
        if (c == '\n')
            s->state = LY_BOL;          /* unterminated: drop the line */
        return;

    case LY_SNCEOL:
        /* A .snc time marker is structure, not a lyric: when it is the whole
         * line, its newline goes with it rather than leaving a blank. Words
         * after it on the same line are the start of that lyric. */
        if (c == '\n') {
            s->state = LY_BOL;
            return;
        }
        s->state = LY_BOL;
        ly_feed(s, c);
        return;

    default:
        return;
    }
}

/* ---- stream plumbing ------------------------------------------------- */

static int ly_fill(ly_st *s)
{
    size_t got = 0;
    int rc;

    if (s->ipos < s->ilen || s->src_eof)
        return TS_OK;
    s->ipos = s->ilen = 0;
    rc = ts_pull(s->src, s->in, LIN, &got);
    if (rc != TS_OK)
        return rc;
    if (!got)
        s->src_eof = 1;
    s->ilen = got;
    return TS_OK;
}

static int ly_pull(ts_stream *st, uint8_t *buf, size_t n, size_t *out)
{
    ly_st *s = st->st;
    size_t done = 0;

    while (done < n) {
        int rc;
        done += ts_pend_take(&s->out, buf + done, n - done);
        if (done == n)
            break;
        /* One feed can emit a whole held-back tag, so it needs real room --
         * which it has, because the take above drains the ring whenever the
         * caller's buffer still has space. */
        if (ts_pend_free(&s->out) < TAGMAX + 2)
            continue;

        rc = ly_fill(s);
        if (rc != TS_OK)
            return rc;
        if (s->ipos >= s->ilen) {
            /* End of input while still holding something back: it was never a
             * tag, so put it back rather than losing the last line. Gated on
             * the state, not just on taglen, so a tag that was consumed
             * cleanly cannot reappear here. */
            if (s->state == LY_LTAG || s->state == LY_WTAG
                || s->state == LY_SNCTRY) {
                const char *open = (s->state == LY_LTAG)? "[":
                                   (s->state == LY_WTAG)? "<": NULL;
                if (open)
                    ts_emit(&s->out, (const uint8_t *)open, 1);
                ts_emit(&s->out, (const uint8_t *)s->tag, s->taglen);
                s->taglen = 0;
                s->state = LY_BODY;
                continue;
            }
            done += ts_pend_take(&s->out, buf + done, n - done);
            break;
        }
        ly_feed(s, s->in[s->ipos++]);
    }
    *out = done;
    return TS_OK;
}

static int ly_reset(ts_stream *st)
{
    ly_st *s = st->st;
    ts_stream *src = s->src;
    int rc = src->reset? src->reset(src): TS_ERR_UNSUP;

    if (rc != TS_OK)
        return rc;
    memset(s, 0, sizeof *s);
    s->src = src;
    return TS_OK;
}

int ts_open_lyrics(ts_ctx *c, ts_stream **out)
{
    ts_stream *file = ts_file_stream(c->arena, c->io, 0, -1);
    ts_stream *st = ts_alloc(c->arena, sizeof *st);
    ly_st *s = ts_alloc(c->arena, sizeof *s);

    if (!file || !st || !s)
        return TS_ERR_NOMEM;

    s->src = ts_charset_stream(c->arena, file, c->detected);
    if (!s->src)
        return TS_ERR_NOMEM;

    st->pull = ly_pull;
    st->reset = ly_reset;
    st->st = s;
    *out = st;
    return TS_OK;
}
