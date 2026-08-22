/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * FNV-1a, and the power of two a hash table rounds up to.
 *
 * Seven places in apps-ipod/ hash something to find it again -- guest names,
 * the summary's artist/album keys, a compiled filter chain, a root menu's row
 * list, and three of the playback viewer's tables. They were seven copies of
 * the same two constants, which is one place for them to disagree per copy.
 *
 * A byte at a time rather than one whole-string call, because half of those
 * seven do not hash a plain string: one folds case and trims as it goes, one
 * joins two strings with a NUL between them, one walks a struct. Those keep
 * their own loops and take the arithmetic from here.
 *
 * Not a cryptographic hash and not a stable one: nothing may write a value
 * from here to disk and expect a later build to reproduce it.
 ****************************************************************************/

#ifndef _APP_HASH_H_
#define _APP_HASH_H_

#include <stddef.h>
#include <stdint.h>

#define FNV1A_BASIS 2166136261u
#define FNV1A_PRIME 16777619u

/* One byte into a running hash. Start from FNV1A_BASIS. */
static inline uint32_t fnv1a_byte(uint32_t h, unsigned char b)
{
    return (h ^ b) * FNV1A_PRIME;
}

static inline uint32_t fnv1a_bytes(const void *p, size_t n)
{
    const unsigned char *b = p;
    uint32_t h = FNV1A_BASIS;

    while (n--)
        h = fnv1a_byte(h, *b++);

    return h;
}

static inline uint32_t fnv1a_str(const char *s)
{
    uint32_t h = FNV1A_BASIS;

    while (*s)
        h = fnv1a_byte(h, (unsigned char)*s++);

    return h;
}

/* The smallest power of two at or above v, and at least 1. What an
 * open-addressed table sizes its slot array to, so a hash can be masked
 * rather than divided. */
static inline int next_pow2(int v)
{
    int p = 1;

    while (p < v)
        p <<= 1;

    return p;
}

#endif /* _APP_HASH_H_ */
