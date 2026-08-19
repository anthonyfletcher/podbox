/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to art_cache.c.
 ****************************************************************************/

#ifndef _ART_CACHE_H_
#define _ART_CACHE_H_

#include <stdbool.h>
#include <stdint.h>
#include "system/bg_task.h"
#include "draw/img_filter.h"  /* struct img_filter, the caller-supplied chain */
#include "art_sizes.h"       /* enum art_layout, named by the file header */

struct bitmap;

/* On-disk thumbnail file format: this header, followed by width*height native
 * (fb_data) pixels in the order the header's 'layout' names. Exposed so
 * consumers (e.g. Cover Flow) can read a cached thumbnail directly. */
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
 *   6 -> 7: coverflow back to 128 (140 tore the angled slides).
 *   7 -> 8: header gained 'layout'; coverflow thumbs are stored column-major so
 *           the carousel can read one straight into a slide. */
#define ART_CACHE_FORMAT_VERSION 8

struct art_cache_header
{
    uint16_t magic;
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint16_t layout;   /* enum art_layout, as stored */
    uint16_t pad;      /* keeps the pixel data 4-byte aligned */
};

/* Background, database-driven album-art thumbnail cache.
 *
 * A dedicated low-priority thread walks the tagcache once the database is
 * ready and idle, resolves each album folder's cover art, and renders it to
 * a set of square thumbnails (see metadata/art_sizes.h) under
 * ROCKBOX_DIR/thumbcache/<sizename>/<hash>.aat. Thumbnails are keyed by a hash
 * of the album's folder path, so once a folder is done later passes skip it
 * with a cheap existence check (no art re-resolution), and keys stay valid
 * across database rebuilds. */

/* Start the background cache thread. Call once at startup, after tagcache. */
void art_cache_init(void);

/* Load a cached .aat thumbnail from an open fd, area-average downscaled to
 * bm->width x bm->height (bm->data must hold that many fb_data). Returns the
 * pixel byte count, or <= 0 on failure. Used by the buffering path so the WPS
 * renders the 300px cache instead of re-decoding source art.
 *
 * `filter` is the chain to run on the way out, or NULL for none. It is the
 * caller's rather than this function's, because each consumer treats cached art
 * differently and the buffering path must pass NULL: the bitmap it loads is what
 * the dynamic-colour extractor derives its palette from, and that palette has to
 * describe the album rather than anyone's treatment of it. */
int art_cache_load_aat(int fd, struct bitmap *bm, int max_size,
                       const struct img_filter *filter);


/* True while a generation pass is running. Drives the status-bar %lc token. */
bool art_cache_is_busy(void);

/* What the current -- or, once it has finished, the last -- pass covered.
 * "art" counts the folders that ended up with a thumbnail; the difference is
 * the folders that simply have no cover image to find. */
struct art_cache_counts
{
    int albums;
    int album_art;
    int artists;
    int artist_art;
};
void art_cache_get_counts(struct art_cache_counts *out);

/* The folder a running pass is on, or "" when idle. */
const char *art_cache_activity(void);

/* The caching pass, for the standard triggers.
 *
 * bg_task_rebuild() purges every cached thumbnail and regenerates from
 * scratch. bg_task_update() runs a pass without purging, so artwork added
 * since the last one is picked up -- needed because the thread otherwise idles
 * until the database's track count changes, and adding a folder.jpg does not
 * change it. That pass is cheap, since a folder whose thumbnails all exist is
 * skipped with an existence check. */
extern struct bg_task art_cache_task;

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

/* The same lookup keyed by the folder's hash rather than its path.
 *
 * The hash is all the lookup ever uses, so a caller that resolves the same
 * folder repeatedly can keep the hash instead of the path and skip both the
 * path and the work of producing it. The carousel does exactly that: finding an
 * album's folder means a database search, so it is done once when the album
 * index is built and only the hash is carried per slide.
 *
 * art_cache_dir_hash() produces the key, and returns 0 for a NULL path. Callers
 * use 0 as their "folder not resolved" marker; a real path can hash to 0 too,
 * and the cost of that collision is one folder's art not being found. */
unsigned int art_cache_dir_hash(const char *dir);
bool art_cache_lookup_hash(unsigned int dir_hash, int size_index,
                           char *out, int out_len, bool *is_fallback);

/* Where a folder's thumbnail would be, said without going near the disk.
 *
 * For a caller that is about to open the file anyway: the lookups above answer
 * "is it there?" by opening it and closing it again, so asking them first and
 * then opening makes two of what should be one. Opening the path this hands
 * back is itself the existence test. */
void art_cache_thumb_path(unsigned int dir_hash, int size_index,
                          char *out, int out_len);

/* The folders the last completed pass found no art for, as a file of one path
 * per line. Written only when a pass finishes, so an interrupted one leaves
 * the previous list rather than a partial one. Read by the health screen
 * (screens/system/art_health.c); absent until a pass has completed. */
const char *art_cache_noart_list(bool artists);

/* Number of configured thumbnail sizes, and accessors for each. */
int         art_cache_num_sizes(void);
int         art_cache_size_dim(int size_index);
const char *art_cache_size_name(int size_index);
/* Index of the named size in the table, or -1 if not present. */
int         art_cache_size_index(const char *name);

#endif /* _ART_CACHE_H_ */
