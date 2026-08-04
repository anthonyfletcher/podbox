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

/* Mix c1 toward c2, per channel. t runs 0..256: 0 leaves c1 alone, 256 gives
 * c2. Used to derive a secondary colour from a foreground/background pair --
 * the only way to do that which holds up for any pair, including the arbitrary
 * ones the album-art colours produce. */
unsigned color_blend(unsigned c1, unsigned c2, int t);

/* A colour in base's family but distinctly not it: base's hue turned by
 * `degrees`, rebuilt at saturation `sat` and brightness `val` (both 0..255).
 * Saturation and brightness are given rather than kept because the colours
 * this is asked about -- theme and album backgrounds -- are typically dark and
 * flat, and keeping theirs would return something as unusable as the input. */
unsigned color_hue_rotate(unsigned base, int degrees, int sat, int val);

/* Where an accent sits from the background it belongs to, as a turn around the
 * colour wheel.
 *
 * 100 degrees rather than a true complement (180): on a navy background 180
 * lands on amber, which is legible but reads as a different design. 100 lands
 * on a pink-mauve -- which is, to within a couple of values, the accent this
 * fork's theme already used by eye before any of it was computed.
 *
 * Also the orientation given to a theme that has no hue of its own to measure
 * from, so its accents land where a derived one would have. */
#define COLOR_ACCENT_ROTATE 100

/* Split a colour into hue (0..359 degrees), saturation and brightness (both
 * 0..255), and put one back together. A grey has no hue and reports 0. */
void color_get_hsv(unsigned c, int *h, int *s, int *v);
unsigned color_from_hsv(int h, int s, int v);

/* c moved to at least `target` contrast (in hundredths) against `against`,
 * keeping its hue and saturation and changing only its brightness. Falls back
 * to white or black when the hue cannot reach the target at any brightness. */
unsigned color_fit_contrast(unsigned c, unsigned against, int target);

/* WCAG contrast between two colours, in hundredths: 450 is the 4.5:1 wanted
 * for body text, 700 the 7:1 of AAA. A ratio rather than a difference, because
 * the same gap between two dark colours reads far weaker than between two
 * light ones. (skin_albumart_color.c carries the same formula over unpacked
 * channels, for use inside its extraction loop.) */
int color_contrast(unsigned c1, unsigned c2);

#endif /* _COLOR_H_ */
