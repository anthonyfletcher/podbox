/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to pv_row.c.
 ****************************************************************************/
#ifndef _PV_ROW_H
#define _PV_ROW_H

/* Spun as a scrolling row of cards: crunch the playback log once, then walk
 * the sections built from it.
 *
 * It runs beside the deck (playback_viewer.c) rather than replacing it yet.
 * Both share one statistics model and one app_buffer claim, so only one can
 * be up at a time -- which is what the root menu and the debug menu already
 * guarantee by only ever entering one.
 *
 * Returns a GO_TO_* screen code for root_menu.c, as every root-menu screen
 * does: where the user should end up once the row is closed. */
int pv_row_screen(void);

#endif /* _PV_ROW_H */
