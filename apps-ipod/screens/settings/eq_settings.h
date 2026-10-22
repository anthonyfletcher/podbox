/***************************************************************************
 * Original code from RockBox
 * was: apps/menus/eq_menu.h
 * Copyright (C) 2006 Dan Everton
 * GNU General Public License (version 2+)
 *
 * Interface to eq_settings.c.
 ****************************************************************************/
#ifndef _EQ_MENU_H
#define _EQ_MENU_H

#include "widgets/menu.h"
#include "config.h"
#include "eq.h"
/* Various user interface limits and sizes */
#define EQ_CUTOFF_MIN        20
#define EQ_CUTOFF_MAX     22040
#define EQ_CUTOFF_STEP       10
#define EQ_GAIN_MIN       (-240)
#define EQ_GAIN_MAX         240
#define EQ_GAIN_STEP          1
#define EQ_Q_MIN              1
#define EQ_Q_MAX             64
#define EQ_Q_STEP             1

#define EQ_USER_DIVISOR      10

extern struct eq_band_setting eq_defaults[EQ_NUM_BANDS];

/* Sends every EQ setting to the DSP, first setting precut from the curve
 * when Auto Headroom is on. */
void eq_apply(void);
/* The DSP's output rate, which the filters are designed at. */
unsigned int eq_dsp_frequency(void);
/* Asks for a name and saves the bands to EQS_DIR, leaving the path written
 * in path. False if cancelled or the write failed. */
bool eq_save_preset(char *path, size_t len);
void eq_reset_defaults(void);
/* The numeric list of every band. MENU_ATTACHED_USB if USB took it. */
int eq_band_list(void);

/* utility functions for settings_list.c */
const char* eq_q_format(char* buffer, size_t buffer_size, int value,
                        const char* unit);
const char* eq_precut_format(char* buffer, size_t buffer_size, int value,
                             const char* unit);

/* callbacks for settings_list.c */
void eq_enabled_option_callback(bool enabled);

/* Applies the curve on leaving the item. Any menu carrying an EQ setting needs
   this, including the copy of Enable EQ on the basic settings page. */
int eq_setting_callback(int action, const struct menu_item_ex *this_item,
                        struct gui_synclist *this_list);

#endif
