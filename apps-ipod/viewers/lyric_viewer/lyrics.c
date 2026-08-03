/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/lrcplayer.c
 * Synchronised lyrics: finding, loading and the document model.
 *
 * Copyright (C) 2008-2009 Teruaki Kawashima
 * GNU General Public License (version 2+)
 *
 * Reads timed lyrics for a track and holds them as a list of lines, each with
 * a start time and optionally per-word times. No drawing and no input: a
 * viewer asks what line N says and which line belongs at time T.
 *
 * Sources, in the order they are tried: a .lrc, .lrc8 or .snc file beside the
 * audio file or in any parent directory, then an ID3v2 SYLT frame inside the
 * file itself. Unsynchronised sources (plain .txt, ID3 USLT) are not read --
 * without timestamps there is nothing to synchronise to.
 *
 * Memory: one immovable buffer holds everything. Line and word structs are
 * allocated downwards from its top, line text upwards from its bottom, and
 * they meet in the middle; when they meet, loading simply stops and the
 * lyrics are short. Buffer pointers are handed out to callers, so the block
 * must not move -- hence buflib_ops_locked.
 *
 * UTF-16 lyrics files are skipped. read_line() splits on single bytes and
 * desynchronises on the first line of a UTF-16 file; upstream worked around
 * this by rewriting the user's file as UTF-8, which a viewer should not do.
 *
 * Parts, in order:
 *   - the buffer and its two-ended allocator
 *   - the document model: lines, words, sorting
 *   - finding a lyrics file
 *   - parsing .lrc and .snc
 *   - reading an ID3v2 SYLT frame
 *   - loading, and the public accessors
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"
#include "system.h"          /* ARRAYLEN */
#include "string-extra.h"    /* strlcpy */
#include "file.h"            /* MAX_PATH, open/read/close/lseek, file_exists */
#include "rbunicode.h"       /* iso_decode_ex, utf16*decode, UTF_8 */
#include "core_alloc.h"      /* core_alloc_ex, buflib_ops_locked */
#include "metadata.h"        /* struct mp3entry, AFMT_*, ID3_VER_* */
#include "settings/settings.h"       /* global_settings */
#include "system/strutil.h"          /* read_line, fix_path_part */
#include "lyrics.h"

/* Longest line read from a lyrics file, in bytes before decoding. */
#define MAX_LINE_LEN    256

/* Big enough for a long song with per-word timing; loading stops cleanly if
 * a file needs more. */
#define LYRICS_BUFFER_SIZE  0x8000      /* 32 kiB */

/* A SYLT frame is read into the middle of the buffer and consumed as it is
 * parsed, so text growing up and structs growing down never reach it. A frame
 * longer than SYLT_MAX loses its tail. */
#define SYLT_TOP    (LYRICS_BUFFER_SIZE * 2 / 3)
#define SYLT_MAX    (LYRICS_BUFFER_SIZE / 3)

enum lyrics_type
{
    TYPE_NONE = 0,
    TYPE_LRC,       /* .lrc, in the user's codepage unless it has a BOM */
    TYPE_LRC8,      /* .lrc8, always UTF-8 */
    TYPE_SNC,
    TYPE_SYLT,      /* ID3v2 synchronised lyrics frame */
};

struct lyrics_word
{
    long time;                  /* ms, or -1 to start with the line */
    unsigned char *text;
};

struct lyrics_line
{
    long time;                  /* ms, before [offset:] is applied */
    short nword;
    struct lyrics_line *next;
    /* Words are stored last-first, because they are allocated downwards:
     * words[nword-1] is the first word, and its text -- the whole line. */
    struct lyrics_word *words;
};

/* What one entry of a SYLT frame takes from the top of the buffer. */
#define SYLT_SLACK  (sizeof(struct lyrics_line) \
                     + sizeof(struct lyrics_word) + 8)

static int lyrics_handle = 0;
static unsigned char *buffer;
static size_t buf_used;         /* text, growing up from 0 */
static size_t buf_end;          /* structs, growing down from the top */

/* A copy of the track being searched for, taken before anything else happens.
 *
 * audio_current_track() hands back a pointer to the playback engine's live
 * mp3entry, not a copy of one -- and allocating the working buffer below can
 * compact the buflib arena and run playback's shrink callback, which is
 * enough for that entry to be refilled underneath us. Reading id3->path
 * afterwards then finds an empty string, the search builds no candidate at
 * all, and the result is indistinguishable from "this track has no lyrics".
 * Nothing past the copy touches the caller's entry. */
