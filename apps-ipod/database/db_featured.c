/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The guest table: every name the library credits as a guest, and the tracks
 * that credit it.
 *
 * The database has no tag for a guest appearance and cannot grow one, so this
 * is derived rather than stored -- crawled out of the title and per-track
 * artist tags by db_featured_parse.c, which is where the reading of the prose
 * lives. Nothing here parses; nothing there touches the database.
 *
 * The table is fixed-size BSS. Any allocation from a screen shrinks the audio
 * buffer and rebuffers the current track, and ~30K is not a number worth that
 * on a player with 32MB. What does not fit is dropped and reported.
 *
 * Parts, in order:
 *   - the table, and what it is made of
 *   - the album-artist set the parse asks about, and the names a user adds
 *   - recording a guest and a track
 *   - the three crawls that fill it
 *   - resolving the two things a crawl cannot say
 *   - db_featured_build() driving them, and reading the result
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "system/hash.h"
#include "system.h"
#include "rbpaths.h"
#include "file.h"
#include "settings/settings.h"
#include "system/strutil.h"         /* read_line */
#include "database/tagcache.h"
#include "db_featured.h"

/* ------------------------------------------------------------------ *
 * the table                                                          *
 * ------------------------------------------------------------------ */

/* One guest. 'key' is the name's hash, held so that finding a guest by name
 * is a walk over 32-bit words rather than over strings -- this is asked once
 * per credit in the library, which is often enough for it to matter. */
struct guest {
    uint32_t key;
    uint16_t name_off;      /* into guest_arena, NUL-terminated */
    uint16_t track_count;   /* pairs naming this guest */
    int32_t  artist_seek;   /* tag_albumartist seek, or -1 if not one */
};

/* One track crediting one guest. There is no per-guest run of these and no
 * index into them: a screen wanting a guest's tracks walks the array once,
 * which happens when it opens and never during a redraw. */
struct pair {
    int32_t  idx_id;    /* the crediting track, in the master index */
    uint32_t owner;     /* hash of that track's album artist */
    uint16_t guest;     /* into guests[] */
};

static char guest_arena[DB_FEATURED_ARENA];
static int arena_used;
static struct guest guests[DB_FEATURED_GUEST_MAX];
static int guest_ct;
static struct pair pairs[DB_FEATURED_PAIR_MAX];
static int pair_ct;

static int titles_scanned;
static bool truncated;

/* FNV-1a over a name, folded to lower case and trimmed, so that it stands for
 * the same identity db_featured_name_eq() does. Never returns 0: the artist
 * set below spends that value on "empty slot".
 *
 * A collision merges two guests, showing one name against the other's tracks.
 * At a few hundred names in 32 bits that is a one-in-fifty-thousand event, and
 * the compare that follows a hash match makes it nothing at all. */
static uint32_t name_hash(const char *s, int len)
{
    uint32_t h = FNV1A_BASIS;
    const char *e = s + len;

    while (s < e && (*s == ' ' || *s == '\t'))
        s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        e--;

    for (; s < e; s++)
    {
        char c = (*s >= 'A' && *s <= 'Z') ? (char)(*s + ('a' - 'A')) : *s;
        h = fnv1a_byte(h, (unsigned char)c);
    }

    return h ? h : 1;
}

/* ------------------------------------------------------------------ *
 * the album-artist set                                               *
 * ------------------------------------------------------------------ */

/* Which names the library already has as an album artist, as hashes in an
 * open-addressed set. The parse asks this to decide where a credit splits --
 * "A & B" is two artists and "Nick Cave & the Bad Seeds" is one -- and it
 * asks once per separator in the library, so the answer has to be immediate.
 *
 * Hashes rather than names because the answer is only ever yes or no, and
 * holding the names would cost an arena as large again as the guests' own.
 * A false positive keeps a run whole that should have come apart; at these
 * sizes that is rarer than a mis-tagged album. */
#define ARTIST_SLOTS 2048              /* power of two, half-filled at most */
#define ARTIST_MAX   (ARTIST_SLOTS / 2)

static uint32_t artist_slot[ARTIST_SLOTS];
static int artist_ct;

