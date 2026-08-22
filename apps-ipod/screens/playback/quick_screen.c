/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/quickscreen.c
 * Copyright (C) 2008 by Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * The quick screen: four settings bound to the directional buttons, edited
 * in place without leaving playback.
 ****************************************************************************/

#include <stdio.h>
#include "config.h"
#include "system.h"
#include "draw/icon_bitmaps.h"
#include "font.h"
#include "kernel.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "system/volume.h"
#include "sound.h"
#include "input/action.h"
#include "settings/settings_list.h"
#include "lang.h"
#include "playlist/playlist.h"
#include "draw/viewport.h"
#include "audio.h"
#include "quick_screen.h"
#include "speech/talk.h"
#include "widgets/list.h"
#include "widgets/option_select.h"
#include "debug.h"
#include "screens/shortcuts.h"
#include "system/appevents.h"
#include "skin/statusbar_skinned.h"  /* sb_skin_force_next_update */
#include "skin/skin_display.h"        /* skin_mark_dirty */

 /* 2 lines for each of the three vertical sections (top/middle/bottom).
  * With less space than that, the top and bottom each lose a line -- at
  * exactly 5 they would otherwise overlap the middle one. */
#define MIN_LINES (3*2)
#define MAX_NEEDED_LINES 10
 /* pixels between the 2 center items minimum or between text and icons,
  * and between text and parent boundaries */
#define MARGIN 10
#define CENTER_ICONAREA_SIZE (MARGIN+8*2)

/* The viewports live here rather than on gui_syncquickscreen_run's stack so
 * that the redraw callback can paint from them directly. Deferring the redraw
 * to the main loop instead left the screen showing a stale frame until the
 * next pass, which is visible as a flicker. */
struct gui_quickscreen
{
    const struct settings_list *items[QUICKSCREEN_ITEM_COUNT];
    struct viewport parent[NB_SCREENS];
    struct viewport vps[NB_SCREENS][QUICKSCREEN_ITEM_COUNT];
    struct viewport vp_icons[NB_SCREENS];
    int button_enter;
    int result;
};

static void quickscreen_fix_viewports(struct gui_quickscreen *qs,
                                      enum screen_type screen)
{
    int char_height, width, pad = 0;
    int left_width = 0, right_width = 0, vert_lines;
    unsigned char *s;
    struct screen *display = &screens[screen];
    struct viewport *parent = &qs->parent[screen];
    struct viewport *vps = qs->vps[screen];
    struct viewport *vp_icons = &qs->vp_icons[screen];
    int nb_lines = viewport_get_nb_lines(parent);

    /* nb_lines only returns the number of fully visible lines, small screens
        or really large fonts could cause problems with the calculation below.
     */
    if (nb_lines == 0)
        nb_lines++;

    char_height = parent->height/nb_lines;

    /* center the icons VP first */
    *vp_icons = *parent;
    vp_icons->width = CENTER_ICONAREA_SIZE; /* abosulte smallest allowed */
    vp_icons->x = parent->x;
    vp_icons->x += (parent->width-CENTER_ICONAREA_SIZE)/2;

    vps[QUICKSCREEN_BOTTOM] = *parent;
    vps[QUICKSCREEN_TOP] = *parent;
    /* depending on the space the top/buttom items use 1 or 2 lines */
    if (nb_lines < MIN_LINES)
        vert_lines = 1;
    else
        vert_lines = 2;
    vps[QUICKSCREEN_TOP].y = parent->y;
    vps[QUICKSCREEN_TOP].height = vps[QUICKSCREEN_BOTTOM].height
            = vert_lines*char_height;
    vps[QUICKSCREEN_BOTTOM].y
            = parent->y + parent->height - vps[QUICKSCREEN_BOTTOM].height;

    /* enough space vertically, so put a nice margin */
    if (nb_lines >= MAX_NEEDED_LINES)
    {
        vps[QUICKSCREEN_TOP].y += MARGIN;
        vps[QUICKSCREEN_BOTTOM].y -= MARGIN;
    }

    vp_icons->y = vps[QUICKSCREEN_TOP].y
            + vps[QUICKSCREEN_TOP].height;
    vp_icons->height = vps[QUICKSCREEN_BOTTOM].y - vp_icons->y;

