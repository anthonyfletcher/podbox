/***************************************************************************
 * Original code from RockBox
 * was: apps/recorder/keyboard.c
 * Copyright (C) 2002 by Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * On-screen keyboard. kbd_input() edits a string via a character grid or
 * the scroll wheel; dialog_input() is the boxed single-line variant.
 ****************************************************************************/

/* Click-wheel oriented single-line text editor.
 *
 * This replaces the old on-screen virtual keyboard with a one-line editor
 * driven entirely by the click wheel. The caret is an insertion point *between*
 * characters (a bar, 0..len), not a block sitting on one:
 *
 *   - the wheel inserts a character at the bar and then cycles it in place, so
 *     text can be added anywhere, not just appended at the end;
 *   - Left/Right move the bar one gap, which commits the character being
 *     composed (the next spin inserts a fresh one);
 *   - holding Left/Right backspaces the character before the bar / deletes the
 *     one after it;
 *   - SELECT accepts and MENU cancels.
 *
 * The two caret shapes state what the wheel will do: a bar means "I will insert
 * a new character here", the inverse-video block means "I will keep changing
 * this one". See the iPod click-wheel text input specification and
 * docs/design/keyboard-gap-caret.md. This is an iPod-only build, so the old
 * keyboard's loadable layouts, morse input and touchscreen grid are
 * intentionally gone. */

#include "draw/screen_access.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "kernel.h"
#include "system.h"
#include "font.h"
#include "settings/settings.h"
#include "system/shutdown.h"
#include "rbunicode.h"
#include "input/action.h"
#include "lang.h"
#include "keyboard.h"
#include "draw/viewport.h"
#include "splash.h"
#include "yesno.h"
#include "dialog.h"

#define KBD_MARGIN 12        /* horizontal gap from the display edge to the box */
#define KBD_BTN_GAP 10       /* gap between the Cancel and OK buttons */
#define KBD_BTN_PAD_Y 5      /* vertical padding inside each button */
#define KBD_MAX_LEN 512      /* absolute cap on editable characters */
#define KBD_CHARSET_MAX 64
#define KBD_CARET_W 2        /* width of the bar caret drawn at the gap */

/* wheel acceleration: shorter gaps between wheel steps skip more characters */
#define WHEEL_FAST   (HZ/15)
#define WHEEL_VFAST  (HZ/40)

/* minimum interval between held backspace/delete repeats (slows down the
 * firmware's fast button auto-repeat so deletion isn't too quick) */
#define KBD_DEL_REPEAT (HZ/5)

struct kbd_edit {
    ucschar_t text[KBD_MAX_LEN];
    int len;
    int caret;           /* insertion point, 0..len: the gap *before* text[caret] */
    int max_len;
    int scroll;          /* horizontal pixel scroll to keep the caret visible */
    long last_wheel_tick;
    long last_del_tick;  /* throttles held backspace/delete */
    bool editing;        /* the wheel is composing text[caret-1] (implies caret>0) */
    bool on_buttons;     /* focus is on the Cancel/OK button row, not the input */
    bool btn_ok;         /* on the button row, OK is selected (else Cancel) */
    bool dirty;          /* text changed since the editor opened */
};

/* Not reentrant (there is only ever one text-input screen at a time); kept
 * out of the stack so plugin callers with small stacks are safe. */
static struct kbd_edit st;

/* The default (fallback) character cycle, used if the localized charset
 * string is missing or empty. Space first, then A-Z, 0-9 and a little
 * punctuation -- no lowercase (see the spec). */
static const ucschar_t default_charset[] = {
    ' ','A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
    'Q','R','S','T','U','V','W','X','Y','Z','0','1','2','3','4','5','6',
    '7','8','9','-','_','.','\'','&'
};

static ucschar_t charset[KBD_CHARSET_MAX];
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
    while (p && *p && charset_len < KBD_CHARSET_MAX)
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
    if (charset_index(' ') < 0 && charset_len < KBD_CHARSET_MAX)
        charset[charset_len++] = ' ';
}

static void insert_at(struct kbd_edit *s, int pos, ucschar_t ch)
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
static bool wheel_step(struct kbd_edit *s, int dir)
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
static void caret_left(struct kbd_edit *s)
{
    if (s->caret > 0)
        s->caret--;
    s->editing = false;
}

static void caret_right(struct kbd_edit *s)
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

static void do_backspace(struct kbd_edit *s)
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

