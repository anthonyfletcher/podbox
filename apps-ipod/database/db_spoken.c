/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Which of the library's genres name spoken word rather than music.
 *
 * No tag says a track is an audiobook, so the genre is the whole of the
 * evidence: a book tagged as music is music here, and retagging is the fix.
 * Neither the track length nor the title is read. Both look like an audiobook
 * often enough to be tempting and are shared with live sets, DJ mixes and
 * anything whose movements are called "Part II".
 *
 * Genre is unique-valued in the database (TAGCACHE_UNIQUE_TAGS), so a track's
 * index entry holds the *seek* of its genre's single entry in the genre tag
 * file. That is what keeps this cheap. The strings are matched once, over the
 * hundred-odd distinct genres a library has, and what is kept is their seeks;
 * asking about a track is then a walk of that short array with no string work
 * at all, and it costs the same whether or not the database is in RAM.
 *
 * Parts, in order:
 *   - the genre names, and matching one
 *   - the seek table, and reading it out of the database
 *   - the albums and artists that hold spoken word, for Search
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "logf.h"
#include "string-extra.h"
#include "database/tagcache.h"
#include "database/db_spoken.h"

/* ------------------------------------------------------------------ *
 * the genre names                                                    *
 * ------------------------------------------------------------------ */

/* Long enough to mean only one thing wherever they appear, so a genre that
 * merely contains one counts. This is what catches the compounds people
 * actually write -- "Audiobooks", "Audiobook/Fiction", "Spoken Word & Poetry".
 */
static const char * const contained_names[] = {
    "audiobook",
    "audio book",
    "spoken word",
    NULL
};

/* Short and shared with music, so only a genre that is exactly one of these
 * counts. Containment on "book" would take "Bookends" with it. */
static const char * const exact_names[] = {
    "book",
    "books",
    "spoken",
    "speech",
    "non-music",
    "nonmusic",
    "podcast",
    NULL
};

bool db_spoken_is_spoken_genre(const char *genre)
{
    int i;

    if (!genre || !*genre)
        return false;

    for (i = 0; contained_names[i]; i++)
    {
        if (strcasestr(genre, contained_names[i]))
            return true;
    }

    for (i = 0; exact_names[i]; i++)
    {
        if (!strcasecmp(genre, exact_names[i]))
            return true;
    }

    return false;
}

/* ------------------------------------------------------------------ *
 * the seek table                                                     *
 * ------------------------------------------------------------------ */

/* Generous: this counts distinct genre *strings* naming spoken word, and a
 * library needs a remarkable number of ways of spelling it to reach the end.
 * Static rather than allocated, because a core_alloc shrinks the audio buffer
 * and rebuffers the current track. */
#define SPOKEN_SEEK_MAX 64

/* Long enough for any genre worth matching; a longer one arrives truncated
 * and simply fails to match. */
#define SPOKEN_GENRE_BUFSZ 128

static long spoken_seek[SPOKEN_SEEK_MAX];
static int spoken_ct;

/* Declared here rather than with the group tables below because
 * db_spoken_build() clears it: a commit moves their seeks too. */
static bool groups_valid;

/* The search state is static rather than automatic because this runs inside
 * tagcache_search(), on whichever thread happened to start a search: a
 * struct tagcache_search is some seven hundred bytes and putting that spike
 * on an arbitrary stack is not worth the byte count it saves. Only one build
 * runs at a time -- tagcache.c guarantees it. */
static struct tagcache_search build_tcs;
static char build_buf[SPOKEN_GENRE_BUFSZ];

bool db_spoken_build(void)
{
    spoken_ct = 0;
    /* The album and artist seeks below are into tag files the commit that
     * brought us here re-sorted, so they are as stale as the genre ones. */
    groups_valid = false;

    if (!tagcache_search(&build_tcs, tag_genre))
        return false;

    /* No filter and no clause, so this walks the genre tag file itself rather
     * than the master index -- one pass over the distinct genres, not over
     * every track. */
    while (tagcache_get_next(&build_tcs, build_buf, sizeof build_buf))
    {
        if (!db_spoken_is_spoken_genre(build_buf))
            continue;

        if (spoken_ct >= SPOKEN_SEEK_MAX)
        {
            logf("db_spoken: seek table full");
            break;
        }

        spoken_seek[spoken_ct++] = build_tcs.result_seek;
    }

    tagcache_search_finish(&build_tcs);

    logf("db_spoken: %d spoken genres", spoken_ct);

    return true;
}

bool db_spoken_is_spoken_seek(long genre_seek)
{
    int i;

    for (i = 0; i < spoken_ct; i++)
    {
        if (spoken_seek[i] == genre_seek)
            return true;
    }

    return false;
}

/* ------------------------------------------------------------------ *
 * the albums and artists that hold spoken word                       *
 * ------------------------------------------------------------------ */

/* A library with more spoken-word albums than this keeps the first of them,
 * and Search treats the rest as music. */
#define SPOKEN_GROUP_MAX 192

static long spoken_album[SPOKEN_GROUP_MAX];
static int spoken_album_ct;
static long spoken_artist[SPOKEN_GROUP_MAX];
static int spoken_artist_ct;

/* Its own search state, not build_tcs. A search started here runs
 * spoken_table_update() on the way in, which may rebuild the genre table
 * through db_spoken_build() -- and that uses build_tcs. Sharing one struct
 * meant the nested search clobbering the one being set up around it. */
static struct tagcache_search group_tcs;
static uint32_t group_uniqbuf[SPOKEN_GROUP_MAX];

static const struct tagcache_search_clause is_spoken_clause = {
    .tag = tag_virt_spoken,
    .type = clause_is,
    .numeric = true,
    .source = source_constant,
    .numeric_data = 1,
    .str = NULL,
};

static int collect_spoken_group(int tag, long *out, int max)
{
    int n = 0;

    if (!tagcache_search(&group_tcs, tag))
        return 0;

    /* Without this the search reports one result per track rather than one
     * per album, and a single book would fill the table on its own. */
    tagcache_search_set_uniqbuf(&group_tcs, group_uniqbuf,
                                sizeof(group_uniqbuf));
    tagcache_search_add_clause(&group_tcs,
                               (struct tagcache_search_clause *)&is_spoken_clause);

    while (tagcache_get_next(&group_tcs, build_buf, sizeof(build_buf)))
    {
        if (n >= max)
        {
            logf("db_spoken: group table full");
            break;
        }
        out[n++] = group_tcs.result_seek;
    }

    tagcache_search_finish(&group_tcs);

    return n;
}

void db_spoken_groups_ensure(void)
{
    if (groups_valid)
        return;

    spoken_album_ct = collect_spoken_group(tag_album, spoken_album,
                                           SPOKEN_GROUP_MAX);
    spoken_artist_ct = collect_spoken_group(tag_albumartist, spoken_artist,
                                            SPOKEN_GROUP_MAX);
    groups_valid = true;

    logf("db_spoken: %d spoken albums, %d artists",
         spoken_album_ct, spoken_artist_ct);
}

static bool seek_in(const long *tab, int ct, long seek)
{
    int i;

    for (i = 0; i < ct; i++)
    {
        if (tab[i] == seek)
            return true;
    }

    return false;
}

bool db_spoken_album_is_spoken(long album_seek)
{
    return seek_in(spoken_album, spoken_album_ct, album_seek);
}

bool db_spoken_artist_is_spoken(long artist_seek)
{
    return seek_in(spoken_artist, spoken_artist_ct, artist_seek);
}
