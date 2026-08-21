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
/* Which side of the library a search covers. Armed just before the screen is
 * opened, by whoever opened it, and spent on that one search. */
enum db_search_scope {
    DB_SEARCH_ALL = 0,   /* both -- the main menu's own Search row */
    DB_SEARCH_MUSIC,     /* the Music menu's */
    DB_SEARCH_SPOKEN,    /* the Audiobooks menu's */
};

void db_search_arm_scope(int scope);

int db_search_run(void);

#endif /* _DB_SEARCH_H */
