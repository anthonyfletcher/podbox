/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to featured_artists.c.
 ****************************************************************************/
#ifndef _FEATURED_ARTISTS_H
#define _FEATURED_ARTISTS_H

/* The guests the library credits, and the tracks that credit them. Owns the
 * screen until the user leaves; returns a GO_TO_* code -- the playback screen
 * if a track was started, the browser if a guest's own albums were asked for,
 * GO_TO_PREVIOUS otherwise.
 *
 * Reached only from the Music menu's built-in row, which hides itself unless
 * there is a table to read (see database/db_featured.c). */
int featured_artists_show(void);

/* The other way in: the tracks that credit one artist as a guest on somebody
 * else's record, which is the browser's [Featured In] row on an artist's
 * album list. The same track list as above, so this is an entry point rather
 * than a second screen.
 *
 * Armed and then run, as the album charts are: a browse level can only hand
 * back a bare screen code, and root_menu.c's dispatch has nowhere to carry
 * the name. */
void featured_artists_arm(const char *artist);
int featured_artists_run(void);

#endif /* _FEATURED_ARTISTS_H */
