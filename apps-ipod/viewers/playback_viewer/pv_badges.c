/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/achievements_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * The badge engine: 340 definitions, and the metrics they are measured
 * against.
 *
 * Most metrics fall straight out of the aggregates -- minutes, plays, unique
 * artists, the heaviest day. A handful cannot: the longest run of tracks you
 * did not skip, the most plays of one song in one day, which listening types
 * you have passed through. Those depend on the ORDER entries arrive in, and
 * so are built up as the log is read, in struct pv_badge_state.
 *
 * That state is plain data on purpose. It rides in the saved index next to
 * the aggregate tables, which is what lets reading only the new tail of the
 * log continue these figures instead of starting them over -- and it is the
 * reason the index was built before the badges rather than after.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "config.h"
#include <file.h>
#include "rbpaths.h"
#include "timefuncs.h"
#include "pv_badges.h"
#include "pv_stats.h"

/* Plays before a listening type is allowed to count. Below this the profile
 * is one afternoon's mood rather than a habit. */
#define PV_TYPE_FLOOR 100

/* Week tiers, matching the year card's ladder (see pv_year.c). */
#define PV_WEEK_ACTIVE_SECS (30 * 60)
#define PV_WEEK_SUPER_SECS  (1440 * 60)
#define PV_WEEK_ULTRA_SECS  (2880 * 60)

/* Date flags, packed into one byte of the saved state. */
#define PV_DF_NEWYEAR   0x01
#define PV_DF_XMAS      0x02
#define PV_DF_HALLOWEEN 0x04
#define PV_DF_FRIDAY13  0x08
#define PV_DF_MIDNIGHT  0x10
#define PV_DF_LEAPDAY   0x20

#include "pv_badges_table.h"

#define PV_N_BADGES ((int)(sizeof(pv_badges_table) / sizeof(pv_badges_table[0])))

static long am_val[PV_AM_COUNT];
static unsigned char unlocked[(PV_N_BADGES + 7) / 8];
static int unlocked_n;

