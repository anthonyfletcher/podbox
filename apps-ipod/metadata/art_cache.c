/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Disk cache for cover art -- BOTH album art and artist art. Pre-scales each
 * image to the sizes skins ask for and stores it, so browsing does not
 * re-decode on every track. Album art comes from the album folder's image or a
 * track's embedded art, as the album art source setting orders them; artist
 * art from an image in its parent or, failing that, the folder above. Each has
 * its own placeholder for when nothing is found.
 *
 * A pass after a library update that only added tracks visits the folders of
 * those tracks alone. One after a deletion, an Update, a Rebuild or a change
 * of format visits every folder, and only that kind removes the thumbnails of
 * folders that have gone and rewrites the lists of folders with no art.
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "config.h"
#include "system/library_files.h"
#include "database/libfile.h"


#include "system.h"
#include "kernel.h"
#include "core_alloc.h"
#include "string-extra.h"
#include "file.h"
#include "dir.h"
#include "dircache.h"               /* whether a stamp check reads the disk */
#include "pathfuncs.h"
#include "rbpaths.h"
#include "metadata.h"
#include "albumart.h"
#include "art_cache.h"
#include "art_sizes.h"
#include "settings/settings.h"  /* global_settings.art_cache_* */
#include "system/debug_log.h"
#include "database/tagcache.h"
#include "system/bg_task.h"         /* the caching pass runs as one */
#include "files/path_list.h"        /* the "found nothing" lists */
#include "system/strutil.h"         /* is_disc_folder() */
#include "lcd.h"
#include "draw/bmp.h"
#include "draw/img_filter.h"
#include "bitmaps/podboxnoart.h" /* shared "no art" placeholder for aa_ensure_fallback */
#include "draw/jpeg_load.h"
#include "events.h"
#include "system/appevents.h"
#include "audio.h"
#include "cpu.h"

/* Define LOGF_ENABLE to enable logf output in this file */
/*#define LOGF_ENABLE*/
#include "logf.h"

#define THUMBCACHE_DIR LIB_ART_DIR
/* Folders a pass found no art for, one path per line, for the health screen
 * (screens/system/art_health.c). Written to a .new and renamed only when a
 * pass finishes, so an aborted pass leaves the previous -- complete -- list
 * standing rather than a partial one that reads as "the rest are fine". */
#define AA_NOART_ALBUMS  LIB_NO_ART_ALBUMS_FILE
#define AA_NOART_ARTISTS LIB_NO_ART_ARTISTS_FILE
/* Entry count the cache was last completed for, so a restart with an
 * unchanged library does not re-walk the whole database. */
/* What each folder's thumbnails were made from, so a pass can tell a replaced
 * or deleted image from an unchanged one: one struct aa_stamp per folder, in a
 * libfile. Later records override earlier ones, which is what lets
 * aa_handle_offer() append rather than rewrite. Its version is the
 * thumbnails' format, and its marks the library the last completed pass
 * covered. A record with folder hash 0, which no folder has, holds
 * aa_marks_key for those marks. */
#define AA_STAMP_FILE   THUMBCACHE_DIR "/stamps.dat"
#define AA_STAMP_MAGIC  LIB_STAMPS_MAGIC

/* On-disk thumbnail format (struct art_cache_header + native pixels, in the
 * order the header names) is declared in art_cache.h so consumers can read it.
 * A magic/version lets a future format change be detected per file rather than
 * needing a global cache wipe. */

/* Folder table: open-addressed by directory-path hash, holding album folders
 * and their parent (artist) folders alike. Sized generously; a folder that
 * finds it full is skipped for that pass. */
#define AA_SEEN_SLOTS 16384  /* power of two */

/* One folder's entry in the table: its path hash and the stamp of the image
 * its thumbnails were made from. The table is the pass's seen set as well, with
 * a bit per slot saying whether this pass has reached the folder yet. */
struct aa_stamp
{
    unsigned int key;    /* folder path hash; 0 marks an empty slot */
    unsigned int stamp;
};
#define AA_TABLE_BYTES (AA_SEEN_SLOTS * sizeof(struct aa_stamp) \
                        + AA_SEEN_SLOTS / 8)

/* Stamps that are not an image's. NONE is a folder with no record, whose
 * thumbnails a pass adopts as they stand rather than regenerating -- the case
 * for every folder cached before stamps existed, and after the stamp file is
 * lost. EMBEDDED is art from a track's tags. NO_EMBED is a folder whose track
 * was read and held no art, so a pass does not read it again. */
#define AA_STAMP_NONE     0u
#define AA_STAMP_EMBEDDED 1u
#define AA_STAMP_NO_EMBED 2u

/* tagcache_entry_key() of the last entry the marks cover, folded to 32 bits;
 * 0 for none. Read with the table. */
static unsigned int aa_marks_key;

static unsigned int aa_fold_key(uint64_t key)
{
    return (unsigned int)(key ^ key >> 32);
}

/* The table, while a pass holds it; NULL otherwise. */
static struct aa_stamp *aa_stamps;
static unsigned char *aa_visited;

static volatile bool cache_busy;

/* Counts every change to a thumbnail: see art_cache_generation(). */
static volatile unsigned int aa_generation;

/* What the current (or last completed) pass has covered, and the folder it is
 * on. Every one of these is a count of a branch the pass already takes, so
 * keeping them costs nothing beyond the increment; aa_dir is the scratch
 * buffer the pass already keeps the current directory in. */
static struct art_cache_counts aa_counts;

/* Scratch id3 used only to feed search_albumart_files(); kept out of the
 * thread stack because struct mp3entry is large. */
static struct mp3entry aa_id3;

/* Scratch buffers used only by the background thread (aa_run_pass /
 * aa_generate_one). Kept off the thread stack -- together with the JPEG
 * decoder's own deep frames they otherwise overflow it. Single-threaded and
 * non-reentrant, so module-level statics are safe. */
static char aa_tcs_buf[TAGCACHE_BUFSZ];
static char aa_artpath[MAX_PATH];
static char aa_dir[MAX_PATH];
static char aa_artist_dir[MAX_PATH];
static char aa_probe[MAX_PATH];
static char aa_check_path[MAX_PATH];
static char aa_out_path[MAX_PATH];
static char aa_chain_path[MAX_PATH];
static char aa_stat_dir[MAX_PATH];
static char aa_tmp_path[MAX_PATH];

/* Stamp-file records go through this a batch at a time. */
#define AA_STAMP_BATCH 64
static struct aa_stamp aa_stamp_io[AA_STAMP_BATCH];

/* Names collected by one round of aa_purge_thumbs(). A thumbnail is
 * "%08x.aat" and the shared placeholder "_fallback.aat", so 32 bytes holds
 * any of them with room to spare; anything longer is left alone rather than
 * truncated, since a truncated name names a different file. */
#define AA_PURGE_BATCH 64
static char aa_purge_names[AA_PURGE_BATCH][32];

/* Queue event id: the current track's embedded art was offered for caching. */
#define AA_EVENT_OFFER 1

/* Filled by the track-change hook (playback thread), consumed by the aa thread.
 * Rockbox is cooperatively scheduled, so the two never run at once and no lock is
 * needed; a newer offer simply supersedes one not yet processed. */
static struct
{
    char          path[MAX_PATH];
    off_t         pos;
    unsigned long size;
    int           flags;
} aa_offer;

/* An offer is on the queue. A pass does not drain the queue, so each track
 * change posting its own would overflow it; one queued offer reads whichever
 * aa_offer holds when it is handled. The thread drops what is queued across a
 * USB session, so a pass starting and the check after USB clear it too. */
static volatile bool aa_offer_posted;

bool art_cache_is_busy(void)
{
    return cache_busy;
}

void art_cache_get_counts(struct art_cache_counts *out)
{
    *out = aa_counts;
}

const char *art_cache_activity(void)
{
    return cache_busy ? aa_dir : "";
}

int art_cache_num_sizes(void)
{
    return ART_CACHE_NUM_SIZES;
}

int art_cache_size_dim(int size_index)
{
    if (size_index < 0 || size_index >= ART_CACHE_NUM_SIZES)
        return 0;
    return art_sizes[size_index].dim;
}

enum art_layout art_cache_size_layout(int size_index)
{
    if (size_index < 0 || size_index >= ART_CACHE_NUM_SIZES)
        return AA_ROWS;
    return art_sizes[size_index].layout;
}

const char *art_cache_size_name(int size_index)
{
    if (size_index < 0 || size_index >= ART_CACHE_NUM_SIZES)
        return NULL;
    return art_sizes[size_index].name;
}

int art_cache_size_index(const char *name)
{
    int i;
    if (!name)
        return -1;
    for (i = 0; i < ART_CACHE_NUM_SIZES; i++)
        if (!strcmp(art_sizes[i].name, name))
            return i;
    return -1;
}

