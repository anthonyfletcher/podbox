/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to progress_bar.c: a determinate progress bar.
 ****************************************************************************/

#ifndef _PROGRESS_BAR_H_
#define _PROGRESS_BAR_H_

#include "draw/screen_access.h"

/* Draw `current` out of `total` as a filled bar with a 1px border, in the
 * drawmode and foreground the caller has already set. `radius` rounds the
 * border; the fill rounds one step tighter so the two are concentric. The
 * unfilled part is left alone, so whatever is behind the bar shows through.
 *
 * Out-of-range arguments are corrected rather than rejected: a radius too
 * large for the bar is reduced to what fits, and `current` is clamped to
 * 0..total. */
void progress_bar_draw(struct screen *s, int x, int y, int w, int h,
                       int current, int total, int radius);

#endif /* _PROGRESS_BAR_H_ */
