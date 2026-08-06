/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/usb_screen.c
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * The USB connection screen shown while the device is mounted: a plain message
 * box over whatever was on screen, plus HID keypad handling.
 *
 * Themes do not get to draw this screen. See usb_screen_fix_viewports().
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include "kernel.h"
#include "input/action.h"
#include "font.h"
#include "lang.h"
#include "usb.h"
#include "usb_core.h"
#include "input/usb_keymaps.h"
#include "settings/settings.h"
#include "led.h"
#include "system/appevents.h"
#include "usb_screen.h"
#include "draw/viewport.h"
#include "skin/skin_engine.h"   /* skin_inhibit_flush, skin_flush_dirty */
#include "playlist/playlist.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "widgets/splash.h"

int usb_keypad_mode;
static bool usb_hid;

static int handle_usb_events(void)
{

    /* Don't return until we get SYS_USB_DISCONNECTED or SYS_TIMEOUT */
    while(1)
    {
        int button;
        if (usb_hid)
        {
            button = get_hid_usb_action();

            /* On mode change, we need to refresh the screen */
            if (button == ACTION_USB_HID_MODE_SWITCH_NEXT ||
                    button == ACTION_USB_HID_MODE_SWITCH_PREV)
            {
                break;
            }
        }
        else
        {
            button = button_get_w_tmo(HZ/2);
            /* hid emits the event in get_action */
            send_event(GUI_EVENT_ACTIONUPDATE, NULL);
        }

        switch(button)
        {
            case SYS_USB_DISCONNECTED:
                return 1;
            case SYS_CHARGER_DISCONNECTED:
                reset_runtime();
                break;
            case SYS_TIMEOUT:
                break;
        }

    }

    return 0;
}

#define MODE_NAME_LEN 32

struct usb_screen_vps_t
{
    struct viewport parent;
    struct viewport logo;
    struct viewport title;
};

static void usb_screen_fix_viewports(struct screen *screen,
        struct usb_screen_vps_t *usb_screen_vps)
{
    int logo_width, logo_height;
    struct viewport *parent = &usb_screen_vps->parent;
    struct viewport *logo = &usb_screen_vps->logo;

    /* Vestigial: nothing is drawn into the logo viewport. The literals stay
     * because the HID title viewport is positioned relative to them and is
     * passed to scroll_stop_viewport(), so the geometry still has to come out
     * the same. Left intact rather than unpicked -- this screen is delicate on
     * PP502x, see the note below. */
    logo_width  = 176;   /* dimensions of the logo bitmap this replaced */
    logo_height = 48;

    /* No theme for the whole USB session: full screen, no SBS, no backdrop, and
     * no skin update at any point. The message below is plain fill and text
     * drawing, which is all this screen does.
     *
     * Themes cannot draw the USB screen here, deliberately. Rendering the .sbs
     * once before the handover was tried and reverted: it never worked reliably
     * from every entry screen, and the connect window is the wrong place to
     * spend CPU. Cooperative scheduling means a long non-yielding stretch
     * starves the USB thread whatever its priority, and the host has
     * SET_ADDRESS outstanding -- when it gives up, the port wedges and only a
     * physical unplug clears it. Treat any work added between here and
     * usb_acknowledge() as USB-critical, and note that threads owing a
     * SYS_USB_CONNECTED ack delay the handover the same way. */
    viewportmanager_theme_enable(screen->screen_type, false, parent);

    if (logo_width  > parent->width)
        logo_width  = parent->width;
    if (logo_height > parent->height)
        logo_height = parent->height;

    *logo = *parent;
    logo->x = parent->x + parent->width - logo_width;
    logo->y = parent->y + (parent->height - logo_height) / 2;
    logo->width = logo_width;
    logo->height = logo_height;

    if (usb_hid)
    {
        struct viewport *title = &usb_screen_vps->title;
        int char_height = font_get(parent->font)->height;
        *title = *parent;
        title->y = logo->y + logo->height + char_height;
        title->height = char_height;
        /* try to fit logo and title to parent */
        if (parent->y + parent->height < title->y + title->height)
        {
            logo->y = parent->y;
            title->y = parent->y + logo->height;
        }

        int i =0, langid = LANG_USB_KEYPAD_MODE;
        while (langid >= 0) /* ensure the USB mode strings get cached */
        {
            font_getstringsize(str(langid), NULL, NULL, title->font);
            langid = keypad_mode_name_get(i++);
        }
    }
}

