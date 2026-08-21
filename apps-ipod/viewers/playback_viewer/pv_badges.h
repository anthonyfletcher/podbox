/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/achievements_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Interface to pv_badges.c.
 ****************************************************************************/
#ifndef _PV_BADGES_H
#define _PV_BADGES_H

#include <stdbool.h>

/* What a badge tests. Most come straight off the aggregates; the rest are
 * running figures only a chronological pass can produce, which is what
 * pv_badges_feed() is for. */
enum pv_badge_metric
{
    /* totals */
    PV_AM_MINUTES,
    PV_AM_PLAYS,
    PV_AM_SKIPS,
    PV_AM_UNIQ_TRACKS,
    PV_AM_UNIQ_ARTISTS,
    PV_AM_UNIQ_ALBUMS,
    PV_AM_NIGHT_PLAYS,
    PV_AM_HOURS_COVERED,   /* distinct hours of the day with a play */
    PV_AM_ACTIVE_DAYS,
    PV_AM_STREAK,
    /* bests */
    PV_AM_TRACK_PLAYS,
    PV_AM_ARTIST_PLAYS,
    PV_AM_ALBUM_PLAYS,
    PV_AM_LOYAL_PLAYS,     /* best play count among never-skipped songs */
    PV_AM_TRACK_SKIPS,
    PV_AM_DAY_PLAYS,
    PV_AM_DAY_MINS,
    PV_AM_TRACK_DAY,       /* most plays of one song in one day */
    PV_AM_SKIP_RUN,        /* longest consecutive-skip run */
    PV_AM_PLAY_RUN,        /* longest skip-free run */
    PV_AM_WEEK_MINS,
    PV_AM_SUPERWEEKS,
    PV_AM_ULTRAWEEKS,
    PV_AM_ACTIVE_WEEKS,
    PV_AM_PERFECT_MONTHS,  /* months with every day active */
    /* one-shot date and time flags, 0 or 1 */
    PV_AM_F_NEWYEAR,
    PV_AM_F_XMAS,
    PV_AM_F_HALLOWEEN,
    PV_AM_F_FRIDAY13,
    PV_AM_F_MIDNIGHT,
    PV_AM_F_LEAPDAY,
    /* Listening types. Set as the log is read rather than at the end, so a
     * type you passed through months ago still counts -- collecting them is
     * the point, and only a chronological pass can see them. */
    PV_AM_TYPE0,  PV_AM_TYPE1,  PV_AM_TYPE2,  PV_AM_TYPE3,
    PV_AM_TYPE4,  PV_AM_TYPE5,  PV_AM_TYPE6,  PV_AM_TYPE7,
    PV_AM_TYPE8,  PV_AM_TYPE9,  PV_AM_TYPE10, PV_AM_TYPE11,
    PV_AM_TYPE12, PV_AM_TYPE13, PV_AM_TYPE14, PV_AM_TYPE15,
    PV_AM_TYPES_COLLECTED,
    /* meta */
    PV_AM_META,            /* how many OTHER badges are unlocked */
    PV_AM_COUNT
};

/* A badge the table does not describe until it is earned. */
#define PV_BADGE_SECRET 1

/* The metal an unlocked badge wears. Locked rows never hint at it. */
enum pv_badge_tier
{
    PV_TIER_GOLD = 0,
    PV_TIER_ICE,
    PV_TIER_VIO,
    PV_TIER_EMBER,
    PV_TIER_OMEGA,
    PV_TIER_PINK,
    PV_TIER_N
};

struct pv_badge
{
    const char   *name;        /* <= 26 chars */
    const char   *desc;        /* <= 40 chars */
    unsigned char metric;
    unsigned char flags;
    long          threshold;   /* unlocked once the metric reaches this */
    unsigned char tier;
};

/* Tracks within one day whose repeats are counted, for "played one song N
 * times in a day". A day with more distinct tracks than this stops counting
 * new ones, which can only ever understate a joke statistic. */
