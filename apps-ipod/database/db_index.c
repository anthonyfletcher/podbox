/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The database index: the flat album and artist list derived from tagcache.
 *
 * Built by walking tagcache, held in the buffer carousel.h describes as
 * pf_idx, and persisted to db_index.idx so later reads get it back instead of
 * rescanning. It carries no artwork -- only names, years, playback figures and
 * the taglist seeks needed to navigate into the database -- so it goes stale
 * when the database changes, not when files on disk do.
 *
 * It began as the carousel's own list and is no longer only that: the album
 * and artist charts read the same index for their rankings, which is why it
 * is named for the database rather than for any one screen. Building it is
 * nobody's screen's job, and is intended to happen while none of them is up.
 *
 * Parts, in order:
 *   - progress reporting and cancellation
 *   - sort comparators
 *   - writing entries into the index buffer
 *   - the tagcache walks that populate it
 *   - the on-disk form
 *   - db_index_build(), which reuses the saved index or rebuilds it
 *   - the background pass that keeps the saved index current
 ****************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"          /* ALIGN_BUFFER/alignof */
#include "rbpaths.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"                     /* dir_exists, mkdir */
#include "lang.h"
#include "settings/settings.h"
#include "database/tagcache.h"
#include "metadata/art_cache.h"      /* art_cache_dir_hash */
#include "widgets/splash.h"
#include "widgets/yesno.h"           /* gui_syncyesno_run, YESNO_YES */
#include "draw/screen_access.h"
#include "skin/statusbar_skinned.h"  /* sb_skin_update */
#include "skin/skin_engine.h"        /* skin_flush_dirty */
#include "input/action.h"
#include "powermgmt.h"               /* reset_poweroff_timer */
#include "system/shutdown.h"
#include "system/activity.h"          /* ui_set_working */
#include "system/bg_task.h"           /* the background pass runs as one */
#include "core_alloc.h"              /* background build buffer */
#include "usb.h"                     /* SYS_USB_CONNECTED handling */
#include "screens/covers/carousel.h"      /* pf_idx, CACHE_PREFIX, SUCCESS/ERROR_* */
#include "screens/covers/album_covers.h"   /* SORT_BY_*, ASCENDING (sort order) */
#include "db_index.h"

/* The index file, and the four-byte magic at its start. It holds albums and
 * artists alike, and is read by more than the carousel now, so it lives beside
 * the other state in ROCKBOX_DIR rather than in the carousel's own folder --
 * which keeps only what is genuinely the carousel's, its slide cache and empty
 * slide. Regenerated if absent. */
#define DB_INDEX_FILE ROCKBOX_DIR "/db_index.idx"
/* Bump the magic whenever struct album_data or struct artist_data, or the
 * file's layout, changes. load_album_index() validates nothing finer -- it
 * checks this and some coarse sizes -- so a struct that grew without a new
 * magic is read back at the wrong stride from a file written by an older
 * build.
 *   PFID -> PFIE: album_data gained playcount/lastplayed (the album charts).
 *   PFIE -> PFIF: artist_data gained the same (the artist charts), and the
 *                 file moved out of the carousel folder.
 *   PFIF -> PFIG: both gained art_hash (the carousel's slide loading).
 *   PFIG -> PFIH: album_data gained artist_art_hash. PFIG indexes also have to
 *                 go regardless: they were written by a build that resolved
 *                 every art_hash to 0, so nothing in them would ever match a
 *                 cached thumbnail. */
#define INDEX_HDR "PFIH"

enum ePFS { ePFS_ARTIST = 0, ePFS_ALBUM };

/* Shared by every walk below; the models keep their own where they need one. */
static struct tagcache_search tcs;

/* The index currently being built, and the buffer it is being built into.
 *
 * A file static rather than a parameter threaded through the ten functions
 * that touch it, and deliberately singular: only one build may run at a time,
 * whoever asked for it. That lets the same code serve the carousel building
 * into the app buffer it has claimed, and a background build into memory of
 * its own -- the allocation differs, the builder does not. */
static struct pf_index_t *pfi;

/* Serialises the two callers of the builder. The carousel blocks here rather
 * than starting a second build; because it has usually just asked for one, it
 * sets idx_abort first so the background pass gives up promptly instead of
 * making the screen wait out a full scan. */
static struct mutex build_mutex;

/* True while the *background* pass owns the builder. It changes two things
 * the builder must not do off the main thread: draw, and read the keypad. */
static bool building_bg;
static volatile bool idx_abort;

/* Last progress the background pass reported; read by anything showing it.
 * bg_step is the step name the builder was already passing to the progress
 * bar and the background path was already throwing away -- keeping the
 * pointer is the whole cost of reporting what the pass is doing. */
static int bg_done, bg_total;
static const char *bg_step;

static void draw_progressbar(int step, int count, char *msg);
static int wait_for_background(void);

/* Keep an idle poweroff from interrupting a build the user is watching.
 *
 * Deliberately not done for a background pass. reset_poweroff_timer() records
 * a *user event*, and claiming one on behalf of a pass nobody asked for would
 * hold poweroff off for the whole build and then restart the timer from the
 * moment it ended -- on a device sitting idle with the screen off. Poweroff is
 * already deferred while the pass runs, by the disk activity it genuinely
 * causes (see handle_auto_poweroff()). */
static void keep_awake_for_build(void)
{
    if (!building_bg)
        reset_poweroff_timer();
}

/* The "working" indicator, likewise only for a build someone is watching.
 *
 * ui_set_working() repaints the status bar on the spot and flushes it -- it has
 * to, because a foreground build then blocks without redrawing, and the
 * indicator would otherwise appear only once the work was over. From the
 * background thread that same repaint lands on whatever screen the user is
 * actually on, which is the flicker seen as a background build finished. The
 * background pass has no indicator to show and nothing to show it on. */
static void set_working_for_build(bool working)
{
    if (!building_bg)
        ui_set_working(working);
}

static void draw_progressbar(int step, int count, char *msg)
{
    if (building_bg)
    {
        /* Record it instead. Nothing may reach the LCD from the background
         * thread -- whatever screen the user is on owns it. */
        bg_done = step;
        bg_total = count;
        bg_step = msg;
        return;
    }

    (void)step; (void)count; (void)msg;

    /* Rate-limited, because this is not cheap: a status bar render plus an LCD
     * flush is a few milliseconds either side of ten, and the callers reach
     * here once per album. Unthrottled, most of a foreground build was spent
     * redrawing rather than reading the database -- the background pass, which
     * draws nothing, finishes the same work several times quicker. 8fps is
     * more than enough for an indicator that only spins. */
    static long next_draw_tick;
    long now = current_tick;

    if (!TIME_AFTER(now, next_draw_tick))
        return;
    next_draw_tick = now + HZ / 8;

    sb_skin_update(SCREEN_MAIN, true);
    skin_flush_dirty();
}