/* Modified FNV hash (same as PictureFlow's, good avalanche/distribution). */
static unsigned int aa_hash(const char *str)
{
    const unsigned int p = 16777619;
    unsigned int hash = 0x811C9DC5;

    if (!str)
        return 0;

    while (*str)
        hash = (hash ^ (unsigned char)*str++) * p;
    hash += hash << 13;
    hash ^= hash >> 7;
    hash += hash << 3;
    hash ^= hash >> 17;
    hash += hash << 5;
    return hash;
}

static void aa_cache_path(char *out, int out_len, int size_index,
                          unsigned int arthash)
{
    snprintf(out, out_len, THUMBCACHE_DIR "/%s/%08x.aat",
             art_sizes[size_index].name, arthash);
}

/* The shared placeholder thumbnail for a size. Its name cannot collide with
 * an %08x hash filename, so it lives alongside the real thumbnails. */
static void aa_fallback_path(char *out, int out_len, int size_index)
{
    snprintf(out, out_len, THUMBCACHE_DIR "/%s/fallback.aat",
             art_sizes[size_index].name);
}

/* Streaming area-average downscale of an open .aat into `bm` (fd is consumed).
 *
 * The source is read once, top to bottom: each destination row averages the
 * contiguous band of source rows that map to it, so at most a few source rows
 * are resident at a time (aat_band[]). Downscale only -- the WPS never asks for
 * more than the 300px cache. Returns the pixel byte count (like the decoders,
 * so load_image can add sizeof(struct bitmap)), or <= 0 on failure. */
#define AAT_BAND_MAX 8    /* source rows resident per output row */
static fb_data aat_band[AAT_BAND_MAX * ART_CACHE_MAX_DIM];
/* Held while aat_band[] is in use: buffering, the art pass and the Report
 * all read thumbnails, and read() locks only the file. */
static struct mutex aat_mutex;

/* ---------------------------------------------------------------------- *
 * Reading a cached thumbnail                                              *
 * ---------------------------------------------------------------------- */

int art_cache_load_aat(int fd, struct bitmap *bm, int max_size,
                       const struct img_filter *filter)
{
    struct art_cache_header hdr;
    int dw = bm->width, dh = bm->height;
    int sw, sh, dy, dx;
    int rc = dw * dh * FB_DATA_SZ;

    lseek(fd, 0, SEEK_SET);
    if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        hdr.magic != ART_CACHE_MAGIC || hdr.version != ART_CACHE_FORMAT_VERSION)
        return -1;

    /* This reads the file a band of rows at a time, so it can only make sense
     * of a row-major one. Refused rather than transposed: no caller wants a
     * column-major source, and silently reading one the wrong way round would
     * produce a plausible-looking but mirrored thumbnail. */
    if (hdr.layout != AA_ROWS)
        return -1;

    sw = hdr.width;
    sh = hdr.height;
    /* Both dimensions, not just the one that sizes aat_band[]. sh drives
     * the row arithmetic below, and a header claiming a huge height
     * overflows dy * sh, which can leave sy1 behind sy0 -- `rows` is then
     * negative, the clamp above it only tests the high side, and read()
     * takes it as a size_t. 300 is the largest the cache ever writes.
     * A source too elongated to crop is stored non-square, and is refused:
     * scaling it to the size asked for would stretch it. */
    if (sw <= 0 || sh <= 0 || sw > ART_CACHE_MAX_DIM ||
        sh > ART_CACHE_MAX_DIM || sw != sh ||
        dw <= 0 || dh <= 0 || dw > sw || dh > sh)   /* downscale only */
        return -1;
    if ((size_t)dw * dh * FB_DATA_SZ > (size_t)max_size)
        return -1;

    mutex_lock(&aat_mutex);
    for (dy = 0; dy < dh; dy++)
    {
        int sy0 = dy * sh / dh;
        int sy1 = (dy + 1) * sh / dh;
        int rows = sy1 - sy0;
        fb_data *out = (fb_data *)bm->data + (size_t)dy * dw;

        if (rows > AAT_BAND_MAX)
            rows = AAT_BAND_MAX;    /* extreme ratios: sample the top of the band */
        if (read(fd, aat_band, (size_t)rows * sw * FB_DATA_SZ) !=
            (ssize_t)((size_t)rows * sw * FB_DATA_SZ))
        {
            rc = -1;
            break;
        }
        /* skip the remainder of a clamped band so the file stays aligned */
        if (sy1 - sy0 > rows)
            lseek(fd, (off_t)(sy1 - sy0 - rows) * sw * FB_DATA_SZ, SEEK_CUR);

        for (dx = 0; dx < dw; dx++)
        {
            int sx0 = dx * sw / dw;
            int sx1 = (dx + 1) * sw / dw;
            unsigned r = 0, g = 0, b = 0, n = 0;
            int sy, sx;

            for (sy = 0; sy < rows; sy++)
                for (sx = sx0; sx < sx1; sx++)
                {
                    fb_data p = aat_band[sy * sw + sx];
                    r += RGB_UNPACK_RED(p);
                    g += RGB_UNPACK_GREEN(p);
                    b += RGB_UNPACK_BLUE(p);
                    n++;
                }
            out[dx] = n ? LCD_RGBPACK(r / n, g / n, b / n) : 0;
        }
    }
    mutex_unlock(&aat_mutex);

    if (rc > 0 && filter)
        img_filter_apply_banded((fb_data *)bm->data, dw, dh, filter);

    return rc;
}

unsigned int art_cache_dir_hash(const char *dir)
{
    return aa_hash(dir);
}

void art_cache_thumb_path(unsigned int dir_hash, int size_index,
                          char *out, int out_len)
{
    aa_cache_path(out, out_len, size_index, dir_hash);
}

bool art_cache_lookup(const char *dir, int size_index,
                           char *out, int out_len, bool *is_fallback)
{
    if (!dir)
    {
        if (is_fallback)
            *is_fallback = false;
        return false;
    }

    return art_cache_lookup_hash(aa_hash(dir), size_index, out, out_len,
                                 is_fallback);
}

bool art_cache_lookup_hash(unsigned int dir_hash, int size_index,
                           char *out, int out_len, bool *is_fallback)
{
    if (is_fallback)
        *is_fallback = false;
    if (size_index < 0 || size_index >= ART_CACHE_NUM_SIZES)
        return false;

    aa_cache_path(out, out_len, size_index, dir_hash);
    if (file_exists(out))
        return true;

    /* No real art for this folder -- hand back the placeholder so callers don't
     * each have to draw their own "missing art" state. Absent until the cache
     * has generated it (early boot), in which case this returns false and the
     * caller falls back to whatever it did before. */
    aa_fallback_path(out, out_len, size_index);
    if (file_exists(out))
    {
        if (is_fallback)
            *is_fallback = true;
        return true;
    }
    return false;
}

static void aa_ensure_dirs(void)
{
    int i;
    char p[MAX_PATH];
    mkdir(THUMBCACHE_DIR);
    for (i = 0; i < ART_CACHE_NUM_SIZES; i++)
    {
        snprintf(p, sizeof(p), THUMBCACHE_DIR "/%s", art_sizes[i].name);
        mkdir(p);
    }
}

/* An empty stamp file in this format: no folder stamped, and the format
 * recorded, so a pass cut short after the purge does not purge again */
static void aa_stamps_clear(void)
{
    struct libfile_writer w;

    if (libfile_begin(&w, AA_STAMP_FILE, AA_STAMP_MAGIC,
                      ART_CACHE_FORMAT_VERSION, sizeof(struct aa_stamp), NULL))
        libfile_finish(&w, true);
}

static bool aa_check_abort(void);

/* Delete every cached thumbnail of every size (the directories themselves stay).
 * Stops for USB or a shutdown, with no stamp file, so the next pass purges the
 * rest; the format is recorded only once every size is empty. */
