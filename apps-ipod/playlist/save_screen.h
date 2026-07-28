/***************************************************************************
 * Original code from RockBox
 * was: apps/playlist_menu.h
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * Declares save_playlist_screen(), the "save current playlist" screen.
 * A screen, not a menu, and unrelated to the playlist settings screen.
 ****************************************************************************/
#ifndef _SAVE_SCREEN_H
#define _SAVE_SCREEN_H

#include "playlist.h"

int save_playlist_screen(struct playlist_info* playlist);

#endif
