/***************************************************************************
 * Original code from RockBox
 * was: apps/shortcuts.c
 * Copyright (C) 2011 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * The shortcuts menu: a user-editable list of jumps to files, directories,
 * settings or screens, persisted to disk.
 *
 * Shortcuts are held in a chain of small buflib allocations rather than one
 * array, because the list grows as the user adds entries. Each handle holds
 * SHORTCUTS_PER_HANDLE shortcuts plus the *handle id* of the next, so
 * get_shortcut() walks the chain to reach an index. Linking by id rather than
 * by pointer is what lets buflib move the blocks freely -- the chain never
 * needs fixing up.
 *
 * What does need care is the pointer get_shortcut() returns, which is raw and
 * dies at the next yield. This file guards it with buflib_move_lock, a
 * hand-rolled pin: the move_callback refuses every move while the count is
 * above zero. Increment it around anything that yields (file I/O) while
 * holding a shortcut pointer, and decrement it on every path out.
 *
 * Parts, in order:
 *   - the handle chain: allocate, look up by index, remove, and the move veto
 *   - reading shortcuts.txt into the chain, and writing it back
 *   - the list screen: names, icons, voicing and per-item actions
 *   - do_shortcut_menu(), which runs a chosen shortcut
 ****************************************************************************/

#include <stdbool.h>
#include <stdlib.h>
#include <dir.h>
#include "config.h"
#include "system.h"
#include "powermgmt.h"
#include "power.h"
#include "kernel.h"
#include "rbpaths.h"
#include "input/action.h"
#include "ata_idle_notify.h"
#include "screens/system/debug_menu.h"
#include "core_alloc.h"
#include "widgets/list.h"
#include "settings/settings.h"
#include "settings/settings_list.h"
#include "string-extra.h"
#include "lang.h"
#include "widgets/menu.h"
#include "widgets/keyboard.h"
#include "system/activity.h"
#include "system/app_util.h"
#include "system/format_time.h"
#include "system/strutil.h"
#include "screens/browse/browser.h"
#include "widgets/splash.h"
#include "pathfuncs.h"
#include "files/filetypes.h"
#include "shortcuts.h"
#include "context_menu.h"
#include "system/app_util.h"
#include "speech/talk.h"
#include "widgets/yesno.h"

#define MAX_SHORTCUT_NAME 64
#define SHORTCUTS_HDR "[shortcut]"
#define SHORTCUTS_FILENAME ROCKBOX_DIR "/shortcuts.txt"
static const char * const type_strings[SHORTCUT_TYPE_COUNT] = {
    [SHORTCUT_SETTING] = "setting",
    [SHORTCUT_SETTING_APPLY] = "apply",
    [SHORTCUT_DEBUGITEM] = "debug",
    [SHORTCUT_BROWSER] = "browse",
    [SHORTCUT_PLAYLISTMENU] = "playlist menu",
    [SHORTCUT_SEPARATOR] = "separator",
    [SHORTCUT_SHUTDOWN] = "shutdown",
    [SHORTCUT_REBOOT] = "reboot",
    [SHORTCUT_TIME] = "time",
    [SHORTCUT_FILE] = "file",
};

struct shortcut {
    enum shortcut_type type;
    int8_t icon;
    char name[MAX_SHORTCUT_NAME];
    char talk_clip[MAX_PATH];
    const struct settings_list *setting;
    union {
        char path[MAX_PATH];
        struct {
            bool talktime;
            int sleep_timeout;
        } timedata;
    } u;
};

#define SHORTCUTS_PER_HANDLE 4
struct shortcut_handle {
    struct shortcut shortcuts[SHORTCUTS_PER_HANDLE];
    int next_handle;
};
static int first_handle = 0;
static int shortcut_count = 0;

static int buflib_move_lock;
static int move_callback(int handle, void *current, void *new)
{
    (void)handle;
    (void)current;
    (void)new;

    if (buflib_move_lock > 0)
        return BUFLIB_CB_CANNOT_MOVE;

    return BUFLIB_CB_OK;
}
static struct buflib_callbacks shortcut_ops = {
    .move_callback = move_callback,
};

static void reset_shortcuts(void)
{
    int current_handle = first_handle;
    struct shortcut_handle *h = NULL;
    while (current_handle > 0)
    {
        int next;
        h = core_get_data(current_handle);
        next = h->next_handle;
        core_free(current_handle);
        current_handle = next;
    }
    first_handle = 0;
    shortcut_count = 0;
}

