/***************************************************************************
 * Original code from RockBox
 * was: apps/recorder/keyboard.c
 * Copyright (C) 2002 by Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * The click-wheel text entry line: model, key handling and rendering. Split
 * out of keyboard.c so the text-input dialog and the database search share one
 * editor -- see edit_line.h for the caret model.
 ****************************************************************************/

#include <string.h>
#include "config.h"
#include "kernel.h"
#include "system.h"
#include "font.h"
#include "lang.h"
#include "rbunicode.h"
#include "input/action.h"
#include "draw/screen_access.h"
#include "edit_line.h"

#define CARET_W 2            /* width of the bar caret drawn at the gap */
#define CHARSET_MAX 64

/* wheel acceleration: shorter gaps between wheel steps skip more characters */
#define WHEEL_FAST   (HZ/15)
#define WHEEL_VFAST  (HZ/40)

/* minimum interval between held backspace/delete repeats (slows down the
 * firmware's fast button auto-repeat so deletion isn't too quick) */
#define DEL_REPEAT (HZ/5)

/* ---- the character cycle ------------------------------------------------ */

/* The default (fallback) character cycle, used if the localized charset
 * string is missing or empty. Space first, then A-Z, 0-9 and a little
 * punctuation -- no lowercase (see the spec). */
static const ucschar_t default_charset[] = {
    ' ','A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
    'Q','R','S','T','U','V','W','X','Y','Z','0','1','2','3','4','5','6',
    '7','8','9','-','_','.','\'','&'
};

static ucschar_t charset[CHARSET_MAX];
static int charset_len;

static int charset_index(ucschar_t ch)
{
    for (int i = 0; i < charset_len; i++)
        if (charset[i] == ch)
            return i;
    return -1;
}

static void build_charset(void)
{
    /* Derive the wheel's character cycle from a localized string so
     * translations can supply their own; fall back to the built-in set. */
    const unsigned char *p = (const unsigned char *)str(LANG_KBD_CLICKWHEEL_CHARSET);
    charset_len = 0;
    while (p && *p && charset_len < CHARSET_MAX)
    {
        ucschar_t ch;
        p = utf8decode(p, &ch);
        if (ch == 0)
            break;
        charset[charset_len++] = ch;
    }
    if (charset_len == 0)
    {
        int n = (int)(sizeof(default_charset) / sizeof(default_charset[0]));
        for (charset_len = 0; charset_len < n; charset_len++)
            charset[charset_len] = default_charset[charset_len];
    }
    /* space must be in the cycle: it is the empty-string / blank character */
    if (charset_index(' ') < 0 && charset_len < CHARSET_MAX)
        charset[charset_len++] = ' ';
}

/* ---- the model ---------------------------------------------------------- */

static void insert_at(struct edit_line *s, int pos, ucschar_t ch)
{
    for (int i = s->len; i > pos; i--)
        s->text[i] = s->text[i - 1];
    s->text[pos] = ch;
    s->len++;
}

/* dir: +1 advance, -1 reverse. Acceleration: fast spins skip characters.
 *
 * Not composing yet: open a gap at the caret with a blank character, step onto
 * it, and start composing -- so one forward click yields the first character of
 * the cycle rather than overwriting whatever was there. Already composing: cycle
 * that character in place. Returns false if the line is full (nothing changed). */
static bool wheel_step(struct edit_line *s, int dir)
{
    long now = current_tick;
    long dt = now - s->last_wheel_tick;
    s->last_wheel_tick = now;

    int mag = 1;
    if (dt < WHEEL_VFAST)
        mag = 4;
    else if (dt < WHEEL_FAST)
        mag = 2;

    if (!s->editing)
    {
        if (s->len >= s->max_len)
            return false;
        insert_at(s, s->caret, ' ');   /* build_charset() guarantees ' ' is in
                                        * the cycle, so the step below finds it */
        s->caret++;
        s->editing = true;
    }

    int idx = charset_index(s->text[s->caret - 1]);
    if (idx < 0)
        idx = 0;
    idx = (idx + dir * mag) % charset_len;
    if (idx < 0)
        idx += charset_len;
    s->text[s->caret - 1] = charset[idx];
    return true;
}

/* Moving the bar commits the character being composed: the next spin inserts a
 * fresh one rather than editing this one again. Right at the end of the line
 * does not grow it -- that is the wheel's job now. */
static void caret_left(struct edit_line *s)
{
    if (s->caret > 0)
        s->caret--;
    s->editing = false;
}

static void caret_right(struct edit_line *s)
{
    /* While composing, s->caret already sits past the character, which is drawn
     * as a block on it -- advancing again would skip a gap. */
    if (s->editing)
    {
        s->editing = false;
        return;
    }
    if (s->caret < s->len)
        s->caret++;
}

static void do_backspace(struct edit_line *s)
{
    /* delete the character before the bar; everything shifts left */
    if (s->caret <= 0)
        return;
    for (int i = s->caret - 1; i < s->len - 1; i++)
        s->text[i] = s->text[i + 1];
    s->len--;
    s->caret--;
    s->editing = false;
}

static void do_delete(struct edit_line *s)
{
    /* delete the character after the bar; everything shifts left */
    if (s->caret >= s->len)
        return;
    for (int i = s->caret; i < s->len - 1; i++)
        s->text[i] = s->text[i + 1];
    s->len--;
    s->editing = false;
}

