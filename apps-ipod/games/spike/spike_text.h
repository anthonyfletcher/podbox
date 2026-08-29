/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A pixel font, drawn as blocks.
 *
 * The chrome is drawn rather than typeset for the same reason the field is:
 * there are no assets, and a loaded font cannot be scaled. Five by seven
 * cells at a whole-number scale is a real pixel font at any size, and it
 * matches a field made entirely of straight lines -- an anti-aliased UI face
 * over it would look like two games.
 ****************************************************************************/

#ifndef SPIKE_TEXT_H
#define SPIKE_TEXT_H

#include <stdbool.h>

/* One cell of the glyph grid, in pixels, at scale 1. */
#define SPK_GLYPH_W      5
#define SPK_GLYPH_H      7
#define SPK_GLYPH_GAP    1

/* Advance and height of a string at a scale, so a caller can centre or
 * right-align one without knowing how it is drawn. */
int spk_text_width(const char *s, int scale);
int spk_text_height(int scale);

/* Digits, capitals and space; anything else is drawn as a space. Bold
 * widens every run by a pixel on *each* side rather than drawing twice, so
 * the word thickens about its own centre and stays on the same grid as the
 * plain one beside it. Growing to the right alone reads as the word sliding
 * rather than swelling. */
void spk_text(int x, int y, const char *s, int scale, bool bold);

/* A crown, on the same grid and drawn the same way, because it stands
 * beside a number: three points over a body and a band under it, seven
 * cells wide against the glyphs' five. It means the score beside it is the
 * best there has been. */
#define SPK_CROWN_W      7

int spk_crown_width(int scale);
void spk_crown(int x, int y, int scale);

#endif /* SPIKE_TEXT_H */