static struct
{
    char path[MAX_PATH];
    char title[MAX_PATH];
    char artist[MAX_PATH];
    unsigned long length;
    unsigned long id3v2len;
    unsigned int codectype;
    unsigned char id3version;
} track;

static struct
{
    char file[MAX_PATH];
    char *title;
    char *artist;
    long offset;                /* ms, from the [offset:] tag */
    long length;                /* track length, ms */
    int nline;
    struct lyrics_line *head, **tail;
    struct lyrics_line **index; /* nline entries, in time order */
    enum lyrics_type type;
    bool loaded;
} lrc;

/* Scratch for one line. parse_snc_line() needs the raw bytes as well as the
 * decoded ones, so both stay around for the length of a load. */
static unsigned char raw_line[MAX_LINE_LEN];
static unsigned char utf8_line[MAX_LINE_LEN * 3 + 2];
static int line_encoding;

/* ---------------------------------------------------------------------------
 * The buffer and its two-ended allocator
 * ------------------------------------------------------------------------ */

/* Space left between the two ends, less the room the line index will need
 * once loading finishes. Reserving it as we go means build_index() cannot
 * fail after the lines it has to describe are already parsed. */
static size_t buf_free(void)
{
    size_t reserve = (lrc.nline + 1) * sizeof(struct lyrics_line *);

    if (buf_used + reserve >= buf_end)
        return 0;
    return buf_end - buf_used - reserve;
}

/* Append a string to the text area. With join, it overwrites the previous
 * string's terminator, so the two become one -- that is how the words of a
 * line end up contiguous. Returns where the string landed, NULL if full. */
static unsigned char *bufadd(const unsigned char *str, bool join)
{
    size_t siz = strlen((const char *)str) + 1;
    unsigned char *pos;

    if (join)
        buf_used--;
    if (siz > buf_free())
        return NULL;
    pos = &buffer[buf_used];
    strcpy((char *)pos, (const char *)str);
    buf_used += siz;
    return pos;
}

static void *alloc_buf(size_t siz)
{
    siz = (siz + 3) & ~3;
    if (siz > buf_free())
        return NULL;
    buf_end -= siz;
    return &buffer[buf_end];
}

static void reset_data(void)
{
    memset(&lrc, 0, sizeof(lrc));
    lrc.tail = &lrc.head;
    buf_used = 0;
    buf_end = LYRICS_BUFFER_SIZE;
}

/* ---------------------------------------------------------------------------
 * The document model
 * ------------------------------------------------------------------------ */

static struct lyrics_word *new_word(long time, const unsigned char *text,
                                    bool join)
{
    struct lyrics_word *word = alloc_buf(sizeof(struct lyrics_word));

    if (word == NULL)
        return NULL;
    if ((word->text = bufadd(text, join)) == NULL)
        return NULL;
    word->time = time;
    return word;
}

/* Append a line, optionally starting its text. */
static bool add_line(struct lyrics_line *line, const unsigned char *text)
{
    line->nword = 0;
    line->next = NULL;
    line->words = NULL;
    if (text)
    {
        if ((line->words = new_word(-1, text, false)) == NULL)
            return false;
        line->nword++;
    }
    *lrc.tail = line;
    lrc.tail = &line->next;
    lrc.nline++;
    return true;
}

/* Put the lines in time order, keeping equal times in file order, and drop
 * any that never got text -- a line with no words would break every
 * accessor. Recounts nline, which is why it must run before build_index(). */
static void sort_lines(void)
{
    struct lyrics_line *p = lrc.head, **q, *next;
    long time_max = 0;

    lrc.head = NULL;
    lrc.tail = &lrc.head;
    lrc.nline = 0;

    while (p != NULL)
    {
        next = p->next;
        if (p->nword)
        {
            /* already the latest? then only the tail needs looking at */
            q = (p->time >= time_max)? lrc.tail: &lrc.head;
            while ((*q) && (*q)->time <= p->time)
                q = &((*q)->next);
            p->next = *q;
            *q = p;
            if (!p->next)
            {
                time_max = p->time;
                lrc.tail = &p->next;
            }
            lrc.nline++;
        }
        p = next;
    }
}

/* Turn the list into an array so lines can be reached by index. Space for it
 * was reserved by buf_free() as the lines were parsed. */