static bool progress_cancel(int step, int count, char *msg)
{
    if (building_bg)
    {
        /* No get_action() here: the background pass must not consume button
         * presses meant for the screen in front of the user. It gives up when
         * someone else wants the builder, or on the way to a USB session.
         *
         * The yield is not optional. Scheduling is cooperative, so a pass that
         * never gives up the CPU freezes the UI however low its priority --
         * and get_action(), which used to be the yield point on this path, is
         * exactly what was removed above. */
        yield();
        if (count)
            draw_progressbar(step, count, msg);
        return idx_abort;
    }

    const struct text_message prompt = {
            (const char*[]) {"Quit?", "Progress will be lost"}, 2};

    int action = get_action(CONTEXT_STD,TIMEOUT_NOBLOCK);
    if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
    {
        if (gui_syncyesno_run(&prompt, NULL, NULL) == YESNO_YES)
            return true;
        lcd_clear_display();
    }

    if (count)
        draw_progressbar(step, count, msg);

    return false;
}

int compare_albums (const void *a_v, const void *b_v)
{
    uint32_t artist_a = ((struct album_data *)a_v)->artist_idx;
    uint32_t artist_b = ((struct album_data *)b_v)->artist_idx;

    uint32_t album_a = ((struct album_data *)a_v)->name_idx;
    uint32_t album_b = ((struct album_data *)b_v)->name_idx;

    int year_a = ((struct album_data *)a_v)->year;
    int year_b = ((struct album_data *)b_v)->year;

    switch (global_settings.album_covers_sort_albums_by)
    {
        case SORT_BY_ARTIST_AND_NAME:
            if (artist_a - artist_b == 0)
                return (int)(album_a - album_b);
            break;
        case SORT_BY_ARTIST_AND_YEAR:
            if (artist_a - artist_b == 0)
            {
                if (global_settings.album_covers_year_sort_order == ASCENDING)
                    return year_a - year_b;
                else
                    return year_b - year_a;
            }
            break;
        case SORT_BY_YEAR:
            if (year_a - year_b != 0)
            {
                if (global_settings.album_covers_year_sort_order == ASCENDING)
                    return year_a - year_b;
                else
                    return year_b - year_a;
            }
            break;
        case SORT_BY_NAME:
            if (album_a - album_b != 0)
                return (int)(album_a - album_b);
            break;
    }

    return (int)(artist_a - artist_b);
}

static int compare_album_artists (const void *a_v, const void *b_v)
{
    uint32_t a = ((struct album_data *)a_v)->artist_idx;
    uint32_t b = ((struct album_data *)b_v)->artist_idx;
    return (int)(a - b);
}

static void write_album_index(int idx, int name_idx,
                              long album_seek, int artist_idx, long artist_seek)
{
    pfi->album_index[idx].name_idx = name_idx;
    pfi->album_index[idx].seek = album_seek;
    pfi->album_index[idx].artist_idx = artist_idx;
    pfi->album_index[idx].artist_seek = artist_seek;
    pfi->album_index[idx].year = 0;
    pfi->album_index[idx].playcount = 0;
    pfi->album_index[idx].lastplayed = 0;
    pfi->album_index[idx].art_hash = 0;
    pfi->album_index[idx].artist_art_hash = 0;
}

static inline void write_album_entry(struct tagcache_search *tcs,
                                     int name_idx, unsigned int len)
{
    write_album_index(-pfi->album_ct, name_idx, tcs->result_seek, 0, -1);
    pfi->album_len += len;
    pfi->album_ct++;

    if (pfi->album_untagged_seek == -1 && strcmp(UNTAGGED, tcs->result) == 0)
    {
        pfi->album_untagged_idx = name_idx;
        pfi->album_untagged_seek = tcs->result_seek;
    }
}

static void write_artist_entry(struct tagcache_search *tcs,
                               int name_idx, unsigned int len)
{
    pfi->artist_index[-pfi->artist_ct].name_idx = name_idx;
    pfi->artist_index[-pfi->artist_ct].seek = tcs->result_seek;
    pfi->artist_index[-pfi->artist_ct].playcount = 0;
    pfi->artist_index[-pfi->artist_ct].lastplayed = 0;
    pfi->artist_index[-pfi->artist_ct].art_hash = 0;
    pfi->artist_len += len;
    pfi->artist_ct++;
}

/* The art_cache keys for the folders holding the artwork of the track the given
 * search is currently sitting on: its own folder (the album's), and that
 * folder's parent (the artist's, under the <artist>/<album>/<track> layout the
 * artist list assumes). Either is set to 0 if the path is too shallow to strip
 * that far. Both are resolved together because they come from one path.
 *
 * The caller must be on a *filtered or numeric* search. Those go through
 * build_lookup_list(), which walks the master index and so records a real
 * master-index id per result -- and tcs->idx_id is what makes the path one
 * direct fetch away, with no search and no scan of our own. An unfiltered
 * search on a unique tag (tag_album, tag_albumartist) is not usable here: those
 * tag files are deduplicated and their entries carry idx_id -1, so there is no
 * track to ask about. That is why this is called from the per-album and
 * per-artist passes rather than from the walks that first enumerate them.
 *
 * Done during the build so that displaying a slide never has to go near the
 * database. Either output pointer may be NULL for a caller that wants only the
 * other one. */
static void resolve_art_hashes(struct tagcache_search *tcs,
                               unsigned int *album_hash,
                               unsigned int *artist_hash)
{
    /* Static rather than automatic: this can run on the background build
     * thread, whose stack is modest, and one build at a time is enforced by
     * build_mutex. */
    static char path[MAX_PATH];
    char *sep;

    if (album_hash)
        *album_hash = 0;
    if (artist_hash)
        *artist_hash = 0;

    if (tcs->idx_id < 0
        || !tagcache_retrieve(tcs, tcs->idx_id, tag_filename,
                              path, sizeof(path)))
        return;

    sep = strrchr(path, '/');
    if (!sep || sep == path)
        return;
    *sep = '\0';                     /* track file -> album folder */
    if (album_hash)
        *album_hash = art_cache_dir_hash(path);

    sep = strrchr(path, '/');
    if (!sep || sep == path)
        return;
    *sep = '\0';                     /* album -> artist folder */
    if (artist_hash)
        *artist_hash = art_cache_dir_hash(path);
}