static unsigned int hash_str(const char *s)
{
    unsigned int h = 2166136261u;
    while (*s)
    {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

void pv_badges_reset(struct pv_badge_state *st)
{
    memset(st, 0, sizeof(*st));
    st->cur_day = -1;
}

void pv_badges_feed(struct pv_badge_state *st, unsigned long ts, bool valid_ts,
                    bool listened, unsigned secs,
                    const char *artist, const char *title)
{
    const struct pv_agg *a;
    long day;
    int y, m, d;
    unsigned tod, h;
    int i;

    if (listened)
    {
        if (++st->play_run > st->play_run_max)
            st->play_run_max = st->play_run;
        st->skip_run = 0;

        st->plays++;

        /* The profile reads the tables as they stand, which is why this must
         * be fed after they are updated: it wants this play included. */
        a = pv_stats_find(PV_T_ARTIST, artist);
        if (a && a->count > st->top_artist)
            st->top_artist = a->count;
        a = pv_stats_find(PV_T_TITLE, title);
        if (a && a->count == 1)
            st->uniq++;         /* first time this title has been heard */

        if (valid_ts && (ts % 86400UL) / 3600UL < 5)
            st->night++;

        if (st->plays >= PV_TYPE_FLOOR)
        {
            /* The same four axes and tipping points as the listening-type
             * card. If one moves, both must move. */
            int v0 = (int)(st->top_artist * 100 / st->plays);
            int v1 = (int)((st->plays - st->uniq) * 100 / st->plays);
            long tot = st->plays + st->skips;
            int v2 = (int)(st->plays * 100 / (tot ? tot : 1));
            int v3 = (int)(st->night * 100 / st->plays);
            int tt = ((v0 >= 45) << 3) | ((v1 >= 55) << 2)
                   | ((v2 >= 78) << 1) |  (v3 >= 8);

            st->type_seen[tt] = 1;
        }
    }
    else
    {
        if (++st->skip_run > st->skip_run_max)
            st->skip_run_max = st->skip_run;
        st->play_run = 0;
        st->skips++;
    }

    /* Everything below is about when a play happened, so a skip or an entry
     * with no usable clock has nothing to add. */
    if (!valid_ts || !listened)
        return;

    day = (long)(ts / 86400UL);
    if (day != st->cur_day)
    {
        st->cur_day = day;
        st->day_secs = 0;
        st->daytrk_n = 0;
    }

    st->day_secs += secs;
    if (st->day_secs > st->day_secs_max)
        st->day_secs_max = st->day_secs;

    h = hash_str(title);
    for (i = 0; i < st->daytrk_n; i++)
    {
        if (st->daytrk[i].hash == h)
            break;
    }
    if (i == st->daytrk_n && i < PV_BADGE_DAYTRK)
    {
        st->daytrk[i].hash = h;
        st->daytrk[i].n = 0;
        st->daytrk_n++;
    }
    if (i < st->daytrk_n)
    {
        if (++st->daytrk[i].n > st->trackday_max)
            st->trackday_max = st->daytrk[i].n;
    }

    pv_civil_from_days(day, &y, &m, &d);
    tod = (unsigned)(ts % 86400UL);

    if (m == 1 && d == 1 && tod < 3600)
        st->date_flags |= PV_DF_NEWYEAR;
    if (m == 12 && d == 25)
        st->date_flags |= PV_DF_XMAS;
    if (m == 10 && d == 31)
        st->date_flags |= PV_DF_HALLOWEEN;
    if (m == 2 && d == 29)
        st->date_flags |= PV_DF_LEAPDAY;
    /* Unix day 0 was a Thursday, so day % 7 == 1 is a Friday. */
    if (d == 13 && (day % 7) == 1)
        st->date_flags |= PV_DF_FRIDAY13;
    if (tod < 60)
        st->date_flags |= PV_DF_MIDNIGHT;
}

/* ------------------------------------------------------------ evaluating */

static bool is_unlocked(int i)
{
    return am_val[pv_badges_table[i].metric] >= pv_badges_table[i].threshold;
}

/* Months where every single day had a play. */
static long perfect_months(void)
{
    struct
    {
        short y;
        signed char m;
        short n;
    } mon[60];
    const struct pv_day *days;
    int day_n, mn = 0;
    long pm = 0;

    days = pv_stats_days(&day_n);
    if (!days)
        return 0;

    for (int i = 0; i < day_n; i++)
    {
        int y, m, d, j;

        pv_civil_from_days(days[i].day, &y, &m, &d);
        for (j = 0; j < mn; j++)
        {
            if (mon[j].y == y && mon[j].m == m)
                break;
        }
        if (j == mn && mn < (int)(sizeof(mon) / sizeof(mon[0])))
        {
            mon[mn].y = (short)y;
            mon[mn].m = (signed char)m;
            mon[mn].n = 0;
            mn++;
        }
        if (j < mn)
            mon[j].n++;
    }

    for (int j = 0; j < mn; j++)
    {
        int ny = (mon[j].m == 12) ? mon[j].y + 1 : mon[j].y;
        int nm = (mon[j].m == 12) ? 1 : mon[j].m + 1;
        long mlen = pv_days_from_civil(ny, nm, 1)
                  - pv_days_from_civil(mon[j].y, mon[j].m, 1);

        if (mon[j].n >= mlen)
            pm++;
    }
    return pm;
}

/* Weeks, from the day array: a week's listening is the sum of its days'. */
static void week_metrics(void)
{
    const struct pv_day *days;
    int day_n;
    long cur_week = 0;
    unsigned cur = 0;
    long wmax = 0, sup = 0, ult = 0, act = 0;

    days = pv_stats_days(&day_n);

    for (int i = 0; i <= day_n; i++)
    {
        long w = (i < day_n) ? ((days[i].day - 4) / 7) : 0;

        /* Close the previous week when the week changes, and once more at
         * the end so the last one is counted too. */
        if (i == day_n || (i > 0 && w != cur_week))
        {
            if (cur / 60 > (unsigned)wmax)
                wmax = cur / 60;
            if (cur >= PV_WEEK_SUPER_SECS)
                sup++;
            if (cur >= PV_WEEK_ULTRA_SECS)
                ult++;
            if (cur >= PV_WEEK_ACTIVE_SECS)
                act++;
            cur = 0;
        }
        if (i == day_n)
            break;

        cur_week = w;
        cur += days[i].secs;
    }

    am_val[PV_AM_WEEK_MINS]    = wmax;
    am_val[PV_AM_SUPERWEEKS]   = sup;
    am_val[PV_AM_ULTRAWEEKS]   = ult;
    am_val[PV_AM_ACTIVE_WEEKS] = act;
}

int pv_badges_eval(const struct pv_badge_state *st,
                   const struct pv_totals *t)
{
    const struct pv_agg *rows;
    int n;
    long hcov = 0, dp = 0;
    long btrk = 0, bart = 0, balb = 0, bloyal = 0, bskip = 0;
    const struct pv_day *days;
    int day_n;

    memset(am_val, 0, sizeof(am_val));
    memset(unlocked, 0, sizeof(unlocked));
    unlocked_n = 0;

    am_val[PV_AM_MINUTES]      = t->seconds / 60;
    am_val[PV_AM_PLAYS]        = t->plays;
    am_val[PV_AM_SKIPS]        = t->skips;
    am_val[PV_AM_UNIQ_TRACKS]  = t->titles;
    am_val[PV_AM_UNIQ_ARTISTS] = t->artists;
    am_val[PV_AM_UNIQ_ALBUMS]  = t->albums;
    am_val[PV_AM_NIGHT_PLAYS]  = t->night;
    am_val[PV_AM_ACTIVE_DAYS]  = t->days;
    am_val[PV_AM_STREAK]       = t->streak;

    for (int i = 0; i < 24; i++)
    {
        if (t->hour_hist[i] > 0)
            hcov++;
    }
    am_val[PV_AM_HOURS_COVERED] = hcov;

    /* Bests, one pass per table. */
    rows = pv_stats_rows(PV_T_TITLE, &n);
    for (int i = 0; i < n; i++)
    {
        if (rows[i].count > btrk)
            btrk = rows[i].count;
        if (rows[i].skips == 0 && rows[i].count > bloyal)
            bloyal = rows[i].count;
        if (rows[i].skips > bskip)
            bskip = rows[i].skips;
    }
    rows = pv_stats_rows(PV_T_ARTIST, &n);
    for (int i = 0; i < n; i++)
    {
        if (rows[i].count > bart)
            bart = rows[i].count;
    }
    rows = pv_stats_rows(PV_T_ALBUM, &n);
    for (int i = 0; i < n; i++)
    {
        if (rows[i].count > balb)
            balb = rows[i].count;
    }
    am_val[PV_AM_TRACK_PLAYS]  = btrk;
    am_val[PV_AM_ARTIST_PLAYS] = bart;
    am_val[PV_AM_ALBUM_PLAYS]  = balb;
    am_val[PV_AM_LOYAL_PLAYS]  = bloyal;
    am_val[PV_AM_TRACK_SKIPS]  = bskip;

    days = pv_stats_days(&day_n);
    for (int i = 0; i < day_n; i++)
    {
        if (days[i].count > dp)
            dp = days[i].count;
    }
    am_val[PV_AM_DAY_PLAYS] = dp;

    /* The order-dependent half. */
    am_val[PV_AM_DAY_MINS]  = st->day_secs_max / 60;
    am_val[PV_AM_TRACK_DAY] = st->trackday_max;
    am_val[PV_AM_SKIP_RUN]  = st->skip_run_max;
    am_val[PV_AM_PLAY_RUN]  = st->play_run_max;

    week_metrics();
    am_val[PV_AM_PERFECT_MONTHS] = perfect_months();

    am_val[PV_AM_F_NEWYEAR]   = (st->date_flags & PV_DF_NEWYEAR)   ? 1 : 0;
    am_val[PV_AM_F_XMAS]      = (st->date_flags & PV_DF_XMAS)      ? 1 : 0;
    am_val[PV_AM_F_HALLOWEEN] = (st->date_flags & PV_DF_HALLOWEEN) ? 1 : 0;
    am_val[PV_AM_F_FRIDAY13]  = (st->date_flags & PV_DF_FRIDAY13)  ? 1 : 0;
    am_val[PV_AM_F_MIDNIGHT]  = (st->date_flags & PV_DF_MIDNIGHT)  ? 1 : 0;
    am_val[PV_AM_F_LEAPDAY]   = (st->date_flags & PV_DF_LEAPDAY)   ? 1 : 0;

    {
        long tc = 0;

        for (int i = 0; i < 16; i++)
        {
            am_val[PV_AM_TYPE0 + i] = st->type_seen[i] ? 1 : 0;
            if (st->type_seen[i])
                tc++;
        }
        am_val[PV_AM_TYPES_COLLECTED] = tc;
    }

    /* Meta counts everything else, so it is worked out after the rest and
     * never counts itself. */
    {
        long meta = 0;

        for (int i = 0; i < PV_N_BADGES; i++)
        {
            if (pv_badges_table[i].metric != PV_AM_META && is_unlocked(i))
                meta++;
        }
        am_val[PV_AM_META] = meta;
    }

    for (int i = 0; i < PV_N_BADGES; i++)
    {
        if (is_unlocked(i))
        {
            unlocked[i >> 3] |= 1u << (i & 7);
            unlocked_n++;
        }
    }

    return unlocked_n;
}

int pv_badges_count(void)
{
    return PV_N_BADGES;
}

int pv_badges_unlocked_count(void)
{
    return unlocked_n;
}

bool pv_badges_unlocked(int i)
{
    if (i < 0 || i >= PV_N_BADGES)
        return false;
    return (unlocked[i >> 3] >> (i & 7)) & 1;
}

const struct pv_badge *pv_badges_get(int i)
{
    if (i < 0 || i >= PV_N_BADGES)
        return NULL;
    return &pv_badges_table[i];
}

long pv_badges_value(int i)
{
    if (i < 0 || i >= PV_N_BADGES)
        return 0;
    return am_val[pv_badges_table[i].metric];
}

/* ---------------------------------------------------------- progress file */

/* What the user has already been shown, and when each badge was earned.
 *
 * Keyed by table index, which is why pv_badges_table.h says never to insert a
 * row: doing so hands every later badge someone else's history. The magic
 * carries the row count so a table that changed size is discarded rather than
 * misread. */
#define PV_BADGES_PATH  ROCKBOX_DIR "/pv_badges.dat"
#define PV_BADGES_MAGIC 0x50564231UL   /* "PVB1" */

static unsigned char seen[(PV_N_BADGES + 7) / 8];
static unsigned char is_new[(PV_N_BADGES + 7) / 8];
static unsigned long when[PV_N_BADGES];
static enum pv_badge_vis vis[PV_N_BADGES];
static bool progress_loaded;
static int  new_n;

/* Whether there was a file to be new against.
 *
 * A device with a year of history unlocks most of the table the first time it
 * is evaluated, and none of it is news. Nothing counts as new unless progress
 * was read in full first, which covers a fresh device, a file the sync
 * deleted and a truncated one in the same rule. */
static bool had_progress;

static bool bit_get(const unsigned char *m, int i)
{
    return (m[i >> 3] >> (i & 7)) & 1;
}

static void bit_set(unsigned char *m, int i)
{
    m[i >> 3] |= 1u << (i & 7);
}

static void progress_load(void)
{
    unsigned long hdr[2];
    int fd;

    if (progress_loaded)
        return;
    progress_loaded = true;

    memset(seen, 0, sizeof(seen));
    memset(when, 0, sizeof(when));

    fd = open(PV_BADGES_PATH, O_RDONLY);
    if (fd < 0)
        return;

    if (read(fd, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
        && hdr[0] == PV_BADGES_MAGIC
        && hdr[1] == (unsigned long)PV_N_BADGES)
    {
        if (read(fd, seen, sizeof(seen)) != (ssize_t)sizeof(seen)
            || read(fd, when, sizeof(when)) != (ssize_t)sizeof(when))
        {
            /* A partial read is not partial progress. */
            memset(seen, 0, sizeof(seen));
            memset(when, 0, sizeof(when));
        }
        else
        {
            had_progress = true;
        }
    }
    close(fd);
}

/* All three writes checked, and the file removed if any falls short.
 *
 * progress_load() treats a short read as no progress at all rather than
 * partial progress, which is the right call -- but it means a truncated save
 * leaves a valid header over a short body, the next load zeroes every badge,
 * and the save after that writes the zeroes back. A careful reader and a
 * careless writer between them turn one full disk into progress lost for good.
 * Removing the file instead leaves the next load with nothing to open, which
 * it already handles. */
void pv_badges_save(void)
{
    unsigned long hdr[2];
    bool ok;
    int fd = open(PV_BADGES_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd < 0)
        return;

    hdr[0] = PV_BADGES_MAGIC;
    hdr[1] = (unsigned long)PV_N_BADGES;

    ok = write(fd, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
      && write(fd, seen, sizeof(seen)) == (ssize_t)sizeof(seen)
      && write(fd, when, sizeof(when)) == (ssize_t)sizeof(when);
    close(fd);

    if (!ok)
        remove(PV_BADGES_PATH);
}

/* Put every unlocked badge back to unannounced, for testing the crowns.
 *
 * The dates survive, because classify() only stamps a badge that has none --
 * otherwise the one hook that makes the feature testable would destroy the
 * earned dates every time it was used. */
void pv_badges_rearm(void)
{
    progress_load();
    memset(seen, 0, sizeof(seen));
    had_progress = true;     /* the save below is the file to be new against */
    pv_badges_save();
}

int pv_badges_classify(void)
{
    bool goal_taken[PV_AM_COUNT];
    unsigned long now;

    progress_load();
    memset(goal_taken, 0, sizeof(goal_taken));
    memset(is_new, 0, sizeof(is_new));
    new_n = 0;

    now = (unsigned long)mktime(get_time());

    for (int i = 0; i < PV_N_BADGES; i++)
    {
        if (pv_badges_unlocked(i))
        {
            vis[i] = PV_BV_DONE;
            if (!bit_get(seen, i))
            {
                bit_set(seen, i);

                /* The log cannot say when this was actually earned -- only
                 * that it is earned now -- so today is the honest answer. A
                 * badge that already carries a date keeps it, and one earned
                 * before this ever ran keeps 0 and says so. */
                if (when[i] == 0)
                    when[i] = now;

                if (had_progress)
                {
                    bit_set(is_new, i);
                    new_n++;
                }
            }
        }
        else if (!(pv_badges_table[i].flags & PV_BADGE_SECRET)
                 && !goal_taken[pv_badges_table[i].metric])
        {
            /* The lowest unearned rung of this ladder, and only that one. */
            vis[i] = PV_BV_GOAL;
            goal_taken[pv_badges_table[i].metric] = true;
        }
        else
        {
            vis[i] = PV_BV_HIDDEN;
        }
    }

    return new_n;
}

int pv_badges_new_count(void)
{
    return new_n;
}

/* Walked rather than kept in a list: this is called once per announcement
 * page, against a few hundred rows, next to a full-screen gradient. */
int pv_badges_new_index(int k)
{
    if (k >= 0)
    {
        for (int i = 0; i < PV_N_BADGES; i++)
        {
            if (bit_get(is_new, i) && k-- == 0)
                return i;
        }
    }
    return -1;
}

enum pv_badge_vis pv_badges_vis(int i)
{
    if (i < 0 || i >= PV_N_BADGES)
        return PV_BV_HIDDEN;
    return vis[i];
}

bool pv_badges_is_new(int i)
{
    if (i < 0 || i >= PV_N_BADGES)
        return false;
    return bit_get(is_new, i);
}

unsigned long pv_badges_when(int i)
{
    if (i < 0 || i >= PV_N_BADGES)
        return 0;
    return when[i];
}
