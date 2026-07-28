/***************************************************************************
 * Original code from RockBox
 * was: apps/usb_keymaps.h
 * Copyright (C) 2009 Tomer Shalev
 * GNU General Public License (version 2+)
 *
 * Interface to usb_keymaps.c.
 ****************************************************************************/
#ifndef _USB_KEYMAPS_H_
#define _USB_KEYMAPS_H_

int get_hid_usb_action(void);

int keypad_mode_name_get(unsigned int mode);

#endif
