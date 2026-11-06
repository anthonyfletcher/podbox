/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Reading statistics: kept by the reader as a book is read, shown from the
 * Books screen for one book or for all of them.
 *
 *   keeping  - a sitting, from opening a book to closing it: the time on
 *              each page turned forward, up to RS_PAGE_CAP of it so a
 *              player left open does not count, the words on the page, and
 *              the day it falls on; a page counts once a sitting, past
 *              where it opened, and only when on screen long enough to
 *              read
 *   records  - one a book, keyed by path, newest first, in
 *              user/reading_stats.dat
 *   showing  - a book's figures, and every book's together
 *
 * Pages turned with Read Aloud on count no time and no words: listening is
 * not reading speed. Days need the clock set; without it the dates and the
 * streak are left out and the rest still counts.
 ****************************************************************************/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "timefuncs.h"
#include "lang.h"
#include "string-extra.h"
#include "widgets/list.h"
#include "system/format_time.h"
#include "speech/talk.h"           /* UNIT_SEC */
#include "system/library_files.h"
#include "database/libfile.h"
#include "database/path_key.h"
#include "viewers/playback_viewer/pv_stats.h"  /* the calendar */
#include "reading_stats.h"

#define RS_MAX      256             /* books kept, as reading.dat keeps */
#define RS_PAGE_CAP (5 * 60 * HZ)   /* the most one page counts */
/* The least a page must be on screen to count: its words at RS_WPM_MAX,
 * which is past fast reading and into skimming, so the font's words a page
 * set it; and never under RS_PAGE_MIN */
#define RS_WPM_MAX  1000
#define RS_PAGE_MIN HZ

struct rs_record
{
    uint32_t seconds;               /* time reading */
    uint32_t pages, words;          /* turned forward, and on them */
    uint32_t longest;               /* the longest sitting, seconds */
    uint16_t started, last;         /* days since 1999-12-31, 0 unknown */
    uint16_t finished;
    uint16_t days;                  /* days read on */
    uint16_t streak, best_streak;   /* days in a row */
    uint8_t  pct;                   /* how far through, at the last close */
    uint8_t  pad[3];
    char     path[MAX_PATH];
};

/* ---- keeping ---------------------------------------------------------- */

static struct rs_record rs_cur;
static bool rs_open, rs_day_done;
static long rs_mark;                /* the clock's last event */
static long rs_ticks;               /* this sitting's time */
static int64_t rs_furthest;         /* pages before this one have counted */

#define RS_EPOCH pv_days_from_civil(1999, 12, 31)

/* Today as a record keeps it, or 0 without the clock */
static int rs_today(void)
{
    struct tm *tm = get_time();

    if (!valid_time(tm))
        return 0;
    return pv_days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday)
           - RS_EPOCH;
}

static int rs_open_file(struct libfile_header *h)
{
    return libfile_open(LIB_READING_STATS_FILE, LIB_READING_STATS_MAGIC,
                        LIB_READING_STATS_VERSION, sizeof(struct rs_record),
                        h, NULL);
}

/* The next record, its path terminated whatever the file holds and without
 * a volume, as text_viewer() keeps it */
static bool rs_read(int fd, struct rs_record *r)
{
    const char *path;

    if (read(fd, r, sizeof(*r)) != (ssize_t)sizeof(*r))
        return false;
    r->path[sizeof(r->path) - 1] = '\0';
    path = path_key_strip(r->path);
    memmove(r->path, path, strlen(path) + 1);
    return true;
}

/* The record of `path`, or a new one */
static void rs_load(const char *path, struct rs_record *out)
{
    struct libfile_header h;
    int fd = rs_open_file(&h);

    memset(out, 0, sizeof(*out));
    if (fd >= 0)
    {
        for (uint32_t i = 0; i < h.count && rs_read(fd, out); i++)
            if (!strcmp(out->path, path))
            {
                close(fd);
                return;
            }
        close(fd);
        memset(out, 0, sizeof(*out));
    }
    strmemccpy(out->path, path, sizeof(out->path));
}

