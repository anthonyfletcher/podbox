/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Every document, or every image, in one list.
 *
 * The file browser shows what is in a folder. These show what is on the
 * player, whichever folder it happens to sit in -- which for a handful of
 * documents scattered around a music library is the difference between
 * finding one and remembering where it was put.
 *
 * Above the files sit Search and By Type, scrolled off the top: a search of
 * the names, and the files of one type -- EPUB, PNG -- in a list of their own.
 * Every list is in the order Sort Documents or Sort Images sets: by name
 * either way, or the newest first by the time each line of the list carries.
 *
 * The list comes from the background walk in files/file_index.c; nothing is
 * scanned here. Opening is left to the core viewers, through the same
 * filetype dispatch the file browser uses, so a document opens exactly as it
 * would from anywhere else -- including resuming where it was left.
 *
 * Parts, in order:
 *   - the list, and a file's type
 *   - opening a file
 *   - Search
 *   - By Type
 *   - the list on screen
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include "config.h"
#include "string-extra.h"        /* strlcpy */
#include "file.h"                /* MAX_PATH */
#include "kernel.h"
#include "system.h"              /* ARRAYLEN, MIN */
#include "lang.h"
#include "settings/settings.h"   /* ID2P */
#include "widgets/list.h"
#include "widgets/splash.h"
#include "widgets/search_dialog.h"
#include "input/action.h"
#include "system/activity.h"
#include "files/file_index.h"
#include "files/path_list.h"
#include "files/filetypes.h"
#include "system/bg_task.h"         /* a file gone: the walk again */
#include "strnatcmp.h"
#include "root_menu.h"
#include "browser_flat.h"

/* ------------------------------------------------------------------ *
 * the list, and a file's type                                        *
 * ------------------------------------------------------------------ */

static struct path_list list;
static bool flat_images;

/* The rows above the whole list */
enum { ROW_SEARCH, ROW_BY_TYPE, HEAD_ROWS };

/* The files a list on screen shows, as indices into `list` */
static int shown[PATH_LIST_MAX];
static int shown_ct;
static int heads;               /* HEAD_ROWS above them, or none */

#define TYPE_MAX 8

/* Line `i`'s path, past the time before it */
static const char *flat_path(int i)
{
    const char *line = path_list_get(&list, i);
    const char *tab = strchr(line, '\t');

    return tab ? tab + 1 : line;
}

static unsigned long flat_time(int i)
{
    return strtoul(path_list_get(&list, i), NULL, 16);
}

static int compare_files(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    int order = flat_images ? global_settings.sort_images
                            : global_settings.sort_documents;
    int by_name = strnatcasecmp(path_list_leaf(&list, x),
                                path_list_leaf(&list, y));

    if (order == FLAT_SORT_NEWEST && flat_time(x) != flat_time(y))
        return flat_time(x) < flat_time(y) ? 1 : -1;
    return order == FLAT_SORT_NAME_REVERSE ? -by_name : by_name;
}

/* A file's type as By Type names it: its extension in capitals, the ways of
 * writing one type made one */
static void file_type(const char *path, char *buf, size_t size)
{
    static const char * const same[][2] = {
        { "JPG", "JPEG" }, { "JPE", "JPEG" }, { "HTM", "HTML" },
        { "LRC8", "LRC" }, { "PRC", "MOBI" }, { "AZW", "MOBI" },
    };
    const char *dot = strrchr(path, '.');
    size_t i;

    strlcpy(buf, dot && !strchr(dot, '/') ? dot + 1 : "", size);
    for (i = 0; buf[i]; i++)
        if (buf[i] >= 'a' && buf[i] <= 'z')
            buf[i] -= 'a' - 'A';
    for (i = 0; i < ARRAYLEN(same); i++)
        if (!strcmp(buf, same[i][0]))
            strlcpy(buf, same[i][1], size);
}

/* `shown` filled with every file of `type`, or every file for NULL, in the
 * order set */
static void fill_shown(const char *type)
{
    char t[TYPE_MAX];

    shown_ct = 0;
    for (int i = 0; i < list.count; i++)
    {
        if (type)
        {
            file_type(flat_path(i), t, sizeof(t));
            if (strcmp(t, type))
                continue;
        }
        shown[shown_ct++] = i;
    }
    qsort(shown, shown_ct, sizeof(shown[0]), compare_files);
}

static bool load(void)
{
    return path_list_load(&list, file_index_list(flat_images), PATH_LIST_MAX);
}

/* ------------------------------------------------------------------ *
 * opening a file                                                     *
 * ------------------------------------------------------------------ */

/* Open file `index` of `list`. GO_TO_PREVIOUS to carry on with the list,
 * which is loaded again; anything else is where to go instead. */
