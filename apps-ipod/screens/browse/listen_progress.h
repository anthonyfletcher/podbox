/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to listen_progress.c.
 ****************************************************************************/
#ifndef _LISTEN_PROGRESS_H
#define _LISTEN_PROGRESS_H

/* How much of the selected database browse row has been heard.
 *
 * Reads the row the database browser has selected, so it is only meaningful
 * while that browser is up -- which is where the context menu offers it.
 * Returns a GO_TO_* code.
 *
 * Says nothing and returns GO_TO_PREVIOUS when the selected row is not an
 * album or an artist; the menu row is hidden in that case, so a caller has to
 * go out of its way to see it. */
int listen_progress_show(void);

#endif /* _LISTEN_PROGRESS_H */
