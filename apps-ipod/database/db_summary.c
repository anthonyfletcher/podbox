/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The database index: the flat album and artist list derived from tagcache.
 *
 * Built by walking tagcache into a caller-supplied struct db_summary_t and a
 * caller-supplied buffer, and persisted to db_summary.dat so later reads get
 * it back instead of rescanning. It carries no artwork -- only names, years,
 * playback figures and the taglist seeks needed to navigate into the database
 * -- so it goes stale when the database changes, not when files on disk do.
 *
 * Nothing here knows about the screens that read it. Whose index is being
 * built, and what that screen does afterwards, is the caller's business.
 *
 * It began as the carousel's own list and is no longer only that: the album
 * and artist charts read the same index for their rankings, which is why it
 * is named for the database rather than for any one screen. Building it is
 * nobody's screen's job, and is intended to happen while none of them is up.
 *
 * Parts, in order:
 *   - progress reporting and cancellation
 *   - name keys, and writing entries into the index buffer
 *   - the tagcache walks that populate it
 *   - the play log
 *   - carrying figures across a rebuild, and create_album_index() driving it
 *   - the on-disk form: saving, and the album and artist loaders
 *   - db_summary_build_into(), which reuses the saved index or rebuilds it
 *   - the background pass: its progress reporting and the acquire/release pair
 *   - reading single records out of the saved file, without holding it
 *   - the background pass again: its staleness gate, its run and its thread
 *   - playing an album
 ****************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "string-extra.h"
#include "config.h"
#include "system/hash.h"
#include "system.h"          /* ALIGN_BUFFER/alignof */
#include "rbpaths.h"
#include "kernel.h"
#include "file.h"
#include "lang.h"
#include "database/tagcache.h"
#include "database/db_spoken.h"    /* which albums and artists are books */
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
#include "system/debug_log.h"         /* what the carry-over managed to skip */
#include "system/app_buffer.h"        /* scratch for ordering an album */
#include "system/app_util.h"          /* warn_on_pl_erase */
#include "playlist/playlist.h"
#include "core_alloc.h"              /* background build buffer */
#include "usb.h"                     /* SYS_USB_CONNECTED handling */
#include "db_summary.h"

/* The index file, and the four-byte magic at its start. It holds albums and
 * artists alike, and is read by more than the carousel now, so it lives beside
 * the other state in ROCKBOX_DIR rather than in the carousel's own folder --
 * which keeps only what is genuinely the carousel's, its slide cache and empty
 * slide. Regenerated if absent. */
#define DB_SUMMARY_FILE ROCKBOX_DIR "/db_summary.dat"
/* The index is always rewritten whole, so it is built here and renamed over
 * the real file rather than written into it. See save_album_index(). */
#define DB_SUMMARY_TMP  DB_SUMMARY_FILE ".tmp"
/* Plays since the summary was written. */
#define DB_PLAYS_FILE ROCKBOX_DIR "/db_summary.plays"

/* One finished track. The serial is tagcache's, which is what the index's own
 * watermark is compared against, and doubles as the lastplayed value.
 *
 * Trap: tagcache_increase_serial() returns the value *before* the increment,
 * while the watermark is the counter as it stands -- the next value to be
 * handed out. So a record is already counted in an index only when its serial
 * is strictly below that watermark. Testing <= instead discards the first
 * play after every build, and no other. */
struct play_rec
{
    int32_t  serial;
    uint32_t album_key;
    uint32_t artist_key;
};

/* Past this the log stops being cheaper to replay than to fold in, so the
 * background pass is asked for. */
#define PLAY_LOG_MAX 512
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
 *                 cached thumbnail.
 *   PFIH -> PFII: both gained key, and the header gained the commitid/serial
 *                 watermarks.
 *   PFII -> PFIJ: the header gained the deleted count.
 *   PFIJ -> PFIK: artist_ct/album_ct are int32_t rather than uint16_t, which
 *                 moves every field after them. They have to be: a 16-bit
 *                 count wraps silently past 65535 albums. */
#define INDEX_HDR "PFIK"

enum ePFS { ePFS_ARTIST = 0, ePFS_ALBUM };

/* Whether a header just read off the disk describes something that could fit
 * in `bufsz` -- and, more to the point, whose counts survive being multiplied
 * by a struct size.
 *
 * Every reader of the file has to ask this before it multiplies or adds any of
 * these four fields, because the failure is not a rejected file: a count large
 * enough to overflow the 32-bit size arithmetic wraps to a *small* number, the
 * reads sized from it are satisfied, and the loop that then walks the real
 * count runs off the end of the allocation. carry_over_prepare() writes in
 * that loop.
 *
 * The magic does not cover this. It is four bytes at the front of the file,
 * and a header scribbled by a bad unmount keeps them while the counts behind
 * them turn to noise. Bounding each count by what the buffer could hold at
 * best is enough: the products below cannot then exceed bufsz, so the sums the
 * callers make of them cannot wrap either. */
static bool header_fits(const struct db_summary_t *d, size_t bufsz)
{
    return d->album_ct  >= 0
        && d->artist_ct >= 0
        && (size_t)d->album_ct  <= bufsz / sizeof(struct album_data)
        && (size_t)d->artist_ct <= bufsz / sizeof(struct artist_data)
        && d->album_len  <= bufsz
        && d->artist_len <= bufsz;
}

/* Shared by every walk below; the models keep their own where they need one. */
static struct tagcache_search tcs;

/* The index currently being built, and the buffer it is being built into.
 *
 * A file static rather than a parameter threaded through the ten functions
 * that touch it, and deliberately singular: only one build may run at a time,
 * whoever asked for it. That lets the same code serve the carousel building
 * into the app buffer it has claimed, and a background build into memory of
 * its own -- the allocation differs, the builder does not. */
static struct db_summary_t *pfi;

/* Serialises the two callers of the builder. The carousel blocks here rather
 * than starting a second build; because it has usually just asked for one, it
 * sets idx_abort first so the background pass gives up promptly instead of
 * making the screen wait out a full scan. */
static struct mutex build_mutex;

/* True while the *background* pass owns the builder. It changes two things
 * the builder must not do off the main thread: draw, and read the keypad. */
static bool building_bg;
static volatile bool idx_abort;

/* Which side is about to want the builder.
 *
 * building_bg cannot answer that, because it is set inside build_mutex: between
 * a foreground caller finding it clear and that caller reaching the lock, the
 * background pass can take the lock, and the caller then blocks on it with a
 * dead screen and no way out -- precisely what wait_for_background() exists to
 * prevent. These two are set before either side reaches for the lock, so the
 * question can be asked while the answer still means something.
 *
 * Each side announces itself and only then looks at the other, so a tie is
 * resolved rather than missed: at least one of them sees the other's mark. The
 * background pass is the side that gives way, because nothing is waiting on it
 * and its next tick is a few seconds away. The reverse case costs nothing --
 * a background thread blocking on the mutex is invisible; only the thread
 * holding the screen must never do it. */
static volatile bool bg_claimed;
static long fg_wanted_tick;

