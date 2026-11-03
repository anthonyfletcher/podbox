/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to reading_stats.c.
 ****************************************************************************/
#ifndef _READING_STATS_H_
#define _READING_STATS_H_

#include <stdbool.h>
#include <stdint.h>

/* The reader's side: a sitting starts when a book opens and ends when it
 * closes. A page turned forward counts the time since the last event, up to
 * a few minutes, and the words on it, if it was on screen long enough to
 * read and lies past what the sitting has counted; anything else only
 * restarts that clock. `at` orders the pages through the book, later pages
 * larger: at begin the page it opened on, at a turn the page turned from. */
void reading_stats_begin(const char *path, int64_t at);
void reading_stats_page(int words, int64_t at);
void reading_stats_touch(void);
/* pct how far through the book the reader is; finished, at its end */
void reading_stats_end(int pct, bool finished);

/* The Books screen's side: one book's figures, or every book's together.
 * False when the list was left for the root menu. */
bool reading_stats_show(const char *path, const char *title);
bool reading_stats_show_all(void);

#endif /* _READING_STATS_H_ */