    /* adjust the left/right items widths to fit the screen nicely */
    if (qs->items[QUICKSCREEN_LEFT])
    {
        s = P2STR(ID2P(qs->items[QUICKSCREEN_LEFT]->lang_id));
        left_width = display->getstringsize(s, NULL, NULL);
    }
    if (qs->items[QUICKSCREEN_RIGHT])
    {
        s = P2STR(ID2P(qs->items[QUICKSCREEN_RIGHT]->lang_id));
        right_width = display->getstringsize(s, NULL, NULL);
    }

    width = MAX(left_width, right_width);
    if (width*2 + vp_icons->width > parent->width)
    {   /* crop text viewports */
        width = (parent->width - vp_icons->width)/2;
    }
    else
    {   /* add more gap in icons vp */
        int excess = parent->width - vp_icons->width - width*2;
        if (excess > MARGIN*4)
        {
            pad = MARGIN;
            excess -= MARGIN*2;
        }
        vp_icons->x -= excess/2;
        vp_icons->width += excess;
    }

    vps[QUICKSCREEN_LEFT] = *parent;
    vps[QUICKSCREEN_LEFT].x = parent->x + pad;
    vps[QUICKSCREEN_LEFT].width = width;

    vps[QUICKSCREEN_RIGHT] = *parent;
    vps[QUICKSCREEN_RIGHT].x = parent->x + parent->width - width - pad;
    vps[QUICKSCREEN_RIGHT].width = width;

    vps[QUICKSCREEN_LEFT].height = vps[QUICKSCREEN_RIGHT].height
            = 2*char_height;

    vps[QUICKSCREEN_LEFT].y = vps[QUICKSCREEN_RIGHT].y
            = parent->y + (parent->height/2) - char_height;

    /* shrink the icons vp by a few pixels if there is room so the arrows
       aren't drawn right next to the text */
    if (vp_icons->width > CENTER_ICONAREA_SIZE*2)
    {
        vp_icons->width -= CENTER_ICONAREA_SIZE*2/3;
        vp_icons->x += CENTER_ICONAREA_SIZE*2/6;
    }
    if (vp_icons->height > CENTER_ICONAREA_SIZE*2)
    {
        vp_icons->height -= CENTER_ICONAREA_SIZE*2/3;
        vp_icons->y += CENTER_ICONAREA_SIZE*2/6;
    }

    /* text alignment */
    vps[QUICKSCREEN_LEFT].flags &= ~VP_FLAG_ALIGNMENT_MASK; /* left-aligned */
    vps[QUICKSCREEN_TOP].flags    |= VP_FLAG_ALIGN_CENTER;  /* centered */
    vps[QUICKSCREEN_BOTTOM].flags |= VP_FLAG_ALIGN_CENTER;  /* centered */
    vps[QUICKSCREEN_RIGHT].flags  &= ~VP_FLAG_ALIGNMENT_MASK;/* right aligned*/
    vps[QUICKSCREEN_RIGHT].flags  |= VP_FLAG_ALIGN_RIGHT;
}

static void gui_quickscreen_draw(struct gui_quickscreen *qs,
                                 enum screen_type screen)
{
    int i;
    char buf[MAX_PATH];
    unsigned const char *title, *value;
    int temp;
    struct screen *display = &screens[screen];
    struct viewport *parent = &qs->parent[screen];
    struct viewport *vps = qs->vps[screen];
    struct viewport *vp_icons = &qs->vp_icons[screen];
    struct viewport *last_vp;

    /* A base skin carrying the quickscreen tags draws this screen itself, and
     * this layout would land on top of it as a second, unstyled copy. Stand
     * down, but still say the values moved -- nothing else tells the skin. */
    if (sb_skin_draws_quickscreen(screen))
    {
        sb_skin_force_next_update();
        skin_mark_dirty(screen);
        return;
    }

    last_vp = display->set_viewport(parent);
    display->clear_viewport();

    for (i = 0; i < QUICKSCREEN_ITEM_COUNT; i++)
    {
        struct viewport *vp = &vps[i];
        if (!qs->items[i])
            continue;
        display->set_viewport(vp);

        title = P2STR(ID2P(qs->items[i]->lang_id));
        temp = option_value_as_int(qs->items[i]);
        value = option_get_valuestring(qs->items[i],
                                       buf, MAX_PATH, temp);

        if (viewport_get_nb_lines(vp) < 2)
        {
            char text[MAX_PATH];
            snprintf(text, MAX_PATH, "%s: %s", title, value);
            display->puts_scroll(0, 0, text);
        }
        else
        {
            display->puts_scroll(0, 0, title);
            display->puts_scroll(0, 1, value);
        }
    }
    /* draw the icons */
    display->set_viewport(vp_icons);

