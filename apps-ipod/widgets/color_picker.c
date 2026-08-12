/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/color_picker.c
 * Copyright (C) Jonathan Gordon (2006)
 * GNU General Public License (version 2+)
 *
 * RGB colour chooser. A hex field and three channel sliders over one value,
 * both live at once, with a swatch and a Cancel/OK row. set_color() returns
 * true if USB interrupted.
 *
 * A dialog rather than a full-screen takeover. The screen it replaced cleared
 * the whole display and then took its text colour straight from the theme with
 * no contrast guard, and on one `cursor style` setting marked the selected
 * channel with an iconset glyph and nothing else -- so on a theme whose iconset
 * had no cursor there was no way to tell which channel the wheel was driving.
 * Everything the user must see is drawn here now.
 *
 * Parts, in order:
 *   - the colour: packing and unpacking
 *   - the grid: eleven cells in three rows
 *   - the dialog: measure, draw, actions
 ****************************************************************************/
#include "config.h"
#include "stdarg.h"
#include "string.h"
#include "stdio.h"
#include "kernel.h"
#include "system.h"
#include "draw/screen_access.h"
#include "font.h"
#include "debug.h"
#include "system/shutdown.h"
#include "settings/settings.h"
#include "draw/scrollbar.h"
#include "lang.h"
#include "splash.h"
#include "input/action.h"
#include "system/app_util.h"    /* clamp_value_wrap */
#include "dialog.h"
#include "color_picker.h"
#include "draw/viewport.h"

/* ---- the colour --------------------------------------------------------- */

/* structure for color info */
struct rgb_pick
{
    unsigned color;                 /* native color value            */
    union
    {
        unsigned char rgb_val[6];   /* access to components as array */
        struct
        {
            unsigned char r;        /* native red value              */
            unsigned char g;        /* native green value            */
            unsigned char b;        /* native blue value             */
            unsigned char red;      /* 8 bit red value               */
            unsigned char green;    /* 8 bit green value             */
            unsigned char blue;     /* 8 bit blue value              */
        } __attribute__ ((__packed__)); /* assume byte packing       */
    };
};


/* list of primary colors */
#define SB_PRIM 0
#define SB_FILL 1
static const unsigned prim_rgb[][3] =
{
    /* Foreground colors for sliders */
    {
        LCD_RGBPACK(255,   0,   0),
        LCD_RGBPACK(  0, 255,   0),
        LCD_RGBPACK(  0,   0, 255),
    },
    /* Fill colors for sliders */
    {
        LCD_RGBPACK( 85,   0,   0),
        LCD_RGBPACK(  0,  85,   0),
        LCD_RGBPACK(  0,   0,  85),
    },
};

/* maximum values for components */
static const unsigned char rgb_max[3] =
{
    LCD_MAX_RED,
    LCD_MAX_GREEN,
    LCD_MAX_BLUE
};

/* Unpacks the color value into native rgb values and 24 bit rgb values */
static void unpack_rgb(struct rgb_pick *rgb)
{
    unsigned color = rgb->color;
    rgb->red   = RGB_UNPACK_RED(color);
    rgb->green = RGB_UNPACK_GREEN(color);
    rgb->blue  = RGB_UNPACK_BLUE(color);
    rgb->r     = RGB_UNPACK_RED_LCD(color);
    rgb->g     = RGB_UNPACK_GREEN_LCD(color);
    rgb->b     = RGB_UNPACK_BLUE_LCD(color);
}

/* Packs the native rgb colors into a color value */
static inline void pack_rgb(struct rgb_pick *rgb)
{
    rgb->color = LCD_RGBPACK_LCD(rgb->r, rgb->g, rgb->b);
}

/* ---- the grid ----------------------------------------------------------- */

/* Eleven cells in three rows: six hex digits, three channels, two buttons.
 * Left and right move within a row, up and down between rows. */
#define CD_DIGITS   6
#define CD_CHAN     CD_DIGITS           /* first channel cell  */
#define CD_CANCEL   (CD_CHAN + 3)
#define CD_OK       (CD_CANCEL + 1)