static struct shortcut_handle * alloc_first_sc_handle(void)
{
    struct shortcut_handle *h = NULL;
#if 0 /* all paths are guarded, compiler doesn't recognize INIT_ATTR callers */
    if (first_handle != 0)
        return h; /* No re-init allowed */
#endif
    first_handle = core_alloc_ex(sizeof(struct shortcut_handle), &shortcut_ops);
    if (first_handle > 0)
    {
        h = core_get_data(first_handle);
        h->next_handle = 0;
    }

    return h;
}

static struct shortcut* get_shortcut(int index, struct shortcut *fail)
{
    int handle_count, handle_index;
    int current_handle = first_handle;
    struct shortcut_handle *h;

    if (first_handle == 0)
    {
        h = alloc_first_sc_handle();
        if (!h)
            return fail;
        current_handle = first_handle;
    }

    handle_count = index/SHORTCUTS_PER_HANDLE + 1;
    handle_index = index%SHORTCUTS_PER_HANDLE;
    do {
        h = core_get_data(current_handle);
        current_handle = h->next_handle;
        handle_count--;
        if(handle_count <= 0)
            return &h->shortcuts[handle_index];
    } while (current_handle > 0);

    if (handle_index == 0)
    {
        /* prevent invalidation of 'h' during compaction */
        ++buflib_move_lock;
        h->next_handle = core_alloc_ex(sizeof(struct shortcut_handle), &shortcut_ops);
        --buflib_move_lock;
        if (h->next_handle <= 0)
            return fail;
        h = core_get_data(h->next_handle);
        h->next_handle = 0;
    }
    return &h->shortcuts[handle_index];
}

static void remove_shortcut(int index)
{
    int this = index, next = index + 1;
    struct shortcut *prev = get_shortcut(this, NULL);

    while (next <= shortcut_count)
    {
        struct shortcut *sc = get_shortcut(next, NULL);
        memcpy(prev, sc, sizeof(struct shortcut));
        next++;
        prev = sc;
    }
    shortcut_count--;
}

static bool verify_shortcut(struct shortcut* sc)
{
    switch (sc->type)
    {
        case SHORTCUT_UNDEFINED:
            return false;
        case SHORTCUT_BROWSER:
        case SHORTCUT_FILE:
        case SHORTCUT_PLAYLISTMENU:
            return sc->u.path[0] != '\0';
        case SHORTCUT_SETTING_APPLY:
        case SHORTCUT_SETTING:
            return sc->setting != NULL;
        case SHORTCUT_TIME:
        case SHORTCUT_DEBUGITEM:
        case SHORTCUT_SEPARATOR:
        case SHORTCUT_SHUTDOWN:
        case SHORTCUT_REBOOT:
        default:
            break;
    }
    return true;
}

static void init_shortcut(struct shortcut* sc)
{
    memset(sc, 0, sizeof(*sc));
    sc->type = SHORTCUT_UNDEFINED;
    sc->icon = Icon_NOICON;
}

