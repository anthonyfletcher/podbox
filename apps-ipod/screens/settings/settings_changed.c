/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Every setting that no longer holds its default, in one list, editable where
 * it stands. The answer to "what did I change?", for which the only answer
 * before this was a full Reset Settings.
 *
 * Rows are collected by walking the settings menu tree (settings_walk.h), not
 * settings[], so each row can be opened exactly as its own menu would open it
 * and can say which menu that is.
 *
 * What the list means is worth being clear about: "changed" is measured
 * against the value compiled in, not against the state the player shipped in.
 * The bundled config.cfg sets around thirty settings at first boot -- the
 * theme, the fonts, the carousel speeds -- and all of them legitimately appear
 * here. See settings_is_changed().
 *
 * Parts, in order:
 *   - collecting the rows
 *   - the list
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lang.h"
#include "settings/settings.h"
#include "settings/settings_list.h"
#include "widgets/list.h"
#include "widgets/menu.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"
#include "input/action.h"
#include "settings_walk.h"
#include "settings_changed.h"

/* Comfortably more than anyone accumulates. A player past this has been
 * reconfigured wholesale, and the list has stopped being the tool for the
 * job. */
#define MAX_ROWS 128

static struct changed_row {
    const struct menu_item_ex *item;
    const char                *parent;
} rows[MAX_ROWS];

static int row_count;

/* ---- collecting the rows ------------------------------------------------ */

static bool collect_changed(const struct menu_item_ex *item,
                            const struct settings_list *setting,
                            const char *parent, void *ctx)
{
    (void)ctx;

    /* The same skips the config writer makes: no cfg name means nothing is
     * persisted to compare, and a deprecated setting is not the user's to
     * reason about.
     *
     * F_RESUMESETTING is the one that is not obvious and matters most. Those
     * are remembered state -- the volume, the resume position -- rather than
     * preferences, and they differ from their defaults nearly always. Without
     * this the screen opens on them and buries whatever was actually changed.
     * settings_write_config() excludes them from SETTINGS_SAVE_ALL for the
     * same reason. */
    if (!setting->cfg_name
        || (setting->flags & (F_DEPRECATED | F_RESUMESETTING)))
        return true;

    if (!settings_is_changed(setting))
        return true;

    rows[row_count].item = item;
    rows[row_count].parent = parent;
    row_count++;

    return row_count < MAX_ROWS;
}

static void collect(void)
{
    row_count = 0;
    settings_walk(collect_changed, NULL);
}

/* ---- the list ----------------------------------------------------------- */

static const char *changed_get_name(int selected_item, void *data,
                                    char *buffer, size_t buffer_len)
{
    const struct settings_list *setting;

    (void)data;

    if (selected_item < 0 || selected_item >= row_count)
        return "";

    setting = find_setting(rows[selected_item].item->variable);
    if (!setting)
        return "";

    snprintf(buffer, buffer_len, "%s  (%s)",
             settings_walk_item_label(rows[selected_item].item, setting),
             rows[selected_item].parent);
    return buffer;
}

/* The count is the useful part of the title -- "how much have I changed" is
 * most of the question being asked -- so it is rebuilt whenever the list is.
 * Static because the title is re-set after every sub-screen and the buffer has
 * to outlive the call that formats it. */
static char title[64];

/* Anything that draws over the list takes the title with it -- an option
 * screen, a yes/no box -- so it is put back rather than set once. Same reason
 * track_info.c re-sets its own after view_text(). */
static void set_title(struct gui_synclist *lists)
{
    snprintf(title, sizeof title, "%s (%d)",
             str(LANG_SETTINGS_CHANGED), row_count);
    gui_synclist_set_title(lists, title, NOICON);
}

/* Re-collect and put the selection somewhere sane. Editing a row can take it
 * off the list -- setting it back to its default is the whole point -- and the
 * rows are pointers into a walk that has to be redone either way. */
static void refresh(struct gui_synclist *lists)
{
    int sel = gui_synclist_get_sel_pos(lists);

    collect();
    gui_synclist_set_nb_items(lists, row_count);
    set_title(lists);

    if (sel >= row_count)
        sel = row_count - 1;
    gui_synclist_select_item(lists, sel < 0 ? 0 : sel);
}

static int changed_action(int action, struct gui_synclist *lists)
{
    int sel = gui_synclist_get_sel_pos(lists);

    if (row_count == 0)
        return action;

    if (action == ACTION_STD_OK)
    {
        do_setting_from_menu_standalone(rows[sel].item, NULL);
        refresh(lists);
        return ACTION_REDRAW;
    }

    /* Undo, from the screen that shows what there is to undo. The menu row's
     * own context menu offers this too, but getting to that row is exactly
     * what this screen exists to avoid. */
    if (action == ACTION_STD_CONTEXT)
    {
        const struct settings_list *setting =
                find_setting(rows[sel].item->variable);

        if (setting && yesno_pop_confirm(ID2P(LANG_RESET_SETTING)))
        {
            reset_setting(setting, setting->setting);
            settings_save();
            settings_apply(false);
            refresh(lists);
        }
        return ACTION_REDRAW;
    }

    return action;
}

bool settings_changed_screen(void)
{
    struct simplelist_info info;

    collect();

    if (row_count == 0)
    {
        splash(HZ * 2, ID2P(LANG_SETTINGS_ALL_DEFAULT));
        return false;
    }

    snprintf(title, sizeof(title), "%s (%d)",
             str(LANG_SETTINGS_CHANGED), row_count);

    simplelist_info_init(&info, title, row_count, NULL);
    info.get_name = changed_get_name;
    info.action_callback = changed_action;

    return simplelist_show_list(&info);
}
