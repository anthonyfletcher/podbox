/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to pv_art.c.
 ****************************************************************************/
#ifndef _PV_ART_H
#define _PV_ART_H

#include <stdbool.h>
#include <stddef.h>
#include "config.h"

struct bitmap;

/* Thumbnails for the top-five rows, taken from the artwork cache.
 *
 * The cache keys everything by a hash of a folder path, and the aggregates
 * captured that hash while reading the log -- so a row's artwork costs one
 * file open, with no database search and no image decoding. The thumbnails
 * are already the right pixels; loading one is a read.
 *
 * A row whose art is missing gets nothing back and falls through to its rank
 * badge, which is why this deliberately does NOT use art_cache_lookup(): that
 * substitutes the shared "no art" placeholder, and a numbered badge says more
 * than a row of identical question marks. */

/* How many rows can hold art at once, and how big each is drawn. */
#define PV_ART_SLOTS 5
#define PV_ART_PX    30

/* Bytes of working memory to hand pv_art_init(). */
#define PV_ART_BYTES (PV_ART_SLOTS * PV_ART_PX * PV_ART_PX * (int)sizeof(fb_data))

/* Point the loader at its scratch memory and resolve the cache size to read.
 * Safe to call when there is no artwork cache: every load then simply fails
 * and every row shows its number. */
void pv_art_init(void *buf, size_t bufsz);

/* Forget what the slots hold, before drawing a different card's rows. */
void pv_art_reset(void);

/* The thumbnail for a folder hash, into slot 'slot', or NULL if there is
 * none. The returned bitmap points into the scratch buffer and stays valid
 * until that slot is loaded again. */
const struct bitmap *pv_art_get(unsigned int hash, int slot);

#endif /* _PV_ART_H */
