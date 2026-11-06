/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The name keys a person's picture is found and cached by, and an album's
 * cover is cached by. A tag and a folder
 * or file name are written differently for the same person -- "N*E*R*D" and
 * "N.E.R.D", "AC/DC" and "AC_DC", "Björk" and "Bjork", "The Beatles" and
 * "Beatles, The" -- so neither is compared as written: each is reduced to its
 * letters and digits, Latin ones lowercased and unaccented, and the reduction
 * hashed. Letters of other scripts are kept as they are, or a name in kana
 * would reduce to nothing.
 *
 * A name with one comma has a second key, read as "Pratchett, Terry" ->
 * "Terry Pratchett", which matching tries as well; the cache key is always
 * the first.
 *
 * Parts, in order: the Latin fold table, the character rule, the hashes.
 ****************************************************************************/

#include <string.h>
#include "rbunicode.h"
#include "art_name.h"

/* U+00C0-U+017F to a lowercase base letter. Upper case stands for two
 * letters: A "ae", O "oe", S "ss", T "th", I "ij"; '-' is dropped. */
static const char latin_fold[] =
    "aaaaaaAceeeeiiiidnooooo-ouuuuyTS"      /* U+00C0 */
    "aaaaaaAceeeeiiiidnooooo-ouuuuyTy"      /* U+00E0 */
    "aaaaaaccccccccddddeeeeeeeeeegggg"      /* U+0100 */
    "gggghhhhiiiiiiiiiiIIjjkkklllllll"      /* U+0120 */
    "lllnnnnnnnnnooooooOOrrrrrrssssss"      /* U+0140 */
    "ssttttttuuuuuuuuuuuuwwyyyzzzzzzs";     /* U+0160 */
_Static_assert(sizeof(latin_fold) == 0x180 - 0xC0 + 1, "latin_fold size");

struct name_hash
{
    unsigned int h;
    bool kept;
};

static void add(struct name_hash *nh, unsigned int c)
{
    nh->h = (nh->h ^ c) * 16777619u;
    nh->kept = true;
}

/* One character of a name into the hash: kept, folded or dropped */
static void add_char(struct name_hash *nh, unsigned int c)
{
    static const char * const pairs = "AaeOoeSssTthIij";

    if (c >= 0xFF01 && c <= 0xFF5E)
        c -= 0xFEE0;                        /* fullwidth ASCII */
    if (c < 0x80)
    {
        if (c >= 'A' && c <= 'Z')
            add(nh, c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            add(nh, c);
        else if (c == '&')
        {
            add(nh, 'a');
            add(nh, 'n');
            add(nh, 'd');
        }
        return;
    }
    if (c < 0xC0                            /* Latin-1 symbols */
        || (c >= 0x0300 && c <= 0x036F)     /* combining accents */
        || (c >= 0x2000 && c <= 0x2BFF)     /* punctuation and symbols */
        || (c >= 0x3000 && c <= 0x303F)     /* CJK punctuation */
        || (c >= 0xFE00 && c <= 0xFE0F))    /* variation selectors */
        return;
    if (c < 0x180)
    {
        char f = latin_fold[c - 0xC0];
        const char *p;

        if (f == '-')
            return;
        if (f >= 'a')
        {
            add(nh, f);
            return;
        }
        p = strchr(pairs, f);
        add(nh, p[1]);
        add(nh, p[2]);
        return;
    }
    add(nh, c);
}

/* The characters of [s, end) into the hash. A leading "The " is skipped when
 * 'first', the part the name starts with. */
static void add_part(struct name_hash *nh, const char *s, const char *end,
                     bool first)
{
    const unsigned char *p = (const unsigned char *)s;

    if (first && end - s > 4 && !strncasecmp(s, "the ", 4))
        p += 4;
    while (p < (const unsigned char *)end && *p)
    {
        ucschar_t c;

        p = utf8decode(p, &c);
        add_char(nh, c);
    }
}

static unsigned int finish(const struct name_hash *nh)
{
    if (!nh->kept)
        return 0;
    return nh->h ? nh->h : 1;
}

#define NAME_SEED (0x811C9DC5u ^ 0x5A5A5A5Au)

unsigned int art_name_hash_n(const char *name, size_t len)
{
    struct name_hash nh = { NAME_SEED, false };
    const char *end;

    if (!name)
        return 0;
    end = name + len;
    if (len > 5 && !strncasecmp(end - 5, ", the", 5))
        end -= 5;
    add_part(&nh, name, end, true);
    return finish(&nh);
}

unsigned int art_name_alt_hash_n(const char *name, size_t len)
{
    struct name_hash nh = { NAME_SEED, false };
    const char *end, *comma;

    if (!name)
        return 0;
    end = name + len;
    comma = memchr(name, ',', len);
    if (!comma || memchr(comma + 1, ',', end - comma - 1))
        return 0;
    add_part(&nh, comma + 1 + (comma[1] == ' '), end, true);
    add_part(&nh, name, comma, false);
    return finish(&nh);
}

unsigned int art_name_work_hash(const char *artist, const char *title)
{
    struct name_hash nh = { NAME_SEED, false };

    if (!title || !title[0])
        return 0;
    if (artist)
        add_part(&nh, artist, artist + strlen(artist), true);
    add_part(&nh, title, title + strlen(title), !nh.kept);
    return finish(&nh);
}

#define ALBUM_SEED (0x811C9DC5u ^ 0xA5A5A5A5u)

unsigned int art_name_album_hash(const char *artist, const char *title)
{
    struct name_hash nh = { ALBUM_SEED, false };

    if (!title)
        return 0;
    add_part(&nh, title, title + strlen(title), true);
    if (!nh.kept)
        return 0;
    if (artist)
    {
        nh.h = (nh.h ^ '/') * 16777619u;    /* no name's letter or digit */
        add_part(&nh, artist, artist + strlen(artist), true);
    }
    return finish(&nh);
}

unsigned int art_name_hash(const char *name)
{
    return name ? art_name_hash_n(name, strlen(name)) : 0;
}

unsigned int art_name_alt_hash(const char *name)
{
    return name ? art_name_alt_hash_n(name, strlen(name)) : 0;
}
