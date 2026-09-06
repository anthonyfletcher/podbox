/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A dialog whose body is prose: wrapped, scrollable, with two buttons.
 *
 * Built on dialog_run(), so the theme, the skin-flush guard and the repaint
 * are the shared ones. What it adds over the yes-no dialog is a body long
 * enough to scroll, and the focus handover that follows from it.
 *
 * The wheel does both jobs, in the order a reader needs them: it scrolls the
 * text, and scrolling past the end hands focus to the buttons, where it moves
 * between them instead. Scrolling back off the first button returns to the
 * text. PLAY crosses over too, as it does between the search dialog's query
 * and its results -- there it arrives as ACTION_KBD_DOWN, and here as
 * ACTION_TREE_WPS, which means nothing inside a modal.
 ****************************************************************************/

#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "widgets/dialog.h"
#include "widgets/dialog_prose.h"
#include "system.h"
#include "kernel.h"
#include "draw/screen_access.h"
#include "draw/scrollbar.h"
#include "draw/viewport.h"
#include "input/action.h"
#include "font.h"
#include "api/misc.h"

/* Wrapped lines held at once. Longer than any explanation worth reading on a
 * 320-pixel screen; the text is clipped rather than the array overrun. */
#define PROSE_MAX_LINES  64

/* Of the box, so it reads as a dialog over the screen rather than a screen. */
#define PROSE_W_PCT      92
#define PROSE_H_PCT      82

#define PROSE_BAR_W      4      /* Scrollbar */
#define PROSE_BAR_GAP    4

enum prose_focus { PROSE_TEXT = 0, PROSE_BUTTONS };

struct prose
{
    const char *body;
    const char *accept;
    const char *cancel;

    int  scroll;        /* First visible wrapped line */
    int  lines;         /* Wrapped, as of the last draw */
    int  visible;       /* Lines the body area fits */

    enum prose_focus focus;
    int  sel;           /* 0 = cancel, 1 = accept */
};

static void prose_measure(struct dialog *d, struct screen *s,
                          struct viewport *box, void *data)
{
    (void)d; (void)data;

    box->width  = s->lcdwidth * PROSE_W_PCT / 100;
    box->height = s->lcdheight * PROSE_H_PCT / 100;
    box->x = (s->lcdwidth - box->width) / 2;
    box->y = (s->lcdheight - box->height) / 2;
}

/* The wrap, kept between repaints.
 *
 * dialog_run() repaints the box on every pass and a wheel being spun makes a
 * lot of passes, so wrapping the whole body inside each one is most of what
 * makes a fast scroll feel like it is dragging. Static rather than a field:
 * a modal dialog is single-instance by definition, and a kilobyte of slices
 * on the UI thread's stack is not free. */
static struct dialog_text_line prose_lines[PROSE_MAX_LINES];
static const char *prose_body;
static int         prose_w;
static int         prose_font;
static int         prose_n;

static int prose_wrap(const char *body, int w, int font)
{
    if (body == prose_body && w == prose_w && font == prose_font)
        return prose_n;

    prose_n = dialog_wrap_text(body, w, font, prose_lines, PROSE_MAX_LINES);
    prose_body = body;
    prose_w = w;
    prose_font = font;

    return prose_n;
}

static void prose_draw(struct dialog *d, struct screen *s,
                       struct viewport *content, void *data)
{
    struct prose *p = data;
    int fh = font_get(content->font)->height;
    int bh = fh + 8;
    /* Room for the scrollbar whether or not one is drawn. Measuring against
     * the full width and re-measuring narrower once a bar turns out to be
     * needed costs two wraps a frame, and reflows the text under the reader
     * at the moment the bar appears. */
    int text_w = content->width - PROSE_BAR_W - PROSE_BAR_GAP;
    int body_h, i, y;
    int btn_w, gap;

    /* The button row is fixed to the foot; the body gets what is left, less a
     * gap so the last line of text never sits against a button. */
    body_h = content->height - bh - fh / 2;
    if (body_h < fh)
        body_h = fh;

    p->visible = body_h / fh;
    if (p->visible < 1)
        p->visible = 1;

    p->lines = prose_wrap(p->body, text_w, content->font);

    if (p->scroll > p->lines - p->visible)
        p->scroll = p->lines - p->visible;
    if (p->scroll < 0)
        p->scroll = 0;

    s->set_drawmode(DRMODE_FG);

    for (i = 0, y = 0; i < p->visible; i++)
    {
        int n = p->scroll + i;

        if (n >= p->lines)
            break;

        s->putsxyf(0, y, "%.*s", prose_lines[n].len, prose_lines[n].str);
        y += fh;
    }

    s->set_drawmode(DRMODE_SOLID);

