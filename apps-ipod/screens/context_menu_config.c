/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Appearance > Menus > Context Menus: which rows each kind of context menu
 * shows, one screen per kind.
 *
 * Hide-only. Which rows a kind has, their order and how a hidden row is saved
 * are context_menu.c's; this screen turns the bits on and off. A row can also
 * be hidden from the menu itself, with a long press on it -- this is where it
 * comes back.
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include "config.h"
#include "lang.h"
#include "settings/settings.h"
#include "input/action.h"
#include "widgets/list.h"
#include "widgets/menu.h"
#include "screens/context_menu.h"
#include "root_menu.h"          /* MENU_ATTACHED_USB */
#include "context_menu_config.h"

/* The kind the open screen edits */
static int edit_kind;
static const short *edit_rows;
static int edit_count;

static bool row_shown(int n)
{
    return !(global_settings.context_hidden[edit_kind] & (1u << n));
}

static const char *row_get_name(int selected_item, void *data,
                                char *buffer, size_t buffer_len)
{
    (void)data;

    /* The state reads on the row itself, as on the main menu's own screen */
    snprintf(buffer, buffer_len, "%s: %s", str(edit_rows[selected_item]),
             row_shown(selected_item) ? str(LANG_ON) : str(LANG_OFF));
    return buffer;
}

static enum themable_icons row_get_icon(int selected_item, void *data)
{
    (void)selected_item;
    (void)data;
    return Icon_Config;
}

static int row_action_callback(int action, struct gui_synclist *lists)
{
    int sel = gui_synclist_get_sel_pos(lists);

    if (action == ACTION_STD_OK && sel >= 0 && sel < edit_count)
    {
        global_settings.context_hidden[edit_kind] ^= 1u << sel;
        /* Stay in the list: turning two rows back on is one visit */
        return ACTION_REDRAW;
    }
    if (action == ACTION_STD_CONTEXT)
    {
        MENUITEM_STRINGLIST(menu, ID2P(LANG_CONTEXT_MENUS), NULL,
                            ID2P(LANG_SHOW_ALL_ROWS));

        if (do_menu(&menu, NULL, NULL, false) == 0)
            global_settings.context_hidden[edit_kind] = 0;
        gui_synclist_set_title(lists, lists->title, lists->title_icon);
        return ACTION_REDRAW;
    }
    return action;
}

int context_menu_config(void *param)
{
    struct simplelist_info info;
    uint32_t was;
    bool to_root;

    edit_kind = (intptr_t)param;
    edit_count = context_menu_kind_rows(edit_kind, &edit_rows);
    was = global_settings.context_hidden[edit_kind];

    simplelist_info_init(&info, str(context_menu_kind_label(edit_kind)),
                         edit_count, NULL);
    info.get_name = row_get_name;
    info.get_icon = row_get_icon;
    info.action_callback = row_action_callback;

    to_root = simplelist_show_list(&info);

    if (global_settings.context_hidden[edit_kind] != was)
        settings_save();
    return to_root ? MENU_ATTACHED_USB : 0;
}
