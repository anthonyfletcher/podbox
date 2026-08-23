/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to db_spoken.c: which of the library's genres name spoken word
 * rather than music.
 ****************************************************************************/
#ifndef _DB_SPOKEN_H
#define _DB_SPOKEN_H

#include <stdbool.h>

/* Whether 'genre' names spoken word. ASCII case folding only, and the string
 * is the whole of the evidence -- nothing here reads the database. */
bool db_spoken_is_spoken_genre(const char *genre);

/* Read the library's genres and keep the seeks of the spoken-word ones.
 * False if the database could not be read, in which case the table is empty
 * and the caller should try again rather than record this as done.
 *
 * Runs a database search of its own, so it must not be called from inside
 * one, and two calls must not overlap -- it works in static storage rather
 * than on the caller's stack. tagcache.c owns the only call and enforces
 * both; see spoken_table_update() there.
 *
 * The seeks are valid only for the commit they were read at, since a commit
 * re-sorts the tag files and moves every seek in them. A library with more
 * spoken-word genres than the table holds keeps the first of them. */
bool db_spoken_build(void);

/* Whether 'genre_seek' -- a track's index_entry.tag_seek[tag_genre] -- is one
 * of those. False before any build, so the tag reads as "music" rather than
 * as an error. */
bool db_spoken_is_spoken_seek(long genre_seek);

/* Whether the album, album artist or canonical artist at 'seek' is a book --
 * it holds spoken word and no music.
 *
 * These three are unique-valued, so a crawl of their tag files yields one
 * entry per distinct string with no track behind it; there is nothing to ask
 * about the genre of, and a search that asks by clause instead comes back in
 * database order rather than alphabetically. The tables answer both.
 *
 * The answer matches what a tag_virt_spoken clause would have decided, so a
 * mixed album reads as music either way. db_spoken_group_tag() says whether a
 * tag has a table at all; anything else is false, i.e. music.
 *
 * Call db_spoken_group_ensure() first. Everything is music until it has run,
 * so a caller that forgets hides nothing rather than everything. */
bool db_spoken_group_tag(int tag);
bool db_spoken_group_is_book(int tag, long seek);

/* Build one tag's table if it is missing or stale, and do nothing for a tag
 * that has none. Two passes over the master index, so this is for a screen
 * that is about to need that tag, not for the search path itself -- it is
 * cheap only because it happens once per database commit. Runs its own
 * searches; not to be called from inside one. */
void db_spoken_group_ensure(int tag);

#endif /* _DB_SPOKEN_H */