static bool build_index(void)
{
    size_t siz = (lrc.nline * sizeof(struct lyrics_line *) + 3) & ~3;
    struct lyrics_line *line;
    int i = 0;

    if (buf_used + siz > buf_end)
        return false;
    buf_end -= siz;
    lrc.index = (struct lyrics_line **)&buffer[buf_end];
    for (line = lrc.head; line; line = line->next)
        lrc.index[i++] = line;
    return true;
}

static struct lyrics_line *get_line(int index)
{
    if (!lrc.index || index < 0 || index >= lrc.nline)
        return NULL;
    return lrc.index[index];
}

/* [offset:] shifts every timestamp, and is applied on the way out rather than
 * stored: lines written [t1][t2]text share one word list, so baking it in
 * would shift those words twice. */
static long apply_offset(long time)
{
    time += lrc.offset;
    return (time < 0)? 0: time;
}

/* ---------------------------------------------------------------------------
 * Finding a lyrics file
 *
 * For /aaa/bbb/ccc/ddd.mp3 this looks for ddd.lrc (then .lrc8, then .snc) in
 * /aaa/bbb/ccc/, then /aaa/bbb/, then /aaa/, then /. The track's title is
 * tried as a file name too, when it differs from the audio file's.
 * ------------------------------------------------------------------------ */

static const struct
{
    const char *ext;
    enum lyrics_type type;
} lyrics_ext[] = {
    { ".lrc",  TYPE_LRC  },
    { ".lrc8", TYPE_LRC8 },
    { ".snc",  TYPE_SNC  },
};

static char path_buf[MAX_PATH];

static bool find_lyrics_file(void)
{
    char fname[MAX_PATH];
    char *names[3] = {NULL, NULL, NULL};
    char *p, *dir;
    unsigned int t;
    int i, len;

    strlcpy(path_buf, track.path, sizeof(path_buf));

    /* All this needs is a directory to search: test for the separator it is
     * about to walk back from, rather than insisting the path starts at the
     * root. A volume-qualified path ("/<HDD0>/Music/...") is normal here, and
     * so is anything else the playlist hands over -- the only thing that
     * cannot work is a bare filename with no directory at all. */
    p = strrchr(path_buf, '/');
    if (p == NULL)
        return false;
    names[0] = p + 1;
    if ((p = strrchr(names[0], '.')) != NULL)
        *p = 0;
    if (track.title[0] && strcmp(names[0], track.title))
    {
        strlcpy(fname, track.title, sizeof(fname));
        fix_path_part(fname, 0, sizeof(fname) - 1);
        names[1] = fname;
    }

    /* walk up the directory chain by truncating path_buf a level at a time.
     * There is no dir_exists() gate: file_exists() on a path under a missing
     * directory is false anyway, so the gate only ever saved a few probes --
     * while being one more thing that has to agree about volume prefixes and
     * trailing separators. albumart.c searches beside a track the same way. */
    dir = path_buf;
    p = names[0] - 1;
    do {
        *p = 0;
        len = snprintf(lrc.file, MAX_PATH, "%s/", dir);
        if (len >= MAX_PATH)
            continue;
        for (t = 0; t < ARRAYLEN(lyrics_ext); t++)
        {
            for (i = 0; names[i] != NULL; i++)
            {
                snprintf(&lrc.file[len], MAX_PATH - len, "%s%s",
                         names[i], lyrics_ext[t].ext);
                if (file_exists(lrc.file))
                {
                    lrc.type = lyrics_ext[t].type;
                    return true;
                }
            }
        }
    } while ((p = strrchr(dir, '/')) != NULL);

    lrc.file[0] = 0;
    return false;
}

/* ---------------------------------------------------------------------------
 * Parsing .lrc and .snc
 * ------------------------------------------------------------------------ */

static char *parse_int(char *ptr, int *val)
{
    *val = atoi(ptr);
    while (isdigit((unsigned char)*ptr))
        ptr++;
    return ptr;
}

/* Read the contents of one [...] or <...> tag. Returns its time in ms, -1 for
 * an id tag that was consumed, or -10 for anything unrecognised.
 * Time tags are [mm:ss], [mm:ss.xx] or [mm:ss.xxx]; id tags are only looked
 * for at the head of a line, which is where the format puts them. */
