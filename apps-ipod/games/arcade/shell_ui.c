/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A cart's dialogs on the player: struct arcade_ui, which ui.* goes
 * through, drawn by the player's own screens -- a list for a choice, the
 * prose box for a question or a message, the number picker, and the
 * wheel keyboard, each with its title in the box. The shell hands the
 * screen back for each and takes it again after (shell_dialog_begin and
 * _end); the game waits meanwhile.
 ****************************************************************************/
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "lang.h"
#include "settings/settings.h"      /* set_int */
#include "speech/talk.h"            /* UNIT_INT */
#include "widgets/list.h"
#include "widgets/dialog_prose.h"
#include "widgets/keyboard.h"
#include "games/arcade/arcade.h"
#include "games/arcade/shell.h"
#include "games/arcade/shell_ui.h"

static const char *item_row(int row, void *data, char *buf, size_t len)
{
    (void)buf; (void)len;
    return ((const char *const *)data)[row];
}

static int ui_choose(void *ctx, const char *title, const char *const *items,
                     int n, int at)
{
    (void)ctx;
    struct simplelist_info info;
    shell_dialog_begin();
    simplelist_info_init(&info, (char *)title, n, (void *)items);
    info.get_name = item_row;
    info.selection = at;
    bool to_root = shell_list(&info);
    shell_dialog_end(to_root);
    return to_root ? -1 : info.selection;
}

static int ui_confirm(void *ctx, const char *title, const char *text)
{
    (void)ctx;
    shell_dialog_begin();
    bool yes = dialog_prose_confirm(title, text, str(LANG_SET_BOOL_YES),
                                    str(LANG_SET_BOOL_NO), false);
    shell_dialog_end(false);
    return yes;
}

static void ui_message(void *ctx, const char *title, const char *text)
{
    (void)ctx;
    shell_dialog_begin();
    shell_dialog_end(dialog_prose_show(title, text));
}

static int32_t ui_number(void *ctx, const char *title, int32_t min,
                         int32_t max, int32_t step, int32_t value)
{
    (void)ctx;
    int v = (int)value;
    shell_dialog_begin();
    bool to_root = set_int((const unsigned char *)title, "", UNIT_INT, &v,
                           NULL, (int)step, (int)min, (int)max, NULL);
    shell_dialog_end(to_root);
    return to_root ? value : v;
}

static int ui_text(void *ctx, const char *title, char *buf, int cap)
{
    (void)ctx;
    shell_dialog_begin();
    int r = dialog_input_titled(title, buf, cap);
    shell_dialog_end(false);
    return r == 0;
}

const struct arcade_ui shell_ui = {
    ui_choose, ui_confirm, ui_message, ui_number, ui_text
};
