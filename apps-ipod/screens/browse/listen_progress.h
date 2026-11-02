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

/* The same for the book or podcast show that is album 'album_seek' by album
 * artist 'artist_seek', under 'title', headed by its Listening Stats: a book
 * has the screen wherever it is listed, including where no browse row stands
 * for it. */
int listen_progress_show_book(long album_seek, long artist_seek,
                              const char *title);

/* Every book's Listening Stats together, or with 'kind' 2 every show's:
 * armed, then shown */
void listen_progress_arm(int kind);
int listen_progress_show_all(void);

#endif /* _LISTEN_PROGRESS_H */
