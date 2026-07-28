/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/usb_screen.h
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * Interface to usb_screen.c.
 ****************************************************************************/
#ifndef _USB_SCREEN_H_
#define _USB_SCREEN_H_

#include <stdint.h>

extern void gui_usb_screen_run(bool early_usb, intptr_t seqnum);

#endif

