/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * The deck: crunch the log once, then flip through the cards built from it.
 *
 * This owns the screen outright -- no status bar, no themed viewport, no
 * list widget. Every card is drawn edge to edge by pv_cards.c, so the theme
 * is switched off on the way in and restored on the way out.
 *
 * It also owns the working memory for as long as it is up. The aggregate
 * tables live in the borrowed app buffer and the cards read them directly
 * (pv_stats_top), so the buffer is CLAIMED rather than borrowed transiently:
 * the tables must outlive the build and stay put until the last card has
 * been drawn.
 *
 * Raw button codes rather than an action context, deliberately. The deck's
 * gestures answer to no existing context, and inventing one for a single
 * screen would be more machinery than the screen is.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "kernel.h"
#include "button.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "settings/settings.h"   /* ID2P */
#include "system/activity.h"
#include "system/app_buffer.h"
#include "system/shutdown.h"
#include "pv_paint.h"
#include "widgets/splash.h"
#include "widgets/menu.h"        /* do_menu, MENUITEM_STRINGLIST */
#include "draw/viewport.h"
#include "root_menu.h"           /* GO_TO_*, MENU_ATTACHED_USB */
#include "pv_art.h"
#include "pv_stats.h"
#include "pv_cards.h"
#include "pv_badges_ui.h"
#include "pv_export.h"
#include "pv_year.h"
#include "playback_viewer.h"

/* Hold-Menu menu, the same gesture the text and image viewers use.
 *
 * Saving lives here rather than on a button because every button on this
 * screen is navigation: a card is a whole screenful and the wheel is under
 * the thumb, so anything bound to a press is something a stray touch will
 * eventually do by accident. Writing a file is not that kind of action.
 *
 * The theme comes back just for the menu chrome, the way the other viewers
 * do it, and the screen is handed back to our own drawing on the way out. */
static int pv_menu(int card, const struct pv_totals *t)
{
    enum { PVM_SAVE_CARD = 0, PVM_SAVE_ALL };
    int result;

    MENUITEM_STRINGLIST(menu, ID2P(LANG_SPUN), NULL,
                        ID2P(LANG_PV_SAVE_CARD),
                        ID2P(LANG_PV_SAVE_ALL));

    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    push_current_activity(ACTIVITY_CONTEXTMENU);
    result = do_menu(&menu, NULL, NULL, false);
    pop_current_activity();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    lcd_set_viewport(NULL);

    if (result == PVM_SAVE_CARD)
    {
        /* Redrawn in export mode first, so the picture is of the finished
         * card rather than of the menu that was just dismissed -- or of a
         * number still counting up. */
        pv_set_export(true);
        pv_cards_draw(card, 0, t);
        pv_set_export(false);
        pv_export_card();
    }
    else if (result == PVM_SAVE_ALL)
    {
        pv_export_deck(t);
    }

    return result;
}

int playback_viewer_screen(void)
{
    struct pv_totals totals;
    enum pv_build_result r;
    void *buf;
    size_t bufsz;
    int card = 0, btn = 0;
    int ret = GO_TO_PREVIOUS;
    bool held = false;      /* the Menu hold that opened the menu is still down */

    buf = app_claim_buffer(&bufsz, "playback viewer");

    /* Two consumers share this, and dividing it is the screen's job because
     * the screen is the only thing that knows both exist. The thumbnails take
     * a fixed slice off the front; the model sizes itself to what is left. */
    pv_art_init(buf, bufsz);
    buf = (char *)buf + PV_ART_BYTES;
    bufsz -= PV_ART_BYTES;

    splash(0, ID2P(LANG_WAIT));
    r = pv_stats_build(buf, bufsz, &totals);

    if (r != PV_BUILD_OK)
    {
        /* Nothing to show is the common case on a device that has never had
         * logging on, so say what to do about it rather than just failing. */
        if (r == PV_BUILD_NO_LOG)
            splash(HZ * 3, ID2P(LANG_PV_NO_LOG));
        else
            splash(HZ * 2, ID2P(LANG_PV_NO_MEMORY));
        app_release_buffer("playback viewer");
        return ret;
    }

    push_current_activity(ACTIVITY_PLAYBACKVIEWER);
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, false, NULL);
    lcd_set_viewport(NULL);

    pv_paint_init();
    button_clear_queue();

    /* A card returns the button that interrupted its animation, so the loop
     * only asks the queue when the last card ran to completion. That is what
     * lets the wheel flip through the deck at its own speed rather than at
     * the speed of the animations. */
    btn = pv_cards_draw(card, 0, &totals);

    for (;;)
    {
        int step;

        if (btn == 0)
            btn = button_get(true);

        if (btn == SYS_USB_CONNECTED)
        {
            /* Acknowledge it rather than swallowing it: an unacknowledged
             * handover is what the host reports as a malfunctioning device. */
            default_event_handler(btn);
            ret = GO_TO_ROOT;
            break;
        }
        if (btn == SYS_POWEROFF)
        {
            default_event_handler(btn);
            break;
        }

        step = pv_nav_step(btn);
        if (step > 0 && card < PV_CARD_COUNT - 1)
        {
            card++;
            btn = pv_cards_draw(card, +1, &totals);
            continue;
        }
        if (step < 0 && card > 0)
        {
            card--;
            btn = pv_cards_draw(card, -1, &totals);
            continue;
        }
        /* The year card is the one card that is also a way in: SELECT hands
         * the screen to the week browser until it hands it back. */
        if ((btn & ~BUTTON_REPEAT) == BUTTON_SELECT
            && card == PV_CARD_YEAR && pv_year_available())
        {
            if (pv_year_browse(pv_cards_theme(PV_CARD_YEAR), &totals)
                == SYS_USB_CONNECTED)
            {
                default_event_handler(SYS_USB_CONNECTED);
                ret = GO_TO_ROOT;
                break;
            }
            btn = pv_cards_draw(card, 0, &totals);
            continue;
        }

        /* So is the achievements card. */
        if ((btn & ~BUTTON_REPEAT) == BUTTON_SELECT && card == PV_CARD_ACH)
        {
            if (pv_badges_browse(&totals) == SYS_USB_CONNECTED)
            {
                default_event_handler(SYS_USB_CONNECTED);
                ret = GO_TO_ROOT;
                break;
            }
            btn = pv_cards_draw(card, 0, &totals);
            continue;
        }

        /* Menu held opens the menu; Menu pressed and let go leaves the deck.
         * Telling them apart means waiting for the release, so the press
         * itself does nothing and 'held' remembers whether a menu happened in
         * between -- otherwise the release that ends the hold would also be
         * read as "leave". */
        if (btn == (BUTTON_MENU | BUTTON_REPEAT))
        {
            if (pv_menu(card, &totals) == MENU_ATTACHED_USB)
            {
                ret = GO_TO_ROOT;
                break;
            }
            button_clear_queue();
            /* Asking the keypad, rather than assuming: the menu usually eats
             * the release as well, and a stuck 'held' would swallow the next
             * press to leave. */
            held = (button_status() & BUTTON_MENU) != 0;
            btn = pv_cards_draw(card, 0, &totals);
            continue;
        }
        if (btn == (BUTTON_MENU | BUTTON_REL))
        {
            if (!held)
                break;
            held = false;
        }

        btn = 0;
    }

    pv_paint_done();
    lcd_setfont(FONT_UI);
    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, false);
    pop_current_activity();
    app_release_buffer("playback viewer");

    return ret;
}
