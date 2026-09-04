/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Turns a logged file path into an artist, an album and a title.
 *
 * The playback log records paths and nothing else, so the names have to come
 * from somewhere. Two sources, in order of how much they know:
 *
 *   The database. Every file the tagcache knows is resolved once into
 *   {path hash -> artist, album, title}, strings pooled, and that map is
 *   saved keyed to the database's entry count so later runs skip the sweep.
 *   This is the only source that can name an ALBUM at all.
 *
 *   The filename. For files the database does not know, "Artist - Album - NN
 *   Title.ext" is unpicked, falling back to the parent folder when the name
 *   carries no " - ". That fallback yields the ALBUM directory as the artist
 *   under an <artist>/<album>/<track> layout -- a known and visible weakness,
 *   and the reason the database path exists.
 *
 * Both keys are the path exactly as logged, with no normalisation. That
 * matters beyond this file: the artwork cache hashes the same string (see
 * aa_dirname() in metadata/art_cache.c), so artwork resolves exactly when
 * names do. If one silently misses, so does the other.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <string-extra.h>
#include <file.h>
#include "config.h"
#include "system/hash.h"
#include "rbpaths.h"
#include "database/tagcache.h"
#include "widgets/splash.h"
#include "pv_names.h"

/* Saved map. The header carries the database entry count it was built
 * against; anything else and the map is rebuilt rather than trusted. */
#define PV_MAP_PATH  ROCKBOX_DIR "/pv_names.dat"
#define PV_MAP_MAGIC 0x50564e31UL   /* "PVN1" */

/* Longest metadata string read out of the database. Anything past this is
 * truncated, which is what the aggregates would do to it anyway. */
#define META_MAX 160

struct map_entry
{
    unsigned int hash;       /* of the full path, as logged */
    unsigned int artist;     /* offsets into the pool; 0 = unknown */
    unsigned int album;
    unsigned int title;
};

static struct map_entry *map;
static int  *map_slots;      /* slot -> entry index + 1; 0 = empty */
static int   map_n, map_cap, map_mask;
static char *map_pool;
static unsigned map_pool_used, map_pool_cap;
static int   map_db_entries;
static bool  map_swept;      /* the map was rebuilt, not read back */

/* Index over the pool, so interning a string is a hash lookup rather than a
 * walk of everything interned so far. Without it the sweep is quadratic: a
 * 3,500-track library interns ~10,000 strings against a pool that ends up
 * holding ~3,500, which is around 18 million string compares and visibly
 * slower with every entry.
 *
 * Only the sweep uses it: a map read back from disk needs no interning. So it
 * sits at the very top of the region and is NOT counted in what this module
 * reports as used -- the caller's own allocations are expected to land on top
 * of it, which is safe precisely because nothing reads it once the sweep has
 * finished. On a 512 KB buffer that 32 KB is the difference between the
 * aggregate tables holding a real library and not. */
static unsigned int *pool_slots;   /* pool offset + 1; 0 = empty */
static int pool_slot_mask;
static int pool_slot_n, pool_slot_max;

/* ------------------------------------------------------------------ pool */

/* Intern a string, returning its offset. Offset 0 is the empty string, which
 * is also what a caller gets when the pool is full -- a name going unknown is
 * a worse answer, not a broken one.
 *
 * Deduping matters because artist and album names repeat across every track
 * of a folder; titles almost never repeat and mostly just fill the pool. */
static unsigned int pool_intern(const char *s)
{
    unsigned int h, off, len;
    int i = 0;

    if (!s || !s[0])
        return 0;

    h = fnv1a_str(s);

    if (pool_slots)
    {
        i = (int)(h & (unsigned)pool_slot_mask);
        while (pool_slots[i])
        {
            off = pool_slots[i] - 1;
            if (!strcmp(map_pool + off, s))
                return off;
            i = (i + 1) & pool_slot_mask;
        }
    }

    len = strlen(s) + 1;
    if (map_pool_used + len > map_pool_cap)
        return 0;

    memcpy(map_pool + map_pool_used, s, len);
    off = map_pool_used;
    map_pool_used += len;

    /* Past the load factor the index stops taking new strings. Lookups still
     * terminate -- there are always empty slots -- and the only cost is that
     * a later repeat of this string is interned again instead of being
     * found, which wastes pool space rather than breaking anything. */
    if (pool_slots && pool_slot_n < pool_slot_max)
    {
        pool_slots[i] = off + 1;
        pool_slot_n++;
    }
    return off;
}

/* ----------------------------------------------------------------- table */

static void map_index(void)
{
    memset(map_slots, 0, (size_t)(map_mask + 1) * sizeof(int));
    for (int i = 0; i < map_n; i++)
    {
        int s = (int)(map[i].hash & (unsigned)map_mask);
        while (map_slots[s])
            s = (s + 1) & map_mask;
        map_slots[s] = i + 1;
    }
}