/* adds tagcache_search results into artist/album index */
static int get_tcs_search_res(int type, struct tagcache_search *tcs,
                              void **buf, size_t *bufsz)
{
    char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    int ret = SUCCESS;
    unsigned int l, name_idx = 0;
    void (*writefn)(struct tagcache_search *, int, unsigned int);
    int data_size;
    if (type == ePFS_ARTIST)
    {
        writefn = &write_artist_entry;
        data_size = sizeof(struct artist_data);
    }
    else
    {
        writefn = &write_album_entry;
        data_size = sizeof(struct album_data);
    }

    while (tagcache_get_next(tcs, tcs_buf, tcs_bufsz))
    {
        if (progress_cancel(0, 0, NULL))
        {
            ret = ERROR_USER_ABORT;
            break;
        }

        l = tcs->result_len;

        /* Check before subtracting -- *bufsz is unsigned, so subtracting
         * data_size (or l) once the real remaining space is smaller than
         * that would wrap to a huge value instead of going negative,
         * permanently defeating this check for the rest of the scan and
         * letting strcpy()/writefn() below walk past the end of
         * pfi->buf on any library large enough to fill it. */
        if ((size_t)data_size > *bufsz || l > *bufsz - data_size)
        {
            /* not enough memory */
            ret = ERROR_BUFFER_FULL;
            break;
        }

        *bufsz -= data_size;

        strcpy(*buf, tcs->result);

        *bufsz -= l;
        *buf = l + (char *)*buf;

        writefn(tcs, name_idx, l);

        name_idx += l;
    }
    tagcache_search_finish(tcs);
    return ret;
}

#define STR_STEP_INDEXING_UNTAGGED "1/5 Find " UNTAGGED
#define STR_STEP_ASSIGNING_ALBUMS "2/5 Find Albums"
#define STR_STEP_ASSIGNING_ALBUM_STATS "3/5 Check Album Info"
#define STR_STEP_REMOVING_DUPLICATES "4/5 Remove Duplicates"
#define STR_STEP_ARTIST_STATS "Check Artist Info"

/*adds <untagged> albums/artist to existing album index */
static int create_album_untagged(struct tagcache_search *tcs, size_t *bufsz)
{
    static char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    int ret = SUCCESS;
    int album_count = pfi->album_ct; /* store existing count */
    int total_count = pfi->album_ct + pfi->artist_ct * 2;
    long seek;
    int last, final, retry;
    int i, j;
    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, total_count, STR_STEP_INDEXING_UNTAGGED);

    /* search tagcache for all <untagged> albums & save the albumartist seek pos */
    if (tagcache_search(tcs, tag_albumartist))
    {
        tagcache_search_add_filter(tcs, tag_album, pfi->album_untagged_seek);

        while (tagcache_get_next(tcs, tcs_buf, tcs_bufsz))
        {
            if (progress_cancel(pfi->album_ct, total_count, STR_STEP_INDEXING_UNTAGGED))
            {
                tagcache_search_finish(tcs);
                return ERROR_USER_ABORT;
            }

            if (tcs->result_seek ==
                pfi->album_index[-(pfi->album_ct - 1)].artist_seek)
                continue;

            if (sizeof(struct album_data) > *bufsz)
            {
                /* not enough memory */
                ret = ERROR_BUFFER_FULL;
                break;
            }

            *bufsz -= sizeof(struct album_data);
            write_album_index(-pfi->album_ct, pfi->album_untagged_idx,
                               pfi->album_untagged_seek, -1, tcs->result_seek);
            pfi->album_ct++;
        }
        tagcache_search_finish(tcs);

        if (ret == SUCCESS) {
            draw_progressbar(0, pfi->album_ct, STR_STEP_INDEXING_UNTAGGED);

            last = 0;
            final = pfi->artist_ct;
            retry = 0;

            /* map the artist_seek position to the artist name index */
            for (j = album_count; j < pfi->album_ct; j++)
            {
                if (progress_cancel(j, pfi->album_ct, STR_STEP_INDEXING_UNTAGGED))
                    return ERROR_USER_ABORT;

                seek = pfi->album_index[-j].artist_seek;

    retry_artist_lookup:
                retry++;
                for (i = last; i < final; i++)
                {
                    if (seek == pfi->artist_index[i].seek)
                    {
                        int idx = pfi->artist_index[i].name_idx;
                        pfi->album_index[-j].artist_idx = idx;
                        last = i; /* last match, start here next loop */
                        final = pfi->artist_ct;
                        retry = 0;
                        break;
                    }
                }
                if (retry > 0 && retry < 2)
                {
                    /* no match start back at beginning */
                    final = last;
                    last = 0;
                    goto retry_artist_lookup;
                }
            }
        }
    }

    return ret;
}

/* Create an index of all artists from the database, into whichever index pfi
 * currently points at. Like everything else here it assumes build_mutex is
 * held and pfi is set -- db_index_build_artists() is how an outside caller
 * gets into that state. */
static int build_artist_index(struct tagcache_search *tcs,
                                 void **buf, size_t *bufsz)
{
    int i, res = SUCCESS;
    struct artist_data* tmp_artist;

    /* artist index starts at end of buf it will be rearranged when finalized */
    pfi->artist_index = ((struct artist_data *)(*bufsz + (char *) *buf)) - 1;
    pfi->artist_ct = 0;
    pfi->artist_len = 0;
    /* artist names starts at beginning of buf */
    pfi->artist_names = *buf;

    tagcache_search(tcs, tag_albumartist);
    res = get_tcs_search_res(ePFS_ARTIST, tcs, &(*buf), bufsz);
    tagcache_search_finish(tcs);
    if (res < SUCCESS)
        return res;

    /* finalize the artist index */
    ALIGN_BUFFER(*buf, *bufsz, alignof(struct artist_data));
    tmp_artist = (struct artist_data*)*buf;
    for (i = pfi->artist_ct - 1; i >= 0; i--)
        tmp_artist[i] = pfi->artist_index[-i];

    pfi->artist_index = tmp_artist;
    /* move buf ptr to end of artist_index */
    *buf = pfi->artist_index + pfi->artist_ct;

    if (res == SUCCESS)
    {
        if (pfi->artist_ct > 0)
            res = pfi->artist_ct;
        else
            res = ERROR_NO_ALBUMS;
    }

    return res;
}

/* Summarise each album from its tracks: the year, and the playback history the
 * album charts sort on.
 *
 * All three come from one walk because the walk is the expensive part -- a
 * filtered tagcache search per album, which is why this is its own numbered
 * build step. A search on a numeric tag iterates one result per track (see
 * get_next() in tagcache.c, which sets idx_id per entry), so tagcache_get_numeric()
 * can be asked for any numeric tag of the track currently under the cursor,
 * not only the one being searched on.
 *
 * Year takes the maximum rather than the first because a compilation can carry
 * several; playcount sums because an album's plays are its tracks' plays; and
 * lastplayed takes the maximum because an album was last heard when its most
 * recently played track was. */
