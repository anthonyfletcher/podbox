/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Appearance > Theme Options: the loaded theme's options files as a menu.
 * skin/theme_options.c reads and writes the files; this is the screen.
 *
 * One options file opens straight onto its options; more than one opens on
 * a row per file. Each change is written when it is made, and the theme
 * reloads once, on leaving, if anything changed.
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "lang.h"
#include "kernel.h"
#include "file.h"
#include "string-extra.h"
#include "settings/settings.h"
#include "input/action.h"
#include "widgets/list.h"
#include "widgets/menu.h"
#include "widgets/keyboard.h"
#include "widgets/splash.h"
#include "widgets/text_box.h"
#include "widgets/yesno.h"
#include "draw/viewport.h"
#include "skin/skin_engine.h"
#include "skin/statusbar_skinned.h"
#include "skin/theme_options.h"
#include "system/app_buffer.h"
#include "root_menu.h"          /* MENU_ATTACHED_USB */

#define OWNER "Theme Options"

/* The open options file, in the app buffer while it is up */
static struct
{
    const char *prefs;
    const char *title;
    struct theme_option *opts;
    char (*values)[THEME_OPTION_TEXT_MAX + 1];
    int count, room;
} f;

static bool changed;

static void collect(const struct theme_option *opt, void *data)
{
    (void)data;
    if (f.count < f.room)
        f.opts[f.count++] = *opt;
}

static const char *label_of(int n)
{
    const struct theme_option *o = &f.opts[n];

    if (o->type == THEME_OPTION_CHOICE)
        for (int i = 0; i < o->count; i++)
            if (!strcmp(o->choices[i].value, f.values[n]))
                return o->choices[i].label;
    return f.values[n];
}

static void set_value(int n, const char *value)
{
    const char *key = f.opts[n].key;

    if (!strcmp(f.values[n], value))
        return;
    if (!theme_options_write(f.prefs, &key, &value, 1))
    {
        splash(HZ, ID2P(LANG_FAILED));
        return;
    }
    strmemccpy(f.values[n], value, sizeof f.values[n]);
    changed = true;
}

static void restore_title(struct gui_synclist *lists, const char *title)
{
    FOR_NB_SCREENS(i)
        sb_set_persistent_title(title, Icon_NOICON, i);
    gui_synclist_set_title(lists, title, Icon_NOICON);
}

/* Explain's screen, for text the options file gave */
static bool show_help(const char *title, const char *help)
{
    int leave;

    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, false, NULL);
    leave = view_text(title, help);
    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, false);
    return leave != 0;
}

/* --- a choice ------------------------------------------------------------ */

static const struct theme_option *picking;

static const char *choice_name(int item, void *data, char *buf, size_t len)
{
    (void)data; (void)buf; (void)len;
    return picking->choices[item].label;
}

static enum list_row_kind choice_kind(int item, void *data)
{
    (void)item; (void)data;
    return LIST_ROW_OPTION;
}

/* True to leave for the root menu */
static bool pick(int n)
{
    struct simplelist_info info;
    bool to_root;

    picking = &f.opts[n];
    simplelist_info_init(&info, (char *)picking->name, picking->count, NULL);
    info.get_name = choice_name;
    info.get_kind = choice_kind;
    info.selection = 0;
    for (int i = 0; i < picking->count; i++)
        if (!strcmp(picking->choices[i].value, f.values[n]))
            info.selection = i;

    to_root = simplelist_show_list(&info);
    if (!to_root && info.selection >= 0)
        set_value(n, picking->choices[info.selection].value);
    return to_root;
}

/* --- text ---------------------------------------------------------------- */

static void type_text(int n)
{
    char buf[THEME_OPTION_TEXT_MAX + 1];
    char *s = buf, *end;

    strmemccpy(buf, f.values[n], f.opts[n].max + 1);
    if (dialog_input(buf, f.opts[n].max + 1) < 0)
        return;
    while (*s == ' ')
        s++;
    end = s + strlen(s);
    while (end > s && end[-1] == ' ')
        *--end = '\0';
    /* An empty entry is the default: %ft cannot read an empty value */
    set_value(n, *s ? s : f.opts[n].def);
}

/* --- an options file ----------------------------------------------------- */

static const char *option_name(int item, void *data, char *buf, size_t len)
{
    (void)data; (void)buf; (void)len;
    if (item < f.count)
        return f.opts[item].name;
    return str(LANG_RESET_ALL);
}

static const char *option_value(int item, void *data, char *buf, size_t len)
{
    (void)data; (void)buf; (void)len;
    return item < f.count ? label_of(item) : NULL;
}

static enum list_row_kind option_kind(int item, void *data)
{
    (void)data;
    return item < f.count ? LIST_ROW_SETTING : LIST_ROW_ACTION;
}

static void reset_all(void)
{
    const char *keys[THEME_OPTIONS_MAX], *values[THEME_OPTIONS_MAX];

    for (int i = 0; i < f.count; i++)
    {
        keys[i] = f.opts[i].key;
        values[i] = f.opts[i].def;
    }
    if (!theme_options_write(f.prefs, keys, values, f.count))
    {
        splash(HZ, ID2P(LANG_FAILED));
        return;
    }
    for (int i = 0; i < f.count; i++)
        strcpy(f.values[i], f.opts[i].def);
    changed = true;
}

