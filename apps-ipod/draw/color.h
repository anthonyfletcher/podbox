/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.h (colour parsing)
 * GNU General Public License (version 2+)
 *
 * Interface to color.c.
 ****************************************************************************/
#ifndef _COLOR_H_
#define _COLOR_H_

#include <stdbool.h>
#include "draw/screen_access.h"

/* Parse "#rrggbb" (or "rrggbb") into a packed RGB value. Returns 0 on
 * success, -1 if the string is not a valid hex colour. */
int hex_to_rgb(const char* hex, int* color);

/* Parse a colour for the given screen, accepting the forms theme files and
 * skins use. Returns true if text held a usable colour. */
bool parse_color(enum screen_type screen, char *text, int *value);

#endif /* _COLOR_H_ */
