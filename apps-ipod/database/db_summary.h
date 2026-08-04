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

#endif /* _DB_SUMMARY_H */
