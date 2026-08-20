/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/statusbar-skinned.c
 * Copyright (C) 2009 Thomas Martitz
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * The skinned status bar (.sbs): drives a skin file as the status bar,
 * including the title text and icon other screens set.
 ****************************************************************************/

#include "config.h"
#include "kernel.h"

#include "input/action.h"
#include "system.h"
#include "settings/settings.h"
#include "system/appevents.h"
#include "draw/screen_access.h"
#include "skin_parser.h"
#include "skin_buffer.h"
#include "skin_engine.h"
#include "wps_internals.h"
#include "draw/viewport.h"
#include "statusbar.h"
#include "statusbar_skinned.h"
#include "debug.h"
#include "font.h"
#include "draw/icon.h"
#include "draw/icon_bitmaps.h"
#include "widgets/option_select.h"
#include "string-extra.h"
#include "skin_albumart_color.h"
#include "button.h"                  /* button_queue_post -- see sb_busy_tick */
#include "led.h"
#include "system/activity.h"
#include "metadata/art_cache.h"
#include "database/tagcache.h"
#include "database/db_summary.h"
#include "files/file_index.h"

/* initial setup of wps_data  */
static int update_delay = DEFAULT_UPDATE_DELAY;

static bool sbs_has_title[NB_SCREENS];
static const char* sbs_title[NB_SCREENS];
static char sbs_persistent_title[NB_SCREENS][80];
static enum themable_icons sbs_icon[NB_SCREENS];
static bool sbs_loaded[NB_SCREENS] = { false };

void sb_set_info_vp(enum screen_type screen, OFFSETTYPE(char*) label);

bool sb_set_title_text(const char* title, enum themable_icons icon, enum screen_type screen)
{
    sbs_title[screen] = title;
    /* Icon_NOICON == -1 which the skin engine wants at position 1, so + 2 */
    sbs_icon[screen] = icon + 2;
    return sbs_has_title[screen];
}

bool sb_set_persistent_title(const char* title, enum themable_icons icon, enum screen_type screen)
{
    if (!title)
        return sb_set_title_text(title, icon, screen);

    strlcpy(sbs_persistent_title[screen], title, sizeof(*sbs_persistent_title));
    return sb_set_title_text((const char *) sbs_persistent_title[screen], icon, screen);
}

void sb_skin_has_title(enum screen_type screen)
{
    sbs_has_title[screen] = true;
}

const char* sb_get_title(enum screen_type screen)
{
    return sbs_has_title[screen] ? sbs_title[screen] : NULL;
}

const char* sb_get_persistent_title(enum screen_type screen)
{
    return (sbs_has_title[screen] &&
            sbs_title[screen] == sbs_persistent_title[screen]) ?
            sbs_title[screen] : NULL;

}

enum themable_icons sb_get_icon(enum screen_type screen)
{
    return sbs_has_title[screen] ? sbs_icon[screen] : Icon_NOICON + 2;
}

void sb_process(enum screen_type screen, struct wps_data *data, bool preprocess)
{
    if (preprocess)
    {
        sbs_loaded[screen] = false;
        sbs_has_title[screen] = false;
        viewportmanager_theme_enable(screen, false, NULL);
        return;
    }
    if (data->wps_loaded)
    {
        /* hide the sb's default viewport because it has nasty effect with stuff
        * not part of the statusbar,
        * hence .sbs's without any other vps are unsupported*/
        struct skin_viewport *vp = skin_find_item(VP_DEFAULT_LABEL_STRING, SKIN_FIND_VP, data);
        struct skin_element *tree = SKINOFFSETTOPTR(get_skin_buffer(data), data->tree);
        struct skin_element *next_vp = NULL;
        if (tree) next_vp = SKINOFFSETTOPTR(get_skin_buffer(data), tree->next);

        if (vp)
        {
            if (!next_vp)
            {    /* no second viewport, let parsing fail */
                return;
            }
            /* hide this viewport, forever */
            vp->hidden_flags = VP_NEVER_VISIBLE;
        }
        sb_set_info_vp(screen, VP_DEFAULT_LABEL);
        sbs_loaded[screen] = true;
    }
    viewportmanager_theme_undo(screen, false);
}

static OFFSETTYPE(char*) infovp_label[NB_SCREENS];
static OFFSETTYPE(char*) oldinfovp_label[NB_SCREENS];
void sb_set_info_vp(enum screen_type screen, OFFSETTYPE(char*) label)
{
    infovp_label[screen] = label;
}

