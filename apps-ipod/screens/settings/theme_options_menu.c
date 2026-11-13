/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Appearance > Theme Options: the loaded theme's options files as a menu.
 * skin/theme_options.c reads and writes the files; this is the screen.
 *
 * One options file opens straight onto its options; more than one opens on
 * a row per file. Each change is written when it is made, and the skins
 * read it at their next draw. Only a file some %ft reads at load alone (see
 * theme_options_live()) has the theme reload once, on leaving, and Now
 * Playing's menu and buttons do not offer it while the theme has such a file.
 *
 * Theme Toggle, at the end, is the button action that moves one option on
 * to its next choice. The owner picks the option from its context menu here;
 * the buttons offer the action only while that option's theme is loaded.
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
#include "widgets/dialog_prose.h"
#include "widgets/yesno.h"
#include "draw/viewport.h"
#include "skin/skin_engine.h"
#include "skin/statusbar_skinned.h"
#include "skin/theme_options.h"
#include "system/activity.h"
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

static bool changed;            /* a value was written */
static bool reload;             /* ...to a file not every %ft reads live */

static void written(void)
{
    changed = true;
    if (!theme_options_live_only(f.prefs))
        reload = true;
}

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
    written();
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
    return dialog_prose_show(title, help);
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
    written();
}

static bool toggle_split(char *key, const char **path);

/* Whether option n is the one Theme Toggle changes */
static bool is_toggle(int n)
{
    char key[THEME_OPTION_KEY_MAX + 1];
    const char *path;

    return toggle_split(key, &path) && !strcmp(key, f.opts[n].key)
           && !strcmp(path, f.prefs);
}

static void set_toggle(int n, bool on)
{
    if (on)
        snprintf(global_settings.theme_toggle,
                 sizeof global_settings.theme_toggle, "%s:%s",
                 f.opts[n].key, f.prefs);
    else
        global_settings.theme_toggle[0] = '\0';
    settings_save();
}

/* The option's context menu: Explain if it has help, Reset, and for a
 * choice the buttons can reach, assigning it to Theme Toggle */
static bool option_menu(int sel)
{
    enum { ROW_EXPLAIN, ROW_RESET, ROW_ASSIGN, ROW_UNASSIGN };
    static const struct menu_callback_with_desc desc =
        { NULL, ID2P(LANG_ONPLAY_MENU_TITLE), Icon_NOICON };
    const struct theme_option *o = &f.opts[sel];
    const char *rows[3];
    int what[3], n = 0, pick;

    if (o->help[0])
    {
        what[n] = ROW_EXPLAIN;
        rows[n++] = ID2P(LANG_EXPLAIN);
    }
    what[n] = ROW_RESET;
    rows[n++] = ID2P(LANG_RESET_SETTING);
    if (o->type == THEME_OPTION_CHOICE && theme_options_live_only(f.prefs))
    {
        bool on = is_toggle(sel);

        what[n] = on ? ROW_UNASSIGN : ROW_ASSIGN;
        rows[n++] = on ? ID2P(LANG_THEME_TOGGLE_UNASSIGN)
                       : ID2P(LANG_THEME_TOGGLE_ASSIGN);
    }

    const struct menu_item_ex menu =
        { MT_RETURN_ID | MENU_HAS_DESC | MENU_ITEM_COUNT(n),
          { .strings = rows }, { .callback_and_desc = &desc } };

    pick = do_menu(&menu, NULL, NULL, false);
    if (pick == MENU_ATTACHED_USB)
        return true;
    if (pick < 0 || pick >= n)
        return false;
    switch (what[pick])
    {
        case ROW_EXPLAIN:
            return show_help(o->name, o->help);
        case ROW_RESET:
            set_value(sel, o->def);
            break;
        case ROW_ASSIGN:
        case ROW_UNASSIGN:
            set_toggle(sel, what[pick] == ROW_ASSIGN);
            break;
    }
    return false;
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
        to_root = option_menu(sel);
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

    changed = reload = false;
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

    if (reload)
    {
        /* A reload is long enough to read as a hang without it */
        splash(0, ID2P(LANG_WAIT));
        settings_apply_skins();
    }
    else if (changed)
    {
        /* A %ft line redraws only on a full update */
        for (int i = 0; i < SKINNABLE_SCREENS_COUNT; i++)
            skin_request_full_update(i);
    }
    return to_root ? MENU_ATTACHED_USB : 0;
}

/* Whether every options file changes without a reload. Now Playing's menu
 * offers the row only then: a reload releases the audio buffer, which stops
 * the track the menu was opened over. */
static bool all_live(void)
{
    for (int i = 0; i < theme_options_count(); i++)
        if (!theme_options_live_only(theme_options_path(i)))
            return false;
    return true;
}

