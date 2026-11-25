/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * On Repeat: a playlist of the tracks played most over a recent span, read
 * from the playback log.
 *
 * The plays are counted in a fixed table of REP_SLOTS tracks, Space-Saving
 * style: a track arriving when the table is full takes the place of the one
 * counted least, inheriting its count as the error it may carry. While the
 * span holds fewer tracks than the table every count is exact, and past that
 * any track played more than plays/REP_SLOTS times is still certain to be
 * kept. A fixed table is what lets an accessory's thread build one while a
 * screen holds the app buffer.
 *
 * A span is found without reading the history before it:
 * pv_log_offset_since() starts at the file that holds its first entry.
 *
 * Parts, in order:
 *   - the span each setting names
 *   - what was last worked out
 *   - counting and ranking
 *   - whether there is anything to offer
 *   - playing it
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "lang.h"
#include "timefuncs.h"
#include "audio.h"
#include "settings/settings.h"
#include "database/path_key.h"
#include "database/tagcache.h"
#include "database/db_spoken.h"
#include "playlist/playlist.h"
#include "playlist/on_repeat.h"
#include "audio/playback.h"
#include "system/app_util.h"
#include "widgets/splash.h"
#include "viewers/playback_viewer/pv_log.h"
#include "viewers/playback_viewer/pv_names.h"
#include "viewers/playback_viewer/pv_stats.h"
#include "root_menu.h"

#define DAY_SECS      86400UL
#define ROLLING_DAYS  7

/* Tracks counted at once: 8 KB */
#define REP_SLOTS     512

struct rep_slot
{
    uint64_t key;       /* path_key() */
    uint32_t last_ts;   /* latest play, which breaks a tie */
    uint16_t plays;     /* 0 once taken by the ranking */
    uint16_t err;       /* plays it may have inherited, Space-Saving's bound */
};

static struct rep_slot slots[REP_SLOTS];
static int used;
static unsigned long span_from, span_to;

/* The table and what was last worked out are shared by the UI and an
 * accessory's thread, so whoever comes second waits. 'busy' is set while a
 * read holds the lock, for a caller that must not wait. */
static struct mutex rep_mutex;
static bool rep_mutex_ready;
static volatile bool busy;

static void rep_lock(void)
{
    /* Threads here are cooperative, so nothing runs between the test and
     * the set */
    if (!rep_mutex_ready)
    {
        mutex_init(&rep_mutex);
        rep_mutex_ready = true;
    }
    mutex_lock(&rep_mutex);
}

/** The span **/

/* Weekly changes on the day the Weekly mood playlists do -- see
 * mix_vary_begin() in database/sound_mix.c -- and counts the seven days
 * before it. Monthly counts the calendar month before this one. 'day' is
 * today. */
static bool rep_span(unsigned long *from, unsigned long *to, long *day)
{
    struct tm *tm = get_time();
    unsigned long now;
    long start;
    int y, m, d;

    if (!valid_time(tm))
        return false;

    now = (unsigned long)mktime(tm);
    *day = (long)(now / DAY_SECS);

    switch (global_settings.on_repeat)
    {
    case ON_REPEAT_MONTHLY:
        start = *day - (tm->tm_mday - 1);
        pv_civil_from_days(start - 1, &y, &m, &d);
        *from = (unsigned long)(start - d) * DAY_SECS;
        *to = (unsigned long)start * DAY_SECS;
        break;

    case ON_REPEAT_ROLLING:
        *from = now - ROLLING_DAYS * DAY_SECS;
        *to = (unsigned long)-1;
        break;

    default:
        start = *day - tm->tm_yday % 7;
        *from = (unsigned long)(start - 7) * DAY_SECS;
        *to = (unsigned long)start * DAY_SECS;
        break;
    }
    return true;
}

/** What was last worked out **/

/* Weekly and Monthly count a span that is over, so their answer stands until
 * the span moves. Rolling's span moves all the time: a yes is kept for the
 * day, and a no only until something more is logged. A database commit, or a
 * change to what is kept apart, asks again. */
