/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The order of a podcast's episodes, newest first, from what the files say.
 *
 * A podcatcher fills the date tags patchily -- often the year alone -- and
 * shows put their order in different places: an episode number or a date in
 * the title, or only a date in the file name. The database keeps titles in
 * RAM but not file names, so the scan settles it while it has both: a
 * podcast episode whose title gives no order of its own is stored with the
 * date after it, " (2024-07-30)", from a full release date in the tags or
 * else from the file name. tag_virt_episode then reads every episode's order
 * from its stored title, and lists sort on it. The date is the scan's own
 * marker, not the publisher's, so whatever shows a title cuts it off; the
 * tag on disk is not touched.
 *
 * The description is the file's COMM frame, which the tag reader keeps only
 * the first 500 bytes of, mid-word; the scan reads it again itself and keeps
 * as much as the database holds, cut at a word.
 *
 * Parts, in order:
 *   - reading numbers, dates and month names
 *   - the order a title gives
 *   - the title the scan stores, and the title shown
 *   - an episode's description
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "file.h"
#include "rbunicode.h"
#include "string-extra.h"
#include "database/db_episode.h"

/* ------------------------------------------------------------------ *
 * reading numbers, dates and month names                             *
 * ------------------------------------------------------------------ */

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool letter(char c)
{
    return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
}

/* The number at *s, up to `most` digits, *s moved past it; -1 for none */
static long number(const char **s, int most)
{
    long v = 0;
    int n = 0;

    while (n < most && digit(**s))
    {
        v = v * 10 + (**s - '0');
        (*s)++;
        n++;
    }
    return n ? v : -1;
}

/* The month named at s, by its first three letters with no letter running
 * on past the name it starts; 0 for none. *s moved past the word. */
