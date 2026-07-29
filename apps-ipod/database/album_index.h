/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to album_index.c: building the carousel's album and artist list.
 ****************************************************************************/
#ifndef _ALBUM_INDEX_H
#define _ALBUM_INDEX_H

#include <stddef.h>
#include "database/tagcache.h"

/* Fill pf_idx from the saved index, rebuilding it from tagcache first if there
 * is none or it predates the current cache version. SUCCESS or an ERROR_*
 * from carousel.h. Expects pf_idx.buf/buf_sz to point at usable space. */
int album_index_build(void);

/* Build only the artist half into *buf, advancing it. The artist model uses
 * this on its own, without an album list. */
int build_artist_index(struct tagcache_search *tcs, void **buf, size_t *bufsz);

/* qsort comparator over struct album_data, in the carousel's display order.
 * Public because the sort order can be changed from the carousel itself. */
int compare_albums(const void *a_v, const void *b_v);

#endif /* _ALBUM_INDEX_H */
