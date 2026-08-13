/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The generic half of the database search screen: a type-and-pick box over
 * whatever list a provider can scan. See search_dialog.h for what a provider
 * owns and what this does.
 *
 * Parts, in order:
 *   - geometry
 *   - the dialog state
 *   - measure, draw, actions
 *   - the entry point
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "string-extra.h"
#include "input/action.h"
#include "widgets/dialog.h"
#include "widgets/edit_line.h"
#include "draw/screen_access.h"
#include "system/activity.h"
#include "system/shutdown.h"      /* default_event_handler */
#include "search_dialog.h"

/* How long the query must stand still before the provider is asked to scan. */
#define SETTLE_TICKS (HZ)

/* How often the dialog loop wakes with ACTION_NONE. Fast enough for a smooth
 * marquee while one is running; slow enough to be idle otherwise, since every
 * wake repaints the whole box. */
#define POLL_ANIM    (HZ/20)
#define POLL_IDLE    (HZ/5)

/* The box, as a fraction of the display. Most of the screen, not all of it --
 * enough of the theme stays visible that this reads as something on top of the
 * UI rather than a screen of its own. */
#define BOX_W_PCT     92
/* Clearance from the top and bottom of the display. Stated as pixels rather
 * than a percentage because it is the thing being looked at -- the strip of
 * theme left showing around the box -- and a percentage turns whatever the row
 * arithmetic leaves over into extra gap on top of it. */
#define BOX_MARGIN_Y  10
#define BOX_PAD       6    /* inside the border, all round */
#define MIN_ROWS      3    /* however large the font, show at least this many */

#define ROW_PAD_Y     3
#define ROW_GAP       2    /* so adjacent rows read as separate buttons */

#define SCROLL_STEP  3          /* pixels per poll */
#define SCROLL_PAUSE (HZ / POLL_ANIM)   /* about a second at each end */

/* ---- geometry ----------------------------------------------------------- */

/* Air above and below the rule, as a fraction of the font. */
#define RULE_AIR(ch_h) ((ch_h) / 4)

/* Where the rule sits, and where the rows start just under it. Both derived
 * here so measure() and draw() cannot disagree: they did, and the head then
 * reserved a whole character of space around a rule drawn a quarter of one
 * below the query -- a dead band under the rule, and a row's worth of height
 * spent on nothing. */
static int rule_offset(struct screen *display)
{
    return edit_line_height(display) + RULE_AIR(display->getcharheight());
}

static int head_height(struct screen *display)
{
    return rule_offset(display) + 1 + RULE_AIR(display->getcharheight());
}

static int row_height(struct screen *display)
{
    return display->getcharheight() + 2 * ROW_PAD_Y;
}

/* ---- the dialog state --------------------------------------------------- */

struct search_dlg {
    const struct search_provider *p;
    void  *ctx;

    struct edit_line ed;
    char  *query;               /* the caller's buffer, written back on exit */
    size_t query_len;

    int    count;               /* what the last scan() returned */
    long   last_edit_tick;
    bool   pending;             /* the query has changed and not been scanned */
    int    selected;            /* row index, or -1 while the focus is the query */
    int    top_row;             /* first visible row */
    int    visible_rows;        /* set by draw(), read by the action handler */

    /* Marquee for the selected row, driven from the poll tick rather than the
     * shared scroll engine -- see dialog_draw_button_ex(). */
    int    scroll_px;
    int    scroll_max;          /* what the last draw said overflows */
    int    scroll_dir;          /* +1 running left, -1 coming back */
    int    scroll_wait;         /* polls to hold still at each end */
    int    scroll_row;          /* the row the marquee belongs to */
    int    chosen;              /* the row SELECT accepted, or -1 */
};

/* ---- measure, draw, actions --------------------------------------------- */

/* Size the box to a whole number of rows rather than a percentage, so the
 * bottom of the box is the bottom of a row at every font size instead of
 * clipping one arbitrarily. */
static void search_measure(struct dialog *d, struct screen *display,
                           struct viewport *box, void *data)
{
    struct search_dlg *s = data;
    struct dialog_insets in;
    int row_h = row_height(display);
    int chrome, limit, rows, box_h;

    dialog_get_insets(&d->style, &in);
    chrome = in.top + in.bottom + head_height(display);

