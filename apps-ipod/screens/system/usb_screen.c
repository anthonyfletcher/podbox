/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/usb_screen.c
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * The USB connection screen shown while the device is mounted, including
 * the logo or skinned variant and HID keypad handling.
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
#include "playlist/playlist.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "draw/icon_bitmaps.h"

/* The USB (eject) screen is a full-screen bitmap plus caption, drawn directly
 * rather than through the skin engine. The skin engine must not run during the
 * USB screen on PP502x -- it breaks enumeration (see the guard in
 * usb_screen_fix_viewports()). */
#define HAVE_ROCKPOD_USB_SCREEN
#include "bitmaps/podboxusb.h"
#include "bitmaps/native/podbox_colors.h"


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

    /* Vestigial: nothing is drawn into the logo viewport, because the screen
     * is a single full-screen bitmap (usb_screens_draw below). The literals
     * stay because the HID title viewport is positioned relative to them and
     * is passed to scroll_stop_viewport(), so the geometry still has to come
     * out the same. Left intact rather than unpicked -- this screen is
     * delicate on PP502x, see the note below. */
    logo_width  = 176;   /* dimensions of the logo bitmap this replaced */
    logo_height = 48;

    /* Draw the USB screen WITHOUT the theme (full screen, no SBS/backdrop).
     *
     * This MUST stay disabled. On the PP502x (iPod Video) target, running the
     * theme/skin engine during the USB screen -- even a single render done
     * before the mass-storage handoff -- destabilises the connection: the
     * device flaps between the menu and the eject screen and never enumerates
     * on the host (you end up in disk mode). Empirically: theme enabled == USB
     * broken, theme disabled == USB solid. So the USB screen is deliberately
     * kept theme-free. The cost is a plain USB logo instead of a theme's custom
     * USB screen -- that is the price of a reliable connection, and intentional.
     * The theme's Eject graphic is rendered by the skin engine (custom font
     * glyphs), so it cannot be shown here without re-enabling the theme. */
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

static void usb_screens_draw(struct usb_screen_vps_t *usb_screen_vps_ar)
{
    struct viewport *last_vp;
    FOR_NB_SCREENS(i)
    {
        struct screen *screen = &screens[i];
        struct viewport *parent = &usb_screen_vps_ar[i].parent;

        last_vp = screen->set_viewport(parent);
        screen->clear_viewport();
        screen->backlight_on();

        if (i == SCREEN_MAIN)
        {
            struct viewport caption = *parent;
            const unsigned char *msg = str(LANG_USB_EJECT_BEFORE_DISCONNECT);
            int tw, th;

            /* full-screen background art */
            screen->bmp(&bm_podboxusb, 0, 0);

            /* caption (#f9f9f9) centred horizontally, top edge at y = 195,
             * drawn transparently over the art in the theme's bold UI font */
            caption.font = font_get_ui_bold();
            caption.drawmode = DRMODE_FG;
            caption.fg_pattern = PODBOX_COLOR_FG;
            screen->set_viewport(&caption);
            screen->getstringsize(msg, &tw, &th);
            screen->putsxy((screen->lcdwidth - tw) / 2, 195, msg);
        }

        screen->set_viewport(last_vp);
        screen->update_viewport();
    }
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

    /* Draw the USB screen once here -- before the fonts are closed and before
     * the mass-storage handoff -- so the caption renders (and its glyphs get
     * cached) while the fonts are still open and the app still owns storage.
     * This is plain bitmap + text drawing, no skin engine, so it is USB-safe. */
    usb_screens_draw(usb_screen_vps_ar);

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
        /* Reached only on a USB-HID keypad mode switch; repaint (the caption's
         * glyphs are already cached from the pre-handoff draw above). */
        usb_screens_draw(usb_screen_vps_ar);
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
