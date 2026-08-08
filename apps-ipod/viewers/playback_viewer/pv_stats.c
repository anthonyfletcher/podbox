/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * What the log adds up to.
 *
 * One pass over the family (pv_log.c), naming each entry as it arrives
 * (pv_names.c) and folding it into three tables -- artists, titles and
 * albums -- plus a day array and an hour histogram. Everything the cards
 * eventually draw is derived from these.
 *
 * The tables live in memory the caller lends us and are gone when this
 * returns; only the summary survives. That is deliberate: the aggregates are
 * the expensive part to produce and the cheap part to keep, so the eventual
 * on-disk index (see .specifications/SPUN_INTEGRATION.md) replaces this
 * working set without changing what a record means. Which is why a record is
 * plain data with no pointers in it -- it is already the shape it needs to be
 * to be written to a file.
 *
 * A note on albums. A path does not carry one, so an album name can only come
 * from the database. Without one, plays are bucketed under the ARTIST
 * instead: the numbers stay meaningful and the album card degrades to an
 * artist card, which is better than a table full of folder names.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <string-extra.h>
#include <file.h>            /* MAX_PATH */
#include "config.h"
#include "kernel.h"      /* current_tick, HZ */
#include "system.h"      /* cpu_boost */
#ifdef HAVE_ALBUMART
#include "metadata/art_cache.h"
#endif
#include "pv_badges.h"
#include "pv_index.h"
#include "pv_log.h"
#include "pv_names.h"
#include "pv_stats.h"

struct pv_htable
{
    struct pv_agg *items;
    int             *slots;   /* slot -> item index + 1; 0 = empty */
    int              cap;
    int              mask;    /* slots - 1, always a power of two */
    int              n;
};

/* Weeks are Monday-aligned and absolute: unix day 4 was Mon 1970-01-05. Being
 * independent of any displayed year means a week keeps its identity however
 * the deck is later filtered. */
#define WEEK_OF_DAY(d) (((d) - 4) / 7)
#define WEEK_START_DAY(w) ((w) * 7 + 4)

static struct pv_htable t_artist, t_title, t_album;
static struct pv_day   *days;
static int day_cap, day_n;
static bool overflowed;

/* Plays older than this count as "not heard lately" for PV_RANK_REDIS.
 * Derived from the log's own span at the end of a build. */
static unsigned long redis_cutoff;

/* The badge engine's chronological state, built as the log is read and
 * carried in the index so an extend continues it rather than restarting. */
static struct pv_badge_state badge_state;

/* Bump allocator over the caller's buffer, above whatever the name map took. */
static char  *abuf;
static size_t abuf_sz, abuf_used;

static void *abuf_alloc(size_t n)
{
    void *p;

    n = (n + 3u) & ~(size_t)3u;
    if (abuf_used + n > abuf_sz)
        return NULL;
    p = abuf + abuf_used;
    abuf_used += n;
    memset(p, 0, n);
    return p;
}

static int next_pow2(int v)
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

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

static bool htable_init(struct pv_htable *t, int cap)
{
    int slots = next_pow2(cap * 2);

    t->items = abuf_alloc((size_t)cap * sizeof(struct pv_agg));
    t->slots = abuf_alloc((size_t)slots * sizeof(int));
    if (!t->items || !t->slots)
        return false;

    t->cap  = cap;
    t->mask = slots - 1;
    t->n    = 0;
    return true;
}

/* Find or create. NULL only when the table is full -- which is a real
 * outcome, not an impossible one: the caller records it so an undercount is
 * visible rather than silent. */
static struct pv_agg *htable_get(struct pv_htable *t, const char *name)
{
    /* Hash and compare the STORED form. A longer name hashed in full would
     * never match its own truncated entry, and would make a new row on every
     * single play. */
    char key[PV_NAME_MAX];
    unsigned int h;
    int i;

    strlcpy(key, name, sizeof(key));
    h = hash_str(key);
    i = (int)(h & (unsigned)t->mask);

    while (t->slots[i])
    {
        struct pv_agg *a = &t->items[t->slots[i] - 1];
        if (strcmp(a->name, key) == 0)
            return a;
        i = (i + 1) & t->mask;
    }