static void aa_purge_thumbs(void)
{
    int i;
    char dirpath[MAX_PATH];
    char filepath[MAX_PATH];

    /* The thumbnails are going, so nothing is cached for any entry count.
     * Said here rather than left to whoever asked, because the format-version
     * check below purges from inside a pass that may then be interrupted. */
    bg_task_forget(&art_cache_task);

    /* The miss lists describe thumbnails that are about to stop existing, so
     * they go too rather than being left to describe a cache that is gone. */
    remove(AA_NOART_ALBUMS);
    remove(AA_NOART_ARTISTS);
    remove(AA_STAMP_FILE);

    debug_log(DEBUG_LOG_ARTCACHE, "purge: start");

    for (i = 0; i < ART_CACHE_NUM_SIZES; i++)
    {
        int total = 0, n, removed;

        snprintf(dirpath, sizeof(dirpath), THUMBCACHE_DIR "/%s",
                 art_sizes[i].name);

        /* Trap: removing an entry while readdir() walks the same directory
         * mutates the structure being enumerated, which loses entries at best
         * and does not terminate at worst. Names are collected with the
         * directory open and deleted with it closed, a batch at a time, and
         * a round that frees nothing stops rather than rescanning forever. */
        do
        {
            DIR *d = opendir(dirpath);
            struct dirent *e;

            if (!d)
                break;

            n = 0;
            while (n < AA_PURGE_BATCH && (e = readdir(d)))
            {
                if (e->d_name[0] == '.')
                    continue;
                if (strlcpy(aa_purge_names[n], e->d_name,
                            sizeof(aa_purge_names[0]))
                        >= sizeof(aa_purge_names[0]))
                    continue;
                n++;
            }
            closedir(d);

            removed = 0;
            aa_generation++;
            for (int j = 0; j < n; j++)
            {
                snprintf(filepath, sizeof(filepath), "%s/%s", dirpath,
                         aa_purge_names[j]);
                if (remove(filepath) == 0)
                    removed++;
                yield();
                if (aa_check_abort())
                {
                    debug_log(DEBUG_LOG_ARTCACHE, "purge: interrupted");
                    return;
                }
            }
            total += removed;
        }
        while (n == AA_PURGE_BATCH && removed > 0);

        debug_log(DEBUG_LOG_ARTCACHE, "purge: %s %d files",
                  art_sizes[i].name, total);
    }

    aa_stamps_clear();
    debug_log(DEBUG_LOG_ARTCACHE, "purge: done");
}

/* After a completed pass: every thumbnail of a folder the pass did not reach
 * belongs to one that has gone. fallback.aat, not a hash, is left alone.
 * Collected and removed a batch at a time, as aa_purge_thumbs() does; a stop
 * for USB leaves the rest to the next completed pass. */
static bool aa_was_visited(unsigned int h);

static void aa_remove_orphans(void)
{
    char dirpath[MAX_PATH];
    char filepath[MAX_PATH];
    int i, removed_all = 0;

    for (i = 0; i < ART_CACHE_NUM_SIZES; i++)
    {
        int n, removed;

        snprintf(dirpath, sizeof(dirpath), THUMBCACHE_DIR "/%s",
                 art_sizes[i].name);
        do
        {
            DIR *d = opendir(dirpath);
            struct dirent *e;

            if (!d)
                break;
            n = 0;
            while (n < AA_PURGE_BATCH && (e = readdir(d)))
            {
                char *end;
                unsigned long h;

                if (strlen(e->d_name) != 12
                    || strcasecmp(e->d_name + 8, ".aat"))
                    continue;
                h = strtoul(e->d_name, &end, 16);
                if (end != e->d_name + 8 || aa_was_visited(h))
                    continue;
                strlcpy(aa_purge_names[n++], e->d_name,
                        sizeof(aa_purge_names[0]));
            }
            closedir(d);

            removed = 0;
            for (int k = 0; k < n; k++)
            {
                snprintf(filepath, sizeof(filepath), "%s/%s", dirpath,
                         aa_purge_names[k]);
                if (remove(filepath) == 0)
                    removed++;
                yield();
                if (aa_check_abort())
                {
                    removed_all += removed;
                    goto out;
                }
            }
            removed_all += removed;
        } while (n == AA_PURGE_BATCH && removed > 0);
    }
out:
    if (removed_all)
    {
        aa_generation++;
        debug_log(DEBUG_LOG_ARTCACHE, "orphans: %d removed", removed_all);
    }
}

/* Whether the stamp file says the thumbnails are in this format */
static bool aa_format_current(void)
{
    struct libfile_header h;

    return libfile_peek(AA_STAMP_FILE, AA_STAMP_MAGIC,
                        ART_CACHE_FORMAT_VERSION, &h);
}

/* The generator decides "already cached?" with a bare file_exists(), and the
 * reader rejects any file whose header version doesn't match. So on a format
 * bump the stale files would be skipped forever *and* refused at render time -
 * every cover would silently go blank. Stamp the format version alongside the
 * cache and purge the thumbnails whenever it moves.
 *
 * Being inside the pass, this only ever runs once the pass has been allowed to
 * start; art_cache_init() is what makes sure a bump allows it. */
static bool aa_check_format_version(void)
{
    if (aa_format_current())
        return true;

    logf("albumart cache: new format %d, purging", ART_CACHE_FORMAT_VERSION);
    aa_purge_thumbs();
    return aa_format_current();
}

/* bg_task.artifact_ok: whether the cache is still on disk. A cache deleted
 * over USB leaves the marks in RAM matching, so without this nothing reruns
 * until the library changes. The folders, not every thumbnail: a pass checks
 * those itself, folder by folder. */
static bool aa_artifact_ok(void)
{
    char p[MAX_PATH];
    int i;

    aa_offer_posted = false;
    if (!aa_format_current())
        return false;

    for (i = 0; i < ART_CACHE_NUM_SIZES; i++)
    {
        snprintf(p, sizeof(p), THUMBCACHE_DIR "/%s", art_sizes[i].name);
        if (!dir_exists(p))
            return false;
    }
    return true;
}

/* Extract the directory portion (without trailing slash) of a full path. */
static void aa_dirname(const char *path, char *dir, int dir_len)
{
    const char *sep = strrchr(path, '/');
    int len = sep ? (int)(sep - path) : 0;
    if (len >= dir_len)
        len = dir_len - 1;
    memcpy(dir, path, len);
    dir[len] = 0;
}

/* Whether 'art', found for the folder of 'probe', can stand for it. The search
 * falls back to the folder above, which is the album for a disc folder but the
 * artist for an album folder -- and the artist's picture is no album's cover.
 * So for an album an image from above counts only for a disc folder. For an
 * artist it always counts, except from the volume root: under
 * <artist>/<album_type>/<album> the folder above the album is the type, and
 * the artist's picture is one further up. Trap: under <Music>/<artist> or
 * <genre>/<artist> the folder above is not the artist's, and an image there
 * becomes the picture of every artist without one of their own. */
static bool aa_art_is_folders(const char *probe, const char *art, bool artist)
{
    const char *end = strrchr(probe, '/');
    const char *start;
    size_t dirlen;

    if (!end)
        return true;
    dirlen = end - probe + 1;
    if (!strncmp(probe, art, dirlen) && !strchr(art + dirlen, '/'))
        return true;
    if (artist)
        return strrchr(art, '/') != art;

    for (start = end; start > probe && start[-1] != '/'; start--)
        ;
    return is_disc_folder(start, end);
}

/* The table slot for folder hash 'h', claimed with no stamp if it is new.
 * NULL when the table is full. h==0 is remapped so 0 can mark an empty slot. */
static struct aa_stamp *aa_slot(unsigned int h)
{
    unsigned int i, idx;
    if (h == 0)
        h = 1;
    idx = h & (AA_SEEN_SLOTS - 1);
    for (i = 0; i < AA_SEEN_SLOTS; i++)
    {
        struct aa_stamp *s = &aa_stamps[(idx + i) & (AA_SEEN_SLOTS - 1)];
        if (s->key == 0)
        {
            s->key = h;
            s->stamp = AA_STAMP_NONE;
            return s;
        }
        if (s->key == h)
            return s;
    }
    return NULL;
}

/* Whether this pass reached the folder with hash 'h' */
static bool aa_was_visited(unsigned int h)
{
    unsigned int i, idx;

    if (h == 0)
        h = 1;
    idx = h & (AA_SEEN_SLOTS - 1);
    for (i = 0; i < AA_SEEN_SLOTS; i++)
    {
        unsigned int at = (idx + i) & (AA_SEEN_SLOTS - 1);

        if (aa_stamps[at].key == 0)
            return false;
        if (aa_stamps[at].key == h)
            return aa_visited[at / 8] & (1u << (at % 8));
    }
    return false;
}

/* A folder this pass skipped for want of a slot. Its thumbnails are not
 * orphans, so the pass then removes none. */
static bool aa_table_full;

/* The slot for 'h' the first time this pass reaches it; NULL if the pass has
 * been here already, or the table is full (the folder is then skipped). */
static struct aa_stamp *aa_visit(unsigned int h)
{
    struct aa_stamp *s = aa_slot(h);
    unsigned int i;

    if (!s)
    {
        aa_table_full = true;
        return NULL;
    }
    i = s - aa_stamps;
    if (aa_visited[i / 8] & (1u << (i % 8)))
        return NULL;
    aa_visited[i / 8] |= 1u << (i % 8);
    return s;
}

/* Fill the table from the stamp file. A missing or unrecognised file leaves
 * every folder unstamped, so the pass adopts what is on disk. */
