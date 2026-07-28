/***************************************************************************
 * Original code from RockBox
 * was: apps/playlist_viewer.h
 * Copyright (C) 2003 Hardeep Sidhu
 * GNU General Public License (version 2+)
 *
 * Interface to viewer.c.
 ****************************************************************************/

#ifndef _PLAYLIST_VIEWER_H_
#define _PLAYLIST_VIEWER_H_

enum playlist_viewer_result playlist_viewer(void);
enum playlist_viewer_result playlist_viewer_ex(const char* filename,
                                               int* most_recent_selection);
bool search_playlist(void);

enum playlist_viewer_result {
    PLAYLIST_VIEWER_OK,
    PLAYLIST_VIEWER_CANCEL,
    PLAYLIST_VIEWER_USB,
    PLAYLIST_VIEWER_MAINMENU,
};

#endif
