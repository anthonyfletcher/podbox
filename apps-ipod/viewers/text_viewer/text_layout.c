/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Lines of the ts_* engine's text: where they break and how they are drawn.
 * The text viewer lays its pages out with it, and the bios and notes screen
 * its pages.
 *
 * Parts, in order:
 *   - widths, with the engine's invisible marks left out and bold text in
 *     the bold font
 *   - breaking a line: at a space, at a soft hyphen, or inside a word too
 *     wide for any line; then its height, which bold can raise
 *   - drawing a line: in one call when it can be, word by word when it holds
 *     marks or is justified
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "system.h"              /* MAX */
#include "font.h"
#include "draw/screen_access.h"   /* screens[] */
#include "text_layout.h"

/* ---- widths ----------------------------------------------------------- */

/* font_getstringnsize()'s "maxbytes" is really a codepoint count -- it
 * decrements once per utf8decode() and leans on unsigned underflow to stop
 * (firmware/font.c) -- and it halts at a NUL either way, so it cannot bound a
 * byte range. The text is the caller's and single-threaded, so a temporary
 * NUL is both simpler and cheaper than counting codepoints first. */
static int tl_width_in(int font, unsigned char *s, size_t n)
{
    unsigned char save = s[n];
    int w;

    s[n] = '\0';
    font_getstringsize(s, &w, NULL, font);
    s[n] = save;
    return w;
}

int tl_width(const struct text_layout *tl, unsigned char *s, size_t n)
{
    return tl_width_in(tl->font, s, n);
}

/* The font text is set in, bold or not */
static int tl_font(const struct text_layout *tl, bool bold)
{
    return bold && tl->bold >= 0 ? tl->bold : tl->font;
}

/* Whether the mark at `s`, `h` bytes of it, opens or closes bold, a
 * heading's too under head_bold: the new state, or `bold` as it was */
static bool tl_bold_after(const struct text_layout *tl, const unsigned char *s,
                          size_t h, bool bold)
{
    if (h == 3 && s[0] == 0xee && s[1] == 0x80)
    {
        if (s[2] == TL_BOLD_OPEN || (tl->head_bold && s[2] == 0x82))
            return true;
        if (s[2] == TL_BOLD_CLOSE
            || (tl->head_bold && (s[2] == 0x83 || s[2] == TL_MINOR)))
            return false;
    }
    return bold;
}

/* A soft hyphen, U+00AD, is two bytes in UTF-8. It is invisible and takes
 * no width, and a line may break at it with a '-' drawn. */
bool tl_is_shy(const unsigned char *s, size_t n)
{
    return n >= 2 && s[0] == 0xc2 && s[1] == 0xad;
}

/* TS_HEADING_OPEN and TS_HEADING_CLOSE: the text between them is
 * underlined */
bool tl_is_heading_mark(const unsigned char *s, size_t n)
{
    return n >= 3 && s[0] == 0xee && s[1] == 0x80
           && (s[2] == 0x82 || s[2] == 0x83);
}

/* A span from the mark ending in byte `open` to the one ending in `close`
 * at `s`: its length, all of `n` when it ends inside it, or 0 */
static size_t tl_span(const unsigned char *s, size_t n, unsigned char open,
                      unsigned char close)
{
    if (n < 3 || s[0] != 0xee || s[1] != 0x80 || s[2] != open)
        return 0;
    for (size_t i = 3; i + 3 <= n; i++)
        if (s[i] == 0xee && s[i + 1] == 0x80 && s[i + 2] == close)
            return i + 3;
    return n;
}

/* A note's mark -- TS_NOTE_OPEN, its href, TS_NOTE_CLOSE -- at `s`: its
 * length, all of `n` when they end inside it, or 0 */
size_t tl_note_mark(const unsigned char *s, size_t n)
{
    return tl_span(s, n, 0x84, 0x85);
}

/* Markdown's marks, TS_DIM_OPEN to TS_BOLD_CLOSE */
bool tl_is_style_mark(const unsigned char *s, size_t n)
{
    return n >= 3 && s[0] == 0xee && s[1] == 0x80
           && s[2] >= TL_DIM_OPEN && s[2] <= TL_MD_LAST;
}

/* TS_BULLET and TS_NUMBERED */
static bool tl_is_item_mark(const unsigned char *s, size_t n)
{
    return n >= 3 && s[0] == 0xee && s[1] == 0x80
           && (s[2] == TL_BULLET || s[2] == TL_NUMBERED);
}

static bool tl_is_minor_mark(const unsigned char *s, size_t n)
{
    return n >= 3 && s[0] == 0xee && s[1] == 0x80 && s[2] == TL_MINOR;
}

size_t tl_hidden(const unsigned char *s, size_t n)
{
    return tl_is_shy(s, n) ? 2
           : tl_is_heading_mark(s, n) || tl_is_style_mark(s, n)
             || tl_is_item_mark(s, n) || tl_is_minor_mark(s, n) ? 3
           : tl_note_mark(s, n) ? tl_note_mark(s, n)
           : tl_span(s, n, TL_HIDE_OPEN, TL_HIDE_CLOSE);
}

