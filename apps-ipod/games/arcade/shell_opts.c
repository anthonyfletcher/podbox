/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A cart's options and help on the player. The options are rows of the
 * pause menu, drawn as the player's own settings are: an on/off flips in
 * place, a choice or a number opens the player's picker. The owner's
 * values are a file under /.rockbox/games/options, named by the cart's
 * id, written when one changes. Help shows the cart's help text, then
 * each option's, in the box Explain uses.
 *
 * Parts, in order:
 *   - the file
 *   - the rows
 *   - help
 ****************************************************************************/
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "dir.h"                    /* mkdir */
#include "lang.h"
#include "settings/settings.h"     /* set_option, set_int */
#include "speech/talk.h"            /* UNIT_INT */
#include "widgets/list.h"
#include "widgets/dialog_prose.h"
#include "games/arcade/arcade.h"
#include "games/arcade/shell_opts.h"

#define OPTIONS_DIR "/.rockbox/games/options"

/* ---- the file ------------------------------------------------------------ */

static void options_path(const uint8_t *id, char *path, size_t size)
{
    snprintf(path, size, OPTIONS_DIR "/%02x%02x%02x%02x%02x%02x%02x%02x.opt",
             id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
}

void shell_options_load(struct vm *vm)
{
    char path[MAX_PATH];
    uint8_t b[ARCADE_OPTIONS_MAX];
    if (!arcade_options(vm)->count)
        return;
    options_path(arcade_id(vm), path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = read(fd, b, sizeof(b));
    close(fd);
    if (n > 0)
        arcade_options_unpack(vm, b, (int)n);
}

/* Written beside the old file and renamed over it, as the save is; a
 * refused write leaves the old values, and the next change tries again */
static void options_write(struct vm *vm)
{
    char path[MAX_PATH], part[MAX_PATH];
    uint8_t b[ARCADE_OPTIONS_MAX];
    int n = arcade_options_pack(vm, b, sizeof(b));
    mkdir("/.rockbox/games");
    mkdir(OPTIONS_DIR);
    options_path(arcade_id(vm), path, sizeof(path));
    snprintf(part, sizeof(part), "%s.new", path);
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    bool whole = n > 0 && write(fd, b, n) == n;
    if (close(fd) < 0)
        whole = false;
    if (!whole || rename(part, path) != 0)
        remove(part);
}

/* ---- the rows ------------------------------------------------------------ */

const char *shell_option_title(struct vm *vm, int i)
{
    return arcade_options(vm)->o[i].title;
}

enum list_row_kind shell_option_kind(struct vm *vm, int i)
{
    return arcade_options(vm)->o[i].type == ARCADE_OPT_ONOFF
        ? LIST_ROW_TOGGLE : LIST_ROW_SETTING;
}

/* Yes and No as the player's own toggles say them, so a skin that draws a
 * switch for those words draws one here */
const char *shell_option_value(struct vm *vm, int i, char *buf, size_t len)
{
    const struct arcade_option *o = &arcade_options(vm)->o[i];
    if (o->type == ARCADE_OPT_ONOFF)
        return str(o->value ? LANG_SET_BOOL_YES : LANG_SET_BOOL_NO);
    if (o->type == ARCADE_OPT_CHOICE)
        return o->choice[o->value];
    snprintf(buf, len, "%ld", (long)o->value);
    return buf;
}

int shell_option_range(struct vm *vm, int i)
{
    const struct arcade_option *o = &arcade_options(vm)->o[i];
    if (o->type != ARCADE_OPT_NUMBER)
        return -1;
    return (int)((int64_t)(o->value - o->min) * 100 / (o->max - o->min));
}

bool shell_option_edit(struct vm *vm, int i)
{
    const struct arcade_option *o = &arcade_options(vm)->o[i];
    int v = (int)o->value;
    bool usb = false;
    if (o->type == ARCADE_OPT_ONOFF) {
        v = !v;
    } else if (o->type == ARCADE_OPT_CHOICE) {
        struct opt_items items[ARCADE_CHOICES];
        for (int k = 0; k < o->nchoices; k++) {
            items[k].string = (const unsigned char *)o->choice[k];
            items[k].voice_id = -1;
        }
        usb = set_option(o->title, &v, RB_INT, items, o->nchoices, NULL);
    } else {
        usb = set_int((const unsigned char *)o->title, "", UNIT_INT, &v, NULL,
                      (int)o->step, (int)o->min, (int)o->max, NULL);
    }
    if (v != o->value) {
        arcade_option_set(vm, i, v);
        options_write(vm);
    }
    return usb;
}

/* ---- help ---------------------------------------------------------------- */

/* The cart's help, then each option's after its title, a paragraph each */
static int help_text(struct vm *vm, char *buf, size_t cap)
{
    const char *text;
    size_t used = 0;
    int n = arcade_help(vm, &text);
    if (n > 0)
        used = (size_t)snprintf(buf, cap, "%.*s", n, text);
    const struct arcade_options *s = arcade_options(vm);
    for (int i = 0; i < s->count && used < cap; i++)
        if (s->o[i].help_len)
            used += (size_t)snprintf(buf + used, cap - used, "%s%s: %.*s",
                                     used ? "\n\n" : "", s->o[i].title,
                                     s->o[i].help_len, s->o[i].help);
    return used > 0;
}

bool shell_has_help(struct vm *vm)
{
    const char *text;
    const struct arcade_options *s = arcade_options(vm);
    bool any = arcade_help(vm, &text) > 0;
    for (int i = 0; i < s->count; i++)
        any = any || s->o[i].help_len > 0;
    return any;
}

bool shell_help(struct vm *vm)
{
    /* static: the box runs a whole screen of its own from this frame */
    static char text[1024];
    if (!help_text(vm, text, sizeof(text)))
        return false;
    return dialog_prose_show(str(LANG_GAME_HELP), text);
}
