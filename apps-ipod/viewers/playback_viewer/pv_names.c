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
 *   The database, through its path index (tagcache_find_path()). Only while
 *   the database is in RAM, and then for every file it holds, with no disk
 *   access. This is the only source that can name an ALBUM at all.
 *
 *   The filename. For files the database does not know, and for every file
 *   while the database is not in RAM, "Artist - Album - NN Title.ext" is
 *   unpicked, falling back to the parent folder when the name carries no
 *   " - ". That fallback yields the ALBUM directory as the artist under an
 *   <artist>/<album>/<track> layout -- a known and visible weakness, and the
 *   reason the database path exists.
 *
 * Between the two sits pv_moves.c: a file the database does not know at its
 * logged path may be one whose folder has since moved, and pv_names_locate()
 * gives the path it has now, which the database may well know.
 *
 * The database is asked by path_key(), which ignores case and a volume
 * prefix. The artwork cache hashes the folder string as given (see
 * aa_dirname() in metadata/art_cache.c), so artwork resolves for every path
 * the database spells the same way, and a path that differs only in case is
 * named without its art.
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
#include "settings/settings.h"
#include "pv_moves.h"
#include "pv_names.h"

/* The saved name map the database's path index replaced. Nothing writes it;
 * it is removed where it is found. */
#define PV_MAP_PATH  ROCKBOX_DIR "/pv_names.dat"

/* Moves whenever the way a path is named changes, so pv_names_identity()
 * does too and every saved report is rebuilt from the log once. */
#define PV_NAMES_VERSION 2

/* Longest file name taken apart by the filename guesswork. */
#define META_MAX 160

static int  names_db_entries;   /* 0 = no usable database */
static long names_db_commit;

void pv_names_discard(void)
{
    remove(PV_MAP_PATH);
    pv_moves_discard();
}

/* The database as the moved-folder table is keyed to it: its entry count and
 * commit id, or false when it is not there to ask. */
static bool db_state(int *entries, long *commit)
{
    struct tagcache_stat *stat = tagcache_get_stat();
    struct tagcache_marks marks;

    if (!stat || !stat->ready || stat->total_entries <= 0)
        return false;
    tagcache_get_marks(&marks);
    *entries = stat->total_entries;
    *commit = marks.commitid;
    return true;
}

unsigned long pv_names_identity(void)
{
    int entries;
    long commit;
    unsigned long key[5];

    if (!db_state(&entries, &commit))
        return 0;
    key[0] = (unsigned long)entries;
    key[1] = (unsigned long)commit;
    key[2] = pv_moves_ident(entries, commit);
    key[3] = PV_NAMES_VERSION;
    /* Names come from the database only while it is in RAM, so a report
     * built without it is named differently from one built with it. */
    key[4] = tagcache_is_in_ram();
    return fnv1a_bytes(key, sizeof(key));
}

size_t pv_names_init(void *buf, size_t bufsz)
{
    pv_moves_forget();
    remove(PV_MAP_PATH);

    if (!db_state(&names_db_entries, &names_db_commit))
    {
        names_db_entries = 0;
        return 0;               /* no database: filenames it is */
    }

    /* The table follows the database: a commit is when folders move. */
    if (pv_moves_stale(names_db_entries, names_db_commit))
        pv_moves_build(buf, bufsz, names_db_entries, names_db_commit);
    return pv_moves_load(buf, bufsz, names_db_entries, names_db_commit);
}

void pv_names_info(int *db_entries, int *mapped)
{
    int slots = 0, found, missed;

    tagcache_path_index_info(&slots, &found, &missed);
    if (db_entries)
        *db_entries = names_db_entries;
    if (mapped)
        *mapped = names_db_entries ? slots : 0;
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
 * A path with one folder in it has no grandparent to take, and there the
 * folder above is the artist. One with none leaves both empty, which the
 * caller answers with "(unknown)" -- so both are cleared here rather than
 * left to whatever the caller's buffer held. */
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

    artist[0] = album[0] = '\0';

    for (const char *p = path; *p; p++)
        if (*p == '/')
        {
            sl[0] = sl[1];
            sl[1] = sl[2];
            sl[2] = p;
            n++;
        }

    /* Three, not four. sl[] holds the last three separators whatever the
     * depth, so /Artist/Album/track.mp3 -- a library at the volume root --
     * brackets the same way a deeper one does, with the leading slash as
     * sl[0]. Asking for four sends that path to the branch below, which then
     * names the ALBUM folder as the artist: the very reading this exists to
     * stop, one directory shallower. */
    if (n >= 3)
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

/* Whether names can come from the database now, or never will this session:
 * no database to name from, or one the RAM copy is switched off for. False
 * while the RAM copy is still to load, which is the case a saved report must
 * not be built in. */
bool pv_names_complete(void)
{
    return names_db_entries == 0 || tagcache_is_in_ram()
        || global_settings.tagcache_ram == TAGCACHE_RAM_OFF;
}

/* Moves apply only to paths the database is known to lack, so without the
 * index nothing is moved. */
const char *pv_names_locate(const char *path)
{
    const char *moved;

    if (!tagcache_is_in_ram() || tagcache_find_path(path) >= 0)
        return path;
    moved = pv_moves_apply(path);
    return moved ? moved : path;
}

enum pv_name_src pv_names_resolve(const char *path, char *artist,
                                      char *title, char *album)
{
    int idx_id = tagcache_find_path(path);

    album[0] = '\0';

    /* Both halves of the name have to be real, or the filename is the better
     * answer -- half a database name is worse than a whole guessed one.
     * tagcache_entry_string() refuses <Untagged> itself. */
    if (idx_id >= 0
        && tagcache_entry_string(idx_id, tag_artist, artist, PV_NAME_MAX)
        && tagcache_entry_string(idx_id, tag_title, title, PV_NAME_MAX)
        && artist[0] && title[0])
    {
        if (!tagcache_entry_string(idx_id, tag_album, album, PV_NAME_MAX))
            album[0] = '\0';
        return PV_NAME_DB;
    }

    path_to_meta(path, artist, title, album);
    return PV_NAME_PATH;
}