static int first_idx_to_writeback = -1;
static bool overwrite_shortcuts = false;
static void shortcuts_ata_idle_callback(void)
{
    int fd;

    int current_idx = first_idx_to_writeback;
    int append = overwrite_shortcuts ? O_TRUNC : O_APPEND;

    if (first_idx_to_writeback < 0)
        return;

    /* This runs on the storage thread. A lock already taken means the
     * menu is up and something is holding a raw shortcut pointer, and
     * the reload at the end of this function frees the whole chain --
     * which the lock does not prevent, since a free is not a move. Come
     * back when the menu has closed; call_storage_idle_notifys() will
     * not ask again for another thirty seconds. */
    if (buflib_move_lock > 0)
    {
        register_storage_idle_func(shortcuts_ata_idle_callback);
        return;
    }

    fd = open(SHORTCUTS_FILENAME, append|O_RDWR|O_CREAT, 0644);
    if (fd < 0)
        return;

    /* ensure pointer returned by get_shortcut()
       survives the yield() of write() */
    ++buflib_move_lock;

    while (current_idx < shortcut_count)
    {
        struct shortcut* sc = get_shortcut(current_idx++, NULL);
        const char *type;

        if (!sc)
            break;
        type = type_strings[sc->type];

        fdprintf(fd, SHORTCUTS_HDR "\ntype: %s\ndata: ", type);

        if (sc->type == SHORTCUT_TIME)
        {
            if (sc->u.timedata.talktime)
                write(fd, "talk", 4);
            else
            {
                write(fd, "sleep", 5);
                if(sc->u.timedata.sleep_timeout >= 0)
                    fdprintf(fd, " %d", sc->u.timedata.sleep_timeout);
            }
        }
        else if (sc->type == SHORTCUT_SETTING_APPLY)
        {
            fdprintf(fd, "%s: %s", sc->setting->cfg_name, sc->u.path);
        }
        else if (sc->type == SHORTCUT_SETTING)
        {
            write(fd, sc->setting->cfg_name, strlen(sc->setting->cfg_name));
        }
        else
            write(fd, sc->u.path, strlen(sc->u.path));

        /* write name:, icon:, talkclip: */
        fdprintf(fd, "\nname: %s\nicon: %d\ntalkclip: %s\n\n",
                 sc->name, sc->icon, sc->talk_clip);
    }
    close(fd);
    if (first_idx_to_writeback == 0)
    {
        /* reload all shortcuts because we appended to the shortcuts file which
         * has not been read yet.
         */
         reset_shortcuts();
         shortcuts_init();
    }
    --buflib_move_lock;
    first_idx_to_writeback = -1;
}

void shortcuts_add(enum shortcut_type type, const char* value)
{
    struct shortcut* sc = get_shortcut(shortcut_count++, NULL);
    if (!sc)
        return;
    init_shortcut(sc);
    sc->type = type;
    if (type == SHORTCUT_SETTING || type == SHORTCUT_SETTING_APPLY)
    {
        sc->setting = (void*)value;
        /* write the current value, will be ignored for SHORTCUT_SETTING */
        cfg_to_string(sc->setting, sc->u.path,  sizeof(sc->u.path));
    }
    else
        strmemccpy(sc->u.path, value, MAX_PATH);

    if (first_idx_to_writeback < 0)
        first_idx_to_writeback = shortcut_count - 1;
    overwrite_shortcuts = false;
    register_storage_idle_func(shortcuts_ata_idle_callback);
}

static int readline_cb(int n, char *buf, void *parameters)
{
    (void)n;
    (void)parameters;
    struct shortcut **param = (struct shortcut**)parameters;
    struct shortcut* sc = *param;
    char *name, *value;

    buf = skip_whitespace(buf);

    if (!strcasecmp(buf, SHORTCUTS_HDR))
    {
        if (sc && verify_shortcut(sc))
            shortcut_count++;
        sc = get_shortcut(shortcut_count, NULL);
        if (!sc)
            return 1;
        init_shortcut(sc);
        *param = sc;
    }
    else if (sc && settings_parseline(buf, &name, &value))
    {
        static const char * const nm_options[] = {"type", "name", "data",
                                                  "icon", "talkclip", NULL};
        int nm_op = string_option(name, nm_options, false);

        if (nm_op == 0) /*type*/
        {
            int t = 0;
            for (t=0; t<SHORTCUT_TYPE_COUNT && sc->type == SHORTCUT_UNDEFINED; t++)
                if (!strcmp(value, type_strings[t]))
                    sc->type = t;
        }
        else if (nm_op == 1) /*name*/
        {
            strmemccpy(sc->name, value, MAX_SHORTCUT_NAME);
        }
        else if (nm_op == 2) /*data*/
        {
            switch (sc->type)
            {
                case SHORTCUT_UNDEFINED:
                case SHORTCUT_TYPE_COUNT:
                    *param = NULL;
                    break;
                case SHORTCUT_BROWSER:
                {
                    char *p = strmemccpy(sc->u.path, value, MAX_PATH);
                    if (p && dir_exists(value))
                    {
                        /* ensure ending slash */
                        *p = '\0';
                        if (*(p-2) != '/')
                            *(p-1) = '/';
                    }
                    break;
                }
                case SHORTCUT_FILE:
                case SHORTCUT_DEBUGITEM:
                case SHORTCUT_PLAYLISTMENU:
                {
                    strmemccpy(sc->u.path, value, MAX_PATH);
                    break;
                }
                case SHORTCUT_SETTING_APPLY:
                case SHORTCUT_SETTING:
                    /* can handle 'name: value' pair for either type */
                    if (settings_parseline(value, &name, &value))
                    {
                        sc->setting = find_setting_by_cfgname(name);
                        strmemccpy(sc->u.path, value, MAX_PATH);
                    }
                    else /* force SHORTCUT_SETTING, no 'name: value' pair */
                    {
                        sc->type = SHORTCUT_SETTING;
                        sc->setting = find_setting_by_cfgname(value);
                    }
                    break;
                case SHORTCUT_TIME:
                    sc->u.timedata.talktime = false;
                    if (!strcasecmp(value, "talk"))
                        sc->u.timedata.talktime = true;
                    else
                    if (!strncasecmp(value, "sleep", sizeof("sleep")-1))
                    {
                        /* 'sleep' may appear alone or followed by number after a space */
                        if (strlen(value) > sizeof("sleep")) /* sizeof 1 larger (+space chr..) */
                            sc->u.timedata.sleep_timeout = atoi(&value[sizeof("sleep")-1]);
                        else
                            sc->u.timedata.sleep_timeout = -1;
                    }
                    else
                        sc->type = SHORTCUT_UNDEFINED; /* error */
                    break;
                case SHORTCUT_SEPARATOR:
                case SHORTCUT_SHUTDOWN:
                case SHORTCUT_REBOOT:
                    break;
            }
        }
        else if (nm_op == 3) /*icon*/
        {
            if (!strcmp(value, "filetype") && sc->type != SHORTCUT_SETTING
                && sc->type != SHORTCUT_SETTING_APPLY && sc->u.path[0])
            {
                sc->icon = filetype_get_icon(filetype_get_attr(sc->u.path));
            }
            else
            {
                sc->icon = atoi(value);
            }
        }
        else if (nm_op == 4) /*talkclip*/
        {
            strmemccpy(sc->talk_clip, value, MAX_PATH);
        }
    }
    return 0;
}

