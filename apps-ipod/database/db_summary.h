/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to db_summary.c: the album and artist summary derived from the
 * tag database.
 ****************************************************************************/
#ifndef _DB_SUMMARY_H
#define _DB_SUMMARY_H

#include <stdbool.h>
#include <stddef.h>
#include "database/tagcache.h"
#include "system/bg_task.h"

struct db_summary_t;
struct album_data;

/* Fill the caller's index from the saved file, rebuilding it from tagcache
 * first if there is none or it predates the current cache version. SUCCESS or
 * an ERROR_* from carousel.h.
 *
 * Only one build may run at a time. A caller arriving while the background
 * pass is running waits for it and then reads what it wrote, rather than
 * stopping it and repeating the work -- waiting is the quicker of the two,
 * since this path redraws for every album and that one does not. The pass is
 * asked to stop only if the user abandons the wait.
 *
 * The buffer is the caller's: the carousel passes the app buffer it has
 * claimed, the background pass passes memory of its own. Nothing else differs
 * between the two, which is the point. */
int db_summary_build_into(struct db_summary_t *target, void *buf, size_t buf_sz);

/* The above against the carousel's own pf_idx; carousel_model.build_index. */
int db_summary_build(void);

/* Claim memory and fill 'target' from the saved index, building it first if
 * there is none. For a caller that just wants to read the album list and has
 * no buffer of its own -- unlike the carousel, which has already claimed the
 * app buffer for its slides. SUCCESS, or an ERROR_* from carousel.h.
 *
 * On success '*handle' is the claim, and db_summary_release() must be given
 * it once the caller has finished reading: 'target' points into that memory,
 * so nothing it holds outlives the release. */
int db_summary_acquire(struct db_summary_t *target, int *handle);
void db_summary_release(int handle);

/* Read single records out of the saved index without holding it.
 *
 * db_summary_acquire() above needs 384K of core memory, and the audio buffer
 * is what gives it up -- which stops playback and rebuffers the current track
 * (see shrink_callback() in playback.c). That is a fair price for the whole
 * list. It is a poor one for a caller that wants one fixed-size record, so
 * those seek to it in the file instead.
 *
 * A reader holds the build lock for its whole lifetime, so the background pass
 * cannot swap the file out underneath it. Open one, take what you need, close
 * it; do not put a screen up while one is open.
 *
 * The struct is here rather than opaque so the caller can keep one on its own
 * stack. Its fields are private. */
struct db_summary_reader {
    int fd;
    int album_ct;      /* albums the file holds */
    long album_off;    /* where the first of them starts */
};

/* Open the saved index, building one first if there is none to read. SUCCESS,
 * or an ERROR_* from carousel.h. On success, and only then, the caller must
 * match this with db_summary_reader_close(). */
int db_summary_reader_open(struct db_summary_reader *r);
void db_summary_reader_close(struct db_summary_reader *r);

/* Album n of r->album_ct into *out. False if it could not be read. */
bool db_summary_read_album(struct db_summary_reader *r, int n,
                           struct album_data *out);

/* One album's release year, against the taglist position the database browser
 * knows it by. */
struct db_summary_year {
    long seek;
    int  year;
};

/* Fill 'out' with every album's pair, sorted by seek so the caller can binary
 * search it. Returns how many were written, or a negative ERROR_*.
 *
 * For sorting album lists by year. The browser cannot read the year from the
 * database itself -- a unique tag's rows carry no index entry for a numeric
 * tag to be read from -- and the year here is the better one anyway: the
 * maximum across the album's tracks, which is what Album covers sorts on.
 *
 * Refuses when the saved index predates the current database commit. A commit
 * that adds a track re-sorts the album tagfile and moves every seek in it, so a
 * stale table would join cleanly against the wrong albums and produce a
 * plausible, wrong order. Name order is the right answer in that case. */
int db_summary_read_year_table(struct db_summary_year *out, int max);

/* Build only the artist half into 'target', from *buf, advancing it. The
 * artist carousel uses this on its own, without an album list. Serialises
 * against the background pass exactly as db_summary_build() does -- the index
 * being built is a single shared slot, so there is no unlocked way in. */
/* Read just the artist half of the saved index into *buf, advancing it. The
 * quick way to get an artist list: no database walk, and the playback figures
 * come with it already summarised. The album halves of the file are skipped,
 * so they cost no memory.
 *
 * SUCCESS, or an ERROR_* when there is no usable saved index -- in which case
 * the caller can fall back to building one with db_summary_build_artists()
 * below. Serialises against the background pass, which rewrites the file. */
int db_summary_load_artists(struct db_summary_t *target,
                          void **buf, size_t *bufsz);

/* Build the artist half from the database instead. Slower, and needed only
 * when there is no saved index to read.
 *
 * 'with_stats' also fills each artist's playcount/lastplayed, which this path
 * cannot get for free -- it has no album list to roll up from, so it costs a
 * filtered database search per artist. Ask for it only if something is going
 * to sort or rank on it. */
int db_summary_build_artists(struct db_summary_t *target,
                              struct tagcache_search *tcs,
                              void **buf, size_t *bufsz, bool with_stats);

/* Record that a track finished, so the album and artist figures move without
 * the index being rebuilt for it. 'serial' is the value tagcache_increase_
 * serial() just returned, which is also what it stores as lastplayed.
 *
 * Both names come from the track's own tags, so this costs no database work:
 * it appends twelve bytes and returns. A reader applies whatever arrived
 * after the index it just loaded was written. */
void db_summary_log_play(const char *album, const char *albumartist,
                       long serial);

/* qsort comparator over struct album_data, in the carousel's display order.
 * Public because the sort order can be changed from the carousel itself. */
int compare_albums(const void *a_v, const void *b_v);

/* Start the background pass that keeps the saved index current. Called once
 * at startup; it builds only while the database is idle and settled. */
void db_summary_init(void);

/* That pass, for the standard bg_task_rebuild()/bg_task_update() triggers.
 * It outranks the artwork cache -- see the ranking note in album_index.c. */
extern struct bg_task db_summary_task;

/* True while that pass is building -- for anything that should stay off the
 * disk while it runs. */
bool db_summary_is_busy(void);

/* The build step the background pass last reported ("2/5 Find Albums" and so
 * on), or "" if it has not run. Held over after a pass, so it also says what
 * the last one finished on. */
const char *db_summary_activity(void);

/* That pass's last reported position within the step. Both zero when idle. */
void db_summary_progress(int *done, int *total);

/* Discard the background pass's record of what it last built, so it runs
 * again on its next tick. */
void db_summary_invalidate(void);

/* Build the current playlist from one album's tracks and start it, in the same
 * disc/track order the browser's track list shows them in. Returns the number
 * of tracks queued, or a negative value if nothing could be played.
 *
 * Scratch memory comes from the app buffer, which panics if a screen still
 * holds a claim on it -- so this cannot be called from inside the carousel.
 * db_summary_play_album_on_exit() exists for that: it records the album, and
 * album_covers() calls db_summary_play_pending() once carousel_run() has
 * returned and released the claim. */
int  db_summary_play_album(const struct album_data *album);
void db_summary_play_album_on_exit(const struct album_data *album);
int  db_summary_play_pending(void);

#endif /* _DB_SUMMARY_H */
