/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Trimming the notes off an album or track name.
 *
 * Tags carry two things in one string: the name of the record, and what
 * pressing was ripped -- "Bad Blood (Taylor's Version)", "Big Tears - 2021
 * Remaster", "exile (feat. Bon Iver)". On a 320-pixel screen the second half
 * costs the first half its room, so this removes it before the skin sees it.
 *
 * Only the presented string changes. The database keeps the tag it was given,
 * which is what lets the setting be switched off with nothing to undo.
 *
 * What counts as a note is not decided here. /.rockbox/trim.config holds one
 * pattern per line and the file is the whole list, so a library this fork's
 * defaults read wrongly is fixed by editing a text file rather than by a new
 * build.
 *
 * Parts, in order:
 *   - the pattern list, and reading the file
 *   - matching one group against the list
 *   - finding the group to cut
 *   - tag_trim()
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "rbpaths.h"
#include "file.h"
#include "settings/settings.h"
#include "system/strutil.h"          /* read_line, BOM_UTF_8 */
#include "tag_trim.h"

#define TRIM_FILE      ROCKBOX_DIR "/trim.config"
#define TRIM_PATTERNS  64      /* patterns kept; a longer file is read and the
                                  rest ignored */
#define TRIM_TEXT      1024    /* bytes of pattern text kept */
#define TRIM_LINE      128

/* ------------------------------------------------------------------ *
 * the pattern list                                                   *
 * ------------------------------------------------------------------ */

static char pattern_text[TRIM_TEXT];
static const char *patterns[TRIM_PATTERNS];
static int pattern_count;

/* Fold one byte, ASCII only. Tags are UTF-8 and every byte of an accented
 * letter is >= 0x80, so leaving those alone is what keeps a multi-byte
 * character from being altered a byte at a time. */
