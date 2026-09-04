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
 * index will hold, so it has to be writable exactly as it stands. The links
 * below are row indices for the same reason.
 *
 * A row is identified by its name AND its artist, not by its name alone.
 * Keying on the name by itself makes two different songs that share a title
 * one row with their plays summed, does the same to two albums that share a
 * name, and leaves nothing able to say which artist a song belongs to. */
struct pv_agg
{
    int           count;      /* plays */
    unsigned      seconds;    /* listened, summed */
    int           skips;
    int           night;      /* plays between midnight and 05:00 */
    unsigned long last_ts;    /* most recent play; 0 if never dated */
    /* The artist row a title or an album belongs to. PV_ROW_NONE on an artist
     * row, and on any row whose artist could not be recorded because the
     * artist table was full. */
    int           artist;
    /* The album row a title belongs to. PV_ROW_NONE on anything else. */
    int           album;
    /* The same three figures again, counting only the year being shown.
     *
     * Both sets, rather than one scoped set, because the badge engine reads
     * these rows for its running profile and badges are for a lifetime: score
     * the rows by year and a badge earned in one year stops being earned in
     * the next. The tiles and the rankings read the y_ fields; pv_badges.c
     * reads the plain ones and needs no year to exist at all. */
    int           y_count;
    unsigned      y_seconds;
    int           y_skips;
    /* The artwork cache's key for this row's folder -- the album's own for an
     * album, its parent for an artist. Captured the first time the row is
     * seen, because the logged path is the only place it can come from and
     * the log is not read again. 0 means it could not be resolved, which is
     * also what the cache uses for "no folder". */
    unsigned int  art_hash;
    char          name[PV_NAME_MAX];
};

/* A day with at least one play.
 *
 * Skips are counted here too, but only on a day that already has a play: a
 * day you only skipped through is not a listening day, and letting one into
 * this array would change what every reader of it means by a day -- the
 * streak, the heat map and the badge thresholds all take a row here as one. */
struct pv_day
{
    long     day;             /* unix day: ts / 86400 */
    int      count;
    unsigned secs;
    int      skips;
    /* Where this day's first entry sits in the log. Recorded during the parse
     * because that pass is already visiting every line in order, and it turns
     * any later question about a date range from a read of the whole history
     * into a seek -- which is what the week drill-down needs and would
     * otherwise have to walk a megabyte for. */
    unsigned long offset;
};

/* No such row: an artist that could not be recorded, or a link that does not
 * apply to this kind of row. */
#define PV_ROW_NONE (-1)

enum pv_table
{
    PV_T_ARTIST,
    PV_T_TITLE,
    PV_T_ALBUM
};

/* What "most played" counts: times started, or time listened.
 *
 * A mode rather than an argument, because it has to be the same answer
 * everywhere at once. A row ranked by minutes whose progress bar is drawn
 * from plays, or whose "top album" was picked by plays, contradicts the order
 * it is standing in -- and the contradiction is invisible until somebody
 * counts. Set it once, before ranking anything. */
void pv_stats_rank_by_time(bool on);

/* What that mode weighs a row at, for a caller drawing one row against
 * another. The unit differs between modes and is not meant to be shown: it is
 * for comparing two rows, and only ever with each other. */
long pv_stats_weight(const struct pv_agg *row);

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

/* Every entry is read; 'out' reports the calendar year asked for.
 *
 * PV_YEAR_ALL is every entry there is, which is what a caller wants when it
 * is describing the log rather than a year of listening.
 *
 * The whole log is always walked, whatever the year: the badge engine has to
 * see every entry, the year on offer comes from the log's full span, and
 * skipping entries would save the read but not the parse. What the year
 * changes is which entries reach 'out' and the rows' y_ fields.
 *
 * Says nothing on screen about failing -- the caller knows how it wants to
 * report. It may still put a progress splash up while the database is being
 * swept, which on a spinning disk is the difference between slow and hung. */
#define PV_YEAR_ALL 0
enum pv_build_result pv_stats_build(void *buf, size_t bufsz,
                                    struct pv_totals *out, int year);

/* The same figures over the whole log, whatever year was asked for. This is
 * what the badges were scored against, and what the year switch offers from;
 * a tile wants 'out' instead. */
const struct pv_totals *pv_stats_lifetime(void);

/* The first and last calendar year the log has a dated play in, so a caller
 * knows what there is to switch between. Both zero when nothing is dated. */
void pv_stats_year_span(int *first, int *last);

/* Which year the last build reported, or PV_YEAR_ALL. */
int pv_stats_year(void);

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

/* One Monday-aligned week of the log.
 *
 * Everything here is summed from the day array rather than kept as the log is
 * read: a week is a range of days, and the days are already there. What is
 * NOT here is the week's top artist, top song and distinct songs -- those need
 * the entries themselves, and `pv_day.offset` is what a reader seeks to for
 * them. They are wanted only for a week the user has opened, which is one
 * bounded scan rather than one per card. */
struct pv_week
{
    long     start_day;       /* unix day of this week's Monday */
    unsigned secs;
    int      plays;
    int      skips;
    unsigned day_secs[7];     /* Monday..Sunday */
    int      best_day;        /* 0..6, or -1 when the week had nothing */
};

/* How many weeks the log spans, first play to last. Zero when nothing in it
 * carried a usable date. */
int pv_stats_week_count(void);

/* Week 'i' of that span, oldest first. False when 'i' is out of range. */
bool pv_stats_week(int i, struct pv_week *out);

/* Weeks of that span that included at least one play -- which is not the
 * same as the span, because a week may be empty. */
int pv_stats_active_weeks(void);

/* The day with the most listening on it. False when there is none. */
bool pv_stats_longest_day(struct pv_day *out);

/* The longest run of consecutive listening days: where it began, where it
 * ended, and the seconds in it. False when there is none. */
bool pv_stats_streak_span(long *from_day, long *to_day, unsigned *secs);

/* A whole table, for a caller that needs the best of something rather than
 * the top few -- ranking would sort repeatedly for answers one pass gives. */
const struct pv_agg *pv_stats_rows(enum pv_table table, int *count);

/* One row by name within an artist, or NULL. For asking what a play just did
 * to its artist's or title's running total, which only makes sense mid-pass.
 *
 * 'artist' is the row index a title or album belongs to, and is ignored for
 * PV_T_ARTIST. PV_ROW_NONE finds only rows whose artist was likewise unknown,
 * not any row of that name. */
const struct pv_agg *pv_stats_find(enum pv_table table, const char *name,
                                   int artist);

/* A row's own index, for asking the questions below about it. PV_ROW_NONE if
 * the row does not belong to that table. */
int pv_stats_index(enum pv_table table, const struct pv_agg *row);

/* One row by index, or NULL. This is how a title reaches its artist's name
 * and an album reaches its artist's. */
const struct pv_agg *pv_stats_row(enum pv_table table, int idx);

/* How many rows of 'child' name row 'idx' of 'parent' as theirs -- the
 * distinct songs of an artist or of an album.
 *
 * Walked rather than counted during the build: the answer is wanted for a
 * handful of rows, and keeping it would cost every row four bytes against
 * capacities already sized to the library. */
int pv_stats_child_count(enum pv_table child, enum pv_table parent, int idx);

/* The most-played row of 'child' naming row 'idx' of 'parent' as theirs, or
 * NULL -- an artist's most-listened album or song, an album's most-listened
 * song. */
const struct pv_agg *pv_stats_best_child(enum pv_table child,
                                         enum pv_table parent, int idx);

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
