/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to art_cache.c.
 ****************************************************************************/

#ifndef _ART_CACHE_H_
#define _ART_CACHE_H_

#include <stdbool.h>
#include <stdint.h>

struct bitmap;

/* On-disk thumbnail file format: this header, followed by width*height native
 * (fb_data) pixels in ROW-MAJOR order. Exposed so consumers (e.g. Cover Flow)
 * can read a cached thumbnail directly. */
#define ART_CACHE_MAGIC          0x5441u  /* 'AT' */
/* Bump whenever the cached pixels' meaning changes (format, geometry, fit rule).
 * aa_check_format_version() purges the thumbnails when this moves, so a stale
 * cache regenerates instead of being skipped by the generator (bare file_exists)
 * while the reader rejects it as the wrong version.
 *   1 -> 2: AA_FIT_COVER implemented; "coverflow" thumbs are now centre-cropped
 *           squares rather than letterboxed CONTAIN fits.
 *   2 -> 3: added the 300px "wps" size.
 *   3 -> 4: coverflow 128 -> 140; single podboxnoart placeholder.
 *   4 -> 5: list 48 -> 46.
 *   5 -> 6: list 46 -> 44 (fits 4 rows of 46px).
 *   6 -> 7: coverflow back to 128 (140 tore the angled slides). */
#define ART_CACHE_FORMAT_VERSION 7

struct art_cache_header
{
    uint16_t magic;
    uint16_t version;
    uint16_t width;
    uint16_t height;
};

/* Background, database-driven album-art thumbnail cache.
 *
 * A dedicated low-priority thread walks the tagcache once the database is
 * ready and idle, resolves each album folder's cover art, and renders it to
 * a set of square thumbnails (see apps/art_sizes.h) under
 * ROCKBOX_DIR/thumbcache/<sizename>/<hash>.aat. Thumbnails are keyed by a hash
 * of the album's folder path, so once a folder is done later passes skip it
 * with a cheap existence check (no art re-resolution), and keys stay valid
 * across database rebuilds. */

/* Start the background cache thread. Call once at startup, after tagcache. */
void art_cache_init(void);

/* Load a cached .aat thumbnail from an open fd, area-average downscaled to
 * bm->width x bm->height (bm->data must hold that many fb_data). Returns the
 * pixel byte count, or <= 0 on failure. Used by the buffering path so the WPS
 * renders the 300px cache instead of re-decoding source art. */
int art_cache_load_aat(int fd, struct bitmap *bm, int max_size);

/* True while a generation pass is running. Drives the status-bar %lc token. */
bool art_cache_is_busy(void);

/* Purge every cached thumbnail and regenerate from scratch (background). */
void art_cache_invalidate(void);

/* Run a generation pass again without purging, so artwork added since the last
 * pass is picked up. Needed because the cache thread otherwise idles until the
 * database's track count changes, and adding a folder.jpg does not change it. */
void art_cache_rescan(void);

/* Resolve the cache-file path for a given album folder and size index.
 * Returns true and fills 'out' if a thumbnail is available, false otherwise.
 * 'dir' is the album's folder path (the directory containing the track), with no
 * trailing slash -- the same key generation uses.
 *
 * When the folder has no real art, the shared placeholder thumbnail is returned
 * instead (if generated), so callers get a valid thumbnail without managing a
 * "missing art" case. 'is_fallback' (may be NULL) is set true in that case, so a
 * caller that wants to prefer another source (e.g. embedded ID3 art) still can. */
bool art_cache_lookup(const char *dir, int size_index,
                           char *out, int out_len, bool *is_fallback);

/* Path to the shared "no artist photo" placeholder (a silhouette) for a size.
 * True if it exists. Album rows get their placeholder folded into
 * art_cache_lookup(); artist rows need a distinct one, so it is requested
 * separately -- see the artist branch in tree.c's art callback. */
bool art_cache_artist_fallback(int size_index, char *out, int out_len);

/* Number of configured thumbnail sizes, and accessors for each. */
int         art_cache_num_sizes(void);
int         art_cache_size_dim(int size_index);
const char *art_cache_size_name(int size_index);
/* Index of the named size in the table, or -1 if not present. */
int         art_cache_size_index(const char *name);

#endif /* _ART_CACHE_H_ */
