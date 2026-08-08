/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Reads whichever log the core is writing, and decides what each line means.
 *
 * There are two, chosen by settings > playback > logging, and never written
 * at once:
 *
 *   playback.log      one line per finished track, "ts:elapsed:length:path",
 *                     rotated to playback_0001.log, playback_0002.log, ...
 *                     once it passes 511 KB. Read oldest first, and the
 *                     concatenation is what consumers see -- rotation renames
 *                     files but never changes those bytes, which is why a
 *                     byte offset into the family survives one.
 *
 *   /.scrobbler.log   Audioscrobbler 1.1: tab-separated, tagged names, a
 *                     played/skipped verdict instead of an elapsed time, and
 *                     no file path. Never rotated -- the core appends to it
 *                     forever and only writes the header when creating it.
 *
 * Both are written by add_playbacklog() in audio/playback.c; see enum
 * pv_source in the header for what the two formats can and cannot say.
 *
 * This knows nothing about statistics. It hands each line to a callback and
 * lets the caller decide what to count.
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <file.h>
#include "config.h"
#include "rbpaths.h"
#include "settings/settings.h"
#include "pv_log.h"

#define PV_LOG_PATH ROCKBOX_DIR "/playback.log"
#define PV_LOG_STEM ROCKBOX_DIR "/playback"
#define PV_SCROBBLER_PATH "/.scrobbler.log"

/* global_settings.playback_log, as audio/playback.c defines it. Not exported
 * from there, and not worth exporting for one comparison. */
#define PV_SETTING_LASTFM 2

/* Audioscrobbler 1.1 field order, and the marker the core writes for a track
 * with no artist tag. */
#define SCROB_F_ARTIST    0
#define SCROB_F_ALBUM     1
#define SCROB_F_TITLE     2
#define SCROB_F_LENGTH    4    /* seconds, not milliseconds */
#define SCROB_F_RATING    5    /* 'L' listened, 'S' skipped */
#define SCROB_F_TIMESTAMP 6
#define SCROB_FIELDS_MIN  7
#define SCROB_UNTAGGED    "<UNTAGGED>"

/* The numbered run is scanned from 1 until one is missing; the live log is
 * the link after it. Matches how create_numbered_filename() names them. */
#define PV_LOG_MAX_NUMBERED 9999

/* A line is the timestamp, two millisecond figures and a path. MAX_PATH plus
 * the three numbers and their separators, rounded up. Longer lines are
 * truncated but consumed whole, so a tail can never pose as the next line. */
#define PV_LINE_MAX 640

/* 16 KB rather than the obvious 4: the PP5022 in the 5G pays dearly per read
 * call, and a family can be a megabyte. Fewer, fatter reads, at the cost of
 * static RAM that the deck would otherwise have to find elsewhere. */
#define PV_BUF_SZ 16384

/* Streaming reader over the family. One log is read at a time, so a single
 * instance is all there is. */
static struct
{
    int fd;
    int pos, len;
    int next;   /* >0: numbered log to try next; 0: live log is next;
                   -1: the chain is finished */
    /* Bytes taken from the stream so far, counted through the whole family
     * rather than per file -- which is what makes an offset something that
     * survives rotation. */
    unsigned long consumed;
} rd = { .fd = -1, .next = -1 };

static char rd_buf[PV_BUF_SZ];

/* Open the next file of the family, stepping off the numbered run onto the
 * live log when the numbers run out. */
static bool rd_advance(void)
{
    if (rd.fd >= 0)
        close(rd.fd);
    rd.fd = -1;
    rd.pos = rd.len = 0;

    while (rd.next > 0)
    {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s_%04d.log", PV_LOG_STEM, rd.next);
        rd.next++;
        rd.fd = open(path, O_RDONLY);
        if (rd.fd >= 0)
            return true;
        rd.next = 0;    /* the first missing number ends the numbered run */
    }

    if (rd.next == 0)
    {
        rd.next = -1;   /* the live log is the last link */
        rd.fd = open(PV_LOG_PATH, O_RDONLY);
    }
    return rd.fd >= 0;
}

static bool rd_open(enum pv_source src)
{
    rd.pos = rd.len = 0;
    rd.consumed = 0;

    if (src == PV_SRC_SCROBBLER)
    {
        rd.next = -1;       /* one file, never rotated */
        rd.fd = open(PV_SCROBBLER_PATH, O_RDONLY);
        return rd.fd >= 0;
    }

    rd.next = 1;
    return rd_advance();
}

/* Open the family positioned at byte 'offset' of the concatenation: find the
 * file that offset falls in, seek into it, and leave the chain pointing at
 * whatever follows so reading simply carries on. */
