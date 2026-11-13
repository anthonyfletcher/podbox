/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/usb_screen.c
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * The USB connection screen shown while the device is mounted: the theme's
 * "usb image" with the message under it, or without one a plain message box
 * over whatever was on screen; plus HID keypad handling, and the wheel as
 * volume while the player is a sound card.
 *
 * The skin never draws this screen. See usb_screen_fix_viewports().
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
#include "system/volume.h"      /* adjust_volume */
#include "system/app_util.h"    /* core_load_bmp */
#include "system/debug_log.h"
#include "core_alloc.h"
#include "skin/skin_albumart_color.h" /* dynamic_colors_resolve */

int usb_keypad_mode;
static bool usb_hid;

/* The theme's image, loaded with the theme because the disk is the host's
 * for the whole session. Movable: its address is fetched at each draw, never
 * kept. */
static int usb_image = CLB_ALOC_ERR;
static struct bitmap usb_image_bm;

void usb_screen_image_init(void)
{
    const char *file = global_settings.usb_image_file;
    ssize_t size;

    usb_image = core_free(usb_image);
    if (!file[0] || file[0] == '-')
        return;
    usb_image = core_load_bmp(file, &usb_image_bm, FORMAT_MONO, &size, NULL);
    if (usb_image <= 0)
        debug_log(DEBUG_LOG_SKIN, "usb image %s: cannot load", file);
    else if (usb_image_bm.width > LCD_WIDTH
             || usb_image_bm.height > LCD_HEIGHT)
    {
        debug_log(DEBUG_LOG_SKIN, "usb image %s: larger than the screen",
                  file);
        usb_image = core_free(usb_image);
    }
}

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
#ifdef USB_ENABLE_AUDIO
            /* The player is the computer's sound card: the wheel sets the
             * codec's volume, as the host's own volume requests do; while
             * the host has it muted, the volume unmuting brings back. */
            case BUTTON_SCROLL_FWD:
            case BUTTON_SCROLL_FWD|BUTTON_REPEAT:
            case BUTTON_SCROLL_BACK:
            case BUTTON_SCROLL_BACK|BUTTON_REPEAT:
                if (!usb_audio_get_active())
                    break;
                if (usb_audio_get_muted())
                    usb_audio_set_unmute_volume(volume_stepped(
                        usb_audio_get_unmute_volume(),
                        (button & BUTTON_SCROLL_FWD) ? 1 : -1));
                else
                    adjust_volume((button & BUTTON_SCROLL_FWD) ? 1 : -1);
                break;
#endif
        }

    }

    return 0;
}

#define MODE_NAME_LEN 32

struct usb_screen_vps_t
{
    struct viewport parent;
    struct viewport title;
};

static void usb_screen_fix_viewports(struct screen *screen,
        struct usb_screen_vps_t *usb_screen_vps)
{
    struct viewport *parent = &usb_screen_vps->parent;

    /* No theme for the whole USB session: full screen, no SBS, no backdrop, and
     * no skin update at any point. The message below is plain fill and text
     * drawing, which is all this screen does.
     *
     * Themes cannot draw the USB screen here, deliberately: the connect window
     * is the wrong place to spend CPU. Cooperative scheduling means a long
     * non-yielding stretch
     * starves the USB thread whatever its priority, and the host has
     * SET_ADDRESS outstanding -- when it gives up, the port wedges and only a
     * physical unplug clears it. Treat any work added between here and
     * usb_acknowledge() as USB-critical, and note that threads owing a
     * SYS_USB_CONNECTED ack delay the handover the same way. */
    viewportmanager_theme_enable(screen->screen_type, false, parent);

    if (usb_hid)
    {
        struct viewport *title = &usb_screen_vps->title;
        *title = *parent;
        title->height = font_get(parent->font)->height;

        int i =0, langid = LANG_USB_KEYPAD_MODE;
        while (langid >= 0) /* ensure the USB mode strings get cached */
        {
            font_getstringsize(str(langid), NULL, NULL, title->font);
            langid = keypad_mode_name_get(i++);
        }
    }
}

/* The image centred on a screen filled with the background, the message's two
 * lines under it, and its dark pixels in the foreground: both colours as the
 * dynamic colours give them. The whole screen is sent, where a splash sends
 * only its box. */
static void usb_image_draw(struct screen *screen)
{
    static const int lines[] = { LANG_USB_CONNECTED,
                                 LANG_USB_EJECT_BEFORE_DISCONNECT };
    struct viewport vp, *last;
    int w, h, y;

    /* Trap: viewport_set_fullscreen() reads buffer before setting it */
    vp.buffer = NULL;                   /* the default framebuffer */
    vp.flags  = VP_DEFAULT_FLAGS;
    viewport_set_fullscreen(&vp, screen->screen_type);
    last = screen->set_viewport(&vp);
    h = font_get(vp.font)->height;

    screen->set_drawmode(DRMODE_SOLID);
    screen->set_foreground(dynamic_colors_resolve(global_settings.bg_color));
    screen->fillrect(0, 0, vp.width, vp.height);
    screen->set_foreground(dynamic_colors_resolve(global_settings.fg_color));
    screen->set_drawmode(DRMODE_FG);

    y = MAX(0, (vp.height - usb_image_bm.height - h / 2 - 2 * h) / 2);
    screen->mono_bitmap(core_get_data(usb_image),
                        (vp.width - usb_image_bm.width) / 2, y,
                        usb_image_bm.width, usb_image_bm.height);
    y += usb_image_bm.height + h / 2;
    for (unsigned i = 0; i < ARRAYLEN(lines); i++, y += h)
    {
        const char *s = str(lines[i]);

        font_getstringsize(s, &w, NULL, vp.font);
        screen->putsxy((vp.width - w) / 2, y, s);
    }

    screen->set_drawmode(DRMODE_SOLID);
    screen->update();
    screen->set_viewport(last);
}

/* Without an image, the message in a centred box, the same one every other
 * modal in the UI uses, drawn over whatever was on screen: a device left
 * plugged in wakes with fragments of the screen it came from around it.
 *
 * Plain fill, bitmap and text drawing, never the skin engine or the disk, so
 * this is safe to repeat after the storage handover. */
static void usb_screens_draw(void)
{
    FOR_NB_SCREENS(i)
        screens[i].backlight_on();

    if (usb_image > 0)
    {
        FOR_NB_SCREENS(i)
            usb_image_draw(&screens[i]);
    }
    else
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