/* Shown only while the loaded theme has options: the paths recorded as its
 * skins loaded decide, so opening Appearance reads no file */
static int theme_options_callback(int action,
                                  const struct menu_item_ex *this_item,
                                  struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM
        && (theme_options_count() == 0
            || (get_current_activity() == ACTIVITY_CONTEXTMENU
                && !all_live())))
        return ACTION_EXIT_MENUITEM;
    return action;
}

/* Pushes the activity Appearance opens it under: a skin styles a list by
 * activity, and Now Playing's would leave these lists unstyled */
int theme_options_from_wps(void)
{
    int ret;

    if (theme_options_count() == 0 || !all_live())
    {
        splash(HZ, ID2P(LANG_UNAVAILABLE));
        return 0;
    }
    push_current_activity(ACTIVITY_SETTINGS);
    ret = theme_options_menu();
    pop_current_activity();
    return ret;
}

MENUITEM_FUNCTION(theme_options_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_THEME_OPTIONS), theme_options_menu,
                  theme_options_callback, Icon_NOICON);

/* --- Theme Toggle -------------------------------------------------------- */

/* The assignment's key, copied out, and path */
static bool toggle_split(char *key, const char **path)
{
    const char *s = global_settings.theme_toggle;
    const char *colon = strchr(s, ':');

    if (!colon || colon == s || colon - s > THEME_OPTION_KEY_MAX)
        return false;
    memcpy(key, s, colon - s);
    key[colon - s] = '\0';
    *path = colon + 1;
    return true;
}

struct toggle
{
    char key[THEME_OPTION_KEY_MAX + 1];
    const char *prefs;
    char value[THEME_OPTION_TEXT_MAX + 1];  /* the current value, in */
    char name[THEME_OPTION_NAME_MAX + 1];   /* ...and these out */
    char next[THEME_OPTION_VALUE_MAX + 1];
    bool found;
};

/* A value that is not a choice moves on from the default */
static void toggle_find(const struct theme_option *opt, void *data)
{
    struct toggle *t = data;
    int at = -1;

    if (t->found || opt->type != THEME_OPTION_CHOICE
        || strcmp(opt->key, t->key))
        return;
    for (int i = 0; i < opt->count; i++)
        if (!strcmp(opt->choices[i].value, t->value))
            at = i;
    for (int i = 0; at < 0 && i < opt->count; i++)
        if (!strcmp(opt->choices[i].value, opt->def))
            at = i;
    strcpy(t->name, opt->name);
    strcpy(t->next, opt->choices[(at + 1) % opt->count].value);
    t->found = true;
}

/* t->key of the .preferences file at `path`, if the loaded theme has it and
 * it changes without a reload. Reads its files. */
static bool toggle_try(struct toggle *t, const char *path)
{
    char opts_path[MAX_PATH];

    t->found = false;
    t->prefs = NULL;
    for (int i = 0; i < theme_options_count(); i++)
        if (!strcmp(theme_options_path(i), path))
            t->prefs = theme_options_path(i);
    if (!t->prefs || !theme_options_live_only(t->prefs)
        || !theme_options_options_path(t->prefs, opts_path, sizeof opts_path))
        return false;
    if (!theme_options_read(t->prefs, t->key, t->value, sizeof t->value))
        t->value[0] = '\0';
    theme_options_parse(opts_path, NULL, toggle_find, t);
    return t->found;
}

/* The owner's assignment while it is the loaded theme's, otherwise the
 * option the theme's options file names with `toggle` */
static bool toggle_lookup(struct toggle *t)
{
    const char *path;

    if (toggle_split(t->key, &path) && toggle_try(t, path))
        return true;
    for (int i = 0; i < theme_options_count(); i++)
    {
        const char *key = theme_options_toggle(i);

        if (!*key)
            continue;
        strcpy(t->key, key);
        if (toggle_try(t, theme_options_path(i)))
            return true;
    }
    return false;
}

const char *theme_toggle_label(void)
{
    static char label[THEME_OPTION_NAME_MAX + 32];
    struct toggle t;

    if (!toggle_lookup(&t))
        return NULL;
    snprintf(label, sizeof label, str(LANG_THEME_TOGGLE_NAMED), t.name);
    return label;
}

int theme_toggle_press(void)
{
    struct toggle t;
    const char *key = t.key, *value = t.next;

    if (!toggle_lookup(&t))
        splash(HZ, ID2P(LANG_UNAVAILABLE));
    else if (!theme_options_write(t.prefs, &key, &value, 1))
        splash(HZ, ID2P(LANG_FAILED));
    /* A %ft line redraws only on a full update, which also clears a splash */
    for (int i = 0; i < SKINNABLE_SCREENS_COUNT; i++)
        skin_request_full_update(i);
    return 0;
}