    if (qs->items[QUICKSCREEN_TOP] != NULL)
    {
        display->mono_bitmap(bitmap_icons_7x8[Icon_UpArrow],
            (vp_icons->width/2) - 4, 0, 7, 8);
    }
    if (qs->items[QUICKSCREEN_RIGHT] != NULL)
    {
        display->mono_bitmap(bitmap_icons_7x8[Icon_FastForward],
            vp_icons->width - 8, (vp_icons->height/2) - 4, 7, 8);
    }
    if (qs->items[QUICKSCREEN_LEFT] != NULL)
    {
        display->mono_bitmap(bitmap_icons_7x8[Icon_FastBackward],
            0, (vp_icons->height/2) - 4, 7, 8);
    }
    if (qs->items[QUICKSCREEN_BOTTOM] != NULL)
    {
        display->mono_bitmap(bitmap_icons_7x8[Icon_DownArrow],
            (vp_icons->width/2) - 4, vp_icons->height - 8, 7, 8);
    }

    display->set_viewport(parent);
    skin_mark_dirty(screen);
    display->set_viewport(last_vp);
}

static void quickscreen_draw_cb(unsigned short id, void *data, void *userdata);

/* Take the screen down: stop scrolling, drop the theme viewport, pop the
 * activity. Shared by the normal exit and the USB one, which has to run it
 * before the USB screen draws over us. */
static void quickscreen_cleanup(void *param)
{
    struct gui_quickscreen *qs = (struct gui_quickscreen *)param;

    remove_event_ex(GUI_EVENT_NEED_UI_UPDATE, quickscreen_draw_cb, qs);

    FOR_NB_SCREENS(i)
    {
        for (int j = 0; j < QUICKSCREEN_ITEM_COUNT; j++)
            screens[i].scroll_stop_viewport(&qs->vps[i][j]);
        viewportmanager_theme_undo(i,
                    !(qs->result & QUICKSCREEN_GOTO_SHORTCUTS_MENU));
    }

    if (qs->result & QUICKSCREEN_GOTO_SHORTCUTS_MENU)
        pop_current_activity_without_refresh(); /* no flash into Shortcuts */
    else
        pop_current_activity();
}

static void quickscreen_draw_cb(unsigned short id, void *data, void *userdata)
{
    (void)id;
    (void)data;

    FOR_NB_SCREENS(i)
        gui_quickscreen_draw((struct gui_quickscreen *)userdata, i);
}

static void talk_qs_option(const struct settings_list *opt, bool enqueue)
{
    if (!global_settings.talk_menu || !opt)
        return;

    if (enqueue)
        talk_id(opt->lang_id, enqueue);
    option_talk_value(opt, option_value_as_int(opt), enqueue);
}

/*
 * Does the actions associated to the given button if any
 *  - qs : the quickscreen
 *  - button : the key we are going to analyse
 * returns : true if the button corresponded to an action, false otherwise
 */
static bool gui_quickscreen_do_button(struct gui_quickscreen * qs, int button)
{
    int item;
    bool previous = false;
    switch(button)
    {
        case ACTION_QS_TOP:
            item = QUICKSCREEN_TOP;
            break;

        case ACTION_QS_LEFT:
            item = QUICKSCREEN_LEFT;
            previous = true;
            break;

        case ACTION_QS_DOWN:
            item = QUICKSCREEN_BOTTOM;
            previous = true;
            break;

        case ACTION_QS_RIGHT:
            item = QUICKSCREEN_RIGHT;
            break;

        default:
            return false;
    }

    if (qs->items[item] == NULL)
        return false;

    option_select_next_val(qs->items[item], previous, true);
    talk_qs_option(qs->items[item], false);
    return true;
}