/* Long enough to cover a foreground caller getting from its announcement to
 * the lock. It is refreshed while that caller waits, so it does not have to
 * cover the waiting itself; and going stale only ever risks the background
 * pass blocking on the mutex, which is harmless. */
#define FG_CLAIM_TICKS (HZ / 2)

static bool foreground_wants_builder(void)
{
    return TIME_BEFORE(current_tick, fg_wanted_tick + FG_CLAIM_TICKS);
}

/* Last progress the background pass reported; read by anything showing it.
 * bg_step is the step name the builder was already passing to the progress
 * bar and the background path was already throwing away -- keeping the
 * pointer is the whole cost of reporting what the pass is doing. */
static int bg_done, bg_total;
static const char *bg_step;

static void draw_progressbar(int step, int count, char *msg);
static int wait_for_background(void);
static bool bg_should_stop(void);

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
        return idx_abort || bg_should_stop();
    }

    const struct text_message prompt = {
            (const char*[]) {"Quit?", "Progress will be lost"}, 2};

    int action = get_action(CONTEXT_STD,TIMEOUT_NOBLOCK);

    /* While a foreground build runs, this get_action() is the only thing
     * reading the button queue -- and a USB connect arrives on it as an
     * action. That queue is in the broadcast list, so the storage handover
     * counts an acknowledgement from it, and default_event_handler() is what
     * sends it. Dropping the action here leaves the handover waiting and the
     * host looking at a drive with no medium in it for the length of the
     * build. Give up afterwards: the library this was reading may have changed
     * while the host had the disk. */
    if (default_event_handler(action) == SYS_USB_CONNECTED)
        return true;

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

static int compare_album_artists (const void *a_v, const void *b_v)
{
    uint32_t a = ((struct album_data *)a_v)->artist_idx;
    uint32_t b = ((struct album_data *)b_v)->artist_idx;
    return (int)(a - b);
}

/* The order the index is left in, and therefore the order it is saved in.
 *
 * Artist, then album name -- both offsets into name blobs that were written in
 * the order tagcache walked them, so this is a total order over the entries
 * and reading no setting. That matters twice: the file is reproducible from
 * the same library, and the charts, which keep array order between entries
 * whose figures tie, show the same tied albums after a rebuild as before it.
 * qsort is not stable, so leaving the ties to it would give neither. */
static int compare_index_order(const void *a_v, const void *b_v)
{
    const struct album_data *a = a_v;
    const struct album_data *b = b_v;

    if (a->artist_idx != b->artist_idx)
        return (int)((uint32_t)a->artist_idx - (uint32_t)b->artist_idx);
    return (int)((uint32_t)a->name_idx - (uint32_t)b->name_idx);
}

/* FNV-1a over one name, or two joined by a NUL so that ("ab", "c") and
 * ("a", "bc") do not land on the same value. Pass NULL for 'b' to key on a
 * single name.
 *
 * 32 bits: a collision merges two entries' playback figures, which is wrong
 * but not corrupt, and the next full rebuild reads the real numbers back out
 * of tagcache anyway. */
static uint32_t name_key(const char *a, const char *b)
{
    uint32_t h = FNV1A_BASIS;

    for (; a && *a; a++)
        h = fnv1a_byte(h, (unsigned char)*a);

    h = fnv1a_byte(h, 0);   /* the joining NUL */

    for (; b && *b; b++)
        h = fnv1a_byte(h, (unsigned char)*b);

    return h;
}

/* Fill in every entry's key, once the album list is final. Left to the end
 * rather than done as entries are written because an album's artist is not
 * known until the artist pass has resolved it.
 *
 * A nameless album keys to 0, meaning "do not match this against anything".
 * The list really does hold a few -- four on the library this was written
 * against, sorted to the front with no name, no artist and no year -- and
 * they would otherwise all share one key and be handed each other's figures. */
static void assign_keys(void)
{
    int i;

    for (i = 0; i < pfi->album_ct; i++)
    {
        struct album_data *a = &pfi->album_index[i];
        const char *name = pfi->album_names + a->name_idx;
        const char *artist = a->artist_idx >= 0
                           ? pfi->artist_names + a->artist_idx : NULL;

        a->key = *name ? name_key(name, artist) : 0;
    }
}