    /* One row short of what actually fits: taking every row the margins allow
     * left the box near enough full-screen that it stopped reading as a dialog
     * over the browser and started reading as a screen of its own. Only when
     * there is a row to spare, so at a large font the row goes to showing
     * results rather than to whitespace. */
    limit = display->lcdheight - 2 * BOX_MARGIN_Y;
    rows = (limit - chrome + ROW_GAP) / (row_h + ROW_GAP);
    if (rows > MIN_ROWS)
        rows--;
    if (rows < MIN_ROWS)
        rows = MIN_ROWS;

    box_h = chrome + rows * (row_h + ROW_GAP) - ROW_GAP;
    if (box_h > display->lcdheight)
        box_h = display->lcdheight;

    /* draw() reads this rather than working it out from the height it was
     * given, so a rounding difference cannot leave a half row visible. */
    s->visible_rows = rows;

    box->width = display->lcdwidth * BOX_W_PCT / 100;
    box->height = box_h;
    box->x = (display->lcdwidth - box->width) / 2;
    box->y = (display->lcdheight - box_h) / 2;
}

static void search_draw(struct dialog *d, struct screen *display,
                        struct viewport *content, void *data)
{
    struct search_dlg *s = data;
    int row_h = row_height(display);
    int line_h = edit_line_height(display);

    /* --- the query, in a clipped sub-viewport so long text can scroll --- */
    struct viewport tvp = *content;
    tvp.height = line_h;
    display->set_viewport(&tvp);
    edit_line_draw(display, &s->ed, tvp.width);

    /* --- a rule between the query and what it found --- */
    display->set_viewport(content);
    display->set_drawmode(DRMODE_FG);
    display->hline(0, content->width - 1, rule_offset(display));
    display->set_drawmode(DRMODE_SOLID);

    /* --- the results, each row a button so the selection carries the accent
     * colour the rest of the dialog world uses --- */
    int rows_y = head_height(display);

    if (s->selected >= 0)
    {
        if (s->selected < s->top_row)
            s->top_row = s->selected;
        else if (s->selected >= s->top_row + s->visible_rows)
            s->top_row = s->selected - s->visible_rows + 1;
    }
    if (s->top_row > s->count - s->visible_rows)
        s->top_row = s->count - s->visible_rows;
    if (s->top_row < 0)
        s->top_row = 0;

    /* The marquee belongs to one row; moving the selection starts it over.
     * Done here rather than at every place the selection changes, so no path
     * can leave the new row mid-scroll. */
    if (s->scroll_row != s->selected)
    {
        s->scroll_row = s->selected;
        s->scroll_px = 0;
        s->scroll_max = 0;
        s->scroll_dir = 1;
        s->scroll_wait = SCROLL_PAUSE;
    }

    for (int r = 0; r < s->visible_rows; r++)
    {
        int item = s->top_row + r;
        bool sel;

        if (item >= s->count)
            break;

        sel = (item == s->selected);

        dialog_draw_button_ex(display, &d->style, 0,
                              rows_y + r * (row_h + ROW_GAP),
                              content->width, row_h,
                              s->p->row_text(item, s->ctx), sel,
                              DIALOG_BTN_LEFT,
                              s->p->row_icon ? s->p->row_icon(item, s->ctx)
                                             : NULL,
                              sel ? s->scroll_px : 0,
                              sel ? &s->scroll_max : NULL);
    }

    display->set_drawmode(DRMODE_SOLID);
}