static int assign_album_stats(void)
{
    char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, pfi->album_ct, STR_STEP_ASSIGNING_ALBUM_STATS);
    for (int album_idx = 0; album_idx < pfi->album_ct; album_idx++)
    {
        keep_awake_for_build();

        if (progress_cancel(album_idx, pfi->album_ct, STR_STEP_ASSIGNING_ALBUM_STATS))
            return ERROR_USER_ABORT;

        int album_year = 0;
        int album_playcount = 0;
        long album_lastplayed = 0;
        unsigned int album_art = 0, artist_art = 0;
        bool first_track = true;

        if (tagcache_search(&tcs, tag_year))
        {
            tagcache_search_add_filter(&tcs, tag_album,
                                       pfi->album_index[album_idx].seek);

            if (pfi->album_index[album_idx].artist_idx >= 0)
                tagcache_search_add_filter(&tcs, tag_albumartist,
                    pfi->album_index[album_idx].artist_seek);

            while (tagcache_get_next(&tcs, tcs_buf, tcs_bufsz)) {
                int track_year = tagcache_get_numeric(&tcs, tag_year);

                /* Where this album's artwork is cached, from the first of its
                 * tracks. Taken here because this is the only per-album pass
                 * that walks tracks, so the folder costs nothing beyond one
                 * fetch -- resolving it per slide instead is a filtered search
                 * per slide, which is what the carousel used to do. */
                if (first_track)
                {
                    first_track = false;
                    resolve_art_hashes(&tcs, &album_art, &artist_art);
                }
                long track_playcount = tagcache_get_numeric(&tcs, tag_playcount);

                if (track_year > album_year)
                    album_year = track_year;

                /* Negative is tagcache's "could not read it", not a count.
                 *
                 * The lastplayed read is inside this test because every
                 * tagcache_get_numeric() is a separate index lookup, and this
                 * loop runs once per track in the library. A track that has
                 * never been played cannot have a last-played time, so asking
                 * for one would be a third lookup per track to learn nothing
                 * -- and on most libraries the never-played tracks are the
                 * majority. */
                if (track_playcount > 0)
                {
                    long track_lastplayed =
                        tagcache_get_numeric(&tcs, tag_lastplayed);

                    album_playcount += track_playcount;
                    if (track_lastplayed > album_lastplayed)
                        album_lastplayed = track_lastplayed;
                }
            }
        }
        tagcache_search_finish(&tcs);

        pfi->album_index[album_idx].year = album_year;
        pfi->album_index[album_idx].playcount = album_playcount;
        pfi->album_index[album_idx].lastplayed = album_lastplayed;
        pfi->album_index[album_idx].art_hash = album_art;
        pfi->album_index[album_idx].artist_art_hash = artist_art;
    }
    return SUCCESS;
}

/* Roll the album figures up to their artists.
 *
 * Derived from the album index rather than walked out of the database again,
 * which makes it free: every album already carries its own playcount and
 * lastplayed, and the artist it belongs to. A second tagcache pass -- one
 * filtered search per artist, as assign_album_stats() does per album -- would
 * produce the same numbers for real disk work, and the build is already the
 * slowest thing here.
 *
 * album_data.artist_idx and artist_data.name_idx are both offsets into the
 * same artist name blob, so that is the join. Albums whose artist never
 * resolved carry a negative artist_idx and are simply not counted anywhere.
 *
 * Must run after assign_album_stats() and before the duplicate pass trims the
 * album list, or the totals would miss whatever the trim removes. */
static void assign_artist_stats(void)
{
    int a, j;

    if (!pfi->artist_index)
        return;

    for (a = 0; a < pfi->artist_ct; a++)
    {
        pfi->artist_index[a].playcount = 0;
        pfi->artist_index[a].lastplayed = 0;
        pfi->artist_index[a].art_hash = 0;
    }

    for (j = 0; j < pfi->album_ct; j++)
    {
        int artist_idx = pfi->album_index[j].artist_idx;

        if (artist_idx < 0)
            continue;

        for (a = 0; a < pfi->artist_ct; a++)
        {
            if (pfi->artist_index[a].name_idx != artist_idx)
                continue;

            pfi->artist_index[a].playcount += pfi->album_index[j].playcount;
            if (pfi->album_index[j].lastplayed >
                pfi->artist_index[a].lastplayed)
                pfi->artist_index[a].lastplayed =
                    pfi->album_index[j].lastplayed;
            /* Its albums' folders share a parent, which is the artist's own --
             * so the first album to resolve one answers for the artist, and no
             * separate walk is needed to find it. */
            if (pfi->artist_index[a].art_hash == 0)
                pfi->artist_index[a].art_hash =
                    pfi->album_index[j].artist_art_hash;
            break;
        }
    }
}

/**
  Create an index of all artists and albums from the database.
  Also store the artists and album names so we can access them later.
 */
