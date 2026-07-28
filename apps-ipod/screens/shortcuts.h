/***************************************************************************
 * Original code from RockBox
 * was: apps/shortcuts.h
 * Copyright (C) 2011 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * Interface to shortcuts.c.
 ****************************************************************************/
#ifndef __SHORTCUTS_H__
#define __SHORTCUTS_H__
#include <stdbool.h>
#include <stdlib.h>
 
enum shortcut_type {
    SHORTCUT_UNDEFINED = -1,
    SHORTCUT_SETTING = 0,
    SHORTCUT_FILE,
    SHORTCUT_DEBUGITEM,
    SHORTCUT_BROWSER,
    SHORTCUT_SETTING_APPLY,
    SHORTCUT_PLAYLISTMENU,
    SHORTCUT_SEPARATOR,
    SHORTCUT_SHUTDOWN,
    SHORTCUT_REBOOT,
    SHORTCUT_TIME,

    SHORTCUT_TYPE_COUNT
};

void shortcuts_add(enum shortcut_type type, const char* value);
void shortcuts_init(void);
int do_shortcut_menu(void*ignored);
/* True if at least one shortcut is defined (cheap file-head probe). */
bool shortcuts_available(void);

#endif
