/***************************************************************************
 * Original code from RockBox
 * was: apps/recorder/keyboard.c
 * Copyright (C) 2002 by Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * On-screen keyboard. dialog_input() edits a string in a boxed single-line
 * editor driven by the click wheel.
 ****************************************************************************/

/* The editor itself -- the caret model, the wheel and the rendering -- is
 * widgets/edit_line.c, shared with the database search. What is here is the
 * enclosure: a dialog box holding the line above a Cancel/OK row, the focus
 * that moves between the two, and the accept/discard semantics.
 *
 * This is an iPod-only build, so the old keyboard's loadable layouts, morse
 * input and touchscreen grid are intentionally gone. */

#include "draw/screen_access.h"
#include "config.h"
#include "kernel.h"
#include "system.h"
#include "system/shutdown.h"
#include "settings/settings.h"    /* ID2P */
#include "rbunicode.h"
#include "input/action.h"
#include "lang.h"
#include "keyboard.h"
#include "draw/viewport.h"
#include "yesno.h"
#include "dialog.h"
#include "edit_line.h"

#define KBD_MARGIN 12        /* horizontal gap from the display edge to the box */
#define KBD_BTN_GAP 10       /* gap between the Cancel and OK buttons */
#define KBD_BTN_PAD_Y 5      /* vertical padding inside each button */

struct kbd_edit {
    struct edit_line ed;
    bool on_buttons;     /* focus is on the Cancel/OK button row, not the input */
    bool btn_ok;         /* on the button row, OK is selected (else Cancel) */
    bool dirty;          /* text changed since the editor opened */
};

/* Not reentrant (there is only ever one text-input screen at a time); kept
 * out of the stack so plugin callers with small stacks are safe. */
static struct kbd_edit st;

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
    tvp.height = edit_line_height(display);
    display->set_viewport(&tvp);
    edit_line_draw(display, &s->ed, tvp.width);

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

    /* On the button row the wheel and the taps mean the buttons, not the text,
     * so the line only sees them while the input has focus. */
    if (edit_line_owns_action(action))
    {
        if (!s->on_buttons)
        {
            if (edit_line_action(&s->ed, action))
                s->dirty = true;
        }
        else switch (action)
        {
            case ACTION_KBD_UP:
            case ACTION_KBD_DOWN:
                s->btn_ok = !s->btn_ok;         /* toggle OK/Cancel */
                break;
            case ACTION_KBD_LEFT:
                s->btn_ok = false;              /* Cancel (left button) */
                break;
            case ACTION_KBD_RIGHT:
                s->btn_ok = true;               /* OK (right button) */
                break;
        }
        return DIALOG_CONTINUE;
    }

    switch (action)
    {
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

    edit_line_init(&st.ed, text, buflen - 1);
    st.on_buttons = false;   /* focus starts on the input line */
    st.btn_ok = true;
    st.dirty = false;

    dialog_init(&d, CONTEXT_KEYBOARD, NULL, NULL, &cb, &st);
    if (dialog_run(&d, TIMEOUT_BLOCK) != DIALOG_ACCEPT)
        return -1;           /* cancelled or USB */

    edit_line_get(&st.ed, text, buflen, true);
    return 0;
}

/* Plugin-ABI wrapper: the loadable-layout `kbd` argument is ignored. */
int kbd_input(char* text, int buflen, ucschar_t *kbd)
{
    (void)kbd;
    return dialog_input(text, buflen);
}
