/***************************************************************************
 * Original code from RockBox
 * was: apps/menus/eq_menu.h
 * Copyright (C) 2006 Dan Everton
 * GNU General Public License (version 2+)
 *
 * Interface to eq_menu.c.
 ****************************************************************************/
#ifndef _EQ_MENU_H
#define _EQ_MENU_H

#include "widgets/menu.h"
#include "config.h"
/* Various user interface limits and sizes */
#define EQ_CUTOFF_MIN        20
#define EQ_CUTOFF_MAX     22040
#define EQ_CUTOFF_STEP       10
#define EQ_CUTOFF_FAST_STEP 100
#define EQ_GAIN_MIN       (-240)
#define EQ_GAIN_MAX         240
#define EQ_GAIN_STEP          1
#define EQ_GAIN_FAST_STEP    10
#define EQ_Q_MIN              1
#define EQ_Q_MAX             64
#define EQ_Q_STEP             1
#define EQ_Q_FAST_STEP       10

#define EQ_USER_DIVISOR      10

bool eq_browse_presets(void);
int eq_menu_graphical(void);

/* utility functions for settings_list.c */
const char* eq_q_format(char* buffer, size_t buffer_size, int value,
                        const char* unit);
const char* eq_precut_format(char* buffer, size_t buffer_size, int value,
                             const char* unit);

/* callbacks for settings_list.c */
void eq_enabled_option_callback(bool enabled);

#endif
