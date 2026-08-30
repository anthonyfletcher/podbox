/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to text_shadow.c: the soft drop shadow behind a line of text.
 ****************************************************************************/

#ifndef _TEXT_SHADOW_H_
#define _TEXT_SHADOW_H_

#include "draw/line.h"
#include "draw/screen_access.h"

/* Blur radii above this are reduced to it. The scratch the shadow is built
 * in is padded by the radius on every side, so the ceiling bounds that
 * buffer as well as the work. */
#define TEXT_SHADOW_MAX_BLUR 3

/* Draw `line`'s shadow for the string that is about to be drawn at (x, y)
 * with `text_skip_pixels` of it scrolled off the left. Coordinates and the
 * clip are the current viewport's, as for putsxy(). The caller's drawmode
 * and foreground are left as they were found. */
void text_shadow_draw(struct screen *display, int x, int y,
                      const char *text, int text_skip_pixels,
                      const struct line_desc *line);

#endif /* _TEXT_SHADOW_H_ */