static const struct map_entry *map_get(const char *path)
{
    unsigned int h;
    int s;

    if (!map_n)
        return NULL;

    h = fnv1a_str(path);
    s = (int)(h & (unsigned)map_mask);
    while (map_slots[s])
    {
        const struct map_entry *e = &map[map_slots[s] - 1];
        if (e->hash == h)
            return e;      /* a collision costs one file the wrong name */
        s = (s + 1) & map_mask;
    }
    return NULL;
}

/* ------------------------------------------------------------ persistence */

/* All three writes checked, and the file removed if any falls short.
 *
 * Less costly to get wrong than the badge file, because map_load() rejecting a
 * short file makes map_sweep() rebuild the map rather than losing anything --
 * but that sweep is a pass over every file the database knows, which the
 * comment on it calls seeky on a spinning disk. Leaving a half-written map on
 * disk buys one of those on the next open for nothing. */
static void map_save(void)
{
    unsigned long hdr[4];
    size_t map_bytes;
    bool ok;
    int fd = open(PV_MAP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd < 0)
        return;

    hdr[0] = PV_MAP_MAGIC;
    hdr[1] = (unsigned long)map_db_entries;
    hdr[2] = (unsigned long)map_n;
    hdr[3] = map_pool_used;
    map_bytes = (size_t)map_n * sizeof(struct map_entry);

    ok = write(fd, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
      && write(fd, map, map_bytes) == (ssize_t)map_bytes
      && write(fd, map_pool, map_pool_used) == (ssize_t)map_pool_used;
    close(fd);

    if (!ok)
        remove(PV_MAP_PATH);
}

/* True if the saved map matches the database as it stands and was read whole. */
static bool map_load(void)
{
    unsigned long hdr[4];
    int fd = open(PV_MAP_PATH, O_RDONLY);
    bool ok = false;

    if (fd < 0)
        return false;

    if (read(fd, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
        && hdr[0] == PV_MAP_MAGIC
        && (int)hdr[1] == map_db_entries
        && (int)hdr[2] > 0 && (int)hdr[2] <= map_cap
        && hdr[3] > 0 && hdr[3] <= map_pool_cap)
    {
        int n = (int)hdr[2];
        size_t bytes = (size_t)n * sizeof(struct map_entry);

        if (read(fd, map, bytes) == (ssize_t)bytes
            && read(fd, map_pool, hdr[3]) == (ssize_t)hdr[3])
        {
            map_n = n;
            map_pool_used = (unsigned)hdr[3];
            map_index();
            ok = true;
        }
        else
        {
            map_n = 0;      /* a partial read is not a map */
        }
    }

    close(fd);
    return ok;
}

/* One pass over every file the database knows. Seeky on a spinning disk, so
 * it only ever runs when the saved map does not match. */
static void map_sweep(void)
{
    struct tagcache_search tcs;
    char fname[MAX_PATH];
    char meta[META_MAX];

    if (!tagcache_search(&tcs, tag_filename))
        return;

    while (map_n < map_cap && tagcache_get_next(&tcs, fname, sizeof(fname)))
    {
        struct map_entry *e = &map[map_n];

        e->hash = fnv1a_str(fname);
        e->artist = tagcache_retrieve(&tcs, tcs.idx_id, tag_artist,
                                      meta, sizeof(meta))
                    ? pool_intern(meta) : 0;
        e->album  = tagcache_retrieve(&tcs, tcs.idx_id, tag_album,
                                      meta, sizeof(meta))
                    ? pool_intern(meta) : 0;
        e->title  = tagcache_retrieve(&tcs, tcs.idx_id, tag_title,
                                      meta, sizeof(meta))
                    ? pool_intern(meta) : 0;
        map_n++;

        if ((map_n & 63) == 0)
            splashf(0, "Reading the database (%d/%d)", map_n, map_db_entries);
    }

    tagcache_search_finish(&tcs);
}

size_t pv_names_init(void *buf, size_t bufsz)
{
    struct tagcache_stat *stat;
    char *base = buf;
    size_t used = 0;
    int slots;

    map = NULL;
    map_slots = NULL;
    map_pool = NULL;
    pool_slots = NULL;
    map_n = map_cap = 0;
    map_pool_used = 0;
    pool_slot_n = 0;
    map_db_entries = 0;

    stat = tagcache_get_stat();
    if (!stat || !stat->ready || stat->total_entries <= 0)
        return 0;               /* no database: filenames it is */

    map_db_entries = stat->total_entries;
    map_cap = map_db_entries;
    slots = next_pow2(map_cap * 2);

    /* 24 bytes an entry covers an artist, an album and a title for a
     * typically-named library, with the repeats deduped away. Offsets are
     * full ints rather than 16-bit: a pool capped at 64 KB loses every name
     * past that point, and the screen then falls back to guesswork from the
     * filename without saying it has. */
    map_pool_cap = (unsigned)map_cap * 24;
    if (map_pool_cap < 4096)
        map_pool_cap = 4096;

    /* What survives this call, and is charged to the caller. */
    used = (size_t)map_cap * sizeof(struct map_entry)
         + (size_t)slots * sizeof(int)          /* entry index */
         + map_pool_cap;

    /* Half the buffer at most, counting the sweep's scratch, since that is
     * the high-water mark even though it is not charged. Past that the
     * aggregates would be squeezed into uselessness, and filename guesswork
     * over a full set of tables beats perfect names over a truncated one. */
    if (used + (size_t)slots * sizeof(unsigned int) > bufsz / 2)
    {
        map_cap = 0;
        return 0;
    }

    map       = (struct map_entry *)base;
    map_slots = (int *)(base + (size_t)map_cap * sizeof(struct map_entry));
    map_pool  = (char *)map_slots + (size_t)slots * sizeof(int);
    /* Above everything that survives, so the caller reuses it. */
    pool_slots = (unsigned int *)(map_pool + map_pool_cap);
    map_mask  = slots - 1;

    pool_slot_mask = slots - 1;
    pool_slot_max  = slots * 3 / 4;
    pool_slot_n    = 0;
    memset(pool_slots, 0, (size_t)slots * sizeof(unsigned int));

    map_pool[0] = '\0';         /* offset 0 is the empty string */
    map_pool_used = 1;

    map_swept = false;
    if (!map_load())
    {
        map_n = 0;
        map_pool_used = 1;
        map_sweep();
        map_swept = true;
        if (map_n > 0)
        {
            map_index();
            map_save();
        }
    }

    if (map_n == 0)
    {
        map = NULL;             /* nothing usable; fall back everywhere */
        return 0;
    }

    /* The pool index is scratch from here on. Say so by not counting it. */
    pool_slots = NULL;

    /* Round up so whatever the caller puts next stays 4-byte aligned. */
    return (used + 3u) & ~(size_t)3u;
}

void pv_names_info(int *db_entries, int *mapped, bool *swept)
{
    if (db_entries)
        *db_entries = map_db_entries;
    if (mapped)
        *mapped = map_n;
    if (swept)
        *swept = map_swept;
}

/* ------------------------------------------------- filename guesswork */

/* Skip a leading "NN", "NN.", "NN -" or "NN_" track number. */
static char *strip_tracknum(char *s)
{
    char *d = s;

    while (*d >= '0' && *d <= '9')
        d++;
    if (d != s)
    {
        while (*d == '.' || *d == ' ' || *d == '-' || *d == '_')
            d++;
        if (*d)
            return d;
    }
    return s;
}

/* The same for an artist field, but only when a dot follows the digits -- so
 * a playlist index like "01. Artist" collapses while "21 Savage" and "311"
 * survive intact. */
static char *strip_tracknum_dot(char *s)
{
    char *d = s;

    while (*d >= '0' && *d <= '9')
        d++;
    if (d != s && *d == '.')
    {
        d++;
        while (*d == ' ' || *d == '_')
            d++;
        if (*d)
            return d;
    }
    return s;
}

/* Locate a " - " separator; returns a pointer to the space. */
static const char *find_dash_sep(const char *s)
{
    for (; s[0]; s++)
    {
        if (s[0] == ' ' && s[1] == '-' && s[2] == ' ')
            return s;
    }
    return NULL;
}

/* Artist and title from a path, preferring the filename's own convention
 * over the folder, which is often named for a format rather than a person:
 *
 *   /x/Velvet Antenna FLAC/Velvet Antenna - Peel - 01 Peel.flac
 *       -> "Velvet Antenna", "Peel"
 *   /x/Artist - 01 Title.mp3          -> "Artist", "Title"
 *   /x/Velvet Antenna/02. Peel.mp4    -> "Velvet Antenna", "Peel"
 */
/* Whether the field before a " - " is a track number rather than a name.
 *
 * "05 - Respect.flac" is a track and a title, not an artist and a title.
 * Reading it the other way makes an artist called "05" that collects every
 * fifth track on the player into one row -- which then ranks on the total and
 * arrives in the top ten as a card with a number where a name should be.
 * Measured against a real 10,000-play log, eight of the top eighteen artists
 * were track numbers.
 *
 * Four digits rather than three, because a leading number that long is a year
 * and not a name either. An artist genuinely called "112" loses, and still
 * gets the right name from its folder. */
static bool is_tracknum(const char *s, int n)
{
    if (n < 1 || n > 4)
        return false;
    for (int i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

/* Artist and album from the folders holding the file.
 *
 * Two above the file, not one. A ripped library is
 * <root>/<library>/<artist>/<album>/<track>, so the folder immediately above
 * a track is its ALBUM -- taking that as the artist made "1989 (Taylor's
 * Version)" the second most played artist on the same log. The album folder
 * is worth keeping too: without it a path-resolved entry has no album at all
 * and the model falls back to naming the album after the artist.
 *
 * A shallower path has no grandparent to take, and there the folder above is
 * the artist. */
static void folders_to_meta(const char *path, char *artist, char *album)
{
    /* The last three separators, kept as a sliding window rather than an
     * array of the first so many. Only the tail of a path says anything: the
     * three here bracket the album folder, the artist folder and the file,
     * and a window has no depth past which it starts answering with folders
     * from near the root.
     *
     * strlcpy's size counts the terminator, so the gap between two
     * separators is exactly the room the name between them needs. */
    const char *sl[3] = { NULL, NULL, NULL };
    int n = 0;

    for (const char *p = path; *p; p++)
        if (*p == '/')
        {
            sl[0] = sl[1];
            sl[1] = sl[2];
            sl[2] = p;
            n++;
        }

    if (n >= 4)
    {
        strlcpy(artist, sl[0] + 1,
                (size_t)(sl[1] - sl[0]) < PV_NAME_MAX
                    ? (size_t)(sl[1] - sl[0]) : PV_NAME_MAX);
        strlcpy(album, sl[1] + 1,
                (size_t)(sl[2] - sl[1]) < PV_NAME_MAX
                    ? (size_t)(sl[2] - sl[1]) : PV_NAME_MAX);
    }
    else if (n >= 2)
    {
        strlcpy(artist, sl[1] + 1,
                (size_t)(sl[2] - sl[1]) < PV_NAME_MAX
                    ? (size_t)(sl[2] - sl[1]) : PV_NAME_MAX);
    }
}

static void path_to_meta(const char *path, char *artist, char *title,
                         char *album)
{
    const char *last  = strrchr(path, '/');
    const char *fname = last ? last + 1 : path;
    const char *sep1;
    char stem[META_MAX];
    char *dot;

    strlcpy(stem, fname, sizeof(stem));
    dot = strrchr(stem, '.');
    if (dot && dot != stem)
        *dot = '\0';

    sep1 = find_dash_sep(stem);
    if (sep1 && !is_tracknum(stem, (int)(sep1 - stem)))
    {
        const char *sep2;
        char *after1, *tsrc, *as;
        int n = (int)(sep1 - stem);

        if (n > PV_NAME_MAX - 1)
            n = PV_NAME_MAX - 1;
        memcpy(artist, stem, n);
        artist[n] = '\0';
        as = strip_tracknum_dot(artist);
        if (as != artist)
            strlcpy(artist, as, PV_NAME_MAX);

        /* The title is whatever follows the album, or the artist when there
         * is no album field. */
        after1 = (char *)(sep1 + 3);
        sep2 = find_dash_sep(after1);
        tsrc = sep2 ? (char *)(sep2 + 3) : after1;
        strlcpy(title, strip_tracknum(tsrc), PV_NAME_MAX);
    }
    else
    {
        /* No name in the filename, so the folders carry it. strip_tracknum()
         * takes "05 - " off the front of the title on its own -- a space, a
         * dash and a space are all in the set it eats. */
        folders_to_meta(path, artist, album);
        strlcpy(title, strip_tracknum(stem), PV_NAME_MAX);
    }

    if (artist[0] == '\0')
        strlcpy(artist, "(unknown)", PV_NAME_MAX);
    if (title[0] == '\0')
        strlcpy(title, fname, PV_NAME_MAX);
}

/* The database's marker for a field the file does not carry. */
static bool is_real(const char *s)
{
    return s[0] && strcmp(s, "<Untagged>") != 0;
}

enum pv_name_src pv_names_resolve(const char *path, char *artist,
                                      char *title, char *album)
{
    const struct map_entry *e = map ? map_get(path) : NULL;

    album[0] = '\0';

    if (e)
    {
        const char *ar = map_pool + e->artist;
        const char *ti = map_pool + e->title;
        const char *al = map_pool + e->album;

        /* Both halves of the name have to be real, or the filename is the
         * better answer -- half a database name is worse than a whole
         * guessed one. */
        if (is_real(ar) && is_real(ti))
        {
            strlcpy(artist, ar, PV_NAME_MAX);
            strlcpy(title, ti, PV_NAME_MAX);
            if (is_real(al))
                strlcpy(album, al, PV_NAME_MAX);
            return PV_NAME_DB;
        }
    }

    path_to_meta(path, artist, title, album);
    return PV_NAME_PATH;
}
