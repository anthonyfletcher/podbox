/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/color_picker.h
 * Copyright (C) Jonathan Gordon (2006)
 * GNU General Public License (version 2+)
 *
 * Interface to color_picker.c.
 ****************************************************************************/
#include <stdbool.h>

/* Edit `color` in place. Returns true if USB interrupted, in which case the
 * colour is left alone. `banned_color` is the one value OK refuses, or
 * (unsigned)-1 to allow any.
 *
 * No screen argument: dialog_run() iterates the screens itself. */
bool set_color(const char *title, unsigned *color, unsigned banned_color);

