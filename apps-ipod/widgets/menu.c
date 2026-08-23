/***************************************************************************
 * Original code from RockBox
 * was: apps/menu.c
 * Copyright (C) 2002 Robert E. Hak
 * GNU General Public License (version 2+)
 *
 * The menu engine. Walks a static menu_item_ex tree, renders it as a list,
 * dispatches callbacks, and hands settings items to the option/value
 * screens.
 *
 * Menus are declared, not built. The MENUITEM_* macros in menu.h construct
 * const structs at file scope, so a menu costs no RAM and cannot be edited at
 * runtime -- what varies is decided by each item's callback, which can hide
 * the item (ACTION_EXIT_MENUITEM) or act when it is entered. Finding out what
 * a menu does means reading its callbacks.
 *
 * do_menu() is re-entered recursively for submenus, so one call sits on the
 * stack per level of menu depth.
 *
 * Parts, in order:
 *   - resolving an item's title, icon and voice id from its type
 *   - the list callbacks that present the menu as a list
 *   - item visibility and callback dispatch
 *   - do_menu(): the loop, including descent into submenus and settings
 ****************************************************************************/
/*
2005 Kevin Ferrare :
 - Multi screen support
 - Rewrote/removed a lot of code now useless with the new gui API
*/
#include <stdbool.h>
#include <stdlib.h>
#include "config.h"
#include "system.h"

#include "system/appevents.h"
#include "lcd.h"
#include "font.h"
#include "file.h"
#include "menu.h"
#include "button.h"
#include "kernel.h"
#include "debug.h"
#include "usb.h"
#include "panic.h"
#include "settings/settings.h"
#include "settings/settings_list.h"
#include "settings/settings_tags.h"
#include "option_select.h"
#include "speech/talk.h"
#include "lang.h"
#include "system/activity.h"
#include "system/app_util.h"
#include "audio/sound_feedback.h"
#include "system/shutdown.h"
#include "input/action.h"
#include "screens/settings/exported_settings.h"
#include "string.h"
#include "root_menu.h"
#include "audio.h"
#include "draw/viewport.h"
#include "text_box.h"                     /* view_text */
#include "splash.h"
#include "screens/settings/settings_help.h"
#include "screens/playback/quick_screen.h"
#include "screens/shortcuts.h"
#include "skin/statusbar_skinned.h"

#include "draw/icon_bitmaps.h"

/* gui api */
#include "list.h"

#define MAX_MENUS 8
/* used to allow for dynamic menus */
#define MAX_MENU_SUBITEMS 64
static struct menu_item_ex *current_submenus_menu;
static int current_subitems[MAX_MENU_SUBITEMS];
static int current_subitems_count = 0;
static int talk_menu_item(int selected_item, void *data);

struct menu_data_t
{
    const struct menu_item_ex *menu;
    int selected;
};

static int empty_menu_callback(int action, const struct menu_item_ex *this_item, struct gui_synclist *this_list)
{
    return action;
    (void)this_item;
    (void)this_list;
}

static void get_menu_callback(const struct menu_item_ex *m,
                        menu_callback_type *menu_callback)
{
    if (m->flags&(MENU_HAS_DESC|MENU_DYNAMIC_DESC))
        *menu_callback= m->callback_and_desc->menu_callback;
    else
        *menu_callback = m->menu_callback;

    if (!*menu_callback)
        *menu_callback = &empty_menu_callback;
}

static bool query_audio_status(int *old_audio_status)
{
    bool redraw_list = false;
    /* query audio status to see if it changed */
    int new_audio_status = audio_status();
    if (*old_audio_status != new_audio_status)
    {  /* force a redraw if anything changed the audio status from outside */
        *old_audio_status = new_audio_status;
        redraw_list = true;
    }
    return redraw_list;
}

