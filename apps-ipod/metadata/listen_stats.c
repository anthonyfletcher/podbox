/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Listening statistics: how long each audiobook and podcast show has been
 * listened to, on which days, and the day a book was finished. Shown with a
 * book's or a show's Listening Stats (screens/browse/listen_progress.c).
 *
 *   keeping  - a spoken-word track counts the time from where it started to
 *              where it stopped, at most the time that passed at the fastest
 *              speed, so a seek forward is not listening. Kept in RAM on the
 *              audio thread and written when a book's place is saved
 *              (book_resume_save()), so it never wakes the disk itself.
 *   records  - one a book or show, keyed as the book resume file keys a
 *              book, newest first, in user/listen_stats.dat
 *   reading  - one record, or every record of a kind summed
 *
 * Days need the clock set; without it the dates and streaks are left out and
 * the time still counts.
 ****************************************************************************/

#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "timefuncs.h"
#include "string-extra.h"
#include "audio.h"                    /* struct track_event, TEF_AUTO_SKIP */
#include "events.h"
#include "system/appevents.h"
#include "metadata.h"
#include "database/db_spoken.h"
#include "database/libfile.h"
#include "database/path_key.h"
#include "metadata/book_resume.h"
#include "screens/browse/book_shelf.h"   /* book_shelf_is_last_track */
#include "system/library_files.h"
#include "viewers/playback_viewer/pv_stats.h"  /* the calendar */
#include "listen_stats.h"

#define LS_MAX      256             /* records kept */
#define LS_PENDING  4               /* plays held between saves */
#define LS_END_SLACK_MS 10000       /* stopped this near the end is the end */

/* ---- the record and file ---------------------------------------------- */

static int ls_open_file(struct libfile_header *h)
{
    return libfile_open(LIB_LISTEN_STATS_FILE, LIB_LISTEN_STATS_MAGIC,
                        LIB_LISTEN_STATS_VERSION,
                        sizeof(struct listen_stats), h, NULL);
}

static bool ls_read(int fd, struct listen_stats *r)
{
    return read(fd, r, sizeof(*r)) == (ssize_t)sizeof(*r);
}

/* Today as a record keeps it, or 0 without the clock */
static int ls_today(void)
{
    struct tm *tm = get_time();

    if (!valid_time(tm))
        return 0;
    return pv_days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday)
           - LS_EPOCH;
}

bool listen_stats_get(uint64_t key, struct listen_stats *out)
{
    struct libfile_header h;
    int fd = ls_open_file(&h);

    memset(out, 0, sizeof(*out));
    if (fd < 0)
        return false;
    for (uint32_t i = 0; i < h.count && ls_read(fd, out); i++)
        if (out->key == key)
        {
            close(fd);
            return true;
        }
    close(fd);
    memset(out, 0, sizeof(*out));
    return false;
}

void listen_stats_sum(int kind, struct listen_stats *all, int *items,
                      int *finished)
{
    struct libfile_header h;
    struct listen_stats r;
    int fd = ls_open_file(&h);

    memset(all, 0, sizeof(*all));
    *items = *finished = 0;
    if (fd < 0)
        return;
    for (uint32_t i = 0; i < h.count && ls_read(fd, &r); i++)
    {
        if (r.kind != kind || !r.seconds)
            continue;
        (*items)++;
        *finished += r.finished != 0;
        all->seconds += r.seconds;
        all->days += r.days;
        if (r.longest > all->longest)
            all->longest = r.longest;
        if (r.best_streak > all->best_streak)
            all->best_streak = r.best_streak;
        if (r.started && (!all->started || r.started < all->started))
            all->started = r.started;
    }
    close(fd);
}

/* Rewrites the file with 'r' first, streaming the others past one buffer */
static void ls_save(const struct listen_stats *r)
{
    static struct listen_stats old;
    struct libfile_writer w;
    struct libfile_header h;
    int fd, kept = 1;
    bool ok;

    if (!libfile_begin(&w, LIB_LISTEN_STATS_FILE, LIB_LISTEN_STATS_MAGIC,
                       LIB_LISTEN_STATS_VERSION, sizeof(struct listen_stats),
                       NULL))
        return;
    ok = libfile_write(&w, r, sizeof(*r), 1);
    fd = ls_open_file(&h);
    if (fd >= 0)
    {
        for (uint32_t i = 0; ok && kept < LS_MAX && i < h.count
                             && ls_read(fd, &old); i++)
        {
            if (old.key == r->key)
                continue;
            ok = libfile_write(&w, &old, sizeof(old), 1);
            kept++;
        }
        close(fd);
    }
    libfile_finish(&w, ok);
}

/* ---- keeping ---------------------------------------------------------- */

/* The spoken-word track playing, from where it started */
static struct
{
    bool live;
    uint64_t track;             /* path_key() of its file */
    uint64_t key;
    uint8_t kind;
    unsigned long elapsed;
    long tick;
} cur;

/* Plays not yet written. The audio thread adds and the saver takes; the
 * saver copies them out and clears the count before it yields. */
