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

/* For the list's drawing: -1 when 'list' is not selecting or 'row' cannot be
 * ticked, else whether it is */
int list_select_row_state(const struct gui_synclist *list, int row);
/* A row's text with its mark in front, for a list that is selecting and is
 * drawn by something that cannot draw the mark itself; 'text' unchanged
 * otherwise. 'buf' may be 'text'. */
const char *list_select_decorate(const struct gui_synclist *list, int row,
                                 const char *text, char *buf, size_t size);

#endif
