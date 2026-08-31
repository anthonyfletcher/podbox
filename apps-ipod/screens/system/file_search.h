/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to file_search.c: text search across every filename on the player.
 ****************************************************************************/
#ifndef _FILE_SEARCH_H
#define _FILE_SEARCH_H

/* Run the search box. Returns a GO_TO_* screen code for root_menu.c to
 * dispatch -- selecting a result shows it in the file browser.
 *
 * The names come from the directory cache, so this has nothing to search when
 * the cache is off or still building. Ask dircache_is_ready() before offering
 * a way in; called anyway, it says so and returns. */
int file_search_run(void);

#endif /* _FILE_SEARCH_H */