    if (t->n >= t->cap)
    {
        overflowed = true;
        return NULL;
    }

    struct pv_agg *a = &t->items[t->n];
    strlcpy(a->name, key, sizeof(a->name));
    t->slots[i] = t->n + 1;
    t->n++;
    return a;
}

/* Logs run forwards, so the day being added is almost always the last one
 * seen. The scan behind it is the exception, not the rule. */
static void day_add(long day, unsigned secs, unsigned long offset)
{
    if (day_n && days[day_n - 1].day == day)
    {
        days[day_n - 1].count++;
        days[day_n - 1].secs += secs;
        return;
    }

    for (int i = 0; i < day_n; i++)
    {
        if (days[i].day == day)
        {
            days[i].count++;
            days[i].secs += secs;
            return;
        }
    }

    if (day_n < day_cap)
    {
        days[day_n].day = day;
        days[day_n].count = 1;
        days[day_n].secs = secs;
        /* The first entry seen for this day, which is where a later reader
         * should start looking for it. */
        days[day_n].offset = offset;
        day_n++;
    }
    else
    {
        overflowed = true;
    }
}

#ifdef HAVE_ALBUMART
/* The artwork cache's key for the folder holding 'path', or for that folder's
 * parent when 'up' is set -- which under an <artist>/<album>/<track> layout is
 * the artist's own folder, and is exactly how art_cache generates its keys
 * (see aa_dirname() in metadata/art_cache.c).
 *
 * The path is hashed with no normalisation and no trailing slash, because
 * that is what the cache does. Anything else here silently misses every
 * lookup. */
static unsigned int folder_hash(const char *path, bool up)
{
    char dir[MAX_PATH];
    const char *slash;
    size_t n;

    if (!path || !path[0])
        return 0;

    slash = strrchr(path, '/');
    if (!slash || slash == path)
        return 0;
    n = (size_t)(slash - path);

    if (up)
    {
        /* Step to the parent by finding the slash before this one. */
        while (n > 0 && path[n - 1] != '/')
            n--;
        if (n <= 1)
            return 0;       /* no distinct parent: a flat or rooted layout */
        n--;
    }

    if (n == 0 || n >= sizeof(dir))
        return 0;

    memcpy(dir, path, n);
    dir[n] = '\0';
    return art_cache_dir_hash(dir);
}
#endif /* HAVE_ALBUMART */

/* Below this much unindexed log, replaying it beats saving it: writing the
 * index costs about what reading it does, and a day's listening is a few
 * kilobytes against a quarter of a megabyte of tables. */
#define PV_INDEX_REWRITE_AT (64 * 1024)

/* The slot arrays are derived, not stored: rebuilding them from the records
 * is a few thousand hash inserts, which is quicker than reading them back and
 * leaves that much less file to be wrong. */
static void htable_reindex(struct pv_htable *t)
{
    memset(t->slots, 0, (size_t)(t->mask + 1) * sizeof(int));
    for (int i = 0; i < t->n; i++)
    {
        int s = (int)(hash_str(t->items[i].name) & (unsigned)t->mask);
        while (t->slots[s])
            s = (s + 1) & t->mask;
        t->slots[s] = i + 1;
    }
}

static void index_identity(struct pv_index_id *id, enum pv_source src)
{
    id->source      = (unsigned long)src;
    id->agg_size    = sizeof(struct pv_agg);
    id->day_size    = sizeof(struct pv_day);
    id->totals_size = sizeof(struct pv_totals);
    id->state_size  = sizeof(struct pv_badge_state);
}

/* Read a saved index into the tables, which the caller has already sized.
 * Anything that does not fit or does not read whole means no index. */