static void write_album_index(int idx, int name_idx,
                              long album_seek, int artist_idx, long artist_seek)
{
    pfi->album_index[idx].name_idx = name_idx;
    pfi->album_index[idx].key = 0;
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
    pfi->artist_index[-pfi->artist_ct].key = 0;
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

/* Spoken word is kept out of the index when the setting says so, which is what
 * reaches Album covers, Artist portraits, Random album and the charts -- all
 * four read this index rather than the database.
 *
 * Dropped entry by entry rather than by a clause on the search, because a
 * clause moves tagcache off the tag file and onto the master index (see
 * build_lookup_list() there): the names would then be collected in database
 * order, and this index is ordered by their offsets into the name blobs. The
 * order of every screen reading it is that order.
 *
 * The index is written to disk, so a change to the setting has to invalidate
 * it: nothing here notices, the saved file's own staleness checks being about
 * the database rather than about what was asked of it. See
 * db_summary_invalidate(), which the setting's callback calls. */
static bool exclude_spoken(const struct tagcache_search *tcs)
{
    return global_settings.segregate_audiobooks
        && db_spoken_group_is_book(tcs->type, tcs->result_seek);
}

/* Start a search with the table it will be filtered against already built.
 *
 * The build has to happen before the search -- it runs searches of its own,
 * which cannot nest -- and both halves have to name one tag, which is why the
 * search is started here rather than beside the call: exclude_spoken() above
 * then reads that tag back off the search and the two cannot drift apart.
 *
 * False when the table would not build, which is a reason to come back later
 * rather than to carry on. This index is written to disk and its own staleness
 * checks are about the database, so an unfiltered one built now would stand as
 * the answer until the setting is toggled or the database commits. */
static bool search_excluding_spoken(struct tagcache_search *tcs, int tag)
{
    if (global_settings.segregate_audiobooks && db_spoken_group_tag(tag)
        && !db_spoken_group_ensure(tag))
        return false;

    tagcache_search(tcs, tag);
    return true;
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
        if (exclude_spoken(tcs))
            continue;

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
                /* Per album, as the same walk in create_album_index() does.
                 * Left at 2 by a miss, every later album searches whatever
                 * truncated range that miss left behind and never wraps. */
                retry = 0;

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
 * held and pfi is set -- db_summary_build_artists() is how an outside caller
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

    if (!search_excluding_spoken(tcs, tag_albumartist))
        return ERROR_USER_ABORT;

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

    /* Here rather than with the albums' keys, so the artist-only build gets
     * them too -- that path never reaches create_album_index(). */
    for (i = 0; i < pfi->artist_ct; i++)
    {
        const char *name = pfi->artist_names + pfi->artist_index[i].name_idx;

        pfi->artist_index[i].key = *name ? name_key(name, NULL) : 0;
    }

    if (res == SUCCESS)
    {
        if (pfi->artist_ct > 0)
            res = pfi->artist_ct;
        else
            res = ERROR_NO_ALBUMS;
    }

    return res;
}

/* ---------------------------------------------------------------------------
 * The play log
 *
 * A play changes an album's figures and nothing else about it, so rebuilding
 * the index for one is absurd -- and rebuilding is the only thing that ever
 * updated them. Instead each finished track appends twelve bytes here, and a
 * reader applies whatever arrived after the index was written.
 *
 * Both keys come from the track's own tags, so a play costs no database work
 * at all: an append, at a moment when tagcache is already writing runtime
 * data to the same disk.
 * ------------------------------------------------------------------------ */

void db_summary_log_play(const char *album, const char *albumartist,
                       long serial)
{
    struct play_rec rec;
    off_t before;
    int fd;

    /* Nothing to attribute it to. assign_keys() gives a nameless album a key
     * of 0 and matches nothing against it, so a record like this could never
     * be applied to anything. */
    if (!album || !*album)
        return;

    rec.serial = serial;
    rec.album_key = name_key(album, albumartist);
    rec.artist_key = albumartist && *albumartist
                   ? name_key(albumartist, NULL) : 0;

    fd = open(DB_PLAYS_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;

    /* A short write here is the one thing that can wreck the whole log. The
     * file has no record markers -- readers step through it at a fixed stride
     * from offset 0 -- so a partial record leaves every record appended after
     * it at the wrong offset, and there is nothing to resynchronise from. The
     * figures would then be credited to whichever album the misaligned bytes
     * happen to name, silently and for good.
     *
     * So put the file back the way it was and drop this play instead. Losing
     * one play is not worth noticing; losing the alignment is permanent. */
    before = filesize(fd);
    if (write(fd, &rec, sizeof(rec)) != (ssize_t)sizeof(rec))
    {
        if (before >= 0)
            ftruncate(fd, before);
        close(fd);
        return;
    }

    /* Ask for a pass once it has grown enough that folding it into the index
     * beats replaying it on every read. Cheap to say so: a build that finds
     * only plays changed carries every album's figures across bar the ones
     * named here. */
    if (filesize(fd) > (long)(PLAY_LOG_MAX * sizeof(rec)))
        bg_task_update(&db_summary_task);

    close(fd);
}

/* The play log, opened for reading, or -1.
 *
 * A log whose length is not a whole number of records has had a write cut
 * short at some point, and everything after that point is misaligned (see
 * db_summary_log_play()). Reading it anyway credits plays to the wrong albums,
 * so it is discarded instead: the figures already folded into the saved index
 * are untouched, and only the plays not yet folded in are lost.
 *
 * Belt and braces -- the writer above is what stops this arising -- but it is
 * the difference between losing a few plays and quietly corrupting every
 * album's figures, and both readers get it by calling this. */
static int open_play_log(void)
{
    int fd = open(DB_PLAYS_FILE, O_RDONLY);
    off_t size;

    if (fd < 0)
        return -1;

    size = filesize(fd);
    if (size < 0 || (size % (off_t)sizeof(struct play_rec)) != 0)
    {
        close(fd);
        remove(DB_PLAYS_FILE);
        return -1;
    }

    return fd;
}

/* Apply the log to an index just read from disk. Records at or below its
 * watermark are already counted in the figures it was saved with.
 *
 * Linear over the arrays rather than sorted-and-searched: a few hundred
 * records against a few hundred albums is nothing, and the arrays are in
 * display order by the time anyone wants them. */
static void replay_plays(struct db_summary_t *idx)
{
    struct play_rec batch[32];
    int fd = open_play_log();
    ssize_t got;

    if (fd < 0)
        return;

    while ((got = read(fd, batch, sizeof(batch))) >= (ssize_t)sizeof(batch[0]))
    {
        int n = got / sizeof(batch[0]);
        int i, j;

        for (i = 0; i < n; i++)
        {
            if (batch[i].serial < idx->serial)
                continue;

            for (j = 0; j < idx->album_ct; j++)
            {
                struct album_data *a = &idx->album_index[j];

                if (a->key != batch[i].album_key || a->key == 0)
                    continue;
                a->playcount++;
                if (batch[i].serial > a->lastplayed)
                    a->lastplayed = batch[i].serial;
                break;
            }

            for (j = 0; j < idx->artist_ct; j++)
            {
                struct artist_data *r = &idx->artist_index[j];

                if (r->key != batch[i].artist_key || r->key == 0)
                    continue;
                r->playcount++;
                if (batch[i].serial > r->lastplayed)
                    r->lastplayed = batch[i].serial;
                break;
            }
        }
    }

    close(fd);
}

/* ---------------------------------------------------------------------------
 * Carrying figures across a rebuild
 *
 * Summarising an album costs a filtered tagcache search, and the build does
 * one per album whatever changed. An album whose tracks are the same as when
 * the last index was written already has its answer in that file.
 *
 * The seeks in it are worthless by then -- a commit that adds a track re-sorts
 * the whole album tagfile and moves every one -- so entries are matched by the
 * name key instead, and only the figures are taken.
 * ------------------------------------------------------------------------ */

/* Room for the albums that gained tracks, and for the search's unique list.
 * Past the first, so much has changed that summarising the lot is no worse
 * than working out what to skip. */
#define CARRY_DIRTY_MAX 512
#define CARRY_UNIQ_MAX  2048

struct carried
{
    uint32_t key;
    int year;
    int playcount;
    long lastplayed;
    unsigned int art_hash;
    unsigned int artist_art_hash;
};

static struct carried *carried_tab;
static int carried_ct;
/* Album-name hashes with a track newer than the saved index. The name alone,
 * without the artist: a search on tag_album yields names, and matching more
 * albums than strictly necessary only costs the search we would have done. */
static uint32_t *dirty_tab;
static int dirty_ct;
/* Full album keys named in the play log. Their figures must come from
 * tagcache, which already holds the real counts -- carrying the old ones and
 * then replaying the log on top of them would count those plays twice. */
static uint32_t *played_tab;
static int played_ct;
/* What was taken off the buffer for the two tables, so the caller can have it
 * back once they are finished with. */
static size_t carry_reserved;

static int compare_carried(const void *a, const void *b)
{
    uint32_t ka = ((const struct carried *)a)->key;
    uint32_t kb = ((const struct carried *)b)->key;
    return ka < kb ? -1 : (ka > kb);
}

static int compare_u32(const void *a, const void *b)
{
    uint32_t ka = *(const uint32_t *)a, kb = *(const uint32_t *)b;
    return ka < kb ? -1 : (ka > kb);
}

static const struct carried *carried_find(uint32_t key)
{
    int lo = 0, hi = carried_ct - 1;

    if (key == 0)   /* nameless: matches nothing, by assign_keys()'s rule */
        return NULL;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;

        if (carried_tab[mid].key == key)
            return &carried_tab[mid];
        if (carried_tab[mid].key < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static bool in_u32_table(const uint32_t *tab, int ct, uint32_t v)
{
    int lo = 0, hi = ct - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;

        if (tab[mid] == v)
            return true;
        if (tab[mid] < v)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

/* Albums that cannot take the previous index's figures: one that gained
 * tracks (matched on the name, which is all the commitid search reports) or
 * one that was played (matched on the full key, which the log carries). */
static bool album_is_dirty(uint32_t namehash, uint32_t key)
{
    return in_u32_table(dirty_tab, dirty_ct, namehash)
        || in_u32_table(played_tab, played_ct, key);
}

/* The albums named in the log since the saved index was written. */
static void load_played(int32_t since)
{
    struct play_rec batch[32];
    int fd = open_play_log();
    ssize_t got;

    if (fd < 0)
        return;

    while ((got = read(fd, batch, sizeof(batch))) >= (ssize_t)sizeof(batch[0]))
    {
        int n = got / sizeof(batch[0]);
        int i;

        for (i = 0; i < n; i++)
        {
            if (batch[i].serial < since || played_ct >= PLAY_LOG_MAX)
                continue;
            played_tab[played_ct++] = batch[i].album_key;
        }
    }

    close(fd);

    if (played_ct > 0)
        qsort(played_tab, played_ct, sizeof(*played_tab), compare_u32);
}

/* Albums holding a track committed after the saved index was written. One
 * pass over the master index, no per-album work. */
static void load_dirty(int32_t since, uint32_t *uniq)
{
    struct tagcache_search dtcs;
    struct tagcache_search_clause clause;
    char buf[TAGCACHE_BUFSZ];

    memset(&clause, 0, sizeof(clause));
    clause.tag = tag_commitid;
    clause.type = clause_gt;
    clause.numeric = true;
    clause.numeric_data = since;

    if (!tagcache_search(&dtcs, tag_album))
        return;

    /* Without this the search reports one result per track rather than per
     * album, and a single album would fill the table on its own. */
    tagcache_search_set_uniqbuf(&dtcs, uniq, CARRY_UNIQ_MAX * sizeof(*uniq));
    tagcache_search_add_clause(&dtcs, &clause);

    while (tagcache_get_next(&dtcs, buf, sizeof(buf)))
    {
        if (dirty_ct >= CARRY_DIRTY_MAX)
        {
            dirty_ct = -1;   /* too many: summarise everything */
            break;
        }
        dirty_tab[dirty_ct++] = name_key(buf, NULL);
    }

    tagcache_search_finish(&dtcs);

    if (dirty_ct > 0)
        qsort(dirty_tab, dirty_ct, sizeof(*dirty_tab), compare_u32);
}

/* Read the previous index's album figures, and work out which albums cannot
 * use them. Both tables are optional: on any doubt they are left empty and
 * every album is summarised from the database exactly as before.
 *
 * The buffer shrinks rather than this being allocated separately. The arrays
 * below are built from both ends of it, so the only safe place to stand is
 * outside what the build can reach. */
static void carry_over_prepare(void **buf, size_t *bufsz)
{
    struct db_summary_t old;
    struct tagcache_marks marks;
    struct album_data *raw;
    size_t need;
    int i, fd;

    carried_tab = NULL;
    carried_ct = 0;
    dirty_tab = NULL;
    dirty_ct = 0;
    played_tab = NULL;
    played_ct = 0;
    carry_reserved = 0;

    fd = open(DB_SUMMARY_FILE, O_RDONLY);
    if (fd < 0)
        return;

    if (read(fd, &old, sizeof(old)) != sizeof(old)
        || memcmp(&old.header, INDEX_HDR, sizeof(old.header)) != 0
        || !header_fits(&old, *bufsz)
        || old.album_ct == 0)
    {
        close(fd);
        return;
    }

    /* A deletion moves figures this cannot account for -- a removed track's
     * plays should stop counting, and nothing here says which album lost one.
     * Deletions are rare, so give the whole carry-over up rather than carry
     * something wrong. */
    tagcache_get_marks(&marks);
    if (marks.deleted_ct != old.deleted)
    {
        close(fd);
        return;
    }

    need = (size_t)old.album_ct * sizeof(struct album_data)
         + (CARRY_DIRTY_MAX + CARRY_UNIQ_MAX + PLAY_LOG_MAX) * sizeof(uint32_t);

    /* Never at the build's expense: it has to fit what it is building. */
    if (need > *bufsz / 3)
    {
        close(fd);
        return;
    }

    *bufsz -= need;
    carry_reserved = need;
    raw = (struct album_data *)((char *)*buf + *bufsz);
    dirty_tab = (uint32_t *)(raw + old.album_ct);

    /* Past the header and both name blobs; the album array follows them. */
    if (lseek(fd, sizeof(old) + old.artist_len + old.album_len, SEEK_SET) < 0
        || read(fd, raw, old.album_ct * sizeof(struct album_data))
           != (ssize_t)(old.album_ct * sizeof(struct album_data)))
    {
        close(fd);
        *bufsz += need;
        carry_reserved = 0;
        dirty_tab = NULL;
        return;
    }
    close(fd);

    /* Compacted in place, forward: struct carried is the smaller of the two,
     * so entry i is always written below where entry i was read from. */
    carried_tab = (struct carried *)raw;
    for (i = 0; i < old.album_ct; i++)
    {
        struct album_data a = raw[i];

        carried_tab[i].key = a.key;
        carried_tab[i].year = a.year;
        carried_tab[i].playcount = a.playcount;
        carried_tab[i].lastplayed = a.lastplayed;
        carried_tab[i].art_hash = a.art_hash;
        carried_tab[i].artist_art_hash = a.artist_art_hash;
    }
    carried_ct = old.album_ct;
    qsort(carried_tab, carried_ct, sizeof(*carried_tab), compare_carried);

    played_tab = dirty_tab + CARRY_DIRTY_MAX + CARRY_UNIQ_MAX;

    load_dirty(old.commitid, dirty_tab + CARRY_DIRTY_MAX);
    load_played(old.serial);

    if (dirty_ct < 0)       /* too much changed to be worth matching */
        carried_ct = 0;
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
    int carried_over = 0;
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

        /* Already answered, by a previous index, for an album whose tracks
         * have not changed since. This is the whole point of the step: the
         * search below is the expensive part of the build. */
        struct album_data *ent = &pfi->album_index[album_idx];
        const struct carried *c;

        if (!album_is_dirty(name_key(pfi->album_names + ent->name_idx, NULL),
                            ent->key)
            && (c = carried_find(ent->key)) != NULL)
        {
            ent->year = c->year;
            ent->playcount = c->playcount;
            ent->lastplayed = c->lastplayed;
            ent->art_hash = c->art_hash;
            ent->artist_art_hash = c->artist_art_hash;
            carried_over++;
            continue;
        }

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

    /* Cast, because album_ct is int32_t and that is `long` on this toolchain,
     * so %d would not match it. */
    debug_log(DEBUG_LOG_TAGCACHE, "index: %d albums, %d carried, %d searched",
              (int)pfi->album_ct, carried_over,
              (int)pfi->album_ct - carried_over);
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
    struct tagcache_marks marks;

    int i, j, last, final, retry, res;

    ALIGN_BUFFER(buf, buf_size, sizeof(long));

    /* Before anything claims the buffer: this takes its share off the top,
     * where neither of the arrays built from the ends can reach it. */
    carry_over_prepare(&buf, &buf_size);

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

    if (!search_excluding_spoken(&tcs, tag_album))
        return ERROR_USER_ABORT;

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

    /* Before the stats, not after: they are what the carry-over matches on. */
    assign_keys();

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

    /* The duplicate pass above rewrites artist_idx, so the keys assigned
     * before the stats no longer describe every entry. Cheap to redo, and
     * these are the ones that get saved. */
    assign_keys();

    pfi->buf = buf;
    /* The carry-over tables are finished with, so hand their space back
     * rather than leaving the caller short of it for this build. */
    pfi->buf_sz = buf_size + carry_reserved;
    carry_reserved = 0;
    /* artist_index must survive this. The figures assign_artist_stats() has
     * just filled in are saved with the rest of the index and read back by the
     * artist charts, and the array itself is intact -- build_artist_index()
     * finalises it into the buffer ahead of the album data, which nothing
     * above overwrites. Clearing the pointer here would lose all of it and
     * look free, because the memory is still there. */

    /* Stamped after the figures have been read, not before. A play landing
     * mid-build is then simply not replayed later; stamping first would
     * replay one that assign_album_stats() had already counted. Both correct
     * themselves at the next full build, which re-reads tagcache -- this is
     * the quieter of the two while they stand. */
    tagcache_get_marks(&marks);
    pfi->commitid = marks.commitid;
    pfi->serial = marks.serial;
    pfi->deleted = marks.deleted_ct;

    /* Everything in the log is now either counted in an album summarised from
     * tagcache, or predates the watermark just stamped. Keeping it would
     * replay those plays on top of figures that already hold them. */
    remove(DB_PLAYS_FILE);

    /* Artist then album name, and that is all a reader is promised. A screen
     * that wants its albums arranged any particular way sorts them itself once
     * it has them -- which is where the setting deciding the arrangement lives
     * anyway, and why no order here can be the right one for everybody. */
    qsort(pfi->album_index, pfi->album_ct,
          sizeof(struct album_data), compare_index_order);

    return (pfi->album_ct > 0) ? 0 : ERROR_NO_ALBUMS;
}

/* Saves the album index into a binary file, so the next screen that wants it
   reads it back instead of walking the database again. */

/* write() that insists on the whole block. A half-written index is not a
 * usable one, so a short write has to fail the save rather than be left to be
 * discovered by whoever reads it back. */
static bool write_block(int fd, const void *buf, size_t len)
{
    return len == 0 || write(fd, buf, len) == (ssize_t)len;
}

static int save_album_index(void){
    /* Written beside the real file and renamed over it, never into it.
     *
     * The index is rewritten whole, so writing in place destroys the previous
     * one the instant it starts -- and anything that cuts the write short, a
     * USB session or a flat battery, leaves a truncated file that the next
     * load rejects, sending the carousel off to rebuild from the database.
     * rename() creates the new directory entry before dropping the old one, so
     * a reader arriving at any moment sees one complete index or the other. */
    int fd = creat(DB_SUMMARY_TMP, 0666);
    struct db_summary_t data;
    bool ok;

    if (fd < 0)
        return -1;

    memcpy(&data, pfi, sizeof(struct db_summary_t));
    memcpy(&data.header, INDEX_HDR, sizeof(pfi->header));

    /* The struct is written whole, pointers and all, and a pointer means
     * nothing to whoever reads it back -- it is this boot's address for memory
     * that will be somewhere else, or gone, next time. Zero them on the way
     * out so the file cannot carry a plausible-looking address, and so a
     * loader that forgets to rebind one gets a NULL fault here rather than a
     * silent read of a stale location. The lengths and counts beside them are
     * the real content. */
    data.artist_names = NULL;
    data.artist_index = NULL;
    data.album_names  = NULL;
    data.album_index  = NULL;
    data.buf          = NULL;
    data.buf_sz       = 0;

    /* Header from the nulled copy, bodies from pfi -- `data`'s pointers have
     * just been cleared, so they are no longer where the arrays live.
     *
     * The artist array is written last so the layout before it is unchanged
     * from the versions that did not keep it; the magic tells the two apart
     * anyway. */
    ok = write_block(fd, &data, sizeof(struct db_summary_t))
      && write_block(fd, pfi->artist_names, data.artist_len)
      && write_block(fd, pfi->album_names, data.album_len)
      && write_block(fd, pfi->album_index,
                     (size_t)data.album_ct * sizeof(struct album_data))
      && write_block(fd, pfi->artist_index,
                     (size_t)data.artist_ct * sizeof(struct artist_data));

    close(fd);

    if (!ok || rename(DB_SUMMARY_TMP, DB_SUMMARY_FILE) < 0)
    {
        remove(DB_SUMMARY_TMP);
        return -1;
    }

    return 0;
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
 * The artist carousel wants the artist list and nothing else, and does not
 * have to rebuild it: the same list, with its playback figures already
 * summarised, is sitting in the index file. Reading it is a file read against
 * a walk of every album-artist tag.
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
static int load_artist_index(struct db_summary_t *target,
                             void **buf, size_t *bufsz)
{
    struct db_summary_t data;
    void *b = *buf;
    size_t bsz = *bufsz;
    unsigned int artist_idx_sz;
    int i, fr = open(DB_SUMMARY_FILE, O_RDONLY);

    if (fr < 0)
        return ERROR_NO_ARTISTS;

    if ((unsigned long)filesize(fr) <= sizeof(data)
        || read(fr, &data, sizeof(data)) != sizeof(data)
        || memcmp(&(data.header), INDEX_HDR, sizeof(data.header)) != 0
        || !header_fits(&data, bsz)
        || data.artist_ct == 0)
        goto failure;

    /* Each length checked against the space left, never their sum against the
     * total: artist_len comes straight out of the file, so on a corrupt header
     * the addition wraps and a bounds check written that way passes. It is
     * caught further down only because read2buf() then comes up short, which
     * is the read defending the check rather than the other way round. */
    artist_idx_sz = data.artist_ct * sizeof(struct artist_data);
    if (data.artist_len > bsz || artist_idx_sz > bsz - data.artist_len)
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
    target->commitid = data.commitid;

    /* No album list on this path, and said three ways rather than one. A zero
     * count alone is not enough of a guard: a walk that forgets to check it
     * would read through whatever pointers the struct happens to be carrying,
     * from the previous load or from the file. NULL faults instead. */
    target->album_ct = 0;
    target->album_len = 0;
    target->album_names = NULL;
    target->album_index = NULL;
    target->serial = data.serial;
    /* Artists only on this path, but the log carries their keys too. */
    replay_plays(target);
    *buf = b;
    *bufsz = bsz;
    return SUCCESS;

failure:
    close(fr);
    return ERROR_NO_ARTISTS;
}

int db_summary_load_artists(struct db_summary_t *target,
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

    int i, fr = open(DB_SUMMARY_FILE, O_RDONLY);
    struct db_summary_t data;

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
                memcmp(&(data.header), INDEX_HDR, sizeof(data.header)) == 0 &&
                header_fits(&data, bufstart_sz))
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

                memcpy(pfi, &data, sizeof(struct db_summary_t));
                pfi->buf = buf;
                pfi->buf_sz = buf_size;

                /* The figures in the file are correct as of its watermark;
                 * anything played since is sitting in the log. */
                replay_plays(pfi);

                return 0;
            }
        }
    }

failure:
    /* Silent: there being no readable index is a normal state -- first run,
     * a bumped INDEX_HDR, a deleted file -- and the caller just builds one.
     * Nothing here is the user's to act on. */
    if (fr >= 0)
        close(fr);

    pfi->buf = bufstart;
    pfi->buf_sz = bufstart_sz;
    pfi->artist_ct = 0;
    pfi->album_ct = 0;
    return -1;

}

/* Reuse the saved index, or build one and persist it, into the caller's
 * struct and the caller's memory. Which memory is the only thing that varies
 * between a build the carousel asks for and one a background pass does. */
static int build_into(struct db_summary_t *target, void *buf, size_t buf_sz,
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
     * Whether to rebuild is decided here, from the file, and must not be taken
     * from any screen's state: a caller that has not opened that screen this
     * boot would read its defaults and walk the whole database with a perfectly
     * good index sitting on disk unread. The album charts and Random album are
     * both such callers. A stale *format* is caught by INDEX_HDR, which is what
     * that magic is for. */
    if (background || load_album_index() < 0)
    {
        set_working_for_build(true);   /* the "working" LED while (re)building */
        ret = create_album_index();
        set_working_for_build(false);

        /* Writing the index is the whole of what a rebuild does. In
         * particular it does not invalidate any artwork: thumbnails are keyed
         * by the folder they came from, not by list position, so the same
         * album still resolves to the same file however the list is
         * reordered. Nothing in this file may touch the carousel's config --
         * see background_build() for what goes wrong when it does. */
        if (ret == 0)
        {
            /* Only the foreground caller may report this: the background pass
             * owns no screen, and there is nothing the user could do anyway --
             * the carousel will simply build it again when asked. */
            if (save_album_index() < 0 && !building_bg)
                splash(HZ * 2, "Could not write index");
        }
    }

    pfi = NULL;
    building_bg = false;
    mutex_unlock(&build_mutex);
    return ret;
}

int db_summary_build_into(struct db_summary_t *target, void *buf, size_t buf_sz)
{
    /* build_into() takes build_mutex, which the background pass holds for its
     * whole run -- so a caller arriving mid-pass blocks on it with a dead
     * screen and no way out. Waiting here instead is the same wait with the
     * pass's progress on it and a way to leave, and doing it inside means no
     * caller can forget. A caller with its own reason to wait earlier still
     * may; arriving here with nothing running costs nothing. */
    int ret = wait_for_background();

    if (ret != SUCCESS)
        return ret;

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
    /* Announce before asking, so a pass deciding whether to start sees this
     * and stands down instead of taking the lock out from under us. */
    fg_wanted_tick = current_tick;

    if (!db_summary_is_busy())
        return SUCCESS;

    /* Nothing on screen at all if it finishes promptly. */
    splash_progress_set_delay(HZ / 2);

    while (db_summary_is_busy())
    {
        /* Kept current for as long as we are here, so the claim never lapses
         * while we are still waiting on a pass that is already running. */
        fg_wanted_tick = current_tick;

        splash_progress(bg_done, bg_total > 0 ? bg_total : 1,
                        "%s", str(LANG_WAIT));

        int action = get_action(CONTEXT_STD, HZ / 10);

        /* USB counts as leaving, and has to be handled rather than dropped --
         * this poll owns the button queue for as long as the wait lasts, and
         * the acknowledgement the storage handover is waiting for is the one
         * default_event_handler() sends. */
        if (default_event_handler(action) == SYS_USB_CONNECTED ||
            action == ACTION_STD_CANCEL)
        {
            /* Leaving, not switching to the slow path: tell it to stop so it
             * is not still running behind whatever comes next. */
            idx_abort = true;
            return ERROR_USER_ABORT;
        }
    }

    return SUCCESS;
}

/* The artist half only, with no album list, into the caller's index and the
 * caller's memory.
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

int db_summary_build_artists(struct db_summary_t *target,
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

/* Whether the background pass should give up where it stands.
 *
 * The event is peeked, never taken. This queue is in the broadcast list, so
 * the storage handover counts an acknowledgement from it and hands the host
 * the disk only once every count is in -- and bg_task_tick() is what sends
 * this one. A pass that swallowed the event here would leave the handover
 * waiting for an acknowledgement nobody can still send, and the player would
 * report an empty drive for as long as it stayed plugged in. Returning true is
 * the whole job: the pass unwinds to the tick, which reads the event properly.
 *
 * Without this the pass has no way to hear about USB at all, and a full
 * library index takes long enough for the host to give up and reset the port
 * while it runs. */
static bool bg_should_stop(void)
{
    struct queue_event ev;

    if (bg_task_preempted(&db_summary_task))
        return true;

    /* Trap: the queue alone is too late for a host. Nothing arrives on it
     * until SET_CONFIGURATION, by which point a pass holding the CPU has
     * already cost the host SET_ADDRESS. */
    if (usb_host_is_present())
        return true;

    if (!queue_peek(&idx_queue, &ev))
        return false;

    switch (ev.id)
    {
        case SYS_USB_CONNECTED:
        case SYS_POWEROFF:
        case SYS_REBOOT:
            return true;
    }

    return false;
}


/* True from before the background pass reaches for the lock until after it has
 * let go, not merely while it holds it -- see bg_claimed. */
bool db_summary_is_busy(void)
{
    return bg_claimed || building_bg;
}

const char *db_summary_activity(void)
{
    return bg_step ? bg_step : "";
}

void db_summary_progress(int *done, int *total)
{
    *done = bg_done;
    *total = bg_total;
}

/* The database entry count the last background build covered. Its own marker,
 * not a screen's cache-version flag: those say whether a screen's on-disk
 * formats are current, are written by that screen and by nothing on this path,
 * and would therefore read as "rebuild" forever. This is the same marker the
 * artwork cache keeps for the same reason -- what was the library like when we
 * last finished. */
#define DB_SUMMARY_DONE ROCKBOX_DIR "/db_summary.done"

/* Read the index without a buffer of your own; see db_summary.h.
 *
 * Here rather than in the caller because IDX_BUILD_BUFSZ is here: how much a
 * build needs is the builder's business, and a second copy of that number
 * elsewhere would be one to keep in step. Same allocation shape as
 * background_build() below, for the same reasons -- ask outright rather than
 * checking core_allocatable() first, since core_alloc() shrinks what will
 * shrink and that is usually the only way this gets memory, and pin it because
 * a handle with no ops is movable while the builder yields throughout. */
int db_summary_acquire(struct db_summary_t *target, int *handle)
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

    ret = db_summary_build_into(target, buf, IDX_BUILD_BUFSZ);
    if (ret < SUCCESS)
    {
        db_summary_release(*handle);
        *handle = 0;
    }
    return ret;
}

void db_summary_release(int handle)
{
    if (handle <= 0)
        return;
    core_put_data_pinned(core_get_data(handle));
    core_free(handle);
}

/* ---------------------------------------------------------------------------
 * Reading records without holding the index
 *
 * What save_album_index() writes is a header, the two name blobs and then the
 * two arrays, all contiguous and in that order, and struct album_data is
 * fixed-size. So album n is one seek away, and a caller that wants one record
 * need not have the whole index in memory to reach it -- see the reader notes
 * in db_summary.h for why that matters.
 *
 * Only albums so far. The charts want the same treatment and will want artists
 * with it; nothing else does.
 * ------------------------------------------------------------------------ */

/* Open the file and work out where the album array starts, or leave nothing
 * open. Separate from db_summary_reader_open() because that tries it twice,
 * once either side of building an index. */
static int reader_start(struct db_summary_reader *r)
{
    struct db_summary_t data;
    off_t fsize;
    int fd = open(DB_SUMMARY_FILE, O_RDONLY);

    if (fd < 0)
        return ERROR_NO_ALBUMS;

    fsize = filesize(fd);
    if (fsize <= (off_t)sizeof(data)
        || read(fd, &data, sizeof(data)) != (ssize_t)sizeof(data)
        || memcmp(&(data.header), INDEX_HDR, sizeof(data.header)) != 0
        || !header_fits(&data, (size_t)fsize)
        || data.album_ct == 0)
        goto failure;

    r->album_ct = data.album_ct;
    r->album_off = (long)(sizeof(data) + data.artist_len + data.album_len);

    /* The array has to be inside the file that claims to hold it. The magic is
     * the first four bytes, so a truncated index passes the check above --
     * without this one a seek would land past the end and the read would leave
     * the caller's record as whatever was on its stack. */
    if (r->album_off + (long)r->album_ct * (long)sizeof(struct album_data)
        > (long)fsize)
        goto failure;

    r->fd = fd;
    return SUCCESS;

failure:
    close(fd);
    return ERROR_NO_ALBUMS;
}

int db_summary_reader_open(struct db_summary_reader *r)
{
    struct db_summary_t built;
    int handle;
    int ret;

    r->fd = -1;

    /* The same wait db_summary_acquire() makes, and for the same reason: the
     * lock below is the one a background pass holds for its whole run. */
    ret = wait_for_background();
    if (ret != SUCCESS)
        return ret;

    /* Held until the reader closes. A pass finishing mid-read renames a new
     * index over this file, which would leave the reader seeking to offsets
     * that no longer describe what it is reading. */
    mutex_lock(&build_mutex);
    if (reader_start(r) == SUCCESS)
        return SUCCESS;
    mutex_unlock(&build_mutex);

    /* Nothing readable there. Build one for the file it writes, not for the
     * copy it returns -- which is thrown away here exactly as
     * background_build() throws its own away. This is the one path that still
     * costs the 384K, and it runs on a player that has never built an index. */
    ret = db_summary_acquire(&built, &handle);
    if (ret < SUCCESS)
        return ret;
    db_summary_release(handle);

    mutex_lock(&build_mutex);
    if (reader_start(r) == SUCCESS)
        return SUCCESS;
    mutex_unlock(&build_mutex);

    return ERROR_NO_ALBUMS;
}

void db_summary_reader_close(struct db_summary_reader *r)
{
    if (r->fd < 0)
        return;

    close(r->fd);
    r->fd = -1;
    mutex_unlock(&build_mutex);
}

bool db_summary_read_album(struct db_summary_reader *r, int n,
                           struct album_data *out)
{
    off_t at;

    if (r->fd < 0 || n < 0 || n >= r->album_ct)
        return false;

    at = r->album_off + (long)n * (long)sizeof(*out);
    return lseek(r->fd, at, SEEK_SET) == at
        && read(r->fd, out, sizeof(*out)) == (ssize_t)sizeof(*out);
}

static int compare_year_seek(const void *a_v, const void *b_v)
{
    const struct db_summary_year *a = a_v;
    const struct db_summary_year *b = b_v;

    return (a->seek > b->seek) - (a->seek < b->seek);
}

int db_summary_read_year_table(struct db_summary_year *out, int max)
{
    struct db_summary_t data;
    struct tagcache_marks marks;
    struct album_data rec;
    off_t at;
    int fd;
    int ret;
    int n;

    ret = wait_for_background();
    if (ret != SUCCESS)
        return ret;

    mutex_lock(&build_mutex);

    fd = open(DB_SUMMARY_FILE, O_RDONLY);
    if (fd < 0)
    {
        ret = ERROR_NO_ALBUMS;
        goto done;
    }

    ret = ERROR_NO_ALBUMS;
    if (read(fd, &data, sizeof(data)) != (ssize_t)sizeof(data)
        || memcmp(&(data.header), INDEX_HDR, sizeof(data.header)) != 0
        || !header_fits(&data, (size_t)filesize(fd))
        || data.album_ct == 0 || data.album_ct > max)
        goto done;

    /* The seeks below are only seeks for the commit they were written against;
     * see the note in db_summary.h. */
    tagcache_get_marks(&marks);
    if (marks.commitid != data.commitid)
        goto done;

    at = (off_t)(sizeof(data) + data.artist_len + data.album_len);
    if (lseek(fd, at, SEEK_SET) != at)
        goto done;

    for (n = 0; n < data.album_ct; n++)
    {
        if (read(fd, &rec, sizeof(rec)) != (ssize_t)sizeof(rec))
            goto done;
        out[n].seek = rec.seek;
        out[n].year = rec.year;
    }

    qsort(out, n, sizeof(*out), compare_year_seek);
    ret = n;

done:
    if (fd >= 0)
        close(fd);
    mutex_unlock(&build_mutex);
    return ret;
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
    int fd = open(DB_SUMMARY_FILE, O_RDONLY);
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
 * Nothing in this file touches the carousel's config, and nothing may. It is
 * a file-scope struct the carousel's own init() fills; a pass running on a
 * boot where nobody opened the carousel would be saving zeroes over the
 * resume position and the artwork-cache mark. Its cache_version is about the
 * slide placeholder's format, which this pass does not build, so there was
 * never anything true for this file to say about it either. */
static bool background_build(void)
{
    struct db_summary_t local;
    size_t bufsz = IDX_BUILD_BUFSZ;
    int handle;
    void *buf;
    int ret;
    bool ok;

    /* Claim the builder before doing anything that leads to the lock, then
     * give way if a foreground caller has already announced itself. Returning
     * false is not a failure: bg_task simply tries again on a later tick, by
     * which time whoever wanted it has finished and written the index this
     * pass would have built anyway. */
    bg_claimed = true;
    if (foreground_wants_builder())
    {
        bg_claimed = false;
        return false;
    }

    /* Ask outright rather than checking core_allocatable() first: that reports
     * space already free, and on a player with the audio buffer claimed there
     * is rarely any. core_alloc() shrinks what will shrink to make room, which
     * is the only way this ever gets memory. Failure just means try later --
     * and by then the artwork pass, which is what usually has the memory, will
     * have seen this task waiting and let go of it. */
    handle = core_alloc(bufsz);
    if (handle <= 0)
    {
        bg_claimed = false;
        return false;
    }

    /* Pinned: a handle with no ops is movable, and the builder yields all the
     * way through -- to the database, and to be nice to playback. */
    buf = core_get_data_pinned(handle);

    memset(&local, 0, sizeof(local));
    bg_done = bg_total = 0;

    ret = build_into(&local, buf, bufsz, true);

    bg_done = bg_total = 0;

    core_put_data_pinned(buf);
    core_free(handle);

    /* Released only now, after the lock has been let go inside build_into():
     * anyone polling db_summary_is_busy() is waiting to take that lock, so it
     * has to stay set until there is nothing left for them to collide with. */
    ok = (ret == SUCCESS);
    bg_claimed = false;

    /* Only on a clean finish: an aborted or failed pass must be retried, not
     * recorded as covering this library. Saying so is enough -- bg_task writes
     * the marker itself once we return true. */
    return ok;
}

struct bg_task db_summary_task =
{
    .done_file   = DB_SUMMARY_DONE,
    .rank        = BG_RANK_INDEX,
    .work_bytes  = IDX_BUILD_BUFSZ,
    .run         = background_build,
    .artifact_ok = saved_index_present,
};

static void idx_thread(void)
{
    while (1)
        bg_task_tick(&db_summary_task, &idx_queue);
}

void db_summary_init(void)
{
    mutex_init(&build_mutex);
    queue_init(&idx_queue, true);
    bg_task_init(&db_summary_task);
    idx_thread_id = create_thread(idx_thread, idx_stack, sizeof(idx_stack), 0,
                                  idx_thread_name IF_PRIO(, PRIORITY_BACKGROUND)
                                  IF_COP(, CPU));
    (void)idx_thread_id;
}

void db_summary_invalidate(void)
{
    /* Forget what the last pass covered, so the next tick rebuilds. Used by
     * the carousel's rebuild/update, which otherwise only reach its own build
     * and would leave the background pass thinking it was up to date.
     *
     * An update rather than a rebuild: there is nothing here to purge that the
     * pass does not overwrite anyway, the index being a single file. */
    bg_task_update(&db_summary_task);
}

/* ---- playing an album -------------------------------------------------- */

/* One track, while the album is being put in order. Only the index id is kept,
 * not the path: a path is MAX_PATH and there is no reason to hold every one of
 * them when tagcache_retrieve() can fetch each again in the order wanted. */
struct album_track {
    int32_t idx_id;
    int32_t key;
};

static int compare_album_tracks(const void *a_v, const void *b_v)
{
    const struct album_track *a = a_v;
    const struct album_track *b = b_v;
    return a->key - b->key;
}

int db_summary_play_album(const struct album_data *album)
{
    struct tagcache_search tcs;
    struct playlist_insert_context context;
    char buf[MAX_PATH];
    struct album_track *list;
    size_t list_sz = 0;
    int cap, found = 0, added = 0;
    bool sorted;

    if (!warn_on_pl_erase())
        return -1;
    if (playlist_create(NULL, NULL) < 0)
        return -1;

    cpu_boost(true);
    if (!tagcache_search(&tcs, tag_filename))
    {
        cpu_boost(false);
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        return -1;
    }
    if (playlist_insert_context_create(NULL, &context, PLAYLIST_INSERT_LAST,
                                       false, false) < 0)
    {
        /* create() keeps the playlist lock even when it fails; release()
         * is the only thing that gives it back. */
        playlist_insert_context_release(&context);
        tagcache_search_finish(&tcs);
        cpu_boost(false);
        return -1;
    }

    /* The same filter pair assign_album_stats() enumerates an album with. */
    tagcache_search_add_filter(&tcs, tag_album, album->seek);
    if (album->artist_idx >= 0)
        tagcache_search_add_filter(&tcs, tag_albumartist, album->artist_seek);

    /* A search returns entries in master-index order -- the order the files
     * were scanned, which is close to track order for most rips and wrong for
     * the rest. The browser's track list is sorted by tagnavi's
     * "%02d%04d%s" discnum/tracknum format, and playing an album has to come
     * out in the order that list shows, so collect the album first and sort it
     * before any of it reaches the playlist. */
    list = app_get_buffer(&list_sz, "album play");
    cap = list ? (int)(list_sz / sizeof(*list)) : 0;
    sorted = (cap > 0);

    while (tagcache_get_next(&tcs, buf, sizeof(buf)))
    {
        int disc, track;

        if (found >= cap)
        {
            /* Nowhere to put the rest of the album, so it cannot be ordered
             * as a whole. Play it in search order rather than not at all. */
            sorted = false;
            break;
        }
        disc  = tagcache_get_numeric(&tcs, tag_discnumber);
        track = tagcache_get_numeric(&tcs, tag_tracknumber);
        if (disc < 0)
            disc = 0;
        if (track < 0)
            track = 0;
        list[found].idx_id = tcs.idx_id;
        /* Disc above track, so disc 2's track 1 follows disc 1's last. An
         * untagged disc or track sorts as 0, which puts it first -- the same
         * place the browser's format string puts it. Both are masked: these
         * come from file tags, so a nonsense value must not shift into the
         * sign bit and sort the track to the front. */
        list[found].key = ((disc & 0x7fff) << 16) | (track & 0xffff);
        found++;
    }

    if (sorted)
    {
        qsort(list, found, sizeof(*list), compare_album_tracks);
        for (int i = 0; i < found; i++)
        {
            if (!tagcache_retrieve(&tcs, list[i].idx_id, tag_filename,
                                   buf, sizeof(buf)))
                continue;
            if (playlist_insert_context_add(&context, buf) < 0)
                break;
            added++;
        }
    }
    else
    {
        /* Restart: the walk above consumed part or all of the search. */
        tagcache_search_finish(&tcs);
        if (tagcache_search(&tcs, tag_filename))
        {
            tagcache_search_add_filter(&tcs, tag_album, album->seek);
            if (album->artist_idx >= 0)
                tagcache_search_add_filter(&tcs, tag_albumartist,
                                           album->artist_seek);
            while (tagcache_get_next(&tcs, buf, sizeof(buf)))
            {
                if (playlist_insert_context_add(&context, buf) < 0)
                    break;
                added++;
            }
        }
    }

    playlist_insert_context_release(&context);
    tagcache_search_finish(&tcs);
    cpu_boost(false);

    if (added <= 0)
        return -1;

    playlist_start(0, 0, 0);
    return added;
}

/* The album the carousel asked for, held across its teardown. */
static struct album_data pending_play;
static bool pending_play_armed;

void db_summary_play_album_on_exit(const struct album_data *album)
{
    pending_play = *album;
    pending_play_armed = true;
}

int db_summary_play_pending(void)
{
    if (!pending_play_armed)
        return -1;
    pending_play_armed = false;
    return db_summary_play_album(&pending_play);
}