static int open_file(int index)
{
    /* Copied out, not pointed at: the list is handed back below and the path
     * has to outlive it. */
    char path[MAX_PATH];
    int attr, rc = GO_TO_PREVIOUS;

    strlcpy(path, flat_path(index), sizeof(path));
    attr = filetype_get_attr(path);

    /* The list is only as fresh as the last walk, so a file named in it may
     * be gone: the walk runs again, and the list follows when it is done. */
    if (!file_exists(path))
    {
        splash(HZ * 2, ID2P(LANG_FILE_NOT_FOUND));
        bg_task_update(&file_index_task);
        return GO_TO_PREVIOUS;
    }

    /* Give the scratch buffer up before the viewer starts. Holding it across
     * the call would panic the moment the viewer wanted it, and letting go
     * also leaves the viewer a whole free buflib pool -- the image viewer
     * takes the largest contiguous block it can find, so this is the
     * difference between opening a big picture from here and from the file
     * browser. */
    path_list_free(&list);

    /* A viewer that returns anywhere other than back here is going somewhere
     * real (the root on a USB session, say). */
    if (filetype_open_core_viewer(attr, path, &rc) && rc != GO_TO_PREVIOUS)
        return rc;

    /* Read again rather than remembering: the background walk may have run
     * while the viewer was up, so this is the current list, not the one we
     * let go of. It can also have become empty, which closes the list. */
    load();
    return GO_TO_PREVIOUS;
}

/* ------------------------------------------------------------------ *
 * Search                                                             *
 * ------------------------------------------------------------------ */

#define MAX_MATCHES 100
static int matches[MAX_MATCHES];
static int match_ct;

static int search_scan(const char *query, void *ctx)
{
    (void)ctx;
    match_ct = 0;
    for (int i = 0; query[0] && i < list.count && match_ct < MAX_MATCHES; i++)
        if (strcasestr(path_list_leaf(&list, i), query))
            matches[match_ct++] = i;
    return match_ct;
}

static const char *search_row(int i, void *ctx)
{
    (void)ctx;
    return path_list_leaf(&list, matches[i]);
}

static int search_run(void)
{
    static char query[SEARCH_MAX_QUERY + 1];
    static const struct search_provider provider = {
        .activity = ACTIVITY_FILE_SEARCH,
        .scan     = search_scan,
        .row_text = search_row,
        .row_icon = NULL,
    };
    struct search_provider p = provider;
    int chosen;

    p.title = str(flat_images ? LANG_IMAGES : LANG_DOCUMENTS);
    chosen = search_dialog_run(&p, NULL, query, sizeof(query));
    if (chosen == SEARCH_USB)
        return GO_TO_ROOT;
    if (chosen < 0)
        return GO_TO_PREVIOUS;
    return open_file(matches[chosen]);
}

/* ------------------------------------------------------------------ *
 * By Type                                                            *
 * ------------------------------------------------------------------ */

static int show_list(const char *type, char *title);

static char types[16][TYPE_MAX];
static int type_files[16];      /* a file of each, for its icon */
static int type_counts[16];
static int type_ct;

static int compare_types(const void *a, const void *b)
{
    return strcmp(types[*(const int *)a], types[*(const int *)b]);
}

static void fill_types(void)
{
    static int order[16];
    char t[TYPE_MAX];
    char sorted[16][TYPE_MAX];
    int files[16], counts[16];
    int i, k;

    type_ct = 0;
    for (i = 0; i < list.count; i++)
    {
        file_type(flat_path(i), t, sizeof(t));
        for (k = 0; k < type_ct && strcmp(types[k], t); k++)
            ;
        if (k == type_ct)
        {
            if (type_ct == (int)ARRAYLEN(types))
                continue;
            strlcpy(types[type_ct], t, TYPE_MAX);
            type_files[type_ct] = i;
            type_counts[type_ct++] = 0;
        }
        type_counts[k]++;
    }
    for (i = 0; i < type_ct; i++)
        order[i] = i;
    qsort(order, type_ct, sizeof(order[0]), compare_types);
    for (i = 0; i < type_ct; i++)
    {
        strlcpy(sorted[i], types[order[i]], TYPE_MAX);
        files[i] = type_files[order[i]];
        counts[i] = type_counts[order[i]];
    }
    for (i = 0; i < type_ct; i++)
    {
        strlcpy(types[i], sorted[i], TYPE_MAX);
        type_files[i] = files[i];
        type_counts[i] = counts[i];
    }
}

static const char *type_name(int i, void *data, char *buf, size_t size)
{
    (void)data;
    snprintf(buf, size, "%s (%d)", types[i], type_counts[i]);
    return buf;
}