static bool index_load(struct pv_totals *out, unsigned long log_size,
                       unsigned long *covered)
{
    struct pv_index_id id;
    struct pv_totals saved;
    int n_artist, n_title, n_album, n_days;

    index_identity(&id, out->source);
    if (!pv_index_read_begin(&id, log_size, covered))
        return false;

    if (!pv_index_read(&saved, sizeof(saved))
        || !pv_index_read(&n_artist, sizeof(n_artist))
        || !pv_index_read(&n_title, sizeof(n_title))
        || !pv_index_read(&n_album, sizeof(n_album))
        || !pv_index_read(&n_days, sizeof(n_days))
        || !pv_index_read(&badge_state, sizeof(badge_state)))
        goto fail;

    /* A library that grew since the index was written can have more rows than
     * the tables were sized for. Rebuilding is the right answer -- loading
     * what fits would silently drop the rest. */
    if (n_artist > t_artist.cap || n_title > t_title.cap
        || n_album > t_album.cap || n_days > day_cap
        || n_artist < 0 || n_title < 0 || n_album < 0 || n_days < 0)
        goto fail;

    if (!pv_index_read(t_artist.items, (size_t)n_artist * sizeof(struct pv_agg))
        || !pv_index_read(t_title.items, (size_t)n_title * sizeof(struct pv_agg))
        || !pv_index_read(t_album.items, (size_t)n_album * sizeof(struct pv_agg))
        || !pv_index_read(days, (size_t)n_days * sizeof(struct pv_day)))
        goto fail;

    pv_index_read_end();

    t_artist.n = n_artist;
    t_title.n  = n_title;
    t_album.n  = n_album;
    day_n      = n_days;

    htable_reindex(&t_artist);
    htable_reindex(&t_title);
    htable_reindex(&t_album);

    /* Carry the accumulated figures across, but not the ones that describe
     * this run: how long it took, and which capacities it was given. */
    {
        long ms_names = out->ms_names;
        bool swept = out->names_swept;
        int ct = out->cap_titles, ca = out->cap_artists, cb = out->cap_albums;
        int de = out->db_entries, dm = out->db_mapped;
        enum pv_source src = out->source;

        *out = saved;
        out->ms_names = ms_names;
        out->names_swept = swept;
        out->cap_titles = ct;
        out->cap_artists = ca;
        out->cap_albums = cb;
        out->db_entries = de;
        out->db_mapped = dm;
        out->source = src;
        out->ms_read = 0;
    }
    return true;

fail:
    pv_index_read_end();
    day_n = 0;
    t_artist.n = t_title.n = t_album.n = 0;
    return false;
}

static void index_save(const struct pv_totals *out, unsigned long covered)
{
    struct pv_index_id id;

    index_identity(&id, out->source);
    if (!pv_index_write_begin(&id, covered, out->source))
        return;

    if (pv_index_write(out, sizeof(*out))
        && pv_index_write(&t_artist.n, sizeof(t_artist.n))
        && pv_index_write(&t_title.n, sizeof(t_title.n))
        && pv_index_write(&t_album.n, sizeof(t_album.n))
        && pv_index_write(&day_n, sizeof(day_n))
        && pv_index_write(&badge_state, sizeof(badge_state))
        && pv_index_write(t_artist.items,
                          (size_t)t_artist.n * sizeof(struct pv_agg))
        && pv_index_write(t_title.items,
                          (size_t)t_title.n * sizeof(struct pv_agg))
        && pv_index_write(t_album.items,
                          (size_t)t_album.n * sizeof(struct pv_agg))
        && pv_index_write(days, (size_t)day_n * sizeof(struct pv_day)))
        pv_index_write_end();
    else
        pv_index_write_abort();
}

