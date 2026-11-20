/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Back ends for the formats that need no container: plain text, markdown
 * and RTF.
 ****************************************************************************/

#include "ts_internal.h"

#define TIN 256

/* The most one input byte can make the filter emit: a held `[name]` with
 * its two marks, and the word held after its `:`, replayed. flt_pull() feeds
 * a byte only with this much room. */
#define FLT_ROOM   448
#define MD_NAME_MAX 192
#define MD_DEF_MAX  16
#define MD_FM_KEY   32              /* the longest key front matter has */

/* ===================== shared filter scaffolding ====================== */

typedef struct flt_st flt_st;
struct flt_st {
    bs_stream *src;
    uint8_t in[TIN];
    size_t ipos, ilen;
    int    src_eof;
    ts_pend out;
    void (*feed)(flt_st *, uint8_t);
    void (*eos)(flt_st *);          /* optional flush at end of input */
    int  started, eos_done;
    /* markdown */
    int at_bol, fence, info, tick, link, esc;
    int code, hashes, heading, quote, dim, bold, stars, unders, bang, image;
    uint8_t prev;                   /* the last byte emitted, 0 at the start */
    size_t nlen;
    uint8_t name[MD_NAME_MAX];      /* a [label] until what follows it shows
                                     * whether it is a link or a name */
    size_t dlen;
    uint8_t def[MD_DEF_MAX];        /* what follows `[label]:` until its first
                                     * word shows whether it is a link */
    int skip;                       /* MD_SKIP_*: inside what is left out */
    int opened;                     /* bytes of "<!--" held */
    int dashes;                     /* '-'s just seen, in a comment */
    int eat_nl;                     /* a comment that was its line's all */
    int front, front_done;          /* '-'s opening the file, and whether
                                     * the file has gone past them */
    int fm_n;                       /* a front matter line's bytes so far */
    uint8_t fm_line[3];
    int fm_kind;                    /* MD_FM_*: what the line is so far */
    int fm_keyn;
    uint8_t fm_key[MD_FM_KEY];      /* its key, until its colon shows it is
                                     * YAML; let go when it is not */
    int inner;                      /* brackets open inside a label */
    int lead;                       /* spaces before a line's first word */
    int itemn, item_skip;           /* bytes held of what may start a list
                                     * item, and whether to let one by */
    uint8_t item[4];                /* `-`, or `12.` */
    /* rtf */
    int depth, skip_depth, ctrl, ctrl_len, uc_skip, hexd, hexv;
    int uc_pend;                    /* fallback chars a \uN has still to eat */
    char ctrl_word[32];
    ts_charset cp;
};

static int flt_fill(flt_st *s)
{
    size_t got = 0;
    int rc;

    if (s->ipos < s->ilen || s->src_eof) return BS_OK;
    s->ipos = s->ilen = 0;
    rc = bs_pull(s->src, s->in, TIN, &got);
    if (rc != BS_OK) return rc;
    if (!got) s->src_eof = 1;
    s->ilen = got;
    return BS_OK;
}

static int flt_pull(bs_stream *st, uint8_t *buf, size_t n, size_t *out)
{
    flt_st *s = st->st;
    size_t done = 0;

    while (done < n) {
        int rc;
        done += ts_pend_take(&s->out, buf + done, n - done);
        if (done == n) break;
        if (ts_pend_free(&s->out) < FLT_ROOM) continue;

        rc = flt_fill(s);
        if (rc != BS_OK) return rc;
        if (s->ipos >= s->ilen) {
            if (s->eos && !s->eos_done) { s->eos_done = 1; s->eos(s); continue; }
            done += ts_pend_take(&s->out, buf + done, n - done);
            break;
        }
        s->feed(s, s->in[s->ipos++]);
    }
    *out = done;
    return BS_OK;
}

static int flt_reset(bs_stream *st)
{
    flt_st *s = st->st;
    bs_stream *src = s->src;
    void (*feed)(flt_st *, uint8_t) = s->feed;
    void (*eos)(flt_st *) = s->eos;
    ts_charset cp = s->cp;
    int rc = src->reset ? src->reset(src) : BS_ERR_UNSUP;

    if (rc != BS_OK) return rc;
    memset(s, 0, sizeof *s);
    s->src = src; s->feed = feed; s->eos = eos; s->cp = cp;
    s->at_bol = 1; s->uc_skip = 1;
    return BS_OK;
}

