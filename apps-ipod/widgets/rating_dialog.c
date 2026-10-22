/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A rating as five stars, in a dialog: the wheel moves half a star.
 *
 * Built on dialog_run(), so the theme, the skin-flush guard and the repaint
 * are the shared ones. The stars are drawn here rather than taken from a
 * font, so they look the same under every theme: a filled star in the box's
 * text colour, an empty one in that colour faded most of the way to the box.
 *
 * Parts, in order:
 *   - drawing a star: the polygon, and its antialiased fill
 *   - the dialog's measure, draw and action callbacks
 *   - rating_dialog(), which runs it
 ****************************************************************************/
#include "config.h"
#include <stdbool.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lang.h"
#include "font.h"
#include "input/action.h"
#include "speech/talk.h"
#include "settings/settings.h"
#include "system/shutdown.h"
#include "draw/color.h"
#include "draw/screen_access.h"
#include "dialog.h"
#include "rating_dialog.h"

#define STAR_R       14   /* outer radius, px */
#define STAR_GAP     8    /* between stars */
#define STAR_COUNT   5
#define RATING_MAX   10   /* two steps a star */
#define EMPTY_FADE   180  /* of 256: how far an empty star fades to the box */
#define TITLE_GAP    8    /* between the title and the stars */
#define BOX_MARGIN   12   /* from the display edge */

/* ------------------------------------------------------------------ *
 * drawing a star                                                     *
 * ------------------------------------------------------------------ */

/* The ten corners, outer and inner by turns from the top point, as cosines
 * and sines of -90 + 36k degrees in thousandths */
static const short corner_cos[10] =
    { 0, 588, 951, 951, 588, 0, -588, -951, -951, -588 };
static const short corner_sin[10] =
    { -1000, -809, -309, 309, 809, 1000, 809, 309, -309, -809 };

/* Inner corners at 0.382 of the outer radius, which is what keeps the edges
 * of each point in line */
#define INNER_PER_MILLE 382

/* How far below its centre a star reaches: its lower points, not its radius */
#define STAR_BELOW (STAR_R * 809 / 1000 + 1)
#define STAR_H     (STAR_R + STAR_BELOW)
#define STAR_W     (2 * STAR_R)
#define STARS_W    (STAR_COUNT * STAR_W + (STAR_COUNT - 1) * STAR_GAP)

/* Where the outline crosses the line at height y, in sixteenths of a pixel,
 * sorted. A star is concave, so a line can cross it four times. */
static int star_crossings(const int *vx, const int *vy, int y, int *xs)
{
    int n = 0;

    for (int i = 0; i < 10; i++)
    {
        int j = (i + 1) % 10;

        if ((vy[i] <= y) == (vy[j] <= y))
            continue;
        int x = vx[i] + (y - vy[i]) * (vx[j] - vx[i]) / (vy[j] - vy[i]);
        int k = n++;

        while (k > 0 && xs[k - 1] > x)
        {
            xs[k] = xs[k - 1];
            k--;
        }
        xs[k] = x;
    }
    return n;
}

/* A star centred on (cx, cy) in the active viewport, over a box of colour
 * 'bg'. Pixels left of 'split' take 'left', the rest 'right', so a half star
 * is one star in two colours. Each pixel is blended by how much of it the
 * star covers, measured on four lines through it. */
static void draw_star(struct screen *s, int cx, int cy, int split,
                      unsigned bg, unsigned left, unsigned right)
{
    int vx[10], vy[10];
    int cover[STAR_W + 2];
    int x0 = cx - STAR_R - 1;

    for (int i = 0; i < 10; i++)
    {
        int r = (i & 1) ? STAR_R * INNER_PER_MILLE / 1000 : STAR_R;

        vx[i] = cx * 16 + r * corner_cos[i] * 16 / 1000;
        vy[i] = cy * 16 + r * corner_sin[i] * 16 / 1000;
    }

    for (int y = cy - STAR_R; y <= cy + STAR_BELOW; y++)
    {
        int xs[10];

        memset(cover, 0, sizeof(cover));
        for (int sub = 0; sub < 4; sub++)
        {
            int n = star_crossings(vx, vy, y * 16 + 2 + sub * 4, xs);

            for (int k = 0; k + 1 < n; k += 2)
            {
                /* The span's overlap with each pixel it touches */
                for (int px = xs[k] >> 4; px <= xs[k + 1] >> 4; px++)
                {
                    int a = MAX(xs[k], px * 16);
                    int b = MIN(xs[k + 1], px * 16 + 16);

                    if (b > a && px - x0 >= 0 && px - x0 < (int)ARRAYLEN(cover))
                        cover[px - x0] += b - a;
                }
            }
        }

        for (int i = 0; i < (int)ARRAYLEN(cover); i++)
        {
            int px = x0 + i;

            if (cover[i] <= 0)
                continue;
            s->set_foreground(color_blend(bg, px < split ? left : right,
                                          MIN(cover[i] * 4, 256)));
            s->drawpixel(px, y);
        }
    }
}

