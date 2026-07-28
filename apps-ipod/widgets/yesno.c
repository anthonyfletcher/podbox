/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/yesno.c
 * Copyright (C) 2005 by Kevin Ferrare
 * GNU General Public License (version 2+)
 *
 * Yes/no confirmation dialog with optional timeout and voiced prompts.
 * Returns YESNO_YES, _NO, _TMO or _USB.
 ****************************************************************************/
#include "config.h"
#include <stdio.h>
#include "yesno.h"
#include "system.h"
#include "kernel.h"
#include "system/shutdown.h"
#include "lang.h"
#include "input/action.h"
#include "speech/talk.h"
#include "settings/settings.h"
#include "draw/viewport.h"
#include "font.h"
#include "splash.h"
#include "dialog.h"

/* Message box geometry: at most the display width minus a horizontal margin,
 * height fitted to the content and centred vertically. The inner padding
 * between the border and the contents is the style's box_margin. */
#define YN_MARGIN 12   /* horizontal gap from the display edge to the box */
#define YN_MAX_LINES 16 /* cap on wrapped message display lines */
#define YN_BTN_PAD_X 12 /* horizontal padding inside each button */
#define YN_BTN_PAD_Y 5  /* vertical padding inside each button */
#define YN_BTN_GAP   18 /* gap between the two buttons */

/* Per-run yes/no state, handed to the dialog callbacks via `data`. */
struct yesno_ctx
{
    const struct text_message *message;
    enum yesno_res selection;      /* highlighted button */
    enum yesno_res tmo_default;    /* result on timeout, or YESNO_TMO for none */
    int  tmo_secs;                 /* initial timeout in seconds (button sizing) */
    long end_tick;
    long talked_tick;
    bool talk_menu;
    const struct text_message *yes_message; /* shown on accept, optional */
    const struct text_message *no_message;  /* shown on cancel, optional */
    enum yesno_res result;
    /* message word-wrapped to the box width, rebuilt by yesno_measure() */
    struct dialog_text_line wrap[YN_MAX_LINES];
    int wrap_count;
};

static void talk_text_message(const struct text_message * message, bool enqueue)
{
    int line;
    for(line=0; line < message->nb_lines; line++)
    {
        long id = P2ID((unsigned char *)message->message_lines[line]);
        if(id>=0)
        {
            talk_id(id, enqueue);
            enqueue = true;
        }
    }
}

static int put_message(struct screen *display,
                        const struct text_message * message,
                        int start, int max_y)
{
    int i;
    int ct = MIN(message->nb_lines, max_y - start);
    for(i=0; i < ct; i++)
    {
        display->puts_scroll(0, i+start,
                             P2STR((unsigned char *)message->message_lines[i]));
    }
    return i;
}

/* Seconds left on the timeout, or -1 when there is no timeout default. */
static int yesno_tmo_remaining(struct yesno_ctx *c)
{
    if (c->tmo_default != YESNO_YES && c->tmo_default != YESNO_NO)
        return -1;
    int r = (c->end_tick - current_tick) / HZ;
    return r < 0 ? 0 : r;
}

/* Build the two button labels; the timeout-default button gets " (secs)"
 * appended (secs < 0 appends nothing). */
static void yesno_button_labels(struct yesno_ctx *c, int secs,
                                char *yes, char *no, size_t sz)
{
    if (secs >= 0 && c->tmo_default == YESNO_YES)
        snprintf(yes, sz, "%s (%d)", str(LANG_SET_BOOL_YES), secs);
    else
        snprintf(yes, sz, "%s", str(LANG_SET_BOOL_YES));

    if (secs >= 0 && c->tmo_default == YESNO_NO)
        snprintf(no, sz, "%s (%d)", str(LANG_SET_BOOL_NO), secs);
    else
        snprintf(no, sz, "%s", str(LANG_SET_BOOL_NO));
}

/* Widest button width for this dialog: sized to the initial countdown so the
 * button (and box) don't jitter as the seconds tick down. */