struct colour_dlg {
    struct rgb_pick rgb;
    unsigned banned;        /* the one value OK refuses, or (unsigned)-1 */
    int  sel;
    int  swatch_h;          /* set by measure(), read by draw() */
    bool cancelled;
    bool usb;
};

static int cd_row_of(int sel)
{
    return sel < CD_CHAN ? 0 : (sel < CD_CANCEL ? 1 : 2);
}

static int cd_col_of(int sel)
{
    return sel < CD_CHAN ? sel
                         : (sel < CD_CANCEL ? sel - CD_CHAN : sel - CD_CANCEL);
}

static int cd_cells_in_row(int row)
{
    return row == 0 ? CD_DIGITS : (row == 1 ? 3 : 2);
}

static int cd_first_of_row(int row)
{
    return row == 0 ? 0 : (row == 1 ? CD_CHAN : CD_CANCEL);
}

/* Selection at (row, col), clamping the column to a shorter row -- the hex row
 * is six wide and the button row two, so moving straight down from the last
 * digit has to land somewhere. */
static int cd_sel_at(int row, int col)
{
    int n = cd_cells_in_row(row);
    if (col >= n)
        col = n - 1;
    return cd_first_of_row(row) + col;
}

static void cd_move_row(struct colour_dlg *s, int delta)
{
    int col = cd_col_of(s->sel);
    int row = clamp_value_wrap(cd_row_of(s->sel) + delta, 2, 0);

    s->sel = cd_sel_at(row, col);
}

static void cd_move_col(struct colour_dlg *s, int delta)
{
    int row = cd_row_of(s->sel);
    int col = clamp_value_wrap(cd_col_of(s->sel) + delta,
                               cd_cells_in_row(row) - 1, 0);

    s->sel = cd_sel_at(row, col);
}

/* Both editors write the packed value and then re-derive everything from it,
 * so what is on screen is always what the theme will be given.
 *
 * The display is RGB565, so eight bits per channel do not survive the trip:
 * type #123456 and it settles on #10505A. That has to be visible, which is why
 * the typed digits are never kept as a shadow copy -- the field snaps under the
 * caret the moment a value is not representable. */
static void cd_repack(struct colour_dlg *s, unsigned r8, unsigned g8,
                      unsigned b8)
{
    s->rgb.color = LCD_RGBPACK(r8, g8, b8);
    unpack_rgb(&s->rgb);
}

/* One hex digit, wrapping 0<->F. `sel` is 0..5, high nibble first. */
static void cd_step_digit(struct colour_dlg *s, int delta)
{
    unsigned v[3] = { s->rgb.red, s->rgb.green, s->rgb.blue };
    int ch = s->sel / 2;
    bool high = (s->sel % 2) == 0;
    unsigned nib = high ? (v[ch] >> 4) : (v[ch] & 0x0F);

    nib = (nib + delta) & 0x0F;
    v[ch] = high ? ((nib << 4) | (v[ch] & 0x0F))
                 : ((v[ch] & 0xF0) | nib);

    cd_repack(s, v[0], v[1], v[2]);
}

/* One channel step, clamped rather than wrapped: a slider that jumps from full
 * to empty on one more click of the wheel reads as a glitch. */
static void cd_step_channel(struct colour_dlg *s, int delta)
{
    int i = s->sel - CD_CHAN;

    if (delta > 0 && s->rgb.rgb_val[i] < rgb_max[i])
        s->rgb.rgb_val[i]++;
    else if (delta < 0 && s->rgb.rgb_val[i] > 0)
        s->rgb.rgb_val[i]--;

    pack_rgb(&s->rgb);
    unpack_rgb(&s->rgb);        /* re-derive the digits from the new value */
}

/* ---- the dialog --------------------------------------------------------- */

#define CD_MARGIN_Y    8   /* clearance from the top and bottom of the display */
#define CD_PAD         6   /* inside the box border, all round                 */
#define CD_PAD_Y       2   /* above and below a cell's text                    */
#define CD_CELL_PAD_X  3   /* either side of a hex digit                        */
#define CD_BTN_PAD_Y   3
#define CD_BTN_GAP     6
#define CD_SLIDER_GAP  4   /* between the channel label and its slider          */
#define CD_SWATCH_MIN 10   /* a band of colour still reads as one               */

