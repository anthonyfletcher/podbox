/***************************************************************************
 * Original code from RockBox
 * was: apps/menus/menu_common.h
 * Copyright (C) 2008 Dan Everton
 * GNU General Public License (version 2+)
 *
 * Interface to menu_common.c.
 ****************************************************************************/
#ifndef _MENU_COMMON_H
#define _MENU_COMMON_H

#include "widgets/menu.h"
#include "config.h"

int lowlatency_callback(int action,
                        const struct menu_item_ex *this_item,
                        struct gui_synclist *this_list);

#endif /* _MENU_COMMON_H */