static int yesno_button_width(struct yesno_ctx *c, struct screen *display)
{
    char yes[24], no[24];
    int w_yes, w_no;
    yesno_button_labels(c, c->tmo_secs, yes, no, sizeof yes);
    display->getstringsize(yes, &w_yes, NULL);
    display->getstringsize(no,  &w_no,  NULL);
    return MAX(w_yes, w_no) + YN_BTN_PAD_X * 2;
}

/* Draw the Yes/No buttons side by side along the bottom of the content area,
 * with the currently selected one highlighted. Coordinates are relative to the
 * content viewport. */
static void yesno_draw_buttons(struct yesno_ctx *c, struct dialog *d,
                               struct screen *display,
                               struct viewport *content)
{
    char yes[24], no[24];
    int h;

    yesno_button_labels(c, yesno_tmo_remaining(c), yes, no, sizeof yes);
    display->getstringsize(yes, NULL, &h);

    const int gap = YN_BTN_GAP;
    int bw = yesno_button_width(c, display);   /* stable width */
    int bh = h + YN_BTN_PAD_Y * 2;
    int x  = (content->width - (bw * 2 + gap)) / 2;
    if (x < 0)
        x = 0;
    int y = content->height - bh;
    if (y < 0)
        y = 0;

    dialog_draw_button(display, &d->style, x, y, bw, bh, yes,
                       c->selection == YESNO_YES);
    dialog_draw_button(display, &d->style, x + bw + gap, y, bw, bh, no,
                       c->selection == YESNO_NO);
}

/* Draw a yes/no result message (shown briefly after a choice), into the
 * theme viewport `vp`. */
static void gui_yesno_draw_result(struct screen *display, struct viewport *vp,
                                  const struct text_message *message)
{
    struct viewport *last_vp = display->set_viewport_ex(vp, VP_FLAG_VP_SET_CLEAN);

    display->clear_viewport();
    put_message(display, message, 0, viewport_get_nb_lines(vp));
    display->update_viewport();
    display->set_viewport(last_vp);
}

/* --- dialog callbacks --- */

/* Word-wrap the message lines to the box content width into c->wrap. */
static void yesno_wrap_message(struct yesno_ctx *c, int content_w, int font)
{
    c->wrap_count = 0;
    for (int i = 0; i < c->message->nb_lines &&
                    c->wrap_count < YN_MAX_LINES; i++)
    {
        const char *line = P2STR((unsigned char *)c->message->message_lines[i]);
        int n = dialog_wrap_text(line, content_w, font,
                                 &c->wrap[c->wrap_count],
                                 YN_MAX_LINES - c->wrap_count);
        if (n == 0)                 /* preserve a blank message line */
        {
            c->wrap[c->wrap_count].str = "";
            c->wrap[c->wrap_count].len = 0;
            n = 1;
        }
        c->wrap_count += n;
    }
}

static void yesno_measure(struct dialog *d, struct screen *s,
                          struct viewport *box, void *data)
{
    struct yesno_ctx *c = data;
    struct dialog_insets in;
    /* Size and centre against the physical display, not the theme content
     * viewport the box arrives as. box->x/y below are absolute screen
     * coordinates, so measuring against s->getwidth()/getheight() (the theme
     * viewport, which a themed SBS can inset - e.g. %Vi(-,3,28,302,-,-)) would
     * centre the box on the viewport's size but position it from absolute 0,
     * leaving it visibly off-centre on the display. */
    int area_y = 0;
    int area_h = s->lcdheight;
    int sw = s->lcdwidth;
    int ch = s->getcharheight();

    dialog_get_insets(&d->style, &in);

    /* wrap to the widest the content may grow to, then size the box to the
     * actual content (widest wrapped line or the button row) and centre it. */
    int max_content_w = sw - 2 * YN_MARGIN - in.left - in.right;
    yesno_wrap_message(c, max_content_w, box->font);

    int text_w = 0;
    for (int i = 0; i < c->wrap_count; i++)
    {
        int w = font_getstringnsize((const unsigned char *)c->wrap[i].str,
                                    c->wrap[i].len, NULL, NULL, box->font);
        if (w > text_w)
            text_w = w;
    }

    int btn_row_w = yesno_button_width(c, s) * 2 + YN_BTN_GAP;