/* ------------------------------------------------------------------ *
 * the dialog                                                         *
 * ------------------------------------------------------------------ */

struct rating_ctx
{
    const char *title;
    int rating;
    bool accepted;
};

static void rating_measure(struct dialog *d, struct screen *s,
                           struct viewport *box, void *data)
{
    struct rating_ctx *c = data;
    struct dialog_insets in;
    int sw = s->lcdwidth;
    int title_w = 0;
    int w, h;

    font_getstringsize((const unsigned char *)c->title, &title_w, NULL,
                       box->font);
    dialog_get_insets(&d->style, &in);
    w = MIN(MAX(title_w, STARS_W) + in.left + in.right, sw - 2 * BOX_MARGIN);
    h = s->getcharheight() + TITLE_GAP + STAR_H + in.top + in.bottom;

    /* Centred on the display, not the theme viewport the box arrives as: its
     * x and y are absolute -- see yesno_measure() */
    box->x = (sw - w) / 2;
    box->width = w;
    box->y = (s->lcdheight - h) / 2;
    box->height = h;
}

static void rating_draw(struct dialog *d, struct screen *s,
                        struct viewport *content, void *data)
{
    struct rating_ctx *c = data;
    unsigned fg = s->get_foreground();
    unsigned bg = s->get_background();
    unsigned empty = color_blend(fg, bg, EMPTY_FADE);
    int ch = s->getcharheight();
    int x = (content->width - STARS_W) / 2;
    struct viewport title = *content;

    (void)d;
    title.height = ch;
    title.flags |= VP_FLAG_ALIGN_CENTER;
    s->set_viewport(&title);
    s->set_drawmode(DRMODE_FG);
    s->putsxy(0, 0, c->title);

    s->set_viewport(content);
    for (int i = 0; i < STAR_COUNT; i++)
    {
        int cx = x + i * (STAR_W + STAR_GAP) + STAR_R;
        int halves = c->rating - 2 * i;
        int split = halves >= 2 ? cx + STAR_R + 1
                  : halves == 1 ? cx
                  : cx - STAR_R - 1;

        draw_star(s, cx, ch + TITLE_GAP + STAR_R, split, bg, fg, empty);
    }
    s->set_foreground(fg);
}

static void rating_set(struct rating_ctx *c, int rating)
{
    rating = MAX(0, MIN(rating, RATING_MAX));
    if (rating == c->rating)
        return;
    c->rating = rating;
    if (global_settings.talk_menu)
        talk_number(rating, false);
}

static int rating_on_action(struct dialog *d, int action, void *data)
{
    struct rating_ctx *c = data;

    switch (action)
    {
        case ACTION_STD_NEXT:
        case ACTION_STD_NEXTREPEAT:
            rating_set(c, c->rating + 1);
            break;
        case ACTION_STD_PREV:
        case ACTION_STD_PREVREPEAT:
            rating_set(c, c->rating - 1);
            break;
        case ACTION_STD_CONTEXT:
            rating_set(c, 0);
            break;
        case ACTION_STD_OK:
            if (!d->backlight_on)
                break;              /* not while the screen is off */
            c->accepted = true;
            return DIALOG_ACCEPT;
        case ACTION_STD_CANCEL:
        case ACTION_STD_MENU:
            if (!d->backlight_on)
                break;
            return DIALOG_CANCEL;
        case ACTION_NONE:
        case ACTION_UNKNOWN:
        case ACTION_REDRAW:
            break;
        default:
            if (default_event_handler(action) == SYS_USB_CONNECTED)
                return DIALOG_ABORT;
            break;
    }
    return DIALOG_CONTINUE;
}

bool rating_dialog(const char *title, int *rating)
{
    static const struct dialog_callbacks cb = {
        .measure   = rating_measure,
        .draw      = rating_draw,
        .on_action = rating_on_action,
    };
    struct rating_ctx c = {
        .title = title,
        .rating = MAX(0, MIN(*rating, RATING_MAX)),
        .accepted = false,
    };
    struct dialog d;

    /* The press that opened this is still down, and its repeats would clear
     * the rating */
    action_wait_for_release();
    if (global_settings.talk_menu)
        talk_number(c.rating, false);

    dialog_init(&d, CONTEXT_STD, title, NULL, &cb, &c);
    dialog_run(&d, HZ / 2);

    if (c.accepted)
        *rating = c.rating;
    return c.accepted;
}