#define PV_BADGE_DAYTRK 96

/* The running state a chronological pass builds up, and the reason this is
 * plain data: it goes into the saved index alongside the aggregate tables, so
 * that reading only the new tail of the log continues these figures rather
 * than starting them again.
 *
 * Everything here depends on the ORDER entries arrive in -- consecutive runs,
 * per-day accumulators that reset when the date changes, and a profile that
 * decides which listening type you were at each point. It is the one part of
 * the model that cannot be recomputed from the aggregates afterwards. */
struct pv_badge_state
{
    long     cur_day;          /* the day being accumulated; -1 = none yet */
    unsigned day_secs, day_secs_max;
    int      skip_run, skip_run_max;
    int      play_run, play_run_max;
    int      trackday_max;

    /* Running profile, for the listening types. */
    long     plays, skips, uniq, night, top_artist;

    unsigned char type_seen[16];   /* types passed through, chronologically */
    unsigned char date_flags;      /* new year, Christmas, Friday 13th, ... */

    int      daytrk_n;
    struct
    {
        unsigned hash;
        int      n;
    } daytrk[PV_BADGE_DAYTRK];
};

/* Start a fresh pass. */
void pv_badges_reset(struct pv_badge_state *st);

/* One entry, in log order, AFTER the aggregate tables have been updated for
 * it -- the running profile reads those tables to see where this play left
 * its artist and title. */
void pv_badges_feed(struct pv_badge_state *st, unsigned long ts, bool valid_ts,
                    bool listened, unsigned secs,
                    const char *artist, const char *title);

struct pv_totals;

/* Turn the aggregates and the running state into the metric values, then
 * decide what is unlocked. Returns how many badges are. */
int pv_badges_eval(const struct pv_badge_state *st,
                   const struct pv_totals *totals);

/* Reading the result. Valid until the next pv_badges_eval(). */
int  pv_badges_count(void);          /* how many rows the table holds */
int  pv_badges_unlocked_count(void);
bool pv_badges_unlocked(int i);
const struct pv_badge *pv_badges_get(int i);
long pv_badges_value(int i);         /* the metric's value, for progress */

/* How much of a row to show.
 *
 * A ladder of thirty "listen for N minutes" rows would be a spoiler and a
 * bore, so only its lowest unearned rung is shown, with live progress; the
 * rest stay a mystery until earned. Rows marked secret never show at all
 * until they do. */
enum pv_badge_vis
{
    PV_BV_HIDDEN,   /* "? ? ?" -- not even its name */
    PV_BV_GOAL,     /* the next rung of its ladder, with progress */
    PV_BV_DONE
};

/* Work out what each row shows, load the saved progress, and stamp anything
 * newly unlocked with today's date. Returns how many are new since the last
 * look. Call after pv_badges_eval(), once per crunch: a second call finds
 * everything already seen and reports nothing new, which would pull the set
 * out from under whatever is still drawing it.
 *
 * Nothing is ever new against a device that had no progress file to compare
 * with -- a fresh one, or one whose file the sync deleted -- or the first run
 * would announce most of the table at once. */
int  pv_badges_classify(void);

enum pv_badge_vis pv_badges_vis(int i);
bool pv_badges_is_new(int i);
unsigned long pv_badges_when(int i);   /* 0 = earned before records began */

/* The new set, for the screens that announce it. Valid until the next
 * pv_badges_classify(). */
int  pv_badges_new_count(void);
int  pv_badges_new_index(int k);       /* the k'th new badge, or -1 */

/* Persist what has been seen and when. Worth doing once the user has
 * actually looked, so "NEW" survives until then. */
void pv_badges_save(void);

/* Mark every unlocked badge unannounced again, keeping the earned dates.
 * A test hook: nothing but the debug menu should call it. */
void pv_badges_rearm(void);

#endif /* _PV_BADGES_H */
