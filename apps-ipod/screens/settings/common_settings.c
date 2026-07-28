/***************************************************************************
 * Original code from RockBox
 * was: apps/menus/menu_common.c
 * Copyright (C) 2008 Dan Everton
 * GNU General Public License (version 2+)
 *
 * Helpers shared by the menu definitions, chiefly the "do you want to
 * apply now" prompts.
 ****************************************************************************/

#include <stddef.h>
#include <limits.h>
#include "config.h"
#include "input/action.h"
#include "widgets/menu.h"
#include "common_settings.h"
#include "audio/pcmbuf.h"

/* Use this callback if your menu adjusts DSP settings. */
int lowlatency_callback(int action,
                        const struct menu_item_ex *this_item,
                        struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_ENTER_MENUITEM: /* on entering an item */
            pcmbuf_set_low_latency(true);
            break;
        case ACTION_EXIT_MENUITEM: /* on exit */
            pcmbuf_set_low_latency(false);
            break;
    }
    return action;
}