static void entry_cb(const struct pv_entry *e, void *ctx)
{
    struct pv_totals *t = ctx;
    char artist[PV_NAME_MAX], title[PV_NAME_MAX], album[PV_NAME_MAX];
    enum pv_name_src src;
    unsigned elapsed;
    bool night;
    struct pv_agg *a;

    if (e->artist)
    {
        /* The scrobbler format carries tagged names already, so there is
         * nothing to resolve and no database to consult. */
        strlcpy(artist, e->artist, sizeof(artist));
        strlcpy(title, e->title, sizeof(title));
        strlcpy(album, e->album ? e->album : "", sizeof(album));
        src = PV_NAME_LOG;
        t->from_log++;
    }
    else
    {
        src = pv_names_resolve(e->path, artist, title, album);
        if (src == PV_NAME_DB)
            t->from_db++;
        else
            t->from_path++;
    }

    elapsed = (unsigned)(e->elapsed_ms / 1000);

    /* Counted here rather than from the reader's return value, so that a pass
     * over only the new tail of the log adds to what was loaded instead of
     * replacing it. Every counter in this function accumulates for the same
     * reason -- extending is the same arithmetic as building, applied to
     * fewer entries. */
    t->lines++;

    if (!e->valid_ts)
        t->unset_clock++;

    /* Keep the first few properly-named entries, so "the names are real" is
     * something that can be seen rather than inferred from a counter. */
    if (src != PV_NAME_PATH
        && t->samples < (int)(sizeof(t->sample) / sizeof(t->sample[0])))
    {
        snprintf(t->sample[t->samples], sizeof(t->sample[0]),
                 "%s - %s", artist, title);
        t->samples++;
    }

    if (e->listened)
    {
        t->plays++;
        t->seconds += elapsed;

        night = e->valid_ts && (e->ts % 86400UL) / 3600UL < 5;
        if (night)
            t->night++;

        a = htable_get(&t_artist, artist);
        if (a)
        {
            a->count++;
            a->seconds += elapsed;
            if (night)
                a->night++;
#ifdef HAVE_ALBUMART
            if (a->art_hash == 0 && e->path && e->path[0])
                a->art_hash = folder_hash(e->path, true);
#endif
        }

        a = htable_get(&t_title, title);
        if (a)
        {
            a->count++;
            a->seconds += elapsed;
            if (night)
                a->night++;
            if (e->valid_ts && e->ts > a->last_ts)
                a->last_ts = e->ts;
        }

        a = htable_get(&t_album, album[0] ? album : artist);
        if (a)
        {
            a->count++;
            a->seconds += elapsed;
#ifdef HAVE_ALBUMART
            if (a->art_hash == 0 && e->path && e->path[0])
                a->art_hash = folder_hash(e->path, false);
#endif
        }

        if (e->valid_ts)
        {
            if (!t->ts_min || e->ts < t->ts_min)
                t->ts_min = e->ts;
            if (e->ts > t->ts_max)
                t->ts_max = e->ts;
            t->hour_hist[(e->ts % 86400UL) / 3600UL]++;
            day_add((long)(e->ts / 86400UL), elapsed, e->offset);
        }
    }
    else if (e->skipped)
    {
        t->skips++;
        a = htable_get(&t_title, title);
        if (a)
            a->skips++;
    }
    else
    {
        t->taps++;
        return;         /* a browsing tap is not a play and not a skip */
    }

    /* Fed last, and only for entries that counted as one or the other: the
     * running figures it keeps are about what you listened to, and it reads
     * the tables above to see where this play left its artist and title. */
    pv_badges_feed(&badge_state, e->ts, e->valid_ts, e->listened, elapsed,
                   artist, title);
}

/* Days arrive in log order, which is nearly sorted but not guaranteed to be
 * -- a log written across a clock change is not. Insertion sort costs almost
 * nothing on nearly-ordered data and makes the run below correct rather than
 * usually correct. */
static void days_sort(void)
{
    for (int i = 1; i < day_n; i++)
    {
        struct pv_day key = days[i];
        int j = i - 1;

        while (j >= 0 && days[j].day > key.day)
        {
            days[j + 1] = days[j];
            j--;
        }
        days[j + 1] = key;
    }
}

static int longest_streak(void)
{
    int best = 0, run = 0;

    for (int i = 0; i < day_n; i++)
    {
        run = (i > 0 && days[i].day == days[i - 1].day + 1) ? run + 1 : 1;
        if (run > best)
            best = run;
    }
    return best;
}

/* A week's listening is the sum of its days', so it costs one pass over the
 * day array rather than a bin of its own during the read. */
static void best_week(struct pv_totals *t)
{
    long cur_week = 0;
    unsigned cur_secs = 0;

    for (int i = 0; i < day_n; i++)
    {
        long w = WEEK_OF_DAY(days[i].day);

        if (i == 0 || w != cur_week)
        {
            cur_week = w;
            cur_secs = 0;
        }
        cur_secs += days[i].secs;

        if ((long)cur_secs > t->best_week_secs)
        {
            t->best_week_secs = (long)cur_secs;
            t->best_week_day  = WEEK_START_DAY(w);
        }
    }
}