static long get_time_value(char *tag, bool read_id_tags)
{
    long time;
    char *ptr;
    int val;

    if (read_id_tags)
    {
        /* keep title and artist only when they say something the track's own
         * metadata does not */
        if (!strncmp(tag, "ti:", 3))
        {
            if (!track.title[0] || strcmp(&tag[3], track.title))
                lrc.title = (char *)bufadd((unsigned char *)&tag[3], false);
            return -1;
        }
        if (!strncmp(tag, "ar:", 3))
        {
            if (!track.artist[0] || strcmp(&tag[3], track.artist))
                lrc.artist = (char *)bufadd((unsigned char *)&tag[3], false);
            return -1;
        }
        if (!strncmp(tag, "offset:", 7))
        {
            lrc.offset = atoi(&tag[7]);
            return -1;
        }
    }

    /* minute */
    ptr = parse_int(tag, &val);
    if (ptr - tag < 1 || ptr - tag > 2 || *ptr != ':')
        return -10;
    time = val * 60000;
    /* second */
    tag = ptr + 1;
    ptr = parse_int(tag, &val);
    if (ptr - tag != 2 || (*ptr != '.' && *ptr != ':' && *ptr != '\0'))
        return -10;
    time += val * 1000;

    if (*ptr != '\0')
    {
        /* fraction: two digits are hundredths, three are milliseconds */
        tag = ptr + 1;
        ptr = parse_int(tag, &val);
        if (ptr - tag < 2 || ptr - tag > 3 || *ptr != '\0')
            return -10;
        time += ((ptr - tag) == 3)? val: val * 10;
    }

    return time;
}

/* One line of a .lrc file:
 *   [time]text
 *   [time]...[time]text          the same text at several times
 *   [time]<time>word<time>word   per-word timing
 */
static bool parse_lrc_line(char *line)
{
    struct lyrics_line *lyr = NULL, *first = NULL;
    struct lyrics_word *word;
    long time, word_time;
    char *str, *tagstart, *tagend;
    int nword = 0;

    /* the [time] tags at the head of the line */
    str = line;
    while (1)
    {
        if (*str != '[')
            break;
        tagend = strchr(str, ']');
        if (tagend == NULL)
            break;
        *tagend = 0;
        time = get_time_value(str + 1, !lyr);
        *tagend++ = ']';
        if (time < 0)
            break;
        lyr = alloc_buf(sizeof(struct lyrics_line));
        if (lyr == NULL)
            return false;
        if (!first)
            first = lyr;
        lyr->time = (time / 10) * 10;
        if (!add_line(lyr, NULL))
            return false;
        str = tagend;
    }
    if (!first)
        return true;    /* an id tag line, a comment, or junk */

    /* Start the line's text as an empty string; each word joins onto it, so
     * the words end up as one run and the first word's text is the line. */
    if (bufadd((const unsigned char *)"", false) == NULL)
        return false;

    /* the <time> tags within the line */
    word_time = -1;
    tagstart = str;
    while (*tagstart)
    {
        tagstart = strchr(tagstart, '<');
        if (!tagstart)
            break;
        tagend = strchr(tagstart, '>');
        if (!tagend)
            break;
        *tagend = 0;
        time = get_time_value(tagstart + 1, false);
        *tagend++ = '>';
        if (time < 0)
        {
            tagstart++;     /* not a time tag; leave it in the text */
            continue;
        }
        *tagstart = 0;
        if (*str || word_time != -1)
        {
            if (new_word(word_time, (unsigned char *)str, true) == NULL)
                return false;
            nword++;
        }
        tagstart = str = tagend;
        word_time = time;
    }
    if ((word = new_word(word_time, (unsigned char *)str, true)) == NULL)
        return false;
    nword++;

    /* [t1][t2]text made several lines that all read the same: point them at
     * the one word list. Word times then belong to the first of them. */
    for (lyr = first; lyr; lyr = lyr->next)
    {
        lyr->nword = nword;
        lyr->words = word;
    }
    return true;
}

/* One line of a .snc file. A time tag sits on a line of its own (possibly
 * with text after it) and the lines that follow belong to it:
 *   \xa2\xe2hhmmssxx\xa2\xd0
 *   line 1
 *   line 2
 */
#define SNC_TAG_START "\xa2\xe2"
#define SNC_TAG_END   "\xa2\xd0"