void edit_line_init(struct edit_line *s, const char *utf8, int max_len)
{
    build_charset();

    s->max_len = max_len;
    if (s->max_len > EDIT_LINE_MAX - 1)
        s->max_len = EDIT_LINE_MAX - 1;
    if (s->max_len < 1)
        s->max_len = 1;

    s->len = 0;
    const unsigned char *p = (const unsigned char *)utf8;
    while (p && *p && s->len < s->max_len)
    {
        ucschar_t ch;
        p = utf8decode(p, &ch);
        if (ch == 0)
            break;
        s->text[s->len++] = ch;
    }

    /* Open with the bar at the end of the existing text, composing nothing: the
     * first wheel spin inserts a character there. An empty line is legal. */
    s->caret = s->len;
    s->editing = false;
    s->scroll = 0;
    s->last_wheel_tick = current_tick;
    s->last_del_tick = 0;
}

int edit_line_get(const struct edit_line *s, char *out, int outlen, bool trim)
{
    int last = s->len;
    int first = 0;

    if (trim)
    {
        while (last > 0 && s->text[last - 1] == ' ')
            last--;
        while (first < last && s->text[first] == ' ')
            first++;
    }

    unsigned char *o = (unsigned char *)out;
    unsigned char *end = o + outlen - 1;
    for (int i = first; i < last; i++)
    {
        unsigned char tmp[8];
        unsigned char *e = utf8encode(s->text[i], tmp);
        int n = e - tmp;
        if (o + n > end)
            break;   /* no room left in the caller's buffer */
        memcpy(o, tmp, n);
        o += n;
    }
    *o = '\0';
    return (char *)o - out;
}

bool edit_line_owns_action(int action)
{
    switch (action)
    {
        case ACTION_KBD_UP:
        case ACTION_KBD_DOWN:
        case ACTION_KBD_LEFT:
        case ACTION_KBD_RIGHT:
        case ACTION_KBD_BACKSPACE:
        case ACTION_KBD_DELETE:
            return true;
    }
    return false;
}

bool edit_line_action(struct edit_line *s, int action)
{
    switch (action)
    {
        case ACTION_KBD_DOWN:      /* wheel forward */
            return wheel_step(s, +1);
        case ACTION_KBD_UP:        /* wheel back */
            return wheel_step(s, -1);
        case ACTION_KBD_LEFT:      /* tap left */
            caret_left(s);
            break;
        case ACTION_KBD_RIGHT:     /* tap right */
            caret_right(s);
            break;
        case ACTION_KBD_BACKSPACE: /* hold left -> backspace (throttled) */
            if (current_tick - s->last_del_tick >= DEL_REPEAT)
            {
                do_backspace(s);
                s->last_del_tick = current_tick;
                return true;
            }
            break;
        case ACTION_KBD_DELETE:    /* hold right -> delete after the bar */
            if (current_tick - s->last_del_tick >= DEL_REPEAT)
            {
                do_delete(s);
                s->last_del_tick = current_tick;
                return true;
            }
            break;
    }
    return false;
}

/* ---- rendering ---------------------------------------------------------- */

static int uc_width(struct screen *display, ucschar_t ch)
{
    unsigned char tmp[8];
    unsigned char *e = utf8encode(ch, tmp);
    *e = '\0';
    int w;
    display->getstringsize((char *)tmp, &w, NULL);
    return w;
}

static void draw_char(struct screen *display, int x, int y, ucschar_t ch)
{
    unsigned char tmp[8];
    unsigned char *e = utf8encode(ch, tmp);
    *e = '\0';
    display->putsxy(x, y, (char *)tmp);
}

/* Draw `ch` over an already-filled block so it reads as inverse video. On colour
 * targets that means DRMODE_FG in the background colour, NOT
 * DRMODE_SOLID|DRMODE_INVERSEVID: inversevid only flips the glyph mask and then
 * clears itself, leaving DRMODE_SOLID, which sources the glyph's background from
 * the theme backdrop and paints over the block we just filled. */
static void draw_char_inverse(struct screen *display, int x, int y, ucschar_t ch)
{
    if (display->depth > 1)
    {
        unsigned fg = display->get_foreground();
        display->set_foreground(display->get_background());
        display->set_drawmode(DRMODE_FG);
        draw_char(display, x, y, ch);
        display->set_foreground(fg);
        return;
    }
    display->set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    draw_char(display, x, y, ch);
}

int edit_line_height(struct screen *display)
{
    return display->getcharheight();
}

void edit_line_draw(struct screen *display, struct edit_line *s, int width)
{
    int height = edit_line_height(display);

    /* The bar sits at the gap before text[caret] (caret == len is legal, so the
     * text is never indexed here). Keep it visible, along with the character
     * being composed just left of it. */
    int caret_x = 0;
    for (int i = 0; i < s->caret; i++)
        caret_x += uc_width(display, s->text[i]);

    int vis_left = caret_x;
    if (s->editing)
        vis_left -= uc_width(display, s->text[s->caret - 1]);
    if (vis_left < s->scroll)
        s->scroll = vis_left;
    if (caret_x + CARET_W > s->scroll + width)
        s->scroll = caret_x + CARET_W - width;
    if (s->scroll < 0)
        s->scroll = 0;

    int x = -s->scroll;
    for (int i = 0; i < s->len; i++)
    {
        int cw = uc_width(display, s->text[i]);
        if (s->editing && i == s->caret - 1)
        {
            /* the character the wheel is composing: inverse-video block */
            display->set_drawmode(DRMODE_FG);
            display->fillrect(x, 0, cw, height);
            draw_char_inverse(display, x, 0, s->text[i]);
        }
        else
        {
            /* DRMODE_FG so glyphs draw over our fill without the theme
             * backdrop bleeding into each character's background */
            display->set_drawmode(DRMODE_FG);
            draw_char(display, x, 0, s->text[i]);
        }
        x += cw;
    }

    /* the insertion bar itself, over the gap */
    display->set_drawmode(DRMODE_FG);
    display->fillrect(caret_x - s->scroll, 0, CARET_W, height);

    display->set_drawmode(DRMODE_SOLID);
}