static enum themable_icons type_icon(int i, void *data)
{
    (void)data;
    return filetype_get_icon(filetype_get_attr(flat_path(type_files[i])));
}

static enum list_row_kind type_kind(int i, void *data)
{
    (void)i; (void)data;
    return LIST_ROW_CONTAINER;
}

static int types_run(void)
{
    struct simplelist_info info;
    char title[TYPE_MAX + 16];
    int sel = 0, ret;

    while (1)
    {
        fill_types();
        simplelist_info_init(&info, str(LANG_SORT_TYPE), type_ct, NULL);
        info.get_name = type_name;
        info.get_icon = type_icon;
        info.get_kind = type_kind;
        info.selection = MIN(sel, type_ct - 1);
        if (simplelist_show_list(&info))
            return GO_TO_ROOT;
        if (info.selection < 0)
            return GO_TO_PREVIOUS;
        sel = info.selection;
        snprintf(title, sizeof(title), "%s (%d)", types[sel],
                 type_counts[sel]);
        ret = show_list(types[sel], title);
        if (ret != GO_TO_PREVIOUS)
            return ret;
    }
}

/* ------------------------------------------------------------------ *
 * the list on screen                                                 *
 * ------------------------------------------------------------------ */

/* The filename, not the path it sits at: the path is long, the row is narrow,
 * and the name is what is being looked for. */
static const char *flat_get_name(int selected_item, void *data,
                                 char *buffer, size_t buffer_len)
{
    (void)data; (void)buffer; (void)buffer_len;
    if (selected_item == ROW_SEARCH && heads)
        return str(LANG_DB_SEARCH);
    if (selected_item == ROW_BY_TYPE && heads)
        return str(LANG_SORT_TYPE);
    return path_list_leaf(&list, shown[selected_item - heads]);
}

static enum themable_icons flat_get_icon(int selected_item, void *data)
{
    (void)data;
    if (selected_item < heads)
        return Icon_NOICON;
    return filetype_get_icon(filetype_get_attr(
                flat_path(shown[selected_item - heads])));
}

static enum list_row_kind flat_get_kind(int selected_item, void *data)
{
    (void)data;
    return selected_item < heads ? LIST_ROW_COMMAND : LIST_ROW_FILE;
}

/* The files of `type`, or for NULL every file with the rows above them */
static int show_list(const char *type, char *title)
{
    struct simplelist_info info;
    char all[64];
    int sel = -1, ret = GO_TO_PREVIOUS, choice;

    while (1)
    {
        heads = type ? 0 : HEAD_ROWS;
        fill_shown(type);
        if (!type)
        {
            snprintf(all, sizeof(all), "%s (%d%s)",
                     str(flat_images ? LANG_IMAGES : LANG_DOCUMENTS),
                     list.count, list.truncated ? "+" : "");
            title = all;
        }
        if (shown_ct == 0)
            break;

        simplelist_info_init(&info, title, heads + shown_ct, NULL);
        info.get_name = flat_get_name;
        info.get_icon = flat_get_icon;
        info.get_kind = flat_get_kind;
        /* Opening on the first file, the rows above it off the top */
        if (sel < 0)
        {
            sel = heads;
            info.top_item = heads;
        }
        info.selection = MIN(sel, heads + shown_ct - 1);

        if (simplelist_show_list(&info))
        {
            ret = GO_TO_ROOT;
            break;
        }
        if (info.selection < 0)
            break;                       /* backed out */
        sel = choice = info.selection;

        if (heads && choice == ROW_SEARCH)
            ret = search_run();
        else if (heads && choice == ROW_BY_TYPE)
            ret = types_run();
        else
            ret = open_file(shown[choice - heads]);
        if (ret != GO_TO_PREVIOUS)
            break;
    }
    return ret;
}

int browser_flat(bool images)
{
    int ret;

    flat_images = images;
    if (!load())
    {
        /* No list yet and an empty one look the same from here: the walk runs
         * in the background and may simply not have happened. Say the neutral
         * thing rather than claiming there are none.
         *
         * The id is chosen before ID2P rather than inside it -- the macro does
         * arithmetic on its argument, so a conditional there is not the string
         * id it looks like. */
        int msg = file_index_is_busy() ? LANG_WAIT : LANG_FLAT_LIST_EMPTY;
        splash(HZ * 2, ID2P(msg));
        return GO_TO_PREVIOUS;
    }

    push_current_activity(images ? ACTIVITY_IMAGEBROWSER
                                 : ACTIVITY_DOCUMENTBROWSER);
    ret = show_list(NULL, NULL);
    pop_current_activity();
    path_list_free(&list);
    return ret;
}