static int create_album_index(void)
{
    static char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    void *buf = pfi->buf;
    size_t buf_size = pfi->buf_sz;

    struct album_data* tmp_album;

    int i, j, last, final, retry, res;

    ALIGN_BUFFER(buf, buf_size, sizeof(long));

    /* Artists */
    res = build_artist_index(&tcs, &buf, &buf_size);
    if (res < SUCCESS)
        return res;

    /* Albums */
    pfi->album_ct = 0;
    pfi->album_len =0;
    pfi->album_untagged_idx = -1;
    pfi->album_untagged_seek = -1;

    /* album_index starts at end of buf it will be rearranged when finalized */
    pfi->album_index = ((struct album_data *)(buf_size + (char *)buf)) - 1;
    /* album_names starts at the beginning of buf */
    pfi->album_names = buf;

    tagcache_search(&tcs, tag_album);
    res = get_tcs_search_res(ePFS_ALBUM, &tcs, &buf, &buf_size);
    tagcache_search_finish(&tcs);
    if (res < SUCCESS)
        return res;

    /* Build artist list for untagged albums */
    res = create_album_untagged(&tcs, &buf_size);

    if (res < SUCCESS)
        return res;

    /* finalize the album index */
    ALIGN_BUFFER(buf, buf_size, alignof(struct album_data));
    tmp_album = (struct album_data*)buf;
    for (i = pfi->album_ct - 1; i >= 0; i--)
        tmp_album[i] = pfi->album_index[-i];

    pfi->album_index = tmp_album;
    /* move buf ptr to end of album_index */
    buf = pfi->album_index + pfi->album_ct;

    /* Assign indices */
    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, pfi->album_ct, STR_STEP_ASSIGNING_ALBUMS);
    for (j = 0; j < pfi->album_ct; j++)
    {
        keep_awake_for_build();

        if (progress_cancel(j, pfi->album_ct, STR_STEP_ASSIGNING_ALBUMS))
            return ERROR_USER_ABORT;

        if (pfi->album_index[j].artist_seek >= 0) { continue; }

        tagcache_search(&tcs, tag_albumartist);
        tagcache_search_add_filter(&tcs, tag_album, pfi->album_index[j].seek);

        last = 0;
        final = pfi->artist_ct;
        retry = 0;
        if (tagcache_get_next(&tcs, tcs_buf, tcs_bufsz))
        {

retry_artist_lookup:
            retry++;
            for (i = last; i < final; i++)
            {
                if (tcs.result_seek == pfi->artist_index[i].seek)
                {
                    int idx = pfi->artist_index[i].name_idx;
                    pfi->album_index[j].artist_idx = idx;
                    pfi->album_index[j].artist_seek = tcs.result_seek;
                    last = i; /* last match, start here next loop */
                    final = pfi->artist_ct;
                    retry = 0;
                    break;
                }
            }
            if (retry > 0 && retry < 2)
            {
                /* no match start back at beginning */
                final = last;
                last = 0;
                goto retry_artist_lookup;
            }
        }
        tagcache_search_finish(&tcs);
    }

    res = assign_album_stats();

    if (res < SUCCESS)
        return res;

    assign_artist_stats();

    /* sort list order to find duplicates */
    qsort(pfi->album_index, pfi->album_ct,
              sizeof(struct album_data), compare_album_artists);

    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, pfi->album_ct, STR_STEP_REMOVING_DUPLICATES);
    /* mark duplicate albums for deletion */
    for (i = 0; i < pfi->album_ct - 1; i++) /* -1 don't check last entry */
    {
        keep_awake_for_build();

        if (progress_cancel(i, pfi->album_ct, STR_STEP_REMOVING_DUPLICATES))
            return ERROR_USER_ABORT;

        int idxi = pfi->album_index[i].artist_idx;
        int seeki = pfi->album_index[i].seek;

        for (j = i + 1; j < pfi->album_ct; j++)
        {
            if (idxi > 0 &&
            idxi == pfi->album_index[j].artist_idx &&
            seeki == pfi->album_index[j].seek)
            {
                pfi->album_index[j].artist_idx = -1;
            }
            else
            {
                i = j - 1;
                break;
            }
        }
    }

    /* now fix the album list order */
    qsort(pfi->album_index, pfi->album_ct,
              sizeof(struct album_data), compare_album_artists);

    /* remove any extra untagged albums
     * extra space is orphaned till restart */
    pfi->album_index += pfi->album_untagged_idx + 1;
    pfi->album_ct -= pfi->album_untagged_idx + 1;

    pfi->buf = buf;
    pfi->buf_sz = buf_size;
    /* artist_index is deliberately kept, where it used to be discarded here.
     * The array itself was always intact -- build_artist_index() finalises it
     * into the buffer ahead of the album data, which nothing above overwrites
     * -- so only the pointer was being thrown away. It is needed now: the
     * artist figures assign_artist_stats() just filled in are saved with the
     * rest of the index and read back by the artist charts. */

    qsort(pfi->album_index, pfi->album_ct,
                          sizeof(struct album_data), compare_albums);

    return (pfi->album_ct > 0) ? 0 : ERROR_NO_ALBUMS;
}

/*Saves the album index into a binary file to be recovered the
 next time PictureFlow is launched*/

static int save_album_index(void){
    int fd = creat(DB_INDEX_FILE,0666);

    struct pf_index_t data;
    memcpy(&data, pfi, sizeof(struct pf_index_t));

    if(fd >= 0)
    {
        memcpy(&data.header, INDEX_HDR, sizeof(pfi->header));

        write(fd, &data, sizeof(struct pf_index_t));

        write(fd, data.artist_names, data.artist_len);
        write(fd, data.album_names, data.album_len);

        write(fd, data.album_index, data.album_ct * sizeof(struct album_data));
        /* The artist array too, which earlier versions did not keep. Written
         * last so the layout above is unchanged; the magic distinguishes the
         * two anyway. */
        write(fd, data.artist_index,
              data.artist_ct * sizeof(struct artist_data));

        close(fd);
        return 0;
    }
    return -1;
}

/* reads data from save file to buffer */
static inline int read2buf(int fildes, void *buf, size_t nbyte){
    int nread;
    nread = read(fildes, buf, nbyte);
    if (nread < (int)nbyte)
        return 0;

    return nread;
}

/* Read only the artist half of the saved index into the caller's buffer.
 *
 * The artist carousel wants the artist list and nothing else, and used to
 * rebuild it from the database on every open. It does not have to: the same
 * list, with its playback figures already summarised, is sitting in the index
 * file. Reading it is a file read against a walk of every album-artist tag.
 *
 * Only the artist parts are kept. The file is written as
 *
 *     header, artist_names, album_names, album_index, artist_index
 *
 * with no padding between -- plain sequential writes -- so the album halves
 * can be stepped over with one seek and never cost the caller any memory.
 * (Alignment in load_album_index() is of the destination buffer, not of file
 * offsets, so it does not enter into the arithmetic here.)
 *
 * SUCCESS, or an ERROR_* leaving the caller to build the list the slow way. */
static int load_artist_index(struct pf_index_t *target,
                             void **buf, size_t *bufsz)
{
    struct pf_index_t data;
    void *b = *buf;
    size_t bsz = *bufsz;
    unsigned int artist_idx_sz;
    int i, fr = open(DB_INDEX_FILE, O_RDONLY);

    if (fr < 0)
        return ERROR_NO_ARTISTS;

    if ((unsigned long)filesize(fr) <= sizeof(data)
        || read(fr, &data, sizeof(data)) != sizeof(data)
        || memcmp(&(data.header), INDEX_HDR, sizeof(data.header)) != 0
        || data.artist_ct == 0)
        goto failure;

    artist_idx_sz = data.artist_ct * sizeof(struct artist_data);
    if (data.artist_len + artist_idx_sz > bsz)
        goto failure;

    if (read2buf(fr, b, data.artist_len) == 0)
        goto failure;
    target->artist_names = b;
    b = (char *)b + data.artist_len;
    bsz -= data.artist_len;

    /* past the album halves, which this caller has no use for */
    if (lseek(fr, data.album_len + data.album_ct * sizeof(struct album_data),
              SEEK_CUR) < 0)
        goto failure;

    ALIGN_BUFFER(b, bsz, alignof(struct artist_data));
    if (read2buf(fr, b, artist_idx_sz) == 0)
        goto failure;
    target->artist_index = b;
    b = (char *)b + artist_idx_sz;
    bsz -= artist_idx_sz;

    close(fr);

    /* Same sanity check load_album_index() makes: a name offset past the end
     * of the blob means the file is not what it claims to be. */
    for (i = 0; i < data.artist_ct; i++)
    {
        if (target->artist_index[i].name_idx >= (int)data.artist_len)
            return ERROR_NO_ARTISTS;
    }

    target->artist_ct = data.artist_ct;
    target->artist_len = data.artist_len;
    target->album_ct = 0;        /* no album list on this path */
    *buf = b;
    *bufsz = bsz;
    return SUCCESS;

failure:
    close(fr);
    return ERROR_NO_ARTISTS;
}

