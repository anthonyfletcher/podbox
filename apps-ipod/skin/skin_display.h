/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/skin_engine/skin_display.h
 * Copyright (C) 2002-2007 Björn Stenberg
 * Copyright (C) 2007-2008 Nicolas Pennequin
 * GNU General Public License (version 2+)
 *
 * Interface to skin_display.c.
 ****************************************************************************/
#include "config.h"
#include <stdio.h>
#include "wps_internals.h"
#include "skin_engine.h"
#include "statusbar_skinned.h"

#ifndef _SKIN_DISPLAY_H_
#define _SKIN_DISPLAY_H_


void draw_progressbar(struct gui_wps *gwps, struct skin_viewport* skin_viewport,
                      int line, struct progressbar *pb);
void draw_playlist_viewer_list(struct gui_wps *gwps, struct playlistviewer *viewer);
/* clears the area where the image was shown */
void clear_image_pos(struct gui_wps *gwps, struct gui_img *img);
void wps_display_images(struct gui_wps *gwps, struct viewport* vp);


/* Returns whether anything reached the framebuffer, so a caller that owns the
 * flush knows whether this viewport's rectangle is owed one. */
bool skin_render_viewport(struct skin_element* viewport, struct gui_wps *gwps,
                        struct skin_viewport* skin_viewport, unsigned long refresh_type);


/* Evaluate the conditional that is at *token_index and return whether a skip
   has ocurred. *token_index is updated with the new position.
*/
int evaluate_conditional(struct gui_wps *gwps, int offset,
                         struct conditional *conditional, int num_options);
/* Display a line appropriately according to its alignment format.
   format_align contains the text, separated between left, center and right.
   line is the index of the line on the screen.
   scroll indicates whether the line is a scrolling one or not.
*/
void write_line(struct screen *display, struct align_pos *format_align,
                int line, bool scroll, struct line_desc *line_desc);
void draw_peakmeters(struct gui_wps *gwps, int line_number,
                     struct viewport *viewport);
/* Draw the album art bitmap from the given handle ID onto the given Skin, in
   the box the %Cl `aa` describes. Call with clear = true to clear the bitmap
   instead of drawing it. */
struct skin_albumart;
void draw_album_art(struct gui_wps *gwps, struct skin_albumart *aa,
                    int handle_id, bool clear);

#endif
