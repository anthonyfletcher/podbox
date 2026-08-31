/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to playlist_search.c: text search across saved playlist names.
 ****************************************************************************/
#ifndef _PLAYLIST_SEARCH_H
#define _PLAYLIST_SEARCH_H

/* Run the search box. Returns a GO_TO_* screen code for root_menu.c to
 * dispatch -- selecting a result opens that playlist in the viewer, and
 * leaving the viewer comes back to the results. */
int playlist_search_run(void);

#endif /* _PLAYLIST_SEARCH_H */
