/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/splash.h
 * Copyright (C) 2005 by Kevin Ferrare
 * GNU General Public License (version 2+)
 *
 * Interface to splash.c.
 ****************************************************************************/

#ifndef _GUI_SPLASH_H_
#define _GUI_SPLASH_H_

#include <stdbool.h>
#include "draw/screen_access.h"
#include "gcc_extensions.h"

/*
 * Puts a splash message centered on all the screens for a given period
 *  - ticks : how long the splash is displayed (in rb ticks)
 *  - fmt : what to say *printf style
 */
extern void splashf(int ticks, const char *fmt, ...) ATTRIBUTE_PRINTF(2, 3);

/*
 * Puts a splash message centered on all the screens for a given period
 *  - ticks : how long the splash is displayed (in rb ticks)
 *  - str : what to say, if this is a LANG_* string (from ID2P)
 *          it will be voiced
 */
#define splash(__ticks, __str) splashf(__ticks, __str)

/* set a delay before displaying the progress meter the first time */
extern void splash_progress_set_delay(long delay_ticks);
/* Silence the progress meter's own voice, for a caller that announces the same
 * work in better words. Sticky: set it before a run of splash_progress() calls
 * and clear it after. */
extern void splash_progress_set_silent(bool silent);
/*
 * Puts a splash message centered on all the screens with a progressbar
 *  - current : current progress increment
 *  - total : total increments
 *  - fmt : what to say *printf style
 * updates limited internally to 20 fps - call repeatedly to update progress
 */
extern void splash_progress(int current, int total, const char *fmt, ...) ATTRIBUTE_PRINTF(3, 4);
#endif /* _GUI_ICON_H_ */
