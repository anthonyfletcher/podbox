/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to db_featured.c: the guest names credited in a track's tags.
 ****************************************************************************/
#ifndef _DB_FEATURED_H
#define _DB_FEATURED_H

#include <stdbool.h>
#include <stdint.h>

/* Longest guest name kept. A longer one is dropped rather than cut short: a
 * cut name is a wrong name, and it would merge with every other name that
 * shares its opening. */
#define DB_FEATURED_NAME_MAX 63

/* Most names taken from one string. Past this the rest are dropped -- a tag
 * crediting nine people is a track listing, not a title. */
#define DB_FEATURED_MAX_GUESTS 8

/* What one string credited. The names are slices of the string that was
 * parsed, trimmed of surrounding space: nothing is copied, and nothing here
 * outlives that string. */
struct db_featured_names
{
    int count;
    const char *name[DB_FEATURED_MAX_GUESTS];
    int len[DB_FEATURED_MAX_GUESTS];
};

/* Whether the library already knows 'name' as an artist, by whatever
 * comparison the caller holding the database wants to make -- db_featured_
 * name_eq() below is that comparison.
 *
 * Splitting a credit needs this: "A & B" is two names and "Nick Cave & the
 * Bad Seeds" is one, and nothing in the string itself says which. */
typedef bool (*db_featured_known_fn)(const char *name, int len, void *ctx);

/* Fill 'out' with the guests credited in 's' -- a title or a per-track
 * artist tag -- and return how many there were. 'known' is required.
 *
 * Nothing here touches the database, so it can be tested on the host: the
 * one thing it needs to know about the library arrives through 'known'. */
int db_featured_parse(const char *s, db_featured_known_fn known, void *ctx,
                      struct db_featured_names *out);

/* Whether two names are the same artist: equal once trimmed and folded to
 * lower case. ASCII case only -- no accent folding, no "The " prefix. */
bool db_featured_name_eq(const char *a, int alen, const char *b, int blen);

/* ---- the table ---------------------------------------------------------
 *
 * db_featured.c, over the database: every guest the library credits, and the
 * tracks that credit them. One table serves both ways of reading it -- the
 * list of guests, and the guests of one artist.
 *
 * It is fixed-size and permanently resident, because the alternative is a
 * core_alloc from a screen, which shrinks the audio buffer and rebuffers the
 * current track. What does not fit is dropped; db_featured_get_stats() says
 * whether that happened. */

#define DB_FEATURED_GUEST_MAX   384
#define DB_FEATURED_PAIR_MAX   1024
#define DB_FEATURED_ARENA      (8 * 1024)

/* Build the table, discarding whatever was there. False, and an empty table,
 * if it cannot be built: the setting off, the database not loaded to RAM, or
 * a commit in progress.
 *
 * This crawls three tag files and does not yield. Call it where a screen is
 * entered, not from a redraw. */
bool db_featured_build(void);

/* Build it if it has not been built, or if the database has changed since it
 * was. Everything that reads the table calls this first; after the first one
 * per boot it costs a pair of comparisons.
 *
 * A play does not count as a change. tagcache's serial moves for every track
 * finished and its commitid only when the library itself does, and a credit
 * lives in a tag -- so watching the serial would rebuild the whole table
 * after every song. */
void db_featured_ensure(void);

/* Guests in the table, and one guest's name and track count.
 * 0 <= n < db_featured_count(). */
int db_featured_count(void);
const char *db_featured_name(int n);
int db_featured_track_count(int n);

/* Guest n's tracks as master-index ids, at most 'max' of them into 'out'.
 * Returns how many were written. */
int db_featured_track_ids(int n, int32_t *out, int max);

/* The tracks that credit 'artist' as a guest **on somebody else's record**.
 * Returns how many there are, writing at most 'max' of them into 'out' --
 * which may be NULL to ask only the count, which is how the browser decides
 * whether the row is worth drawing.
 *
 * An artist whose own album has a track titled "Song (feat. Someone Else)"
 * credits themselves on their own record, and finding that under their own
 * "featured in" reads as a bug. That is what each pair's owner is for. */
int db_featured_guest_tracks(const char *artist, int32_t *out, int max);

/* Where guest n can be browsed to as an album artist in their own right, or
 * -1 if the library holds no albums under that name -- which is the usual
 * case, and the reason the feature is worth having. */
long db_featured_artist_seek(int n);

/* What the last build filled, for the debug screen -- enough to say whether
 * the fixed sizes above still fit the library. */
struct db_featured_stats {
    int titles;      /* tag_title entries crawled */
    int artists;     /* album artists known to the split test */
    int tracks;      /* guest/track pairs recorded */
    int arena_used;  /* bytes of the name arena */
    bool truncated;  /* something did not fit and was dropped */
};
void db_featured_get_stats(struct db_featured_stats *out);

#endif /* _DB_FEATURED_H */
