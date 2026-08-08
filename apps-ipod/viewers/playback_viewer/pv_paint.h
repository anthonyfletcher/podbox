/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Interface to pv_paint.c.
 ****************************************************************************/
#ifndef _PV_PAINT_H
#define _PV_PAINT_H

#include <stddef.h>
#include "config.h"
#include "lcd.h"

/* A card's whole palette. Five colours describe one, which is what makes the
 * deck a table rather than sixteen drawing routines. */
struct pv_theme
{
    unsigned bg0, bg1;      /* background gradient, top to bottom */
    unsigned num0, num1;    /* hero numeral gradient, top to bottom */
    unsigned accent;        /* kickers, badges, highlights */
};

#define PV_TEXT_LIGHT  LCD_RGBPACK(255, 255, 255)
#define PV_TEXT_DIM    LCD_RGBPACK(205, 210, 220)

/* Load the body font, once. Falls back to the system font if the theme's is
 * missing, so nothing here can fail in a way a caller must handle. */
void pv_paint_init(void);
void pv_paint_done(void);
int  pv_body_font(void);

/* Lerp two RGB565 colours; t runs 0..256, 0 being all 'a'. */
unsigned pv_blend(unsigned a, unsigned b, int t);

/* The background colour at row y. Everything anti-aliased below blends
 * against this rather than reading the framebuffer back -- the whole toolkit
 * knows what is underneath it analytically, which is why a card can be drawn
 * over a gradient without ever sampling one. */
unsigned pv_grad_at(const struct pv_theme *th, int y);

void pv_fill(const struct pv_theme *th);

/* Repaint a horizontal band back to bare gradient, to redraw over it. */
void pv_band(const struct pv_theme *th, int y0, int h);

/* Anti-aliased filled disc; alpha 0..256 scales the whole thing. */
void pv_disc(const struct pv_theme *th, int cx, int cy, int r,
             unsigned col, int alpha);

/* Anti-aliased round-ended thick line, half-width hw. */
void pv_capsule(const struct pv_theme *th, int x1, int y1, int x2, int y2,
                int hw, unsigned col);

/* Vector numerals: seven-segment glyphs with rounded ends, anti-aliased and
 * filled with the theme's numeral gradient. Digits, comma and colon only --
 * these draw the hero figures, not text. 'cw'/'ch' are one glyph's box and
 * 'sw' the stroke width. */
int  pv_number_width(const char *s, int cw, int sw);
void pv_number(const struct pv_theme *th, int cx, int oy, const char *s,
               int cw, int ch, int sw);

/* A percent sign to match, built from two discs and a diagonal. */
void pv_percent(const struct pv_theme *th, int cx, int cy, int sz);

void pv_text_centre(int y, const char *s, unsigned colour);
void pv_text_centre2(int y, const char *a, unsigned ca,
                     const char *b, unsigned cb);

/* A centred label with a little letter-spacing, for the small-caps headings
 * above each card's figure. */
void pv_kicker(int y, const char *s, unsigned colour);

void pv_underline(const struct pv_theme *th, int y);

/* Copy s into out, truncating with ".." if it would be wider than maxw. */
void pv_fit_text(const char *s, int maxw, char *out, int outsz);

/* "1234" -> "1,234" */
void pv_commafmt(long v, char *out, size_t outsz);

/* The dots along the bottom saying where in the deck this card is. */
void pv_page_dots(const struct pv_theme *th, int idx, int count);

/* Whether cards should skip their animations and draw the finished frame.
 *
 * Set while exporting to pictures, where a frame caught mid-count is a
 * picture of a number that was never true. */
void pv_set_export(bool on);
bool pv_exporting(void);

/* Ease-out-quad: most of the distance early, settling gently. */
long pv_ease(long target, int fr, int frames);

/* Buttons worth abandoning an animation for -- navigation, or USB. */
bool pv_nav_button(int b);

/* Count a figure up into place, in the band at (band_y, band_h).
 *
 * Returns 0 when it ran to the end, or the button that interrupted it. An
 * interrupted animation draws its final value before returning, so giving up
 * on one never leaves a half-counted number on screen -- which is what lets a
 * card be a flourish rather than a delay. */
int pv_animate_count(const struct pv_theme *th, int cx, int oy,
                     int band_y, int band_h,
                     int cw, int ch, int sw, long target);

/* The same, for a percentage with its sign drawn alongside. */
int pv_animate_percent(const struct pv_theme *th, int oy, int pct);

/* Push the finished frame. dir 0 flushes it; +1/-1 reveals it as a vertical
 * wipe in that direction -- a transition that costs no redrawing, only the
 * order the columns are sent in. */
void pv_present(int dir);

#endif /* _PV_PAINT_H */
