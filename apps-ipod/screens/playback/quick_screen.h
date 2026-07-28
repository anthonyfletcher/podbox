/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/quickscreen.h
 * Copyright (C) 2005 by Kevin Ferrare
 * GNU General Public License (version 2+)
 *
 * Interface to quickscreen.c.
 ****************************************************************************/
#include "button.h"
#include "config.h"


#ifndef _QUICK_SCREEN_H_
#define _QUICK_SCREEN_H_

#include "draw/screen_access.h"

struct settings_list;

enum quickscreen_item {
    QUICKSCREEN_TOP = 0,
    QUICKSCREEN_LEFT,
    QUICKSCREEN_RIGHT,
    QUICKSCREEN_BOTTOM,
    QUICKSCREEN_ITEM_COUNT,
};

enum quickscreen_return {
    QUICKSCREEN_OK = 0,
    QUICKSCREEN_IN_USB = 0x1,
    QUICKSCREEN_GOTO_SHORTCUTS_MENU = 0x2,
    QUICKSCREEN_CHANGED = 0x4,
};

extern int quick_screen_quick(int button_enter);
int quickscreen_set_option(void *data);
bool is_setting_quickscreenable(const struct settings_list *setting);

#endif /*_GUI_QUICK_SCREEN_H_*/
