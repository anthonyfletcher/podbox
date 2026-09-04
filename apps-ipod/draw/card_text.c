/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A line of mixed-size text: runs measured, wrapped together and set on a
 * common baseline.
 *
 * Parts: the run list; the word walk, which is the only place the text is
 * broken up; the line flush, which is where a line's height is finally known;
 * then the two entry points, which are the same walk with drawing turned off.
 ****************************************************************************/

#include <string.h>
#include <stdbool.h>
#include "lcd.h"
#include "font.h"
#include "card_text.h"

/* Words held before a line has to be flushed. A line is a few words across a
 * card 250 pixels wide at most; running out flushes early, which costs a
 * break in an unlikely place and nothing else. */
#define LINE_WORDS 24

/* The longest word measured in one piece. A longer one is measured in
 * LINE_CHARS-byte pieces, so it wraps mid-word rather than being cut. */
#define LINE_CHARS 64

/* Between two runs on the same line -- "12,345" then " minutes". A run
 * carries no padding of its own, so without this an emphasised figure would
 * touch the word after it. */
#define RUN_GAP 3

void card_text_reset(struct card_text *t)
{
    t->n = 0;
}

static void add(struct card_text *t, const char *s, int font,
                unsigned colour, int brk)
{
    if (!s || !*s || t->n >= CARD_RUN_MAX)
        return;

    t->run[t->n].text   = s;
    t->run[t->n].font   = (short)font;
    t->run[t->n].colour = colour;
    t->run[t->n].brk    = (unsigned char)brk;
    t->n++;
}

void card_text_add(struct card_text *t, const char *s, int font,
                   unsigned colour)
{
    add(t, s, font, colour, 0);
}

void card_text_add_line(struct card_text *t, const char *s, int font,
                        unsigned colour)
{
    add(t, s, font, colour, 1);
}

/* One word, already measured, waiting for the rest of its line. */
struct word
{
    const char *p;
    int len;
    int gap;        /* space or run gap that precedes it, zero at a break */
    int w;
    short font;
    unsigned colour;
};

/* The largest length no greater than 'len' that does not cut a character in
 * half. UTF-8 continuation bytes are 10xxxxxx, so walking back off them lands
 * on a lead byte. */
static int char_boundary(const char *p, int len)
{
    while (len > 0 && (p[len] & 0xC0) == 0x80)
        len--;
    return len;
}

static int str_width(const char *p, int len, int font)
{
    char buf[LINE_CHARS + 1];
    int w = 0;

    if (len > LINE_CHARS)
        len = LINE_CHARS;
    memcpy(buf, p, len);
    buf[len] = 0;
    font_getstringsize((const unsigned char *)buf, &w, NULL, font);
    return w;
}

/* Added between the descent of one line and the ascent of the next. This is
 * the leading, and it is all of it: the faces bring their own metrics. */
#define LEAD_PAD 3

/* Where the next line sits, carried down the block.
 *
 * The step from one baseline to the next is the descent of the line above,
 * this leading, and the ascent of the line below. Nothing block-wide comes
 * into it, and that is the point: take one leading from the block's tallest
 * face and every line is spaced as though it were a figure, which is what
 * puts a whole numeral's worth of air between "your best" and "day".
 */
struct pen
{
    int base;       /* baseline of the line just set */
    int desc;       /* how far that line reaches below it */
    int lines;
};

/* Set one line's words on their shared baseline and advance the pen. */
static void flush_line(const struct word *w, int n, int x, int top,
                       struct pen *p, bool draw)
{
    int asc = 0, desc = 0;

    for (int i = 0; i < n; i++)
    {
        struct font *f = font_get(w[i].font);
        int a = f ? f->ascent : 0;
        int d = f ? (int)f->height - a : 0;

        if (a > asc)  asc = a;
        if (d > desc) desc = d;
    }

    p->base = p->lines ? p->base + p->desc + LEAD_PAD + asc : top + asc;
    p->desc = desc;
    p->lines++;

    if (!draw)
        return;

    for (int i = 0; i < n; i++)
    {
        struct font *f = font_get(w[i].font);
        char buf[LINE_CHARS + 1];
        int len = w[i].len;

        if (len > LINE_CHARS)
            len = LINE_CHARS;
        memcpy(buf, w[i].p, len);
        buf[len] = 0;

        x += w[i].gap;
        lcd_setfont(w[i].font);
        lcd_set_foreground(w[i].colour);
        /* putsxy takes the glyph top, so a shorter run is pushed down by the
         * difference in ascent rather than sitting level with the tallest. */
        lcd_putsxy(x, p->base - (f ? f->ascent : 0), buf);
        x += w[i].w;
    }
}

/* Put an ellipsis on a line that had to stop early, dropping trailing words
 * until it fits -- but never the last of them.
 *
 * Three ASCII dots rather than U+2026, because a figure is set in a face
 * subset to seventeen glyphs and the ellipsis is not among them -- the full
 * stop is. */
static void ellipsise(struct word *line, int *n, int *pen, int max_w,
                      int font, unsigned colour)
{
    int ew = str_width("...", 3, font);

    while (*n > 1 && *pen + ew > max_w)
    {
        (*n)--;
        *pen -= line[*n].w + line[*n].gap;
        colour = line[*n].colour;
        font = line[*n].font;
        ew = str_width("...", 3, font);
    }

    /* Never down to nothing. A word wider than the card keeps its place and
     * is clipped by the card's own edge, which reads as a name cut short;
     * three dots standing alone read as a fault. */
    if (*n >= LINE_WORDS || *pen + ew > max_w)
        return;

    line[*n].p      = "...";
    line[*n].len    = 3;
    line[*n].gap    = 0;
    line[*n].w      = ew;
    line[*n].font   = (short)font;
    line[*n].colour = colour;
    (*n)++;
    *pen += ew;
}