/* Rewrites the file with `r` first, streaming the others past one buffer */
static void rs_save(const struct rs_record *r)
{
    static struct rs_record old;
    struct libfile_writer w;
    struct libfile_header h;
    int fd, kept = 1;
    bool ok;

    if (!libfile_begin(&w, LIB_READING_STATS_FILE, LIB_READING_STATS_MAGIC,
                       LIB_READING_STATS_VERSION, sizeof(struct rs_record),
                       NULL))
        return;
    ok = libfile_write(&w, r, sizeof(*r), 1);
    fd = rs_open_file(&h);
    if (fd >= 0)
    {
        for (uint32_t i = 0; ok && kept < RS_MAX && i < h.count
                             && rs_read(fd, &old); i++)
        {
            if (!strcmp(old.path, r->path))
                continue;
            ok = libfile_write(&w, &old, sizeof(old), 1);
            kept++;
        }
        close(fd);
    }
    libfile_finish(&w, ok);
}

void reading_stats_begin(const char *path, int64_t at)
{
    rs_load(path, &rs_cur);
    rs_open = true;
    rs_day_done = false;
    rs_ticks = 0;
    rs_furthest = at;
    rs_mark = current_tick;
}

void reading_stats_touch(void)
{
    rs_mark = current_tick;
}

void reading_stats_page(int words, int64_t at)
{
    long t = current_tick - rs_mark;
    long need = MAX(RS_PAGE_MIN,
                    (long)((int64_t)words * 60 * HZ / RS_WPM_MAX));

    if (!rs_open)
        return;
    rs_mark = current_tick;
    /* Paged past, or read already in this sitting: nothing counts, and
     * a page paged past still counts once it is read */
    if (t < need || at < rs_furthest)
        return;
    rs_furthest = at + 1;
    rs_ticks += t > RS_PAGE_CAP ? RS_PAGE_CAP : t;
    rs_cur.pages++;
    rs_cur.words += words;
    if (!rs_day_done)
    {
        int today = rs_today();

        rs_day_done = true;
        if (today)
        {
            if (!rs_cur.started)
                rs_cur.started = today;
            if (today != rs_cur.last)
            {
                rs_cur.days++;
                rs_cur.streak = rs_cur.last && today == rs_cur.last + 1
                                ? rs_cur.streak + 1 : 1;
                if (rs_cur.streak > rs_cur.best_streak)
                    rs_cur.best_streak = rs_cur.streak;
                rs_cur.last = today;
            }
        }
    }
}

void reading_stats_end(int pct, bool finished)
{
    uint32_t secs = rs_ticks / HZ;
    bool finish = finished && !rs_cur.finished && rs_today();

    if (!rs_open)
        return;
    rs_open = false;
    if (!rs_day_done && !finish)
        return;                         /* nothing read this time */
    rs_cur.seconds += secs;
    if (secs > rs_cur.longest)
        rs_cur.longest = secs;
    rs_cur.pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    if (finish)
        rs_cur.finished = rs_today();
    rs_save(&rs_cur);
}

/* ---- showing ---------------------------------------------------------- */

#define RS_LINES 12
static char rs_line[RS_LINES][80];
static int rs_n;

static const char *rs_name(int item, void *data, char *buf, size_t len)
{
    (void)data; (void)buf; (void)len;
    return rs_line[item];
}

/* A line "label: value", value formatted */
static void rs_add(int label, const char *fmt, ...)
{
    char value[64];
    va_list ap;

    if (rs_n == RS_LINES)
        return;
    va_start(ap, fmt);
    vsnprintf(value, sizeof(value), fmt, ap);
    va_end(ap);
    snprintf(rs_line[rs_n++], sizeof(rs_line[0]), "%s: %s", str(label),
             value);
}

static const char *rs_time(char *buf, size_t size, uint32_t secs)
{
    return format_time_auto(buf, size, secs, UNIT_SEC, false);
}

static void rs_date(char *buf, size_t size, int day)
{
    int y, m, d;

    pv_civil_from_days(day + RS_EPOCH, &y, &m, &d);
    snprintf(buf, size, "%s %d %d", str(LANG_MONTH_JANUARY + m - 1), d, y);
}

/* n days, or one */
static const char *rs_days(char *buf, size_t size, int n)
{
    if (n == 1)
        return str(LANG_RS_ONE_DAY);
    snprintf(buf, size, str(LANG_RS_DAYS), n);
    return buf;
}