static void aa_stamps_load(void)
{
    struct libfile_header h;
    int fd, n, i;

    fd = libfile_open(AA_STAMP_FILE, AA_STAMP_MAGIC, ART_CACHE_FORMAT_VERSION,
                      sizeof(struct aa_stamp), &h, NULL);
    aa_marks_key = 0;
    if (fd < 0)
        return;
    while ((n = read(fd, aa_stamp_io, sizeof(aa_stamp_io))) > 0)
    {
        n /= sizeof(aa_stamp_io[0]);
        for (i = 0; i < n; i++)
        {
            struct aa_stamp *s;

            if (aa_stamp_io[i].key == 0)
            {
                aa_marks_key = aa_stamp_io[i].stamp;
                continue;
            }
            s = aa_slot(aa_stamp_io[i].key);
            if (s)
                s->stamp = aa_stamp_io[i].stamp;
        }
    }
    close(fd);
}

/* Write the table back. A completed pass keeps only the folders it reached,
 * which drops those gone from the library; an interrupted one keeps every
 * stamp, since the folders it never reached still have their thumbnails.
 * The header keeps the last completed pass's marks, which are what the file
 * still covers: an interrupted pass that cleared them would have the next one
 * visit every folder. */
static void aa_stamps_save(bool completed)
{
    const struct bg_marks *done = &art_cache_task.done_marks;
    struct libfile_marks lm;
    struct libfile_writer w;
    bool ok;
    int i, n = 0;

    lm.entries = done->entries;
    lm.commitid = done->commitid;
    lm.deleted = done->deleted;
    if (!libfile_begin(&w, AA_STAMP_FILE, AA_STAMP_MAGIC,
                       ART_CACHE_FORMAT_VERSION, sizeof(struct aa_stamp),
                       &lm))
        return;
    ok = true;
    if (aa_marks_key)
    {
        aa_stamp_io[n].key = 0;
        aa_stamp_io[n++].stamp = aa_marks_key;
    }

    for (i = 0; ok && i < AA_SEEN_SLOTS; i++)
    {
        const struct aa_stamp *s = &aa_stamps[i];

        if (s->key == 0 || s->stamp == AA_STAMP_NONE)
            continue;
        if (completed && !(aa_visited[i / 8] & (1u << (i % 8))))
            continue;
        aa_stamp_io[n++] = *s;
        if (n == AA_STAMP_BATCH)
        {
            ok = libfile_write(&w, aa_stamp_io, sizeof(aa_stamp_io), n);
            n = 0;
        }
    }
    if (ok && n)
        ok = libfile_write(&w, aa_stamp_io, n * sizeof(aa_stamp_io[0]), n);
    libfile_finish(&w, ok);
}

/* Record one folder's stamp outside a pass, by appending to the file. */
static void aa_stamp_append(unsigned int h, unsigned int stamp)
{
    struct aa_stamp rec = { h ? h : 1, stamp };

    libfile_append(AA_STAMP_FILE, AA_STAMP_MAGIC, ART_CACHE_FORMAT_VERSION,
                   sizeof(rec), &rec, 1);
}

/* The stamp of the image at 'path': its path, size and modification time,
 * hashed. Size and time come from the directory entry, so no image is read.
 * Equality is all a stamp is compared for -- a replacement copied in with an
 * older timestamp is still a different one. */
static unsigned int aa_art_stamp(const char *path)
{
    const char *name = strrchr(path, '/');
    unsigned int stamp = aa_hash(path);
    DIR *d;
    struct dirent *e;

    if (name)
    {
        aa_dirname(path, aa_stat_dir, sizeof(aa_stat_dir));
        d = opendir(aa_stat_dir[0] ? aa_stat_dir : "/");
        if (d)
        {
            while ((e = readdir(d)))
            {
                if (strcasecmp(e->d_name, name + 1))
                    continue;
                struct dirinfo info = dir_get_info(d, e);
                stamp = (stamp ^ (unsigned int)info.size) * 16777619u;
                stamp = (stamp ^ (unsigned int)info.mtime) * 16777619u;
                break;
            }
            closedir(d);
        }
    }

    /* never one of the stamps that are not an image's */
    if (stamp <= AA_STAMP_NO_EMBED)
        stamp += 3;
    return stamp;
}

/* Whether every size of one folder's thumbnails is on disk. */
static bool aa_thumbs_exist(unsigned int dh)
{
    int s;

    for (s = 0; s < ART_CACHE_NUM_SIZES; s++)
    {
        aa_cache_path(aa_check_path, sizeof(aa_check_path), s, dh);
        if (!file_exists(aa_check_path))
            return false;
    }
    return true;
}

unsigned int art_cache_generation(void)
{
    return aa_generation;
}

/* Delete every size of one folder's thumbnails. */
static void aa_remove_thumbs(unsigned int dh)
{
    int s;

    aa_generation++;
    for (s = 0; s < ART_CACHE_NUM_SIZES; s++)
    {
        aa_cache_path(aa_check_path, sizeof(aa_check_path), s, dh);
        remove(aa_check_path);
    }
}

/* Where an album-art image comes from: a file on disk (folder art), or a JPEG
 * blob embedded in an audio file (emb_pos >= 0, reusing metadata already parsed
 * by playback -- see aa_track_change_cb()). */
struct aa_src
{
    const char   *path;      /* file to read: the folder image, or the track */
    off_t         emb_pos;   /* embedded JPEG offset, or -1 for a whole file */
    unsigned long emb_size;  /* embedded JPEG blob length (embedded only) */
    int           emb_flags; /* embedded aa type/flags for clip_jpeg_fd */
};

/* Read the source image into `bm` with `fmt`, requesting a `w` x `h` target (the
 * readers treat that as the resize target, or overwrite it with the source's own
 * dimensions when `fmt` carries no FORMAT_RESIZE). Returns the reader's result. */
static int aa_read_source(const struct aa_src *src, struct bitmap *bm, int fmt,
                          int w, int h, void *workbuf, size_t workbuf_sz)
{
    memset(bm, 0, sizeof(*bm));
    bm->data = workbuf;
    bm->width = w;
    bm->height = h;
    bm->format = FORMAT_NATIVE;

    if (src->emb_pos >= 0)
    {
        /* Embedded art -- always JPEG in Rockbox. lseek + clip_jpeg_fd is how
         * the WPS decodes it, and it handles the ID3-unsync flag in emb_flags. */
        int fd = open(src->path, O_RDONLY);
        int rc;
        if (fd < 0)
            return -1;
        lseek(fd, src->emb_pos, SEEK_SET);
        rc = clip_jpeg_fd(fd, src->emb_flags, src->emb_size, bm,
                          (int)workbuf_sz, fmt, NULL);
        close(fd);
        return rc;
    }

    size_t namelen = strlen(src->path);
    if (namelen >= 4 && strcmp(src->path + namelen - 4, ".bmp") != 0)
    {
        return read_jpeg_file(src->path, bm, (int)workbuf_sz, fmt, NULL);
    }
    return read_bmp_file(src->path, bm, (int)workbuf_sz, fmt, NULL);
}

/* Target size for an AA_FIT_COVER decode: scale so the SHORTER side lands on
 * exactly `dim`, leaving the longer side >= dim to be cropped away. False if the
 * staged image would not fit the work buffer (caller falls back to CONTAIN) or
 * its dimensions are nonsense. */
static bool aa_cover_dim(int sw, int sh, int dim, int *tw, int *th)
{
    if (sw <= 0 || sh <= 0)
        return false;

    if (sw <= sh)
    {
        *tw = dim;
        *th = (sh * dim + sw / 2) / sw;     /* rounded; >= dim */
    }
    else
    {
        *th = dim;
        *tw = (sw * dim + sh / 2) / sh;
    }

    /* rounding must never leave the short side under dim -- the crop below
     * assumes both sides are at least dim */
    if (*tw < dim)
        *tw = dim;
    if (*th < dim)
        *th = dim;

    return (long)*tw * *th <= ART_CACHE_COVER_MAX_PX &&
           *tw <= ART_CACHE_COVER_MAX_W;
}

/* Centre-crop a tw x th image down to dim x dim, in place. Each output row sits
 * at or before its source row (dim <= tw), so copying front-to-back is safe. */
static void aa_crop_center(void *buf, int tw, int th, int dim)
{
    fb_data *px = buf;
    int x0 = (tw - dim) / 2;
    int y0 = (th - dim) / 2;
    int y;

    for (y = 0; y < dim; y++)
        memmove(px + (size_t)y * dim,
                px + (size_t)(y + y0) * tw + x0,
                (size_t)dim * FB_DATA_SZ);
}

/* Write a native (row-major fb_data) bitmap to a .aat file: the shared header
 * followed by the pixels. Removes the file on a short write. */
