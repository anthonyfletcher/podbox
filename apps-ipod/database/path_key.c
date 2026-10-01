/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The one key that names a file. See path_key.h for what it promises.
 *
 * Built into the desktop sound scanner (tools/soundscan) as well as the
 * player, which is why it leans on nothing beyond the C library: the two must
 * agree byte for byte.
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include "database/path_key.h"

#define FNV64_BASIS  0xcbf29ce484222325ULL
#define FNV64_PRIME  0x100000001b3ULL

uint64_t path_key_fold_hash(const char *s)
{
    uint64_t h = FNV64_BASIS;

    for (; s && *s; s++)
    {
        unsigned char c = (unsigned char)*s;

        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';

        h = (h ^ c) * FNV64_PRIME;
    }

    return h;
}

/* The same track arrives under two names. tagcache hands a walk the path as
 * it was scanned, while retrieving an entry dircache holds rebuilds it from
 * the dircache tree, which puts the volume root on the front -- so one file
 * is "/Music/x.flac" through tagcache_get_next() and "/<HDD0>/Music/x.flac"
 * through tagcache_retrieve(). A key has to name the file rather than the
 * route the caller took to it, so every key goes through here.
 *
 * Parsed here rather than by path_strip_volume(), which lives behind
 * HAVE_MULTIVOLUME: the offline tool computes keys that must match the
 * player's byte for byte, without the firmware's path layer. */
const char *path_key_strip(const char *path)
{
    const char *p = path;

    if (path == NULL || p[0] != '/' || p[1] != '<')
        return path;

    for (p += 2; *p != '\0' && *p != '>' && *p != '/'; p++)
        ;

    return (p[0] == '>' && p[1] == '/') ? p + 1 : path;
}

uint64_t path_key(const char *path)
{
    uint64_t h = path_key_fold_hash(path_key_strip(path));

    return h ? h : 1;
}
