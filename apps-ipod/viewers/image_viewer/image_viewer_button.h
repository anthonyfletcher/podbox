/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/imageviewer_button.h
 * Button definitions for the core image viewer.
 *
 * Only the iPod 4G pad (used by iPod Video 5G and Classic 6G/7G) is kept.
 * GNU General Public License (version 2+)
 *
 * Button mapping for the image viewer, kept separate so the keypad
 * bindings are all in one place.
 ****************************************************************************/

#ifndef _IMAGE_VIEWER_BUTTONS_H
#define _IMAGE_VIEWER_BUTTONS_H

#include "config.h"
#include "button.h"

/* Controls (iPod wheel), matching the rest of the system -- hold Menu for the
 * menu, Select to leave:
 *   scroll wheel      zoom in / out
 *   Menu (tap)        pan up (when zoomed)
 *   Menu (hold)       open the settings menu (slideshow, dithering, quit)
 *   Play              pan down (when zoomed)
 *   Left / Right      pan left / right (when zoomed); previous / next image
 *                     when the image fits the screen
 *   Select            exit the viewer
 */
#define IMGVIEW_ZOOM_IN     BUTTON_SCROLL_FWD
#define IMGVIEW_ZOOM_OUT    BUTTON_SCROLL_BACK
#define IMGVIEW_UP          BUTTON_MENU
#define IMGVIEW_DOWN        BUTTON_PLAY
#define IMGVIEW_LEFT        BUTTON_LEFT
#define IMGVIEW_RIGHT       BUTTON_RIGHT
#define IMGVIEW_MENU        (BUTTON_MENU | BUTTON_REPEAT)
#define IMGVIEW_QUIT        (BUTTON_SELECT | BUTTON_REL)

#endif /* _IMAGE_VIEWER_BUTTONS_H */
