/***************************************************************************
 * Original code from RockPod
 * was: apps/gui/skin_engine/skin_albumart_color.h
 * Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Interface to skin_albumart_color.c.
 ****************************************************************************/

#ifndef SKIN_ALBUMART_COLOR_H
#define SKIN_ALBUMART_COLOR_H


/* Initialize dynamic colors: save theme defaults, register playback events */
void dynamic_colors_init(void);

/* Resolve a color: if it matches theme fg/bg and dynamic colors are active,
 * return the album-art-derived color (with fade interpolation).
 * Otherwise return the original color unchanged. */
unsigned int dynamic_colors_resolve(unsigned int original);

/* True for a short window after the palette changes, during which whatever is
 * on screen owes itself a repaint. The colours have already changed -- this is
 * not a transition in progress -- but a screen that only redraws on input
 * would otherwise keep the previous ones until the next keypress, and one
 * blocked waiting for input would keep them indefinitely. */
bool dynamic_colors_needs_repaint(void);

/* Check if color extraction is needed and perform it (call from UI thread) */
void dynamic_colors_check_extraction(int aa_slot);

/* Re-save theme default colors (call after theme .cfg is applied) */
void dynamic_colors_save_theme(void);

/* Returns true once after a fade completes, to request a full screen redraw */
bool dynamic_colors_needs_full_update(void);

/* Returns true once after a fade completes, to clear full-screen bg gaps */
bool dynamic_colors_screen_clear_needed(void);

/* Returns true when color extraction is queued but not yet performed */
bool dynamic_colors_pending(void);


#endif /* SKIN_ALBUMART_COLOR_H */