static bool aa_write_aat(const char *out_path, struct bitmap *bm, int size_index)
{
    struct art_cache_header hdr;
    size_t bytes;
    bool ok;
    enum art_layout layout = AA_ROWS;
    int fd;

    /* Transposed here, once, for a size that wants columns -- and only when the
     * thumbnail is square, because that is an in-place swap where a rectangle
     * would need a second full-size buffer. A COVER thumbnail is square unless
     * its source was too elongated to crop, so the occasional rectangle is
     * stored by rows and the header says which it is. */
    if (art_sizes[size_index].layout == AA_COLUMNS
        && bm->width == bm->height)
    {
        fb_data *px = (fb_data *)bm->data;
        int n = bm->width, r, c;

        for (r = 0; r < n; r++)
            for (c = r + 1; c < n; c++)
            {
                fb_data t = px[r * n + c];
                px[r * n + c] = px[c * n + r];
                px[c * n + r] = t;
            }

        layout = AA_COLUMNS;
    }

    /* Written whole under another name and renamed over the thumbnail, so a
     * reader never sees one part-written. ".new" is not ".aat", so the orphan
     * sweep passes it by. */
    if (strlcpy(aa_tmp_path, out_path, sizeof(aa_tmp_path))
            >= sizeof(aa_tmp_path))
        return false;
    strcpy(aa_tmp_path + strlen(aa_tmp_path) - 4, ".new");
    fd = open(aa_tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;

    hdr.magic = ART_CACHE_MAGIC;
    hdr.version = ART_CACHE_FORMAT_VERSION;
    hdr.width = bm->width;
    hdr.height = bm->height;
    hdr.layout = layout;
    hdr.pad = 0;
    bytes = (size_t)bm->width * bm->height * FB_DATA_SZ;

    ok = (write(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)) &&
         (write(fd, bm->data, bytes) == (ssize_t)bytes);
    close(fd);

    if (ok)
        ok = rename(aa_tmp_path, out_path) == 0;
    if (!ok)
        remove(aa_tmp_path);
    return ok;
}

/* Decode/scale the source art into a thumbnail and write it.
 *
 * CONTAIN: fitted (aspect preserved) inside dim x dim, so the cached image is
 * only square when the source is.
 * COVER:   decoded to the cover size (shorter side == dim) and centre-cropped to
 * exactly dim x dim. A source wider than ART_CACHE_COVER_MAX_ASPECT falls
 * back to CONTAIN rather than overrun the work buffer.
 *
 * Returns true on success. */
static bool aa_generate_one(const struct aa_src *src, int size_index,
                            const char *out_path, void *workbuf,
                            size_t workbuf_sz)
{
    int dim = art_sizes[size_index].dim;
    bool cover = art_sizes[size_index].fit == AA_FIT_COVER;
    struct bitmap bm;
    int fmt = FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_DITHER;
    int tw = dim, th = dim;
    int ret;

    if (cover)
    {
        /* Header-only probe: with no FORMAT_RESIZE the readers report the
         * source's own dimensions, and FORMAT_RETURN_SIZE stops them decoding
         * any pixels. */
        ret = aa_read_source(src, &bm, FORMAT_NATIVE | FORMAT_RETURN_SIZE,
                             dim, dim, workbuf, workbuf_sz);
        if (ret <= 0 || !aa_cover_dim(bm.width, bm.height, dim, &tw, &th))
        {
            cover = false;      /* unreadable header, or too elongated to crop */
            tw = th = dim;
        }
    }

    /* KEEP_ASPECT is what makes a decode CONTAIN: it shrinks the requested box
     * to the source's aspect. COVER instead asks for the exact cover size it
     * computed above, so the decode must NOT keep aspect. */
    if (!cover)
        fmt |= FORMAT_KEEP_ASPECT;

    ret = aa_read_source(src, &bm, fmt, tw, th, workbuf, workbuf_sz);

    if (ret <= 0 || bm.width <= 0 || bm.height <= 0)
        return false;

    /* the decode should have landed on exactly tw x th, but never crop from an
     * image smaller than the crop window -- write what we got instead */
    if (cover && bm.width >= dim && bm.height >= dim)
    {
        aa_crop_center(bm.data, bm.width, bm.height, dim);
        bm.width = dim;
        bm.height = dim;
    }

    return aa_write_aat(out_path, &bm, size_index);
}

/* art_sizes[] indices, largest dim first. Fast-build derives each thumbnail
 * from the next larger one, so generation has to run in this order; the table
 * itself stays free to list its sizes in whatever order reads best. */
static void aa_sizes_largest_first(int *order)
{
    int i, j;

    for (i = 0; i < ART_CACHE_NUM_SIZES; i++)
        order[i] = i;

    for (i = 1; i < ART_CACHE_NUM_SIZES; i++)
    {
        int v = order[i];
        for (j = i; j > 0 && art_sizes[order[j - 1]].dim < art_sizes[v].dim; j--)
            order[j] = order[j - 1];
        order[j] = v;
    }
}

/* Fast build: make this size by downscaling an already-cached larger thumbnail
 * rather than decoding the source again. Reading a 300px .aat and area-averaging
 * it costs a fraction of a full JPEG decode on a 30MHz ARM7, at the price of
 * resampling an image that was already resampled once.
 *
 * Chains from the SMALLEST cached size still larger than this one, so the least
 * is thrown away. Only sound when that image is square: a COVER thumbnail always
 * is, but an over-elongated source falls back to CONTAIN, and stretching a
 * letterboxed image to a square would distort it.
 *
 * Returns false for "no usable larger thumbnail" -- the caller then decodes from
 * the source as normal. */
static bool aa_generate_chained(int size_index, unsigned int dh,
                                const char *out_path,
                                void *workbuf, size_t workbuf_sz)
{
    int dim = art_sizes[size_index].dim;
    int order[ART_CACHE_NUM_SIZES];
    int i;

    if (art_sizes[size_index].fit != AA_FIT_COVER)
        return false;

    aa_sizes_largest_first(order);

    for (i = ART_CACHE_NUM_SIZES - 1; i >= 0; i--)
    {
        int s = order[i];
        struct art_cache_header hdr;
        struct bitmap bm;
        int fd, ret;

        if (art_sizes[s].dim <= dim)
            continue;
        /* Chaining reads with art_cache_load_aat(), which is row-major only, so
         * a size stored by columns cannot be a source. Nothing is lost: the
         * loop simply carries on to the next size up. */
        if (art_sizes[s].layout != AA_ROWS)
            continue;

        aa_cache_path(aa_chain_path, sizeof(aa_chain_path), s, dh);
        fd = open(aa_chain_path, O_RDONLY);
        if (fd < 0)
            continue;

        /* Peek the header for squareness; art_cache_load_aat() seeks back to 0
         * and does its own magic/version validation. */
        if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
            hdr.width != hdr.height)
        {
            close(fd);
            continue;
        }

        memset(&bm, 0, sizeof(bm));
        bm.data = workbuf;
        bm.width = dim;
        bm.height = dim;
        bm.format = FORMAT_NATIVE;

        /* Unfiltered: this is the generator chaining one cached size into a
         * smaller one, and what it writes back to disk has to stay the
         * theme's raw material. Filtering happens on the way out, per read. */
        ret = art_cache_load_aat(fd, &bm, (int)workbuf_sz, false);
        close(fd);

        if (ret > 0)
            return aa_write_aat(out_path, &bm, size_index);
    }

    return false;
}

/* Render one missing thumbnail by the configured strategy. */
static void aa_generate_size(const struct aa_src *src, int size_index,
                             unsigned int dh, const char *out_path,
                             void *workbuf, size_t workbuf_sz)
{
    aa_generation++;
    if (global_settings.art_cache_fast_build &&
        aa_generate_chained(size_index, dh, out_path, workbuf, workbuf_sz))
        return;

    aa_generate_one(src, size_index, out_path, workbuf, workbuf_sz);
}

/* Area-average downscale of a native (fb_data) image. Used only for the
 * compiled-in placeholder, so it needs no upscale or aspect handling: the source
 * is square and always larger than the thumbnail sizes. */
static void aa_scale_native(const fb_data *src, int sw, int sh,
                            fb_data *dst, int dw, int dh)
{
    for (int dy = 0; dy < dh; dy++)
    {
        int sy0 = dy * sh / dh, sy1 = (dy + 1) * sh / dh;
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        for (int dx = 0; dx < dw; dx++)
        {
            int sx0 = dx * sw / dw, sx1 = (dx + 1) * sw / dw;
            unsigned r = 0, g = 0, b = 0, n = 0;
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++)
                {
                    fb_data p = src[sy * sw + sx];
                    r += RGB_UNPACK_RED(p);
                    g += RGB_UNPACK_GREEN(p);
                    b += RGB_UNPACK_BLUE(p);
                    n++;
                }
            dst[dy * dw + dx] = LCD_RGBPACK(r / n, g / n, b / n);
        }
    }
}

/* Downscale one compiled-in (square) placeholder bitmap into every size's
 * placeholder .aat, once. `pathfn` picks the per-size destination file. Cheap in
 * steady state (a file_exists per size); regenerated after a format bump because
 * the purge removes it. */
