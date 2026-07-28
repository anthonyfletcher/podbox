/***************************************************************************
 * Original code from RockBox
 * was: apps/screens.h (browse_id3)
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * Interface to track_info.c (browse_id3).
 ****************************************************************************/

#ifndef _TRACK_INFO_H_
#define _TRACK_INFO_H_

#include <stdbool.h>
#include "timefuncs.h"
#include "metadata.h"
#include "playlist/playlist.h"

/* Track Info screen. The view_text callback renders one field full-screen;
 * callers pass widgets/text_box.c's view_text(). */
bool browse_id3_ex(struct mp3entry *id3, struct playlist_info *playlist,
                   int playlist_display_index, int playlist_amount,
                   struct tm *modified, int track_ct,
                   int (*view_text)(const char *title, const char *text));

bool browse_id3(struct mp3entry *id3, int playlist_display_index,
                int playlist_amount, struct tm *modified, int track_ct,
                int (*view_text)(const char *title, const char *text));

#endif /* _TRACK_INFO_H_ */
