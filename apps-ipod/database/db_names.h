/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to db_names.c: Split Album Artists' table of the album-artist
 * strings that name several artists, and the artists they name.
 ****************************************************************************/
#ifndef _DB_NAMES_H
#define _DB_NAMES_H

#include <stdbool.h>
#include <stdint.h>

/* Fixed-size and resident, as the guest table is: what does not fit is
 * dropped, and the strings it would have split stay one artist */
#define DB_NAMES_PARTS_MAX   512
#define DB_NAMES_JOINTS_MAX  256
#define DB_NAMES_LINKS_MAX  1024
#define DB_NAMES_ARENA      (8 * 1024)

/* A split artist ("a part") is browsed by a handle where an album-artist seek
 * would go: tagcache's filter matches it against every album-artist string
 * naming them, and tagcache_seek_string() gives their name. Seeks are never
 * negative, and -1 and INT_MIN already mean other things. */
#define DB_NAMES_HANDLE_FIRST (-0x40000000L)

static inline bool db_names_is_handle(long seek)
{
    return seek <= DB_NAMES_HANDLE_FIRST
           && seek > DB_NAMES_HANDLE_FIRST - DB_NAMES_PARTS_MAX;
}

/* Build the table if the setting is on and the database has changed since
 * it was built, or empty it if the setting is off. Crawls the album-artist
 * tag, so not from a redraw and never inside a search: the UI thread, or the
 * art cache's pass. While one builds, the other is told the table is not in
 * use, and a handle matches nothing. Whether the table is in use
 * afterwards. */
bool db_names_ensure(void);

/* Whether the table is in use: the setting on, and the table built against
 * the database as it is now. Any thread; false while it is being built. */
bool db_names_active(void);

/* Whether the album-artist string at 'seek' names several artists, and so
 * has no row of its own while the table is in use */
bool db_names_is_joint(long seek);

/* The parts, 0 <= n < db_names_count(): name, handle, and the seek of an
 * album-artist string that is the part's name alone, -1 for none */
int db_names_count(void);
const char *db_names_name(int n);
long db_names_handle(int n);
long db_names_own_seek(int n);

/* The part a handle stands for, or -1 */
int db_names_index(long handle);

/* The part named 'name', by db_featured_name_eq()'s identity, or -1 */
int db_names_find(const char *name);

/* The parts the joint string at 'seek' names, in the tag's order, at most
 * 'max' into 'out'. How many; 0 for a string that is not joint. */
int db_names_joint_parts(long seek, int *out, int max);

/* Whether the album-artist string at 'seek' names the part 'handle' */
bool db_names_match(long handle, long seek);

/* What a list row for the album-artist string at 'seek' browses by: a
 * part's handle when the string is their name alone, so their joint albums
 * come with it; otherwise 'seek' */
long db_names_browse_seek(long seek);

/* The album-artist strings naming part n: its own first, if it has one,
 * then the joint ones. How many, at most 'max'. */
int db_names_strings(int n, long *out, int max);

/* Part n as the database's artist table would hold them, over every string
 * naming them: plays summed, the latest play and the newest cover. 'skip',
 * if given, leaves out a string's figures. The seek is the part's handle and
 * the art key that of their name. False if no string is left. */
struct tagcache_artist;
bool db_names_artist(int n, bool (*skip)(const struct tagcache_artist *),
                     struct tagcache_artist *out);

#endif /* _DB_NAMES_H */