static void aa_render_placeholder(const fb_data *src, int sw, int sh,
                                  void (*pathfn)(char *, int, int), void *workbuf)
{
    int s;
    char path[MAX_PATH];

    for (s = 0; s < ART_CACHE_NUM_SIZES; s++)
    {
        int dim = art_sizes[s].dim;
        struct bitmap bm;

        pathfn(path, sizeof(path), s);
        if (file_exists(path))
            continue;

        aa_scale_native(src, sw, sh, (fb_data *)workbuf, dim, dim);
        bm.data = workbuf;
        bm.width = dim;
        bm.height = dim;
        bm.format = FORMAT_NATIVE;
        aa_write_aat(path, &bm, s);
        yield();
    }
}

/* Render podboxnoart into both the album and artist placeholder .aat of every
 * size. One shared square source, so COVER is a plain downscale. */
/* One placeholder, used for album and artist rows alike. There were two files
 * and two bitmaps once; they have since become the same image, so the second
 * file only cost a render and a copy of the same pixels. */
static void aa_ensure_fallback(void *workbuf, size_t workbuf_sz)
{
    (void)workbuf_sz;
    aa_render_placeholder((const fb_data *)podboxnoart,
                          BMPWIDTH_podboxnoart, BMPHEIGHT_podboxnoart,
                          aa_fallback_path, workbuf);
}

/* True if the pass should stop right now: a USB connection or shutdown is
 * pending, or a task that outranks this one is waiting to start. Standing
 * down costs nothing: the pass resumes from where the thumbnails on disk
 * leave it. */
static bool aa_check_abort(void)
{
    return bg_task_should_stop(&art_cache_task);
}

/* Whether this pass looks up every folder's image. Off the directory cache
 * that reads the disk for every folder, so only Update and Rebuild do it
 * there; a pass the library starts takes a stamped folder with every size
 * cached as it stands. */
static bool aa_check_all;

/* Whether this pass visits only the tracks added since the last completed
 * one. The library has only grown since then, so a folder that pass covered
 * has lost nothing; one that gained a track is visited with it. */
static bool aa_added_only;

/* Whether this pass reads a track's tags again for a folder whose stamp
 * already says what they hold. Reading a track goes to the disk even with the
 * directory cache up, so only a pass with no marks to have covered does it:
 * Update, Rebuild, a change of album art source, a first pass. */
static bool aa_reread_tags;

/* Find the image file standing for the folder of `probe_path`, into
 * aa_artpath. album/albumartist left NULL: only folder-based art is searched
 * (cover.bmp, folder.jpg, and for a disc folder ../cover.bmp). */
static bool aa_find_file(const char *probe_path, bool artist)
{
    memset(&aa_id3, 0, sizeof(aa_id3));
    strlcpy(aa_id3.path, probe_path, sizeof(aa_id3.path));
    return search_albumart_files(&aa_id3, "", aa_artpath, sizeof(aa_artpath))
           && aa_art_is_folders(probe_path, aa_artpath, artist);
}

/* Find the JPEG embedded in the track at `path`. Only JPEG: it is all the WPS
 * shows from a track's tags too. */
static bool aa_find_embedded(const char *path, struct aa_src *src)
{
    if (!get_metadata_ex(&aa_id3, -1, path, METADATA_EXCLUDE_ID3_PATH)
        || !aa_id3.has_embedded_albumart
        || (aa_id3.albumart.type & AA_CLEAR_FLAGS_MASK) != AA_TYPE_JPG)
        return false;
    src->path = path;
    src->emb_pos = aa_id3.albumart.pos;
    src->emb_size = aa_id3.albumart.size;
    src->emb_flags = aa_id3.albumart.type;
    return true;
}

/* Bring one folder's thumbnails in line with `src`, whose stamp is `stamp`:
 * replace the lot if they were made from something else, and render the sizes
 * that don't exist yet. */
static bool aa_fill(const struct aa_src *src, unsigned int stamp,
                    unsigned int dh, struct aa_stamp *slot,
                    void *workbuf, size_t worksz, bool *aborted)
{
    bool all_exist = aa_thumbs_exist(dh);
    int order[ART_CACHE_NUM_SIZES];
    int i, s;

    /* Stamped before generating, so an interrupted pass leaves a part set
     * that the next one finishes rather than throws away again. Every size
     * goes on a change, because fast build derives the small ones from the
     * large and must not find an old one to derive from. */
    if (slot->stamp == AA_STAMP_NONE)
        slot->stamp = stamp;
    else if (slot->stamp != stamp)
    {
        debug_log(DEBUG_LOG_ARTCACHE, "art changed: %s", src->path);
        aa_remove_thumbs(dh);
        slot->stamp = stamp;
        all_exist = false;
    }
    if (all_exist)
        return true;

    /* Largest first, so a fast build has the big thumbnail on disk to derive
     * the smaller ones from. */
    aa_sizes_largest_first(order);

    for (i = 0; i < ART_CACHE_NUM_SIZES; i++)
    {
        s = order[i];
        aa_cache_path(aa_check_path, sizeof(aa_check_path), s, dh);
        if (file_exists(aa_check_path))
            continue;
        /* Re-check right before a (potentially slow) decode so a USB
         * connection is acknowledged with minimal delay. */
        if (aa_check_abort() || tagcache_is_busy())
        {
            *aborted = true;
            return false;
        }
        aa_cache_path(aa_out_path, sizeof(aa_out_path), s, dh);
        aa_generate_size(src, s, dh, aa_out_path, workbuf, worksz);
        yield();
    }
    /* An image that would not decode leaves the folder bare, and is counted
     * and listed as one with no art. */
    return aa_thumbs_exist(dh);
}

/* Resolve one folder's cover art and bring its thumbnails in line with it.
 * `probe_path` is a track filename under the folder (real for an album folder,
 * synthetic "<dir>/_" for an artist folder); `dh` is that folder's hash (the
 * cache key) and `slot` its table entry. An album takes its art from the
 * sources the album art source setting names, in its order, reading one track's
 * tags for the embedded kind; an `artist` takes it from an image file only, and
 * with no image of its own takes the one above it, cached under its own key so
 * that every reader keying artist art on the album's parent finds it. Sets
 * *aborted if a USB/shutdown/DB-busy stop was hit. Cheap once the folder is
 * cached: an existence check per size, plus the image lookup and stamp under
 * aa_check_all, and no image or track is read. */
static bool aa_cache_dir(const char *probe_path, unsigned int dh,
                         struct aa_stamp *slot, bool artist,
                         void *workbuf, size_t worksz, bool *aborted)
{
    int source = artist ? ART_SOURCE_FILES
                        : global_settings.art_cache_album_source;
    bool embedded = source != ART_SOURCE_FILES;
    struct aa_src src;

    if (!aa_check_all && slot->stamp != AA_STAMP_NONE && aa_thumbs_exist(dh))
        return true;

    /* A folder of loose tracks beside album folders is also an album, and
     * one visit serves both. Its embedded cover yields only where the album
     * art source would yield it, to an image of its own -- never to one from
     * above. */
    if (artist && slot->stamp == AA_STAMP_EMBEDDED && aa_thumbs_exist(dh))
    {
        if (global_settings.art_cache_album_source <= ART_SOURCE_FILES_FIRST
            && aa_find_file(probe_path, false))
            goto from_file;
        return true;
    }

    if (source <= ART_SOURCE_FILES_FIRST && aa_find_file(probe_path, artist))
        goto from_file;

    if (embedded)
    {
        if (!aa_reread_tags && slot->stamp == AA_STAMP_EMBEDDED
            && aa_thumbs_exist(dh))
            return true;

        /* Not read again until an Update: a folder already read and found
         * bare, and with tags first, one that took an image file -- its tags
         * were read then and held nothing. */
        bool bare = !aa_reread_tags
                    && (slot->stamp == AA_STAMP_NO_EMBED
                        || (source == ART_SOURCE_EMBEDDED_FIRST
                            && slot->stamp > AA_STAMP_NO_EMBED));
        if (!bare)
        {
            if (aa_check_abort() || tagcache_is_busy())
            {
                *aborted = true;
                return false;
            }
            if (aa_find_embedded(probe_path, &src))
                return aa_fill(&src, AA_STAMP_EMBEDDED, dh, slot,
                               workbuf, worksz, aborted);
        }
    }

    if (source == ART_SOURCE_EMBEDDED_FIRST && aa_find_file(probe_path, artist))
        goto from_file;

    /* Nothing found. Thumbnails stamped EMBEDDED came from tags all the same:
     * playback fills a folder from whichever track it plays, which need not be
     * the one read here. */
    if (embedded && slot->stamp == AA_STAMP_EMBEDDED && aa_thumbs_exist(dh))
        return true;
    /* What these thumbnails were made from is gone, or no longer a source.
     * Unstamped thumbnails never came from anything here to lose. */
    if (slot->stamp == AA_STAMP_EMBEDDED || slot->stamp > AA_STAMP_NO_EMBED)
        aa_remove_thumbs(dh);
    slot->stamp = embedded ? AA_STAMP_NO_EMBED : AA_STAMP_NONE;
    return false;

from_file:
    src.path = aa_artpath;
    src.emb_pos = -1;
    src.emb_size = 0;
    src.emb_flags = 0;
    return aa_fill(&src, aa_art_stamp(aa_artpath), dh, slot,
                   workbuf, worksz, aborted);
}