    int box_w = MAX(text_w, btn_row_w) + in.left + in.right;
    if (box_w > sw - 2 * YN_MARGIN)
        box_w = sw - 2 * YN_MARGIN;         /* clamp to the display */

    int btn_h = ch + 2 * YN_BTN_PAD_Y;      /* button height (matches buttons) */

    /* Gap between the message and the buttons. The last text line reserves
     * descender space under its baseline that is usually empty, so subtract
     * that from the margin -- otherwise the visible gap here is the margin
     * plus a descender, noticeably wider than the margins above and below. */
    int gap = in.top - (ch - font_get(box->font)->ascent);
    if (gap < 0)
        gap = 0;
    int nlines = c->wrap_count > 0 ? c->wrap_count : 1;
    int content_h = nlines * ch + gap + btn_h;
    int box_h = content_h + in.top + in.bottom;
    if (box_h > area_h)
        box_h = area_h;                     /* clamp to the display */

    box->x = (sw - box_w) / 2;
    box->width = box_w;
    box->y = area_y + (area_h - box_h) / 2;
    box->height = box_h;
}

static void yesno_draw(struct dialog *d, struct screen *s,
                       struct viewport *content, void *data)
{
    struct yesno_ctx *c = data;
    int ch = s->getcharheight();

    /* message: centred, word-wrapped, in the area above the buttons */
    struct viewport txt = *content;
    txt.height = (c->wrap_count > 0 ? c->wrap_count : 1) * ch;
    txt.flags |= VP_FLAG_ALIGN_CENTER;
    s->set_viewport(&txt);
    /* DRMODE_FG so glyphs draw over our solid fill without the theme backdrop
     * bleeding into each character's background */
    s->set_drawmode(DRMODE_FG);
    for (int i = 0; i < c->wrap_count; i++)
        s->putsxyf(0, i * ch, "%.*s", c->wrap[i].len, c->wrap[i].str);

    s->set_viewport(content);
    yesno_draw_buttons(c, d, s, content);
}

static int yesno_on_action(struct dialog *d, int action, void *data)
{
    struct yesno_ctx *c = data;

    /* Repeat the question every 5secs (more or less) */
    if (c->talk_menu && TIME_AFTER(current_tick, c->talked_tick))
    {
        c->talked_tick = current_tick + (HZ * 5);
        talk_text_message(c->message, false);
    }

    switch (action)
    {
        case ACTION_YESNO_ACCEPT:
            if (!d->backlight_on)
                break;              /* don't accept while the screen is off */
            c->result = c->selection; /* confirm the highlighted button */
            return (c->selection == YESNO_YES) ? DIALOG_ACCEPT : DIALOG_CANCEL;
        case ACTION_STD_PREV: /* scroll back / left -> Yes (left button) */
            c->selection = YESNO_YES;
            break;
        case ACTION_STD_NEXT: /* scroll fwd / right -> No (right button) */
            c->selection = YESNO_NO;
            break;
        case ACTION_STD_CANCEL:
        case ACTION_STD_MENU:
            if (!d->backlight_on)
                break;
            c->result = YESNO_NO; /* back / cancel */
            return DIALOG_CANCEL;
        case ACTION_NONE:
            if (c->tmo_default != YESNO_TMO &&
                TIME_AFTER(current_tick, c->end_tick))
            {
                splash(HZ/2, ID2P(LANG_TIMEOUT));
                c->result = c->tmo_default;
                return DIALOG_ACCEPT;
            }
            break;
        case ACTION_UNKNOWN:
        case ACTION_REDRAW: /* handled by the per-pass repaint */
            break;
        default:
            if (default_event_handler(action) == SYS_USB_CONNECTED)
            {
                c->result = YESNO_USB;
                return DIALOG_ABORT;
            }
            /* ignore unmapped buttons; the choice is explicit via the
             * highlighted Yes/No buttons */
            break;
    }
    return DIALOG_CONTINUE;
}

