/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Word-wrapping text to a pixel width, one display line at a time. Backs the
 * %wr and %wt skin tags, the text reel behind the credits and About, dialog
 * text, splashes, view_text() and the lyric viewer.
 *
 * Parts: finding a line's end, then counting and drawing whole texts.
 ****************************************************************************/
#include "config.h"
#include <string.h>
#include "font.h"
#include "rbunicode.h"
#include "diacritic.h"
#include "text_wrap.h"

/* What a line may break after besides a space: CJK is written without
 * spaces, so a Japanese or Chinese line would otherwise only ever break at
 * the hard edge. */
static bool is_punct_break(ucschar_t ch)
{
    static const ucschar_t brk[] = {
        '\t', '!', ',', '-', '.', ':', ';', '?',
        0x3000, 0x3001, 0x3002,                     /* 　、。 */
        0xff01, 0xff0c, 0xff0e, 0xff1a, 0xff1b, 0xff1f, /* ！，．：；？ */
        0x2015,                                     /* ― */
    };
    unsigned i;

    for (i = 0; i < sizeof(brk) / sizeof(brk[0]); i++)
        if (ch == brk[i])
            return true;
    return false;
}

/* One forward pass: add up glyph widths, remember the last place a break is
 * allowed, and break there on overflow -- never re-measuring a prefix. */
static const char *wrap_next(const char *text, int width, struct font *pf,
                             bool punct, const char **end)
{
    const unsigned char *start = (const unsigned char *)text;
    const unsigned char *cursor = start;
    const unsigned char *cut = NULL, *cut_next = NULL; /* the last break */
    const unsigned char *next;
    int w = 0;
    ucschar_t ch;

    for (;;)
    {
        const unsigned char *prev = cursor;
        cursor = utf8decode(cursor, &ch);
        if (ch == 0)
        {
            *end = (const char *)prev;
            return (const char *)prev;
        }
        if (ch == '\n' || ch == '\r')
        {
            *end = (const char *)prev;
            return (const char *)((ch == '\r' && *cursor == '\n')
                                  ? cursor + 1 : cursor);
        }
        if (IS_DIACRITIC(ch))
            continue;
        if (ch == ' ')                  /* break at it; it is dropped */
        {
            cut = prev;
            cut_next = cursor;
        }
        w += font_get_width(pf, ch);
        if (w > width)
        {
            if (cut && cut > start)
            {
                *end = (const char *)cut;
                next = cut_next;
                while (*next == ' ')
                    next++;
            }
            else if (prev > start)
                *end = (const char *)(next = prev); /* break before ch */
            else
                *end = (const char *)(next = cursor); /* a lone wide glyph */
            return (const char *)next;
        }
        /* after the overflow test, so punctuation that does not fit is not
         * kept on the line */
        if (punct && is_punct_break(ch))
            cut = cut_next = cursor;
    }
}

const char *text_wrap_next(const char *text, int width, struct font *pf,
                           const char **end)
{
    return wrap_next(text, width, pf, false, end);
}

const char *text_wrap_next_punct(const char *text, int width,
                                 struct font *pf, const char **end)
{
    return wrap_next(text, width, pf, true, end);
}

int text_wrap_count(const char *text, int width, int font)
{
    struct font *pf = font_get(font);
    const char *end;
    int n = 0;

    font_lock(font, true);
    while (*text)
    {
        text = text_wrap_next(text, width, pf, &end);
        n++;
    }
    font_lock(font, false);
    return n;
}

int text_wrap_draw(struct screen *d, const char *text, int x, int y,
                   int width, char align)
{
    /* Static so a deep caller such as the skin render does not carry it on
     * its stack; every caller is on the UI thread. */
    static char line[256];
    struct viewport *vp = *d->current_viewport;
    struct font *pf = font_get(vp->font);
    int pitch = pf->height;
    int n = 0;

    font_lock(vp->font, true);
    while (*text)
    {
        const char *end;
        const char *start = text;
        int top = y + n * pitch;

        text = text_wrap_next(text, width, pf, &end);
        if (top > -pitch && top < vp->height)
        {
            int len = end - start, w, lx = x;

            if (len >= (int)sizeof(line))
                len = sizeof(line) - 1;
            memcpy(line, start, len);
            line[len] = '\0';
            d->getstringsize((const unsigned char *)line, &w, NULL);
            if (align == 'r')
                lx += width - w;
            else if (align == 'c')
                lx += (width - w) / 2;
            if (lx < x)
                lx = x;
            d->putsxy(lx, top, (const unsigned char *)line);
        }
        n++;
    }
    font_lock(vp->font, false);
    return n;
}