static bool parse_snc_line(char *line)
{
    /* The tag is raw bytes that iso_decode_ex() may have rewritten, so it is
     * matched against raw_line rather than the decoded copy. */
    if (strlen((char *)raw_line) >= 12
        && !memcmp(raw_line, SNC_TAG_START, 2)
        && !memcmp(raw_line + 10, SNC_TAG_END, 2))
    {
        const unsigned char *pos = raw_line + 2;
        struct lyrics_line *lyr;
        unsigned char *end;
        int hh, mm, ss, xx;

        hh = (pos[0]-'0')*10 + (pos[1]-'0'); pos += 2;
        mm = (pos[0]-'0')*10 + (pos[1]-'0'); pos += 2;
        ss = (pos[0]-'0')*10 + (pos[1]-'0'); pos += 2;
        xx = (pos[0]-'0')*10 + (pos[1]-'0'); pos += 2;
        pos += 2;   /* SNC_TAG_END */

        lyr = alloc_buf(sizeof(struct lyrics_line));
        if (lyr == NULL)
            return false;
        lyr->time = hh*3600000 + mm*60000 + ss*1000 + xx*10;
        if (!add_line(lyr, (const unsigned char *)""))
            return false;
        if (pos[0] == 0)
            return true;

        /* text after the tag is the first line of its lyric */
        end = iso_decode_ex(pos, (unsigned char *)line, line_encoding,
                            strlen((const char *)pos) + 1,
                            sizeof(utf8_line) - 2);
        *end = 0;
    }
    if (lrc.head)
    {
        strcat(line, "\n");
        if (bufadd((unsigned char *)line, true) == NULL)
            return false;
    }
    return true;
}

static void load_lyrics_file(void)
{
    bool (*parse_line)(char *line);
    unsigned char header[3];
    unsigned char *end;
    int fd, got;

    line_encoding = global_settings.default_codepage;
    switch (lrc.type)
    {
        case TYPE_LRC8:
            line_encoding = UTF_8;
            /* fall through */
        case TYPE_LRC:
            parse_line = parse_lrc_line;
            break;
        case TYPE_SNC:
            parse_line = parse_snc_line;
            break;
        default:
            return;
    }

    fd = open(lrc.file, O_RDONLY);
    if (fd < 0)
        return;

    got = read(fd, header, sizeof(header));
    if (got == (int)sizeof(header) && !memcmp(header, "\xef\xbb\xbf", 3))
        line_encoding = UTF_8;
    else if (got >= 2 && (!memcmp(header, "\xff\xfe", 2)
                          || !memcmp(header, "\xfe\xff", 2)))
    {
        close(fd);      /* UTF-16 -- see the note at the top of the file */
        return;
    }
    else
        lseek(fd, 0, SEEK_SET);

    while (read_line(fd, (char *)raw_line, MAX_LINE_LEN) > 0)
    {
        end = iso_decode_ex(raw_line, utf8_line, line_encoding,
                            strlen((const char *)raw_line) + 1,
                            sizeof(utf8_line) - 2);
        *end = 0;
        if (!parse_line((char *)utf8_line))
            break;
    }
    close(fd);
}

/* ---------------------------------------------------------------------------
 * Reading an ID3v2 SYLT frame
 * ------------------------------------------------------------------------ */

static unsigned long unsync(unsigned long b0, unsigned long b1,
                            unsigned long b2, unsigned long b3)
{
    return (((long)(b0 & 0x7F) << (3*7)) |
            ((long)(b1 & 0x7F) << (2*7)) |
            ((long)(b2 & 0x7F) << (1*7)) |
            ((long)(b3 & 0x7F) << (0*7)));
}

static unsigned long bytes2int(unsigned long b0, unsigned long b1,
                               unsigned long b2, unsigned long b3)
{
    return (((long)(b0 & 0xFF) << (3*8)) |
            ((long)(b1 & 0xFF) << (2*8)) |
            ((long)(b2 & 0xFF) << (1*8)) |
            ((long)(b3 & 0xFF) << (0*8)));
}

/* Undo the 0xFF 0x00 escaping ID3 uses to keep tag data from looking like an
 * MPEG sync word. Rewrites in place and returns the shorter length. */
static int unsynchronize(unsigned char *tag, int len, bool *ff_found)
{
    unsigned char *rp = tag, *wp = tag;
    bool _ff_found = ff_found? *ff_found: false;
    int i;

    for (i = 0; i < len; i++)
    {
        unsigned char c = *rp++;
        *wp = c;
        if (_ff_found)
        {
            /* keep the byte only if it is not the inserted 0x00 */
            if (c != 0)
                wp++;
            _ff_found = false;
        }
        else
        {
            if (c == 0xff)
                _ff_found = true;
            wp++;
        }
    }
    if (ff_found)
        *ff_found = _ff_found;
    return wp - tag;
}

