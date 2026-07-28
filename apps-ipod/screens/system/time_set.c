/***************************************************************************
 * Original code from RockBox
 * was: apps/screens.c (time-setting screen)
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * The interactive date and time picker, with voiced feedback. Shared by
 * the time menu and the alarm screen.
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include "config.h"
#include "lang.h"
#include "timefuncs.h"
#include "font.h"
#include "usb.h"
#include "input/action.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "widgets/dialog.h"
#include "system/app_util.h"
#include "system/shutdown.h"
#include "time_set.h"

/* little helper function for voice output */
static void say_time(int cursorpos, const struct tm *tm)
{
    int value = 0;
    int unit = 0;

    if (!global_settings.talk_menu)
        return;

    switch(cursorpos)
    {
    case 0:
        value = tm->tm_hour;
        unit = UNIT_HOUR;
        break;
    case 1:
        value = tm->tm_min;
        unit = UNIT_MIN;
        break;
    case 2:
        value = tm->tm_sec;
        unit = UNIT_SEC;
        break;
    case 3:
        value = tm->tm_year + 1900;
        break;
    case 5:
        value = tm->tm_mday;
        break;
    }

    if (cursorpos == 4) /* month */
        talk_id(LANG_MONTH_JANUARY + tm->tm_mon, false);
    else
        talk_value(value, unit, false);
}


#define SEPARATOR " : "

#define IDX_HOURS   0
#define IDX_MINUTES 1
#define IDX_SECONDS 2
#define IDX_YEAR    3
#define IDX_MONTH   4
#define IDX_DAY     5

#define TS_MARGIN    12   /* gap from the display edge to the box */
#define TS_BTN_GAP   10   /* gap between the Cancel and OK buttons */
#define TS_BTN_PAD_Y  5   /* vertical padding inside each button */

/* Selection runs through the fields, then Cancel, then OK. */
struct ts_edit
{
    struct tm *tm;
    int  last_field;      /* IDX_SECONDS for time-only, IDX_DAY with a date */
    int  sel;             /* 0..last_field, then +1 Cancel, +2 OK */
    bool cancelled;
    bool usb;
};

#define TS_CANCEL(s) ((s)->last_field + 1)
#define TS_OK(s)     ((s)->last_field + 2)

