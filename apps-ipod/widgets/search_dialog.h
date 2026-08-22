/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to search_dialog.c: a type-and-pick box over any list of matches.
 ****************************************************************************/

/* A modal search box: an edit line, a rule, and a scrolling list of results
 * drawn as borderless buttons with the selected row in the accent colour.
 *
 * A dialog rather than a screen: a screen must take its input box out of the
 * theme's list rectangle, and themes decorate that rectangle from viewports at
 * absolute coordinates -- corner pieces are placed to meet the top edge the
 * list already has, so a list shortened to make room leaves them behind.
 * Dialog styling is self-contained.
 *
 * Everything about *what* a match is belongs to the provider: the scan, where
 * matches are stored, what a row says, and what selecting one does. The dialog
 * owns the box geometry, the settle timer, the selection and its marquee, and
 * the handover of the wheel between the query and the results.
 *
 * The provider is asked to scan only once the query has stood still for about
 * a second, not on every keystroke, so a scan may be as slow as a sequential
 * crawl without costing anything at the keyboard.
 *
 * One box at a time. The dialog's whole state is a single static -- seven
 * hundred bytes is not worth putting on an arbitrary caller's stack -- so a
 * second search opened from inside a first one takes the first one's state
 * with it. The providers stack the same way: their hit tables are statics
 * too. Nothing nests today; a screen reached *from* a result may not open a
 * search of its own.
 */

#ifndef _GUI_SEARCH_DIALOG_H_
#define _GUI_SEARCH_DIALOG_H_

#include <stdbool.h>
#include <stddef.h>
#include "lcd.h"

/* Longest query the box will accept, and so the smallest buffer a caller may
 * pass to search_dialog_run(). */
#define SEARCH_MAX_QUERY 32

struct search_provider
{
    const char *title;        /* already a str() result, or NULL for none    */
    int         activity;     /* pushed for the dialog's lifetime            */

    /* Re-run the whole search and return how many matches there now are. The
     * provider owns the storage; the dialog only ever asks for rows by index.
     *
     * Called from the settle timer, so it may take a moment, but it must not
     * yield to the UI: nothing repaints while it runs.
     *
     * A query below the provider's own minimum length is its business, not the
     * dialog's -- return 0 and the list empties. */
    int (*scan)(const char *query, void *ctx);

    /* Row `i` of the last scan, 0 <= i < the count scan() returned. The
     * returned pointer only has to stay valid until the next call. */
    const char *(*row_text)(int i, void *ctx);

    /* Optional (may be NULL, and may return NULL per row): a 1bpp bitmap drawn
     * at the left of the row in the row's own colour. */
    const struct bitmap *(*row_icon)(int i, void *ctx);
};

/* What search_dialog_run() returns instead of a row index. USB is distinct
 * from cancel deliberately: the caller has to unwind to the root rather than
 * to wherever it was, and a provider that conflates the two leaves the screen
 * standing while the root menu tears it down. */
#define SEARCH_CANCELLED (-1)
#define SEARCH_USB       (-2)

/* Run the box until a row is chosen or the user leaves. Returns the index of
 * the chosen row -- an index into the provider's own last scan, so the caller
 * acts on it via the same accessors -- or one of the two codes above.
 *
 * `query` is both the starting text and where the closing text is written
 * back, so a caller that keeps it in a static reopens on the last thing
 * searched for. It must be at least SEARCH_MAX_QUERY + 1 bytes. */
int search_dialog_run(const struct search_provider *p, void *ctx,
                      char *query, size_t query_len);

#endif /* _GUI_SEARCH_DIALOG_H_ */
