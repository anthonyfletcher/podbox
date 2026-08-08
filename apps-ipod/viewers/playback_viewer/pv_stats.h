/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Interface to pv_stats.c.
 ****************************************************************************/
#ifndef _PV_STATS_H
#define _PV_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include "pv_log.h"
#include "pv_names.h"

/* One row of an aggregate table -- an artist, a title or an album, with what
 * the log says about it.
 *
 * Fixed size and free of pointers on purpose: this is the record the on-disk
 * index will hold, so it has to be writable exactly as it stands. */
struct pv_agg
{
    int           count;      /* plays */
    unsigned      seconds;    /* listened, summed */
    int           skips;
    int           night;      /* plays between midnight and 05:00 */
    unsigned long last_ts;    /* most recent play; 0 if never dated */
    /* The artwork cache's key for this row's folder -- the album's own for an
     * album, its parent for an artist. Captured the first time the row is
     * seen, because the logged path is the only place it can come from and
     * the log is not read again. 0 means it could not be resolved, which is
     * also what the cache uses for "no folder". */
    unsigned int  art_hash;
    char          name[PV_NAME_MAX];
};

/* A day with at least one play. */
struct pv_day
{
    long     day;             /* unix day: ts / 86400 */
    int      count;
    unsigned secs;
    /* Where this day's first entry sits in the log. Recorded during the parse
     * because that pass is already visiting every line in order, and it turns
     * any later question about a date range from a read of the whole history
     * into a seek -- which is what the week drill-down needs and would
     * otherwise have to walk a megabyte for. */
    unsigned long offset;
};

enum pv_table
{
    PV_T_ARTIST,
    PV_T_TITLE,
    PV_T_ALBUM
};

/* How a table is ordered for a card. Each is "best first" by its own measure,
 * and rows that score zero are excluded rather than shown at one end. */
enum pv_rank
{
    PV_RANK_PLAYS,   /* most played */
    PV_RANK_SKIPS,   /* most skipped */
    PV_RANK_LOYAL,   /* most played, among those never skipped */
    PV_RANK_REDIS,   /* most played, among those not heard lately */
    PV_RANK_NIGHT    /* most played between midnight and 05:00 */
};

/* Everything one crunch of the log produced. Small and self-contained, so a
 * caller can keep it after the working memory has been handed back. */
struct pv_totals
{
    /* Which log these came from. Worth showing rather than assuming: the two
     * formats measure "minutes" and "skips" differently (see enum pv_source),
     * so a figure only means something once you know its source. */
    enum pv_source source;

    /* what the log held */
    long lines;         /* data lines: every parseable non-header line */
    long plays;         /* of those, the ones that counted as listening */
    long skips;         /* started, given up on after more than a tap */
    long taps;          /* browsing: counted as a line and nothing else */
    long seconds;       /* listened, summed */
    long night;         /* plays between midnight and 05:00 */
    long unset_clock;   /* entries logged before the clock was set */

    unsigned long ts_min, ts_max;

    /* what was in it */
    int artists, titles, albums;
    int days;           /* distinct days with at least one play */
    int streak;         /* longest run of consecutive such days */
    int hour_hist[24];

    /* The heaviest Monday-Sunday week, which is what the week tiers are
     * measured against (SUPERWEEK starts at 1440 minutes). */
    long best_week_secs;
    long best_week_day;   /* unix day of that week's Monday */

    /* How long "not heard lately" is, for PV_RANK_REDIS. Scaled to the log's
     * own span so the card works for a month of history as well as a year. */
    int  redis_days;

    /* Where the names came from. With a playback log they are resolved from
     * the path, and from_db being 0 on a device that has a database means the
     * path forms disagree -- artwork, keyed off the same strings, will miss
     * everything too. A scrobbler log carries tagged names itself, so it is
     * all from_log and the other two stay 0. */
    long from_db, from_path, from_log;
    int  db_entries, db_mapped;
    char sample[3][PV_NAME_MAX * 2];
    int  samples;

    /* A table that filled up stopped counting, silently, and every figure
     * above is then an undercount. Capacities are reported so that shows. */
    int  cap_titles, cap_artists, cap_albums;
    bool overflowed;

    /* Where the time went, in milliseconds. Split because the two halves
     * have entirely different fixes: the name map is a file read (or, once,
     * a database sweep), the log pass is reading and hashing every entry.
     * An on-disk index would replace the second and not the first. */
    long ms_names, ms_read;
    /* The phases that are neither: choosing a source, clearing the tables,
     * and the sorting and derived figures afterwards. Instrumented because a
     * loaded index made the other two nearly free and left most of the time
     * unaccounted for. */
    long ms_pick, ms_alloc, ms_post;
    bool names_swept;   /* the map was rebuilt rather than read back */
    bool from_index;    /* loaded from the saved index rather than the log */

    /* Badges: how many of the wall is lit, and how big the wall is. */
    int  badges_unlocked, badges_total;
};

enum pv_build_result
{
    PV_BUILD_OK,
    PV_BUILD_NO_LOG,      /* not one file of the family would open */
    PV_BUILD_NO_MEMORY    /* the buffer could not hold even the small tables */
};

/* Crunch the whole log into 'out', using 'buf' as working memory for the
 * database map and the aggregate tables.
 *
 * Says nothing on screen about failing -- the caller knows how it wants to
 * report. It may still put a progress splash up while the database is being
 * swept, which on a spinning disk is the difference between slow and hung. */
enum pv_build_result pv_stats_build(void *buf, size_t bufsz,
                                        struct pv_totals *out);

/* Reading the model afterwards.
 *
 * Both hand back pointers INTO the buffer the build was given, so they are
 * valid only while the caller still holds it. A screen drawing from them must
 * therefore CLAIM the buffer for its lifetime rather than borrow it
 * transiently -- see system/app_buffer.h. A caller that only wants the
 * totals can hand the buffer straight back instead. */

/* The top 'max' rows of a table under a given order, best first, into out[].
 * Returns how many there were: fewer than asked for is normal, and none is a
 * real answer for a log with no skips in it. */
int pv_stats_top(enum pv_table table, enum pv_rank rank,
                 const struct pv_agg **out, int max);

/* Every day that had a play, oldest first. */
const struct pv_day *pv_stats_days(int *count);

/* A whole table, for a caller that needs the best of something rather than
 * the top few -- ranking would sort repeatedly for answers one pass gives. */
const struct pv_agg *pv_stats_rows(enum pv_table table, int *count);

/* One row by name, or NULL. For asking what a play just did to its artist's
 * or title's running total, which only makes sense mid-pass. */
const struct pv_agg *pv_stats_find(enum pv_table table, const char *name);

/* The calendar, on the model's own time axis.
 *
 * A unix day is what the aggregates are keyed by, and there is no gmtime() in
 * this tree, so the conversion lives here rather than being reinvented by
 * each card that wants to print a date. Proleptic Gregorian, valid either
 * side of the epoch. */
void pv_civil_from_days(long day, int *y, int *m, int *d);
long pv_days_from_civil(int y, int m, int d);

/* "Jan".."Dec"; index 0 is empty so a month number indexes directly. */
extern const char *const pv_month_abbr[13];

#endif /* _PV_STATS_H */