void shortcuts_init(void)
{
    int fd;
    char buf[512];
    struct shortcut *param = NULL;
    struct shortcut_handle *h;
    shortcut_count = 0;
    fd = open_utf8(SHORTCUTS_FILENAME, O_RDONLY);
    if (fd < 0)
        return;
    h = alloc_first_sc_handle();
    if (!h) {
        close(fd);
        return;
    }

    /* we enter readline_cb() multiple times with a buffer
       obtained when we encounter a SHORTCUTS_HDR ("[shortcut]") section.
       fast_readline() might yield() -> protect buffer */
    ++buflib_move_lock;

    fast_readline(fd, buf, sizeof buf, &param, readline_cb);
    close(fd);
    if (param && verify_shortcut(param))
        shortcut_count++;

    /* invalidate at scope end since "param" contains
       the shortcut pointer */
    --buflib_move_lock;
}

/* Cheap probe for the root menu: true if there is at least one shortcut. The
 * in-RAM count comes first because shortcuts_add() writes the file only at
 * storage idle, so a shortcut added a moment ago is not in it yet. The file is
 * still read for one copied over USB, which the in-RAM list never sees. Its
 * first [shortcut] header sits at the top, so the head of the file is enough. */
bool shortcuts_available(void)
{
    char buf[512];
    if (shortcut_count > 0)
        return true;
    int fd = open_utf8(SHORTCUTS_FILENAME, O_RDONLY);
    if (fd < 0)
        return false;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    return strstr(buf, SHORTCUTS_HDR) != NULL;
}

/* The shortcut being carried by Move, and where it was picked up; -1 when
 * nothing is moving */
static int moving_item = -1;
static int moving_from;

