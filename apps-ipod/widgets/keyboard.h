/***************************************************************************
 * Original code from RockBox
 * was: apps/keyboard.h
 * Copyright (C) 2002 by Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * Interface to keyboard.c.
 ****************************************************************************/
#ifndef _KEYBOARD_H
#define _KEYBOARD_H

/* Click-wheel single-line text editor. */
int dialog_input(char* buffer, int buflen);

/* Plugin-ABI wrapper for dialog_input(); `kbd` is ignored. */
int kbd_input(char* buffer, int buflen, ucschar_t *kbd);

#endif
