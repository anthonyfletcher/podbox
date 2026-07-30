/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to album_index.c: building the carousel's album and artist list.
 ****************************************************************************/
#ifndef _ALBUM_INDEX_H
#define _ALBUM_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include "database/tagcache.h"
#include "system/bg_task.h"

struct pf_index_t;

/* Fill the caller's index from the saved file, rebuilding it from tagcache
 * first if there is none or it predates the current cache version. SUCCESS or
 * an ERROR_* from carousel.h. Only one build may run at a time; a caller
 * arriving while another is running asks it to stop, then waits.
 *
 * The buffer is the caller's: the carousel passes the app buffer it has
 * claimed, the background pass passes memory of its own. Nothing else differs
 * between the two, which is the point. */
int album_index_build_into(struct pf_index_t *target, void *buf, size_t buf_sz);

/* The above against the carousel's own pf_idx; carousel_model.build_index. */
int album_index_build(void);

/* Build only the artist half into 'target', from *buf, advancing it. The
 * artist carousel uses this on its own, without an album list. Serialises
 * against the background pass exactly as album_index_build() does -- the index
 * being built is a single shared slot, so there is no unlocked way in. */
int album_index_build_artists(struct pf_index_t *target,
                              struct tagcache_search *tcs,
                              void **buf, size_t *bufsz);

/* qsort comparator over struct album_data, in the carousel's display order.
 * Public because the sort order can be changed from the carousel itself. */
int compare_albums(const void *a_v, const void *b_v);

/* Start the background pass that keeps the saved index current. Called once
 * at startup; it builds only while the database is idle and settled. */
void album_index_init(void);

/* That pass, for the standard bg_task_rebuild()/bg_task_update() triggers.
 * It outranks the artwork cache -- see the ranking note in album_index.c. */
extern struct bg_task album_index_task;

/* True while that pass is building -- for anything that should stay off the
 * disk while it runs. */
bool album_index_is_busy(void);

/* The build step the background pass last reported ("2/5 Find Albums" and so
 * on), or "" if it has not run. Held over after a pass, so it also says what
 * the last one finished on. */
const char *album_index_activity(void);

/* That pass's last reported position within the step. Both zero when idle. */
void album_index_progress(int *done, int *total);

/* Discard the background pass's record of what it last built, so it runs
 * again on its next tick. */
void album_index_invalidate(void);

#endif /* _ALBUM_INDEX_H */