static bs_stream *flt_new(bs_arena *a, bs_stream *src,
                          void (*feed)(flt_st *, uint8_t), flt_st **sp)
{
    bs_stream *st = bs_alloc(a, sizeof *st);
    flt_st *s = bs_alloc(a, sizeof *s);

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

/* What the screens can show of Markdown, as the marks in txt_source.h:
 * headings, dim emphasis, bold, pull lines and bracketed names. The rest is
 * dropped to its words, and code passes through as it is. Lines are kept as
 * written: a paragraph is a line. */
enum { LK_NONE = 0, LK_IN_LABEL, LK_AFTER_BRACKET, LK_IN_TARGET, LK_SKIP_LINE,
       LK_AFTER_COLON };

static void md_emit(flt_st *s, uint8_t c)
{
    ts_emit(&s->out, &c, 1);
    s->at_bol = (c == '\n');
    s->prev = c;
}

static int md_word(uint8_t c)
{
    return c >= 0x80 || (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' &&
                                                   (c | 0x20) <= 'z');
}

static int md_space(uint8_t c)
{
    return c == 0 || c == ' ' || c == '\t' || c == '\n';
}

/* A run of `n` emphasis characters `ch`, now that `next` follows it. An odd
 * run opens or closes dim, and one of two or more bold. A run that can do neither is literal: `snake_case`, `2 * 3`. An
 * underscore opens only after a space or an opening bracket, so one inside a
 * word or an HTML attribute (`target='_blank'`) stays: the stream cannot look
 * ahead for the closer that CommonMark would want. */
static void md_run(flt_st *s, uint8_t ch, int n, uint8_t next)
{
    int dim = n & 1, bold = n >= 2, can_open, can_close, close;

    if (ch == '_')
    {
        can_open = (md_space(s->prev) || s->prev == '(' || s->prev == '[')
                   && !md_space(next);
        can_close = !md_space(s->prev) && !md_word(next);
    }
    else
    {
        can_open = !md_space(next);
        can_close = !md_space(s->prev);
    }
    close = can_close && ((dim && s->dim) || (bold && s->bold));

    if (!close && !can_open)
    {
        while (n--)
            md_emit(s, ch);
        return;
    }
    if (dim)
    {
        ts_emit_cp(&s->out, close ? TS_DIM_CLOSE : TS_DIM_OPEN);
        s->dim = !close;
    }
    if (bold)
    {
        ts_emit_cp(&s->out, close ? TS_BOLD_CLOSE : TS_BOLD_OPEN);
        s->bold = !close;
    }
}

/* A name is a label with a letter in it, not a task box's `[x]` or a
 * footnote's `[^1]` */
static int md_is_name(const flt_st *s)
{
    size_t i;

    if (s->image || s->nlen < 2 || s->name[0] == '^')
        return 0;
    for (i = 0; i < s->nlen; i++)
        if (md_word(s->name[i]) && !(s->name[i] >= '0' && s->name[i] <= '9'))
            return 1;
    return 0;
}

/* The held label: as a name, as plain words, or as it was written -- with
 * its `[`, and its `]` when it has one */
enum { MD_WORDS = 0, MD_OPENED, MD_BRACKETED };

static void md_label(flt_st *s, int as_name, int brackets)
{
    size_t i, shown = s->nlen;

    /* A name's trailing "(...)" with words before it is its qualifier */
    if (as_name && s->nlen > 2 && s->name[s->nlen - 1] == ')')
    {
        for (i = s->nlen - 1; i > 0 && s->name[i] != '('; i--)
            ;
        while (i > 0 && s->name[i - 1] == ' ')
            i--;
        if (i > 0 && s->name[i] == ' ')
            shown = i;
    }
    if (as_name)
        ts_emit_cp(&s->out, TS_NAME_OPEN);
    if (brackets != MD_WORDS)
        md_emit(s, '[');
    for (i = 0; i < s->nlen; i++)
    {
        if (i == shown)
            ts_emit_cp(&s->out, TS_HIDE_OPEN);
        if (i >= shown && s->name[i] == ' ')
        {
            md_emit(s, 0xc2);           /* U+00A0 */
            md_emit(s, 0xa0);
        }
        else if (brackets != MD_WORDS
                 || (s->name[i] != '*' && s->name[i] != '_'
                     && s->name[i] != '`'))
            md_emit(s, s->name[i]);
    }
    if (shown < s->nlen)
        ts_emit_cp(&s->out, TS_HIDE_CLOSE);
    if (brackets == MD_BRACKETED)
        md_emit(s, ']');
    if (as_name)
        ts_emit_cp(&s->out, TS_NAME_CLOSE);
    s->nlen = 0;
    s->image = 0;
    s->link = LK_NONE;
}

/* The held label, with nothing after it that makes it a link: a name, or
 * as it was written */
static void md_label_alone(flt_st *s)
{
    if (md_is_name(s))
        md_label(s, 1, MD_WORDS);
    else
        md_label(s, 0, s->image ? MD_WORDS : MD_BRACKETED);
}

/* Whether the word after `[label]:` is a link's address, which makes the
 * line a reference definition */
static int md_def_is_link(const flt_st *s)
{
    const uint8_t *d = s->def;
    size_t i = 0, j, n = s->dlen;

    while (i < n && (d[i] == ' ' || d[i] == '\t'))
        i++;
    if (i < n && (d[i] == '<' || d[i] == '/' || d[i] == '#'))
        return 1;
    if (n - i >= 4 && !strncasecmp((const char *)d + i, "www.", 4))
        return 1;
    for (j = i; j + 2 < n; j++)
        if (d[j] == ':' && d[j + 1] == '/' && d[j + 2] == '/')
            return 1;
    return 0;
}

static void md_feed(flt_st *s, uint8_t c);

/* What may start a list item, held from a line's start, let go as text:
 * the first byte is let by, or it would be held again */
static void md_item_let_go(flt_st *s)
{
    int i, n = s->itemn;

    s->itemn = 0;
    s->item_skip = 1;
    for (i = 0; i < n; i++)
        md_feed(s, s->item[i]);
    s->item_skip = 0;
}

/* The list item held: its marks, once a level, and a number's text */
static void md_item_make(flt_st *s, int num)
{
    int level = s->lead / 2 < 3 ? s->lead / 2 : 3, i;

    for (i = 0; i <= level; i++)
        ts_emit_cp(&s->out, num ? TS_NUMBERED : TS_BULLET);
    if (num)
    {
        for (i = 0; i < s->itemn; i++)
            md_emit(s, s->item[i]);
        md_emit(s, ' ');
    }
    s->itemn = 0;
    s->at_bol = 0;
}

/* What md_item_more() made of a byte */
enum { MD_ITEM_NOT = 0, MD_ITEM_TAKEN, MD_ITEM_MADE };

/* `c` after what md_feed() holds as it may start a list item: TAKEN, held
 * with it or making the item; MADE, the item made and `c` its text's first
 * byte; NOT, what is held is not an item after all. A bullet waits for the
 * byte after its space, as `- - -` and `* * *` are rules. */
static int md_item_more(flt_st *s, uint8_t c)
{
    uint8_t last = s->item[s->itemn - 1];
    int num = s->item[0] >= '0' && s->item[0] <= '9';

    if (num && last >= '0' && last <= '9' && s->itemn < 4
        && ((c >= '0' && c <= '9' && s->itemn < 3) || c == '.' || c == ')'))
    {
        s->item[s->itemn++] = c;
        return MD_ITEM_TAKEN;
    }
    if (!num && s->itemn == 2)
    {
        if (c == s->item[0])
            return MD_ITEM_NOT;
        s->itemn = 1;
        md_item_make(s, 0);
        return c == ' ' ? MD_ITEM_TAKEN : MD_ITEM_MADE;
    }
    if (c != ' ' || (num && last != '.' && last != ')'))
        return MD_ITEM_NOT;
    if (!num)
    {
        s->item[s->itemn++] = ' ';
        return MD_ITEM_TAKEN;
    }
    md_item_make(s, 1);
    return MD_ITEM_TAKEN;
}

/* What md_take() is leaving out */
enum { MD_SKIP_NONE = 0, MD_SKIP_COMMENT, MD_SKIP_FRONT };

static const uint8_t md_open[] = "<!--";

/* The bytes md_take() held, let go: they were not what it held them for */
static void md_let_go(flt_st *s)
{
    int i, n = s->opened;

    s->opened = 0;
    for (i = 0; i < n; i++)
        md_feed(s, md_open[i]);
}

/* A front matter line: at its start, inside its key, or YAML -- blank,
 * indented, a list's or a comment's, or a key and its colon */
enum { MD_FM_START = 0, MD_FM_IN_KEY, MD_FM_YAML };

static int md_key_byte(uint8_t c)
{
    return md_word(c) || c == '_' || c == '-' || c == ' ' || c == '.'
           || c == '"' || c == '\'';
}

/* Front matter that turns out not to be, at a line that is not YAML: the
 * line so far let go, and what follows read as the page */
static void md_take(flt_st *s, uint8_t c);

static void md_front_ends(flt_st *s, uint8_t c)
{
    int i, n = s->fm_keyn;

    s->skip = MD_SKIP_NONE;
    s->fm_keyn = 0;
    for (i = 0; i < n; i++)
        md_take(s, s->fm_key[i]);
    md_take(s, c);
}

/* `c` inside front matter */
static void md_front(flt_st *s, uint8_t c)
{
    if (c == '\n')
    {
        if (s->fm_n == 3 && s->fm_line[0] == s->fm_line[1]
            && s->fm_line[1] == s->fm_line[2]
            && (s->fm_line[0] == '-' || s->fm_line[0] == '.'))
            s->skip = MD_SKIP_NONE;     /* its closing line */
        else if (s->fm_kind == MD_FM_IN_KEY)
        {
            md_front_ends(s, c);
            return;
        }
        s->fm_n = 0;
        s->fm_kind = MD_FM_START;
        s->fm_keyn = 0;
        return;
    }
    if (c != ' ' && s->fm_n < 4)
    {
        if (s->fm_n < 3)
            s->fm_line[s->fm_n] = c;
        s->fm_n++;
    }
    if (s->fm_kind == MD_FM_START)
    {
        if (c == ' ' || c == '\t' || c == '-' || c == '#' || c == '.')
            s->fm_kind = MD_FM_YAML;
        else if (md_key_byte(c))
        {
            s->fm_kind = MD_FM_IN_KEY;
            s->fm_key[s->fm_keyn++] = c;
        }
        else
            md_front_ends(s, c);
    }
    else if (s->fm_kind == MD_FM_IN_KEY)
    {
        if (c == ':')
            s->fm_kind = MD_FM_YAML;
        else if (md_key_byte(c) && s->fm_keyn < MD_FM_KEY)
            s->fm_key[s->fm_keyn++] = c;
        else
            md_front_ends(s, c);
    }
}

/* Whether the byte arriving now is code: a backtick run still held decides
 * whether it opens or closes code before that byte is read */
static int md_in_code(const flt_st *s)
{
    if (!s->tick)
        return s->fence || s->code;
    if (s->tick >= 3 && s->at_bol)
        return !s->fence;
    return s->fence || !s->code;
}

/* Markdown less its comments, <!-- to -->, and its front matter: a ---
 * line opening the file, to the next --- or ... line, for as long as its
 * lines are YAML. A comment that is all of its line takes the line with
 * it. Inside code they are code. */
static void md_take(flt_st *s, uint8_t c)
{
    if (c == '\r')
        return;
    if (!s->front_done)
    {
        if (c == '-' && s->front < 3)
        {
            s->front++;
            return;
        }
        s->front_done = 1;
        if (c == '\n' && s->front == 3)
        {
            s->skip = MD_SKIP_FRONT;
            return;
        }
        while (s->front)
        {
            s->front--;
            md_take(s, '-');
        }
    }
    if (s->skip == MD_SKIP_FRONT)
    {
        md_front(s, c);
        return;
    }
    if (s->skip == MD_SKIP_COMMENT)
    {
        if (c == '>' && s->dashes >= 2)
            s->skip = MD_SKIP_NONE;
        s->dashes = c == '-' ? s->dashes + 1 : 0;
        return;
    }
    if (s->eat_nl)
    {
        s->eat_nl = 0;
        if (c == '\n')
            return;
    }
    if (s->opened && c != md_open[s->opened])
        md_let_go(s);
    if (!md_in_code(s) && s->link != LK_IN_LABEL && c == md_open[s->opened])
    {
        if (++s->opened < 4)
            return;
        s->opened = 0;
        s->skip = MD_SKIP_COMMENT;
        s->dashes = 0;
        s->eat_nl = s->at_bol && !s->hashes && !s->quote;
        return;
    }
    md_feed(s, c);
}

/* The end of the input: what is held is let go, and the marks closed, as a
 * line's end does */
static void md_eos(flt_st *s)
{
    if (!s->front_done)
    {
        s->front_done = 1;
        while (s->front--)
            md_feed(s, '-');
    }
    if (s->skip == MD_SKIP_FRONT && s->fm_kind == MD_FM_IN_KEY)
        md_front_ends(s, '\n');
    md_let_go(s);
    if (s->itemn)
        md_item_let_go(s);
    if (!s->at_bol || s->link || s->stars || s->unders || s->esc || s->bang
        || s->tick || s->hashes)
        md_feed(s, '\n');
}

static void md_feed(flt_st *s, uint8_t c)
{
    if (c == '\r') return;

    if (s->itemn) {
        int got = md_item_more(s, c);

        if (got == MD_ITEM_TAKEN) return;
        if (got == MD_ITEM_NOT) md_item_let_go(s);
    }

    /* The rest of a fence line is its info string, and is dropped. */
    if (s->info) {
        if (c != '\n') return;
        s->info = 0;
    }

    /* A label is held whole, backticks and all, until its `]`: brackets
     * inside it, `[Song [Live]]`, are its own */
    if (s->link == LK_IN_LABEL) {
        if (c == ']' && !s->inner) { s->link = LK_AFTER_BRACKET; return; }
        if (c == '[' || c == ']') s->inner += c == '[' ? 1 : -1;
        if (c != '\n' && s->nlen < MD_NAME_MAX) {
            s->name[s->nlen++] = c;
            return;
        }
        md_label(s, 0, MD_OPENED);              /* too long for a name */
    }

    /* `[label]:` at a line's start is a reference definition only when an
     * address follows; `[Name]: vocals` is a name and its words. The first
     * word after the colon is held until it shows which. */
    if (s->link == LK_AFTER_COLON) {
        size_t i, n;
        uint8_t last = s->dlen ? s->def[s->dlen - 1] : ' ';

        if (c != '\n' && s->dlen < MD_DEF_MAX
            && !((c == ' ' || c == '\t') && last != ' ' && last != '\t')) {
            s->def[s->dlen++] = c;
            return;
        }
        if (md_def_is_link(s)) {
            s->nlen = 0;
            s->image = 0;
            s->dlen = 0;
            s->link = LK_SKIP_LINE;
        } else {
            md_label_alone(s);
            n = s->dlen;
            s->dlen = 0;
            md_feed(s, ':');
            for (i = 0; i < n; i++)
                md_feed(s, s->def[i]);
        }
    }

    if (s->link == LK_AFTER_BRACKET) {
        if (c == '(' || c == '[') {             /* [label](url), [label][ref] */
            md_label(s, 0, MD_WORDS);
            s->link = LK_IN_TARGET;
            return;
        }
        if (c == ':' && (s->prev == '\n' || !s->prev) && !s->heading
            && !s->quote) {
            s->dlen = 0;
            s->link = LK_AFTER_COLON;
            return;
        }
        md_label_alone(s);
    }

    /* An emphasis run ends at the first byte that is not its own: code's
     * backtick included, so `**`code`**` is bold */
    if (s->stars && c != '*') {
        int n = s->stars;
        s->stars = 0;
        md_run(s, '*', n, c);
    }
    if (s->unders && c != '_') {
        int n = s->unders;
        s->unders = 0;
        md_run(s, '_', n, c);
    }

    /* Backtick runs: three or more at the start of a line open or close a
     * fenced block; a shorter run opens or closes inline code, and vanishes. */
    if (c == '`') { s->tick++; return; }
    if (s->tick) {
        int run = s->tick;
        s->tick = 0;
        if (run >= 3 && s->at_bol) {
            s->fence = !s->fence;
            if (c != '\n') { s->info = 1; return; }
        } else if (!s->fence) {
            s->code = !s->code;
        }
    }

    if (s->fence) { md_emit(s, c); return; }
    if (s->code) {
        if (c != '\n') { md_emit(s, c); return; }
        s->code = 0;
    }

    if (s->link == LK_SKIP_LINE) {              /* a reference definition */
        if (c != '\n') return;
        s->link = LK_NONE;
    }

    /* `!` is an image's only before `[` */
    if (s->bang) {
        s->bang = 0;
        if (c == '[') {
            s->image = 1; s->inner = 0; s->link = LK_IN_LABEL;
            return;
        }
        md_emit(s, '!');
    }

    if (s->link == LK_IN_TARGET) {              /* swallow (url) or [ref] */
        if (c == ')' || c == ']' || c == '\n') s->link = LK_NONE;
        if (c != '\n') return;
    }

    /* An escaped line end is a line end all the same */
    if (s->esc) {
        s->esc = 0;
        if (c != '\n') { md_emit(s, c); return; }
    }

    if (c == '\n') {
        if (s->dim) { ts_emit_cp(&s->out, TS_DIM_CLOSE); s->dim = 0; }
        if (s->bold) { ts_emit_cp(&s->out, TS_BOLD_CLOSE); s->bold = 0; }
        if (s->heading) { ts_emit_cp(&s->out, TS_HEADING_CLOSE); s->heading = 0; }
        s->hashes = 0;
        s->quote = 0;
        s->lead = 0;
        md_emit(s, c);
        return;
    }

    if (s->at_bol) {
        if (c == '#' && s->hashes < 7) { s->hashes++; return; }
        if (s->hashes) {
            int level = s->hashes;
            s->hashes = 0;
            if (c == ' ' && level <= 6) {
                ts_emit_cp(&s->out, TS_HEADING_OPEN);
                if (level > 1)
                    ts_emit_cp(&s->out, TS_HEADING_MINOR);
                s->heading = 1;
                s->at_bol = 0;
                return;
            }
            while (level--)                     /* `#1 hit` is text */
                md_emit(s, '#');
        }
        if (c == '>') {
            if (!s->quote) { ts_emit_cp(&s->out, TS_QUOTE); s->quote = 1; }
            return;
        }
        /* `- `, `* `, `+ ` and `1. ` start a list item */
        if (!s->item_skip && !s->heading
            && (c == '-' || c == '*' || c == '+' || (c >= '0' && c <= '9'))) {
            s->item[0] = c;
            s->itemn = 1;
            return;
        }
        s->item_skip = 0;
        if (c == ' ' || c == '\t') { s->lead += c == '\t' ? 4 : 1; return; }
        if (c == '=' || c == '-') { /* setext rules pass through as-is */ }
        s->at_bol = 0;
    }

    switch (c) {
    case '\\': s->esc = 1; return;
    case '*':  s->stars++; return;
    case '_':  s->unders++; return;
    case '[':
        s->nlen = 0; s->image = 0; s->inner = 0; s->link = LK_IN_LABEL;
        return;
    case '!':  s->bang = 1; return;
    default:   break;
    }
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
    if (s->uc_pend > 0) { s->uc_pend--; return; }   /* swallowed by \uN */
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
    if (!strcmp(w, "uc") && has_arg) {
        s->uc_skip = arg < 0 ? 0 : (int)arg;    /* \uc0: no fallback follows */
        return;
    }
    if (!strcmp(w, "u") && has_arg) {
        uint32_t cp = (arg < 0) ? (uint32_t)(arg + 65536) : (uint32_t)arg;
        s->uc_pend = 0;                         /* a \u ends the last fallback */
        rtf_char(s, cp);
        s->uc_pend = s->uc_skip;
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

int ts_open_text(ts_ctx *c, ts_format fmt, bs_stream **out)
{
    bs_stream *file = bs_file_stream(c->arena, c->io, 0, -1);
    bs_stream *st;
    flt_st *s = NULL;

    if (!file) return BS_ERR_NOMEM;

    if (fmt == TS_FMT_RTF) {
        /* RTF is 7-bit ASCII on the wire; the filter does its own decoding. */
        st = flt_new(c->arena, file, rtf_feed_outer, &s);
        if (!st) return BS_ERR_NOMEM;
        s->cp = (c->cfg->charset != TS_CS_AUTO) ? c->cfg->charset : TS_CS_CP1252;
        *out = st;
        return BS_OK;
    }

    st = ts_charset_stream(c->arena, file, c->detected);
    if (!st) return BS_ERR_NOMEM;

    st = flt_new(c->arena, st,
                 (fmt == TS_FMT_MARKDOWN) ? md_take : plain_feed, &s);
    if (!st) return BS_ERR_NOMEM;
    if (fmt == TS_FMT_MARKDOWN)
        s->eos = md_eos;
    *out = st;
    return BS_OK;
}
