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

/* Whether 'genre' names a podcast -- spoken word, but a show rather than a
 * book, so it has no end to have been finished. Exact match, ASCII folding. */
bool db_spoken_is_podcast_genre(const char *genre);

/* Whether 'genre' names a book: spoken word, not a podcast */
bool db_spoken_is_book_genre(const char *genre);

/* Whether a track of 'genre' (NULL for none) is an episode of a podcast kept
 * apart by Segregate Podcasts: one that keeps its own place in the database
 * whatever Automatic Resume says. */
bool db_spoken_is_episode(const char *genre);

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

/* The same, as tag_virt_spoken answers it: 0 music, 1 a book, 2 a podcast */
int db_spoken_kind_seek(long genre_seek);

/* Whether the album, album artist or canonical artist at 'seek' is a book --
 * it holds only spoken word of the kinds kept apart.
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
 * Album and album artist are answered from tagcache's album tables, and have
 * no table while the database is not in RAM. For canonical artist, call
 * db_spoken_group_ensure() first, and believe it. Everything is music until
 * it has run, so a caller that forgets hides nothing rather than everything. */
bool db_spoken_group_tag(int tag);
bool db_spoken_group_is_book(int tag, long seek);

/* The kinds of spoken word kept apart from the music, as bits 1 << kind:
 * books by Segregate Audiobooks, podcasts by Segregate Podcasts. 0 for none,
 * when every list is music. */
int db_spoken_apart(void);
bool db_spoken_kind_apart(int kind);

/* Make 'c' the clause a list keeps its music by: the tracks of no kind kept
 * apart. For a list to insert while db_spoken_apart() is not 0. */
struct tagcache_search_clause;
void db_spoken_music_clause(struct tagcache_search_clause *c);

/* What an album of tagcache's album table is, as tag_virt_spoken answers
 * for a track: 0 music (any music in it), 2 a podcast (podcasts only), else
 * 1 a book. `kind` below is one of those, or 0 for any kind kept apart. */
struct tagcache_album;
int db_spoken_album_kind(const struct tagcache_album *al);
bool db_spoken_album_is_kind(const struct tagcache_album *al, int kind);

/* Whether the album artist at 'seek' has at least one album that is a book
 * (of `kind`), and whether any album of the name at 'seek' is one, whoever it
 * is by. False while the database is not in RAM. */
bool db_spoken_artist_has_book(long seek, int kind);
bool db_spoken_album_has_book(long seek, int kind);

/* Whether the album at 'album_seek' by 'artist_seek' is a podcast, going by
 * its first track's genre. Runs a database search of its own. */
bool db_spoken_album_is_podcast(long album_seek, long artist_seek);

/* Build one tag's table if it is missing or stale. True once the table can be
 * read; false for a tag that has none, and for a build that could not finish
 * -- the database was unreadable, a commit landed inside it, or another thread
 * was already building. Only one build runs at a time and the loser is turned
 * away rather than made to wait, so false is a normal answer, not an error.
 * Album and album artist are tagcache's album tables, so they are false only
 * while the database is not in RAM.
 *
 * What a caller does with false is its own: a browse or a search shows the
 * books this once and gets the table on its next visit, which costs nothing
 * that lasts.
 *
 * Two passes over the master index, so this is for a screen that is about to
 * need that tag, not for the search path itself: it is cheap only because it
 * happens once per database commit. Runs its own searches; not to be called
 * from inside one. */
bool db_spoken_group_ensure(int tag);

#endif /* _DB_SPOKEN_H */
