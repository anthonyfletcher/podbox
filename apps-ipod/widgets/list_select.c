/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A list's selecting mode. A screen enters it from a row's context menu,
 * fetches its actions with list_select_context() while it is on, and hands
 * each one here after the list has moved. Selecting is modal and the list
 * cannot change under it -- a screen that reloads ends it -- so a row number
 * names a row for as long as the mode lasts.
 *
 * Parts:
 *   - the marks: a bitmap over the rows, and the count
 *   - runs: SELECT held while the wheel turns
 *   - the title, which says how many are ticked
 *   - the mode: begun, suspended under another screen, ended
 *   - the saved rows, which the ticked rows' menu acts from
 *   - what the list's drawing asks
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>     /* strtol */
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "string-extra.h"
#include "button.h"
#include "kernel.h"     /* IS_SYSEVENT */
#include "lang.h"
#include "system/strutil.h"     /* read_line */
#include "input/action.h"
#include "widgets/list.h"
#include "widgets/list_select.h"

static struct {
    struct gui_synclist *list;
    list_select_eligible *eligible;
    void *data;
    int count;
    int cursor;             /* where the cursor was after the last action */
    int run_start;          /* the row a run began on, -1 for none */
    int run_end;            /* where it reaches now */
    bool run_state;         /* what the rows it covers become */
    const char *title;      /* the list's own, put back at the end */
    enum themable_icons title_icon;
    char title_buf[48];
} ls = { .run_start = -1 };

/** The marks **/

static unsigned char marks[LIST_SELECT_MAX_ROWS / 8];
/* The marks as a run found them, which the rows it leaves go back to */
static unsigned char before_run[LIST_SELECT_MAX_ROWS / 8];

static bool get_mark(const unsigned char *map, int row)
{
    return map[row >> 3] & (1 << (row & 7));
}

static void set_mark(int row, bool on)
{
    if (get_mark(marks, row) == on)
        return;
    marks[row >> 3] ^= 1 << (row & 7);
    ls.count += on ? 1 : -1;
}

/* Bounded by marks[] as well as the list: a list that grows past
 * LIST_SELECT_MAX_ROWS mid-mode leaves its extra rows unmarkable. */
static bool eligible(int row)
{
    return row >= 0 && row < ls.list->nb_items && row < LIST_SELECT_MAX_ROWS
           && (!ls.eligible || ls.eligible(row, ls.data));
}

int list_select_count(void)
{
    return ls.list ? ls.count : 0;
}

void list_select_menu_name(char *buf, size_t size, bool tracks)
{
    int n = list_select_count();

    if (n == 1)
        snprintf(buf, size, "/%s",
                 str(tracks ? LANG_ONE_TRACK_TITLE : LANG_ONE_ITEM_TITLE));
    else
    {
        buf[0] = '/';
        snprintf(buf + 1, size - 1,
                 str(tracks ? LANG_N_TRACKS_TITLE : LANG_N_ITEMS_TITLE), n);
    }
}

int list_select_next(int after)
{
    if (!ls.list)
        return -1;
    int rows = MIN(ls.list->nb_items, LIST_SELECT_MAX_ROWS);

    for (int row = after + 1; row < rows; row++)
        if (get_mark(marks, row))
            return row;
    return -1;
}

/** Runs **/

/* The rows between the run's start and 'end' take the run's state; the rows
 * it reached before and no longer does go back to what they were. */
static void extend_run(int end)
{
    int lo = MIN(ls.run_start, MIN(end, ls.run_end));
    int hi = MAX(ls.run_start, MAX(end, ls.run_end));
    int from = MIN(ls.run_start, end), to = MAX(ls.run_start, end);

    for (int row = lo; row <= hi; row++)
    {
        if (!eligible(row))
            continue;
        set_mark(row, row >= from && row <= to ? ls.run_state
                                               : get_mark(before_run, row));
    }
    ls.run_end = end;
}

/* Called after every action: a run is SELECT held while the cursor moves. It
 * begins on the row the cursor left, which flips, and ends when SELECT is let
 * go. */
static bool track_run(void)
{
    int cursor = ls.list->selected_item;
    bool held = (button_status() & BUTTON_SELECT) != 0;
    bool changed = false;

    if (!held)
        ls.run_start = -1;
    else if (cursor != ls.cursor)
    {
        if (ls.run_start < 0)
        {
            ls.run_start = ls.run_end = ls.cursor;
            ls.run_state = !(ls.cursor < LIST_SELECT_MAX_ROWS
                             && get_mark(marks, ls.cursor));
            memcpy(before_run, marks, sizeof(marks));
        }
        extend_run(cursor);
        changed = true;
    }
    ls.cursor = cursor;
    return changed;
}

/** The title **/