/* Read len bytes of unescaped data, reading more from the file as escaping
 * eats into what was read. */
static int read_unsynched(int fd, void *buf, int len, bool *ff_found)
{
    int remaining = len;
    unsigned char *wp = buf;

    while (remaining)
    {
        int rc = read(fd, wp, remaining);
        if (rc <= 0)
            return rc;

        rc = unsynchronize(wp, rc, ff_found);
        remaining -= rc;
        wp += rc;
    }
    return len;
}

/* Skip an escaped frame. It has to be read to be skipped, because its length
 * on disk is not its length in the tag -- but a frame can be far bigger than
 * our buffer (embedded cover art), so discard it a chunk at a time. */
static bool skip_unsynched(int fd, long len, bool *ff_found)
{
    unsigned char scrap[64];

    while (len > 0)
    {
        int n = (len < (long)sizeof(scrap))? (int)len: (int)sizeof(scrap);
        if (read_unsynched(fd, scrap, n, ff_found) != n)
            return false;
        len -= n;
    }
    return true;
}

static unsigned char *utf8cpy(const unsigned char *src,
                              unsigned char *dst, int count)
{
    strlcpy((char *)dst, (const char *)src, count + 1);
    return dst + strlen((char *)dst);
}

/* Walk the ID3v2 tag looking for SYLT, and turn what it holds into lines.
 * The unsynchronised (USLT) frame is deliberately ignored. */