static void artist_set_add(uint32_t h)
{
    unsigned i = h & (ARTIST_SLOTS - 1);

    if (artist_ct >= ARTIST_MAX)
    {
        truncated = true;
        return;
    }

    while (artist_slot[i] != 0)
    {
        if (artist_slot[i] == h)
            return;
        i = (i + 1) & (ARTIST_SLOTS - 1);
    }

    artist_slot[i] = h;
    artist_ct++;
}

static bool artist_set_has(uint32_t h)
{
    unsigned i = h & (ARTIST_SLOTS - 1);

    while (artist_slot[i] != 0)
    {
        if (artist_slot[i] == h)
            return true;
        i = (i + 1) & (ARTIST_SLOTS - 1);
    }

    return false;
}

/* Names the user has told the player about, one per line. An artist whose own
 * name is punctuated -- "Tyler, The Creator" -- splits in two otherwise, since
 * nothing in the string says whether that comma is inside a name or between
 * two, and the library only knows if it has an album under it.
 *
 * They go into the same set the album artists do, so they answer the same
 * question and need no rule of their own. That is also what makes them work
 * inside a longer credit: "feat. Tyler, The Creator & Drake" cuts after the
 * longest opening run the set knows, which is now the whole of the first name.
 *
 * Absent is the ordinary case and costs one failed open. Read at build time,
 * so an edit takes effect at the next boot -- or at the next database update,
 * or from the debug screen, both of which build the table again. */
#define KNOWN_ARTISTS_FILE ROCKBOX_DIR "/known_artists.txt"

static void read_known_artists(void)
{
    char line[TAGCACHE_BUFSZ];
    int fd = open(KNOWN_ARTISTS_FILE, O_RDONLY);
    bool first = true;

    if (fd < 0)
        return;

    while (read_line(fd, line, sizeof(line)) > 0)
    {
        char *s = line;

        /* A byte-order mark, which a Windows editor adds without saying so.
         * Left in place it belongs to the first name and nothing matches it. */
        if (first)
        {
            first = false;
            if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
                (unsigned char)s[2] == 0xBF)
                s += 3;
        }

        s = skip_whitespace(s);
        if (*s == '#' || !*s)
            continue;

        artist_set_add(name_hash(s, (int)strlen(s)));
    }

    close(fd);
}

/* The one question db_featured_parse() puts to the library. */
static bool known_artist(const char *name, int len, void *ctx)
{
    (void)ctx;
    return artist_set_has(name_hash(name, len));
}

/* ------------------------------------------------------------------ *
 * recording                                                          *
 * ------------------------------------------------------------------ */

/* The guest called [name, len), or -1. 'key' is its hash, which the callers
 * have already; the string compare only runs where that matches. */
static int guest_find(const char *name, int len, uint32_t key)
{
    for (int i = 0; i < guest_ct; i++)
    {
        const char *held = guest_arena + guests[i].name_off;

        if (guests[i].key == key &&
            db_featured_name_eq(held, (int)strlen(held), name, len))
            return i;
    }

    return -1;
}

/* As above, adding the guest if it is new. -1 if it will not fit. */
static int guest_index(const char *name, int len)
{
    uint32_t key = name_hash(name, len);
    int i = guest_find(name, len, key);

    if (i >= 0)
        return i;

    if (guest_ct >= DB_FEATURED_GUEST_MAX ||
        arena_used + len + 1 > DB_FEATURED_ARENA)
    {
        truncated = true;
        return -1;
    }

    memcpy(guest_arena + arena_used, name, len);
    guest_arena[arena_used + len] = '\0';

    guests[guest_ct].key = key;
    guests[guest_ct].name_off = (uint16_t)arena_used;
    guests[guest_ct].track_count = 0;
    guests[guest_ct].artist_seek = -1;

    arena_used += len + 1;
    return guest_ct++;
}

/* Record that track 'idx_id' credits the guest called [name, len).
 *
 * Room for the track is checked before the guest is added, so a name only
 * ever enters the table along with a track to show for it -- otherwise a
 * table that filled up would leave guests behind whose track list was empty.
 *
 * A track can arrive twice -- once for the "feat. B" in its title and again
 * for the one in its artist tag -- and the two are the same credit, so the
 * pair is looked for before it is appended. */