static const char * shortcut_label(int selected_item, char * buffer,
                                   size_t buffer_len)
{
    struct shortcut *sc = get_shortcut(selected_item, NULL);
    if (!sc)
        return "";
    const char *ret = sc->u.path;

    if (sc->type == SHORTCUT_SETTING && sc->name[0] == '\0')
        return P2STR(ID2P(sc->setting->lang_id));
    else if (sc->type == SHORTCUT_SETTING_APPLY && sc->name[0] == '\0')
    {
        snprintf(buffer, buffer_len, "%s (%s)",
                 P2STR(ID2P(sc->setting->lang_id)), sc->u.path);
        return buffer;
    }
    else if (sc->type == SHORTCUT_SEPARATOR)
        return sc->name;
    else if (sc->type == SHORTCUT_TIME)
    {
        if (sc->u.timedata.sleep_timeout < 0
            && !sc->u.timedata.talktime
        ) /* String representation for toggling sleep timer */
            return string_sleeptimer(buffer, buffer_len);

        if (sc->name[0])
            return sc->name;

        if (sc->u.timedata.talktime)
            return P2STR(ID2P(LANG_CURRENT_TIME));
        else
        {
            format_sleeptimer(sc->name, sizeof(sc->name),
                              sc->u.timedata.sleep_timeout, NULL);
            snprintf(buffer, buffer_len, "%s (%s)",
                     P2STR(ID2P(LANG_SLEEP_TIMER)),
                     sc->name[0] ? sc->name : P2STR(ID2P(LANG_OFF)));
            sc->name[0] = '\0';
            return buffer;
        }

    }
    else if ((sc->type == SHORTCUT_SHUTDOWN || sc->type == SHORTCUT_REBOOT) &&
             sc->name[0] == '\0')
    {
        /* No translation support as only soft_shutdown has LANG_SHUTDOWN defined */
        return type_strings[sc->type];
    }
    else if (sc->type == SHORTCUT_BROWSER && sc->name[0] == '\0' && (sc->u.path)[0] != '\0')
    {
        char* pos;
        if (path_basename(sc->u.path, (const char **)&pos) > 0)
        {
            if (snprintf(buffer, buffer_len, "%s (%s)", pos, sc->u.path) < (int)buffer_len)
                return buffer;
        }
    }

    return sc->name[0] ? sc->name : ret;
}

/* The carried shortcut is marked in its text as well as by its icon, as the
 * main menu's editor marks it: a theme need never map Icon_Moving. */
static const char * shortcut_menu_get_name(int selected_item, void * data,
                                           char * buffer, size_t buffer_len)
{
    char label[MAX_PATH];

    (void)data;
    if (selected_item != moving_item)
        return shortcut_label(selected_item, buffer, buffer_len);
    snprintf(buffer, buffer_len, ">%s",
             shortcut_label(selected_item, label, sizeof(label)));
    return buffer;
}

static int shortcut_menu_speak_item(int selected_item, void * data)
{
    (void)data;
    struct shortcut *sc = get_shortcut(selected_item, NULL);
    if (sc)
    {
        if (sc->talk_clip[0])
        {
            talk_file(NULL, NULL, sc->talk_clip, NULL, NULL, false);
        }
        else
        {
            switch (sc->type)
            {
            case SHORTCUT_BROWSER:
                {
                    DIR* dir;
                    struct dirent* entry;
                    char* slash = strrchr(sc->u.path, PATH_SEPCH);
                    char* filename = slash + 1;
                    if (slash && *filename != '\0')
                    {
                        *slash = '\0'; /* terminate the path to open the directory */
                        dir = opendir(sc->u.path);
                        *slash = PATH_SEPCH; /* restore fullpath */
                        if (dir)
                        {
                            while (0 != (entry = readdir(dir)))
                            {
                                if (!strcmp(entry->d_name, filename))
                                {
                                    struct dirinfo info = dir_get_info(dir, entry);

                                    if (info.attribute & ATTR_DIRECTORY)
                                        talk_dir_or_spell(sc->u.path, NULL, false);
                                    else
                                        talk_file_or_spell(NULL, sc->u.path, NULL, false);

                                    closedir(dir);
                                    return 0;
                                }
                            }
                            closedir(dir);
                        }
                    }
                    else
                    {
                        talk_dir_or_spell(sc->u.path, NULL, false);
                        break;
                    }
                    talk_name(sc->u.path, false);
                }
                break;
            case SHORTCUT_FILE:
            case SHORTCUT_PLAYLISTMENU:
                talk_file_or_spell(NULL, sc->u.path, NULL, false);
                break;
            case SHORTCUT_SETTING_APPLY:
            case SHORTCUT_SETTING:
                talk_id(sc->setting->lang_id, false);
                if (sc->type == SHORTCUT_SETTING_APPLY)
                    talk_name(sc->u.path, true);
                break;

            case SHORTCUT_TIME:
                if (sc->u.timedata.talktime)
                    talk_timedate();
                else
                if (sc->name[0] && sc->u.timedata.sleep_timeout >= 0)
                    talk_name(sc->name, false);
                else
                    talk_sleeptimer(sc->u.timedata.sleep_timeout);
                break;
            case SHORTCUT_SHUTDOWN:
            case SHORTCUT_REBOOT:
                if (!sc->name[0])
                {
                    talk_name(type_strings[sc->type], false);
                    break;
                }
                /* fall-through */
            default:
                talk_name(sc->name[0] ? sc->name : sc->u.path, false);
                break;
            }
        }
    }
    return 0;
}