static int option_action(int action, struct gui_synclist *lists)
{
    int sel = gui_synclist_get_sel_pos(lists);
    bool to_root = false;

    if (sel < 0)
        return action;
    if (action == ACTION_STD_OK)
    {
        if (sel == f.count)
        {
            if (yesno_pop(ID2P(LANG_RESET_ALL_ASK)))
                reset_all();
        }
        else if (f.opts[sel].type == THEME_OPTION_CHOICE)
            to_root = pick(sel);
        else
            type_text(sel);
    }
    else if (action == ACTION_STD_CONTEXT && sel < f.count)
    {
        MENUITEM_STRINGLIST(help_menu, ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                            ID2P(LANG_EXPLAIN), ID2P(LANG_RESET_SETTING));
        MENUITEM_STRINGLIST(plain_menu, ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                            ID2P(LANG_RESET_SETTING));
        const struct theme_option *o = &f.opts[sel];
        bool help = o->help[0] != '\0';
        int pick = do_menu(help ? &help_menu : &plain_menu, NULL, NULL, false);

        if (help && pick == 0)
            to_root = show_help(o->name, o->help);
        else if (pick == (help ? 1 : 0))
            set_value(sel, o->def);
        else if (pick == MENU_ATTACHED_USB)
            to_root = true;
    }
    else
        return action;

    if (to_root)
        return ACTION_STD_MENU;
    restore_title(lists, f.title);
    return ACTION_REDRAW;
}

/* True to leave for the root menu */
static bool edit_file(int n)
{
    struct simplelist_info info;
    size_t size;
    char *buf;
    bool to_root = false;
    const size_t each = sizeof *f.opts + sizeof *f.values;

    buf = app_try_claim_buffer(&size, OWNER);
    if (!buf)
        return false;

    f.prefs = theme_options_path(n);
    f.title = theme_options_title(n);
    f.room = MIN(size / each, THEME_OPTIONS_MAX);
    f.opts = (struct theme_option *)buf;
    f.values = (void *)(buf + f.room * sizeof *f.opts);
    f.count = 0;

    char opts_path[MAX_PATH];
    if (f.prefs && theme_options_options_path(f.prefs, opts_path,
                                              sizeof opts_path))
        theme_options_parse(opts_path, NULL, collect, NULL);

    for (int i = 0; i < f.count; i++)
        if (!theme_options_read(f.prefs, f.opts[i].key, f.values[i],
                                sizeof f.values[i])
            || !theme_option_valid(&f.opts[i], f.values[i]))
            strcpy(f.values[i], f.opts[i].def);

    if (f.count > 0)
    {
        simplelist_info_init(&info, (char *)f.title, f.count + 1, NULL);
        info.get_name = option_name;
        info.get_value = option_value;
        info.get_kind = option_kind;
        info.action_callback = option_action;
        to_root = simplelist_show_list(&info);
    }

    app_release_buffer(OWNER);
    return to_root;
}

/* --- more than one file -------------------------------------------------- */

static const char *file_name(int item, void *data, char *buf, size_t len)
{
    (void)data; (void)buf; (void)len;
    return theme_options_title(item);
}

static enum list_row_kind file_kind(int item, void *data)
{
    (void)item; (void)data;
    return LIST_ROW_MENU;
}

static int file_action(int action, struct gui_synclist *lists)
{
    static struct theme_options_head head;
    int sel = gui_synclist_get_sel_pos(lists);
    char path[MAX_PATH];

    if (action != ACTION_STD_CONTEXT || sel < 0)
        return action;

    /* Explain, if the file has help for the whole menu */
    if (theme_options_options_path(theme_options_path(sel), path, sizeof path)
        && theme_options_parse(path, &head, NULL, NULL) > 0 && head.help[0])
    {
        MENUITEM_STRINGLIST(menu, ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                            ID2P(LANG_EXPLAIN));
        int pick = do_menu(&menu, NULL, NULL, false);

        if ((pick == 0 && show_help(theme_options_title(sel), head.help))
            || pick == MENU_ATTACHED_USB)
            return ACTION_STD_MENU;
    }
    restore_title(lists, str(LANG_THEME_OPTIONS));
    return ACTION_REDRAW;
}

static int theme_options_menu(void)
{
    bool to_root = false;

    changed = false;
    if (theme_options_count() == 1)
        to_root = edit_file(0);
    else
    {
        struct simplelist_info info;

        simplelist_info_init(&info, str(LANG_THEME_OPTIONS),
                             theme_options_count(), NULL);
        info.get_name = file_name;
        info.get_kind = file_kind;
        info.action_callback = file_action;
        while (!to_root)
        {
            to_root = simplelist_show_list(&info);
            if (to_root || info.selection < 0)
                break;
            to_root = edit_file(info.selection);
        }
    }

    if (changed)
    {
        /* A reload is long enough to read as a hang without it */
        splash(0, ID2P(LANG_WAIT));
        settings_apply_skins();
    }
    return to_root ? MENU_ATTACHED_USB : 0;
}

/* Shown only while the loaded theme has options: the paths recorded as its
 * skins loaded decide, so opening Appearance reads no file */
static int theme_options_callback(int action,
                                  const struct menu_item_ex *this_item,
                                  struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && theme_options_count() == 0)
        return ACTION_EXIT_MENUITEM;
    return action;
}

MENUITEM_FUNCTION(theme_options_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_THEME_OPTIONS), theme_options_menu,
                  theme_options_callback, Icon_NOICON);
