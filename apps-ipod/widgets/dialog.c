/***************************************************************************
 * Modal box primitive shared by the popup, yes/no and text-input dialogs.
 * Draws a centred bordered box and returns its content viewport; owns the
 * dialog style defaults.
 ****************************************************************************/

/* Modal box primitive: a framed box (dialog_frame_box) plus the interactive
 * theme + skin-flush-guard + get_action loop (dialog_run) shared by the
 * yes/no and text-input dialogs. */

#include "config.h"
#include "lcd.h"
#include "font.h"
#include "kernel.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "input/action.h"
#include "backlight.h"
#include "draw/icon.h"          /* Icon_NOICON */
#include "draw/color.h"         /* color_blend */
#include "settings/settings.h"  /* theme fg/bg for the derived colours */
#include "skin/statusbar_skinned.h"
#include "skin/skin_engine.h" /* skin_inhibit_flush */
#include "skin/skin_albumart_color.h" /* dynamic_colors_resolve/_fading */
#include "dialog.h"

#define DIALOG_MARGIN 10   /* default content inset inside the box */

/* ---------------------------------------------------------------------- *
 * Style                                                                  *
 * ---------------------------------------------------------------------- */

void dialog_style_default(struct dialog_style *s)
{
    s->box_fg           = DIALOG_COLOR_INHERIT;
    s->box_bg           = DIALOG_COLOR_INHERIT;
    s->box_border_color = DIALOG_COLOR_INHERIT;
    s->box_border_width  = 1;
    s->box_margin        = DIALOG_MARGIN;
    s->box_font          = DIALOG_FONT_INHERIT;

    s->button_fg                     = DIALOG_COLOR_INHERIT;
    s->button_bg                     = DIALOG_COLOR_INHERIT;
    s->button_border_color           = DIALOG_COLOR_INHERIT;
    s->button_fg_selected            = DIALOG_COLOR_INHERIT;
    s->button_bg_selected            = DIALOG_COLOR_INHERIT;
    s->button_border_color_selected  = DIALOG_COLOR_INHERIT;
    s->button_border_width           = 1;
    s->button_border_radius          = 0;
    s->button_font                   = DIALOG_FONT_INHERIT;
}

static struct dialog_style default_style;
static bool default_style_valid;

void dialog_set_default_style(const struct dialog_style *s)
{
    if (s)
        default_style = *s;
    else
        dialog_style_default(&default_style);
    default_style_valid = true;
}

const struct dialog_style *dialog_get_default_style(void)
{
    if (!default_style_valid)
        dialog_set_default_style(NULL);
    return &default_style;
}

void dialog_get_insets(const struct dialog_style *style,
                       struct dialog_insets *out)
{
    out->left = out->top = out->right = out->bottom = style->box_margin;
}

/* ---------------------------------------------------------------------- *
 * Rounded-rectangle drawing -- buttons only                              *
 *                                                                        *
 * The box itself is always square and uses the plain rectangle pair       *
 * below. Only dialog_draw_button() rounds, by the theme's                 *
 * `dialog button border radius`.                                          *
 * ---------------------------------------------------------------------- */

static int isqrt_int(int n)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= n)
        r++;
    return r;
}

/* Horizontal inset of a corner of radius `r`, on the row `dy` above (or below)
 * the corner circle's centre. dy == r is the box's outermost row (inset r),
 * dy == 0 is level with the centre (inset 0). */
static int corner_inset(int r, int dy)
{
    return r - isqrt_int(r * r - dy * dy);
}

static int clamp_radius(int r, int w, int h)
{
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (r < 0)          /* also catches a degenerate (zero-size) box */
        r = 0;
    return r;
}

/* An empty span must not be drawn: lcd_hline() swaps reversed endpoints rather
 * than rejecting them, which would paint a stray line where a fully-rounded
 * corner leaves nothing to fill. */
static void span(struct screen *s, int x1, int x2, int y)
{
    if (x1 <= x2)
        s->hline(x1, x2, y);
}

/* Filled (rounded) rectangle, in the drawmode/foreground already set. */
static void fill_round_rect(struct screen *s, int x, int y, int w, int h, int r)
{
    r = clamp_radius(r, w, h);
    if (r == 0)
    {
        s->fillrect(x, y, w, h);
        return;
    }

    for (int i = 0; i < r; i++)          /* the two rounded end bands */
    {
        int dx = corner_inset(r, r - i);
        span(s, x + dx, x + w - 1 - dx, y + i);
        span(s, x + dx, x + w - 1 - dx, y + h - 1 - i);
    }
    s->fillrect(x, y + r, w, h - 2 * r); /* the full-width middle */
}

