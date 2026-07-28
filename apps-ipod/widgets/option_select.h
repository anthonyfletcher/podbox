/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/option_select.h
 * Copyright (C) 2005 by Kevin Ferrare
 * GNU General Public License (version 2+)
 *
 * Interface to option_select.c.
 ****************************************************************************/

#ifndef _GUI_OPTION_SELECT_H_
#define _GUI_OPTION_SELECT_H_
#include "config.h"
#include "draw/screen_access.h"
#include "settings/settings.h"

enum {
    LIST_ORDER_DESCENDING = 0,
    LIST_ORDER_ASCENDING  = 1,
};

bool option_screen(const struct settings_list *setting,
                   struct viewport parent[NB_SCREENS],
                   bool use_temp_var, const unsigned char* option_title);

void option_select_next_val(const struct settings_list *setting,
                            bool previous, bool apply);
const char *option_get_valuestring(const struct settings_list *setting,
                             char *buffer, int buf_len,
                             intptr_t temp_var);
void option_talk_value(const struct settings_list *setting, int value, bool enqueue);

/* only use this for int and bool settings */
int option_value_as_int(const struct settings_list *setting);

int get_setting_info_for_bar(const struct settings_list *setting, int offset, int *count, int *val);

#endif /* _GUI_OPTION_SELECT_H_ */