int tl_item_indent(const struct text_layout *tl, unsigned char *s, size_t n,
                   int step, int *first, int *bullet)
{
    size_t i = 0, w;
    int levels = 0;

    *first = 0;
    *bullet = -1;
    while (i + 3 <= n && tl_is_item_mark(s + i, n - i) && s[i + 2] == s[2])
    {
        levels++;
        i += 3;
    }
    if (!levels)
        return 0;
    *first = (levels - 1) * step;
    if (s[2] == TL_BULLET)
    {
        *bullet = *first + step / 4;
        *first += step;
        return *first;
    }
    /* the number, and the space after it */
    for (w = i; w < n && s[w] != ' ' && s[w] != '\n'; w++)
        ;
    if (w < n && s[w] == ' ')
        w++;
    return *first + tl_width(tl, s + i, w - i);
}

void tl_draw_bullet(const struct tl_line *ln, int x, int y)
{
    int d = MAX(4, ln->ascent / 3), top = y + ln->ascent * 2 / 3 - d / 2;

    screens[SCREEN_MAIN].fillrect(x + 1, top, d - 2, d);
    screens[SCREEN_MAIN].fillrect(x, top + 1, d, d - 2);
}

int tl_text_width(const struct text_layout *tl, unsigned char *s, size_t n,
                  bool *bold)
{
    size_t i = 0, start = 0;
    int w = 0;

    while (i < n)
    {
        size_t h = tl_hidden(s + i, n - i);

        if (h)
        {
            w += tl_width_in(tl_font(tl, *bold), s + start, i - start);
            *bold = tl_bold_after(tl, s + i, h, *bold);
            i += h;
            start = i;
        }
        else
            i++;
    }
    return w + tl_width_in(tl_font(tl, *bold), s + start, n - start);
}

/* tl_width() cannot be given a constant: it writes past what it measures */
int tl_literal_width(const struct text_layout *tl, const char *s)
{
    int w;

    font_getstringsize((const unsigned char *)s, &w, NULL, tl->font);
    return w;
}

/* ---- breaking a line --------------------------------------------------- */

/* Breaks at the last space, or at a soft hyphen inside the word that does
 * not fit. Rockbox bitmap fonts have no kerning -- font_getstringnsize() just
 * sums per-glyph widths -- so accumulating word widths is exact and avoids
 * re-measuring the line prefix per word. */
static void tl_break(const struct text_layout *tl, unsigned char *s,
                     size_t avail, int width, struct tl_line *ln)
{
    size_t i = 0;
    int used = 0;
    bool bold = ln->bold;

    if (avail == 0)
        return;

    if (s[0] == '\n')                        /* explicit blank line */
    {
        ln->used = 1;
        return;
    }

    while (i < avail)
    {
        size_t word_end = i;
        bool word_bold = bold;
        int word_w;

        while (word_end < avail && s[word_end] != ' ' && s[word_end] != '\n')
            word_end++;

        word_w = tl_text_width(tl, s + i, word_end - i, &word_bold);

        if (used + word_w > width)
        {
            size_t p;

            ln->full = true;

            /* The last soft hyphen in the word whose part fits, dash and
             * all */
            for (p = word_end; p-- > i; )
            {
                bool part_bold = bold;
                int part_w, dash_w;

                if (!tl_is_shy(s + p, word_end - p))
                    continue;
                part_w = tl_text_width(tl, s + i, p - i, &part_bold);
                font_getstringsize((const unsigned char *)"-", &dash_w, NULL,
                                   tl_font(tl, part_bold));
                if (used + part_w + dash_w <= width)
                {
                    ln->draw = p;
                    ln->used = p + 2;
                    ln->width = used + part_w + dash_w;
                    ln->hyphen = true;
                    return;
                }
            }

            if (i == 0)
            {
                /* A single word too wide for the line: break inside it at the
                 * last codepoint that fits, never mid-sequence. */
                size_t j = 0;
                int w = 0;

                while (j < word_end)
                {
                    size_t k = j + 1;
                    int cw;

                    while (k < word_end && (s[k] & 0xc0) == 0x80)
                        k++;                 /* to the next codepoint start */

                    cw = tl_text_width(tl, s + j, k - j, &bold);
                    if (w + cw > width && j > 0)
                        break;
                    w += cw;
                    j = k;
                }
                ln->draw = ln->used = j;
                ln->width = w;
                return;
            }
            break;                           /* line ends before this word */
        }

        used += word_w;
        bold = word_bold;
        i = word_end;
        ln->width = used;

        if (i >= avail)
            break;

        if (s[i] == '\n')                    /* hard break ends the line */
        {
            ln->draw = i;
            ln->used = i + 1;
            return;
        }

        /* s[i] is a space: it only stays if a word follows it on this line */
        used += tl_width(tl, s + i, 1);
        i++;
    }

    /* Trim the trailing space from what is drawn, but still consume it. */
    ln->draw = (i > 0 && s[i - 1] == ' ') ? i - 1 : i;
    ln->used = i;
}

