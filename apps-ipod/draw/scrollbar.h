/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/scrollbar.h
 * Copyright (C) 2005 Kevin Ferrare
 * GNU General Public License (version 2+)
 *
 * Interface to scrollbar.c.
 ****************************************************************************/

#ifndef _GUI_SCROLLBAR_H_
#define _GUI_SCROLLBAR_H_
#include "screen_access.h"

enum orientation {
    VERTICAL          = 0x0000,   /* Vertical orientation     */
    HORIZONTAL        = 0x0001,   /* Horizontal orientation   */
    INVERTFILL        = 0x0002,   /* Invert the fill direction */
    INNER_NOFILL      = 0x0004,   /* Do not fill inner part */
    BORDER_NOFILL     = 0x0008,   /* Do not fill border part */
    FOREGROUND        = 0x0020,   /* Do not clear background pixels */
    INNER_FILL        = 0x0040,   /* Fill inner part even if FOREGROUND */
    INNER_BGFILL      = 0x0080,   /* Fill inner part with background
                                     color even if FOREGROUND */
    INNER_FILL_MASK   = 0x00c0,
    DONT_CLEAR_EXCESS = 0x0100,   /* Don't clear the entire bar area */
};

/*
 * Draws a scrollbar on the given screen
 *  - screen : the screen to put the scrollbar on
 *  - x : x start position of the scrollbar
 *  - y : y start position of the scrollbar
 *  - width : you won't guess =(^o^)=
 *  - height : I won't tell you either !
 *  - items : total number of items on the screen
 *  - min_shown : index of the starting item on the screen
 *  - max_shown : index of the last item on the screen
 *  - orientation : either VERTICAL or HORIZONTAL
 */
extern void gui_scrollbar_draw(struct screen * screen, int x, int y,
                               int width, int height, int items,
                               int min_shown, int max_shown,
                               unsigned flags);
extern void gui_bitmap_scrollbar_draw(struct screen * screen, struct bitmap *bm,
                            int x, int y,
                            int width, int height, int items,
                            int min_shown, int max_shown,
                            unsigned flags);
extern void show_busy_slider(struct screen *s, int x, int y,
                            int width, int height);
#endif /* _GUI_SCROLLBAR_H_ */
