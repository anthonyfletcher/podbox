/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Guest credits, read out of the strings the database already holds.
 *
 * There is no tag for a guest appearance. The only record of one is prose --
 * "Song (feat. X)" in a title, "A feat. B" in a per-track artist tag -- so
 * anything that wants to browse by guest has to read the prose. This turns
 * one such string into the names it credits.
 *
 * It is a string routine and nothing more: no database, no file, no
 * allocation, and the names it returns are slices of the caller's string.
 * The single thing it needs to know about the library -- whether a fragment
 * is already an artist in its own right -- arrives through a callback, which
 * is what lets the whole of it run on the host under tools/dbfeat.
 *
 * Parts, in order:
 *   - byte classification and trimming
 *   - the markers, and finding the first one
 *   - how far past a marker the guest list runs
 *   - cutting that list into names
 *   - db_featured_parse() driving the three, and name identity
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "db_featured.h"

/* ------------------------------------------------------------------ *
 * bytes                                                              *
 * ------------------------------------------------------------------ */

/* Whether 'c' continues a word. Tags are UTF-8, so every byte of an accented
 * letter has to count as one: read as a word boundary instead, the trailing
 * byte of the accent in "Defeat" spelled with one would let the "feat" after
 * it stand as a marker. */
static bool is_word_byte(char c)
{
    unsigned char b = (unsigned char)c;

    return (b >= '0' && b <= '9') || (b >= 'a' && b <= 'z') ||
           (b >= 'A' && b <= 'Z') || b >= 0x80;
}

static bool is_space_byte(char c)
{
    return c == ' ' || c == '\t';
}

