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
 * Otherwise return the original color unchanged.
 *
 * Also where COLOR_FIXED (draw/color.h) comes off, so a colour a skin wrote
 * with a leading '!' passes through untouched. Every skin colour reaches the
 * display through here, which is what makes one strip enough. */
unsigned int dynamic_colors_resolve(unsigned int original);

/* True for a short window after the palette changes, during which whatever is
 * on screen owes itself a repaint. The colours have already changed -- this is
 * not a transition in progress -- but a screen that only redraws on input
 * would otherwise keep the previous ones until the next keypress, and one
 * blocked waiting for input would keep them indefinitely. */
bool dynamic_colors_needs_repaint(void);

/* Check if color extraction is needed and perform it (call from UI thread) */
void dynamic_colors_check_extraction(int aa_slot);

/* Run a skin's %Cl filter chain over the art in the given slot, at most once
 * per buffered image. Cheap to call every render pass, and does nothing for
 * a chain with no stages.
 *
 * A chain that only rewrites pixels does so in the slot's own copy; one that
 * blurs renders into the skin's filter buffer instead and leaves the art
 * alone. Either way draw_album_art() picks up the result.
 *
 * Call it after dynamic_colors_check_extraction(), never before: the palette
 * must be derived from the unfiltered art. UI thread only, for the same
 * reason extraction is -- the source bitmap is movable and read unpinned. */
struct skin_albumart;
void skin_albumart_filter(int aa_slot, struct skin_albumart *aa);

/* Which run of buffered art the filter guards refer to. It moves whenever the
 * art changes, so `filtered_gen == skin_albumart_gen()` is the question "is
 * what I rendered still this track's cover?" -- one a handle id cannot answer
 * on its own, since buflib reissues ids. Compared, never interpreted. */
unsigned skin_albumart_gen(void);

/* Playback has loaded a new art bitmap, and `handle` is where it put it. Two
 * things follow: the chain owes that buffer a pass, and any record of having
 * filtered a handle of this id describes a picture that no longer exists,
 * since buflib reissues ids.
 *
 * Call it from the load and not from the track change. Art outlives a track --
 * playback hands the same handle back for every track of an album -- and a
 * chain that rewrites pixels in place must not run over one of those twice.
 *
 * Called on the audio thread, and safe there because playback makes the handle
 * current only after this returns: the UI thread cannot match on an id this
 * has not finished striking off. */
void skin_albumart_art_opened(int handle);

/* Take the palette from the folder the last session's resume point belongs to.
 * Boot only, called before the first screen is painted: it establishes the
 * colours rather than changing them, so nothing repaints. Does nothing without
 * a resume point, a remembered folder, or a thumbnail cached for it. */
void dynamic_colors_seed_resume(void);

/* Re-save theme default colors (call after theme .cfg is applied) */
void dynamic_colors_save_theme(void);

/* Returns true once after a fade completes, to request a full screen redraw */
bool dynamic_colors_needs_full_update(void);

/* Returns true once after a fade completes, to clear full-screen bg gaps */
bool dynamic_colors_screen_clear_needed(void);

/* Returns true when color extraction is queued but not yet performed */
bool dynamic_colors_pending(void);


#endif /* SKIN_ALBUMART_COLOR_H */
