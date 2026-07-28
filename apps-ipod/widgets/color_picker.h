/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/color_picker.h
 * Copyright (C) Jonathan Gordon (2006)
 * GNU General Public License (version 2+)
 *
 * Interface to color_picker.c.
 ****************************************************************************/
#include "draw/screen_access.h"


bool set_color(struct screen *display, char *title,
               unsigned *color, unsigned banned_color);

