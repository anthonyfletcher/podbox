/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Reads playback.log, and decides what each line means.
 *
 * One line per finished track, "ts:elapsed:length:path", rotated to
 * playback_0001.log, playback_0002.log, ... once it passes 511 KB. Read oldest
 * first, and the concatenation is what consumers see -- rotation renames files
 * but never changes those bytes, which is why a byte offset into the family
 * survives one. Written by add_playbacklog() in audio/playback.c under both On
 * and On + Last.fm; the /.scrobbler.log that On + Last.fm also writes is the
 * uploader's, and is not read here: it carries no file paths, so no artwork.
 *
 * This knows nothing about statistics. It hands each line to a callback and
 * lets the caller decide what to count.
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <file.h>
#include "config.h"
#include "kernel.h"
#include "system/library_files.h"
#include "rbpaths.h"
#include "pv_log.h"

#define PV_LOG_PATH LIB_PLAYBACK_LOG
#define PV_LOG_STEM LIB_PLAYBACK_STEM

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

/* The one reader is shared by the UI and an accessory's thread (On Repeat,
 * read for a car), so whoever comes second waits */
static struct mutex rd_mutex;
static bool rd_mutex_ready;

static void rd_lock(void)
{
    /* Threads here are cooperative, so nothing runs between the test and
     * the set */
    if (!rd_mutex_ready)
    {
        mutex_init(&rd_mutex);
        rd_mutex_ready = true;
    }
    mutex_lock(&rd_mutex);
}

static void rd_unlock(void)
{
    mutex_unlock(&rd_mutex);
}

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

static bool rd_open(void)
{
    rd.pos = rd.len = 0;
    rd.consumed = 0;
    rd.next = 1;
    return rd_advance();
}

/* Open the family positioned at byte 'offset' of the concatenation: find the
 * file that offset falls in, seek into it, and leave the chain pointing at
 * whatever follows so reading simply carries on. */