/* (Rounded) rectangle outline, `bw` pixels thick, in the drawmode/foreground
 * already set. The inner edge is the same box inset by bw, so its corner circle
 * shares the outer one's centre and has radius r - bw. */
static void draw_round_rect(struct screen *s, int x, int y, int w, int h,
                            int r, int bw)
{
    if (bw <= 0)
        return;

    r = clamp_radius(r, w, h);
    if (r == 0)
    {
        for (int i = 0; i < bw; i++)
            s->drawrect(x + i, y + i, w - 2 * i, h - 2 * i);
        return;
    }

    int ri = r - bw;                                   /* inner corner radius */

    s->fillrect(x + r, y, w - 2 * r, bw);              /* top edge    */
    s->fillrect(x + r, y + h - bw, w - 2 * r, bw);     /* bottom edge */
    s->fillrect(x, y + r, bw, h - 2 * r);              /* left edge   */
    s->fillrect(x + w - bw, y + r, bw, h - 2 * r);     /* right edge  */

    for (int i = 0; i < r; i++)
    {
        int dy  = r - i;
        int dxo = corner_inset(r, dy);                 /* outer edge  */
        int x0  = x + dxo;
        int x1  = x + w - 1 - dxo;

        if (ri > 0 && dy <= ri)
        {   /* the inner corner reaches this row: border spans both sides of it */
            int dxi = (r - ri) + corner_inset(ri, dy);
            span(s, x0, x + dxi - 1, y + i);
            span(s, x + w - dxi, x1, y + i);
            span(s, x0, x + dxi - 1, y + h - 1 - i);
            span(s, x + w - dxi, x1, y + h - 1 - i);
        }
        else
        {   /* beyond the inner corner: the whole row is border */
            span(s, x0, x1, y + i);
            span(s, x0, x1, y + h - 1 - i);
        }
    }
}

/* ---------------------------------------------------------------------- *
 * Frame primitive                                                        *
 * ---------------------------------------------------------------------- */

/* Square border, `bw` pixels thick, in the drawmode/foreground already set.
 * One fillrect per side rather than nested outlines, so every side is `bw`
 * by construction and there is nothing to get out of step. */
static void draw_box_border(struct screen *s, int w, int h, int bw)
{
    if (bw <= 0)
        return;
    if (bw * 2 > h)                     /* a border thicker than the box */
        bw = h / 2;

    s->fillrect(0, 0, w, bw);                        /* top    */
    s->fillrect(0, h - bw, w, bw);                   /* bottom */
    s->fillrect(0, bw, bw, h - 2 * bw);              /* left   */
    s->fillrect(w - bw, bw, bw, h - 2 * bw);         /* right  */
}

/* How far BG2 is mixed toward the foreground, out of 256.
 *
 * Every step raises the box off the screen behind it and lowers the contrast of
 * the text on it, and the two cross over. Measured against Themify_2's pair:
 * 32 gives 1.50:1 box-against-screen and 10.97:1 text; 48 gives 1.88:1 and
 * 8.71:1; 64 gives 2.40:1 but drops text to 6.84:1, under WCAG's 7:1. So 48 --
 * the box becomes a shape in its own right rather than an outline, and the
 * text keeps a AAA margin. */
#define DIALOG_BG2_MIX 48

/* The accent, as a turn around the colour wheel from the background.
 *
 * 100 degrees rather than a true complement (180): on a navy background 180
 * lands on amber, which is legible but reads as a different design. 100 lands
 * on a pink-mauve -- which is, to within a couple of values, the accent this
 * fork's theme already used by eye before any of it was computed.
 *
 * The saturation and brightness are fixed rather than taken from the
 * background, which is typically too dark and too flat to yield anything
 * usable. These give roughly 6.2:1 against the background it came from. */
#define DIALOG_ACCENT_ROTATE 100
#define DIALOG_ACCENT_SAT    107   /* 0.42 of full */