static struct
{
    bool valid, any;
    int mode, apart;
    int32_t commit;
    unsigned long from, log_size;
    long day;
} seen;

static bool rolling(void)
{
    return global_settings.on_repeat == ON_REPEAT_ROLLING;
}

/* Whether 'seen' was worked out for the span starting 'from' on 'day', under
 * the database and settings as they are */
static bool seen_matches(unsigned long from, long day)
{
    return seen.valid && seen.mode == global_settings.on_repeat
           && seen.commit == tagcache_commit_id()
           && seen.apart == db_spoken_apart()
           && (rolling() ? seen.day == day : seen.from == from);
}

/* Whether it still answers. A Rolling no is checked against the log's size
 * only with 'may_read'. */
static bool seen_holds(unsigned long from, long day, bool may_read)
{
    if (!seen_matches(from, day))
        return false;
    if (!rolling() || seen.any)
        return true;
    if (!may_read)
        return false;
    playback_log_flush();
    return seen.log_size == pv_log_size(PV_SRC_PLAYBACK);
}

/* 'log_size' is the log as the read began, so a play logged during the read
 * makes a Rolling no stale */
static void remember(bool any, unsigned long from, long day,
                     unsigned long log_size)
{
    seen.valid = true;
    seen.any = any;
    seen.mode = global_settings.on_repeat;
    seen.commit = tagcache_commit_id();
    seen.apart = db_spoken_apart();
    seen.from = from;
    seen.day = day;
    seen.log_size = log_size;
}

/** Counting and ranking **/

static void count_cb(const struct pv_entry *e, void *ctx)
{
    struct rep_slot *s, *least = NULL;
    uint64_t key;
    int i;

    (void)ctx;
    if (!e->listened || !e->valid_ts || e->ts < span_from || e->ts >= span_to
        || (key = path_key(e->path)) == 0)
        return;

    for (i = 0; i < used; i++)
    {
        s = &slots[i];
        if (s->key == key)
        {
            if (s->plays < UINT16_MAX)
                s->plays++;
            s->last_ts = e->ts;
            return;
        }
        if (!least || s->plays < least->plays)
            least = s;
    }

    if (used < REP_SLOTS)
    {
        s = &slots[used++];
        s->plays = 1;
        s->err = 0;
    }
    else
    {
        s = least;
        s->err = s->plays;
        if (s->plays < UINT16_MAX)
            s->plays++;
    }
    s->key = key;
    s->last_ts = e->ts;
}

/* Plays a slot is certain of, which is what it ranks by */
static int rep_score(const struct rep_slot *s)
{
    return s->plays - s->err;
}

/* The best slot not yet taken, or NULL */
static struct rep_slot *rep_best(void)
{
    struct rep_slot *best = NULL;
    int i;

    for (i = 0; i < used; i++)
    {
        struct rep_slot *s = &slots[i];

        if (s->plays == 0)
            continue;
        if (!best || rep_score(s) > rep_score(best)
            || (rep_score(s) == rep_score(best) && s->last_ts > best->last_ts))
            best = s;
    }
    return best;
}

int on_repeat_keys(uint64_t *keys, int max, const volatile bool *stop)
{
    struct rep_slot *s;
    enum pv_source src;
    unsigned long log_size = 0;
    long day;
    int n = 0;

    rep_lock();
    if (!rep_span(&span_from, &span_to, &day))
    {
        mutex_unlock(&rep_mutex);
        return -1;
    }
    busy = true;

    /* Plays still held in memory are part of the span too */
    playback_log_flush();
    if (rolling())
        log_size = pv_log_size(PV_SRC_PLAYBACK);

    used = 0;
    src = pv_log_pick_source();
    if (src != PV_SRC_NONE)
    {
        cpu_boost(true);
        pv_log_read_range_stop(src, pv_log_offset_since(src, span_from), 0,
                               count_cb, NULL, stop);
        cpu_boost(false);
    }

    while (n < max && (s = rep_best()) != NULL)
    {
        int idx = tagcache_find_key(s->key);

        if (idx >= 0 && !pv_names_kept_apart_idx(idx))
            keys[n++] = s->key;
        s->plays = 0;
    }

    if (max > 0 && !(stop && *stop))
        remember(n > 0, span_from, day, log_size);

    busy = false;
    mutex_unlock(&rep_mutex);
    return n;
}