static bool rd_open_at(unsigned long offset)
{
    unsigned long left = offset;

    rd.pos = rd.len = 0;
    rd.consumed = offset;

    for (int n = 1; n <= PV_LOG_MAX_NUMBERED; n++)
    {
        char path[MAX_PATH];
        unsigned long sz;
        int fd;

        snprintf(path, sizeof(path), "%s_%04d.log", PV_LOG_STEM, n);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            break;          /* numbered run ended; the live log is next */

        sz = (unsigned long)ffilesize(fd);
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

/* Refill the buffer from the open file. False at its end, which leaves the
 * buffer empty. */
static bool rd_fill(void)
{
    rd.len = (rd.fd >= 0) ? read(rd.fd, rd_buf, sizeof(rd_buf)) : 0;
    rd.pos = 0;
    if (rd.len <= 0)
    {
        rd.len = 0;
        return false;
    }
    return true;
}

/* Next line into 'line', CR/LF stripped. Steps across the family
 * transparently, treating each file's end as a line break. False only when
 * everything has been read. */
static bool rd_line(char *line, int sz)
{
    int n = 0;

    for (;;)
    {
        if (rd.pos >= rd.len && !rd_fill())
        {
            if (n > 0)
                break;              /* an unterminated tail still counts */
            if (rd.next >= 0 && rd_advance())
                continue;           /* next file of the family */
            break;
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

    /* The Last.fm rule: half the track, or four minutes, is a play. A track
     * of unknown length is taken at its word. */
    e->listened = (e->length_ms == 0)
               || (e->elapsed_ms * 2 >= e->length_ms)
               || (e->elapsed_ms >= 240000UL);
    e->skipped = !e->listened && e->elapsed_ms >= PV_TAP_MS;
    return true;
}

unsigned long pv_log_size(enum pv_source src)
{
    unsigned long total = 0;
    char path[MAX_PATH];
    int fd;

    if (src == PV_SRC_NONE)
        return 0;

    for (int n = 1; n <= PV_LOG_MAX_NUMBERED; n++)
    {
        snprintf(path, sizeof(path), "%s_%04d.log", PV_LOG_STEM, n);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            break;
        total += (unsigned long)ffilesize(fd);
        close(fd);
    }

    fd = open(PV_LOG_PATH, O_RDONLY);
    if (fd >= 0)
    {
        total += (unsigned long)ffilesize(fd);
        close(fd);
    }
    return total;
}

const char *pv_log_source_name(enum pv_source src)
{
    switch (src)
    {
    case PV_SRC_PLAYBACK:  return "playback log";
    default:               return "none";
    }
}

int pv_log_peek(enum pv_source src, unsigned long offset, void *buf, int len)
{
    int got = 0;

    if (src == PV_SRC_NONE || len <= 0 || offset < (unsigned long)len)
        return 0;
    rd_lock();
    if (!rd_open_at(offset - (unsigned long)len))
    {
        rd_unlock();
        return 0;
    }

    /* Straight out of the stream rather than through the line reader: this
     * is bytes, not entries, and may well start mid-line. */
    while (got < len)
    {
        int n;

        if (rd.pos >= rd.len && !rd_fill())
        {
            if (rd.next >= 0 && rd_advance())
                continue;
            break;
        }

        n = rd.len - rd.pos;
        if (n > len - got)
            n = len - got;
        memcpy((char *)buf + got, rd_buf + rd.pos, (size_t)n);
        rd.pos += n;
        got += n;
    }

    rd_close();
    rd_unlock();
    return got;
}

/* Stops at the first data line, so an established log costs one buffer's
 * worth of reading. */
enum pv_source pv_log_pick_source(void)
{
    char line[PV_LINE_MAX];
    struct pv_entry e;
    bool found = false;

    rd_lock();
    if (rd_open())
    {
        while (!found && rd_line(line, sizeof(line)))
            found = line[0] != '#' && line[0] != '\0'
                    && parse_playback_line(line, &e);
        rd_close();
    }
    rd_unlock();
    return found ? PV_SRC_PLAYBACK : PV_SRC_NONE;
}

#define PV_FIRST_SZ 512

/* The first dated entry among the first PV_FIRST_SZ bytes of 'fd', or 0 when
 * none of them carries one, which has the caller read the file whole. A date
 * opens its line, and one the read cuts short is too small to count. Reads
 * into rd_buf, which is free while no reader runs. */
static unsigned long file_first_ts(int fd)
{
    int len = read(fd, rd_buf, PV_FIRST_SZ);
    char *p = rd_buf;

    if (len <= 0)
        return 0;
    rd_buf[len] = '\0';

    while (*p)
    {
        unsigned long ts = (*p >= '0' && *p <= '9') ? parse_ul(p) : 0;

        if (ts >= PV_MIN_VALID_TS)
            return ts;
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    return 0;
}

/* Whether the file open on 'fd' starts before 'ts', moving 'at' to its start
 * when it does; 'total' is where it starts. Closes 'fd'. A file that starts
 * at or after 'ts' holds nothing earlier, and neither does anything after
 * it. */
static bool starts_before(int fd, unsigned long ts, unsigned long *at,
                          unsigned long *total)
{
    unsigned long sz = (unsigned long)ffilesize(fd);
    unsigned long first = file_first_ts(fd);

    close(fd);
    if (first >= ts)
        return false;
    *at = *total;
    *total += sz;
    return true;
}

unsigned long pv_log_offset_since(enum pv_source src, unsigned long ts)
{
    unsigned long at = 0, total = 0;
    char path[MAX_PATH];
    bool more = true;
    int fd;

    if (src == PV_SRC_NONE)
        return 0;

    rd_lock();     /* file_first_ts() reads into rd_buf */
    for (int n = 1; more && n <= PV_LOG_MAX_NUMBERED; n++)
    {
        snprintf(path, sizeof(path), "%s_%04d.log", PV_LOG_STEM, n);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            break;
        more = starts_before(fd, ts, &at, &total);
    }

    if (more && (fd = open(PV_LOG_PATH, O_RDONLY)) >= 0)
        starts_before(fd, ts, &at, &total);
    rd_unlock();
    return at;
}

long pv_log_read(enum pv_source src, pv_entry_fn cb, void *ctx)
{
    return pv_log_read_range(src, 0, 0, cb, ctx);
}

long pv_log_read_range(enum pv_source src, unsigned long from,
                       unsigned long to, pv_entry_fn cb, void *ctx)
{
    return pv_log_read_range_stop(src, from, to, cb, ctx, NULL);
}

long pv_log_read_range_stop(enum pv_source src, unsigned long from,
                            unsigned long to, pv_entry_fn cb, void *ctx,
                            const volatile bool *stop)
{
    char line[PV_LINE_MAX];
    long lines = 0;

    if (src == PV_SRC_NONE)
        return -1;
    rd_lock();
    if (!(from ? rd_open_at(from) : rd_open()))
    {
        rd_unlock();
        return -1;
    }

    for (;;)
    {
        unsigned long line_at = rd.consumed;
        struct pv_entry e;

        if ((to && line_at >= to) || (stop && *stop))
            break;
        if (!rd_line(line, sizeof(line)))
            break;

        /* '#' starts the per-boot marker line */
        if (line[0] == '#' || line[0] == '\0')
            continue;
        if (!parse_playback_line(line, &e))
            continue;

        e.valid_ts = (e.ts >= PV_MIN_VALID_TS);
        e.offset = line_at;

        lines++;
        if (cb)
            cb(&e, ctx);
    }

    rd_close();
    rd_unlock();
    return lines;
}
