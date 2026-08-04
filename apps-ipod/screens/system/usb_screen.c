/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/usb_screen.c
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * The USB connection screen shown while the device is mounted: the theme's own
 * screen where it has one, the eject message otherwise, plus HID keypad
 * handling.
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
#include "skin/skin_engine.h"
#include "skin/statusbar_skinned.h"   /* sb_skin_update */
#include "playlist/playlist.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "widgets/splash.h"

/* Below this, in either direction, the theme's UI viewport is taken as a
 * suppression rather than a place to draw: the convention for "this theme draws
 * its own USB screen" is a 1x1 %Vi, and nothing smaller than a line of text
 * could hold the message anyway. */
#define USB_UIVP_MIN 16

int usb_keypad_mode;
static bool usb_hid;

/* Whether the theme rendered its own USB screen and suppressed the UI viewport,
 * i.e. the built-in message must not be drawn over it. */
static bool usb_screen_skinned;


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

    /* Vestigial: nothing is drawn into the logo viewport, because the screen
     * is a single full-screen bitmap (usb_screens_draw below). The literals
     * stay because the HID title viewport is positioned relative to them and
     * is passed to scroll_stop_viewport(), so the geometry still has to come
     * out the same. Left intact rather than unpicked -- this screen is
     * delicate on PP502x, see the note below. */
    logo_width  = 176;   /* dimensions of the logo bitmap this replaced */
    logo_height = 48;

    /* Tear the theme down for the rest of the USB session: full screen, no SBS,
     * no backdrop, and above all no skin update running once the handover has
     * happened. A repaint through the skin engine reaches for backdrops, covers
     * and fonts, and after the handover those live on a volume the app no
     * longer owns.
     *
     * A theme that draws its own USB screen has already been rendered once by
     * the caller, before this point, while the app still owns storage.
     *
     * This screen is delicate on PP502x and the failure is expensive -- the
     * host wedges and only a physical unplug clears it -- so treat any work
     * added between here and usb_acknowledge() as USB-critical. An earlier
     * revision read the same symptom as "the theme cannot be drawn at all";
     * the cause was threads that owed a SYS_USB_CONNECTED ack and did not send
     * it, which delays the handover the same way. */
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

/* The built-in "eject before disconnecting" message: a centred box, the same
 * one every other modal in the UI uses.
 *
 * With a theme up it goes straight over whatever the SBS rendered, so the
 * theme's chrome survives underneath -- and a theme that draws the whole screen
 * itself has suppressed the UI viewport, so nothing is drawn at all. Without a
 * theme there is nothing underneath worth keeping (early USB still shows the
 * boot logo), so the screen is blanked to the background first.
 *
 * Plain fill and text drawing, never the skin engine, so this is safe to repeat
 * after the storage handover. */
static void usb_screens_draw(struct usb_screen_vps_t *usb_screen_vps_ar,
                             bool themed)
{
    if (usb_screen_skinned)
        return;

    FOR_NB_SCREENS(i)
    {
        struct screen *screen = &screens[i];

        screen->backlight_on();
        if (themed)
            continue;

        struct viewport *last_vp =
            screen->set_viewport(&usb_screen_vps_ar[i].parent);
        screen->clear_viewport();
        screen->update_viewport();
        screen->set_viewport(last_vp);
    }

    splash(0, ID2P(LANG_USB_EJECT_BEFORE_DISCONNECT));
}

void gui_usb_screen_run(bool early_usb, intptr_t seqnum)
{

    struct usb_screen_vps_t usb_screen_vps_ar[NB_SCREENS];

    push_current_activity(ACTIVITY_USBSCREEN);

    usb_hid = global_settings.usb_hid;
    usb_keypad_mode = global_settings.usb_keypad_mode;

    /* Render the theme once, so a %cs 21 branch gets its screen.
     *
     * Here and only here: the app still owns storage and the fonts are still
     * open, which is what the skin engine needs, and the theme is torn down
     * immediately afterwards so nothing repaints through it for the rest of the
     * session. A skin update running after the handover would reach for a
     * backdrop, a cover or a font on a volume the app no longer owns.
     *
     * Unconditional, because that is the contract themes were written against:
     * the core draws its message afterwards, and a theme drawing its own screen
     * says so by shrinking the UI viewport (%VI to a 1x1 %Vi). Gating this on a
     * setting instead would silently drop the USB screen of every theme that
     * never heard of the setting. */
    bool themed = false;

    usb_screen_skinned = false;
    FOR_NB_SCREENS(i)
    {
        if (early_usb || !viewportmanager_theme_enabled(i))
            continue;
        themed = true;

        /* Backlight first: skin_update() is gated on lcd_active(), so a device
         * plugged in with the screen off would draw nothing at all.
         *
         * And flush explicitly. sb_skin_update() only draws -- the transfer to
         * the LCD is viewportmanager_update()'s job, and that runs off the event
         * loop this screen is about to leave. Without this the screen appears
         * the next time anything else flushes, which on a device left plugged in
         * means when it is woken. */
        screens[i].backlight_on();
        sb_skin_update(i, true);
        screens[i].update();

        /* Only now: the UI viewport a conditional %VI selects is set as a side
         * effect of rendering, so asking before this would read the previous
         * screen's answer. */
        struct viewport ui;
        viewport_set_defaults(&ui, i);
        if (ui.width < USB_UIVP_MIN || ui.height < USB_UIVP_MIN)
            usb_screen_skinned = true;
    }

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
    usb_screens_draw(usb_screen_vps_ar, themed);

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
        usb_screens_draw(usb_screen_vps_ar, themed);
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

    pop_current_activity();
}