void tl_line_break(const struct text_layout *tl, unsigned char *s,
                   size_t avail, int width, struct tl_line *ln)
{
    struct font *f = font_get(tl->font);
    bool any = ln->bold;
    size_t i;

    ln->used = ln->draw = 0;
    ln->width = ln->indent = 0;
    ln->hyphen = ln->full = false;
    tl_break(tl, s, avail, width, ln);

    /* As tall as the tallest font the line draws in, on the lowest baseline
     * of the two, so they share it */
    for (i = 0; i < ln->draw && !any; i++)
        any = tl_bold_after(tl, s + i, tl_hidden(s + i, ln->draw - i),
                            false);
    ln->height = f->height;
    ln->ascent = f->ascent;
    if (any && tl->bold >= 0)
    {
        struct font *b = font_get(tl->bold);
        int below = MAX(f->height - f->ascent, b->height - b->ascent);

        ln->ascent = MAX(f->ascent, b->ascent);
        ln->height = ln->ascent + below;
    }
}

/* ---- drawing a line ---------------------------------------------------- */

/* Draws `n` bytes of one word at x, what is hidden left out, on the
 * baseline `base`; returns the x after it. A dim or a name's mark changes the
 * colour of what follows, `*fg` carrying it between words and set before each
 * run is drawn, and a bold mark the font, `*bold` carrying it alike. `*dim`
 * says whether the text is dim, which a name's end goes back to. */
static int tl_put_word(const struct text_layout *tl, unsigned char *s,
                       size_t n, int x, int base, bool *bold, bool *dim,
                       unsigned *fg)
{
    size_t i = 0, start = 0;

    while (i <= n)
    {
        size_t h = i < n ? tl_hidden(s + i, n - i) : 0;

        if (i == n || h)
        {
            int font = tl_font(tl, *bold);
            unsigned char save = s[i];

            s[i] = '\0';
            screens[SCREEN_MAIN].setfont(font);
            screens[SCREEN_MAIN].set_foreground(*fg);
            screens[SCREEN_MAIN].putsxy(x, base - font_get(font)->ascent,
                                        s + start);
            s[i] = save;
            x += tl_width_in(font, s + start, i - start);
            if (h == 3 && tl_is_style_mark(s + i, n - i)
                && s[i + 2] <= TL_DIM_CLOSE)
            {
                *dim = s[i + 2] == TL_DIM_OPEN;
                *fg = *dim ? tl->dim : tl->fg;
            }
            else if (h == 3 && tl->links && s[i + 2] == TL_NAME_OPEN)
                *fg = s + i == tl->current ? tl->link_on : tl->link;
            else if (h == 3 && tl->links && s[i + 2] == TL_NAME_CLOSE)
                *fg = *dim ? tl->dim : tl->fg;
            *bold = tl_bold_after(tl, s + i, h, *bold);
            i += h ? h : 1;
            start = i;
        }
        else
            i++;
    }
    return x;
}

/* Justified, a full line's leftover pixels are shared out between its
 * spaces; a paragraph's last line stays left. A line with neither that nor
 * anything hidden in it is drawn in one call. */
void tl_draw_line(const struct text_layout *tl, unsigned char *s,
                  const struct tl_line *ln, int y)
{
    bool justify = tl->justify && ln->full;
    bool hidden = false, bold = ln->bold, dim = tl->links && ln->dim;
    unsigned fg = screens[SCREEN_MAIN].get_foreground();
    int gaps = 0, gap = 0, extra = 0, space_w, x = ln->indent;
    int base = y + ln->ascent;
    size_t i;

    for (i = 0; i < ln->draw; i++)
    {
        if (s[i] == ' ')
            gaps++;
        else if (tl_hidden(s + i, ln->draw - i))
            hidden = true;
    }

    if (!hidden && !ln->hyphen && !bold && (!justify || gaps == 0))
    {
        unsigned char save = s[ln->draw];

        s[ln->draw] = '\0';
        screens[SCREEN_MAIN].putsxy(x, base - font_get(tl->font)->ascent, s);
        s[ln->draw] = save;
        return;
    }

    space_w = tl_literal_width(tl, " ");
    if (justify)
        extra = tl->width - ln->indent - ln->width;

    i = 0;
    while (i < ln->draw)
    {
        size_t j = i;

        if (s[i] == ' ')
        {
            x += space_w;
            if (justify)
                x += extra * (gap + 1) / gaps - extra * gap / gaps;
            gap++;
            i++;
            continue;
        }
        while (j < ln->draw && s[j] != ' ')
            j++;
        x = tl_put_word(tl, s + i, j - i, x, base, &bold, &dim, &fg);
        i = j;
    }
    /* A dim mark's colour stays set for the next line */
    screens[SCREEN_MAIN].set_foreground(fg);
    if (ln->hyphen)
        screens[SCREEN_MAIN].putsxy(x, base
                                    - font_get(tl_font(tl, bold))->ascent,
                                    (unsigned char *)"-");
    screens[SCREEN_MAIN].setfont(tl->font);
}