/* The whole list again, written back once the disk is next idle */
static void shortcuts_changed(void)
{
    first_idx_to_writeback = 0;
    overwrite_shortcuts = true;
    shortcuts_ata_idle_callback();
}

/* Move carries the shortcut by swapping it along to wherever the cursor is,
 * so the list never shows it anywhere but under the cursor */
static void swap_shortcuts(int a, int b)
{
    struct shortcut tmp;
    struct shortcut *sa = get_shortcut(a, NULL);
    struct shortcut *sb = get_shortcut(b, NULL);

    if (!sa || !sb)
        return;
    memcpy(&tmp, sa, sizeof(tmp));
    memcpy(sa, sb, sizeof(tmp));
    memcpy(sb, &tmp, sizeof(tmp));
}

static void carry_to(int to)
{
    while (moving_item < to)
    {
        swap_shortcuts(moving_item, moving_item + 1);
        moving_item++;
    }
    while (moving_item > to)
    {
        swap_shortcuts(moving_item, moving_item - 1);
        moving_item--;
    }
}

/* The list has already moved the cursor for whatever was pressed by the time
 * this sees it, so the shortcut follows the cursor rather than the button */
static int moving_action(int action, struct gui_synclist *lists)
{
    int sel = gui_synclist_get_sel_pos(lists);

    if (IS_SYSEVENT(action))
    {
        /* System events go on to the list. USB takes the screen, so the
         * shortcut goes back where it was first and the list in memory
         * still matches the file. */
        if (action == SYS_USB_CONNECTED)
        {
            carry_to(moving_from);
            gui_synclist_select_item(lists, moving_from);
            moving_item = -1;
        }
        return action;
    }

    if (sel != moving_item)
    {
        carry_to(sel);
        return ACTION_REDRAW;
    }

    switch (action)
    {
        case ACTION_STD_OK:
            moving_item = -1;
            shortcuts_changed();
            return ACTION_REDRAW;
        case ACTION_STD_CANCEL:
            /* Put back where it was */
            carry_to(moving_from);
            gui_synclist_select_item(lists, moving_from);
            moving_item = -1;
            return ACTION_REDRAW;
        case ACTION_NONE:
        case ACTION_REDRAW:
        case ACTION_STD_NEXT:
        case ACTION_STD_NEXTREPEAT:
        case ACTION_STD_PREV:
        case ACTION_STD_PREVREPEAT:
            return action;
        default:
            /* Nothing else while a shortcut is in hand */
            return ACTION_NONE;
    }
}

static void rename_shortcut(int index)
{
    char name[MAX_SHORTCUT_NAME];
    char label[MAX_PATH];
    struct shortcut *sc = get_shortcut(index, NULL);

    if (!sc)
        return;
    strmemccpy(name, shortcut_label(index, label, sizeof(label)), sizeof(name));
    if (kbd_input(name, sizeof(name), NULL) < 0)
        return;

    sc = get_shortcut(index, NULL);
    if (!sc)
        return;
    strmemccpy(sc->name, name, sizeof(sc->name));
    shortcuts_changed();
}

/* Delete asks first, as it always has here. False when the last shortcut has
 * gone and the list has nothing left to show. */
static bool delete_shortcut(struct gui_synclist *lists, int selection)
{
    if (confirm_delete_yesno("", str(LANG_SHORTCUTS)) != YESNO_YES)
        return true;

    remove_shortcut(selection);
    gui_synclist_set_nb_items(lists, shortcut_count);
    if (selection >= shortcut_count)
        gui_synclist_select_item(lists, shortcut_count - 1);
    shortcuts_changed();
    return shortcut_count > 0;
}