/* Brightnesses to try for the accent, preferred one first.
 *
 * A fixed brightness cannot work, because it is not tied to the pair it was
 * derived from: 204 against a dark background gives the pink above, but
 * against a *light* album it lands on a light green with the text at 1.4:1 --
 * the unreadable selection this list exists to avoid. The rest are fallbacks,
 * reached only when the preferred one is not legible, so a theme whose accent
 * already works never has it changed underneath it. */
static const unsigned char dialog_accent_vals[] =
    { 204, 240, 160, 128, 96, 64, 32 };

/* Contrast the accent and its text must reach before the search stops: WCAG's
 * 4.5:1 for body text, in hundredths. */
#define DIALOG_ACCENT_TARGET 450

/* The theme's live foreground and background, album-derived when dynamic
 * colours are running. dynamic_colors_resolve() matches by value against the
 * saved theme colours, which is exactly what these are. */
static unsigned dialog_theme_fg(void)
{
    return dynamic_colors_resolve(global_settings.fg_color);
}

static unsigned dialog_theme_bg(void)
{
    return dynamic_colors_resolve(global_settings.bg_color);
}

/* Pick the accent and the colour that goes on it together -- they are chosen
 * against each other, so neither can be resolved alone.
 *
 * Take the preferred brightness, ask which of the foreground and background
 * reads better on the result, and stop there if that pairing is legible. Only
 * when it is not does this go looking, which keeps a working accent from being
 * quietly retuned. */
static void resolve_accent(unsigned *accent_out, unsigned *text_out)
{
    unsigned fg = dialog_theme_fg();
    unsigned bg = dialog_theme_bg();
    unsigned best_accent = 0, best_text = 0;
    int best_contrast = -1;
    unsigned i;

    for (i = 0; i < sizeof(dialog_accent_vals); i++)
    {
        unsigned accent = color_hue_rotate(bg, DIALOG_ACCENT_ROTATE,
                                           DIALOG_ACCENT_SAT,
                                           dialog_accent_vals[i]);
        int on_bg = color_contrast(bg, accent);
        int on_fg = color_contrast(fg, accent);
        bool bg_wins = on_bg >= on_fg;
        int contrast = bg_wins ? on_bg : on_fg;

        if (contrast > best_contrast)
        {
            best_contrast = contrast;
            best_accent   = accent;
            best_text     = bg_wins ? bg : fg;
        }

        if (i == 0 && contrast >= DIALOG_ACCENT_TARGET)
            break;
    }

    *accent_out = best_accent;
    *text_out   = best_text;
}

/* The absolute sentinels. Returns false for anything else, including INHERIT,
 * which is screen-relative and resolved by the callers below. */
static bool resolve_derived(unsigned color, unsigned *out)
{
    unsigned accent, text;

    switch (color)
    {
        case DIALOG_COLOR_FG:
            *out = dialog_theme_fg();
            return true;
        case DIALOG_COLOR_BG:
            *out = dialog_theme_bg();
            return true;
        case DIALOG_COLOR_ACCENT:
            resolve_accent(&accent, &text);
            *out = accent;
            return true;
        case DIALOG_COLOR_ON_ACCENT:
            resolve_accent(&accent, &text);
            *out = text;
            return true;
        case DIALOG_COLOR_BG2:
            *out = color_blend(dialog_theme_bg(), dialog_theme_fg(),
                               DIALOG_BG2_MIX);
            return true;
    }
    return false;
}

static unsigned resolve_fg(struct screen *s, unsigned color)
{
    unsigned derived;

    if (resolve_derived(color, &derived))
        return derived;
    return color == DIALOG_COLOR_INHERIT ? s->get_foreground() : color;
}

static unsigned resolve_bg(struct screen *s, unsigned color)
{
    unsigned derived;

    if (resolve_derived(color, &derived))
        return derived;
    return color == DIALOG_COLOR_INHERIT ? s->get_background() : color;
}

void dialog_frame_box(struct screen *screen, struct viewport *box,
                      const struct dialog_style *style,
                      struct viewport *content_out)
{
    /* box must already be the active viewport: set_viewport() reloads fg/bg
     * from the viewport's stored patterns on colour targets, which would undo
     * a caller's colour override (e.g. the popup's broken-theme fallback, which
     * is what DIALOG_COLOR_INHERIT picks up here). */
    int bw = style->box_border_width;
    struct dialog_insets in;

