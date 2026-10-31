/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A list's selecting mode: rows ticked one at a time or in a run, then acted
 * on together. One list at a time; see list_select.c.
 ****************************************************************************/

#ifndef LIST_SELECT_H
#define LIST_SELECT_H

#include <stdbool.h>
#include <stddef.h>

struct gui_synclist;

/* Rows a list holds at most for the mode to be offered on it */
#define LIST_SELECT_MAX_ROWS 32768

/* Whether a row can be ticked: a list's own rows, such as [All Tracks],
 * cannot */
typedef bool list_select_eligible(int row, void *data);

enum list_select_result {
    LIST_SELECT_PASS,       /* not the mode's: the screen handles it */
    LIST_SELECT_HANDLED,
    LIST_SELECT_MENU,       /* the screen opens its menu for the ticked rows */
    LIST_SELECT_END,        /* the mode ended; the list is redrawn */
};

bool list_select_begin(struct gui_synclist *list, int row,
                       list_select_eligible *eligible, void *data);
bool list_select_active(void);
/* The action context a screen fetches with while the mode is on */
int  list_select_context(int otherwise);
/* After gui_synclist_do_button(), every action the screen fetched */
enum list_select_result list_select_action(int action);
int  list_select_count(void);
/* The ticked rows' menu's name, as a path the context menu titles it by:
 * "/3 tracks", "/1 item" */
void list_select_menu_name(char *buf, size_t size, bool tracks);
/* The ticked rows in order: from -1, then each one returned; -1 at the end */
int  list_select_next(int after);
/* After the screen redraws its list, which sets the title afresh */
void list_select_retitle(void);
void list_select_end(void);
/* For a screen opened over a selecting one, such as a playlist picker: the
 * mode is off, and ending it does nothing, until resumed with what this
 * returned */
struct gui_synclist *list_select_suspend(void);
void list_select_resume(struct gui_synclist *list);

/* Acting on one ticked row can change the list the next is read from -- a
 * folder's insert reloads the browser's listing, Play empties the Queue -- so
 * a screen saves every row's path here before its menu opens, acts from the
 * file, and removes it after. One "<attr> <path>" line a row. */
#define LIST_SELECT_SAVED ROCKBOX_DIR "/select.lst"
/* A row's path and attributes into 'buf'; false leaves the row out */
typedef bool list_select_path_of(int row, char *buf, size_t size, int *attr,
                                 void *data);
/* False when the file could not be written */
bool list_select_save(list_select_path_of *path_of, void *data);
/* Each saved path to 'fn', in order, until it returns false. The count it
 * took, or -1 when there is no file. */
int  list_select_saved_each(bool (*fn)(const char *path, int attr,
                                       void *data),
                            void *data);

/* For the list's drawing: -1 when 'list' is not selecting or 'row' cannot be
 * ticked, else whether it is */
int list_select_row_state(const struct gui_synclist *list, int row);
/* A row's text with its mark in front, for a list that is selecting and is
 * drawn by something that cannot draw the mark itself; 'text' unchanged
 * otherwise. 'buf' may be 'text'. */
const char *list_select_decorate(const struct gui_synclist *list, int row,
                                 const char *text, char *buf, size_t size);

#endif
