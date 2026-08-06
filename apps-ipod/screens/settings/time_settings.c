/***************************************************************************
 * Original code from RockBox
 * was: apps/menus/time_menu.c
 * Copyright (C) 2007 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * Time and date menu: sets the clock and the display format.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include "config.h"
#include "string.h"
#include "lang.h"
#include "kernel.h"
#include "timefuncs.h"
#include "input/action.h"
#include "settings/settings.h"
#include "widgets/menu.h"
#include "system/activity.h"
#include "system/format_time.h"
#include "widgets/keyboard.h"
#include "speech/talk.h"
#include "time.h"
#include "draw/viewport.h"
#include "widgets/list.h"
#include "screens/system/time_set.h"
#include "font.h"
#include "system.h"


static int timedate_set(void)
{
    /* Make a local copy of the time struct */
    struct tm tm = *get_time();
    int result;

    /* do some range checks */
    /* This prevents problems with time/date setting after a power loss */
    if (!valid_time(&tm))
    {
/* Macros to convert a 2-digit string to a decimal constant. 
        (YEAR), MONTH and DAY are set by the date command, which outputs
        DAY as 00..31 and MONTH as 01..12. The leading zero would lead to
        misinterpretation as an octal constant. */
#define S100(x) 1 ## x
#define C2DIG2DEC(x) (S100(x)-100)

        tm.tm_hour = 0;
        tm.tm_min  = 0;
        tm.tm_sec  = 0;
        tm.tm_mday = C2DIG2DEC(DAY);
        tm.tm_mon  = C2DIG2DEC(MONTH)-1;
        tm.tm_wday = 1;
        tm.tm_year = YEAR-1900;
    }

    result = (int)set_time_screen(str(LANG_SET_TIME), &tm, true);

    if(tm.tm_year != -1) {
        set_time(&tm);
    }
    return result;
}

MENUITEM_FUNCTION(time_set, 0, ID2P(LANG_SET_TIME),
                  timedate_set, NULL, Icon_NOICON);
MENUITEM_SETTING(timeformat, &global_settings.timeformat, NULL);

static void draw_timedate(struct viewport *vp, struct screen *display)
{
    struct tm *tm = get_time();
    int line;
    char time[16], date[16];
    const char *t = time, *d = date;
    if (vp->height == 0)
        return;
    struct viewport *last_vp = display->set_viewport(vp);
    display->clear_viewport();
    if (viewport_get_nb_lines(vp) >= 4)
        line = 1;
    else
        line = 0;

    if (valid_time(tm))
    {
        snprintf(time, sizeof(time), "%02d:%02d:%02d%s", 
                global_settings.timeformat == 0 ? tm->tm_hour :
                        ((tm->tm_hour + 11) % 12) + 1,
                            tm->tm_min, 
                            tm->tm_sec, 
                        global_settings.timeformat == 0 ? "" :
                                tm->tm_hour>11 ? " P" : " A");
        snprintf(date, sizeof(date), "%s %d %d", 
                str(LANG_MONTH_JANUARY + tm->tm_mon),
                    tm->tm_mday,
                    tm->tm_year+1900);
    }
    else
    {
        t = "--:--:--";
        d = str(LANG_UNKNOWN);
    }

    display->puts(0, line++, t);
    display->puts(0, line, d);

    display->update_viewport();
    display->set_viewport(last_vp);
}


static struct viewport clock_vps[NB_SCREENS], menu[NB_SCREENS];
static bool menu_was_pressed;
static int time_menu_callback(int action,
                              const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    static int last_redraw = 0;
    bool redraw = false;

    if (TIME_BEFORE(last_redraw+HZ/2, current_tick))
        redraw = true;
    switch (action)
    {
        case ACTION_ENTER_MENUITEM:
            /* This fires before the list is drawn, so painting the clock now
             * would only be covered by it. Returning an action instead makes
             * do_menu() skip its first get_action() and come straight back
             * here once the list has settled -- otherwise the clock is blank
             * for up to a second. */
            return ACTION_REDRAW;
        case ACTION_REDRAW:
            redraw = true;
            break;
        case ACTION_STD_CONTEXT:
            talk_timedate();
            action = ACTION_NONE;
            break;
        /* need to tell do_menu() to return, but then get time_screen()
           to return 0! ACTION_STD_MENU will return GO_TO_PREVIOUS from here
           so check do_menu()'s return val and menu_was_pressed */
        case ACTION_STD_MENU:
            menu_was_pressed = true;
            break;
    }
    if (redraw)
    {
        last_redraw = current_tick;
        FOR_NB_SCREENS(i)
            draw_timedate(&clock_vps[i], &screens[i]);
    }
    return action;
}


MAKE_MENU(time_menu, ID2P(LANG_TIME_MENU), time_menu_callback, Icon_NOICON,
          &time_set,
          &timeformat);

int time_screen(void* ignored)
{
    (void)ignored;
    int nb_lines, font_h, ret;
    menu_was_pressed = false;

    push_current_activity(ACTIVITY_TIMEDATESCREEN);

    FOR_NB_SCREENS(i)
    {
        viewport_set_defaults(&clock_vps[i], i);
        nb_lines = viewport_get_nb_lines(&clock_vps[i]);

        gui_synclist_set_viewport_defaults(&menu[i], i);
        /* force time to be drawn centered */
        clock_vps[i].flags |= VP_FLAG_ALIGN_CENTER;

        font_h = font_get(clock_vps[i].font)->height;
        nb_lines -= 2; /* at least 2 lines for menu */
        if (nb_lines > 4)
            nb_lines = 4;
        if (nb_lines >= 2)
            clock_vps[i].height = nb_lines*font_h;
        else /* disable the clock_vps drawing */
            clock_vps[i].height = 0;
        menu[i].y += clock_vps[i].height;
        menu[i].height -= clock_vps[i].height;
        draw_timedate(&clock_vps[i], &screens[i]);
    }


    ret = do_menu(&time_menu, NULL, menu, false);
    pop_current_activity();
    /* see comments above in the button callback */
    if (!menu_was_pressed && ret == GO_TO_PREVIOUS)
        return 0;
    return ret;
}
