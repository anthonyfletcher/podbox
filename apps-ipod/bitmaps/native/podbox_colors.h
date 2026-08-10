/***************************************************************************
 * New to this fork.
 * GNU General Public License (version 2+)
 *
 * Shared background/foreground colours for the podbox brand screens: the
 * credits background (credits.c). The podbox*.bmp art in this folder is drawn
 * over a matching fill, so keep these in step with the bitmaps when the design
 * changes.
 *
 * Not the boot screen -- that carries its own palettes, in main.c.
 ****************************************************************************/

#ifndef _PODBOX_COLORS_H_
#define _PODBOX_COLORS_H_

#include "lcd.h"

#define PODBOX_COLOR_BG  LCD_RGBPACK(0x03, 0x18, 0x35) /* #031835 */
#define PODBOX_COLOR_FG  LCD_RGBPACK(0xff, 0xff, 0xff) /* white   */

#endif /* _PODBOX_COLORS_H_ */