static bool rd_open_at(enum pv_source src, unsigned long offset)
{
    unsigned long left = offset;

    rd.pos = rd.len = 0;
    rd.consumed = offset;

    if (src == PV_SRC_SCROBBLER)
    {
        rd.next = -1;
        rd.fd = open(PV_SCROBBLER_PATH, O_RDONLY);
        if (rd.fd < 0)
            return false;
        lseek(rd.fd, (off_t)left, SEEK_SET);
        return true;
    }

    for (int n = 1; n <= PV_LOG_MAX_NUMBERED; n++)
    {
        char path[MAX_PATH];
        unsigned long sz;
        int fd;

        snprintf(path, sizeof(path), "%s_%04d.log", PV_LOG_STEM, n);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            break;          /* numbered run ended; the live log is next */

        sz = (unsigned long)filesize(fd);
        if (left < sz)
        {
            lseek(fd, (off_t)left, SEEK_SET);
            rd.fd = fd;
            rd.next = n + 1;
            return true;
        }

        left -= sz;
        close(fd);
    }

    rd.fd = open(PV_LOG_PATH, O_RDONLY);
    rd.next = -1;
    if (rd.fd < 0)
        return false;
    lseek(rd.fd, (off_t)left, SEEK_SET);
    return true;
}

static void rd_close(void)
{
    if (rd.fd >= 0)
        close(rd.fd);
    rd.fd = -1;
    rd.next = -1;
}

/* Next line into 'line', CR/LF stripped. Steps across the family
 * transparently, treating each file's end as a line break. False only when
 * everything has been read. */
static bool rd_line(char *line, int sz)
{
    int n = 0;

    for (;;)
    {
        if (rd.pos >= rd.len)
        {
            rd.len = (rd.fd >= 0) ? read(rd.fd, rd_buf, sizeof(rd_buf)) : 0;
            rd.pos = 0;
            if (rd.len <= 0)
            {
                rd.len = 0;
                if (n > 0)
                    break;          /* an unterminated tail still counts */
                if (rd.next >= 0 && rd_advance())
                    continue;       /* next file of the family */
                break;
            }
        }

        char c = rd_buf[rd.pos++];
        rd.consumed++;
        if (c == '\n')
        {
            line[n] = '\0';
            return true;
        }
        if (c != '\r' && n < sz - 1)
            line[n++] = c;
    }

    line[n] = '\0';
    return n > 0;
}

static unsigned long parse_ul(const char *s)
{
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9')
    {
        v = v * 10 + (unsigned long)(*s - '0');
        s++;
    }
    return v;
}

static char *next_colon(char *s)
{
    while (*s && *s != ':')
        s++;
    return *s ? s + 1 : NULL;
}

/* Split a tab-separated line in place, filling up to maxf field pointers.
 * Trailing empty fields are real fields -- a track with no MusicBrainz id
 * still ends with a tab and nothing after it. */
static int split_tabs(char *line, char **f, int maxf)
{
    int n = 0;

    f[n++] = line;
    while (*line && n < maxf)
    {
        if (*line == '\t')
        {
            *line = '\0';
            f[n++] = line + 1;
        }
        line++;
    }
    return n;
}

/* "ts:elapsed:length:path". False if the line is not one. */
static bool parse_playback_line(char *line, struct pv_entry *e)
{
    char *p = line;

    e->ts = parse_ul(p);
    if (!(p = next_colon(p)))
        return false;
    e->elapsed_ms = parse_ul(p);
    if (!(p = next_colon(p)))
        return false;
    e->length_ms = parse_ul(p);
    if (!(p = next_colon(p)))
        return false;
    if (*p == '\0')
        return false;

    e->path   = p;
    e->artist = NULL;
    e->album  = NULL;
    e->title  = NULL;

    /* The Last.fm rule: half the track, or four minutes, is a play. A track
     * of unknown length is taken at its word. */
    e->listened = (e->length_ms == 0)
               || (e->elapsed_ms * 2 >= e->length_ms)
               || (e->elapsed_ms >= 240000UL);
    e->skipped = !e->listened && e->elapsed_ms >= PV_TAP_MS;
    return true;
}

/* Audioscrobbler: the verdict is already in the file, so there is no rule to
 * apply and no elapsed time to apply it to. */
static bool parse_scrobbler_line(char *line, struct pv_entry *e)
{
    char *f[8];
    char rating;

    if (split_tabs(line, f, 8) < SCROB_FIELDS_MIN)
        return false;

    e->ts        = parse_ul(f[SCROB_F_TIMESTAMP]);
    e->length_ms = parse_ul(f[SCROB_F_LENGTH]) * 1000UL;

    rating = f[SCROB_F_RATING][0];
    e->listened = (rating == 'L' || rating == 'l');

    /* No elapsed time is recorded. A play is credited with the whole track,
     * which is the closest honest answer -- the writer's own threshold was
     * half of it. A skip is credited with nothing. */
    e->elapsed_ms = e->listened ? e->length_ms : 0;

    /* With no elapsed time there is no way to tell a two-second browsing tap
     * from a track genuinely given up on, so every non-play is a skip. This
     * format simply cannot make the distinction the other one can. */
    e->skipped = !e->listened;

    e->path   = "";
    e->artist = f[SCROB_F_ARTIST];
    e->album  = f[SCROB_F_ALBUM];
    e->title  = f[SCROB_F_TITLE];

    if (!e->artist[0] || !strcmp(e->artist, SCROB_UNTAGGED))
        e->artist = "(unknown)";
    if (!e->title[0])
        e->title = "(unknown)";
    return true;
}