static int cd_cell_h(struct screen *d)
{
    return d->getcharheight() + 2 * CD_PAD_Y;
}

static int cd_btn_h(struct screen *d)
{
    return d->getcharheight() + 2 * CD_BTN_PAD_Y;
}

static int cd_gap(struct screen *d)
{
    return d->getcharheight() / 2;
}

/* The widest hex digit, so every cell is the same width and the digits do not
 * shuffle sideways as they are edited. Fonts here are proportional. */
static int cd_digit_w(struct screen *d)
{
    static const char hex[] = "0123456789ABCDEF";
    int max = 0, i;

    for (i = 0; i < 16; i++)
    {
        char buf[2] = { hex[i], '\0' };
        int w = d->getstringsize((const unsigned char *)buf, NULL, NULL);
        if (w > max)
            max = w;
    }
    return max + 2 * CD_CELL_PAD_X;
}

/* The widest "R 63", so all three sliders start at the same x. */
static int cd_label_w(struct screen *d)
{
    const char *labels = str(LANG_COLOR_RGB_LABELS);
    int max = 0, i;

    for (i = 0; i < 3; i++)
    {
        char buf[8];
        int w;
        snprintf(buf, sizeof(buf), "%c %2d", labels[i], rgb_max[i]);
        w = d->getstringsize((const unsigned char *)buf, NULL, NULL);
        if (w > max)
            max = w;
    }
    return max;
}

/* Draws text in a cell, inverted when selected. DRMODE_FG over a fill rather
 * than DRMODE_INVERSEVID, which would source the glyph background from the
 * theme backdrop and punch through the box. Same treatment the time/date
 * dialog gives its fields, and code-drawn either way -- the screen this
 * replaced marked its selection with an iconset glyph on one cursor setting,
 * which a theme is free not to supply. */
static void cd_draw_cell(struct screen *d, int x, int y, int w, int h,
                         const char *text, bool selected)
{
    int tw, th;

    d->getstringsize((const unsigned char *)text, &tw, &th);

    if (selected)
    {
        unsigned fg = d->get_foreground();
        d->set_drawmode(DRMODE_FG);
        d->fillrect(x, y, w, h);
        d->set_foreground(d->get_background());
        d->putsxy(x + (w - tw) / 2, y + (h - th) / 2,
                  (const unsigned char *)text);
        d->set_foreground(fg);
    }
    else
    {
        d->set_drawmode(DRMODE_FG);
        d->putsxy(x + (w - tw) / 2, y + (h - th) / 2,
                  (const unsigned char *)text);
    }
    d->set_drawmode(DRMODE_SOLID);
}

/* "# 1A 2F 0C", centred, each digit its own cell. Six cells rather than a text
 * box on purpose: every cell holds a hex digit by construction, so there is no
 * invalid input to reject, no parse step and no error state to draw. */
static void cd_draw_hex_row(struct screen *d, struct colour_dlg *s,
                            struct viewport *content, int y, int h)
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned v[3] = { s->rgb.red, s->rgb.green, s->rgb.blue };
    int dw = cd_digit_w(d);
    int pair_gap = d->getstringsize((const unsigned char *)" ", NULL, NULL);
    int hash_w;
    int total, x, i;

    d->getstringsize((const unsigned char *)"#", &hash_w, NULL);
    total = hash_w + pair_gap + CD_DIGITS * dw + 2 * pair_gap;
    x = (content->width - total) / 2;
    if (x < 0)
        x = 0;

    d->set_drawmode(DRMODE_FG);
    d->putsxy(x, y + (h - d->getcharheight()) / 2, (const unsigned char *)"#");
    d->set_drawmode(DRMODE_SOLID);
    x += hash_w + pair_gap;

    for (i = 0; i < CD_DIGITS; i++)
    {
        unsigned nib = (i % 2) == 0 ? (v[i / 2] >> 4) : (v[i / 2] & 0x0F);
        char buf[2] = { hex[nib], '\0' };

        cd_draw_cell(d, x, y, dw, h, buf, s->sel == i);
        x += dw;
        if ((i % 2) == 1)
            x += pair_gap;
    }
}