static char lower_byte(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* Narrow [*start, *end) onto the text it holds, dropping space at both ends.
 */
static void trim(const char **start, const char **end)
{
    while (*start < *end && is_space_byte(**start))
        (*start)++;
    while (*end > *start && is_space_byte((*end)[-1]))
        (*end)--;
}

/* Whether [p, end) opens with 'word', which must be given in lower case. */
static bool word_is(const char *p, const char *end, const char *word, int len)
{
    if (end - p < len)
        return false;

    for (int i = 0; i < len; i++)
        if (lower_byte(p[i]) != word[i])
            return false;

    return true;
}

/* ------------------------------------------------------------------ *
 * markers                                                            *
 * ------------------------------------------------------------------ */

/* What introduces a guest list, longest first so "featuring" is never read as
 * "feat" followed by rubbish. A marker must start a word, and one wanting a
 * separator must also end one -- otherwise "defeat" and "Feather" both match.
 * "w/" brings its own separator with it.
 *
 * "with" is deliberately absent. "Song with a Broken Heart" is a title, not a
 * credit, and there is no test that tells the two apart. */
static const struct {
    const char *text;
    int len;
    bool needs_sep;
} markers[] = {
    { "featuring", 9, true  },
    { "feat",      4, true  },
    { "ft",        2, true  },
    { "w/",        2, false },
};

#define MARKER_COUNT ((int)(sizeof(markers) / sizeof(markers[0])))

/* Where the guest list after the first marker in 's' begins, or NULL if 's'
 * credits nobody. */
static const char *find_marker(const char *s)
{
    for (const char *p = s; *p != '\0'; p++)
    {
        if (p > s && is_word_byte(p[-1]))
            continue;

        for (int m = 0; m < MARKER_COUNT; m++)
        {
            const char *after = p + markers[m].len;
            int i;

            for (i = 0; i < markers[m].len; i++)
                if (lower_byte(p[i]) != markers[m].text[i])
                    break;

            if (i < markers[m].len)
                continue;

            if (!markers[m].needs_sep)
            {
                /* "w/o" is "without", and credits nobody. */
                if (lower_byte(after[0]) == 'o' && !is_word_byte(after[1]))
                    continue;
                return after;
            }
            if (*after == '.')
                return after + 1;
            if (is_space_byte(*after))
                return after;
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ *
 * scope                                                              *
 * ------------------------------------------------------------------ */

/* Where the guest list beginning at 'guests' ends: at the close of the bracket
 * the marker sits inside, at the next bracket to open after it, or at the end
 * of the string, whichever comes first. Both brackets are what keep the
 * "(Live)" out of "Song (feat. X) (Live)" and "Song feat. X (Live)" alike --
 * whatever a bracket opens, it is not more of the name.
 *
 * 's' is the whole string, since the bracket the marker sits inside opened
 * before it. */
static const char *guest_list_end(const char *s, const char *guests)
{
    char open[4];
    int depth = 0;
    const char *end;
    char closer;

    for (const char *p = s; p < guests; p++)
    {
        if (*p == '(' || *p == '[')
        {
            if (depth < (int)sizeof(open))
                open[depth] = *p;
            depth++;
        }
        else if ((*p == ')' || *p == ']') && depth > 0)
            depth--;
    }

    end = guests + strlen(guests);

    /* Unbracketed, or nested past what is worth tracking: no close to find.
     * The close needs no nesting count of its own, since the loop below stops
     * at anything that would have opened one. */
    if (depth > 0 && depth <= (int)sizeof(open))
    {
        closer = (open[depth - 1] == '[') ? ']' : ')';

        for (const char *p = guests; p < end; p++)
            if (*p == closer)
            {
                end = p;
                break;
            }
    }

    for (const char *p = guests; p < end; p++)
        if (*p == '(' || *p == '[')
            return p;

    return end;
}

/* ------------------------------------------------------------------ *
 * cutting                                                            *
 * ------------------------------------------------------------------ */

/* The next separator at or after 'p', with its width in *seplen, or NULL.
 * " and " and " x " separate only as whole words; the space around them is
 * left for trim() to take. */
static const char *next_sep(const char *p, const char *end, int *seplen)
{
    for (; p < end; p++)
    {
        if (*p == '&' || *p == ',' || *p == ';' || *p == '+' || *p == '/')
        {
            *seplen = 1;
            return p;
        }

        if (!is_space_byte(*p))
            continue;

        if (word_is(p + 1, end, "and", 3) && is_space_byte(p[4]))
        {
            *seplen = 3;
            return p + 1;
        }
        if (word_is(p + 1, end, "x", 1) && is_space_byte(p[2]))
        {
            *seplen = 1;
            return p + 1;
        }
    }

    return NULL;
}

/* Where [start, end) should be cut in two, or NULL to keep it whole.
 *
 * Trap: a separator is not a cut on its own. "A & B" is two artists and "Nick
 * Cave & the Bad Seeds" is one, and the string says nothing about which. Only
 * the library does, in two steps:
 *
 *   - a run that is itself an artist is one name, whatever it contains;
 *   - otherwise the cut goes after the longest opening run the library knows,
 *     so a band later in the list survives a guest earlier in it.
 *
 * Failing both, the first separator wins. Most guests have no entry of their
 * own -- that is the point of the feature -- so a credit the library can say
 * nothing at all about is still two people rather than one long name. The
 * price is a guest band nobody in the library has heard of, which comes apart
 * at its ampersand. */
static const char *find_cut(const char *start, const char *end,
                            db_featured_known_fn known, void *ctx,
                            int *seplen)
{
    const char *p = start;
    const char *first = NULL, *longest = NULL;
    int first_len = 0, longest_len = 0;
    const char *ws = start, *we = end;

    trim(&ws, &we);
    if (we > ws && known(ws, (int)(we - ws), ctx))
        return NULL;

    while (p < end)
    {
        const char *ls, *le, *rs, *re;
        const char *sep = next_sep(p, end, seplen);
        int len = *seplen;

        if (sep == NULL)
            break;

        ls = start;
        le = sep;
        trim(&ls, &le);

        rs = sep + len;
        re = end;
        trim(&rs, &re);

        if (le > ls && re > rs)
        {
            if (first == NULL)
            {
                first = sep;
                first_len = len;
            }
            if (known(ls, (int)(le - ls), ctx))
            {
                longest = sep;
                longest_len = len;
            }
        }

        p = sep + len;
    }

    if (longest != NULL)
    {
        *seplen = longest_len;
        return longest;
    }
    if (first != NULL)
    {
        *seplen = first_len;
        return first;
    }

    return NULL;
}

/* ------------------------------------------------------------------ *
 * the parse                                                          *
 * ------------------------------------------------------------------ */

/* Add [s, e) if it is a name worth keeping. Too short and too long are both
 * dropped rather than repaired. */
static void add_name(struct db_featured_names *out, const char *s,
                     const char *e)
{
    trim(&s, &e);

    if (e - s < 2 || e - s > DB_FEATURED_NAME_MAX)
        return;

    out->name[out->count] = s;
    out->len[out->count] = (int)(e - s);
    out->count++;
}

int db_featured_parse(const char *s, db_featured_known_fn known, void *ctx,
                      struct db_featured_names *out)
{
    const char *guests, *end;

    out->count = 0;

    guests = find_marker(s);
    if (guests == NULL)
        return 0;

    end = guest_list_end(s, guests);

    while (out->count < DB_FEATURED_MAX_GUESTS)
    {
        int seplen;
        const char *cut = find_cut(guests, end, known, ctx, &seplen);

        if (cut == NULL)
            break;

        add_name(out, guests, cut);
        guests = cut + seplen;
    }

    if (out->count < DB_FEATURED_MAX_GUESTS)
        add_name(out, guests, end);

    return out->count;
}

bool db_featured_name_eq(const char *a, int alen, const char *b, int blen)
{
    const char *ae = a + alen;
    const char *be = b + blen;

    trim(&a, &ae);
    trim(&b, &be);

    if (ae - a != be - b)
        return false;

    for (int i = 0; i < ae - a; i++)
        if (lower_byte(a[i]) != lower_byte(b[i]))
            return false;

    return true;
}
