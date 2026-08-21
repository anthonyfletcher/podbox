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

/* Controls. The viewer has two key maps; the "Zoom / Pan" row in the settings
 * menu -- hold Menu -- swaps between them, and nothing else does.
 *
 *                  navigation (default)     zoom / pan
 *   wheel          -                        zoom in / out
 *   Left / Right   previous / next picture  pan, repeating
 *   Menu (tap)     leave the viewer         pan up
 *   Menu (hold)    settings menu            settings menu
 *   Play           -                        pan down, repeating
 *
 * Menu leaves on the release, not the press: the press cannot yet be told
 * apart from the start of a hold.
 */
#define IMGVIEW_ZOOM_IN     BUTTON_SCROLL_FWD
#define IMGVIEW_ZOOM_OUT    BUTTON_SCROLL_BACK
#define IMGVIEW_UP          BUTTON_MENU
#define IMGVIEW_DOWN        BUTTON_PLAY
#define IMGVIEW_LEFT        BUTTON_LEFT
#define IMGVIEW_RIGHT       BUTTON_RIGHT
#define IMGVIEW_MENU        (BUTTON_MENU | BUTTON_REPEAT)
#define IMGVIEW_EXIT        (BUTTON_MENU | BUTTON_REL)

#endif /* _IMAGE_VIEWER_BUTTONS_H */
