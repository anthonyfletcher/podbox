/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Interface to pv_export.c.
 ****************************************************************************/
#ifndef _PV_EXPORT_H
#define _PV_EXPORT_H

#include "pv_stats.h"

/* Saving cards as pictures.
 *
 * The firmware's screen_dump() writes the framebuffer to a BMP and chooses
 * the filename itself -- "dump <date>-<time>.bmp" in the device root, with no
 * say in the matter and no way to ask what it picked. So a card that wants a
 * name of its own is dumped, then found again by being the newest dump in the
 * root, then renamed. That is the whole trick, and the reason it lives here
 * rather than being repeated at both call sites. */

/* The card on screen, saved to the device root under screen_dump()'s own
 * name. Reports what happened. */
void pv_export_card(void);

/* Every card in the deck, drawn and saved in turn to /spun_cards.
 *
 * Quick enough on real hardware to need no progress of its own -- the cards
 * going past are the progress -- so it only reports how it went at the end.
 * Buttons pressed while it runs are swallowed rather than queued up.
 *
 * Animations are skipped rather than played: a picture of a half-counted
 * number would be a picture of something that was never true. */
void pv_export_deck(const struct pv_totals *t);

#endif /* _PV_EXPORT_H */