static void parse_sylt(int fd)
{
    unsigned char header[10], tmp[8];
    unsigned char *tag, *p;
    unsigned char *(*utf_decode)(const unsigned char *,
                                 unsigned char *, int) = NULL;
    unsigned char version, global_flags;
    bool global_unsynch = false, global_ff_found = false, unsynch = false;
    bool found = false;
    long framelen = 0;
    int minframesize, size, flags, rc, bytesread, encoding, chsiz;

    if (track.id3v2len < 10)
        return;
    if (read(fd, header, 10) != 10)
        return;

    size = track.id3v2len - 10;
    version = track.id3version;
    switch (version)
    {
        case ID3_VER_2_2:
            minframesize = 8;
            break;
        case ID3_VER_2_3:
        case ID3_VER_2_4:
            minframesize = 12;
            break;
        default:
            return;
    }

    global_flags = header[5];

    /* skip the extended header if there is one */
    if (global_flags & 0x40)
    {
        if (version == ID3_VER_2_3)
        {
            if (read(fd, header, 10) != 10)
                return;
            /* 2.3's size excludes the size field itself, and is not escaped */
            framelen = 4 + bytes2int(header[0], header[1],
                                     header[2], header[3]);
            lseek(fd, framelen - 10, SEEK_CUR);
        }
        if (version >= ID3_VER_2_4)
        {
            if (read(fd, header, 4) != 4)
                return;
            /* 2.4's size covers the whole header, and is escaped */
            framelen = unsync(header[0], header[1], header[2], header[3]);
            lseek(fd, framelen - 4, SEEK_CUR);
        }
    }

    if (global_flags & 0x80)
        global_unsynch = true;

    while (size >= minframesize)
    {
        flags = 0;

        if (version >= ID3_VER_2_3)
        {
            if (global_unsynch && version <= ID3_VER_2_3)
                rc = read_unsynched(fd, header, 10, &global_ff_found);
            else
                rc = read(fd, header, 10);
            if (rc != 10)
                return;
            size -= 10;

            flags = bytes2int(0, 0, header[8], header[9]);
            if (version >= ID3_VER_2_4)
                framelen = unsync(header[4], header[5], header[6], header[7]);
            else
                framelen = bytes2int(header[4], header[5],
                                     header[6], header[7]);
        }
        else
        {
            if (read(fd, header, 6) != 6)
                return;
            size -= 6;
            framelen = bytes2int(0, header[3], header[4], header[5]);
        }

        if (framelen == 0)
        {
            /* all-zero header means padding: the frames are done */
            if (header[0] == 0 && header[1] == 0 && header[2] == 0)
                return;
            continue;
        }

        unsynch = false;

        if (flags)
        {
            if (flags & ((version >= ID3_VER_2_4)? 0x0040: 0x0020))
            {
                lseek(fd, 1, SEEK_CUR);     /* grouping identity */
                framelen--;
            }
            if (flags & 0x000c)             /* compressed or encrypted */
            {
                size -= framelen;
                lseek(fd, framelen, SEEK_CUR);
                continue;
            }
            if (flags & 0x0002)
                unsynch = true;
            if (version >= ID3_VER_2_4 && (flags & 0x0001))
            {
                if (read(fd, tmp, 4) != 4)  /* data length indicator */
                    return;
                framelen -= 4;
            }
        }

        if (framelen == 0)
            continue;
        if (framelen < 0)
            return;

        if (!memcmp(header, "SLT", 3) || !memcmp(header, "SYLT", 4))
        {
            found = true;
            break;
        }

        if (global_unsynch && version <= ID3_VER_2_3)
        {
            if (!skip_unsynched(fd, framelen, &global_ff_found))
                return;
            size -= framelen;
        }
        else
        {
            size -= framelen;
            if (lseek(fd, framelen, SEEK_CUR) == -1)
                return;
        }
    }
    if (!found)
        return;

    if (framelen >= SYLT_MAX)
        framelen = SYLT_MAX - 1;
    tag = buffer + SYLT_TOP - framelen - 1;
    if (global_unsynch && version <= ID3_VER_2_3)
        bytesread = read_unsynched(fd, tag, framelen, &global_ff_found);
    else
        bytesread = read(fd, tag, framelen);

    if (bytesread != framelen)
        return;
    if (unsynch || (global_unsynch && version >= ID3_VER_2_4))
        bytesread = unsynchronize(tag, bytesread, NULL);
    tag[bytesread] = 0;

    encoding = tag[0];
    /* text encoding, language, timestamp format, content type */
    p = tag + 6;

    switch (encoding)
    {
        case 0x01:  /* UTF-16, with or without a BOM */
        case 0x02:
            if (!memcmp(p, "\xff\xfe", 2))
                utf_decode = utf16LEdecode;
            else if (!memcmp(p, "\xfe\xff", 2))
                utf_decode = utf16BEdecode;
            else
                utf_decode = NULL;

            encoding = NUM_CODEPAGES;
            /* skip the content descriptor, terminated by a zero unit */
            do {
                size = p[0] | p[1];
                p += 2;
            } while (size && p < tag + bytesread);
            chsiz = 2;
            break;

        default:
            utf_decode = utf8cpy;
            encoding = (encoding == 0x03)? UTF_8:
                       global_settings.default_codepage;
            p += strlen((char *)p) + 1;
            chsiz = 1;
            break;
    }

    if (encoding == NUM_CODEPAGES)
    {
        if (!memcmp(p, "\xff\xfe", 2))
        {
            utf_decode = utf16LEdecode;
            p += 2;
        }
        else if (!memcmp(p, "\xfe\xff", 2))
        {
            utf_decode = utf16BEdecode;
            p += 2;
        }
        else if (!utf_decode)
        {
            /* No BOM, which the specification does not allow. Guess: a zero
             * byte is far more likely to be the high half of a character. */
            utf_decode = (p[1] == 0)? utf16LEdecode: utf16BEdecode;
        }
    }

    bytesread -= p - tag;
    tag = p;

    /* Each entry is its text, then a four-byte timestamp. The frame data
     * still to be read sits between the two ends of the allocator, so stop
     * as soon as either end would reach it. */
    while (bytesread > 0
           && buffer + buf_used < tag
           && tag + bytesread + SYLT_SLACK < buffer + buf_end)
    {
        struct lyrics_line *lyr = alloc_buf(sizeof(struct lyrics_line));
        if (!lyr)
            break;

        if (encoding == NUM_CODEPAGES)
        {
            unsigned char *utf8 = utf8_line;
            unsigned char *lim = utf8_line + sizeof(utf8_line) - 4;
            p = tag;
            do {
                utf8 = utf_decode(p, utf8, 1);
                p += 2;
            } while (*(utf8 - 1) && utf8 < lim);
            *utf8 = 0;
        }
        else
        {
            unsigned char *end;
            size = strlen((char *)tag) + 1;
            end = iso_decode_ex(tag, utf8_line, encoding, size,
                                sizeof(utf8_line) - 2);
            *end = 0;
            p = tag + size;
        }

        lyr->time = bytes2int(p[0], p[1], p[2], p[3]);
        p += 4;
        utf_decode(p, tmp, 1);
        if (tmp[0] == 0x0a)     /* entries may be separated by a newline */
            p += chsiz;

        bytesread -= p - tag;
        tag = p;
        if (!add_line(lyr, utf8_line))
            break;
    }

    if (lrc.head)
    {
        lrc.type = TYPE_SYLT;
        strlcpy(lrc.file, track.path, sizeof(lrc.file));
    }
}