static void credit(const char *name, int len, int32_t idx_id)
{
    int g, i;

    if (pair_ct >= DB_FEATURED_PAIR_MAX)
    {
        truncated = true;
        return;
    }

    g = guest_index(name, len);
    if (g < 0)
        return;

    for (i = 0; i < pair_ct; i++)
        if (pairs[i].idx_id == idx_id && pairs[i].guest == (uint16_t)g)
            return;

    pairs[pair_ct].idx_id = idx_id;
    pairs[pair_ct].owner = 0;
    pairs[pair_ct].guest = (uint16_t)g;
    pair_ct++;

    guests[g].track_count++;
}

/* ------------------------------------------------------------------ *
 * the crawls                                                         *
 * ------------------------------------------------------------------ */

/* Every album artist into the set above. Must run before either scan below:
 * they cannot decide where a credit splits until it can be answered.
 *
 * A unique tag's file holds one entry per distinct string, so this is the
 * cheapest of the three crawls by a long way. */
static void scan_album_artists(void)
{
    struct tagcache_search tcs;
    char buf[TAGCACHE_BUFSZ];

    if (!tagcache_search(&tcs, tag_albumartist))
        return;

    while (tagcache_get_next(&tcs, buf, sizeof(buf)))
        artist_set_add(name_hash(buf, (int)strlen(buf)));

    tagcache_search_finish(&tcs);
}

/* "Song (feat. X)". Title is not a unique tag, so its file holds one entry
 * per track and the crawl hands back a usable track id with each -- which is
 * what makes this the easy source and the one that dominates the cost.
 *
 * No filter and no clause, so tagcache_get_next() crawls the tag file
 * sequentially rather than walking the master index: the same strings for a
 * fraction of the memory traffic. */
static void scan_titles(void)
{
    struct tagcache_search tcs;
    char buf[TAGCACHE_BUFSZ];
    struct db_featured_names names;

    if (!tagcache_search(&tcs, tag_title))
        return;

    while (tagcache_get_next(&tcs, buf, sizeof(buf)))
    {
        titles_scanned++;

        if (db_featured_parse(buf, known_artist, NULL, &names) == 0)
            continue;

        for (int i = 0; i < names.count; i++)
            credit(names.name[i], names.len[i], tcs.idx_id);
    }

    tagcache_search_finish(&tcs);
}

/* The tracks whose per-track artist is the string at 'seek', credited to
 * every name in 'names'.
 *
 * Trap: a unique tag's crawl yields no track id -- its entries carry
 * idx_id -1 -- so the tracks have to be searched for. That search does walk
 * the master index, and is affordable only because it runs for the handful of
 * artist strings that credit somebody rather than for all of them. */
static void add_artist_tracks(int32_t seek,
                              const struct db_featured_names *names)
{
    struct tagcache_search tcs;
    char buf[TAGCACHE_BUFSZ];

    if (!tagcache_search(&tcs, tag_title))
        return;

    if (tagcache_search_add_filter(&tcs, tag_artist, seek))
        while (tagcache_get_next(&tcs, buf, sizeof(buf)))
            for (int i = 0; i < names->count; i++)
                credit(names->name[i], names->len[i], tcs.idx_id);

    tagcache_search_finish(&tcs);
}

/* "A feat. B" in the per-track artist tag -- the better-quality source, since
 * everything after the marker is a clean name with no bracket around it. */
static void scan_track_artists(void)
{
    struct tagcache_search tcs;
    char buf[TAGCACHE_BUFSZ];
    struct db_featured_names names;

    if (!tagcache_search(&tcs, tag_artist))
        return;

    while (tagcache_get_next(&tcs, buf, sizeof(buf)))
        if (db_featured_parse(buf, known_artist, NULL, &names) > 0)
            add_artist_tracks(tcs.result_seek, &names);

    tagcache_search_finish(&tcs);
}

/* ------------------------------------------------------------------ *
 * what a crawl cannot say                                            *
 * ------------------------------------------------------------------ */

/* Each pair's owner: the album artist of the track that credits the guest.
 * Held as a hash because the only question asked of it is whether it is the
 * artist being looked at, which is what keeps an artist's own tracks out of
 * their own "Featured in" list.
 *
 * Left to a pass of its own rather than taken during the title crawl, so that
 * nothing reaches into the master index while a sequential crawl is running. */
static void resolve_owners(void)
{
    struct tagcache_search tcs;
    char buf[TAGCACHE_BUFSZ];

    if (!tagcache_search(&tcs, tag_albumartist))
        return;

    for (int i = 0; i < pair_ct; i++)
        if (tagcache_retrieve(&tcs, pairs[i].idx_id, tag_albumartist,
                              buf, sizeof(buf)))
            pairs[i].owner = name_hash(buf, (int)strlen(buf));

    tagcache_search_finish(&tcs);
}