unsigned long pv_log_size(enum pv_source src)
{
    unsigned long total = 0;
    char path[MAX_PATH];
    int fd;

    if (src == PV_SRC_SCROBBLER)
    {
        fd = open(PV_SCROBBLER_PATH, O_RDONLY);
        if (fd < 0)
            return 0;
        total = (unsigned long)filesize(fd);
        close(fd);
        return total;
    }

    for (int n = 1; n <= PV_LOG_MAX_NUMBERED; n++)
    {
        snprintf(path, sizeof(path), "%s_%04d.log", PV_LOG_STEM, n);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            break;
        total += (unsigned long)filesize(fd);
        close(fd);
    }

    fd = open(PV_LOG_PATH, O_RDONLY);
    if (fd >= 0)
    {
        total += (unsigned long)filesize(fd);
        close(fd);
    }
    return total;
}

const char *pv_log_source_name(enum pv_source src)
{
    switch (src)
    {
    case PV_SRC_PLAYBACK:  return "playback log";
    case PV_SRC_SCROBBLER: return "scrobbler log";
    default:               return "none";
    }
}

int pv_log_peek(enum pv_source src, unsigned long offset, void *buf, int len)
{
    int got = 0;

    if (len <= 0 || offset < (unsigned long)len)
        return 0;
    if (!rd_open_at(src, offset - (unsigned long)len))
        return 0;

    /* Straight out of the stream rather than through the line reader: this
     * is bytes, not entries, and may well start mid-line. */
    while (got < len)
    {
        int n;

        if (rd.pos >= rd.len)
        {
            rd.len = (rd.fd >= 0) ? read(rd.fd, rd_buf, sizeof(rd_buf)) : 0;
            rd.pos = 0;
            if (rd.len <= 0)
            {
                rd.len = 0;
                if (rd.next >= 0 && rd_advance())
                    continue;
                break;
            }
        }

        n = rd.len - rd.pos;
        if (n > len - got)
            n = len - got;
        memcpy((char *)buf + got, rd_buf + rd.pos, (size_t)n);
        rd.pos += n;
        got += n;
    }

    rd_close();
    return got;
}

/* Does this source hold anything but headers? Stops at the first data line,
 * so an established log costs one buffer's worth of reading. */
static bool has_data(enum pv_source src)
{
    char line[PV_LINE_MAX];
    bool found = false;

    if (!rd_open(src))
        return false;

    while (rd_line(line, sizeof(line)))
    {
        if (line[0] != '#' && line[0] != '\0')
        {
            found = true;
            break;
        }
    }

    rd_close();
    return found;
}

enum pv_source pv_log_pick_source(void)
{
    enum pv_source preferred, other;

    preferred = (global_settings.playback_log == PV_SETTING_LASTFM)
              ? PV_SRC_SCROBBLER : PV_SRC_PLAYBACK;
    other = (preferred == PV_SRC_SCROBBLER)
          ? PV_SRC_PLAYBACK : PV_SRC_SCROBBLER;

    if (has_data(preferred))
        return preferred;
    if (has_data(other))
        return other;
    return PV_SRC_NONE;
}

long pv_log_read(enum pv_source src, pv_entry_fn cb, void *ctx)
{
    return pv_log_read_range(src, 0, 0, cb, ctx);
}

long pv_log_read_range(enum pv_source src, unsigned long from,
                       unsigned long to, pv_entry_fn cb, void *ctx)
{
    char line[PV_LINE_MAX];
    long lines = 0;

    if (src == PV_SRC_NONE)
        return -1;
    if (!(from ? rd_open_at(src, from) : rd_open(src)))
        return -1;

    for (;;)
    {
        unsigned long line_at = rd.consumed;
        struct pv_entry e;

        if (to && line_at >= to)
            break;
        if (!rd_line(line, sizeof(line)))
            break;

        /* '#' starts a header line in both formats: the per-boot marker in
         * one, the Audioscrobbler preamble in the other. */
        if (line[0] == '#' || line[0] == '\0')
            continue;

        if (src == PV_SRC_SCROBBLER)
        {
            if (!parse_scrobbler_line(line, &e))
                continue;
        }
        else if (!parse_playback_line(line, &e))
        {
            continue;
        }

        e.valid_ts = (e.ts >= PV_MIN_VALID_TS);
        e.offset = line_at;

        lines++;
        if (cb)
            cb(&e, ctx);
    }

    rd_close();
    return lines;
}