/* One channel: label, value and a slider filling the rest of the row. The
 * slider keeps its primary colour so the three rows stay distinguishable
 * whatever the theme is doing. */
static void cd_draw_channel(struct screen *d, struct colour_dlg *s,
                            struct viewport *content, int i, int y, int h)
{
    const char *labels = str(LANG_COLOR_RGB_LABELS);
    bool selected = (s->sel == CD_CHAN + i);
    int label_w = cd_label_w(d);
    int slider_x = label_w + CD_SLIDER_GAP;
    int slider_w = content->width - slider_x;
    unsigned sb_flags = HORIZONTAL;
    unsigned fg = d->get_foreground();
    unsigned bg = d->get_background();
    char buf[8];
    int ch = d->getcharheight();

    snprintf(buf, sizeof(buf), "%c %2d", labels[i], s->rgb.rgb_val[i]);

    if (selected)
    {
        d->set_drawmode(DRMODE_FG);
        d->fillrect(0, y, content->width, h);
        d->set_foreground(bg);
        d->putsxy(0, y + (h - ch) / 2, (const unsigned char *)buf);
        d->set_foreground(fg);
        sb_flags |= FOREGROUND | INNER_BGFILL;
    }
    else
    {
        d->set_drawmode(DRMODE_FG);
        d->putsxy(0, y + (h - ch) / 2, (const unsigned char *)buf);
    }
    d->set_drawmode(DRMODE_SOLID);

    if (slider_w > 0)
    {
        if (selected)
            d->set_drawinfo(DRMODE_FG, prim_rgb[SB_PRIM][i],
                            prim_rgb[SB_FILL][i]);
        else
            d->set_drawinfo(DRMODE_SOLID, fg, bg);

        gui_scrollbar_draw(d, slider_x, y + (h - ch / 2) / 2,
                           slider_w, ch / 2,
                           rgb_max[i], 0, s->rgb.rgb_val[i], sb_flags);

        d->set_drawinfo(DRMODE_SOLID, fg, bg);
    }
}

/* A band of the colour, bordered in the box's foreground. No code inside it:
 * the hex row above is already the value, in the form you can edit. */
static void cd_draw_swatch(struct screen *d, struct colour_dlg *s,
                           struct viewport *content, int y, int h)
{
    unsigned fg = d->get_foreground();

    d->set_foreground(s->rgb.color);
    d->set_drawmode(DRMODE_FG);
    d->fillrect(0, y, content->width, h);

    d->set_foreground(fg);
    d->set_drawmode(DRMODE_SOLID);
    d->drawrect(0, y, content->width, h);
}

static void cd_measure(struct dialog *d, struct screen *display,
                       struct viewport *box, void *data)
{
    struct colour_dlg *s = data;
    struct dialog_insets in;
    int ch = display->getcharheight();
    int gap = cd_gap(display);
    int fixed, avail, box_h;

    dialog_get_insets(&d->style, &in);

    /* No title row: it costs a whole line, and at the shipped font that line is
     * the swatch. The menu entry just pressed already names the colour. */
    fixed = in.top + in.bottom
          + cd_cell_h(display)          /* hex row              */
          + 3 * cd_cell_h(display)      /* channels             */
          + gap + cd_btn_h(display);    /* buttons              */

    /* One line tall, and no more: it is a readout, not the subject of the
     * screen -- the digits and the sliders are what you operate. Squeezed only
     * if a large font leaves less than that, and never below a band that still
     * reads as a colour. */
    avail = display->lcdheight - 2 * CD_MARGIN_Y - fixed - gap;
    s->swatch_h = ch;
    if (s->swatch_h > avail)
        s->swatch_h = avail;
    if (s->swatch_h < CD_SWATCH_MIN)
        s->swatch_h = CD_SWATCH_MIN;

    box_h = fixed + gap + s->swatch_h;
    if (box_h > display->lcdheight)
        box_h = display->lcdheight;