/* Where a guest who is also an album artist can be browsed to. The set above
 * says whether one is, but not where, since it holds no seeks -- so the artist
 * file is crawled once more now that the guest list is final. */
static void resolve_artist_seeks(void)
{
    struct tagcache_search tcs;
    char buf[TAGCACHE_BUFSZ];

    if (!tagcache_search(&tcs, tag_albumartist))
        return;

    while (tagcache_get_next(&tcs, buf, sizeof(buf)))
    {
        int len = (int)strlen(buf);
        uint32_t key = name_hash(buf, len);

        for (int i = 0; i < guest_ct; i++)
            if (guests[i].key == key && guests[i].artist_seek < 0 &&
                db_featured_name_eq(guest_arena + guests[i].name_off,
                                    (int)strlen(guest_arena +
                                                guests[i].name_off),
                                    buf, len))
                guests[i].artist_seek = tcs.result_seek;
    }

    tagcache_search_finish(&tcs);
}

/* ------------------------------------------------------------------ *
 * the build, and reading it                                          *
 * ------------------------------------------------------------------ */

static void discard(void)
{
    memset(artist_slot, 0, sizeof(artist_slot));
    artist_ct = 0;
    guest_ct = 0;
    pair_ct = 0;
    arena_used = 0;
    titles_scanned = 0;
    truncated = false;
}

bool db_featured_build(void)
{
    discard();

    if (!global_settings.featured_artists || !tagcache_is_in_ram())
        return false;

    scan_album_artists();
    read_known_artists();
    scan_titles();
    scan_track_artists();

    resolve_owners();
    resolve_artist_seeks();

    return true;
}

/* What the database looked like when the table was last built, and whether
 * that build got as far as finishing. */
static struct tagcache_marks built_marks;
static bool built;

void db_featured_ensure(void)
{
    struct tagcache_marks now;

    if (!global_settings.featured_artists || !tagcache_is_in_ram())
    {
        /* The setting can go off while the table is standing, and the rows
         * drawn from it have to go with it. */
        if (guest_ct > 0)
            db_featured_build();
        built = false;
        return;
    }

    tagcache_get_marks(&now);

    if (built && now.commitid == built_marks.commitid &&
        now.deleted_ct == built_marks.deleted_ct)
        return;

    if (db_featured_build())
    {
        built = true;
        built_marks = now;
    }
}

int db_featured_count(void)
{
    return guest_ct;
}

const char *db_featured_name(int n)
{
    return guest_arena + guests[n].name_off;
}

int db_featured_track_count(int n)
{
    return guests[n].track_count;
}

/* Guest g's tracks, skipping any whose album artist hashes to 'not_owner' --
 * 0 to keep them all. 'out' may be NULL to count without collecting.
 *
 * name_hash() never returns 0, so 0 is free to mean "no filter"; an
 * unresolved owner is 0 as well and is therefore never filtered out, which is
 * the right way round -- a track whose album artist could not be read is not
 * evidence that it belongs to this artist. */
static int collect(int g, uint32_t not_owner, int32_t *out, int max)
{
    int ct = 0;

    for (int i = 0; i < pair_ct && (out == NULL || ct < max); i++)
        if (pairs[i].guest == (uint16_t)g &&
            (not_owner == 0 || pairs[i].owner != not_owner))
        {
            if (out)
                out[ct] = pairs[i].idx_id;
            ct++;
        }

    return ct;
}

int db_featured_track_ids(int n, int32_t *out, int max)
{
    return collect(n, 0, out, max);
}

int db_featured_guest_tracks(const char *artist, int32_t *out, int max)
{
    int len = (int)strlen(artist);
    uint32_t key = name_hash(artist, len);
    int g = guest_find(artist, len, key);

    return g < 0 ? 0 : collect(g, key, out, max);
}

long db_featured_artist_seek(int n)
{
    return guests[n].artist_seek;
}

void db_featured_get_stats(struct db_featured_stats *out)
{
    out->titles = titles_scanned;
    out->artists = artist_ct;
    out->tracks = pair_ct;
    out->arena_used = arena_used;
    out->truncated = truncated;
}
