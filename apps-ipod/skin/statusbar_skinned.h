/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/statusbar-skinned.h
 * Copyright (C) 2009 Thomas Martitz
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Interface to statusbar_skinned.c.
 ****************************************************************************/
#ifndef __STATUSBAR_SKINNED_H__
#define __STATUSBAR_SKINNED_H__

#define DEFAULT_UPDATE_DELAY (HZ/7)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "kernel.h"
#include "draw/icon.h"
#include "skin_engine.h"

struct wps_data;

char* sb_create_from_settings(enum screen_type screen);
void sb_skin_init(void) INIT_ATTR;
struct viewport *sb_skin_get_info_vp(enum screen_type screen);
void sb_skin_update(enum screen_type screen, bool force);

void sb_skin_set_update_delay(int delay);
void sb_skin_force_next_update(void);
bool sb_set_title_text(const char* title, enum themable_icons icon, enum screen_type screen);
bool sb_set_persistent_title(const char* title, enum themable_icons icon,
                             enum screen_type screen);
void sb_skin_has_title(enum screen_type screen);
const char* sb_get_title(enum screen_type screen);
const char* sb_get_persistent_title(enum screen_type screen);
enum themable_icons sb_get_icon(enum screen_type screen);


int sb_get_backdrop(enum screen_type screen);
void sb_process(enum screen_type screen, struct wps_data *data, bool preprocess);

void do_sbs_update_callback(unsigned short id, void *param);
#endif /* __STATUSBAR_SKINNED_H__ */
