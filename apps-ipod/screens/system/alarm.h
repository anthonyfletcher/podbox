/***************************************************************************
 * Original code from RockBox
 * was: apps/alarm_menu.h
 * Copyright (C) 2003 Uwe Freese
 * GNU General Public License (version 2+)
 *
 * Interface to alarm.c.
 ****************************************************************************/
#ifndef _ALARM_H
#define _ALARM_H

#include <stdbool.h>

int alarm_screen(void);

/* Set by root_menu.c for the one boot that an RTC alarm started, and cleared
 * by the first alarm_show_wake_image() to act on it. */
extern bool alarm_woke_us;

/* Show the full-screen wake image until any button is pressed. Does nothing
 * unless alarm_woke_us is set, so it is safe to call on every WPS entry. */
void alarm_show_wake_image(void);

#endif
