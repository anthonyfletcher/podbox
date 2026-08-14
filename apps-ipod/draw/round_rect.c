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
 *
 * The anti-aliased version at the bottom keeps the same span decomposition and
 * changes only the corners, which it draws through a coverage mask. The cost
 * of a smooth corner is therefore the corner alone -- the straight remainder
 * of the shape still goes out as plain fills.
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

/* ---------------------------------------------------------------------- *
 * Coverage masks                                                          *
 * ---------------------------------------------------------------------- */

/* The mask is a filled circle of diameter 2r, in the format the anti-aliased
 * font renderer reads: 4 bits per pixel, two pixels to a byte with the first
 * in the low nibble, and the sense inverted -- 0 is fully opaque, 15 fully
 * transparent. The diameter is even, so a row never shares a byte with the
 * next one and the stride is exactly r bytes.
 */

int round_rect_mask_size(int r)
{
    return 2 * r * r;                    /* (2r * 2r) pixels, two to a byte */
}

void round_rect_build_mask(unsigned char *mask, int r, int opacity)
{
    int d = 2 * r;

    /* Everything below is in half-pixel units, which puts pixel centres on
     * integers: dx and dy are twice the offset from the circle's centre, so
     * dx*dx + dy*dy is four times the squared distance. `edge` is where
     * coverage reaches zero, one half-pixel outside the radius. */
    int edge = 4 * r * r + 4 * r;

    for (int py = 0; py < d; py++)
    {
        for (int px = 0; px < d; px++)
        {
            int dx = 2 * px + 1 - d;
            int dy = 2 * py + 1 - d;

            /* Coverage without a square root: writing d2 for the squared
             * distance, the true distance sqrt(d2) stays within
             * r + (d2 - r*r) / 2r across the one-pixel band that needs
             * anti-aliasing at all, and closer than 4 bits can tell apart.
             * That leaves one multiply per pixel, and an expression that
             * saturates either side of the band, so the interior and the
             * exterior need no cases of their own. */
            int cov = edge - dx * dx - dy * dy;

            cov = cov <= 0 ? 0 : (2 * cov + r / 2) / r;
            if (cov > LCD_BLEND_OPAQUE)
                cov = LCD_BLEND_OPAQUE;

            /* Folding the opacity in here costs nothing and makes a tinted
             * corner meet its straight edge exactly: lcd_blendrect() reduces
             * to this same value where coverage is full. */
            cov = (cov * opacity + LCD_BLEND_OPAQUE / 2) / LCD_BLEND_OPAQUE;

            unsigned char *byte = &mask[(py * d + px) / 2];
            unsigned char alpha = LCD_BLEND_OPAQUE - cov;

            if (px & 1)
                *byte = (*byte & 0x0f) | (alpha << 4);
            else
                *byte = (*byte & 0xf0) | alpha;
        }
    }
}

void fill_round_rect_aa(struct screen *s, int x, int y, int w, int h,
                        int r, const unsigned char *mask, int opacity)
{
    bool tinted = opacity < LCD_BLEND_OPAQUE;

    /* A radius the mask was not cut for would take its quadrants from the
     * wrong place, so this falls back rather than drawing something wrong. */
    if (r <= 0 || !mask || 2 * r > w || 2 * r > h)
    {
        if (tinted)
            s->blendrect(x, y, w, h, opacity);
        else
            s->fillrect(x, y, w, h);
        return;
    }

    /* The four corners are the circle's four quadrants, in the order they
     * already sit in the mask. */
    int d = 2 * r;
    s->alpha_bitmap_part(mask, 0, 0, d, x,         y,         r, r);
    s->alpha_bitmap_part(mask, r, 0, d, x + w - r, y,         r, r);
    s->alpha_bitmap_part(mask, 0, r, d, x,         y + h - r, r, r);
    s->alpha_bitmap_part(mask, r, r, d, x + w - r, y + h - r, r, r);

    /* The rest is straight-edged. Any of these three can come out empty on a
     * shape that is exactly 2r across or tall -- a circle is the extreme
     * case -- and both fills reject an empty rectangle. */
    if (tinted)
    {
        s->blendrect(x + r, y,         w - 2 * r, r,         opacity);
        s->blendrect(x + r, y + h - r, w - 2 * r, r,         opacity);
        s->blendrect(x,     y + r,     w,         h - 2 * r, opacity);
    }
    else
    {
        s->fillrect(x + r, y,         w - 2 * r, r);
        s->fillrect(x + r, y + h - r, w - 2 * r, r);
        s->fillrect(x,     y + r,     w,         h - 2 * r);
    }
}

void bitmap_part_round(struct screen *s, const fb_data *src, int stride,
                       int x, int y, int w, int h,
                       int r, const unsigned char *mask)
{
    if (r <= 0 || !mask || 2 * r > w || 2 * r > h)
    {
        s->bitmap_part(src, 0, 0, stride, x, y, w, h);
        return;
    }

    int d = 2 * r;

    /* One (src_x, src_y) addresses both planes, and the mask's quadrants are
     * not where the image's corners are. The coordinates therefore belong to
     * the mask, and each image pointer is wound back by exactly as much as
     * they will advance it -- never past the start of the image, since d is
     * no larger than either side. */
    s->alpha_bitmap_part_img(src,
                             mask, 0, 0, x,         y,         r, r, stride, d);
    s->alpha_bitmap_part_img(src + w - d,
                             mask, r, 0, x + w - r, y,         r, r, stride, d);
    s->alpha_bitmap_part_img(src + (h - d) * stride,
                             mask, 0, r, x,         y + h - r, r, r, stride, d);
    s->alpha_bitmap_part_img(src + (h - d) * stride + w - d,
                             mask, r, r, x + w - r, y + h - r, r, r, stride, d);

    /* Everything the corners did not cover, straight from the image. */
    s->bitmap_part(src, r, 0,     stride, x + r, y,         w - d, r);
    s->bitmap_part(src, r, h - r, stride, x + r, y + h - r, w - d, r);
    s->bitmap_part(src, 0, r,     stride, x,     y + r,     w,     h - d);
}