static int gui_syncquickscreen_run(struct gui_quickscreen * qs, int button_enter, bool *usb)
{
    int button;
    /* To quit we need either :
     *  - a second press on the button that made us enter
     *  - an action taken while pressing the enter button,
     *    then release the enter button*/
    bool can_quit = false;

    qs->button_enter = button_enter;
    qs->result = QUICKSCREEN_OK;

    push_current_activity(ACTIVITY_QUICKSCREEN);

    FOR_NB_SCREENS(i)
    {
        screens[i].set_viewport(NULL);
        screens[i].scroll_stop();
        viewportmanager_theme_enable(i, true, &qs->parent[i]);
        quickscreen_fix_viewports(qs, i);
        gui_quickscreen_draw(qs, i);
    }
    add_event_ex(GUI_EVENT_NEED_UI_UPDATE, false, quickscreen_draw_cb, qs);
    *usb = false;
    /* Announce current selection on entering this screen. This is all
       queued up, but can be interrupted as soon as a setting is
       changed. */
    cond_talk_ids(VOICE_QUICKSCREEN);
    talk_qs_option(qs->items[QUICKSCREEN_TOP], true);
    if (qs->items[QUICKSCREEN_TOP] != qs->items[QUICKSCREEN_BOTTOM])
        talk_qs_option(qs->items[QUICKSCREEN_BOTTOM], true);
    talk_qs_option(qs->items[QUICKSCREEN_LEFT], true);
    if (qs->items[QUICKSCREEN_LEFT] != qs->items[QUICKSCREEN_RIGHT])
        talk_qs_option(qs->items[QUICKSCREEN_RIGHT], true);

    while (true) {
        button = get_action(CONTEXT_QUICKSCREEN, HZ/5);
        /* The USB screen runs inside the handler, so the quickscreen has to
         * take itself down first -- otherwise it is still the current
         * activity, with its theme viewport in place, underneath it. */
        if (default_event_handler_ex(button, quickscreen_cleanup, qs)
            == SYS_USB_CONNECTED)
        {
            *usb = true;
            return qs->result | QUICKSCREEN_IN_USB;
        }
        if (gui_quickscreen_do_button(qs, button))
        {
            qs->result |= QUICKSCREEN_CHANGED;
            can_quit = true;
            FOR_NB_SCREENS(i)
                gui_quickscreen_draw(qs, i);
        }
        else if (button == button_enter)
            can_quit = true;
        else if (button == ACTION_QS_VOLUP)
            adjust_volume(1);
        else if (button == ACTION_QS_VOLDOWN)
            adjust_volume(-1);
        else if (button == ACTION_STD_CONTEXT)
        {
            qs->result |= QUICKSCREEN_GOTO_SHORTCUTS_MENU;
            break;
        }
        if ((button == button_enter) && can_quit)
            break;

        if (button == ACTION_STD_CANCEL)
            break;

        /* A base skin may show a setting this screen just changed -- shuffle,
         * repeat, a brightness slider. Bypass the status bar's update_delay so
         * it agrees with the row the user is looking at. Only on a real press:
         * get_action() above times out five times a second, and forcing on
         * those would repaint the bar continuously for as long as the
         * quickscreen is open. */
        if (button != ACTION_NONE)
            sb_skin_force_next_update();
    }
    /* Notify that we're exiting this screen */
    cond_talk_ids_fq(VOICE_OK);
    quickscreen_cleanup(qs);

    return qs->result;
}

int quick_screen_quick(int button_enter)
{
    struct gui_quickscreen qs;
    bool usb = false;

    for (int i = 0; i < 4; ++i)
    {
        qs.items[i] = global_settings.qs_items[i];

        if (!is_setting_quickscreenable(qs.items[i]))
            qs.items[i] = NULL;
    }

    int ret = gui_syncquickscreen_run(&qs, button_enter, &usb);
    if (ret & QUICKSCREEN_CHANGED)
        settings_save();
    if (usb)
        return QUICKSCREEN_IN_USB;
    return ret & QUICKSCREEN_GOTO_SHORTCUTS_MENU ? QUICKSCREEN_GOTO_SHORTCUTS_MENU :
                                                   QUICKSCREEN_OK;
}

/* stuff to make the quickscreen configurable */
bool is_setting_quickscreenable(const struct settings_list *setting)
{
    if (!setting)
        return true;

    /* to keep things simple, only settings which have a lang_id set are ok */
    if (setting->lang_id < 0 || (setting->flags & F_BANFROMQS))
        return false;

    switch (setting->flags & F_T_MASK)
    {
        case F_T_BOOL:
            return true;
        case F_T_INT:
        case F_T_UINT:
            return (setting->RESERVED != NULL);
        default:
            return false;
    }
}
