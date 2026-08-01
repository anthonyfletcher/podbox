/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Filled and outlined rectangles with rounded corners, drawn as a stack of
 * horizontal spans.
 ****************************************************************************/

/* Each corner row's inset comes from the circle equation -- one integer
 * square root per row, at most `r` rows per corner. The straight middle is
 * left to fillrect(), so only the rounded bands cost anything extra.
 *
 * Callers: dialog buttons (widgets/dialog.c), spectrum bars (%Sb,
 * skin/skin_render.c).
 */

#include "draw/round_rect.h"

/* ---------------------------------------------------------------------- *
 * Corner geometry                                                        *
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

/* ---------------------------------------------------------------------- *
 * Drawing                                                                *
 * ---------------------------------------------------------------------- */

void fill_round_rect(struct screen *s, int x, int y, int w, int h, int r)
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

void draw_round_rect(struct screen *s, int x, int y, int w, int h,
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