/* The lines every screen has: time, pages, speed, longest, streak */
static void rs_add_pace(const struct rs_record *r)
{
    char t[32];

    rs_add(LANG_RS_TIME, "%s", rs_time(t, sizeof(t), r->seconds));
    if (r->days)
        rs_add(LANG_RS_PAGES, str(LANG_RS_PAGES_A_DAY), (int)r->pages,
               (int)((r->pages + r->days / 2) / r->days));
    else
        rs_add(LANG_RS_PAGES, "%d", (int)r->pages);
    if (r->seconds >= 60)
        rs_add(LANG_RS_SPEED, str(LANG_RS_WPM),
               (int)((uint64_t)r->words * 60 / r->seconds));
    if (r->longest)
        rs_add(LANG_RS_LONGEST, "%s", rs_time(t, sizeof(t), r->longest));
    if (r->best_streak)
        rs_add(LANG_RS_STREAK, "%s",
               rs_days(t, sizeof(t), r->best_streak));
}

static bool rs_show(const char *title)
{
    struct simplelist_info info;

    simplelist_info_init(&info, (char *)title, rs_n, NULL);
    info.get_name = rs_name;
    return !simplelist_show_list(&info);
}

bool reading_stats_show(const char *path, const char *title)
{
    struct rs_record r;
    char d[40];

    rs_load(path, &r);
    rs_n = 0;
    if (!r.pages)
    {
        strmemccpy(rs_line[rs_n++], str(LANG_RS_NOT_READ), sizeof(rs_line[0]));
        return rs_show(title);
    }
    rs_add_pace(&r);
    if (r.pct > 0 && r.pct < 100 && !r.finished && r.seconds >= 60)
    {
        char t[32];

        rs_add(LANG_RS_LEFT, str(LANG_RS_ABOUT),
               rs_time(t, sizeof(t),
                       (uint32_t)((uint64_t)r.seconds * (100 - r.pct) / r.pct)));
    }
    if (r.started)
    {
        rs_date(d, sizeof(d), r.started);
        rs_add(LANG_RS_STARTED, "%s", d);
    }
    if (r.last && r.last != r.finished)
    {
        rs_date(d, sizeof(d), r.last);
        rs_add(LANG_RS_LAST, "%s", d);
    }
    if (r.finished)
    {
        char n[24];

        rs_date(d, sizeof(d), r.finished);
        if (r.started && r.finished >= r.started)
            rs_add(LANG_BOOKS_FINISHED, str(LANG_RS_IN_DAYS), d,
                   rs_days(n, sizeof(n), r.finished - r.started + 1));
        else
            rs_add(LANG_BOOKS_FINISHED, "%s", d);
    }
    return rs_show(title);
}

bool reading_stats_show_all(void)
{
    struct libfile_header h;
    struct rs_record r, all;
    int fd = rs_open_file(&h), books = 0, finished = 0;
    char d[40];

    memset(&all, 0, sizeof(all));
    if (fd >= 0)
    {
        for (uint32_t i = 0; i < h.count && rs_read(fd, &r); i++)
        {
            if (!r.pages)
                continue;
            books++;
            finished += r.finished != 0;
            all.seconds += r.seconds;
            all.pages += r.pages;
            all.words += r.words;
            if (r.longest > all.longest)
                all.longest = r.longest;
            if (r.best_streak > all.best_streak)
                all.best_streak = r.best_streak;
            if (r.started && (!all.started || r.started < all.started))
                all.started = r.started;
        }
        close(fd);
    }
    rs_n = 0;
    if (!books)
    {
        strmemccpy(rs_line[rs_n++], str(LANG_RS_NOT_READ), sizeof(rs_line[0]));
        return rs_show(str(LANG_READING_STATS));
    }
    rs_add(LANG_RS_BOOKS, str(LANG_RS_BOOKS_FINISHED), books, finished);
    rs_add_pace(&all);
    if (all.started)
    {
        rs_date(d, sizeof(d), all.started);
        rs_add(LANG_RS_SINCE, "%s", d);
    }
    return rs_show(str(LANG_READING_STATS));
}