/* One full generation pass: walk every track filename, dedup by directory,
 * resolve folder art, and render any missing thumbnails -- both the track's own
 * (album) folder and its parent (artist) folder, for libraries laid out as
 * <artist>/<album>/<track>. Returns BG_INTERRUPTED if it was aborted (USB/DB
 * busy/no memory) and BG_FAILED if the database could not be read to the end. */
/* The two miss lists a pass is writing. One open file each for the whole pass
 * rather than an open/append/close per folder, which on a library with a lot
 * of coverless folders would be thousands of them. */
static struct path_list_writer noart_albums;
static struct path_list_writer noart_artists;

/* Both or neither: one pass fills both lists, so publishing only the one that
 * opened would leave the two describing different passes. A pass whose lists
 * could not be opened still runs -- the thumbnails are the point, these are
 * diagnostics -- it simply publishes nothing, leaving the previous pair. */
static void noart_open(void)
{
    if (path_list_write_open(&noart_albums, AA_NOART_ALBUMS)
        && path_list_write_open(&noart_artists, AA_NOART_ARTISTS))
        return;

    path_list_write_close(&noart_albums, false);
    path_list_write_close(&noart_artists, false);
}

/* Publish on a complete pass, discard on an aborted one. */
static void noart_close(bool completed)
{
    path_list_write_close(&noart_albums, completed);
    path_list_write_close(&noart_artists, completed);
}

const char *art_cache_noart_list(bool artists)
{
    return artists ? AA_NOART_ARTISTS : AA_NOART_ALBUMS;
}

/* The scratch one decode needs. An AA_FIT_COVER decode stages a non-square
 * image before cropping it square, so the buffer is sized for the widest one
 * aa_cover_dim() will pass: the largest size at COVER_MAX_ASPECT. That fixes
 * both of the terms BM_SCALED_SIZE adds up -- the pixels, and the scaler's
 * line buffers, which depend on the width alone -- and those are the two
 * bounds aa_cover_dim() tests a stage against. See art_sizes.h. */
static size_t aa_work_bytes(void)
{
    return BM_SCALED_SIZE(ART_CACHE_MAX_DIM * ART_CACHE_COVER_MAX_ASPECT,
                          ART_CACHE_MAX_DIM, FORMAT_NATIVE, 0)
           + JPEG_DECODE_OVERHEAD;
}

static enum bg_result aa_run_pass(void)
{
    struct tagcache_search tcs;
    size_t worksz;
    int wh, sh;
    void *workbuf;
    bool aborted = false;
    bool failed = false;
    bool completed;
    int since_yield = 0;

    worksz = aa_work_bytes();
    /* No marks to have covered: a trigger, a first pass or a format bump */
    aa_check_all = dircache_is_ready()
                || art_cache_task.done_marks.entries < 0;
    aa_reread_tags = art_cache_task.done_marks.entries < 0;
    wh = core_alloc(worksz);
    if (wh <= 0)
        return BG_INTERRUPTED; /* not enough free memory right now */

    sh = core_alloc(AA_TABLE_BYTES);
    if (sh <= 0)
    {
        core_free(wh);
        return BG_INTERRUPTED;
    }

    workbuf = core_get_data_pinned(wh);
    aa_stamps = core_get_data_pinned(sh);
    aa_visited = (unsigned char *)(aa_stamps + AA_SEEN_SLOTS);
    memset(aa_stamps, 0, AA_TABLE_BYTES);

    aa_ensure_dirs();
    /* A purge cut short leaves no stamp file, which saving one would hide */
    if (!aa_check_format_version())
    {
        aa_stamps = NULL;
        aa_visited = NULL;
        core_unpin(wh);
        core_unpin(sh);
        core_free(sh);
        core_free(wh);
        return BG_INTERRUPTED;
    }
    aa_table_full = false;
    aa_ensure_fallback(workbuf, worksz);
    aa_stamps_load();
    /* A Rebuild renumbers the entries, which the file at the old last entry
     * shows */
    {
        const struct bg_marks *done = &art_cache_task.done_marks;
        struct tagcache_marks now;

        tagcache_get_marks(&now);
        aa_added_only = done->entries >= 0 && done->deleted >= 0
                     && now.deleted_ct == done->deleted
                     && now.commitid >= done->commitid
                     && tagcache_get_stat()->total_entries > done->entries
                     && aa_marks_key != 0
                     && aa_fold_key(tagcache_entry_key(done->entries - 1))
                        == aa_marks_key;
    }

    if (!tagcache_search(&tcs, tag_filename))
    {
        aborted = true;
        goto out;
    }
    /* New tracks are appended to the index. A filename search walks the
     * index only with the database in RAM; from the disk it takes everything,
     * and the pass is a whole one. */
    if (aa_added_only && tcs.ramsearch)
        tagcache_search_set_range(&tcs, art_cache_task.done_marks.entries,
                                  tagcache_get_stat()->total_entries - 1);
    else
        aa_added_only = false;
    debug_log(DEBUG_LOG_ARTCACHE, aa_added_only ? "visiting added tracks"
                                                : "visiting every folder");

    /* Counting starts over: these describe this pass, not every pass since
     * boot. Zeroed here rather than at the top so an early return -- no
     * memory, no search -- leaves the previous pass's figures readable. */
    memset(&aa_counts, 0, sizeof(aa_counts));
    noart_open();

    /* A pass is running -> lights the status-bar %lc ("Caching") token. */
    cache_busy = true;
    /* Speed up the (CPU-bound) image decoding, like Cover Flow's own
     * generator does. Passes are rare (only after the database settles or
     * changes), so the extra clock is a brief one-off. */
    cpu_boost(true);

    while (tagcache_get_next(&tcs, aa_tcs_buf, sizeof(aa_tcs_buf)))
    {
        unsigned int dh, ah;
        struct aa_stamp *slot;

        /* Abort promptly on USB/shutdown (the thread loop then acknowledges)
         * or yield the disk to an incoming database commit, so a long pass
         * never blocks either. */
        if (aa_check_abort() || tagcache_is_busy())
        {
            aborted = true;
            break;
        }

        /* The track's own folder (the album), keyed by its path hash. Skip the
         * per-folder work entirely once this folder has been visited this pass
         * (aa_visit records it), so later tracks of the same album are
         * cheap. */
        aa_dirname(tcs.result, aa_dir, sizeof(aa_dir));
        dh = aa_hash(aa_dir);
        if ((slot = aa_visit(dh)))
        {
            aa_counts.albums++;
            if (aa_cache_dir(tcs.result, dh, slot, false, workbuf, worksz,
                             &aborted))
                aa_counts.album_art++;
            else if (!aborted)
                path_list_write_record(&noart_albums, aa_dir);
        }
        if (aborted)
            break;

        /* The parent folder (the artist), cached from <artist>/folder.jpg etc.
         * for <artist>/<album>/<track> layouts, and from the folder above it
         * for <artist>/<album_type>/<album>/<track>. Deduped independently of
         * the album so an artist whose first album has no cover still gets
         * resolved. Skipped when there is no distinct parent (flat/rooted layouts). */
        aa_dirname(aa_dir, aa_artist_dir, sizeof(aa_artist_dir));
        if (aa_artist_dir[0] && strcmp(aa_artist_dir, aa_dir) != 0)
        {
            ah = aa_hash(aa_artist_dir);
            if ((slot = aa_visit(ah)))
            {
                snprintf(aa_probe, sizeof(aa_probe), "%s/_", aa_artist_dir);
                aa_counts.artists++;
                if (aa_cache_dir(aa_probe, ah, slot, true, workbuf, worksz,
                                 &aborted))
                    aa_counts.artist_art++;
                else if (!aborted)
                    path_list_write_record(&noart_artists, aa_artist_dir);
            }
        }
        if (aborted)
            break;

        if (++since_yield >= 16)
        {
            since_yield = 0;
            yield();
        }
    }
    /* The walk ends the same way at the last entry and at an unreadable one,
     * and only the search knows which it was. A database a rebuild took away
     * mid-walk is an interruption: the rebuilt one can carry the same marks. */
    if (tcs.failed && (!tagcache_is_usable() || tagcache_is_busy()))
        aborted = true;
    failed = !aborted && tcs.failed;
    tagcache_search_finish(&tcs);
    cpu_boost(false); /* balances the boost above (skipped on the goto-out path) */

out:
    completed = !aborted && !failed;
    /* What the walk did not visit is not missing when it saw only the
     * added tracks */
    noart_close(completed && !aa_added_only);
    if (completed && !aa_added_only && !aa_table_full)
        aa_remove_orphans();
    aa_stamps_save(completed && !aa_added_only);
    aa_stamps = NULL;
    aa_visited = NULL;
    core_unpin(wh);
    core_unpin(sh);
    core_free(sh);
    core_free(wh);
    cache_busy = false;
    if (failed)
        return BG_FAILED;
    return aborted ? BG_INTERRUPTED : BG_DONE;
}

