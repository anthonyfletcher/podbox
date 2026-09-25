/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to book_shelf.c: the Audiobooks shelf's In progress, Not started
 * and Finished rows.
 ****************************************************************************/
#ifndef _BOOK_SHELF_H
#define _BOOK_SHELF_H

#include <stdbool.h>

/* Which list. Carried in a built-in row's extraseek, so append only. */
enum book_shelf {
    BOOK_SHELF_IN_PROGRESS,
    BOOK_SHELF_NOT_STARTED,
    BOOK_SHELF_FINISHED,
};

/* Which list the next book_shelf_run() shows. The database browser's rows arm
 * this before returning GO_TO_BOOK_SHELF, because a browse level can hand back
 * only a bare screen code. */
void book_shelf_arm(enum book_shelf which);

/* The armed list. Choosing a book plays it -- from where it was left if it is
 * in progress, from the start otherwise. Returns a GO_TO_* code. */
int book_shelf_run(void);

/* Whether 'path' is the last track of the book 'book' -- its album -- in the
 * order the shelf plays it. False where either is not found. */
bool book_shelf_is_last_track(const char *book, const char *path);

#endif /* _BOOK_SHELF_H */
