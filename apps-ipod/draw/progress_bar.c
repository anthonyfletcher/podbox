/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A determinate progress bar: a rounded border with a fill nested inside it.
 ****************************************************************************/

/* Fill first, border last. The fill's right edge is square -- it is a partial
 * width, not a shape -- so at a high `current` it runs into the border's
 * rounded corners. Drawing the border afterwards puts the outline back over
 * the top of it, which is cheaper and exact where clipping the fill to the
 * rounded track would be neither.
 */

#include "draw/progress_bar.h"
#include "draw/round_rect.h"

void progress_bar_draw(struct screen *s, int x, int y, int w, int h,
                       int current, int total, int radius)
{
    int fill_w;

    if (w <= 0 || h <= 0)
        return;
    if (radius < 0)
        radius = 0;

    if (total <= 0)
        total = 1;
    if (current < 0)
        current = 0;
    if (current > total)
        current = total;

    /* Inside the 1px border, so one step tighter: the fill's corner circle
     * then shares the border's centre instead of running parallel to it. */
    fill_w = ((w - 2) * current) / total;
    if (fill_w > 0 && h > 2)
        fill_round_rect(s, x + 1, y + 1, fill_w, h - 2,
                        radius > 0 ? radius - 1 : 0);

    draw_round_rect(s, x, y, w, h, radius, 1);
}
