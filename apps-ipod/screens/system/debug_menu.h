/***************************************************************************
 * Original code from RockBox
 * was: apps/debug_menu.h
 * Copyright (C) 2002 Heikki Hannikainen
 * GNU General Public License (version 2+)
 *
 * Interface to debug_menu.c.
 ****************************************************************************/
#ifndef _DEBUG_MENU_H
#define _DEBUG_MENU_H

/* The Debug list. MENU_ATTACHED_USB if it was left for the root menu,
 * otherwise 0. */
int  debug_menu(void);

/* Open one debug screen by its menu label, for a shortcut. True if that screen
 * was left for the root menu -- MENU, or a USB attach; false either for an
 * ordinary exit or for a label that names no screen. */
bool run_debug_screen(char* screen);

#endif
