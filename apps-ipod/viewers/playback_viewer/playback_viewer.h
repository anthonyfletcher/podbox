/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Interface to playback_viewer.c.
 ****************************************************************************/
#ifndef _PLAYBACK_VIEWER_H
#define _PLAYBACK_VIEWER_H

/* Crunch the playback log and flip through the deck of cards built from it.
 * Presented in the UI as "Spun".
 *
 * Returns a GO_TO_* screen code for root_menu.c, as every root-menu screen
 * does: where the user should end up once the deck is closed. */
int playback_viewer_screen(void);

#endif /* _PLAYBACK_VIEWER_H */
