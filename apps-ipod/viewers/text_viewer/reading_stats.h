/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to reading_stats.c.
 ****************************************************************************/
#ifndef _READING_STATS_H_
#define _READING_STATS_H_

#include <stdbool.h>

/* The reader's side: a sitting starts when a book opens and ends when it
 * closes. A page turned forward counts the time since the last event, up to
 * a few minutes, and the words on it; anything else only restarts that
 * clock. */
void reading_stats_begin(const char *path);
void reading_stats_page(int words);
void reading_stats_touch(void);
/* pct how far through the book the reader is; finished, at its end */
void reading_stats_end(int pct, bool finished);

/* The Books screen's side: one book's figures, or every book's together.
 * False when the list was left for the root menu. */
bool reading_stats_show(const char *path, const char *title);
bool reading_stats_show_all(void);

#endif /* _READING_STATS_H_ */
