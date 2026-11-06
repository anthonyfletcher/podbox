/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to shell_font.c: FONT.TEXT's glyphs from the font file.
 ****************************************************************************/
#ifndef SHELL_FONT_H
#define SHELL_FONT_H

#include <stdint.h>

/* struct arcade_host's glyph call */
int shell_glyph(void *ctx, uint32_t cp, uint8_t out[13]);
/* A cart ending: the file closed, to be opened again by the next */
void shell_font_end(void);

#endif
