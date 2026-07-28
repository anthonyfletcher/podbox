/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.c (activity stack)
 * Copyright (C) 2002 by Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * The activity stack: tracks which screen is in front so the skin engine
 * and status bar can react to context changes.
 ****************************************************************************/

#include <stdbool.h>
#include "config.h"
#include "system.h"
#include "debug.h"
#include "draw/viewport.h"
#include "skin/skin_engine.h"  /* skin_update, CUSTOM_STATUSBAR */
#include "skin/statusbar_skinned.h" /* sb_skin_force_next_update */
#include "widgets/list.h"            /* skinlist_set_cfg */
#include "activity.h"

#define MAX_ACTIVITY_DEPTH 12
static enum current_activity
        current_activity[MAX_ACTIVITY_DEPTH] = {ACTIVITY_UNKNOWN};
static int current_activity_top = 0;

static void push_current_activity_refresh(enum current_activity screen, bool refresh)
{
    current_activity[current_activity_top++] = screen;
    FOR_NB_SCREENS(i)
    {
        skinlist_set_cfg(i, NULL);
        if (refresh)
        {
            skin_inhibit_flush(true);
            skin_update(CUSTOM_STATUSBAR, i, SKIN_REFRESH_ALL);
            skin_inhibit_flush(false);
        }
    }
    if (refresh)
        sb_skin_force_next_update();
}

static void pop_current_activity_refresh(bool refresh)
{
    current_activity_top--;
    FOR_NB_SCREENS(i)
    {
        skinlist_set_cfg(i, NULL);
        if (refresh)
        {
            skin_inhibit_flush(true);
            skin_update(CUSTOM_STATUSBAR, i, SKIN_REFRESH_ALL);
            skin_inhibit_flush(false);
        }
    }
    if (refresh)
        sb_skin_force_next_update();
}

void push_current_activity(enum current_activity screen)
{
    push_current_activity_refresh(screen, true);
}

void push_activity_without_refresh(enum current_activity screen)
{
    push_current_activity_refresh(screen, false);
}

void pop_current_activity(void)
{
    pop_current_activity_refresh(true);
}

void pop_current_activity_without_refresh(void)
{
    pop_current_activity_refresh(false);
}

enum current_activity get_current_activity(void)
{
    return current_activity[current_activity_top?current_activity_top-1:0];
}

/* Generic "working" busy indicator, surfaced to skins as %lw. */
static bool ui_working_flag = false;

bool ui_working(void)
{
    return ui_working_flag;
}

void ui_set_working(bool working)
{
    if (ui_working_flag == working)
        return;
    ui_working_flag = working;
    /* Redraw here: callers block without redrawing, so the indicator would
     * otherwise appear only once the work is over. */
    FOR_NB_SCREENS(i)
        sb_skin_update(i, true);
    skin_flush_dirty();
}

