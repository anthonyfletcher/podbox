/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to db_search.c: the incremental database search prototype.
 ****************************************************************************/
#ifndef _DB_SEARCH_H
#define _DB_SEARCH_H

/* Run the search screen. Returns a GO_TO_* screen code for root_menu.c to
 * dispatch -- selecting an album or an artist hands off to the database
 * browser, and a track plays and goes to the WPS. */
int db_search_run(void);

#endif /* _DB_SEARCH_H */
