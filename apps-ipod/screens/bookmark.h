/***************************************************************************
 * Original code from RockBox
 * was: apps/bookmark.h
 * Copyright (C) 2003 by Benjamin Metzler
 * GNU General Public License (version 2+)
 *
 * Interface to bookmark.c.
 ****************************************************************************/

#ifndef __BOOKMARK_H__
#define __BOOKMARK_H__

#include <stdbool.h>

enum {
    BOOKMARK_FAIL = -1,
    BOOKMARK_SUCCESS = 0,
    BOOKMARK_USB_CONNECTED = 1
};

enum {
    BOOKMARK_CANCEL,
    BOOKMARK_DONT_RESUME,
    BOOKMARK_DO_RESUME
};

int  bookmark_load_menu(void);
bool bookmark_autobookmark(bool prompt_ok);
bool bookmark_create_menu(void);
bool bookmark_mrb_load(void);
int  bookmark_autoload(const char* file);
bool bookmark_load(const char* file, bool autoload);
bool bookmark_exists(void);
bool bookmark_is_bookmarkable_state(void);

#endif /* __BOOKMARK_H__ */