/** Whether there is anything to offer **/

/* A Rolling no counts as known: checking it is a size, not a read */
bool on_repeat_known(void)
{
    unsigned long from, to;
    long day;

    if (busy)
        return false;
    if (!tagcache_is_in_ram() || !rep_span(&from, &to, &day))
        return true;
    return seen_matches(from, day);
}

void on_repeat_refresh(const volatile bool *stop)
{
    unsigned long from, to;
    uint64_t key;
    long day;

    if (!tagcache_is_in_ram() || !rep_span(&from, &to, &day))
        return;
    rep_lock();
    if (!seen_holds(from, day, true))
        on_repeat_keys(&key, 1, stop);
    mutex_unlock(&rep_mutex);
}

bool on_repeat_offered(bool may_read)
{
    unsigned long from, to;
    long day;

    if (!tagcache_is_in_ram() || !rep_span(&from, &to, &day))
        return false;

    /* Plain reads of 'seen', which cooperative threads make safe */
    if (!may_read)
        return busy || !seen_holds(from, day, false) || seen.any;

    on_repeat_refresh(NULL);
    return seen_matches(from, day) && seen.any;
}

/** Playing it **/

static bool rep_start(const uint64_t *keys, int n)
{
    struct playlist_insert_context context;
    struct tagcache_search tcs;
    char path[MAX_PATH];
    int i, added = 0;

    if (!tagcache_search(&tcs, tag_filename))
        return false;
    if (playlist_create(NULL, NULL) < 0)
    {
        tagcache_search_finish(&tcs);
        return false;
    }

    if (playlist_insert_context_create(NULL, &context, PLAYLIST_INSERT_LAST,
                                       false, false) >= 0)
    {
        for (i = 0; i < n; i++)
        {
            int idx = tagcache_find_key(keys[i]);

            if (idx < 0
                || !tagcache_retrieve(&tcs, idx, tag_filename, path,
                                      sizeof(path))
                || !file_exists(path))
                continue;
            if (playlist_insert_context_add(&context, path) < 0)
                break;
            added++;
        }
    }
    /* create() keeps the playlist lock even when it fails */
    playlist_insert_context_release(&context);
    tagcache_search_finish(&tcs);

    if (added == 0)
        return false;
    /* Shuffled as a saved playlist is, and as a dock's choice of it is */
    if (global_settings.playlist_shuffle)
        playlist_shuffle(current_tick, -1);
    playlist_start(0, 0, 0);
    return true;
}

int on_repeat_play(void)
{
    static uint64_t keys[ON_REPEAT_MAX];
    int n;

    if (global_settings.party_mode && audio_status())
    {
        splash(HZ, ID2P(LANG_PARTY_MODE));
        return GO_TO_PREVIOUS;
    }

    splash(0, ID2P(LANG_WAIT));

    n = on_repeat_keys(keys, global_settings.on_repeat_length, NULL);
    if (n < 0)
    {
        splash(HZ * 2, ID2P(LANG_ON_REPEAT_NO_CLOCK));
        return GO_TO_PREVIOUS;
    }
    if (n == 0)
    {
        splash(HZ * 2, global_settings.playback_log
                       ? ID2P(LANG_ON_REPEAT_NONE)
                       : ID2P(LANG_ON_REPEAT_NO_LOG));
        return GO_TO_PREVIOUS;
    }

    if (!warn_on_pl_erase())
        return GO_TO_PREVIOUS;

    if (!rep_start(keys, n))
    {
        splash(HZ * 2, ID2P(LANG_SOUND_MIX_FAILED));
        return GO_TO_PREVIOUS;
    }
    return GO_TO_WPS;
}