static const unsigned char daysinmonth[] =
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static int days_in_month(const struct tm *tm)
{
    int year = tm->tm_year + 1900;
    if (tm->tm_mon == 1 &&
        ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        return 29;
    return daysinmonth[tm->tm_mon];
}

/* Clamps the day to the (possibly changed) month and refreshes the weekday. */
static void ts_normalise(struct tm *tm)
{
    if (tm->tm_mday > days_in_month(tm))
        tm->tm_mday = days_in_month(tm);
    set_day_of_week(tm);
}

/* The editable value behind a field, and the range it wraps within. */
static int *field_limits(struct tm *tm, int field, unsigned *min, unsigned *max)
{
    *min = 0;
    *max = 59;
    switch (field)
    {
        case IDX_HOURS:   *max = 23;                  return &tm->tm_hour;
        case IDX_MINUTES:                             return &tm->tm_min;
        case IDX_SECONDS:                             return &tm->tm_sec;
        case IDX_YEAR:    *min = 1; *max = 200;       return &tm->tm_year;
        case IDX_MONTH:   *max = 11;                  return &tm->tm_mon;
        case IDX_DAY:     *min = 1;
                          *max = days_in_month(tm);   return &tm->tm_mday;
    }
    return NULL;
}

/* Renders field `i` into `buf`. */
static const unsigned char *field_text(const struct tm *tm, int field,
                                       unsigned char *buf, size_t bufsz)
{
    if (field == IDX_MONTH)
        return (const unsigned char *)str(LANG_MONTH_JANUARY + tm->tm_mon);

    switch (field)
    {
        case IDX_HOURS:   snprintf(buf, bufsz, "%02d", tm->tm_hour); break;
        case IDX_MINUTES: snprintf(buf, bufsz, "%02d", tm->tm_min);  break;
        case IDX_SECONDS: snprintf(buf, bufsz, "%02d", tm->tm_sec);  break;
        case IDX_YEAR:    snprintf(buf, bufsz, "%04d", tm->tm_year + 1900); break;
        case IDX_DAY:     snprintf(buf, bufsz, "%02d", tm->tm_mday); break;
        default:          buf[0] = '\0'; break;
    }
    return buf;
}

/* Draws one field, inverted when selected. DRMODE_FG over a fill rather than
 * DRMODE_INVERSEVID, which would source the glyph background from the theme
 * backdrop and punch through the box. */
static void draw_field(struct screen *d, int x, int y, int w, int h,
                       const unsigned char *text, bool selected)
{
    if (selected)
    {
        unsigned fg = d->get_foreground();
        d->set_drawmode(DRMODE_FG);
        d->fillrect(x, y, w, h);
        d->set_foreground(d->get_background());
        d->putsxy(x, y, text);
        d->set_foreground(fg);
    }
    else
    {
        d->set_drawmode(DRMODE_FG);
        d->putsxy(x, y, text);
    }
    d->set_drawmode(DRMODE_SOLID);
}

/* Draws `count` fields starting at `first` as one centred row, separated by
 * SEPARATOR, with `lead` (the weekday, or nothing) in front of them. */
static void draw_field_row(struct screen *d, struct ts_edit *s,
                           struct viewport *content, int y, int h,
                           const unsigned char *lead, int first, int count)
{
    unsigned char buf[16];
    int widths[3];
    int sep_w = d->getstringsize((const unsigned char *)SEPARATOR, NULL, NULL);
    int lead_w = lead ? d->getstringsize(lead, NULL, NULL) + sep_w : 0;
    int total = lead_w;
    int i, x;

    for (i = 0; i < count; i++)
    {
        field_text(s->tm, first + i, buf, sizeof(buf));
        widths[i] = d->getstringsize(buf, NULL, NULL);
        total += widths[i] + (i ? sep_w : 0);
    }

    x = (content->width - total) / 2;
    if (x < 0)
        x = 0;

    if (lead)
    {
        d->set_drawmode(DRMODE_FG);
        d->putsxy(x, y, lead);
        d->set_drawmode(DRMODE_SOLID);
        x += lead_w - sep_w;
    }

    for (i = 0; i < count; i++)
    {
        if (i || lead)
        {
            d->set_drawmode(DRMODE_FG);
            d->putsxy(x, y, (const unsigned char *)SEPARATOR);
            d->set_drawmode(DRMODE_SOLID);
            x += sep_w;
        }
        field_text(s->tm, first + i, buf, sizeof(buf));
        draw_field(d, x, y, widths[i], h, buf, s->sel == first + i);
        x += widths[i];
    }
}


static void ts_measure(struct dialog *d, struct screen *display,
                       struct viewport *box, void *data)
{
    struct ts_edit *s = data;
    struct dialog_insets in;
    int ch = display->getcharheight();
    int rows = (s->last_field == IDX_DAY) ? 2 : 1;
    int bh = ch + 2 * TS_BTN_PAD_Y;
    int gap = ch / 2;
    int box_h;

    dialog_get_insets(&d->style, &in);

    box_h = rows * ch + gap + bh + in.top + in.bottom;
    if (box_h > display->lcdheight)
        box_h = display->lcdheight;

    box->x = TS_MARGIN;
    box->width = display->lcdwidth - 2 * TS_MARGIN;
    box->y = (display->lcdheight - box_h) / 2;
    box->height = box_h;
}

static void ts_draw(struct dialog *d, struct screen *display,
                    struct viewport *content, void *data)
{
    struct ts_edit *s = data;
    int ch = display->getcharheight();
    int bh = ch + 2 * TS_BTN_PAD_Y;
    int y = 0;

    ts_normalise(s->tm);

    draw_field_row(display, s, content, y, ch, NULL, IDX_HOURS, 3);

    if (s->last_field == IDX_DAY)
    {
        y += ch;
        draw_field_row(display, s, content, y, ch,
                       (const unsigned char *)
                           str(LANG_WEEKDAY_SUNDAY + s->tm->tm_wday),
                       IDX_YEAR, 3);
    }

    int by = content->height - bh;
    int bw = (content->width - TS_BTN_GAP) / 2;
    if (bw > 0)
    {
        dialog_draw_button(display, &d->style, 0, by, bw, bh,
                           str(LANG_DIAG_CANCEL), s->sel == TS_CANCEL(s));
        dialog_draw_button(display, &d->style, bw + TS_BTN_GAP, by, bw, bh,
                           str(LANG_DIAG_OK), s->sel == TS_OK(s));
    }
}

/* say_time() indexes by field, and the button positions reuse those numbers
 * (in time-only mode Cancel/OK land on 3 and 4, the year and month slots), so
 * only voice an actual field. */
static void ts_say(struct ts_edit *s)
{
    if (s->sel <= s->last_field)
        say_time(s->sel, s->tm);
}

/* The selection is a grid: a row of time fields, optionally a row of date
 * fields, then the buttons. Fields run three to a row, buttons two. */
static int ts_rows(struct ts_edit *s)
{
    return (s->last_field == IDX_DAY) ? 3 : 2;
}

static int ts_row_of(struct ts_edit *s)
{
    return (s->sel <= s->last_field) ? s->sel / 3 : ts_rows(s) - 1;
}

static int ts_col_of(struct ts_edit *s)
{
    return (s->sel <= s->last_field) ? s->sel % 3 : s->sel - TS_CANCEL(s);
}

static int ts_cells_in_row(struct ts_edit *s, int row)
{
    return (row == ts_rows(s) - 1) ? 2 : 3;
}

/* Selection at (row, col), clamping the column to a shorter row. */
static int ts_sel_at(struct ts_edit *s, int row, int col)
{
    int n = ts_cells_in_row(s, row);
    if (col >= n)
        col = n - 1;
    return (row == ts_rows(s) - 1) ? TS_CANCEL(s) + col : row * 3 + col;
}

static void ts_move_row(struct ts_edit *s, int delta)
{
    int col = ts_col_of(s);
    int row = clamp_value_wrap(ts_row_of(s) + delta, ts_rows(s) - 1, 0);

    s->sel = ts_sel_at(s, row, col);
    ts_say(s);
}

static void ts_move_col(struct ts_edit *s, int delta)
{
    int row = ts_row_of(s);
    int col = clamp_value_wrap(ts_col_of(s) + delta,
                               ts_cells_in_row(s, row) - 1, 0);

    s->sel = ts_sel_at(s, row, col);
    ts_say(s);
}

static int ts_on_action(struct dialog *d, int action, void *data)
{
    (void)d;
    struct ts_edit *s = data;
    unsigned min, max;
    int *valptr;

    switch (action)
    {
        case ACTION_STD_PREV:
            ts_move_col(s, -1);
            break;

        case ACTION_STD_NEXT:
            ts_move_col(s, +1);
            break;

        case ACTION_TIME_UP:
            ts_move_row(s, -1);
            break;

        case ACTION_TIME_DOWN:
            ts_move_row(s, +1);
            break;

        case ACTION_SETTINGS_INC:
        case ACTION_SETTINGS_INCREPEAT:
        case ACTION_SETTINGS_DEC:
        case ACTION_SETTINGS_DECREPEAT:
            if (s->sel > s->last_field)
                break;                  /* on the buttons: nothing to step */
            valptr = field_limits(s->tm, s->sel, &min, &max);
            if (action == ACTION_SETTINGS_INC || action == ACTION_SETTINGS_INCREPEAT)
                *valptr = clamp_value_wrap(*valptr + 1, max, min);
            else
                *valptr = clamp_value_wrap(*valptr - 1, max, min);
            ts_normalise(s->tm);
            ts_say(s);
            break;

        case ACTION_STD_OK:
            /* Selecting a field accepts too, so the buttons are a way to
             * confirm rather than the only one. */
            if (s->sel == TS_CANCEL(s))
            {
                s->cancelled = true;
                return DIALOG_CANCEL;
            }
            return DIALOG_ACCEPT;

        case ACTION_STD_CANCEL:
            s->cancelled = true;
            return DIALOG_CANCEL;

        default:
            if (default_event_handler(action) == SYS_USB_CONNECTED)
            {
                s->usb = true;
                return DIALOG_ABORT;
            }
            break;
    }
    return DIALOG_CONTINUE;
}

/* Interactive time (and optionally date) picker. Returns true if USB
 * interrupted; sets tm->tm_year to -1 if the user cancelled. */
bool set_time_screen(const char* title, struct tm *tm, bool set_date)
{
    static const struct dialog_callbacks cb = {
        .measure   = ts_measure,
        .draw      = ts_draw,
        .on_action = ts_on_action,
    };
    struct ts_edit s;
    struct dialog d;

    s.tm = tm;
    s.last_field = set_date ? IDX_DAY : IDX_SECONDS;
    s.sel = 0;
    s.cancelled = false;
    s.usb = false;

    ts_say(&s);

    dialog_init(&d, CONTEXT_SETTINGS_TIME, title, NULL, &cb, &s);
    dialog_run(&d, TIMEOUT_BLOCK);

    if (s.cancelled)
        tm->tm_year = -1;

    return s.usb;
}