    /* A bar rather than a marker: it says how much is left as well as that
     * there is some, which is the question a reader actually has. */
    if (p->lines > p->visible)
    {
        gui_scrollbar_draw(s, content->width - PROSE_BAR_W, 0,
                           PROSE_BAR_W, body_h, p->lines,
                           p->scroll, p->scroll + p->visible, VERTICAL);
    }

    gap = 6;
    btn_w = (content->width - gap) / 2;
    y = content->height - bh;

    /* Neither button is highlighted while the text has focus: nothing is
     * selected then, and showing one as though it were invites a press that
     * does something else. */
    dialog_draw_button(s, &d->style, 0, y, btn_w, bh, p->cancel,
                       p->focus == PROSE_BUTTONS && p->sel == 0);
    dialog_draw_button(s, &d->style, btn_w + gap, y, btn_w, bh, p->accept,
                       p->focus == PROSE_BUTTONS && p->sel == 1);
}

static int prose_action(struct dialog *d, int action, void *data)
{
    struct prose *p = data;

    (void)d;

    switch (action)
    {
    case ACTION_STD_NEXT:
        if (p->focus == PROSE_TEXT)
        {
            if (p->scroll + p->visible < p->lines)
                p->scroll++;
            else
                p->focus = PROSE_BUTTONS;   /* past the end: hand over */
        }
        else if (p->sel < 1)
        {
            p->sel++;
        }
        return DIALOG_CONTINUE;

    case ACTION_STD_PREV:
        if (p->focus == PROSE_BUTTONS)
        {
            if (p->sel > 0)
                p->sel--;
            else
                p->focus = PROSE_TEXT;      /* back into the text */
        }
        else if (p->scroll > 0)
        {
            p->scroll--;
        }
        return DIALOG_CONTINUE;

    /* A wheel kept turning arrives as these, not as a run of the two above,
     * so without them the text only moves when the wheel is nudged.
     *
     * They scroll and never hand over. Handing over on a repeat would throw
     * the focus onto the buttons the instant a fast scroll reached the last
     * line, which is the opposite of what someone spinning to the end wants:
     * they want to arrive at the end and stop there. Crossing to the buttons
     * stays a deliberate single click. */
    case ACTION_STD_NEXTREPEAT:
        if (p->focus == PROSE_TEXT && p->scroll + p->visible < p->lines)
            p->scroll++;
        return DIALOG_CONTINUE;

    case ACTION_STD_PREVREPEAT:
        if (p->focus == PROSE_TEXT && p->scroll > 0)
            p->scroll--;
        return DIALOG_CONTINUE;

    case ACTION_STD_OK:
        /* While reading, select moves to the buttons rather than choosing
         * one. Nothing is highlighted yet, so choosing would be guessing --
         * and this is not a decision to make by accident. */
        if (p->focus == PROSE_TEXT)
        {
            p->focus = PROSE_BUTTONS;
            return DIALOG_CONTINUE;
        }
        return p->sel == 1 ? DIALOG_ACCEPT : DIALOG_CANCEL;

    /* PLAY, the same handover key the search dialog uses. There it arrives as
     * ACTION_KBD_DOWN; CONTEXT_STD gives it this name instead, and the screen
     * it would jump to means nothing from inside a modal explanation. Offered
     * as well as scrolling past the end, not instead: both are things a hand
     * reaches for. */
    case ACTION_TREE_WPS:
        p->focus = PROSE_BUTTONS;
        return DIALOG_CONTINUE;

    case ACTION_STD_MENU:
    case ACTION_STD_CANCEL:
        if (p->focus == PROSE_BUTTONS)
        {
            p->focus = PROSE_TEXT;
            return DIALOG_CONTINUE;
        }
        return DIALOG_CANCEL;

    default:
        break;
    }

    if (default_event_handler(action) == SYS_USB_CONNECTED)
        return DIALOG_ABORT;

    return DIALOG_CONTINUE;
}

static const struct dialog_callbacks prose_cb =
{
    .measure   = prose_measure,
    .draw      = prose_draw,
    .on_action = prose_action,
    .on_close  = NULL,
};

bool dialog_prose_confirm(const char *title, const char *body,
                          const char *accept_label,
                          const char *cancel_label)
{
    struct dialog d;
    struct prose p;

    memset(&p, 0, sizeof (p));
    p.body   = body;
    p.accept = accept_label;
    p.cancel = cancel_label;
    p.sel    = 1;               /* accept, once the buttons are reached */

    dialog_init(&d, CONTEXT_STD, title, NULL, &prose_cb, &p);

    return dialog_run(&d, TIMEOUT_BLOCK) == DIALOG_ACCEPT;
}