    box->x = CD_MARGIN_Y;
    box->width = display->lcdwidth - 2 * CD_MARGIN_Y;
    box->y = (display->lcdheight - box_h) / 2;
    box->height = box_h;
}

static void cd_draw(struct dialog *d, struct screen *display,
                    struct viewport *content, void *data)
{
    struct colour_dlg *s = data;
    int gap = cd_gap(display);
    int cellh = cd_cell_h(display);
    int btnh = cd_btn_h(display);
    int y = 0;
    int i, by, bw;

    cd_draw_hex_row(display, s, content, y, cellh);
    y += cellh;

    for (i = 0; i < 3; i++)
    {
        cd_draw_channel(display, s, content, i, y, cellh);
        y += cellh;
    }

    y += gap;
    cd_draw_swatch(display, s, content, y, s->swatch_h);

    by = content->height - btnh;
    bw = (content->width - CD_BTN_GAP) / 2;
    if (bw > 0)
    {
        dialog_draw_button(display, &d->style, 0, by, bw, btnh,
                           str(LANG_DIAG_CANCEL), s->sel == CD_CANCEL);
        dialog_draw_button(display, &d->style, bw + CD_BTN_GAP, by, bw, btnh,
                           str(LANG_DIAG_OK), s->sel == CD_OK);
    }
}

static int cd_on_action(struct dialog *d, int action, void *data)
{
    struct colour_dlg *s = data;
    (void)d;

    switch (action)
    {
        case ACTION_STD_PREV:
        case ACTION_STD_PREVREPEAT:
            cd_move_col(s, -1);
            break;

        case ACTION_STD_NEXT:
        case ACTION_STD_NEXTREPEAT:
            cd_move_col(s, +1);
            break;

        case ACTION_TIME_UP:
            cd_move_row(s, -1);
            break;

        case ACTION_TIME_DOWN:
            cd_move_row(s, +1);
            break;

        case ACTION_SETTINGS_INC:
        case ACTION_SETTINGS_INCREPEAT:
        case ACTION_SETTINGS_DEC:
        case ACTION_SETTINGS_DECREPEAT:
        {
            int delta = (action == ACTION_SETTINGS_INC ||
                         action == ACTION_SETTINGS_INCREPEAT) ? +1 : -1;
            if (s->sel < CD_CHAN)
                cd_step_digit(s, delta);
            else if (s->sel < CD_CANCEL)
                cd_step_channel(s, delta);
            break;                      /* on the buttons: nothing to step */
        }

        case ACTION_STD_OK:
            if (s->sel == CD_CANCEL)
            {
                s->cancelled = true;
                return DIALOG_CANCEL;
            }
            /* Selecting a field accepts too, so the buttons are a way to
             * confirm rather than the only one. */
            if (s->banned != (unsigned)-1 && s->banned == s->rgb.color)
            {
                splash(HZ * 2, ID2P(LANG_COLOR_UNACCEPTABLE));
                break;
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

/***********
 set_color
 returns true if USB was inserted, false otherwise
 color is a pointer to the colour (in native format) to modify
 set banned_color to -1 to allow all
 ***********/
bool set_color(const char *title, unsigned *color, unsigned banned_color)
{
    static const struct dialog_callbacks cb = {
        .measure   = cd_measure,
        .draw      = cd_draw,
        .on_action = cd_on_action,
    };
    struct colour_dlg s;
    struct dialog d;
    struct dialog_style style;

    s.rgb.color = *color;
    unpack_rgb(&s.rgb);
    s.banned = banned_color;
    s.sel = 0;
    s.swatch_h = 0;
    s.cancelled = false;
    s.usb = false;

    /* The box owns its own margin: the default 10 on every side plus a title,
     * four rows, a swatch and a button row does not fit at a large font. */
    style = *dialog_get_default_style();
    style.box_margin = CD_PAD;

    dialog_init(&d, CONTEXT_SETTINGS_COLOURCHOOSER, title, &style, &cb, &s);

    if (dialog_run(&d, TIMEOUT_BLOCK) == DIALOG_ACCEPT)
        *color = s.rgb.color;

    return s.usb;
}