/* The one walk. Both entry points are this with 'draw' set or not, so a
 * measured block and a drawn one cannot break in different places. */
static int walk(const struct card_text *t, int x, int y, int max_w,
                int max_lines, bool draw, int *out_w, int *out_h)
{
    struct pen pen_y = { 0, 0, 0 };
    struct word line[LINE_WORDS];
    int n = 0, pen = 0, widest = 0;
    int last_run = -1;

    lcd_set_drawmode(DRMODE_FG);

    for (int i = 0; i < t->n; i++)
    {
        const char *p = t->run[i].text;

        if (t->run[i].brk && n > 0)
        {
            if (max_lines && pen_y.lines + 1 >= max_lines)
            {
                ellipsise(line, &n, &pen, max_w, t->run[i].font,
                          t->run[i].colour);
                if (pen > widest)
                    widest = pen;
                flush_line(line, n, x, y, &pen_y, draw);
                n = 0;
                goto done;
            }
            if (pen > widest)
                widest = pen;
            flush_line(line, n, x, y, &pen_y, draw);
            n = 0;
            pen = 0;
        }

        while (*p)
        {
            const char *e;
            int len, ww, gap;

            while (*p == ' ')
                p++;
            if (!*p)
                break;

            e = p;
            while (*e && *e != ' ')
                e++;
            len = (int)(e - p);
            if (len > LINE_CHARS)
                len = LINE_CHARS;
            len = char_boundary(p, len);

            ww = str_width(p, len, t->run[i].font);

            /* A word too wide for a whole line is broken inside itself,
             * between characters.
             *
             * Not an edge case: Japanese and Chinese put no spaces between
             * words at all, so a track name in either is one "word" the width
             * of the card several times over. Without this it is placed
             * whole, runs off the card's edge and the rest of it is lost. */
            if (n == 0 && ww > max_w && len > 1)
            {
                /* Guess by proportion first, then walk. One measurement per
                 * character removed is forty measurements for a forty-
                 * character title, EVERY FRAME it is on screen; from a
                 * proportional guess it is two or three. */
                int guess = len * max_w / ww;

                if (guess < 1)
                    guess = 1;
                if (guess < len)
                {
                    len = char_boundary(p, guess);
                    if (len < 1)
                        len = char_boundary(p, guess + 1);
                    ww = str_width(p, len, t->run[i].font);
                }
                while (ww > max_w && len > 1)
                {
                    len = char_boundary(p, len - 1);
                    ww = str_width(p, len, t->run[i].font);
                }
            }
            /* A space between two words of the same run, and RUN_GAP between
             * two runs; at the start of a line, neither. */
            gap = (n == 0) ? 0
                 : (last_run == i ? str_width(" ", 1, t->run[i].font)
                                  : RUN_GAP);

            if (n > 0 && (pen + gap + ww > max_w || n >= LINE_WORDS))
            {
                /* The line that would follow is one too many, so this one is
                 * the last and says so. */
                if (max_lines && pen_y.lines + 1 >= max_lines)
                {
                    ellipsise(line, &n, &pen, max_w, t->run[i].font,
                              t->run[i].colour);
                    if (pen > widest)
                        widest = pen;
                    flush_line(line, n, x, y, &pen_y, draw);
                    n = 0;
                    goto done;
                }

                if (pen > widest)
                    widest = pen;
                flush_line(line, n, x, y, &pen_y, draw);
                n = 0;
                pen = 0;
                gap = 0;
            }

            /* The gap belongs to the word after it, which is what makes a
             * line that breaks here open flush against the margin. */
            line[n].p      = p;
            line[n].len    = len;
            line[n].gap    = gap;
            line[n].w      = ww;
            line[n].font   = t->run[i].font;
            line[n].colour = t->run[i].colour;

            pen += ww + gap;
            n++;
            last_run = i;
            p += len;
        }
    }

    if (n > 0)
    {
        if (pen > widest)
            widest = pen;
        flush_line(line, n, x, y, &pen_y, draw);
    }

done:
    if (out_w) *out_w = widest;
    /* Top of the first line to the bottom of the last, and no further: a
     * block measured for centring must not carry a line's leading under it. */
    if (out_h) *out_h = pen_y.lines ? pen_y.base + pen_y.desc - y : 0;
    return pen_y.lines;
}

int card_text_measure(const struct card_text *t, int w, int max_lines,
                      int *out_w, int *out_h)
{
    return walk(t, 0, 0, w, max_lines, false, out_w, out_h);
}

void card_text_draw(const struct card_text *t, int x, int y, int w,
                    int max_lines)
{
    walk(t, x, y, w, max_lines, true, NULL, NULL);
}

int card_text_lines_in(const struct card_text *t, int w, int room)
{
    int n, h;

    /* Down from what the text wants, a line at a time. Each pass measures the
     * block as it would really be set, so the answer accounts for the leading
     * between the lines that remain and for whichever faces they carry. */
    n = walk(t, 0, 0, w, 0, false, NULL, &h);
    while (n > 1 && h > room)
        n = walk(t, 0, 0, w, n - 1, false, NULL, &h);

    return n < 1 ? 1 : n;
}
