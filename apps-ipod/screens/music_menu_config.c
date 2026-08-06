/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Turns rows of the Music menu on and off, the way main_menu_config.c does for
 * the main menu -- so that hiding one does not mean editing tagnavi.config.
 *
 * That file still owns which rows exist and in what order; this only says
 * which of them are drawn. Reordering stays a config edit, deliberately: the
 * order is the config's own text and is the part worth reading there.
 *
 * A row is remembered by its position in the root menu, which only means
 * anything for the row set it was chosen against -- so the choice is stored
 * with a signature of that set (see browser_db.h). Edit tagnavi.config and
 * every row comes back, rather than the wrong ones going missing.
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include "string-extra.h"
#include "config.h"
#include "lang.h"
#include "settings/settings.h"
#include "input/action.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "browse/browser_db.h"
#include "music_menu_config.h"

/* One row per bit of the mask; the rest cannot be hidden and so are not
 * listed. Comfortably more than the shipped menu's sixteen rows. */
#define MAX_ROWS 31

static struct {
    int index;                    /* position in tagnavi.config's root menu */
    const unsigned char *name;
    bool shown;
} rows[MAX_ROWS];

static int row_count;

static const char *row_get_name(int selected_item, void *data,
                                char *buffer, size_t buffer_len)
{
    (void)data;

    if (selected_item < 0 || selected_item >= row_count)
    {
        buffer[0] = '\0';
        return buffer;
    }

    /* The state reads on the row itself, as the main menu's own screen does:
     * an icon for "off" leaves a blank cell that looks like a missing icon
     * rather than like a choice. */
    snprintf(buffer, buffer_len, "%s: %s",
             P2STR(rows[selected_item].name),
             rows[selected_item].shown ? str(LANG_ON) : str(LANG_OFF));

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
    if (action == ACTION_STD_OK)
    {
        int sel = gui_synclist_get_sel_pos(lists);

        if (sel >= 0 && sel < row_count)
            rows[sel].shown = !rows[sel].shown;

        /* Stay in the list: this screen is a series of toggles, and leaving on
         * the first one would make turning two rows off a trip each. */
        return ACTION_REDRAW;
    }
    return action;
}

static void load_rows(void)
{
    int n;

    row_count = 0;
    for (n = 0; n < browser_db_root_row_count() && row_count < MAX_ROWS; n++)
    {
        int index;
        const unsigned char *name;

        if (!browser_db_get_root_row(n, &index, &name))
            break;

        rows[row_count].index = index;
        rows[row_count].name = name;
        rows[row_count].shown = !browser_db_root_row_is_hidden(index);
        row_count++;
    }
}

static void save_rows(void)
{
    int hidden = 0;
    int i;

    for (i = 0; i < row_count; i++)
    {
        if (!rows[i].shown && rows[i].index < MAX_ROWS)
            hidden |= 1 << rows[i].index;
    }

    /* Written together: the mask is meaningless without the signature that
     * says which row set it describes. */
    global_settings.music_menu_hidden = hidden;
    global_settings.music_menu_sig = browser_db_root_row_signature();
    settings_save();
}

int music_menu_config(void)
{
    struct simplelist_info info;

    load_rows();

    if (row_count == 0)
    {
        /* No database, or a tagnavi.config that parsed into nothing. */
        splash(HZ * 2, ID2P(LANG_TAGCACHE_BUSY));
        return 0;
    }

    simplelist_info_init(&info, str(LANG_MUSIC_BROWSER), row_count, NULL);
    info.get_name = row_get_name;
    info.get_icon = row_get_icon;
    info.action_callback = row_action_callback;

    simplelist_show_list(&info);

    save_rows();
    return 0;
}