/* The count in the title, and the list redrawn */
static void show_count(void)
{
    snprintf(ls.title_buf, sizeof(ls.title_buf), str(LANG_N_SELECTED),
             ls.count);
    gui_synclist_set_title(ls.list, ls.title_buf, ls.title_icon);
    gui_synclist_draw(ls.list);
}

void list_select_retitle(void)
{
    if (!ls.list)
        return;
    /* Unless the screen left our title up, which is not its own */
    if (ls.list->title != ls.title_buf)
    {
        ls.title = ls.list->title;
        ls.title_icon = ls.list->title_icon;
    }
    show_count();
}

/** The mode **/

bool list_select_begin(struct gui_synclist *list, int row,
                       list_select_eligible *is_eligible, void *data)
{
    if (list->nb_items > LIST_SELECT_MAX_ROWS)
        return false;
    memset(marks, 0, sizeof(marks));
    ls.list = list;
    ls.eligible = is_eligible;
    ls.data = data;
    ls.count = 0;
    ls.cursor = list->selected_item;
    ls.run_start = -1;
    ls.title = list->title;
    ls.title_icon = list->title_icon;
    if (eligible(row))
        set_mark(row, true);
    show_count();
    return true;
}

bool list_select_active(void)
{
    return ls.list != NULL;
}

int list_select_context(int otherwise)
{
    return ls.list ? CONTEXT_LIST_SELECT : otherwise;
}

void list_select_end(void)
{
    struct gui_synclist *list = ls.list;

    if (!list)
        return;
    ls.list = NULL;
    gui_synclist_set_title(list, ls.title, ls.title_icon);
    gui_synclist_draw(list);
}

struct gui_synclist *list_select_suspend(void)
{
    struct gui_synclist *list = ls.list;

    ls.list = NULL;
    return list;
}

void list_select_resume(struct gui_synclist *list)
{
    ls.list = list;
}

enum list_select_result list_select_action(int action)
{
    if (!ls.list)
        return LIST_SELECT_PASS;

    switch (action)
    {
        case ACTION_LS_TOGGLE:
            if (eligible(ls.list->selected_item))
            {
                set_mark(ls.list->selected_item,
                         !get_mark(marks, ls.list->selected_item));
                show_count();
            }
            ls.cursor = ls.list->selected_item;
            return LIST_SELECT_HANDLED;

        case ACTION_LS_MENU:
            return ls.count > 0 ? LIST_SELECT_MENU : LIST_SELECT_HANDLED;

        case ACTION_STD_CANCEL:
            list_select_end();
            return LIST_SELECT_END;

        /* Entering a row, or a list's other keys, mean nothing here */
        case ACTION_STD_OK:
        case ACTION_STD_CONTEXT:
        case ACTION_STD_MENU:
        case ACTION_TREE_WPS:
            return LIST_SELECT_HANDLED;
    }

    if (track_run())
        show_count();
    return IS_SYSEVENT(action) ? LIST_SELECT_PASS : LIST_SELECT_HANDLED;
}

/** The saved rows **/

/* One line, read or written: never both at once, and never under itself */
static char line[MAX_PATH + 16];

bool list_select_save(list_select_path_of *path_of, void *data)
{
    int fd = open(LIST_SELECT_SAVED, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool ok = fd >= 0;

    for (int row = list_select_next(-1); ok && row >= 0;
         row = list_select_next(row))
    {
        int attr;

        if (path_of(row, line, sizeof(line), &attr, data))
            ok = fdprintf(fd, "%d %s\n", attr, line) > 0;
    }
    if (fd >= 0)
    {
        close(fd);
        if (!ok)
            remove(LIST_SELECT_SAVED);
    }
    return ok;
}

int list_select_saved_each(bool (*fn)(const char *path, int attr, void *data),
                           void *data)
{
    int fd = open(LIST_SELECT_SAVED, O_RDONLY);
    int n = 0;

    if (fd < 0)
        return -1;
    while (read_line(fd, line, sizeof(line)) > 0)
    {
        char *path;
        int attr = strtol(line, &path, 10);

        if (*path++ != ' ')
            continue;
        if (!fn(path, attr, data))
            break;
        n++;
    }
    close(fd);
    return n;
}

/** What the list's drawing asks **/

int list_select_row_state(const struct gui_synclist *list, int row)
{
    if (list != ls.list || !eligible(row))
        return -1;
    return get_mark(marks, row);
}

const char *list_select_decorate(const struct gui_synclist *list, int row,
                                 const char *text, char *buf, size_t size)
{
    static char copy[MAX_PATH];
    int state = list_select_row_state(list, row);

    if (state < 0 || !text)
        return text;
    strlcpy(copy, text, sizeof(copy));
    snprintf(buf, size, "%s%s", state ? "[x] " : "[ ] ", copy);
    return buf;
}