    /* Fill an opaque box, then the border. On colour targets fill with a SOLID
     * background colour via DRMODE_FG -- not DRMODE_INVERSEVID, which draws the
     * "background" and, when the theme has a backdrop active, copies the
     * backdrop image into the box (leaving it looking unfilled with unreadable
     * text over it). */
    if (screen->depth > 1)
    {
        unsigned fg = resolve_fg(screen, style->box_fg);
        unsigned bg = resolve_bg(screen, style->box_bg);
        unsigned bc = resolve_fg(screen, style->box_border_color);

        screen->set_drawmode(DRMODE_FG);
        screen->set_foreground(bg);          /* fill with the background colour */
        screen->fillrect(0, 0, box->width, box->height);
        screen->set_foreground(bc);
        draw_box_border(screen, box->width, box->height, bw);
        screen->set_foreground(fg);          /* interior draws in the box fg */
        screen->set_drawmode(DRMODE_SOLID);

        /* so set_viewport(content) reloads the style's colours, not the theme's */
        box->fg_pattern = fg;
        box->bg_pattern = bg;
    }
    else
    {
        screen->set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
        screen->fillrect(0, 0, box->width, box->height);
        screen->set_drawmode(DRMODE_SOLID);
        draw_box_border(screen, box->width, box->height, bw);
    }

    /* content sub-viewport: the box inset by the margin. It inherits the box's
     * (styled) colours and font. */
    dialog_get_insets(style, &in);
    *content_out = *box;
    content_out->x      += in.left;
    content_out->y      += in.top;
    content_out->width  -= in.left + in.right;
    content_out->height -= in.top + in.bottom;
}

void dialog_draw_button(struct screen *screen,
                        const struct dialog_style *style,
                        int x, int y, int w, int h,
                        const char *label, bool selected)
{
    int r  = style->button_border_radius;
    int bw = style->button_border_width;
    int old_font = (*screen->current_viewport)->font;
    int lw, lh;

    if (style->button_font != DIALOG_FONT_INHERIT)
        screen->setfont(style->button_font);

    screen->getstringsize((const unsigned char *)label, &lw, &lh);
    int tx = x + (w - lw) / 2;
    int ty = y + (h - lh) / 2;

    if (screen->depth > 1)
    {
        /* Inheriting reproduces the old inverse-video selector exactly: the
         * selected button takes the box's foreground as its fill and its
         * background as its text, and the unselected one fills with the box's
         * own background, so it reads as the plain outline drawn before. */
        unsigned box_fg = screen->get_foreground();
        unsigned fill   = selected ? resolve_fg(screen, style->button_bg_selected)
                                   : resolve_bg(screen, style->button_bg);
        unsigned text   = selected ? resolve_bg(screen, style->button_fg_selected)
                                   : resolve_fg(screen, style->button_fg);
        unsigned border = selected
                        ? resolve_fg(screen, style->button_border_color_selected)
                        : resolve_fg(screen, style->button_border_color);

        screen->set_drawmode(DRMODE_FG);
        screen->set_foreground(fill);
        fill_round_rect(screen, x, y, w, h, r);
        screen->set_foreground(border);
        draw_round_rect(screen, x, y, w, h, r, bw);
        screen->set_foreground(text);
        screen->putsxy(tx, ty, (const unsigned char *)label);

        screen->set_foreground(box_fg);      /* restore for the rest of the box */
        screen->set_drawmode(DRMODE_SOLID);
    }
    else
    {
        if (selected)
        {
            screen->set_drawmode(DRMODE_FG);
            fill_round_rect(screen, x, y, w, h, r);
            screen->set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
            screen->putsxy(tx, ty, (const unsigned char *)label);
        }
        else
        {
            screen->set_drawmode(DRMODE_SOLID);
            draw_round_rect(screen, x, y, w, h, r, bw);
            screen->set_drawmode(DRMODE_FG);
            screen->putsxy(tx, ty, (const unsigned char *)label);
        }
        screen->set_drawmode(DRMODE_SOLID);
    }

    if (style->button_font != DIALOG_FONT_INHERIT)
        screen->setfont(old_font);
}

int dialog_wrap_text(const char *text, int max_width, int font,
                     struct dialog_text_line *out, int max_lines)
{
    int count = 0;
    const char *p = text;