static void yesno_on_close(struct dialog *d, void *data)
{
    struct yesno_ctx *c = data;
    const struct text_message *resmsg = NULL;

    if (c->result == YESNO_YES)
        resmsg = c->yes_message;
    else if (c->result == YESNO_NO)
        resmsg = c->no_message;

    if (resmsg == NULL)
        return;

    FOR_NB_SCREENS(i)
        gui_yesno_draw_result(&screens[i], &d->parent[i], resmsg);

    if (c->talk_menu)
    {
        talk_text_message(resmsg, false);
        talk_force_enqueue_next();
    }

    sleep(HZ);
}

/* Display a YES_NO prompt to the user
 *
 * ticks < HZ will be ignored and the prompt will be blocking
 * tmo_default_res is the answer that is returned when the timeout expires
 * a default result of YESNO_TMO will also make the prompt blocking
 * if tmo_default_res is YESNO_YES or YESNO_NO a seconds countdown will
 * be present next to the default option
 *
 *        ticks - timeout if (>=HZ) otherwise ignored
 *  default_res - result returned on timeout YESNO_TMO creates a blocking prompt
 * main_message - prompt to the user
 *  yes_message - displayed when YESNO_YES is choosen
 *   no_message - displayed when YESNO_NO is choosen
*/
enum yesno_res gui_syncyesno_run_w_tmo(int ticks, enum yesno_res tmo_default_res,
                                       const char * title,
                                       const struct text_message * main_message,
                                       const struct text_message * yes_message,
                                       const struct text_message * no_message)
{
    static const struct dialog_callbacks cb = {
        .measure   = yesno_measure,
        .draw      = yesno_draw,
        .on_action = yesno_on_action,
        .on_close  = yesno_on_close,
    };
    struct yesno_ctx c;
    struct dialog d;

    if (ticks < HZ) /* Display a prompt with NO timeout to the user */
        tmo_default_res = YESNO_TMO;

    c.message      = main_message;
    c.selection    = YESNO_YES;     /* default highlighted button */
    c.tmo_default  = tmo_default_res;
    c.tmo_secs     = ticks / HZ;
    c.end_tick     = current_tick + ticks;
    c.talked_tick  = current_tick - 1;
    c.talk_menu    = global_settings.talk_menu;
    c.yes_message  = yes_message;
    c.no_message   = no_message;
    c.result       = YESNO_NO;

    dialog_init(&d, CONTEXT_YESNOSCREEN, title, NULL, &cb, &c);
    dialog_run(&d, HZ / 2); /* poll for statusbar and the timeout countdown */
    return c.result;
}

enum yesno_res gui_syncyesno_run(const struct text_message * main_message,
                                 const struct text_message * yes_message,
                                 const struct text_message * no_message)
{
    return gui_syncyesno_run_w_tmo(TIMEOUT_BLOCK, YESNO_TMO, NULL,
                                   main_message, yes_message, no_message);
}

extern enum yesno_res gui_syncyesno_run_w_title(
                                 const char * title,
                                 const struct text_message * main_message,
                                 const struct text_message * yes_message,
                                 const struct text_message * no_message)
{
    return gui_syncyesno_run_w_tmo(TIMEOUT_BLOCK, YESNO_TMO, title,
                                   main_message, yes_message, no_message);
}

static bool yesno_pop_lines(const char *lines[], int line_cnt)
{
    const struct text_message message={lines, line_cnt};
    bool ret = (gui_syncyesno_run(&message,NULL,NULL)== YESNO_YES);
    FOR_NB_SCREENS(i)
        screens[i].clear_viewport();
    return ret;
}

/* YES/NO dialog, uses text parameter as prompt */
bool yesno_pop(const char* text)
{
    const char *lines[]= {text};
    return yesno_pop_lines(lines, 1);
}

/* YES/NO dialog, asks "Are you sure?", displays
   text parameter on second line.

   Says "Cancelled" if answered negatively.
*/
bool yesno_pop_confirm(const char* text)
{
    bool confirmed;
    const char *lines[] = {ID2P(LANG_ARE_YOU_SURE), text};
    confirmed = yesno_pop_lines(lines, 2);

    if (!confirmed)
        splash(HZ, ID2P(LANG_CANCEL));

    return confirmed;
}
