/***************************************************************************
 * Original code from RockBox
 * was: apps/filetree.h
 * Copyright (C) 2005 by Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * Interface to browser_files.c (the ft_* entry points).
 ****************************************************************************/
#ifndef _BROWSER_DISK_H_
#define _BROWSER_DISK_H_
#include "browser.h"

int browser_disk_load(struct browser_context* c, const char* tempdir);
int browser_disk_enter(struct browser_context* c);
int browser_disk_exit(struct browser_context* c);
int browser_disk_assemble_path(char *buf, size_t bufsz,
                      const char* currdir, const char* filename);
int browser_disk_build_playlist(struct browser_context* c, int start_index);
bool browser_disk_play_playlist(char* pathname, char* dirname, char* filename);

/* The top row a freshly loaded directory wants shown, or -1 for no preference:
 * a root carrying the Search row opens scrolled past it. Clears the request.
 * The counterpart to browser_db_take_pending_top_item(). */
int browser_disk_take_pending_top_item(void);

#endif
