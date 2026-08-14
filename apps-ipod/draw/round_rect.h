/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to round_rect.c: filled and outlined rectangles with rounded
 * corners.
 ****************************************************************************/

#ifndef _ROUND_RECT_H_
#define _ROUND_RECT_H_

#include "draw/screen_access.h"

/* Both draw in the drawmode and foreground the caller has already set, the
 * same way s->fillrect() does. Radius 0 gives a plain rectangle; one too
 * large for the box is reduced to the largest that fits, so callers need not
 * range-check it. */

/* Filled rounded rectangle. */
void fill_round_rect(struct screen *s, int x, int y, int w, int h, int r);

/* Rounded rectangle outline, `bw` pixels thick. The inner edge is the box
 * inset by bw, so its corner circle shares the outer one's centre. */
void draw_round_rect(struct screen *s, int x, int y, int w, int h,
                     int r, int bw);

/* ---------------------------------------------------------------------- *
 * Anti-aliased corners                                                    *
 * ---------------------------------------------------------------------- */

/* The smooth version needs a coverage mask, which depends only on the radius
 * and the opacity. Build it once, where the shape is defined, and hand the
 * same one to every draw -- it is a table, not scratch space. */

/* Radii above this are refused rather than clamped: the mask grows as the
 * square of the radius, and a 64-pixel corner is already larger than anything
 * a 320x240 screen has room for. */
#define ROUND_RECT_MAX_RADIUS 32

/* Bytes round_rect_build_mask() writes, for the radius it will be given. */
int round_rect_mask_size(int r);

/* Fill `mask` with the coverage of a circle of radius r, at `opacity`
 * (0..LCD_BLEND_OPAQUE). The mask is one filled anti-aliased circle rather
 * than one corner, because a 2r-by-2r rounded square *is* a circle -- its
 * four quadrants are the four corners. */
void round_rect_build_mask(unsigned char *mask, int r, int opacity);

/* Filled rounded rectangle with smooth corners, at `opacity`. `mask` must be
 * the one built for this r and opacity, and r must be no more than half the
 * shorter side -- unlike the hard-edged calls above, this one cannot reduce a
 * radius that does not fit, because the mask was cut to the original.
 *
 * Blends with what is already in the viewport's buffer, so call it with
 * DRMODE_FG set. */
void fill_round_rect_aa(struct screen *s, int x, int y, int w, int h,
                        int r, const unsigned char *mask, int opacity);

/* An image drawn with its corners rounded off, using the same mask. `src` is
 * native pixels, `stride` its row length; `w` and `h` are what to draw, and r
 * must be no more than half the smaller of them.
 *
 * The corners blend with what is already on screen rather than knocking
 * through to a background colour, so whatever the image is being laid over --
 * a backdrop, a panel, a list row -- is what shows around the curve. Wants
 * DRMODE_FG, like fill_round_rect_aa(). */
void bitmap_part_round(struct screen *s, const fb_data *src, int stride,
                       int x, int y, int w, int h,
                       int r, const unsigned char *mask);

#endif /* _ROUND_RECT_H_ */
