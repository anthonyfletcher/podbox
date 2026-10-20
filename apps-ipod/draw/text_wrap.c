/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Word-wrapping text to a pixel width, one display line at a time. Backs the
 * %wr and %wt skin tags and the text reel behind the credits and About.
 *
 * Parts: finding a line's end, then counting and drawing whole texts.
 ****************************************************************************/
#include "config.h"
#include <string.h>
#include "font.h"
#include "rbunicode.h"
#include "diacritic.h"
#include "text_wrap.h"

/* One forward pass: add up glyph widths, remember the last space, and break
 * there on overflow -- never re-measuring a prefix. */
const char *text_wrap_next(const char *text, int width, struct font *pf,
                           const char **end)
{
    const unsigned char *start = (const unsigned char *)text;
    const unsigned char *cursor = start, *last_space = NULL;
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
        if (ch == ' ')
            last_space = prev;
        w += font_get_width(pf, ch);
        if (w > width)
        {
            if (last_space && last_space > start)
            {
                *end = (const char *)last_space;    /* break at the space */
                next = last_space;
                while (*next == ' ')                /* which is dropped */
                    next++;
            }
            else if (prev > start)
                *end = (const char *)(next = prev); /* break before ch */
            else
                *end = (const char *)(next = cursor); /* a lone wide glyph */
            return (const char *)next;
        }
    }
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