static int get_menu_selection(int selected_item, const struct menu_item_ex *menu)
{
    int type = (menu->flags&MENU_TYPE_MASK);
    if ((type == MT_MENU || type == MT_RETURN_ID)
        && (selected_item<current_subitems_count))
        return current_subitems[selected_item];
    return selected_item;
}
static int find_menu_selection(int selected)
{
    int i;
    for (i=0; i< current_subitems_count; i++)
        if (current_subitems[i] == selected)
            return i;
    return 0;
}
static const char* get_menu_item_name(int selected_item,
                                      void * data,
                                      char *buffer,
                                      size_t buffer_len)
{
    const char *name;
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int type = (menu->flags&MENU_TYPE_MASK);
    selected_item = get_menu_selection(selected_item, menu);

    /* only MT_MENU or MT_RETURN_ID is allowed in here */
    if (type == MT_RETURN_ID)
    {
        if (menu->flags&MENU_DYNAMIC_DESC)
        {
            name = menu->menu_get_name_and_icon->list_get_name(selected_item,
            menu->menu_get_name_and_icon->list_get_name_data, buffer, buffer_len);
        }
        else
            name = menu->strings[selected_item];

        if (P2ID((unsigned char *)name) > VOICEONLY_DELIMITER)
            name = "";

        return name;
    }
    if (type == MT_MENU)
        menu = menu->submenus[selected_item];

    if ((menu->flags&MENU_DYNAMIC_DESC) && (type != MT_SETTING_W_TEXT))
        return menu->menu_get_name_and_icon->list_get_name(selected_item,
            menu->menu_get_name_and_icon->list_get_name_data, buffer, buffer_len);

    type = (menu->flags&MENU_TYPE_MASK);
    if ((type == MT_SETTING) || (type == MT_SETTING_W_TEXT))
    {
        const struct settings_list *v = find_setting(menu->variable);
        if (v)
            return str(v->lang_id);
        else return "Not Done yet!";
    }
    return P2STR(menu->callback_and_desc->desc);
}

static enum themable_icons  menu_get_icon(int selected_item, void * data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int menu_icon = Icon_NOICON;
    int type = (menu->flags&MENU_TYPE_MASK);
    selected_item = get_menu_selection(selected_item, menu);

    if (type == MT_RETURN_ID)
    {
        return Icon_Menu_functioncall;
    }
    if (type == MT_MENU)
        menu = menu->submenus[selected_item];

    if (menu->flags&MENU_HAS_DESC)
        menu_icon = menu->callback_and_desc->icon_id;
    else if (menu->flags&MENU_DYNAMIC_DESC)
        menu_icon = menu->menu_get_name_and_icon->icon_id;

    /* An item that named no icon of its own gets the generic one for its type.
     * Every MT_* has to appear here: a type missing from the chain keeps
     * Icon_NOICON and draws a blank where its neighbours draw an icon, whatever
     * iconset the theme supplies. MT_FUNCTION_CALL_W_PARAM was the missing one
     * -- it left the six colour setters, Manage Settings' four entries, Browse
     * EQ Presets, Save Current Playlist, Properties and Track Info iconless. */
    if (menu_icon == Icon_NOICON)
    {
        unsigned int flags = (menu->flags&MENU_TYPE_MASK);
        if(flags == MT_MENU)
            menu_icon = Icon_Submenu;
        else if (flags == MT_SETTING || flags == MT_SETTING_W_TEXT)
             menu_icon = Icon_Menu_setting;
        else if (flags == MT_FUNCTION_CALL
              || flags == MT_FUNCTION_CALL_W_PARAM
              || flags == MT_RETURN_VALUE)
             menu_icon = Icon_Menu_functioncall;
    }
    return menu_icon;
}

static char* init_title(const struct menu_item_ex *menu, int *icon,
                        char* buf, size_t buf_sz)
{
    char *title;

    if (menu->flags&MENU_HAS_DESC)
    {
        *icon = menu->callback_and_desc->icon_id;
        title = P2STR(menu->callback_and_desc->desc);
    }
    else if (menu->flags&MENU_DYNAMIC_DESC)
    {
        *icon = menu->menu_get_name_and_icon->icon_id;
        title = menu->menu_get_name_and_icon->
                      list_get_name(-1, menu->menu_get_name_and_icon->
                                    list_get_name_data, buf, buf_sz);
    }
    else
    {
        *icon = Icon_NOICON;
        title = "";
    }

    if (*icon == Icon_NOICON)
        *icon = Icon_Submenu_Entered;

    return title;
}

/* True if this row is hidden at the current Settings Mode.
 *
 * Two sources, because there are two kinds of row. A setting says so through
 * settings_tags.c, which keeps that table the single place deciding what is
 * advanced -- there is nothing to mark here and nothing to fall out of step.
 * A submenu or an action has no settings_list entry to tag, so it carries
 * MENU_ADVANCED itself.
 *
 * A submenu with nothing left to show hides too. That means marking every row
 * of a screen advanced hides the screen, without anyone having to remember to
 * mark the screen as well -- and it is why Peak Meter and Artwork Filter
 * disappear whole rather than becoming empty lists. */
