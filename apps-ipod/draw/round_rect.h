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

#endif /* _ROUND_RECT_H_ */