int db_index_load_artists(struct pf_index_t *target,
                          void **buf, size_t *bufsz)
{
    int ret = wait_for_background();

    if (ret != SUCCESS)
        return ret;

    /* Held for the read: the background pass rewrites this file in place, so
     * without it a pass finishing mid-read would be seen as a corrupt index
     * and thrown away for a full rebuild. */
    mutex_lock(&build_mutex);
    ret = load_artist_index(target, buf, bufsz);
    mutex_unlock(&build_mutex);

    return ret;
}

/*Loads the album_index information stored in the hard drive*/
static int load_album_index(void){

    int i, fr = open(DB_INDEX_FILE, O_RDONLY);
    struct pf_index_t data;

    void *bufstart = pfi->buf;
    unsigned int bufstart_sz = pfi->buf_sz;

    void* buf = pfi->buf;
    size_t buf_size = pfi->buf_sz;

    unsigned int name_sz, album_idx_sz, artist_idx_sz;
    int album_idx, artist_idx;

    if (fr >= 0){
        const unsigned long fsize = filesize(fr);
        if (fsize > sizeof(data))
        {
            if (read(fr, &data, sizeof(data)) == sizeof(data) &&
                memcmp(&(data.header), INDEX_HDR, sizeof(data.header)) == 0)
            {
                name_sz = data.artist_len + data.album_len;
                album_idx_sz = data.album_ct * sizeof(struct album_data);
                artist_idx_sz = data.artist_ct * sizeof(struct artist_data);

                if (name_sz + album_idx_sz + artist_idx_sz > bufstart_sz)
                    goto failure;

                /* lseek(fr, sizeof(data) + 1, SEEK_SET); */
                /* artist names */
                if (read2buf(fr, buf, data.artist_len) == 0)
                    goto failure;

                data.artist_names = buf;
                buf = (char *)buf + data.artist_len;
                buf_size -= data.artist_len;

                /* album names */
                if (read2buf(fr, buf, data.album_len) == 0)
                    goto failure;

                data.album_names = buf;
                buf = (char *)buf + data.album_len;
                buf_size -= data.album_len;

                /* index of album names */
                ALIGN_BUFFER(buf, buf_size, alignof(struct album_data));
                if (read2buf(fr, buf, album_idx_sz) == 0)
                    goto failure;

                data.album_index = buf;
                buf = (char *)buf + album_idx_sz;
                buf_size -= album_idx_sz;

                /* index of artists, with their playback figures */
                ALIGN_BUFFER(buf, buf_size, alignof(struct artist_data));
                if (artist_idx_sz > 0
                    && read2buf(fr, buf, artist_idx_sz) == 0)
                    goto failure;

                data.artist_index = buf;
                buf = (char *)buf + artist_idx_sz;
                buf_size -= artist_idx_sz;

                close(fr);

                /* sanity check loaded data */
                for (i = 0; i < data.album_ct; i++)
                {
                    album_idx = data.album_index[i].name_idx;
                    artist_idx = data.album_index[i].artist_idx;
                    if (album_idx >= (int) data.album_len ||
                        artist_idx >= (int) data.artist_len)
                    {
                        goto failure;
                    }
                }

                memcpy(pfi, &data, sizeof(struct pf_index_t));
                pfi->buf = buf;
                pfi->buf_sz = buf_size;

                qsort(pfi->album_index, pfi->album_ct,
                          sizeof(struct album_data), compare_albums);

                return 0;
            }
        }
    }

failure:
    /* Foreground only -- the background pass owns no screen, and a failed
     * load there simply means it goes on to build one. */
    if (!building_bg)
        splash(HZ/2, "Failed to load index");
    if (fr >= 0)
        close(fr);

    pfi->buf = bufstart;
    pfi->buf_sz = bufstart_sz;
    pfi->artist_ct = 0;
    pfi->album_ct = 0;
    return -1;

}

/* carousel_model.build_index for the album model: reuse the on-disk index when
 * it matches the current cache version, otherwise rebuild it (and persist it).
 * Returns SUCCESS or one of the ERROR_* codes. */
/* Reuse the saved index, or build one and persist it, into the caller's
 * struct and the caller's memory. Which memory is the only thing that varies
 * between a build the carousel asks for and one a background pass does. */
static int build_into(struct pf_index_t *target, void *buf, size_t buf_sz,
                      bool background)
{
    int ret = SUCCESS;

    mutex_lock(&build_mutex);
    idx_abort = false;

    /* Set inside the lock, and cleared before releasing it, so it describes
     * exactly who is building right now. Set it outside and a foreground build
     * can start while it still reads "background", then abort itself on the
     * next tick's idx_abort -- which closed the carousel mid-open. */
    building_bg = background;

    pfi = target;
    pfi->buf = buf;
    pfi->buf_sz = buf_sz;

    /* The background pass is the one that rebuilds. bg_task runs it only when
     * the library has changed or a trigger fired, so when it does run there is
     * nothing to reuse and it always builds. Every other caller reads the saved
     * index, and builds only when there is none to read.
     *
     * This used to ask pf_cfg.cache_version instead, which is the carousel's
     * own state: loaded from its config file by the carousel's init and by
     * nothing else. A caller that had not opened the carousel this boot -- the
     * album charts, Random album -- therefore read it as CACHE_REBUILD and
     * walked the whole database on every invocation, with a perfectly good
     * index sitting on disk unread. A stale *format* is caught by INDEX_HDR,
     * which is what that magic is for. */
    if (background || load_album_index() < 0)
    {
        set_working_for_build(true);   /* the "working" LED while (re)building */
        ret = create_album_index();
        set_working_for_build(false);

        if (ret == 0)
        {
            pf_cfg.cache_version = CACHE_REBUILD;
            pf_config_save();
            /* Only the foreground caller may report this: the background pass
             * owns no screen, and there is nothing the user could do anyway --
             * the carousel will simply build it again when asked. */
            if (save_album_index() < 0 && !building_bg)
                splash(HZ, "Could not write index");
        }
    }

    pfi = NULL;
    building_bg = false;
    mutex_unlock(&build_mutex);
    return ret;
}