static bool item_hidden(const struct menu_item_ex *item, int depth)
{
    int type;

    if (!item || depth > MAX_MENUS)
        return false;
    if (item->flags & MENU_ADVANCED)
        return true;

    type = item->flags & MENU_TYPE_MASK;

    if (type == MT_SETTING || type == MT_SETTING_W_TEXT)
    {
        const struct settings_list *setting = find_setting(item->variable);
        return setting && (settings_tags_get(setting) & TAG_ADVANCED);
    }

    if (type == MT_MENU)
    {
        int count = MENU_GET_COUNT(item->flags);
        for (int i = 0; i < count; i++)
            if (!item_hidden(item->submenus[i], depth + 1))
                return false;
        return count > 0;
    }

    return false;
}

/* Show the explanation for a setting, or say there is not one.
 *
 * Offered on every setting rather than only where a stanza exists, which would
 * mean reading the file before the context menu opens -- on every context
 * press. The 5G powers its ATA interface down after seven idle seconds, so
 * that turns a button press into a possible disk spin-up. The file is read
 * only when someone actually asks. */
static bool explain(const char *key, const char *title)
{
    /* Static rather than automatic: view_text() below runs a whole screen of
     * its own from this frame, so half a kilobyte here sits under all of it.
     * One explanation is on screen at a time, so one buffer is enough. */
    static char text[512];

    if (!key || !settings_help_lookup(key, text, sizeof text))
    {
        splash(HZ, ID2P(LANG_NO_EXPLANATION));
        return false;
    }

    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, false, NULL);
    int leave = view_text(title, text);
    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, false);

    return leave != 0;
}

static bool explain_setting(const struct settings_list *setting)
{
    return explain(setting->cfg_name,
                   setting->lang_id != -1 ? (const char *)str(setting->lang_id)
                                          : setting->cfg_name);
}

/* An action row -- Rebuild Database, Clear Backdrop -- has no settings_list
 * entry and so no cfg name to key its help by. It is keyed by its own label
 * instead, under an "action: " namespace so the two cannot collide:
 *
 *     [action: Rebuild Database]
 *
 * The label rather than something more stable because it is the only identity
 * such a row has at runtime. Renaming a row therefore orphans its stanza, which
 * shows as Explain saying there is nothing rather than as anything worse. */
static bool explain_action(const char *label)
{
    char key[96];

    if (!label)
    {
        splash(HZ, ID2P(LANG_NO_EXPLANATION));
        return false;
    }

    snprintf(key, sizeof key, "action: %s", label);
    return explain(key, label);
}

/* A top row asked for by whoever is about to open a menu, consumed once by the
 * next list build.
 *
 * A request rather than an argument because do_menu()'s start_selected is used
 * by most of its callers to *restore* a position, and those want the list's
 * usual habit of keeping a row visible above the cursor. Only a menu opening
 * deliberately past its own first row wants that row scrolled away, and it is
 * the caller who knows. Same shape as browser_db.c's pending_top_item, for the
 * same reason. */
static int pending_top_item = -1;

void menu_set_pending_top_item(int item)
{
    pending_top_item = item;
}

static int init_menu_lists(const struct menu_item_ex *menu,
                     struct gui_synclist *lists, int selected, bool callback,
                     struct viewport parent[NB_SCREENS], char* buf, size_t buf_sz)
{
    if (!menu || !lists)
    {
        panicf("init_menu_lists, NULL pointer");
        return 0;
    }

    int i;
    int start_action = ACTION_ENTER_MENUITEM;
    int count = MIN(MENU_GET_COUNT(menu->flags), MAX_MENU_SUBITEMS);
    int type = (menu->flags&MENU_TYPE_MASK);
    menu_callback_type menu_callback = &empty_menu_callback;
    int icon;
    char * title;
    current_subitems_count = 0;

    if (type == MT_RETURN_ID)
        get_menu_callback(menu, &menu_callback);

