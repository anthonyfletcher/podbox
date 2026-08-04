/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/viewport.h
 * Copyright (C) 2008 by Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * Interface to viewport.c and the viewport helpers used to set up drawing
 * areas.
 ****************************************************************************/

#ifndef __VIEWPORT_H__
#define __VIEWPORT_H__


#include "config.h"
#include "lcd.h"
#include "system.h"
#include "screen_access.h"

/* return the number of text lines in the vp viewport */
int viewport_get_nb_lines(const struct viewport *vp);

#define THEME_STATUSBAR             (BIT_N(0))
#define THEME_UI_VIEWPORT           (BIT_N(1))
#define THEME_BUTTONBAR             (BIT_N(2))
#define THEME_LANGUAGE              (BIT_N(3))
#define THEME_LISTS                 (BIT_N(3))
#define THEME_ALL                   (~(0u))

/* These are needed in checkwps */
void viewport_set_defaults(struct viewport *vp,
                            const enum screen_type screen);
void viewport_set_fullscreen(struct viewport *vp,
                              const enum screen_type screen);


/*
 * Initialize the viewportmanager, which in turns initializes the UI vp and
 * statusbar stuff
 */
void viewportmanager_init(void) INIT_ATTR;

void viewportmanager_theme_enable(enum screen_type screen, bool enable,
                                 struct viewport *viewport);
/* Force will cause a redraw even if the theme was previously and
 * currently enabled (i,e the undo doing nothing).
 * Should almost always be set to false except coming out of fully skinned screens */
void viewportmanager_theme_undo(enum screen_type screen, bool force_redraw);

/* True while the theme owns the screen, which is also what decides where a
 * default viewport's colours came from: the skin's UI viewport when set, the
 * global settings when not. */
bool viewportmanager_theme_enabled(enum screen_type screen);

/* call this when a theme changed */
void viewportmanager_theme_changed(const int);

void viewport_set_buffer(struct viewport *vp, struct frame_buffer_t *buffer,
                                                const enum screen_type screen);



#endif /* __VIEWPORT_H__ */
