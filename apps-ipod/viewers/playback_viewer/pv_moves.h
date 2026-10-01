/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to pv_moves.c.
 ****************************************************************************/
#ifndef _PV_MOVES_H
#define _PV_MOVES_H

#include <stdbool.h>
#include <stddef.h>

/* Whether the saved table was built against some other state of the
 * database than this one -- or there is none. */
bool pv_moves_stale(int db_entries, long db_commit);

/* Match the log's missing folders to where their files are now, and save the
 * table. Reads the whole playback log and walks the database's filenames
 * twice, so pv_names_init() runs it only when the saved table is stale.
 * 'scratch' is working memory only; nothing in it survives the call. */
void pv_moves_build(void *scratch, size_t size, int db_entries,
                    long db_commit);

/* Read the saved table into the bottom of 'buf' and return the bytes it took,
 * or 0 when there is none for this database or it does not fit. Until the
 * next load or pv_moves_forget(), pv_moves_apply() reads it from there. */
size_t pv_moves_load(void *buf, size_t size, int db_entries, long db_commit);

/* Drop the loaded table, for when its memory is about to be reused. */
void pv_moves_forget(void);

/* Where 'path' lives now if its folder moved, or NULL. The result is a static
 * buffer, valid until the next call. */
const char *pv_moves_apply(const char *path);

/* A value that changes whenever the saved table does, for a cache built from
 * moved paths to be keyed to. 0 when there is no table for this database. */
unsigned long pv_moves_ident(int db_entries, long db_commit);

/* Delete the saved table. The next pv_names_init() builds a new one. */
void pv_moves_discard(void);

#endif /* _PV_MOVES_H */