    for (i=0; i<count; i++)
    {
        if (type != MT_RETURN_ID)
        {
            /* A string list indexes by position and holds no items, so there
               is nothing to hide there. */
            if (global_settings.settings_mode == SETTINGS_MODE_STANDARD
                && item_hidden(menu->submenus[i], 0))
                continue;

            get_menu_callback(menu->submenus[i],&menu_callback);
        }

        if (menu_callback(ACTION_REQUEST_MENUITEM,
            type==MT_RETURN_ID ? (void*)(intptr_t)i: menu->submenus[i], lists)
                != ACTION_EXIT_MENUITEM)
        {
            current_subitems[current_subitems_count] = i;
            current_subitems_count++;
        }
    }

    current_submenus_menu = (struct menu_item_ex *)menu;

    gui_synclist_init(lists,get_menu_item_name,(void*)menu,false,1, parent);
    title = init_title(menu, &icon, buf, buf_sz);
    gui_synclist_set_title(lists, title, icon);
    gui_synclist_set_icon_callback(lists, global_settings.show_icons?menu_get_icon:NULL);
    if(global_settings.talk_menu)
        gui_synclist_set_voice_callback(lists, talk_menu_item);
    gui_synclist_set_nb_items(lists,current_subitems_count);
    gui_synclist_select_item(lists, find_menu_selection(selected));

    /* After the selection, which sets a top row of its own. */
    if (pending_top_item >= 0)
    {
        gui_synclist_set_top_item(lists, pending_top_item);
        pending_top_item = -1;
    }

    get_menu_callback(menu,&menu_callback);
    if (callback)
        start_action = menu_callback(start_action, menu, lists);

    return start_action;
}

static int talk_menu_item(int selected_item, void *data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int id = -1;
    int type;
    unsigned char *str;
    int sel = get_menu_selection(selected_item, menu);

        if ((menu->flags&MENU_TYPE_MASK) == MT_MENU)
        {
            type = menu->submenus[sel]->flags&MENU_TYPE_MASK;
            if ((type == MT_SETTING) || (type == MT_SETTING_W_TEXT))
                talk_setting(menu->submenus[sel]->variable);
            else
            {
                if (menu->submenus[sel]->flags&(MENU_DYNAMIC_DESC))
                {
                    int (*list_speak_item)(int selected_item, void * data)
                        = menu->submenus[sel]->menu_get_name_and_icon->
                        list_speak_item;
                    if(list_speak_item)
                        list_speak_item(sel, menu->submenus[sel]->
                                        menu_get_name_and_icon->
                                        list_get_name_data);
                    else
                    {
                        char buffer[80];
                        str = menu->submenus[sel]->menu_get_name_and_icon->
                            list_get_name(sel, menu->submenus[sel]->
                                    menu_get_name_and_icon->
                                    list_get_name_data, buffer, sizeof(buffer));
                        id = P2ID(str);
                    }
                }
                else
                    id = P2ID(menu->submenus[sel]->callback_and_desc->desc);
                if (id != -1)
                    talk_id(id,false);
            }
        }
        else if(((menu->flags&MENU_TYPE_MASK) == MT_RETURN_ID))
        {
            if ((menu->flags&MENU_DYNAMIC_DESC) == 0)
            {
                unsigned char *s = (unsigned char *)menu->strings[sel];
                /* string list, try to talk it if ID2P was used */
                id = P2ID(s);
                if (id != -1)
                    talk_id(id,false);
            }
        }
        return 0;
}

void do_setting_screen(const struct settings_list *setting, const char * title,
                        struct viewport parent[NB_SCREENS])
{
    char padded_title[MAX_PATH];
    /* Pad the title string by repeating it. This is needed
       so the scroll settings title can actually be used to
       test the setting */
    if (setting->flags&F_PADTITLE)
    {
        int i = 0, len;
        title = P2STR((unsigned char*)title);
        len = strlen(title);
        while (i < MAX_PATH-1)
        {
            int padlen = MIN(len, MAX_PATH-1-i);
            memcpy(&padded_title[i], title, padlen);
            i += padlen;
            if (i<MAX_PATH-1)
                padded_title[i++] = ' ';
        }
        padded_title[i] = '\0';
        title = padded_title;
    }

    option_screen((struct settings_list *)setting, parent,
                  setting->flags&F_TEMPVAR, (char*)title);
}


void do_setting_from_menu(const struct menu_item_ex *temp,
                          struct viewport parent[NB_SCREENS])
{
    char *title;
    if (!temp)
    {
        panicf("do_setting_from_menu, NULL pointer");
        return;
    }
    const struct settings_list *setting = find_setting(temp->variable);