static void do_delete(struct kbd_edit *s)
{
    /* delete the character after the bar; everything shifts left */
    if (s->caret >= s->len)
        return;
    for (int i = s->caret; i < s->len - 1; i++)
        s->text[i] = s->text[i + 1];
    s->len--;
    s->editing = false;
}

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

/* Box spans the physical display width minus a horizontal margin; height fits
 * the content (edit line + button row) and is centred on the display, matching
 * the yes/no dialog. box->x/y are absolute screen coordinates, so the geometry
 * must come from the display (lcdwidth/lcdheight), not from the theme viewport
 * (getwidth()/getheight()), which a themed SBS can inset - see yesno_measure(). */
static void kbd_measure(struct dialog *d, struct screen *display,
                        struct viewport *box, void *data)
{
    (void)data;
    struct dialog_insets in;
    int area_y = 0;
    int area_h = display->lcdheight;
    int sw = display->lcdwidth;
    int ch_h = display->getcharheight();

    dialog_get_insets(&d->style, &in);

    int bh = ch_h + 2 * KBD_BTN_PAD_Y;      /* button height */
    int gap = ch_h / 2;                     /* gap between edit line and buttons */
    int box_h = ch_h + gap + bh + in.top + in.bottom;
    if (box_h > area_h)
        box_h = area_h;

    box->x = KBD_MARGIN;
    box->width = sw - 2 * KBD_MARGIN;
    box->y = area_y + (area_h - box_h) / 2;
    box->height = box_h;
}

static void kbd_draw(struct dialog *d, struct screen *display,
                     struct viewport *content, void *data)
{
    struct kbd_edit *s = data;
    int ch_h = display->getcharheight();
    int bh = ch_h + 2 * KBD_BTN_PAD_Y;      /* button height */

    /* --- edit line in a clipped sub-viewport so long text can scroll --- */
    struct viewport tvp = *content;
    tvp.height = ch_h;
    display->set_viewport(&tvp);

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
    if (caret_x + KBD_CARET_W > s->scroll + tvp.width)
        s->scroll = caret_x + KBD_CARET_W - tvp.width;
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
            display->fillrect(x, 0, cw, tvp.height);
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
    display->fillrect(caret_x - s->scroll, 0, KBD_CARET_W, tvp.height);

    /* --- bottom Cancel / OK buttons (triggered by MENU / SELECT) --- */
    display->set_viewport(content);
    display->set_drawmode(DRMODE_SOLID);

    int by = content->height - bh;
    int bw = (content->width - KBD_BTN_GAP) / 2;
    if (bw > 0)
    {
        dialog_draw_button(display, &d->style, 0, by, bw, bh,
                           str(LANG_DIAG_CANCEL),
                           s->on_buttons && !s->btn_ok);
        dialog_draw_button(display, &d->style, bw + KBD_BTN_GAP, by, bw, bh,
                           str(LANG_DIAG_OK),
                           s->on_buttons && s->btn_ok);
    }

    display->set_drawmode(DRMODE_SOLID);
}

/* Confirm discarding edits (only if the text changed); returns the disposition
 * dialog_run() should act on. */
static int kbd_confirm_cancel(struct kbd_edit *s)
{
    if (s->dirty)
    {
        static const unsigned char *lines[1];
        lines[0] = (const unsigned char *)ID2P(LANG_KBD_DISCARD);
        struct text_message message = { (const char **)lines, 1 };
        if (gui_syncyesno_run(&message, NULL, NULL) != YESNO_YES)
        {
            s->on_buttons = false;   /* keep editing, back to the input */
            return DIALOG_CONTINUE;
        }
    }
    return DIALOG_CANCEL;
}

