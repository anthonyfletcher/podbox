/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to text_wrap.c: word-wrapping text to a pixel width.
 ****************************************************************************/

#ifndef _TEXT_WRAP_H_
#define _TEXT_WRAP_H_

#include "font.h"
#include "draw/screen_access.h"

/* Finds the end of the display line starting at `text`, wrapped to `width`
 * pixels in `pf`: at the last space that fits, else before the first glyph
 * that does not, else after a lone glyph wider than the width. A line break
 * in the text (\n, \r\n or \r) ends the line as well. Sets *end to the end
 * of the line's text and returns where the next line starts, which is the
 * '\0' when this was the last. The caller holds `pf` locked. */
const char *text_wrap_next(const char *text, int width, struct font *pf,
                           const char **end);

/* The number of display lines `text` wraps to in `font`; 0 for "". */
int text_wrap_count(const char *text, int width, int font);

/* Draws `text` wrapped to `width` in the current viewport's font, each line
 * aligned within it ('l', 'c' or 'r') from x, the first line's top at y and
 * the rest a font height apart. Lines wholly outside the viewport are
 * skipped and the viewport clips the rest, so y may be negative. Returns the
 * number of lines. UI thread only. */
int text_wrap_draw(struct screen *d, const char *text, int x, int y,
                   int width, char align);

#endif /* _TEXT_WRAP_H_ */