    if ((temp->flags&MENU_TYPE_MASK) == MT_SETTING_W_TEXT)
        title = temp->callback_and_desc->desc;
    else
        title = ID2P(setting->lang_id);

    do_setting_screen(setting, title, parent);
}

/* Open a setting from outside any menu -- the settings search, which finds an
 * item by walking the tree rather than by the user standing on it.
 *
 * do_setting_screen() alone is not enough. A setting whose effect is applied
 * by its menu callback rather than by the setting itself -- Directory Cache
 * enabling the cache, Charge During USB calling usb_charging_enable(), the
 * sleep timer restarting a running countdown -- would change value and do
 * nothing. So this reproduces what the menu loop does around a row: the same
 * ENTER/EXIT pair, on the same item, in the same order.
 *
 * Calling the item's own callback rather than keeping a list of which settings
 * need what is the point: there is nothing here to fall out of step, and a
 * setting given a callback tomorrow is handled without anyone remembering
 * this function exists.
 *
 * `this_list` is NULL, which every callback in the tree already tolerates --
 * none reads it. */
void do_setting_from_menu_standalone(const struct menu_item_ex *item,
                                     struct viewport parent[NB_SCREENS])
{
    menu_callback_type cb;

    if (!item)
        return;

    get_menu_callback(item, &cb);

    /* A callback may refuse the item on entry, exactly as in a menu. */
    if (cb(ACTION_ENTER_MENUITEM, item, NULL) == ACTION_EXIT_MENUITEM)
        return;

    do_setting_from_menu(item, parent);

    cb(ACTION_EXIT_MENUITEM, item, NULL);
}

/* display a menu */
int do_menu(const struct menu_item_ex *start_menu, int *start_selected,
            struct viewport parent[NB_SCREENS], bool hide_theme)
{
    int selected = start_selected? *start_selected : 0;
    int ret = 0;
    int action;
    int start_action;
    int icon;
    char buf[80], *title;
    struct gui_synclist lists;
    const struct menu_item_ex *temp = NULL;
    const struct menu_item_ex *menu = start_menu;

    bool in_stringlist, done = false;
    bool redraw_lists;

    int old_audio_status = audio_status();

    /* Plugins run as ACTIVITY_PLUGIN, but SBS themes typically exclude
     * that activity from sub-menu styling (%Lb, header viewports).
     * Temporarily switch to ACTIVITY_CONTEXTMENU so the SBS renders
     * its full menu chrome for plugin menus. */
    bool plugin_activity = (get_current_activity() == ACTIVITY_PLUGIN);
    if (plugin_activity)
        push_current_activity(ACTIVITY_CONTEXTMENU);


    title = init_title(menu, &icon, buf, sizeof buf);
    FOR_NB_SCREENS(i)
    {
        sb_set_persistent_title(title, icon, i);
        viewportmanager_theme_enable(i, !hide_theme, NULL);
    }
    struct menu_data_t mstack[MAX_MENUS]; /* menu, selected */
    int stack_top = 0;

    struct viewport *vps = NULL;
    menu_callback_type menu_callback = &empty_menu_callback;

    /* if hide_theme is true, assume parent has been fixed before passed into
     * this function, e.g. with viewport_set_defaults(parent, screen)
     * start_action allows an action to be processed
     * by menu logic by bypassing get_action on the initial run */
    start_action = init_menu_lists(menu, &lists, selected, true, parent,
                                   buf, sizeof buf);
    vps = *(lists.parent);
    in_stringlist = ((menu->flags&MENU_TYPE_MASK) == MT_RETURN_ID);
    /* load the callback, and only reload it if menu changes */
    get_menu_callback(menu, &menu_callback);

    gui_synclist_draw_settled(&lists);
    gui_synclist_speak_item(&lists);