struct viewport *sb_skin_get_info_vp(enum screen_type screen)
{
    if (sbs_loaded[screen] == false)
        return NULL;
    struct wps_data *data = skin_get_gwps(CUSTOM_STATUSBAR, screen)->data;
    struct skin_viewport *vp = NULL;
    char *label;
    if (oldinfovp_label[screen] &&
        (oldinfovp_label[screen] != infovp_label[screen]))
    {
        /* UI viewport changed, so force a redraw */
        oldinfovp_label[screen] = infovp_label[screen];
        viewportmanager_theme_enable(screen, false, NULL);
        viewportmanager_theme_undo(screen, true);
    }
    if (infovp_label[screen] == VP_DEFAULT_LABEL)
        label = VP_DEFAULT_LABEL_STRING;
    else
        label = SKINOFFSETTOPTR(get_skin_buffer(data), infovp_label[screen]);
    if (!label)
        return NULL;
    vp = skin_find_item(label, SKIN_FIND_UIVP, data);
    if (!vp)
        return NULL;
    if (vp->parsed_fontid == 1)
        vp->vp.font = screens[screen].getuifont();
    return &vp->vp;
}

bool sb_skin_draws_quickscreen(enum screen_type screen)
{
    struct wps_data *data = skin_get_gwps(CUSTOM_STATUSBAR, screen)->data;
    return data->wps_loaded && data->draws_quickscreen;
}

int sb_get_backdrop(enum screen_type screen)
{
    struct wps_data *data = skin_get_gwps(CUSTOM_STATUSBAR, screen)->data;
    if (data->wps_loaded)
        return data->backdrop_id;
    else
        return -1;
}
static bool force_waiting = false;
void sb_skin_update(enum screen_type screen, bool force)
{
    struct wps_data *data = skin_get_gwps(CUSTOM_STATUSBAR, screen)->data;
    static long next_update[NB_SCREENS] = {0};
    int i = screen;
    if (!data->wps_loaded)
        return;
    {
        static bool sb_was_recolouring[NB_SCREENS] = {false};
        bool sb_recolouring = dynamic_colors_needs_repaint() || dynamic_colors_pending();
        if (sb_recolouring)
        {
            force = true;
            sb_was_recolouring[i] = true;
        }
        else if (sb_was_recolouring[i])
        {
            force = true;
            sb_was_recolouring[i] = false;
        }
    }
    if (TIME_AFTER(current_tick, next_update[i]) || force || force_waiting)
    {
        force_waiting = false;
        /* currently, all remotes are readable without backlight
         * so still update those */
        if (lcd_active() || (i != SCREEN_MAIN))
        {
            if (force)
                skin_request_full_update(CUSTOM_STATUSBAR);
            skin_update(CUSTOM_STATUSBAR, screen, SKIN_REFRESH_NON_STATIC);
        }
        next_update[i] = current_tick + update_delay; /* don't update too often */
    }
}

void do_sbs_update_callback(unsigned short id, void *param)
{
    (void)id;
    (void)param;
    /* the WPS handles changing the actual id3 data in the id3 pointers
     * we imported, we just want a full update */
    skin_request_full_update(CUSTOM_STATUSBAR);
    force_waiting = true;
    /* force timeout in wps main loop, so that the update is instantly */
    button_queue_post(BUTTON_NONE, 0);
}

/* The four background passes behind %lb: the music database, the album index,
 * the album-art thumbnail cache and the document/image index. None was asked
 * for, none is visible, and any of them can be why a screen is slow to open --
 * so they share one indicator. Here rather than beside the tag because the busy
 * tick below needs the same question answered. */
bool sb_background_busy(void)
{
    return tagcache_is_busy() || db_summary_is_busy()
        || art_cache_is_busy() || file_index_is_busy();
}

/* Animate a busy indicator on a screen that is otherwise still.
 *
 * The status bar has no clock: sb_skin_update() is only reached from
 * GUI_EVENT_ACTIONREDRAW, which the UI sends when it draws something. So on an
 * idle list nothing samples a time-based tag, and a %la spinner sits on one
 * frame until the user moves. Posting an empty button breaks the action loop's
 * wait, and the redraw that follows samples the tag.
 *
 * That post is not a keypress. reset_poweroff_timer() and the backlight live in
 * button_tick()'s keypress path (firmware/drivers/button.c), not in
 * button_queue_post(), so a long database build still gets to sleep.
 *
 * The pace is a real choice, not a consequence: force_waiting bypasses
 * sb_skin_update()'s own next_update throttle, so this sets the bar's refresh
 * rate outright while it runs. update_delay is what that rate would otherwise
 * be, which makes it the cheapest pace that still animates -- about 7 frames a
 * second. A spinner with more frames than that drops some, which is invisible
 * when consecutive frames differ only slightly. */