static struct ls_pending
{
    uint64_t key;
    uint8_t kind;
    uint32_t seconds;           /* all of them together */
    uint32_t longest;           /* the longest one */
    uint16_t day;
    bool ended;                 /* a track of it played to its end */
    uint64_t track;             /* that track's path_key() */
    char book[BOOK_ID_MAX];     /* the book's id, for the shelf to ask */
} pending[LS_PENDING];
static int pending_ct;

static void track_change_event(unsigned short id, void *ev_data)
{
    const struct mp3entry *id3 = ((struct track_event *)ev_data)->id3;
    static char name[BOOK_ID_MAX];
    (void)id;

    cur.live = id3 && db_spoken_is_spoken_genre(id3->genre_string);
    if (!cur.live)
        return;
    cur.track = path_key(id3->path);
    cur.kind = db_spoken_is_podcast_genre(id3->genre_string) ? 2 : 1;
    /* A show is its album whoever each episode names as its artist */
    cur.key = cur.kind == 2 && id3->album && id3->album[0]
              ? book_resume_key(id3->album)
              : book_resume_key(book_resume_name(id3, name, sizeof(name)));
    cur.elapsed = id3->elapsed;
    cur.tick = current_tick;
}

/* What the playing track has added since its baseline, into the pending
 * plays; 'ended' when it played to its end. Yields nowhere, so the audio
 * thread and a saver cannot interleave in it. */
static void take(const struct mp3entry *id3, bool ended)
{
    struct ls_pending *p = NULL;
    unsigned long ms, most;
    int i;

    ms = id3->elapsed > cur.elapsed ? id3->elapsed - cur.elapsed : 0;
    most = (unsigned long)(current_tick - cur.tick) * (1000 / HZ) * 5 / 2;
    if (ms > most)
        ms = most;
    if (ms < 1000)
        return;

    for (i = 0; i < pending_ct; i++)
        if (pending[i].key == cur.key)
            p = &pending[i];
    if (!p)
    {
        if (pending_ct == LS_PENDING)
            return;
        p = &pending[pending_ct];
        memset(p, 0, sizeof(*p));
        p->key = cur.key;
        p->kind = cur.kind;
        pending_ct++;
    }
    p->seconds += ms / 1000;
    if (ms / 1000 > p->longest)
        p->longest = ms / 1000;
    p->day = ls_today();
    if (ended && cur.kind == 1)
    {
        const char *name = book_resume_name(id3, p->book, sizeof(p->book));

        p->ended = true;
        p->track = cur.track;
        if (name != p->book)            /* a book known by its path */
            strmemccpy(p->book, name, sizeof(p->book));
    }
}

/* On the audio thread: no file I/O */
static void track_finish_event(unsigned short id, void *ev_data)
{
    const struct track_event *te = ev_data;
    const struct mp3entry *id3 = te->id3;
    (void)id;

    if (!cur.live || !id3 || path_key(id3->path) != cur.track)
        return;
    /* A seek to the track's start: the same track goes on from there */
    if (te->flags & TEF_REWIND)
    {
        take(id3, false);
        cur.elapsed = 0;
        cur.tick = current_tick;
        return;
    }
    cur.live = false;
    take(id3, (te->flags & TEF_AUTO_SKIP)
              && id3->elapsed + LS_END_SLACK_MS >= id3->length);
}

/* One play into its record */
static void ls_apply(const struct ls_pending *p)
{
    struct listen_stats r;

    listen_stats_get(p->key, &r);
    r.key = p->key;
    r.kind = p->kind;
    r.seconds += p->seconds;
    if (p->longest > r.longest)
        r.longest = p->longest;
    if (p->day)
    {
        if (!r.started)
            r.started = p->day;
        if (p->day != r.last)
        {
            r.days++;
            r.streak = r.last && p->day == r.last + 1 ? r.streak + 1 : 1;
            if (r.streak > r.best_streak)
                r.best_streak = r.streak;
            r.last = p->day;
        }
    }
    if (p->ended && !r.finished && p->day
        && book_shelf_is_last_track(p->book, p->track))
        r.finished = p->day;
    ls_save(&r);
}

/* Held across a flush: the UI, the background thread's spin-down save and
 * the iAP worker each save a book's place, and a flush yields in its writes */
static struct mutex ls_mutex;

void listen_stats_flush(void)
{
    static struct ls_pending copy[LS_PENDING];
    const struct mp3entry *id3;
    int n;

    mutex_lock(&ls_mutex);
    id3 = audio_status() ? audio_current_track() : NULL;

    /* The track still playing counts what it has so far, from a fresh
     * baseline: a save comes with a pause, a stop or a shutdown */
    if (cur.live && id3 && path_key(id3->path) == cur.track)
    {
        take(id3, false);
        cur.elapsed = id3->elapsed;
        cur.tick = current_tick;
    }
    n = pending_ct;
    memcpy(copy, pending, n * sizeof(copy[0]));
    pending_ct = 0;
    for (int i = 0; i < n; i++)
        ls_apply(&copy[i]);
    mutex_unlock(&ls_mutex);
}

void listen_stats_init(void)
{
    mutex_init(&ls_mutex);
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, track_change_event);
    add_event(PLAYBACK_EVENT_TRACK_FINISH, track_finish_event);
}