    while (!done)
    {
        keyclick_set_callback(gui_synclist_keyclick_callback, &lists);

        if (UNLIKELY(start_action != ACTION_ENTER_MENUITEM))
        {
            action = start_action;
            start_action = ACTION_ENTER_MENUITEM;
        }
        else
            action = get_action(CONTEXT_MAINMENU|ALLOW_SOFTLOCK,
                                list_do_action_timeout(&lists, HZ));
            /* HZ so the status bar redraws corectly */

        /* query audio status to see if it changed */
        redraw_lists = query_audio_status(&old_audio_status);


        int new_action = menu_callback(action, menu, &lists);
        if (new_action == ACTION_EXIT_AFTER_THIS_MENUITEM)
            ret = MENU_SELECTED_EXIT; /* exit after return from selection */
        else if (new_action == ACTION_REDRAW)
            redraw_lists = true;
        else
            action = new_action;

        if (LIKELY(gui_synclist_do_button(&lists, &action)))
            continue;
        else if (action == ACTION_STD_QUICKSCREEN)
        {
            if (global_settings.shortcuts_replaces_qs ||
                quick_screen_quick(action) == QUICKSCREEN_GOTO_SHORTCUTS_MENU)
            {
                int last_screen = global_status.last_screen;
                global_status.last_screen = GO_TO_SHORTCUTMENU;
                int shortcut_ret = do_shortcut_menu(NULL);
                if (shortcut_ret == GO_TO_PREVIOUS)
                    global_status.last_screen = last_screen;
                else
                {
                    ret = shortcut_ret;
                    done = true;
                }
            }
            if (!done)
                init_menu_lists(menu, &lists, lists.selected_item, false, vps,
                                buf, sizeof buf);
            redraw_lists = true;
        }
        else if (action == ACTION_TREE_WPS)
        {
            ret = GO_TO_PREVIOUS_MUSIC;
            done = true;
        }
        else if (action == ACTION_TREE_STOP)
        {
            redraw_lists = list_stop_handler();
        }
        else if (action == ACTION_STD_CONTEXT)
        {
            if (menu == &root_menu_)
            {
                ret = GO_TO_ROOTITEM_CONTEXT;
                done = true;
            }
            else if (!in_stringlist)
            {
                int type = (menu->flags&MENU_TYPE_MASK);
                selected = get_menu_selection(gui_synclist_get_sel_pos(&lists),menu);
                if (type == MT_MENU)
                {
                    temp = menu->submenus[selected];
                    type = (temp->flags&MENU_TYPE_MASK);
                }
                else
                    type = -1;

                if (type == MT_SETTING_W_TEXT || type == MT_SETTING)
                {
                    const struct menu_item_ex *context_menu;
                    const struct settings_list *setting =
                            find_setting(temp->variable);

                    /* Explain first, Reset second, and that order is load
                       bearing: non_quickscreen_op_menu below reuses the first
                       N of these strings, so anything a non-quickscreenable
                       setting must still offer has to come before the
                       quickscreen entries. */
                    MENUITEM_STRINGLIST(settings_op_menu,
                                        ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                                        ID2P(LANG_EXPLAIN),
                                        ID2P(LANG_RESET_SETTING),
                                        ID2P(LANG_TOP_QS_ITEM),
                                        ID2P(LANG_LEFT_QS_ITEM),
                                        ID2P(LANG_BOTTOM_QS_ITEM),
                                        ID2P(LANG_RIGHT_QS_ITEM),
                                        ID2P(LANG_ADD_CURRENT_TO_FAVES),
                                        ID2P(LANG_ADD_TO_FAVES));

                    /* re-use the strings and desc from the settings_op_menu */
                    static const struct menu_item_ex non_quickscreen_op_menu =
                    {MT_RETURN_ID|MENU_HAS_DESC|MENU_ITEM_COUNT(2),
                    { .strings = settings_op_menu_},
                    {.callback_and_desc = &settings_op_menu__}};

                    if (is_setting_quickscreenable(setting))
                        context_menu = &settings_op_menu;
                    else
                    {
                        context_menu = &non_quickscreen_op_menu;
                    }
                    int msel = do_menu(context_menu, NULL, NULL, false);

                    switch (msel)
                    {
                        case GO_TO_PREVIOUS:
                            break;
                        case 0: /* explain */
                            /* MENU in the explanation means the root menu,
                               not back to this row's context. */
                            if (explain_setting(setting))
                            {
                                ret = GO_TO_ROOT;
                                done = true;
                            }
                            break;
                        case 1: /* reset setting */
                            reset_setting(setting, setting->setting);
                            settings_save();
                            settings_apply(false);
                            break;
                        case 2: /* set as top QS item */
                            global_settings.qs_items[QUICKSCREEN_TOP] = setting;
                            break;
                        case 3: /* set as left QS item */
                            global_settings.qs_items[QUICKSCREEN_LEFT] = setting;
                            break;
                        case 4: /* set as bottom QS item */
                            global_settings.qs_items[QUICKSCREEN_BOTTOM] = setting;
                            break;
                        case 5: /* set as right QS item */
                            global_settings.qs_items[QUICKSCREEN_RIGHT] = setting;
                            break;
                        case 6: /* Add current value of setting to faves.
                                   Same limitation on which can be
                                   added to the shortcuts menu as the quickscreen */
                            shortcuts_add(SHORTCUT_SETTING_APPLY, (void*)setting);
                            break;
                        case 7: /* Add to faves. Same limitation on which can be
                                  added to the shortcuts menu as the quickscreen */
                            shortcuts_add(SHORTCUT_SETTING, (void*)setting);
                            break;
                    } /* switch(do_menu()) */
                    if (menu->flags & MENU_EXITAFTERTHISMENU)
                        done = true; /* in case context_menu_show menu contains setting */
                    redraw_lists = true;
                }
                else if ((type == MT_FUNCTION_CALL
                          || type == MT_FUNCTION_CALL_W_PARAM)
                         && (temp->flags & MENU_HAS_DESC))
                {
                    /* Explain only. There is nothing to reset on an action and
                       nothing the quickscreen could do with one. Requires a
                       fixed label, so the dynamic-text rows are left out --
                       their text is built per draw and is nobody's key. */
                    MENUITEM_STRINGLIST(action_op_menu,
                                        ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                                        ID2P(LANG_EXPLAIN));

                    if (do_menu(&action_op_menu, NULL, NULL, false) == 0
                        && explain_action(P2STR(temp->callback_and_desc->desc)))
                    {
                        ret = GO_TO_ROOT;
                        done = true;
                    }

                    redraw_lists = true;
                }

                /* Rebuild this menu's row mapping, which the context menu we
                   just ran overwrote -- current_subitems is one global shared
                   by every do_menu. Without it the rows redraw unfiltered, so
                   an advanced row hidden by Standard comes back after a
                   context press and opens the wrong screen. */
                if (current_submenus_menu != menu)
                    init_menu_lists(menu, &lists, selected, true, vps,
                                    buf, sizeof buf);
            } /* else if (!in_stringlist) */
        }
        else if (action == ACTION_STD_MENU)
        {
            if (menu != &root_menu_)
                ret = GO_TO_ROOT;
            else
                ret = GO_TO_PREVIOUS;
            done = true;
        }
        else if (action == ACTION_STD_CANCEL)
        {
            /* might be leaving list, so stop scrolling */
            gui_synclist_scroll_stop(&lists);

            bool exiting_menu = false;
            in_stringlist = false;

            menu_callback(ACTION_EXIT_MENUITEM, menu, &lists);

            if (menu->flags&MENU_EXITAFTERTHISMENU)
                done = true;
            else if ((menu->flags&MENU_TYPE_MASK) == MT_MENU)
                exiting_menu = true;

            if (stack_top > 0)
            {
                stack_top--;
                menu = mstack[stack_top].menu;
                int msel = mstack[stack_top].selected;
                if (!exiting_menu && (menu->flags&MENU_EXITAFTERTHISMENU))
                    done = true;
                else
                    init_menu_lists(menu, &lists, msel, false, vps, buf, sizeof buf);
                redraw_lists = true;
                /* new menu, so reload the callback */
                get_menu_callback(menu, &menu_callback);
            }
            else if (menu != &root_menu_)
            {
                ret = GO_TO_PREVIOUS;
                done = true;
            }
        }
        else if (action == ACTION_STD_OK)
        {
            /* entering an item that may not be a list, so stop scrolling */
            gui_synclist_scroll_stop(&lists);
            redraw_lists = true;

            int type = (menu->flags&MENU_TYPE_MASK);
            selected = get_menu_selection(gui_synclist_get_sel_pos(&lists), menu);
            if (type == MT_MENU)
                temp = menu->submenus[selected];
            else if (!in_stringlist)
                type = -1;

            if (!in_stringlist && temp)
            {
                type = (temp->flags&MENU_TYPE_MASK);
                get_menu_callback(temp, &menu_callback);
                action = menu_callback(ACTION_ENTER_MENUITEM, temp, &lists);
                if (action == ACTION_EXIT_MENUITEM)
                    break;
            }
            switch (type)
            {
                case MT_MENU:
                    if (stack_top < MAX_MENUS)
                    {
                        mstack[stack_top].menu = menu;
                        mstack[stack_top].selected = selected;
                        stack_top++;
                        menu = temp;
                        init_menu_lists(menu, &lists, 0, true, vps, buf, sizeof buf);
                    }
                    break;
                case MT_FUNCTION_CALL_W_PARAM:
                case MT_FUNCTION_CALL:
                {
                    int return_value;
                    if (type == MT_FUNCTION_CALL_W_PARAM)
                    {
                        return_value = temp->function_param->function_w_param(
                                    temp->function_param->param);
                    }
                    else
                    {
                        return_value = temp->function->function();
                    }
                    if (!(menu->flags&MENU_EXITAFTERTHISMENU) ||
                            (temp->flags&MENU_EXITAFTERTHISMENU))
                    {
                        /* Reload menu but don't run the calback again FS#8117 */
                        init_menu_lists(menu, &lists, selected, false, vps,
                                        buf, sizeof buf);
                    }
                    if (temp->flags&MENU_FUNC_CHECK_RETVAL)
                    {
                        if (return_value != 0)
                        {
                            done = true;
                            ret =  return_value;
                        }
                    }
                    break;
                }
                case MT_SETTING:
                case MT_SETTING_W_TEXT:
                {
                    do_setting_from_menu(temp, vps);
                    init_menu_lists(menu, &lists, selected, false, vps, buf, sizeof buf);
                    redraw_lists = true;

                    break;
                }
                case MT_RETURN_ID:
                    if (in_stringlist)
                    {
                        done = true;
                        ret =  selected;
                    }
                    else if (stack_top < MAX_MENUS)
                    {
                        mstack[stack_top].menu = menu;
                        mstack[stack_top].selected = selected;
                        stack_top++;
                        menu = temp;
                        init_menu_lists(menu, &lists, 0, false, vps, buf, sizeof buf);
                        in_stringlist = true;
                    }
                    break;
                case MT_RETURN_VALUE:
                    ret = temp->value;
                    done = true;
                    break;

                default:
                    ret = GO_TO_PREVIOUS;
                    done = true;
                    break;
            }
            if (type != MT_MENU)
            {
                menu_callback(ACTION_EXIT_MENUITEM, temp, &lists);
            }
            if (current_submenus_menu != menu)
                init_menu_lists(menu, &lists,selected, true, vps, buf, sizeof buf);
            /* callback was changed, so reload the menu's callback */
            get_menu_callback(menu, &menu_callback);
            if ((menu->flags&MENU_EXITAFTERTHISMENU) &&
                !(temp->flags&MENU_EXITAFTERTHISMENU))
            {
                done = true;
                break;
            }
        }
        else
        {
            if (action == SYS_USB_CONNECTED)
                gui_synclist_scroll_stop(&lists);

            switch(default_event_handler(action))
            {
                case SYS_USB_CONNECTED:
                    ret = MENU_ATTACHED_USB;
                    done = true;
                    break;
                case SYS_CALL_HUNG_UP:
                case BUTTON_MULTIMEDIA_PLAYPAUSE:
                /* remove splash from playlist_resume() */
                    redraw_lists = true;
                    break;
            }
        }

        if (redraw_lists && !done)
        {
            if (menu_callback(ACTION_REDRAW, menu, &lists) != ACTION_REDRAW)
                continue;


            gui_synclist_set_title(&lists, lists.title, lists.title_icon);
            gui_synclist_draw_settled(&lists);
            gui_synclist_speak_item(&lists);
        }
    }

    if (start_selected)
    {
        /* make sure the start_selected variable is set to
           the selected item from the menu do_menu() was called from */
        if (stack_top > 0)
        {
            menu = mstack[0].menu;
            init_menu_lists(menu, &lists, mstack[0].selected, true, vps,
                            buf, sizeof buf);
        }
        *start_selected = get_menu_selection(
                            gui_synclist_get_sel_pos(&lists), menu);
    }

    if (plugin_activity)
        pop_current_activity();

    FOR_NB_SCREENS(i)
    {
        sb_set_persistent_title(lists.title, lists.title_icon, i);
        viewportmanager_theme_undo(i, false);
        skinlist_set_cfg(i, NULL); /* Bugfix dangling reference in skin_draw() */
    }
    return ret;
}