static int month(const char **s)
{
    static const char names[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    const char *p = *s;

    if (!letter(p[0]) || !letter(p[1]) || !letter(p[2]))
        return 0;
    for (int m = 0; m < 12; m++)
        if (!strncasecmp(p, names + 3 * m, 3))
        {
            p += 3;
            while (letter(*p))
                p++;
            if (*p == '.')
                p++;
            *s = p;
            return m + 1;
        }
    return 0;
}

static long ymd(long y, long m, long d)
{
    if (y < 100)
        y += 2000;
    if (y < 1900 || y > 2099 || m < 1 || m > 12 || d < 0 || d > 31)
        return 0;
    return y * 10000 + m * 100 + d;
}

/* A full date written YYYY-MM-DD (or with / . _ between, or none) at s */
static long iso_date(const char *s)
{
    long y, m, d;
    char sep;

    y = number(&s, 4);
    if (y < 1900)
        return 0;
    sep = *s;
    if (sep == '-' || sep == '/' || sep == '.' || sep == '_')
        s++;
    else
        sep = 0;
    m = number(&s, 2);
    if (sep && *s == sep)
        s++;
    else if (sep)
        return 0;
    d = number(&s, 2);
    if (m < 0 || d < 1 || digit(*s))
        return 0;
    return ymd(y, m, d);
}

/* ------------------------------------------------------------------ *
 * the order a title gives                                            *
 * ------------------------------------------------------------------ */

/* S01E04 or 1x04: season and episode */
static long season_episode(const char *t)
{
    for (const char *p = t; *p; p++)
    {
        const char *s = p;
        long a, b;

        if (p > t && (letter(p[-1]) || digit(p[-1])))
            continue;
        if ((*s | 0x20) == 's')
        {
            s++;
            if ((a = number(&s, 3)) >= 0 && (*s | 0x20) == 'e')
            {
                s++;
                if ((b = number(&s, 4)) >= 0 && !digit(*s))
                    return a * 10000 + b;
            }
        }
        else if ((a = number(&s, 2)) >= 0 && (*s | 0x20) == 'x')
        {
            s++;
            if ((b = number(&s, 4)) >= 0 && !letter(*s) && !digit(*s))
                return a * 10000 + b;
        }
    }
    return 0;
}

/* Episode 12, Ep 12, Ep.12, #12, E12 */
static long episode_number(const char *t)
{
    for (const char *p = t; *p; p++)
    {
        const char *s = p;
        long n;

        if (p > t && (letter(p[-1]) || digit(p[-1])))
            continue;
        if (!strncasecmp(s, "episode", 7))
            s += 7;
        else if (!strncasecmp(s, "ep", 2) && !letter(s[2]))
            s += 2;
        else if (*s == '#')
            s++;
        else if ((*s | 0x20) == 'e' && digit(s[1]))
            s++;
        else
            continue;
        while (*s == ' ' || *s == '.')
            s++;
        if ((n = number(&s, 5)) > 0 && !letter(*s))
            return n;
    }
    return 0;
}

/* 2025-05-26, 26 May 2025, May 26, 2025 */
static long full_date(const char *t)
{
    for (const char *p = t; *p; p++)
    {
        const char *s = p;
        long a, b, v;
        int m;

        if (p > t && (letter(p[-1]) || digit(p[-1])))
            continue;
        if (digit(*s) && (v = iso_date(s)))
            return v;
        if (digit(*s))
        {
            a = number(&s, 2);
            while (*s == ' ')
                s++;
            if ((m = month(&s)))
            {
                while (*s == ' ' || *s == ',')
                    s++;
                if ((b = number(&s, 4)) >= 1900 && (v = ymd(b, m, a)))
                    return v;
            }
        }
        else if ((m = month(&s)))
        {
            while (*s == ' ')
                s++;
            a = number(&s, 2);
            while (*s == ' ' || *s == ',')
                s++;
            if (a > 0 && (b = number(&s, 4)) >= 1900 && (v = ymd(b, m, a)))
                return v;
        }
    }
    return 0;
}

/* Jan 2025, January 2025, 03/26: the month alone, day 0 */
static long month_year(const char *t)
{
    for (const char *p = t; *p; p++)
    {
        const char *s = p;
        long a, b;
        int m;

        if (p > t && (letter(p[-1]) || digit(p[-1])))
            continue;
        if ((m = month(&s)))
        {
            while (*s == ' ')
                s++;
            if ((b = number(&s, 4)) >= 1900 && !digit(*s))
                return ymd(b, m, 0);
        }
        else if ((a = number(&s, 2)) > 0 && *s == '/')
        {
            s++;
            if ((b = number(&s, 2)) >= 0 && !digit(*s) && *s != '/')
                return ymd(b, a, 0);
        }
    }
    return 0;
}

static long title_key(const char *t)
{
    long k;

    if (!t || !*t)
        return 0;
    if ((k = season_episode(t)) || (k = episode_number(t))
        || (k = full_date(t)) || (k = month_year(t)))
        return k;
    return 0;
}

long db_episode_key(const char *title, unsigned long mtime)
{
    long k = title_key(title);
    time_t t = mtime;
    struct tm *tm;

    if (k)
        return k;
    /* gmtime(), not gmtime_r(): the desktop tool's C library lacks the
     * second, and nothing yields before tm is read */
    if (!mtime || !(tm = gmtime(&t)))
        return 0;
    return ymd(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
}

/* ------------------------------------------------------------------ *
 * the title the scan stores, and the title shown                     *
 * ------------------------------------------------------------------ */

/* A full date somewhere in the file's name, as eight digits or with
 * separators: "Seriously-20240730-StealingPower.mp3" */
static long name_date(const char *path)
{
    const char *name = strrchr(path, '/');

    for (const char *p = name ? name + 1 : path; *p; p++)
    {
        long v;

        if ((p == path || !digit(p[-1])) && digit(*p) && (v = iso_date(p)))
            return v;
    }
    return 0;
}

const char *db_episode_title(const char *title, const char *date,
                             const char *path, char *buf, size_t size)
{
    long v;

    if (title_key(title))
        return title;
    v = date ? iso_date(date) : 0;
    if (!v)
        v = name_date(path);
    if (!v)
        return title;
    snprintf(buf, size, "%s (%04ld-%02ld-%02ld)", title ? title : "",
             v / 10000, v / 100 % 100, v % 100);
    return buf;
}

/* ------------------------------------------------------------------ *
 * an episode's description                                           *
 * ------------------------------------------------------------------ */

static unsigned long be32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static unsigned long syncsafe(const unsigned char *p)
{
    return (unsigned long)p[0] << 21 | p[1] << 14 | p[2] << 7 | p[3];
}

/* The text of a COMM frame's body 'raw' (n bytes) as UTF-8 in 'out', past
 * its encoding byte, language and short description. False where it is cut
 * short: 'out' or 'raw' ran out first. */
static bool comm_text(const unsigned char *raw, size_t n, bool raw_cut,
                      char *out, size_t size)
{
    int enc = raw[0];
    bool wide = enc == 1 || enc == 2;
    bool le = false;
    size_t i = 4;
    char *o = out, *end = out + size - 4;

    /* the short description, ended by a 0 of the encoding's width */
    if (wide)
    {
        while (i + 1 < n && (raw[i] || raw[i + 1]))
            i += 2;
        i += 2;
    }
    else
    {
        while (i < n && raw[i])
            i++;
        i++;
    }
    if (wide && i + 1 < n && ((raw[i] == 0xff && raw[i + 1] == 0xfe)
                              || (raw[i] == 0xfe && raw[i + 1] == 0xff)))
    {
        le = raw[i] == 0xff;
        i += 2;
    }
    else if (enc == 1)
        le = true;

    while (i < n && o < end)
    {
        unsigned long c;

        if (wide)
        {
            if (i + 1 >= n)
                break;
            c = le ? raw[i] | raw[i + 1] << 8 : raw[i] << 8 | raw[i + 1];
            i += 2;
            /* a character past U+FFFF comes as a pair, one character */
            if (c >= 0xd800 && c < 0xdc00 && i + 1 < n)
            {
                unsigned long lo = le ? raw[i] | raw[i + 1] << 8
                                      : raw[i] << 8 | raw[i + 1];

                if (lo >= 0xdc00 && lo < 0xe000)
                {
                    c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                    i += 2;
                }
            }
            if (c >= 0xd800 && c < 0xe000)
                c = 0xfffd;
        }
        else if (enc == 3)
        {
            *o++ = raw[i++];
            continue;
        }
        else
            c = raw[i++];
        if (c == 0)
            break;
        o = (char *)utf8encode(c, (unsigned char *)o);
    }
    *o = '\0';
    return !raw_cut && (i >= n || o < end);
}

/* Whether a COMM frame's short description starts "iTun": iTunNORM,
 * iTunSMPB and the rest hold numbers, not notes */
static bool comm_is_itunes(const unsigned char *raw, size_t n)
{
    static const char want[] = "iTun";
    size_t i = 4, k;

    if (raw[0] == 1 || raw[0] == 2)
    {
        if (i + 1 < n && ((raw[i] == 0xff && raw[i + 1] == 0xfe)
                          || (raw[i] == 0xfe && raw[i + 1] == 0xff)))
            i += 2;
        for (k = 0; k < 4; k++, i += 2)
            if (i + 1 >= n || (raw[i] | raw[i + 1]) != want[k]
                || (raw[i] && raw[i + 1]))
                return false;
        return true;
    }
    return n >= 8 && !memcmp(raw + 4, want, 4);
}

/* "... the last whole word…", within 'size' */
static void cut_at_word(char *s, size_t size)
{
    size_t len = strlen(s);
    size_t lead;
    char *sp;

    if (len + 4 > size)
        len = size - 4;
    /* a character cut short at the end goes whole */
    lead = len;
    while (lead > 0 && ((unsigned char)s[lead - 1] & 0xc0) == 0x80)
        lead--;
    if (lead > 0 && (unsigned char)s[lead - 1] >= 0xc0)
    {
        unsigned char c = s[lead - 1];
        size_t whole = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;

        if (lead - 1 + whole > len)
            len = lead - 1;
    }
    s[len] = '\0';
    sp = strrchr(s, ' ');
    if (sp && sp > s + len / 2)
        *sp = '\0';
    strcat(s, "\xe2\x80\xa6");
}

bool db_episode_comment(const char *path, char *buf, size_t size)
{
    static unsigned char raw[1024];
    unsigned char h[10];
    unsigned long end, pos = 10, n;
    bool found = false;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return false;
    /* ID3v2.3 or 2.4, without whole-tag unsynchronisation or an extended
     * header, which podcatchers do not write */
    if (read(fd, h, 10) != 10 || memcmp(h, "ID3", 3) || h[3] < 3 || h[3] > 4
        || (h[5] & 0xc0))
    {
        close(fd);
        return false;
    }
    end = 10 + syncsafe(h + 6);
    while (!found && pos + 10 <= end)
    {
        unsigned char f[10];

        if (lseek(fd, pos, SEEK_SET) < 0 || read(fd, f, 10) != 10 || !f[0])
            break;
        n = h[3] == 4 ? syncsafe(f + 4) : be32(f + 4);
        /* a frame running past the tag would carry pos round to the start */
        if (n > end - pos - 10)
            break;
        if (!memcmp(f, "COMM", 4) && !f[9] && n > 4)
        {
            size_t got = n < sizeof(raw) ? n : sizeof(raw);

            if ((size_t)read(fd, raw, got) != got)
                break;
            /* iTunes' own numbers, which its tag readers skip too */
            if (!comm_is_itunes(raw, got))
            {
                if (!comm_text(raw, got, got < n, buf, size))
                    cut_at_word(buf, size);
                found = buf[0] != '\0';
            }
        }
        pos += 10 + n;
    }
    close(fd);
    return found;
}

int db_episode_shown_len(const char *title, int len)
{
    const char *s = title + len - 13;

    /* " (YYYY-MM-DD)", the marker exactly */
    if (len >= 13 && s[0] == ' ' && s[1] == '(' && s[12] == ')'
        && s[6] == '-' && s[9] == '-'
        && digit(s[2]) && digit(s[3]) && digit(s[4]) && digit(s[5])
        && digit(s[7]) && digit(s[8]) && digit(s[10]) && digit(s[11]))
        return len - 13;
    return len;
}
