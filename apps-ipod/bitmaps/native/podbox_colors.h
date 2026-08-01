/***************************************************************************
 * New to this fork.
 * GNU General Public License (version 2+)
 *
 * Shared background/foreground colours for the podbox brand screens: the
 * boot logo splash (apps/main.c), the USB screen art (usb_screen.c) and the
 * credits background (credits.c). The podbox*.bmp art in this folder is drawn
 * over a matching fill, so keep these in step with the bitmaps when the design
 * changes.
 ****************************************************************************/

#ifndef _PODBOX_COLORS_H_
#define _PODBOX_COLORS_H_

#include "lcd.h"

#define PODBOX_COLOR_BG  LCD_RGBPACK(0x03, 0x18, 0x35) /* #031835 */
#define PODBOX_COLOR_FG  LCD_RGBPACK(0xff, 0xff, 0xff) /* white   */

/* Boot screen progress bar (apps/main.c). One colour draws both the border
 * and the fill; the unfilled part is left as the art behind it. */
#define PODBOX_COLOR_BAR LCD_RGBPACK(0xc7, 0x8d, 0xbe) /* #c78dbe */

#endif /* _PODBOX_COLORS_H_ */
