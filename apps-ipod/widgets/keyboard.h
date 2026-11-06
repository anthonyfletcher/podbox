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

/* Click-wheel single-line text editor; the titled one shows title in the
 * box above the line, as the prose box does. */
int dialog_input(char* buffer, int buflen);
int dialog_input_titled(const char *title, char* buffer, int buflen);

/* Plugin-ABI wrapper for dialog_input(); `kbd` is ignored. */
int kbd_input(char* buffer, int buflen, ucschar_t *kbd);

#endif
