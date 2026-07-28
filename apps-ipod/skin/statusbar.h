/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/statusbar.h
 * Copyright (C) 2005 by Kevin Ferrare
 * GNU General Public License (version 2+)
 *
 * Interface to statusbar.c and the status bar state types.
 ****************************************************************************/

#ifndef _GUI_STATUSBAR_H_
#define _GUI_STATUSBAR_H_

#include "config.h"
#include "button.h"
#include "timefuncs.h"
#include "audio/play_status.h"
#include "draw/screen_access.h"
#include "events.h"

struct status_info {
    int battlevel;
    int batt_charge_step;
    int volume;
    int playmode;
    int repeat;
    bool inserted;
    bool usb_inserted;
    bool battery_state;
    bool shuffle;
    bool keylock;
    bool led; /* disk LED simulation in the status bar */

};

/* statusbar visibility/position, used for settings also */
enum statusbar_values { STATUSBAR_OFF = 0, STATUSBAR_TOP, STATUSBAR_BOTTOM };

struct gui_statusbar
{
    struct status_info info;
    struct status_info lastinfo;

    bool redraw_volume; /* true if the volume gauge needs updating */
    int last_volume;
    long volume_icon_switch_tick;

    long battery_icon_switch_tick;

    struct tm *time;
    int last_tm_min;
    struct screen * display;
};


extern struct gui_syncstatusbar statusbars;

/*
 * Attach the status bar to a screen
 * (The previous screen attachement is lost)
 *  - bar : the statusbar structure
 *  - display : the screen to attach
 */
#define gui_statusbar_set_screen(gui_statusbar, _display) \
    (gui_statusbar)->display = (_display);


/*
 * Draws the status bar on the attached screen
 * - bar : the statusbar structure
 */
extern void gui_statusbar_draw(struct gui_statusbar * bar,
                               bool force_redraw, struct viewport *vp);


struct gui_syncstatusbar
{
    struct gui_statusbar statusbars[NB_SCREENS];
};

extern void gui_syncstatusbar_init(struct gui_syncstatusbar * bars) INIT_ATTR;

#include "settings/settings.h"
#define statusbar_position(a) ((enum statusbar_values)global_settings.statusbar)

#endif /*_GUI_STATUSBAR_H_*/