static int shortcut_menu_get_action(int action, struct gui_synclist *lists)
{
    if (moving_item >= 0)
        return moving_action(action, lists);

    if (action == ACTION_STD_CONTEXT)
    {
        int selection = gui_synclist_get_sel_pos(lists);
        int choice;
        MENUITEM_STRINGLIST(menu, ID2P(LANG_SHORTCUTS), NULL,
                            ID2P(LANG_MOVE), ID2P(LANG_RENAME),
                            ID2P(LANG_DELETE));

        push_current_activity(ACTIVITY_CONTEXTMENU);
        choice = do_menu(&menu, NULL, NULL, false);
        if (get_current_activity() == ACTIVITY_CONTEXTMENU)
            pop_current_activity();

        switch (choice)
        {
            case MENU_ATTACHED_USB:
                return ACTION_STD_CANCEL;
            case 0:
                if (shortcut_count > 1)
                    moving_item = moving_from = selection;
                break;
            case 1:
                rename_shortcut(selection);
                break;
            case 2:
                if (!delete_shortcut(lists, selection))
                    return ACTION_STD_CANCEL;
                break;
        }

        gui_synclist_set_title(lists, lists->title, lists->title_icon);
        if (global_settings.talk_menu)
            shortcut_menu_speak_item(gui_synclist_get_sel_pos(lists), NULL);
        return ACTION_REDRAW;
    }

    if (action == ACTION_STD_OK
     || action == ACTION_STD_QUICKSCREEN)
    {
        return ACTION_STD_CANCEL;
    }

    return action;
}

static enum themable_icons shortcut_menu_get_icon(int selected_item, void * data)
{
    static const int8_t type_icons[SHORTCUT_TYPE_COUNT] = {
        [SHORTCUT_SETTING] = Icon_Menu_setting,
        [SHORTCUT_SETTING_APPLY] = Icon_Queued,
        [SHORTCUT_DEBUGITEM] = Icon_Menu_functioncall,
        [SHORTCUT_BROWSER] = Icon_Folder,
        [SHORTCUT_PLAYLISTMENU] = Icon_Playlist,
        [SHORTCUT_SEPARATOR] = Icon_NOICON,
        [SHORTCUT_SHUTDOWN] = Icon_System_menu,
        [SHORTCUT_REBOOT] = Icon_System_menu,
        [SHORTCUT_TIME] = Icon_Menu_functioncall,
        [SHORTCUT_FILE] = Icon_NOICON,
    };

    (void)data;
    int icon;
    struct shortcut *sc = get_shortcut(selected_item, NULL);
    if (!sc)
        return Icon_NOICON;
    if (selected_item == moving_item)
        return Icon_Moving;
    if (sc->icon == Icon_NOICON)
    {
        if (sc->type == SHORTCUT_BROWSER || sc->type == SHORTCUT_FILE)
        {
            icon = filetype_get_icon(filetype_get_attr(sc->u.path));
            if (icon > 0)
                return icon;
        }
        /* excluding SHORTCUT_UNDEFINED (-1) */
        if (sc->type >= 0 && sc->type < SHORTCUT_TYPE_COUNT)
            return type_icons[sc->type];
    }
    return sc->icon;
}

static void apply_new_setting(const struct settings_list *setting)
{
    settings_apply(false);
    if ((setting->flags & (F_THEMESETTING|F_NEEDAPPLY)) == (F_THEMESETTING|F_NEEDAPPLY))
    {
            settings_apply_skins();
    }
    if (setting->setting == &global_settings.sleeptimer_duration && get_sleep_timer())
    {
        set_sleeptimer_duration(global_settings.sleeptimer_duration);
    }
}