/* ------------------------------------------------------------- calendar */

const char *const pv_month_abbr[13] =
{
    "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* Howard Hinnant's civil-from-days, which treats March as the first month so
 * the leap day lands at the end of a year and the arithmetic stays branchless.
 * Kept in its published form: it is easy to verify against the original and
 * horrible to re-derive. */
void pv_civil_from_days(long z, int *y, int *m, int *d)
{
    long era, doe, yoe, yr, doy, mp;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yr  = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;

    if (d)
        *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    if (m)
        *m = (int)(mp + (mp < 10 ? 3 : -9));
    if (y)
        *y = (int)(yr + ((mp + (mp < 10 ? 3 : -9)) <= 2 ? 1 : 0));
}

long pv_days_from_civil(int y, int m, int d)
{
    long era, yoe, doy, doe;

    y -= (m <= 2) ? 1 : 0;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* ------------------------------------------------------------- querying */

static struct pv_htable *table_of(enum pv_table which)
{
    switch (which)
    {
    case PV_T_ARTIST: return &t_artist;
    case PV_T_ALBUM:  return &t_album;
    default:          return &t_title;
    }
}

/* What a row scores under a given order. Zero or less means it does not
 * belong in that list at all -- a track with no skips has no business on the
 * skip card, and saying "0" there would be worse than saying nothing. */
static int agg_metric(const struct pv_agg *a, enum pv_rank rank)
{
    switch (rank)
    {
    case PV_RANK_SKIPS: return a->skips;
    case PV_RANK_LOYAL: return (a->skips == 0) ? a->count : 0;
    case PV_RANK_REDIS: return (a->last_ts && a->last_ts <= redis_cutoff)
                               ? a->count : 0;
    case PV_RANK_NIGHT: return a->night;
    default:            return a->count;
    }
}

int pv_stats_top(enum pv_table which, enum pv_rank rank,
                 const struct pv_agg **out, int max)
{
    struct pv_htable *t = table_of(which);
    int got = 0;

    if (!t->items)
        return 0;

    /* Selection sort, one pass per row wanted. Five rows out of a few
     * thousand is far cheaper than ordering the whole table, and the table is
     * never wanted in order for its own sake. */
    for (int k = 0; k < max; k++)
    {
        int best = -1, best_val = 0;

        for (int i = 0; i < t->n; i++)
        {
            struct pv_agg *a = &t->items[i];
            int v = agg_metric(a, rank);
            bool taken = false;

            if (v <= 0)
                continue;
            for (int j = 0; j < got; j++)
            {
                if (out[j] == a)
                {
                    taken = true;
                    break;
                }
            }
            if (taken)
                continue;

            if (best < 0 || v > best_val)
            {
                best = i;
                best_val = v;
            }
        }

        if (best < 0)
            break;
        out[got++] = &t->items[best];
    }

    return got;
}

const struct pv_agg *pv_stats_rows(enum pv_table which, int *count)
{
    struct pv_htable *t = table_of(which);

    if (count)
        *count = t->n;
    return t->items;
}

const struct pv_agg *pv_stats_find(enum pv_table which, const char *name)
{
    struct pv_htable *t = table_of(which);
    char key[PV_NAME_MAX];
    unsigned int h;
    int i;

    if (!t->items || !t->slots)
        return NULL;

    /* The stored form, as htable_get() uses -- a longer name would hash
     * differently from its own truncated row and never be found. */
    strlcpy(key, name, sizeof(key));
    h = hash_str(key);
    i = (int)(h & (unsigned)t->mask);

    while (t->slots[i])
    {
        struct pv_agg *a = &t->items[t->slots[i] - 1];
        if (strcmp(a->name, key) == 0)
            return a;
        i = (i + 1) & t->mask;
    }
    return NULL;
}

const struct pv_day *pv_stats_days(int *count)
{
    if (count)
        *count = day_n;
    return days;
}

static enum pv_build_result build_body(void *buf, size_t bufsz,
                                       struct pv_totals *out)
{
    size_t names_used;
    int cap_title, cap_artist, cap_album;
    long lines;
    unsigned long save_covered = 0;
    bool save_wanted = false;

    memset(out, 0, sizeof(*out));
    overflowed = false;
    day_n = 0;

    /* Decide the source before anything else: a scrobbler log names its own
     * entries, so the database sweep -- much the most expensive part of a
     * first run -- is not merely unnecessary but meaningless, there being no
     * paths for it to resolve. */
    {
        long t0 = current_tick;
        out->source = pv_log_pick_source();
        out->ms_pick = (current_tick - t0) * 1000 / HZ;
    }
    if (out->source == PV_SRC_NONE)
        return PV_BUILD_NO_LOG;

    /* The name map claims the bottom of the buffer; the tables live above it
     * and never move, so the two never have to know about each other. */
    if (out->source == PV_SRC_PLAYBACK)
    {
        long t0 = current_tick;

        names_used = pv_names_init(buf, bufsz);
        out->ms_names = (current_tick - t0) * 1000 / HZ;
        pv_names_info(&out->db_entries, &out->db_mapped, &out->names_swept);
    }
    else
    {
        /* Not merely zero for tidiness: pv_names_info() would otherwise hand
         * back whatever a previous crunch left behind, which would both
         * misreport on screen and mis-size the tables below. */
        names_used = 0;
        out->db_entries = out->db_mapped = 0;
    }

    abuf      = (char *)buf + names_used;
    abuf_sz   = bufsz - names_used;
    abuf_used = 0;

    /* Size the tables to the library rather than to a guess. The database
     * knows how many files there are, and a track table can never need more
     * rows than that; artists and albums are a fraction of it. Guessing
     * instead -- the same 1500 rows for artists as for albums -- spent most
     * of the buffer on rows a real library never fills while the track table
     * ran out, which is the one that matters. */
    cap_title = out->db_entries > 0 ? out->db_entries : 4000;
    cap_artist = cap_title / 8;
    cap_album  = cap_title / 4;
    if (cap_artist < 200)
        cap_artist = 200;
    if (cap_album < 200)
        cap_album = 200;

    /* Give ground in order of what is least missed. The 5G has 512 KB of
     * scratch against the 6G's 3 MB and can be a few kilobytes short on a
     * large library, so what gets sacrificed matters: shrinking every table
     * together costs the track table -- the one the deck is mostly made of --
     * to buy room for artist rows a library will never fill.
     *
     * Days go first (1024 of them is nearly three years of daily listening,
     * and they feed only the heatmap and the streak), then albums, then
     * artists. The track table is cut last and only when nothing else is
     * left, and a table that then fills up is reported rather than hidden. */
    day_cap = 1500;
    for (int step = 0; ; step++)
    {
        size_t need;

        need  = (size_t)cap_title * sizeof(struct pv_agg)
              + (size_t)next_pow2(cap_title * 2) * sizeof(int);
        need += (size_t)cap_artist * sizeof(struct pv_agg)
              + (size_t)next_pow2(cap_artist * 2) * sizeof(int);
        need += (size_t)cap_album * sizeof(struct pv_agg)
              + (size_t)next_pow2(cap_album * 2) * sizeof(int);
        need += (size_t)day_cap * sizeof(struct pv_day);

        if (need <= abuf_sz)
            break;

        if (step == 0)
            day_cap = 1024;
        else if (step == 1)
            day_cap = 512;
        else if (step == 2 && cap_album > 200)
            cap_album /= 2;
        else if (step == 3 && cap_artist > 200)
            cap_artist /= 2;
        else if (cap_title > 200)
            cap_title /= 2;
        else
            break;      /* as small as it goes; overflow will say so */
    }

    long t_alloc = current_tick;

    pv_badges_reset(&badge_state);

    memset(&t_artist, 0, sizeof(t_artist));
    memset(&t_title, 0, sizeof(t_title));
    memset(&t_album, 0, sizeof(t_album));

    if (!htable_init(&t_title, cap_title)
        || !htable_init(&t_artist, cap_artist)
        || !htable_init(&t_album, cap_album))
        return PV_BUILD_NO_MEMORY;

    days = abuf_alloc((size_t)day_cap * sizeof(struct pv_day));
    if (!days)
        return PV_BUILD_NO_MEMORY;

    out->cap_titles  = cap_title;
    out->cap_artists = cap_artist;
    out->cap_albums  = cap_album;
    out->ms_alloc = (current_tick - t_alloc) * 1000 / HZ;

    {
        long t0 = current_tick;
        unsigned long log_size = pv_log_size(out->source);
        unsigned long covered = 0;

        /* Three ways in, in order of what they cost. Each falls through to
         * the next, so a saved index that cannot be trusted is simply not
         * used -- there is no repair path and no half-loaded state. */
        if (index_load(out, log_size, &covered) && covered <= log_size)
        {
            out->from_index = true;
            lines = (covered < log_size)
                  ? pv_log_read_range(out->source, covered, 0, entry_cb, out)
                  : 0;
            out->ms_read = (current_tick - t0) * 1000 / HZ;

            /* Rewrite only once the unindexed tail is worth the write, which
             * costs as much as the read. Below that it is cheaper to replay
             * the same few kilobytes next time than to save them. */
            save_wanted = (log_size - covered >= PV_INDEX_REWRITE_AT);
        }
        else
        {
            lines = pv_log_read(out->source, entry_cb, out);
            out->ms_read = (current_tick - t0) * 1000 / HZ;
            save_wanted = (lines >= 0);
        }

        if (lines < 0)
            return PV_BUILD_NO_LOG;

        save_covered = log_size;
    }

    /* Titles with no play at all are skip-only rows: real entries, and part
     * of the table, but not something the deck would ever call a track you
     * listened to. Count what was actually heard. */
    out->titles = 0;
    for (int i = 0; i < t_title.n; i++)
    {
        if (t_title.items[i].count > 0)
            out->titles++;
    }
    out->artists = t_artist.n;
    out->albums  = t_album.n;

    long t_post = current_tick;

    days_sort();
    out->days   = day_n;
    out->streak = longest_streak();
    best_week(out);

    /* "Not heard in a while" is a third of however long the log covers,
     * held between two and six weeks. A fixed window would mean the
     * rediscovery card had nothing to say about a month-old log and far too
     * much to say about a five-year-old one. */
    {
        unsigned long span = (out->ts_min && out->ts_max > out->ts_min)
                           ? out->ts_max - out->ts_min : 0;
        unsigned long gap = span / 3;

        if (gap < 14UL * 86400)
            gap = 14UL * 86400;
        if (gap > 45UL * 86400)
            gap = 45UL * 86400;

        redis_cutoff = (out->ts_max > gap) ? out->ts_max - gap : 0;
        out->redis_days = (int)(gap / 86400);
    }

    out->ms_post = (current_tick - t_post) * 1000 / HZ;
    out->overflowed = overflowed;

    out->badges_unlocked = pv_badges_eval(&badge_state, out);
    out->badges_total = pv_badges_count();

    /* Saved here rather than the moment the log was read, so the file holds a
     * complete snapshot. Written any earlier and the derived figures -- the
     * unique counts, the streak, the heaviest week -- would all go to disk as
     * zero, which works only because they happen to be recomputed on load and
     * would quietly stop working the day something trusted them. */
    if (save_wanted)
        index_save(out, save_covered);
    return PV_BUILD_OK;
}

enum pv_build_result pv_stats_build(void *buf, size_t bufsz,
                                    struct pv_totals *out)
{
    enum pv_build_result r;

    /* Reading a megabyte and hashing ten thousand entries is exactly the
     * bounded, user-is-waiting work the core boosts for everywhere else it
     * touches the database (tagcache.c, db_summary.c). Unboosted this runs at
     * 30 MHz of the 5G's 80, and 54 of the 6G's 216. */
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(true);
#endif

    r = build_body(buf, bufsz, out);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(false);
#endif

    return r;
}
