/***************************************************************************
 * Original code from RockBox
 * was: apps/screens.h (set_time_screen)
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * Interface to time_set.c.
 ****************************************************************************/

#ifndef _TIME_SET_H_
#define _TIME_SET_H_

#include <stdbool.h>
#include "timefuncs.h"

/* Interactive date/time setter. Returns true if the user confirmed. */
bool set_time_screen(const char* title, struct tm *tm, bool set_date);

#endif /* _TIME_SET_H_ */