int do_shortcut_menu(void *ignored)
{
    (void)ignored;
    struct simplelist_info list;
    struct shortcut *sc;
    int done = GO_TO_PREVIOUS;
    if (first_handle == 0)
        shortcuts_init();
    simplelist_info_init(&list, P2STR(ID2P(LANG_SHORTCUTS)), shortcut_count, NULL);
    list.get_name = shortcut_menu_get_name;
    list.action_callback = shortcut_menu_get_action;
    list.title_icon = Icon_Bookmark;
    list.wps_on_play = true;   /* this screen can return GO_TO_WPS */

    if (shortcut_count == 0)
    {
        splash(HZ, ID2P(LANG_NO_FILES));
        return GO_TO_PREVIOUS;
    }
    push_current_activity(ACTIVITY_SHORTCUTSMENU);

    /* prevent buffer moves while the menu is active.
       Playing talk files or other I/O actions call yield() */
    ++buflib_move_lock;

    while (done == GO_TO_PREVIOUS)
    {
        list.count = shortcut_count; /* in case shortcut was deleted */
        list.title = P2STR(ID2P(LANG_SHORTCUTS)); /* in case language changed */
        list.get_icon = global_settings.show_icons ? shortcut_menu_get_icon : NULL;
        list.get_talk = global_settings.talk_menu ? shortcut_menu_speak_item : NULL;

        if (simplelist_show_list(&list))
        {
            /* MENU, or a USB attach: out to the root rather than back to
             * whichever screen opened this one. */
            done = GO_TO_ROOT;
            break;
        }

        if (list.selection == -1)
            break;
        else if (list.selection == -2)
        {
            /* PLAY: straight to the playing screen rather than back. */
            done = GO_TO_WPS;
            break;
        }
        else
        {
            sc = get_shortcut(list.selection, NULL);

            if (!sc)
                continue;

            switch (sc->type)
            {
                case SHORTCUT_PLAYLISTMENU:
                    if (!file_exists(sc->u.path))
                    {
                        splash(HZ, ID2P(LANG_NO_FILES));
                        break;
                    }
                    else
                    {
                        context_menu_show_playlist(sc->u.path,
                                                  dir_exists(sc->u.path) ? ATTR_DIRECTORY :
                                                  filetype_get_attr(sc->u.path),
                                                  NULL, false);
                    }
                    break;
                case SHORTCUT_FILE:
                    if (!file_exists(sc->u.path))
                    {
                        splash(HZ, ID2P(LANG_NO_FILES));
                        break;
                    }
                    /* else fall through */
                case SHORTCUT_BROWSER:
                {
                    struct browse_context browse = {
                        .dirfilter = SHOW_ALL, /* ignored for SHORTCUT_BROWSER */
                        .icon = Icon_NOICON,
                        .root = sc->u.path,
                    };
                    /* A file shortcut must open its file even when the current
                     * dir filter hides it -- otherwise the entry is not in the
                     * tree and running it fails with "Failed reading". Only
                     * 'browse' shortcuts keep the global filter. */
                    if (sc->type == SHORTCUT_FILE)
                        browse.flags = BROWSE_RUNFILE | BROWSE_DIRFILTER;
                    done = rockbox_browse(&browse);

                }
                break;
                case SHORTCUT_SETTING_APPLY:
                {
                    bool theme_changed;
                    string_to_cfg(sc->setting->cfg_name, sc->u.path, &theme_changed);
                    settings_save();
                    apply_new_setting(sc->setting);
                    break;
                }
                case SHORTCUT_SETTING:
                {
                    do_setting_screen(sc->setting,
                        sc->name[0] ? sc->name : P2STR(ID2P(sc->setting->lang_id)),NULL);
                    apply_new_setting(sc->setting);
                    break;
                }
                case SHORTCUT_DEBUGITEM:
                    if (run_debug_screen(sc->u.path))
                        done = GO_TO_ROOT;
                    break;
                case SHORTCUT_SHUTDOWN:
#if CONFIG_CHARGING && !defined(HAVE_POWEROFF_WHILE_CHARGING)
                    if (charger_inserted())
                        charging_splash();
                    else
#endif
                        sys_poweroff();
                    break;
                case SHORTCUT_REBOOT:
#if CONFIG_CHARGING && !defined(HAVE_POWEROFF_WHILE_CHARGING)
                    if (charger_inserted())
                        charging_splash();
                    else
#endif
                        sys_reboot();
                    break;
                case SHORTCUT_TIME:
                  if (!sc->u.timedata.talktime)
                    {
                        char timer_buf[10];
                        if (sc->u.timedata.sleep_timeout >= 0)
                        {
                            set_sleeptimer_duration(sc->u.timedata.sleep_timeout);
                            splashf(HZ, "%s (%s)", str(LANG_SLEEP_TIMER),
                                    format_sleeptimer(timer_buf, sizeof(timer_buf),
                                                          sc->u.timedata.sleep_timeout,
                                                          NULL));
                        }
                        else
                            toggle_sleeptimer();
                    }
                    break;
                case SHORTCUT_UNDEFINED:
                default:
                    break;
            }
        }
    }
    if (GO_TO_PLUGIN == done)
        pop_current_activity_without_refresh();
    else
        pop_current_activity();
    --buflib_move_lock;

    return done;
}