/* Cache the offered track's embedded art into any of its folder's thumbnails
 * that don't exist yet. Fill-only: never overwrites art already cached (folder
 * images are the preferred on-disk source) -- this just gives a coverless folder
 * the art playback already had parsed for the WPS, at no decode cost to it.
 * Stamped EMBEDDED, so a pass that later finds folder art replaces it. */
static void aa_handle_offer(void)
{
    char path[MAX_PATH];
    char dir[MAX_PATH];
    unsigned int dh;
    size_t worksz;
    int s, wh;
    void *workbuf;
    struct aa_src src;
    bool need = false;

    aa_offer_posted = false;
    if (aa_offer.path[0] == '\0' || !tagcache_is_usable())
        return;

    /* Copy the path out before any yield so a newer offer can't move it. */
    strlcpy(path, aa_offer.path, sizeof(path));
    src.path = path;
    src.emb_pos = aa_offer.pos;
    src.emb_size = aa_offer.size;
    src.emb_flags = aa_offer.flags;

    aa_dirname(path, dir, sizeof(dir));
    dh = aa_hash(dir);

    for (s = 0; s < ART_CACHE_NUM_SIZES; s++)
    {
        aa_cache_path(aa_check_path, sizeof(aa_check_path), s, dh);
        if (!file_exists(aa_check_path))
        {
            need = true;
            break;
        }
    }
    if (!need)
        return;   /* already cached (folder art wins) */
    /* A pass removes the thumbnails of a folder outside the database, and a
     * stamp file written now would record a format the purge has not reached */
    if (!aa_format_current())
        return;
    {
        struct tagcache_search tcs;

        if (!tagcache_find_index(&tcs, path))
            return;
        tagcache_search_finish(&tcs);
    }

    /* Only from free memory: taking it from the audio buffer stops and
     * rebuffers the track that is playing, which costs more than the offer
     * saves. */
    worksz = aa_work_bytes();
    if (core_allocatable() < worksz)
        return;
    wh = core_alloc(worksz);
    if (wh <= 0)
        return;
    workbuf = core_get_data_pinned(wh);

    aa_ensure_dirs();
    int order[ART_CACHE_NUM_SIZES];
    int i;

    aa_sizes_largest_first(order);      /* see aa_cache_dir() */

    for (i = 0; i < ART_CACHE_NUM_SIZES; i++)
    {
        if (aa_check_abort())
            break;
        s = order[i];
        aa_cache_path(aa_check_path, sizeof(aa_check_path), s, dh);
        if (file_exists(aa_check_path))
            continue;
        aa_cache_path(aa_out_path, sizeof(aa_out_path), s, dh);
        aa_generate_size(&src, s, dh, aa_out_path, workbuf, worksz);
        yield();
    }

    /* So the next pass replaces these if the folder gains an image. */
    aa_stamp_append(dh, AA_STAMP_EMBEDDED);

    core_unpin(wh);
    core_free(wh);
}

/* Playback thread: a track became current. If it carries embedded JPEG art, hand
 * its location to the aa thread (reusing the metadata playback already parsed) --
 * no decoding here. */
static void aa_track_change_cb(unsigned short id, void *event_data)
{
    (void)id; (void)event_data;
    struct mp3entry *id3 = audio_current_track();
    if (global_settings.art_cache_album_source == ART_SOURCE_FILES)
        return;
    if (!id3 || !id3->has_embedded_albumart ||
        (id3->albumart.type & AA_CLEAR_FLAGS_MASK) != AA_TYPE_JPG)
        return;

    strlcpy(aa_offer.path, id3->path, sizeof(aa_offer.path));
    aa_offer.pos = id3->albumart.pos;
    aa_offer.size = id3->albumart.size;
    aa_offer.flags = id3->albumart.type;
    if (aa_offer_posted)
        return;
    aa_offer_posted = true;
    bg_task_post(&art_cache_task, AA_EVENT_OFFER);
}

/* A pass trusts each folder's stamp for what its tags hold, so a change of
 * source needs the pass that reads them again. */
/* The source the last change was acted on for, or -1 before the first. */
static int aa_applied_source = -1;

void art_cache_album_source_callback(int source)
{
    aa_applied_source = source;
    bg_task_update(&art_cache_task);
}

void art_cache_settings_applied(void)
{
    int source = global_settings.art_cache_album_source;

    if (aa_applied_source >= 0 && aa_applied_source != source)
        art_cache_album_source_callback(source);
    aa_applied_source = source;
}

/* bg_task.run: one pass, with the tracing the pass itself does not do. */
static enum bg_result aa_task_run(void)
{
    enum bg_result result;

    debug_log(DEBUG_LOG_ARTCACHE, "starting pass");
    aa_offer_posted = false;
    result = aa_run_pass();
    debug_log(DEBUG_LOG_ARTCACHE,
              result == BG_DONE   ? "pass complete, idle" :
              result == BG_FAILED ? "pass failed: database unreadable" :
                                    "pass interrupted");
    return result;
}

/* bg_task.handle_event: everything on the queue that is ours rather than the
 * tick's -- which is just the offer posted by the track-change hook. */
static void aa_task_event(const struct queue_event *ev)
{
    if (ev->id == AA_EVENT_OFFER)
        aa_handle_offer();
}

/* The marker is read back at init rather than started at -1 -- starting at -1
 * meant the first settled count after a boot never matched, so a full pass ran
 * on every startup, walking the whole database with cache_busy set, which is
 * what held the "Building" indicator up with nothing actually to do. */
/* bg_task.read_marks and write_marks: the stamp file's header */
static void aa_read_marks(struct bg_marks *m)
{
    struct libfile_header h;

    if (libfile_peek(AA_STAMP_FILE, AA_STAMP_MAGIC, ART_CACHE_FORMAT_VERSION,
                     &h))
    {
        m->entries = h.marks.entries;
        m->commitid = h.marks.commitid;
        m->deleted = h.marks.deleted;
    }
    else
        m->entries = m->commitid = m->deleted = -1;
}

static void aa_write_marks(const struct bg_marks *m)
{
    struct libfile_marks lm;

    libfile_no_marks(&lm);
    if (m)
    {
        lm.entries = m->entries;
        lm.commitid = m->commitid;
        lm.deleted = m->deleted;
    }
    if (libfile_set_marks(AA_STAMP_FILE, AA_STAMP_MAGIC,
                          ART_CACHE_FORMAT_VERSION, &lm) && m)
    {
        struct aa_stamp rec =
            { 0, aa_fold_key(tagcache_entry_key(m->entries - 1)) };

        libfile_append(AA_STAMP_FILE, AA_STAMP_MAGIC, ART_CACHE_FORMAT_VERSION,
                       sizeof(rec), &rec, 1);
    }
}

struct bg_task art_cache_task =
{
    .read_marks   = aa_read_marks,
    .write_marks  = aa_write_marks,
    .rank         = BG_RANK_ART,
    .run          = aa_task_run,
    .purge        = aa_purge_thumbs,
    .artifact_ok  = aa_artifact_ok,
    .handle_event = aa_task_event,
};

void art_cache_init(void)
{
    cache_busy = false;
    mutex_init(&aat_mutex);

    /* Start the log from a readable point once per boot, so it says what this
     * run did rather than every run since the setting went on -- and so the
     * file exists from boot however the setting was set. Settings are loaded
     * before init_tagcache() reaches here, so the gate reads the real value. */
    debug_log_restart(DEBUG_LOG_ARTCACHE);

    /* A format bump leaves every cached thumbnail unreadable without moving
     * the database's marks. The stamp file carries the format as its version,
     * so aa_read_marks() finds none then and the task is stale; the purge
     * itself stays inside the pass. */
    /* A pass holds the decode scratch and the folder table at once, so the peak
     * declared to bg_task is both. */
    art_cache_task.work_bytes = aa_work_bytes() + AA_TABLE_BYTES;

    bg_task_init(&art_cache_task);

    /* Opportunistically cache the embedded art of tracks as they play, for
     * folders that have no on-disk cover (fill-only -- see aa_handle_offer). */
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, aa_track_change_cb);
}