static char fold(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* Reading the file blocks on the disk, which yields, so a skin can render
 * part-way through this. `n` is only published to pattern_count once the list
 * is whole: a reader that arrives mid-load sees the old count or none, never
 * a pattern pointing into text being rewritten under it. */
void tag_trim_init(void)
{
    char line[TRIM_LINE];
    bool first = true;
    int used = 0;
    int n = 0;
    int fd;

    pattern_count = 0;

    fd = open(TRIM_FILE, O_RDONLY);
    if (fd < 0)
        return;

    while (n < TRIM_PATTERNS && read_line(fd, line, sizeof(line)) > 0)
    {
        char *p = line;
        int len, i;

        /* A byte-order mark, which a Windows editor adds without saying so.
         * Left in place it belongs to the first pattern and nothing it names
         * is ever trimmed. */
        if (first)
        {
            first = false;
            if (strncmp(p, BOM_UTF_8, BOM_UTF_8_SIZE) == 0)
                p += BOM_UTF_8_SIZE;
        }

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\0')
            continue;

        len = strlen(p);
        while (len > 0 && (p[len-1] == ' ' || p[len-1] == '\t'))
            len--;
        if (len == 0 || used + len + 1 > TRIM_TEXT)
            continue;

        /* Folded once here rather than at every comparison; only the subject
         * still has to be folded as it is read. */
        patterns[n++] = &pattern_text[used];
        for (i = 0; i < len; i++)
            pattern_text[used++] = fold(p[i]);
        pattern_text[used++] = '\0';
    }

    close(fd);
    pattern_count = n;
}

/* ------------------------------------------------------------------ *
 * matching                                                           *
 * ------------------------------------------------------------------ */

/* Wildcard match with a single backtrack point: '*' stands for any run of
 * bytes, including none. pat is already folded; s is folded as it is read,
 * so the subject never has to be copied. */
static bool pattern_match(const char *pat, const char *s, int len)
{
    int p = 0, i = 0, star = -1, mark = 0;

    while (i < len)
    {
        if (pat[p] != '\0' && pat[p] != '*' && pat[p] == fold(s[i]))
        {
            p++;
            i++;
        }
        else if (pat[p] == '*')
        {
            star = p++;
            mark = i;
        }
        else if (star >= 0)
        {
            /* The last '*' takes one byte more and the tail is retried. */
            p = star + 1;
            i = ++mark;
        }
        else
            return false;
    }

    while (pat[p] == '*')
        p++;
    return pat[p] == '\0';
}

/* Whether the text of one group is something the file says to drop. Spaces
 * either side are ignored, so "( feat. X )" reads as "feat. X". */
static bool is_note(const char *s, int len)
{
    int i;

    while (len > 0 && *s == ' ')
    {
        s++;
        len--;
    }
    while (len > 0 && s[len-1] == ' ')
        len--;
    if (len <= 0)
        return false;

    for (i = 0; i < pattern_count; i++)
        if (pattern_match(patterns[i], s, len))
            return true;
    return false;
}

/* ------------------------------------------------------------------ *
 * finding the group                                                  *
 * ------------------------------------------------------------------ */

static const char openers[] = "([{";
static const char closers[] = ")]}";

/* Which bracket 'c' closes, as an index into openers[], or -1. */
static int closer_index(char c)
{
    const char *found = (c == '\0') ? NULL : strchr(closers, c);
    return found ? (int)(found - closers) : -1;
}

static bool is_opener(char c)
{
    return c != '\0' && strchr(openers, c) != NULL;
}

/* How much of name survives. Each pass removes at most one group and the next
 * looks again, so "Song (Album Version) (Explicit)" loses both.
 *
 * A group at the very start is never a candidate -- "(I Can't Get No)
 * Satisfaction" is the name of the song, not a note about it -- which is also
 * what stops a name that is nothing but a group from trimming away to
 * nothing. */
static int trimmed_length(const char *name, int len)
{
    bool cut = true;

    while (cut)
    {
        cut = false;

        /* Trailing spaces, and a dash stranded by a group that followed one.
         * The space in front of the dash is what makes this safe: a name
         * ending in a hyphen keeps it. */
        for (;;)
        {
            if (len > 0 && name[len-1] == ' ')
                len--;
            else if (len > 1 && name[len-1] == '-' && name[len-2] == ' ')
                len -= 2;
            else
                break;
        }
        if (len == 0)
            break;

        /* A closed group ending the name: "Bad Blood (Taylor's Version)" */
        int ci = closer_index(name[len-1]);
        if (ci >= 0)
        {
            int o = len - 2;

            while (o >= 0 && name[o] != openers[ci])
                o--;
            if (o > 0 && is_note(&name[o+1], len - o - 2))
            {
                len = o;
                cut = true;
                continue;
            }
        }

        /* A group never closed: "Album [LATEST". Only when a space precedes
         * the bracket, and only when nothing closes it -- a closer further on
         * means the group ended and the case above owns it. */
        int o = len - 1;
        while (o >= 0 && !is_opener(name[o]) && closer_index(name[o]) < 0)
            o--;
        if (o > 0 && is_opener(name[o]) && name[o-1] == ' ' &&
            is_note(&name[o+1], len - o - 1))
        {
            len = o;
            cut = true;
            continue;
        }

        /* A dash-separated tail: "Big Tears - 2021 Remaster". The last such
         * separator, so "Warm-Up - Remastered 1998" keeps its hyphen. */
        int d = len - 3;
        while (d > 0 &&
               !(name[d] == ' ' && name[d+1] == '-' && name[d+2] == ' '))
            d--;
        if (d > 0 && is_note(&name[d+3], len - d - 3))
        {
            len = d;
            cut = true;
        }
    }

    return len;
}

/* ------------------------------------------------------------------ *
 * the interface                                                      *
 * ------------------------------------------------------------------ */

const char *tag_trim(const char *name, char *buf, int buf_size)
{
    int len, trimmed;

    if (!global_settings.trim_titles || pattern_count == 0 || !name || !buf)
        return name;

    len = strlen(name);
    trimmed = trimmed_length(name, len);

    /* Nothing removed is the ordinary answer, and it comes back as the
     * caller's own pointer so that a name too long for buf is passed on whole
     * rather than cut short. */
    if (trimmed == len || trimmed <= 0 || trimmed >= buf_size)
        return name;

    memcpy(buf, name, trimmed);
    buf[trimmed] = '\0';
    return buf;
}