static int kbd_on_action(struct dialog *d, int action, void *data)
{
    (void)d;
    struct kbd_edit *s = data;

    switch (action)
    {
        case ACTION_KBD_DOWN:      /* wheel forward */
            if (s->on_buttons)
                s->btn_ok = !s->btn_ok;         /* toggle OK/Cancel */
            else if (wheel_step(s, +1))         /* insert / advance character */
                s->dirty = true;
            break;
        case ACTION_KBD_UP:        /* wheel back */
            if (s->on_buttons)
                s->btn_ok = !s->btn_ok;
            else if (wheel_step(s, -1))         /* insert / reverse character */
                s->dirty = true;
            break;
        case ACTION_KBD_LEFT:      /* tap left */
            if (s->on_buttons)
                s->btn_ok = false;              /* Cancel (left button) */
            else
                caret_left(s);
            break;
        case ACTION_KBD_RIGHT:     /* tap right */
            if (s->on_buttons)
                s->btn_ok = true;               /* OK (right button) */
            else
                caret_right(s);
            break;
        case ACTION_KBD_BACKSPACE: /* hold left -> backspace (throttled) */
            if (!s->on_buttons &&
                current_tick - s->last_del_tick >= KBD_DEL_REPEAT)
            {
                do_backspace(s);
                s->dirty = true;
                s->last_del_tick = current_tick;
            }
            break;
        case ACTION_KBD_DELETE:    /* hold right -> delete after the bar (throttled) */
            if (!s->on_buttons &&
                current_tick - s->last_del_tick >= KBD_DEL_REPEAT)
            {
                do_delete(s);
                s->dirty = true;
                s->last_del_tick = current_tick;
            }
            break;
        case ACTION_KBD_DONE:      /* PLAY: move focus down to the buttons */
            if (!s->on_buttons)
            {
                s->on_buttons = true;
                s->btn_ok = true;               /* start on OK */
            }
            break;
        case ACTION_KBD_SELECT:    /* accept, or confirm the chosen button */
            if (s->on_buttons && !s->btn_ok)
                return kbd_confirm_cancel(s);   /* Cancel button */
            return DIALOG_ACCEPT;               /* OK / input shortcut */
        case ACTION_KBD_ABORT:     /* MENU: up to the input, or cancel */
            if (s->on_buttons)
            {
                s->on_buttons = false;          /* back up to the input */
                break;
            }
            return kbd_confirm_cancel(s);
        default:
            if (default_event_handler(action) == SYS_USB_CONNECTED)
                return DIALOG_ABORT;
            break;
    }
    return DIALOG_CONTINUE;
}

/* Click-wheel single-line text editor. Returns 0 on accept (buffer updated),
 * -1 on cancel or USB (buffer left unchanged). */
int dialog_input(char* text, int buflen)
{
    static const struct dialog_callbacks cb = {
        .measure   = kbd_measure,
        .draw      = kbd_draw,
        .on_action = kbd_on_action,
    };
    struct dialog d;

    build_charset();

    st.max_len = buflen - 1;
    if (st.max_len > KBD_MAX_LEN - 1)
        st.max_len = KBD_MAX_LEN - 1;
    if (st.max_len < 1)
        st.max_len = 1;

    /* decode the incoming UTF-8 into codepoints */
    st.len = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p && st.len < st.max_len)
    {
        ucschar_t ch;
        p = utf8decode(p, &ch);
        if (ch == 0)
            break;
        st.text[st.len++] = ch;
    }
    /* Open with the bar at the end of the existing text, composing nothing: the
     * first wheel spin inserts a character there. An empty line is legal. */
    st.caret = st.len;
    st.editing = false;
    st.scroll = 0;
    st.last_wheel_tick = current_tick;
    st.last_del_tick = 0;
    st.on_buttons = false;   /* focus starts on the input line */
    st.btn_ok = true;
    st.dirty = false;

    dialog_init(&d, CONTEXT_KEYBOARD, NULL, NULL, &cb, &st);
    if (dialog_run(&d, TIMEOUT_BLOCK) != DIALOG_ACCEPT)
        return -1;           /* cancelled or USB */

    /* trim leading and trailing spaces, then re-encode into the caller buffer */
    while (st.len > 0 && st.text[st.len - 1] == ' ')
        st.len--;
    int trim_start = 0;
    while (trim_start < st.len && st.text[trim_start] == ' ')
        trim_start++;
    {
        unsigned char *out = (unsigned char *)text;
        unsigned char *end = out + buflen - 1;
        for (int i = trim_start; i < st.len; i++)
        {
            unsigned char tmp[8];
            unsigned char *e = utf8encode(st.text[i], tmp);
            int n = e - tmp;
            if (out + n > end)
                break;   /* no room left in the caller's buffer */
            memcpy(out, tmp, n);
            out += n;
        }
        *out = '\0';
    }
    return 0;
}

/* Plugin-ABI wrapper: the loadable-layout `kbd` argument is ignored. */
int kbd_input(char* text, int buflen, ucschar_t *kbd)
{
    (void)kbd;
    return dialog_input(text, buflen);
}