static void load_sylt(void)
{
    int fd;

    if (track.codectype != AFMT_MPA_L1
        && track.codectype != AFMT_MPA_L2
        && track.codectype != AFMT_MPA_L3)
        return;

    fd = open(track.path, O_RDONLY);
    if (fd < 0)
        return;
    parse_sylt(fd);
    close(fd);
}

/* ---------------------------------------------------------------------------
 * Loading, and the public accessors
 * ------------------------------------------------------------------------ */

enum lyrics_result lyrics_load(const struct mp3entry *id3)
{
    lyrics_close();

    /* An mp3entry is filled in as its track buffers, so it can be handed over
     * still empty -- audio_get_track_metadata() wipes it when even the
     * playlist cannot name the file. That is not "no lyrics"; it is "ask
     * again in a moment". */
    if (!id3 || !id3->path[0])
        return LYRICS_NO_TRACK;

    /* Copy everything needed out of the caller's entry BEFORE allocating --
     * see the comment on `track`. Order matters here, not style. */
    memset(&track, 0, sizeof(track));
    strlcpy(track.path, id3->path, sizeof(track.path));
    if (id3->title)
        strlcpy(track.title, id3->title, sizeof(track.title));
    if (id3->artist)
        strlcpy(track.artist, id3->artist, sizeof(track.artist));
    track.length     = id3->length;
    track.id3v2len   = id3->id3v2len;
    track.codectype  = id3->codectype;
    track.id3version = id3->id3version;

    lyrics_handle = core_alloc_ex(LYRICS_BUFFER_SIZE, &buflib_ops_locked);
    if (lyrics_handle <= 0)
    {
        lyrics_handle = 0;
        return LYRICS_NO_MEMORY;
    }
    buffer = core_get_data(lyrics_handle);

    reset_data();
    lrc.length = track.length;

    if (find_lyrics_file())
        load_lyrics_file();
    else
        load_sylt();

    sort_lines();
    /* without the index nothing can be reached, so that counts as no lyrics */
    lrc.loaded = (lrc.nline > 0) && build_index();

    if (!lrc.loaded)
    {
        lyrics_close();
        return LYRICS_NOT_FOUND;
    }
    return LYRICS_OK;
}

void lyrics_close(void)
{
    if (lyrics_handle > 0)
        core_free(lyrics_handle);
    lyrics_handle = 0;
    buffer = NULL;
    reset_data();
}

bool lyrics_loaded(void)
{
    return lrc.loaded;
}

const char *lyrics_file(void)
{
    return lrc.file;
}

const char *lyrics_title(void)
{
    return lrc.title;
}

const char *lyrics_artist(void)
{
    return lrc.artist;
}

int lyrics_count(void)
{
    return lrc.nline;
}

const char *lyrics_text(int index)
{
    struct lyrics_line *line = get_line(index);

    if (!line)
        return NULL;
    return (const char *)line->words[line->nword - 1].text;
}

long lyrics_time(int index)
{
    struct lyrics_line *line = get_line(index);

    if (!line)
        return 0;
    return apply_offset(line->time);
}

long lyrics_end_time(int index)
{
    long start, end;

    if (index < 0 || index >= lrc.nline)
        return 0;
    if (index + 1 < lrc.nline)
        return lyrics_time(index + 1);

    /* the last line runs to the end of the track */
    start = lyrics_time(index);
    end = lrc.length;
    return (end > start)? end: start;
}

int lyrics_index_at(long time)
{
    int lo = 0, hi = lrc.nline - 1, found = LYRICS_NONE;

    /* the last line to have started */
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (lyrics_time(mid) <= time)
        {
            found = mid;
            lo = mid + 1;
        }
        else
            hi = mid - 1;
    }
    return found;
}

int lyrics_word_count(int index)
{
    struct lyrics_line *line = get_line(index);

    return line? line->nword: 0;
}

const char *lyrics_word_text(int index, int word)
{
    struct lyrics_line *line = get_line(index);

    if (!line || word < 0 || word >= line->nword)
        return NULL;
    return (const char *)line->words[line->nword - 1 - word].text;
}

long lyrics_word_time(int index, int word)
{
    struct lyrics_line *line = get_line(index);
    long time;

    if (!line || word < 0 || word >= line->nword)
        return -1;
    time = line->words[line->nword - 1 - word].time;
    return (time < 0)? -1: apply_offset(time);
}
