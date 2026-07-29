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

/* Build only the artist half into *buf, advancing it. The artist model uses
 * this on its own, without an album list. */
int build_artist_index(struct tagcache_search *tcs, void **buf, size_t *bufsz);

/* qsort comparator over struct album_data, in the carousel's display order.
 * Public because the sort order can be changed from the carousel itself. */
int compare_albums(const void *a_v, const void *b_v);

/* Start the background pass that keeps the saved index current. Called once
 * at startup; it builds only while the database is idle and settled. */
void album_index_init(void);

/* True while that pass is building -- for anything that should stay off the
 * disk while it runs. */
bool album_index_is_busy(void);

/* Discard the background pass's record of what it last built, so it runs
 * again on its next tick. */
void album_index_invalidate(void);

#endif /* _ALBUM_INDEX_H */