static int search_on_action(struct dialog *d, int action, void *data)
{
    struct search_dlg *s = data;
    (void)d;

    /* The wheel means the query while the query has focus, and the results
     * once it does not -- the same handover the text-input dialog makes to its
     * button row.
     *
     * The wheel and the caret taps never cross between the two: inside the
     * query they are the only way to change, advance or delete a character, so
     * giving any of them a second meaning would cost an edit. Moving between
     * the two is PLAY (down) and MENU (up), the physical buttons either side
     * of the wheel, which do nothing else here. */
    if (edit_line_owns_action(action))
    {
        if (s->selected < 0)
        {
            if (edit_line_action(&s->ed, action))
            {
                edit_line_get(&s->ed, s->query, s->query_len, false);
                s->last_edit_tick = current_tick;
                s->pending = true;
            }
        }
        else switch (action)
        {
            case ACTION_KBD_DOWN:
                if (s->selected + 1 < s->count)
                    s->selected++;
                break;
            case ACTION_KBD_UP:
                if (s->selected > 0)
                    s->selected--;
                break;
        }
        return DIALOG_CONTINUE;
    }

    switch (action)
    {
        case ACTION_KBD_DONE:      /* PLAY (down): into the results */
            if (s->selected < 0 && s->count > 0)
                s->selected = 0;
            break;

        case ACTION_KBD_SELECT:
            if (s->selected < 0)
            {
                if (s->count > 0)
                    s->selected = 0;
                break;
            }
            s->chosen = s->selected;
            return DIALOG_ACCEPT;

        case ACTION_KBD_ABORT:     /* MENU (up): results -> query -> out */
            if (s->selected >= 0)
            {
                s->selected = -1;
                break;
            }
            return DIALOG_CANCEL;

        case ACTION_NONE:
            /* The settle timer. Nothing else drives the scan. */
            if (s->pending
                && current_tick - s->last_edit_tick >= SETTLE_TICKS)
            {
                s->count = s->p->scan(s->query, s->ctx);
                s->pending = false;
                s->top_row = 0;
                if (s->selected >= s->count)
                    s->selected = s->count - 1;
            }

            /* Only tick fast while something is actually moving. Every pass is
             * a full repaint of the box -- the status bar renders into the
             * framebuffer under it on each get_action, so there is no smaller
             * update to make -- and at rest that is pure waste. */
            d->poll_ticks = (s->scroll_max > 0) ? POLL_ANIM : POLL_IDLE;

            /* One step of the selected row's marquee: out to the end of the
             * text, back to the start, pausing at both. */
            if (s->scroll_max > 0)
            {
                if (s->scroll_wait > 0)
                    s->scroll_wait--;
                else
                {
                    s->scroll_px += s->scroll_dir * SCROLL_STEP;
                    if (s->scroll_px >= s->scroll_max)
                    {
                        s->scroll_px = s->scroll_max;
                        s->scroll_dir = -1;
                        s->scroll_wait = SCROLL_PAUSE;
                    }
                    else if (s->scroll_px <= 0)
                    {
                        s->scroll_px = 0;
                        s->scroll_dir = 1;
                        s->scroll_wait = SCROLL_PAUSE;
                    }
                }
            }
            break;

        default:
            if (default_event_handler(action) == SYS_USB_CONNECTED)
                return DIALOG_ABORT;
            break;
    }
    return DIALOG_CONTINUE;
}

/* ---- the entry point ---------------------------------------------------- */

int search_dialog_run(const struct search_provider *p, void *ctx,
                      char *query, size_t query_len)
{
    static const struct dialog_callbacks cb = {
        .measure   = search_measure,
        .draw      = search_draw,
        .on_action = search_on_action,
    };
    static struct search_dlg s;
    struct dialog d;
    struct dialog_style style;

    s.p = p;
    s.ctx = ctx;
    s.query = query;
    s.query_len = query_len;

    /* Reopen on whatever the caller kept. The results come back on their own:
     * the query is marked pending, so the settle timer runs the scan a moment
     * after the box appears rather than making the caller wait for one. */
    edit_line_init(&s.ed, query, SEARCH_MAX_QUERY);
    s.pending = (query[0] != '\0');
    s.count = 0;
    s.last_edit_tick = current_tick;
    s.selected = -1;
    s.top_row = 0;
    s.visible_rows = 1;
    s.scroll_row = -1;
    s.scroll_px = 0;
    s.scroll_max = 0;
    s.scroll_dir = 1;
    s.scroll_wait = SCROLL_PAUSE;
    s.chosen = -1;

    /* Rows are buttons, but a list of forty outlined boxes reads as clutter.
     * An unselected row is therefore borderless and takes the box's own fill,
     * so it is just text and an icon on the dialog; only the selected one
     * becomes a shape, in the accent. A property of "a list of results in a
     * box" rather than of any one provider, so it is the default here. */
    style = *dialog_get_default_style();
    style.box_margin = BOX_PAD;     /* the default 10 costs a row of height */
    style.button_border_width = 0;
    style.button_bg = DIALOG_COLOR_INHERIT;
    style.button_fg = DIALOG_COLOR_INHERIT;
    style.button_bg_selected = DIALOG_COLOR_ACCENT;
    style.button_fg_selected = DIALOG_COLOR_ON_ACCENT;

    push_current_activity(p->activity);
    dialog_init(&d, CONTEXT_KEYBOARD, p->title, &style, &cb, &s);

    int res = dialog_run(&d, POLL_IDLE);

    pop_current_activity();

    if (res == DIALOG_ABORT)
        return SEARCH_USB;

    if (s.chosen >= 0 && s.chosen < s.count)
        return s.chosen;

    return SEARCH_CANCELLED;
}