int db_index_build_into(struct pf_index_t *target, void *buf, size_t buf_sz)
{
    return build_into(target, buf, buf_sz, false);
}

/* Wait out a background build rather than restarting its work here.
 *
 * Interrupting it and rebuilding in the foreground was the original design,
 * from when both cost the same. They do not: this path redraws the status bar
 * for every album and the background one does not, which makes it several
 * times slower. So the quickest way to get an index is to let the pass that is
 * already running finish, then read what it wrote.
 *
 * Polls rather than blocking on the mutex, so the screen stays alive and can
 * be left. */
static int wait_for_background(void)
{
    if (!db_index_is_busy())
        return SUCCESS;

    /* Nothing on screen at all if it finishes promptly. */
    splash_progress_set_delay(HZ / 2);

    while (db_index_is_busy())
    {
        splash_progress(bg_done, bg_total > 0 ? bg_total : 1,
                        "%s", str(LANG_WAIT));

        if (get_action(CONTEXT_STD, HZ / 10) == ACTION_STD_CANCEL)
        {
            /* Leaving, not switching to the slow path: tell it to stop so it
             * is not still running behind whatever comes next. */
            idx_abort = true;
            return ERROR_USER_ABORT;
        }
    }

    return SUCCESS;
}

/* carousel_model.build_index: build into the engine's own index and the
 * buffer it has already claimed. */
int db_index_build(void)
{
    int ret = wait_for_background();
    if (ret != SUCCESS)
        return ret;

    return db_index_build_into(&pf_idx, pf_idx.buf, pf_idx.buf_sz);
}

/* carousel_model.build_index for the artist model: the artist half only, with
 * no album list, into the caller's index and the caller's memory.
 *
 * The locking is the point of this function existing. pfi is one shared slot
 * and build_mutex is what makes pointing it somewhere safe -- so the artist
 * model cannot reach build_artist_index() directly, however tempting, because
 * a background pass holding pfi would resume after a yield writing through
 * whatever the artist build had left there. Same wait, same lock, same
 * clear-before-unlock as build_into(). */
/* Artist figures for a build that has no album list to roll up from.
 *
 * The full build derives them from the albums for free (assign_artist_stats());
 * this cannot, so it goes to the database -- one filtered search per artist,
 * the same shape assign_album_stats() uses per album. The two agree by
 * construction: an artist's total is every track filed under them either way.
 *
 * Only worth its cost when something is going to sort on it, hence the opt-in
 * at the call site. Artists are far fewer than albums, so even then it is a
 * fraction of what a full build does. */
/* Fill in each artist's folder, and optionally their playback figures, by
 * walking the database.
 *
 * The artist-only build has no album list to derive either from -- that is what
 * assign_artist_stats() does on the full build -- so this is the one path that
 * has to ask. The folder is always resolved: it is what the artist carousel
 * shows a photo from, and skipping it here would leave every artist without one
 * until a full build wrote an index. The figures are only summed when something
 * is going to sort on them, since that is the part that reads every track. */
static void assign_artist_art_and_stats(struct tagcache_search *tcs,
                                        bool with_stats)
{
    char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    int a;

    for (a = 0; a < pfi->artist_ct; a++)
    {
        int playcount = 0;
        long lastplayed = 0;
        unsigned int artist_art = 0;
        bool first_track = true;

        keep_awake_for_build();
        if (progress_cancel(a, pfi->artist_ct, STR_STEP_ARTIST_STATS))
            break;

        if (tagcache_search(tcs, tag_playcount))
        {
            tagcache_search_add_filter(tcs, tag_albumartist,
                                       pfi->artist_index[a].seek);

            while (tagcache_get_next(tcs, tcs_buf, tcs_bufsz))
            {
                if (first_track)
                {
                    first_track = false;
                    resolve_art_hashes(tcs, NULL, &artist_art);
                    /* Nothing else here needs a second track. */
                    if (!with_stats)
                        break;
                }

                long n = tagcache_get_numeric(tcs, tag_playcount);

                if (n > 0)
                {
                    long when = tagcache_get_numeric(tcs, tag_lastplayed);
                    playcount += n;
                    if (when > lastplayed)
                        lastplayed = when;
                }
            }
        }
        tagcache_search_finish(tcs);

        pfi->artist_index[a].art_hash = artist_art;
        if (with_stats)
        {
            pfi->artist_index[a].playcount = playcount;
            pfi->artist_index[a].lastplayed = lastplayed;
        }
    }
}

int db_index_build_artists(struct pf_index_t *target,
                              struct tagcache_search *tcs,
                              void **buf, size_t *bufsz, bool with_stats)
{
    int ret = wait_for_background();

    if (ret != SUCCESS)
        return ret;

    mutex_lock(&build_mutex);
    pfi = target;
    ret = build_artist_index(tcs, buf, bufsz);
    if (ret >= SUCCESS)
        assign_artist_art_and_stats(tcs, with_stats);
    pfi = NULL;
    mutex_unlock(&build_mutex);

    return ret;
}

/* ---------------------------------------------------------------------------
 * Building it in the background
 *
 * The index only goes stale when the database changes, so the work can be done
 * once after a scan settles rather than when someone opens the carousel and is
 * made to wait for it. The carousel is unchanged: it still asks for the index
 * the same way, and simply finds it already written most of the time.
 *
 * The pass runs on the same terms as the artwork cache thread it sits beside,
 * and for the same reasons -- so both are expressed as a bg_task and the terms
 * themselves live in system/bg_task.c: only while the database is usable and
 * idle, and only once the entry count has stopped moving, so a scan in
 * progress is left alone.
 *
 * This one outranks the artwork cache. It finishes in seconds where a full
 * artwork pass takes minutes, the carousel can be sat waiting on it, and it
 * needs a 384K allocation that an artwork pass in flight has effectively
 * spoken for -- so the artwork pass is the one that gives way.
 * ------------------------------------------------------------------------ */

#define IDX_STACK_SIZE (DEFAULT_STACK_SIZE + 0x2000)
static long idx_stack[IDX_STACK_SIZE / sizeof(long)];
static const char idx_thread_name[] = "albumidx";
static unsigned int idx_thread_id;
static struct event_queue idx_queue;