static void sb_busy_tick(void)
{
    static long next_poke = 0;
    static bool was_busy = false;
    bool busy;

    if (TIME_BEFORE(current_tick, next_poke))
        return;
    next_poke = current_tick + update_delay;

    /* Nothing to animate on a bar that is not drawn, or a dark screen: the
     * update would be skipped anyway and the wakeup wasted. */
    if (!sbs_loaded[SCREEN_MAIN] || !lcd_active())
        return;

    /* Somebody is already driving the UI. The action loop is turning over and
     * the bar refreshes with it, so a wakeup here animates nothing and only
     * lengthens the queue the wheel's own events are waiting in -- which is
     * what made scrolling drag while the database was building.
     *
     * Before was_busy is touched, deliberately: a fall that happens while the
     * user is scrolling is not lost, it is just handled on the next idle tick. */
    if (!button_queue_empty())
        return;

    /* The union of the three indicators a skin can draw: %lh, %lb and %lw. */
    busy = led_read(HZ/2) || sb_background_busy() || ui_working();

    /* One poke after the last one, or the indicator stays on screen: the tag
     * turns false but nothing repaints the line it was drawn on, so the final
     * frame sits there until the user moves. */
    if (!busy && !was_busy)
        return;

    /* While it runs, deliberately not skin_request_full_update(): the indicator
     * is a dynamic tag, so the ordinary non-static refresh redraws it and the
     * dirty-rect flush transfers its rectangle alone. A full update repaints the
     * whole bar, backdrop composite included -- what a track change needs, and
     * far more than a spinner frame does. Asking for one every frame is what
     * made lists drag.
     *
     * The last poke is the exception and does need one. A line is only rewritten
     * when its content changed, and a tag that stops producing text is not a
     * change the engine notices -- the line simply comes out shorter and nothing
     * erases what the longer one left. Once, at the end of the work, a full
     * repaint costs nothing worth counting. */
    if (!busy)
        skin_request_full_update(CUSTOM_STATUSBAR);
    was_busy = busy;

    force_waiting = true;
    button_queue_post(BUTTON_NONE, 0);
}

void sb_skin_set_update_delay(int delay)
{
    update_delay = delay;
}

void sb_skin_force_next_update(void)
{
    force_waiting = true;
}

/* This creates and loads a ".sbs" based on the user settings for:
 *  - regular statusbar
 *  - colours
 *  - ui viewport
 *  - backdrop
 */
char* sb_create_from_settings(enum screen_type screen)
{
    static char buf[128];
    char *ptr, *ptr2;
    int len, remaining = sizeof(buf);
    int bar_position = statusbar_position(screen);
    ptr = buf;
    ptr[0] = '\0';

    /* setup the inbuilt statusbar */
    if (bar_position != STATUSBAR_OFF)
    {
        int y = 0, height = STATUSBAR_HEIGHT;
        if (bar_position == STATUSBAR_BOTTOM)
        {
            y = screens[screen].lcdheight - STATUSBAR_HEIGHT;
        }
        len = snprintf(ptr, remaining, "%%V(0,%d,-,%d,0)\n%%wi\n",
                       y, height);
        remaining -= len;
        ptr += len;
    }
    /* %Vi viewport, colours handled by the parser */
        ptr2 = global_settings.ui_vp_config;

    if (ptr2[0] && ptr2[0] != '-') /* from ui viewport setting */
    {
        char *comma = ptr;
        int param_count = 0;
        len = snprintf(ptr, remaining, "%%ax%%Vi(-,%s)\n", ptr2);
        /* The config put the colours at the end of the viewport,
         * they need to be stripped for the skin code though */
        do {
            param_count++;
            comma = strchr(comma+1, ',');

        } while (comma && param_count < 6);
        if (comma && strchr(comma+1, ','))
        {
            char *end = comma;
            char fg[8], bg[8];
            int i = 0;
            comma++;
            while (*comma != ',' && i < (int) sizeof(fg) - 1)
                fg[i++] = *comma++;
            fg[i] = '\0'; comma++; i=0;
            while (*comma != ')'  && i < (int) sizeof(bg) - 1)
                bg[i++] = *comma++;
            bg[i] = '\0';
            len += snprintf(end, remaining-len, ") %%Vf(%s) %%Vb(%s)\n", fg, bg);
        }
        else
        {
            ptr2[0] = '-';
            ptr2[1] = '\0';
        }
    }

    if (!ptr2[0] || ptr2[0] == '-')
    {
        int y = 0, height;
        switch (bar_position)
        {
            case STATUSBAR_TOP:
                y = STATUSBAR_HEIGHT;
                /* Fallthrough */
            case STATUSBAR_BOTTOM:
                height = screens[screen].lcdheight - STATUSBAR_HEIGHT;
                break;
            default:
                height = screens[screen].lcdheight;
        }
        len = snprintf(ptr, remaining, "%%ax%%Vi(-,0,%d,-,%d,1)\n",
                       y, height);
    }
    return buf;
}

void sb_skin_init(void)
{
    FOR_NB_SCREENS(i)
    {
        oldinfovp_label[i] = VP_DEFAULT_LABEL;
    }
    tick_add_task(sb_busy_tick);
}