    while (*p && count < max_lines)
    {
        while (*p == ' ')           /* skip leading spaces */
            p++;
        if (!*p)
            break;

        const char *line_start = p;
        const char *fit_end = NULL;  /* end of the widest run that still fits */
        const char *w = p;

        while (*w)
        {
            const char *word_end = w;
            while (*word_end && *word_end != ' ')
                word_end++;

            int width = font_getstringnsize((const unsigned char *)line_start,
                                            word_end - line_start,
                                            NULL, NULL, font);
            /* stop before a word that overflows, but always take the first */
            if (width > max_width && fit_end != NULL)
                break;

            fit_end = word_end;
            if (!*word_end)
                break;
            w = word_end + 1;        /* step past the space */
        }

        out[count].str = line_start;
        out[count].len = (int)(fit_end - line_start);
        count++;
        p = fit_end;
    }

    return count;
}

void dialog_init(struct dialog *d, int context, const char *title,
                 const struct dialog_style *style,
                 const struct dialog_callbacks *cb, void *data)
{
    d->cb = cb;
    d->data = data;
    d->context = context;
    d->title = title;
    d->style = style ? *style : *dialog_get_default_style();
    d->backlight_on = true;
}

/* Frame + draw the dialog on one screen. The theme parent viewport supplies the
 * font and (unless the style overrides them) the colours; the interior is the
 * callback's job. Only the box region is flushed, so the rest of the screen is
 * left untouched. */
static void dialog_draw_one(struct dialog *d, struct screen *s, int screen_nr)
{
    struct viewport parent = d->parent[screen_nr];  /* theme font/colours */
    struct viewport box, content;
    struct viewport *outer;

    if (d->style.box_font != DIALOG_FONT_INHERIT)
        parent.font = d->style.box_font;

    /* measure() runs with the parent active, so its text metrics see the font
     * the box will actually be drawn in */
    outer = s->set_viewport(&parent);

    box = parent;
    d->cb->measure(d, s, &box, d->data);            /* final geometry */
    s->set_viewport_ex(&box, VP_FLAG_VP_SET_CLEAN);
    dialog_frame_box(s, &box, &d->style, &content);

    s->set_viewport_ex(&content, VP_FLAG_VP_SET_CLEAN);
    d->cb->draw(d, s, &content, d->data);

    s->set_viewport(&box);
    s->update_viewport();
    s->set_viewport(outer);
}

int dialog_run(struct dialog *d, int poll_ticks)
{
    int disp = DIALOG_CONTINUE;

    FOR_NB_SCREENS(i)
    {
        screens[i].scroll_stop();
        sb_set_persistent_title(d->title, Icon_NOICON, i);
        viewportmanager_theme_enable(i, true, &d->parent[i]);
    }

    /* eat any stray keypresses from before the dialog opened */
    action_wait_for_release();

    while (disp == DIALOG_CONTINUE)
    {
        /* Repaint every pass. The status-bar skin renders into the framebuffer
         * on GUI_EVENT_ACTIONUPDATE (fired inside get_action) and dirties the
         * box region -- its flush is inhibited, but we still have to repaint
         * over it, the same way the list and album-covers screens do. */
        FOR_NB_SCREENS(i)
            dialog_draw_one(d, &screens[i], i);

        /* sampled before get_action so a callback can ignore the keypress
         * that merely woke the screen (matches the old yes/no behaviour) */
        d->backlight_on = is_backlight_on(false);

        /* A dialog left open across a track change would otherwise hold the
         * previous album's colours: the loop only comes round on input, and a
         * caller waiting on TIMEOUT_BLOCK never does. Poll while the fade runs
         * so the box travels with the rest of the UI, and go back to whatever
         * the caller asked for once it has arrived. */
        int ticks = poll_ticks;
        if (dynamic_colors_fading()
            && (ticks == TIMEOUT_BLOCK || ticks > HZ / 10))
            ticks = HZ / 10;

        skin_inhibit_flush(true);
        int action = get_action(d->context, ticks);
        skin_inhibit_flush(false);

        disp = d->cb->on_action(d, action, d->data);
    }

    if (d->cb->on_close)
        d->cb->on_close(d, d->data);

    FOR_NB_SCREENS(i)
    {
        screens[i].scroll_stop_viewport(&d->parent[i]);
        sb_set_persistent_title(d->title, Icon_NOICON, i);
        viewportmanager_theme_undo(i, false);
    }

    return disp;
}