/* The message in a centred box, the same one every other modal in the UI uses,
 * drawn over whatever was on screen.
 *
 * The screen is not blanked first, and attempts to blank it belong here only if
 * they are known to work: clearing the full-screen parent viewport and dropping
 * the backdrop before clear_display() were both tried on hardware and neither
 * blanked anything. The cost of leaving it is visible on wake -- a device left
 * plugged in comes back with fragments of the screen it came from around the
 * message.
 *
 * Plain fill and text drawing, never the skin engine, so this is safe to repeat
 * after the storage handover. */
static void usb_screens_draw(void)
{
    FOR_NB_SCREENS(i)
        screens[i].backlight_on();

    splashf(0, "%s\n%s", str(LANG_USB_CONNECTED),
                         str(LANG_USB_EJECT_BEFORE_DISCONNECT));
}

void gui_usb_screen_run(bool early_usb, intptr_t seqnum)
{

    struct usb_screen_vps_t usb_screen_vps_ar[NB_SCREENS];

    push_current_activity(ACTIVITY_USBSCREEN);

    usb_hid = global_settings.usb_hid;
    usb_keypad_mode = global_settings.usb_keypad_mode;

    FOR_NB_SCREENS(i)
    {
        struct screen *screen = &screens[i];
        /* we might be coming from anywhere, and the originating screen
         * can't be practically expected to cleanup the UI because
         * we're invoked via default_event_handler(), therefore we make a
         * generic cleanup here */
        screen->set_viewport(NULL);
        screen->scroll_stop();
        usb_screen_fix_viewports(screen, &usb_screen_vps_ar[i]);
    }

    /* Draw the message once here -- before the fonts are closed and before the
     * mass-storage handoff -- so its glyphs get cached while the fonts are
     * still open and the app still owns storage. */
    usb_screens_draw();

    /* Then let the USB thread run. It is raised to PRIORITY_REALTIME the moment
     * a host is seen (usb.c, usb_set_host_present), but scheduling is
     * cooperative: priority only chooses among ready threads at a switch point,
     * so the full-screen flush above starves it for its whole duration -- with
     * SET_ADDRESS outstanding on the host.
     *
     * Trap: not before the scroll_stop() above. Until then the scrolling lines
     * of the screen we came from are still registered, and the scroll thread
     * paints them into the framebuffer off its own timer, on top of whatever is
     * there. */
    yield();

    if(!early_usb)
    {
        /* The font system leaves the .fnt fd's open, so we need for force close them all */
        font_disable_all();
    }

    usb_acknowledge(SYS_USB_CONNECTED_ACK, seqnum);

    while (1)
    {
        if (handle_usb_events())
            break;
        /* Reached only on a USB-HID keypad mode switch; repaint (the message's
         * glyphs are already cached from the pre-handoff draw above). */
        usb_screens_draw();
    }

    FOR_NB_SCREENS(i)
    {
        const struct viewport* vp = NULL;

        vp = usb_hid ? &usb_screen_vps_ar[i].title : NULL;
        if (vp)
            screens[i].scroll_stop_viewport(vp);
    }
    if (global_settings.usb_keypad_mode != usb_keypad_mode)
    {
        global_settings.usb_keypad_mode = usb_keypad_mode;
        settings_save();
    }


    /* One repaint on the way out, not a burst of them. Everything from here to
     * the flush below rebuilds the theme -- fonts reopened and reloaded,
     * settings re-applied, the theme stack popped -- and popping the stack
     * fires GUI_EVENT_ACTIONREDRAW, which would otherwise draw and transfer the
     * status bar on its own before the screen underneath has been redrawn.
     *
     * This holds off skin flushes only. playlist_resume() opens with its own
     * "Loading..." splash, which draws and flushes directly and still shows. */
    skin_inhibit_flush(true);

    if(!early_usb)
    {
        font_enable_all();
        /* Not pretty, reload all settings so fonts are loaded again correctly */
        settings_apply(true);
        /* Reload playlist */
        playlist_resume();
    }

    FOR_NB_SCREENS(i)
    {
        screens[i].backlight_on();
        viewportmanager_theme_undo(i, false);
    }

    skin_inhibit_flush(false);
    skin_flush_dirty();

    pop_current_activity();
}