/* Enough to build a large library's index; the app buffer the carousel builds
 * into is 512K, and this is the background equivalent. Anything smaller just
 * fails with ERROR_BUFFER_FULL, which leaves the carousel to build inline as
 * it always has. */
#define IDX_BUILD_BUFSZ (384 * 1024)


bool db_index_is_busy(void)
{
    return building_bg;
}

const char *db_index_activity(void)
{
    return bg_step ? bg_step : "";
}

void db_index_progress(int *done, int *total)
{
    *done = bg_done;
    *total = bg_total;
}

/* The database entry count the last background build covered.
 *
 * Deliberately not pf_cfg.cache_version: that means "the on-disk formats are
 * current", is set by the carousel's prepare callback, and also gates the
 * slide placeholder's regeneration. Borrowing it here would both rebuild
 * forever (nothing sets it on this path) and quietly change what the carousel
 * does with it. This is the same marker the artwork cache keeps for the same
 * reason -- what was the library like when we last finished. */
#define IDX_DONE_FILE ROCKBOX_DIR "/db_index.log"

/* Read the index without a buffer of your own; see album_index.h.
 *
 * Here rather than in the caller because IDX_BUILD_BUFSZ is here: how much a
 * build needs is the builder's business, and a second copy of that number
 * elsewhere would be one to keep in step. Same allocation shape as
 * background_build() below, for the same reasons -- ask outright rather than
 * checking core_allocatable() first, since core_alloc() shrinks what will
 * shrink and that is usually the only way this gets memory, and pin it because
 * a handle with no ops is movable while the builder yields throughout. */
int db_index_acquire(struct pf_index_t *target, int *handle)
{
    void *buf;
    int ret;

    /* Same wait the carousel does, and for the same reason: build_into() takes
     * build_mutex, so without this a caller arriving while the background pass
     * is running blocks on it with a dead screen and no way out. Here it shows
     * the pass's progress and can be left. */
    ret = wait_for_background();
    if (ret != SUCCESS)
        return ret;

    *handle = core_alloc(IDX_BUILD_BUFSZ);
    if (*handle <= 0)
        return ERROR_BUFFER_FULL;

    buf = core_get_data_pinned(*handle);
    memset(target, 0, sizeof(*target));

    ret = db_index_build_into(target, buf, IDX_BUILD_BUFSZ);
    if (ret < SUCCESS)
    {
        db_index_release(*handle);
        *handle = 0;
    }
    return ret;
}

void db_index_release(int handle)
{
    if (handle <= 0)
        return;
    core_put_data_pinned(core_get_data(handle));
    core_free(handle);
}

/* bg_task.artifact_ok: the marker matching the entry count is not on its own
 * enough -- the index file it refers to has to still be there, and be in a
 * format this build can read. The magic is checked because the library does not
 * change when the firmware does: after an upgrade that bumped INDEX_HDR the
 * marker still matches, so without this the background pass would leave a file
 * it can no longer read in place and the next caller that wanted the index
 * would rebuild it in the foreground, progress bar and all. Cheap: the header
 * is four bytes and nothing else is read. */
static bool saved_index_present(void)
{
    uint32_t header;
    int fd = open(DB_INDEX_FILE, O_RDONLY);
    bool ok;

    if (fd < 0)
        return false;

    ok = read(fd, &header, sizeof(header)) == sizeof(header)
      && memcmp(&header, INDEX_HDR, sizeof(header)) == 0;
    close(fd);
    return ok;
}

/* bg_task.run: one background build, into memory of its own. The result is the
 * file on disk; the index it builds in RAM is thrown away, because the
 * carousel will read it back for itself when it needs it.
 *
 * Returns false if it could not finish, which leaves the marker alone so the
 * next tick tries again.
 *
 * Nothing here touches pf_cfg. It used to mark the carousel's cache_version
 * current, because build_into() consulted that flag and would otherwise have
 * rebuilt what this pass had just written. build_into() no longer asks, and
 * the flag is only about the carousel's slide cache -- which this pass does
 * not build -- so asserting anything about it from here was both untrue and,
 * on a boot where the carousel had never loaded its config, a save of zeroes
 * over the resume position. The carousel's own prepare callback owns it. */
static bool background_build(void)
{
    struct pf_index_t local;
    size_t bufsz = IDX_BUILD_BUFSZ;
    int handle;
    void *buf;
    int ret;

    /* The carousel makes this directory when it starts up; a build that runs
     * before it ever has would otherwise have nowhere to write the index. */
    if (!dir_exists(CACHE_PREFIX) && mkdir(CACHE_PREFIX) < 0)
        return false;

    /* Ask outright rather than checking core_allocatable() first: that reports
     * space already free, and on a player with the audio buffer claimed there
     * is rarely any. core_alloc() shrinks what will shrink to make room, which
     * is the only way this ever gets memory. Failure just means try later --
     * and by then the artwork pass, which is what usually has the memory, will
     * have seen this task waiting and let go of it. */
    handle = core_alloc(bufsz);
    if (handle <= 0)
        return false;

    /* Pinned: a handle with no ops is movable, and the builder yields all the
     * way through -- to the database, and to be nice to playback. */
    buf = core_get_data_pinned(handle);

    memset(&local, 0, sizeof(local));
    bg_done = bg_total = 0;

    ret = build_into(&local, buf, bufsz, true);

    bg_done = bg_total = 0;

    core_put_data_pinned(buf);
    core_free(handle);

    /* Only on a clean finish: an aborted or failed pass must be retried, not
     * recorded as covering this library. Saying so is enough -- bg_task writes
     * the marker itself once we return true. */
    return ret == SUCCESS;
}

struct bg_task db_index_task =
{
    .done_file   = IDX_DONE_FILE,
    .rank        = BG_RANK_INDEX,
    .run         = background_build,
    .artifact_ok = saved_index_present,
};

static void idx_thread(void)
{
    while (1)
        bg_task_tick(&db_index_task, &idx_queue);
}

void db_index_init(void)
{
    mutex_init(&build_mutex);
    queue_init(&idx_queue, true);
    bg_task_init(&db_index_task);
    idx_thread_id = create_thread(idx_thread, idx_stack, sizeof(idx_stack), 0,
                                  idx_thread_name IF_PRIO(, PRIORITY_BACKGROUND)
                                  IF_COP(, CPU));
    (void)idx_thread_id;
}

void db_index_invalidate(void)
{
    /* Forget what the last pass covered, so the next tick rebuilds. Used by
     * the carousel's rebuild/update, which otherwise only reach its own build
     * and would leave the background pass thinking it was up to date.
     *
     * An update rather than a rebuild: there is nothing here to purge that the
     * pass does not overwrite anyway, the index being a single file. */
    bg_task_update(&db_index_task);
}
