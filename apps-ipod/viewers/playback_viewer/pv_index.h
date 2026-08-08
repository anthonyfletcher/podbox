/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to pv_index.c.
 ****************************************************************************/
#ifndef _PV_INDEX_H
#define _PV_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include "pv_log.h"

/* The saved aggregates, so opening the deck does not mean reading the whole
 * history again.
 *
 * This owns the FILE -- its header, its watermark, and the order its blocks
 * come in. It does not know what a block means; pv_stats writes the tables
 * and reads them back, and is the only thing that has to agree with itself
 * about their shape.
 *
 * It is a cache and never the record. The log remains the truth, a rebuild
 * always reproduces this, and anything that does not verify is discarded
 * rather than repaired.
 *
 * The watermark is a byte count plus the bytes immediately before it. The
 * count alone cannot tell a log that grew from a different log of a similar
 * size -- a reinstall, a restored backup, a fixture swapped in -- and reading
 * the whole history back to check would defeat the purpose. Sixty-four bytes
 * at a known position costs one seek and catches every case that matters. */

/* What a caller must match to reuse a saved index. Anything that changes the
 * meaning of a record belongs in here, because a stale file that still parses
 * is far worse than one that does not. */
struct pv_index_id
{
    unsigned long source;      /* which log it describes */
    unsigned long agg_size;    /* sizeof(struct pv_agg) */
    unsigned long day_size;    /* sizeof(struct pv_day)  */
    unsigned long totals_size; /* sizeof(struct pv_totals) */
    unsigned long state_size;  /* sizeof(struct pv_badge_state) */
};

/* Open the saved index and check it describes this log at this moment.
 *
 * On success '*covered' is how many bytes of the log it accounts for -- the
 * caller reads from there to the end and applies the difference. Equal to the
 * log's size means there is nothing new and the deck can open on what was
 * loaded alone.
 *
 * Must be matched by pv_index_read_end() when it returns true. */
bool pv_index_read_begin(const struct pv_index_id *id, unsigned long log_size,
                         unsigned long *covered);

/* Blocks come back in the order they were written. False on a short read,
 * which invalidates everything read so far -- the caller must fall back to a
 * full rebuild rather than draw from half a table. */
bool pv_index_read(void *dst, size_t bytes);
void pv_index_read_end(void);

/* Write a new index. Blocks are written in the order given, and the file is
 * only put in place once all of them succeeded, so an interrupted write
 * leaves the previous index rather than a half-written one. */
bool pv_index_write_begin(const struct pv_index_id *id,
                          unsigned long covered, enum pv_source src);
bool pv_index_write(const void *src, size_t bytes);
bool pv_index_write_end(void);
void pv_index_write_abort(void);

/* Forget the saved index. For when it cannot be trusted and should not be
 * left lying around to be found again. */
void pv_index_discard(void);

#endif /* _PV_INDEX_H */
