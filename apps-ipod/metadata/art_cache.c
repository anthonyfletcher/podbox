/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Disk cache for cover art -- album art, artist art and ebook covers. Pre-scales each
 * image to the sizes skins ask for and stores it, so browsing does not
 * re-decode on every track. Album art comes from the album folder's image or a
 * track's embedded art, as the album art source setting orders them. A
 * person's picture -- an artist's or an author's -- is keyed by their name
 * (art_name.h), not by a folder: from /Artwork/People, else a folder above
 * their works named for them, else the folder above a work unless that is a
 * library folder holding several people's. Each has its own placeholder for
 * when nothing is found. A book's cover, keyed by the book's own path, is an
 * image named after the book or the one inside it, and keeps its shape rather
 * than being cropped square: a cover is taller than wide.
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
#include "art_name.h"
#include "art_sizes.h"
#include "settings/settings.h"  /* global_settings.art_cache_* */
#include "system/debug_log.h"
#include "database/tagcache.h"
#include "database/db_spoken.h"   /* which albums are books */
#include "system/bg_task.h"         /* the caching pass runs as one */
#include "files/book_index.h"       /* the books the pass visits */
#include "files/zip_member.h"       /* a deflated cover, inflated */
#include "files/path_list.h"        /* the "found nothing" lists */
#include "lcd.h"
#include "draw/bmp.h"
#include "draw/img_filter.h"
#include "bitmaps/podboxnoart.h" /* shared "no art" placeholder for aa_ensure_fallback */
#include "draw/image_load.h"
#include "events.h"
#include "system/appevents.h"
#include "audio.h"
#include "storage.h"
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

/* Folder table: open-addressed by key, holding album folders, books and
 * people alike. Sized generously; a key that finds it full is skipped for
 * that pass. */
#define AA_SEEN_SLOTS 16384  /* power of two */

/* One folder's entry in the table: its path hash and the stamp of the image
 * its thumbnails were made from. The table is the pass's seen set as well, with
 * a bit per slot saying whether this pass has reached the folder yet. */
struct aa_stamp
{
    unsigned int key;    /* folder path hash; 0 marks an empty slot */
    unsigned int stamp;
};
/* People, by name key (see "people" below), and the tables their pictures
 * and the books' covers are found with. A person who finds the table full
 * has no picture that pass. */
#define AA_PEOPLE_SLOTS  8192   /* power of two */
#define AA_LIBDIR_SLOTS  1024   /* power of two */
#define AA_BOOKDIR_SLOTS 2048   /* power of two */
#define AA_CENTRAL_MAX   2048   /* files in /Artwork/People and Covers */
#define AA_CENTRAL_NAMES (48 * 1024)

struct aa_person
{
    unsigned int key;           /* art_name_hash(); 0 marks an empty slot */
    unsigned int alt;           /* art_name_alt_hash(), or 0 */
    unsigned int match;         /* the highest folder named for them, or 0 */
    unsigned short level;       /* how far above a track's folder it is */
    unsigned short flags;
};

/* A folder and the one thing in it, or 0 for who once there are two: the
 * people whose named folders it holds, which makes it a library folder, or
 * the books it holds, whose cover its image then is not */
struct aa_libdir
{
    unsigned int dir;           /* folder path hash; 0 marks an empty slot */
    unsigned int who;
};

struct aa_central
{
    unsigned int key, alt;      /* of the file name without its extension */
    unsigned short name;        /* offset of the file name in the names */
    bool covers;                /* in /Artwork/Covers, not People */
};

#define AA_PEOPLE_BYTES (AA_PEOPLE_SLOTS * sizeof(struct aa_person) \
                         + AA_LIBDIR_SLOTS * sizeof(struct aa_libdir) \
                         + AA_BOOKDIR_SLOTS * sizeof(struct aa_libdir) \
                         + AA_CENTRAL_MAX * sizeof(struct aa_central) \
                         + AA_CENTRAL_NAMES)
#define AA_TABLE_BYTES (AA_SEEN_SLOTS * sizeof(struct aa_stamp) \
                        + AA_SEEN_SLOTS / 8 + AA_PEOPLE_BYTES)

/* Stamps that are not an image's. NONE is a folder with no record, whose
 * thumbnails a pass adopts as they stand rather than regenerating -- the case
 * for every folder cached before stamps existed, and after the stamp file is
 * lost. EMBEDDED is art from a track's tags. NO_EMBED is a folder whose track
 * was read and held no art, so a pass does not read it again. 2 is NO_EMBED
 * as recorded when only JPEG was looked for: a pass takes it for neither, and
 * reads the folder again for PNG. */
#define AA_STAMP_NONE     0u
#define AA_STAMP_EMBEDDED 1u
#define AA_STAMP_NO_EMBED 3u

/* The stamp file's first record says how its keys are made: key AA_SCHEME_KEY,
 * stamp AA_SCHEME, people by name. A file without it keys people by folder,
 * so its marks are read as none and the next pass is a whole one, which moves
 * their thumbnails to their names. A folder hashing to AA_SCHEME_KEY is
 * tabled as the one below it, as one hashing to 0 is as 1. */
#define AA_SCHEME_KEY 0xFFFFFFFFu
#define AA_SCHEME     1u

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
static struct aa_person *aa_people;
static struct aa_libdir *aa_libdirs;
static struct aa_libdir *aa_bookdirs;
static struct aa_central *aa_central;
static char *aa_central_names;

static volatile bool cache_busy;

/* Counts every change to a thumbnail: see art_cache_generation(). */
static volatile unsigned int aa_generation;

/* What the current (or last completed) pass has covered, and the folder it is
 * on. Every one of these is a count of a branch the pass already takes, so
 * keeping them costs nothing beyond the increment; aa_dir is the scratch
 * buffer the pass already keeps the current directory in. */
static struct art_cache_counts aa_counts;

/* Scratch id3 a track's tags are read into for its embedded art; kept out of
 * the thread stack because struct mp3entry is large. */
static struct mp3entry aa_id3;

/* Scratch buffers used only by the background thread (aa_run_pass /
 * aa_generate_one). Kept off the thread stack -- together with the JPEG
 * decoder's own deep frames they otherwise overflow it. Single-threaded and
 * non-reentrant, so module-level statics are safe. */
static char aa_tcs_buf[TAGCACHE_BUFSZ];
static char aa_artpath[MAX_PATH];
static char aa_dir[MAX_PATH];
static char aa_probe[MAX_PATH];
static char aa_name[TAGCACHE_BUFSZ];
static char aa_title[TAGCACHE_BUFSZ];
static char aa_check_path[MAX_PATH];
static char aa_out_path[MAX_PATH];
static char aa_chain_path[MAX_PATH];
static char aa_stat_dir[MAX_PATH];
static char aa_tmp_path[MAX_PATH];

/* Stamp-file records go through this a batch at a time. */
#define AA_STAMP_BATCH 64
static struct aa_stamp aa_stamp_io[AA_STAMP_BATCH];

/* Names collected by one round of aa_purge_thumbs(). A thumbnail is
 * "%08x.aat" and the shared placeholder "fallback.aat", so 32 bytes holds
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

/* Modified FNV hash (same as PictureFlow's, good avalanche/distribution),
 * of the first 'len' bytes of 'str' */
static unsigned int aa_hash_n(const char *str, size_t len)
{
    const unsigned int p = 16777619;
    unsigned int hash = 0x811C9DC5;

    while (len-- && *str)
        hash = (hash ^ (unsigned char)*str++) * p;
    hash += hash << 13;
    hash ^= hash >> 7;
    hash += hash << 3;
    hash ^= hash >> 17;
    hash += hash << 5;
    return hash;
}

static unsigned int aa_hash(const char *str)
{
    return str ? aa_hash_n(str, strlen(str)) : 0;
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

unsigned int art_cache_track_key(const char *path)
{
    struct tagcache_album al;
    const char *sep;
    int idx;

    if (!path || !(sep = strrchr(path, '/')))
        return 0;
    idx = tagcache_find_path(path);
    if (idx >= 0 && tagcache_album_get(tagcache_album_of(idx), &al)
        && al.art_hash)
        return al.art_hash;
    return aa_hash_n(path, sep - path);
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
    struct aa_stamp scheme = { AA_SCHEME_KEY, AA_SCHEME };

    if (libfile_begin(&w, AA_STAMP_FILE, AA_STAMP_MAGIC,
                      ART_CACHE_FORMAT_VERSION, sizeof(struct aa_stamp), NULL))
        libfile_finish(&w, libfile_write(&w, &scheme, sizeof(scheme), 1));
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

/* The table slot for folder hash 'h', claimed with no stamp if it is new.
 * NULL when the table is full. h==0 is remapped so 0 can mark an empty slot. */
static struct aa_stamp *aa_slot(unsigned int h)
{
    unsigned int i, idx;
    if (h == 0)
        h = 1;
    else if (h == AA_SCHEME_KEY)
        h--;
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
    else if (h == AA_SCHEME_KEY)
        h--;
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
    uint32_t left;

    fd = libfile_open(AA_STAMP_FILE, AA_STAMP_MAGIC, ART_CACHE_FORMAT_VERSION,
                      sizeof(struct aa_stamp), &h, NULL);
    aa_marks_key = 0;
    if (fd < 0)
        return;
    /* By the count: a torn append can leave a record past it */
    left = h.count;
    while (left > 0 && (n = read(fd, aa_stamp_io, sizeof(aa_stamp_io))) > 0)
    {
        n /= sizeof(aa_stamp_io[0]);
        if ((uint32_t)n > left)
            n = left;
        left -= n;
        for (i = 0; i < n; i++)
        {
            struct aa_stamp *s;

            if (aa_stamp_io[i].key == 0)
            {
                aa_marks_key = aa_stamp_io[i].stamp;
                continue;
            }
            if (aa_stamp_io[i].key == AA_SCHEME_KEY)
                continue;
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
    aa_stamp_io[n].key = AA_SCHEME_KEY;
    aa_stamp_io[n++].stamp = AA_SCHEME;
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

    if (h == AA_SCHEME_KEY)
        rec.key--;

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
        stamp += AA_STAMP_NO_EMBED + 1;
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

/* Where an album-art image comes from: a file on disk (folder art), or an
 * image embedded in an audio file (emb_pos >= 0, reusing metadata already
 * parsed by playback -- see aa_track_change_cb()). */
struct aa_src
{
    const char   *path;      /* file to read: the folder image, or the track */
    off_t         emb_pos;   /* embedded image offset, or -1 for a whole file */
    unsigned long emb_size;  /* embedded image length (embedded only) */
    int           emb_flags; /* embedded mp3_albumart type and flags */
};

/* Rendering a book's cover: fitted at its own shape, never cropped */
static bool aa_book;

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

    struct image_src is = {
        .fd = open(src->path, O_RDONLY),
        .pos = src->emb_pos >= 0 ? src->emb_pos : 0,
        .len = src->emb_pos >= 0 ? (off_t)src->emb_size : -1,
        .flags = src->emb_pos >= 0 ? src->emb_flags & ~AA_CLEAR_FLAGS_MASK : 0,
    };
    int rc;

    if (is.fd < 0)
        return -1;
    rc = image_load(&is, bm, (int)workbuf_sz, fmt, NULL);
    close(is.fd);
    return rc;
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
    bool cover = art_sizes[size_index].fit == AA_FIT_COVER && !aa_book;
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

    if (art_sizes[size_index].fit != AA_FIT_COVER || aa_book)
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
        ret = art_cache_load_aat(fd, &bm, (int)workbuf_sz, NULL);
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

/* Render podboxnoart into the placeholder .aat of every size, one file that
 * album and artist rows share. A square source, so COVER is a plain
 * downscale. */
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
 * aa_artpath: art_folder_file()'s */
static bool aa_find_file(const char *probe_path)
{
    return art_folder_file(probe_path, aa_artpath, sizeof(aa_artpath));
}

/* What a work's cover may come from beyond its embedded art */
struct aa_work
{
    const char *artist, *title; /* for /Artwork/Covers, or NULL */
    bool named;                 /* an image named after the file, first */
    bool folder;                /* its folder's images */
};

static bool aa_covers_central(const char *artist, const char *title);
static bool aa_book_file(const char *path);

/* Find the JPEG or PNG embedded in the track at `path` */
static bool aa_find_embedded(const char *path, struct aa_src *src)
{
    if (!get_metadata_ex(&aa_id3, -1, path, METADATA_EXCLUDE_ID3_PATH)
        || !aa_id3.has_embedded_albumart
        || !image_embedded_usable(aa_id3.albumart.type))
        return false;
    src->path = path;
    src->emb_pos = aa_id3.albumart.pos;
    src->emb_size = aa_id3.albumart.size;
    src->emb_flags = aa_id3.albumart.type;
    return true;
}

/* Why the last folder aa_cache_dir() gave no art has none, for the log; and
 * the image it tried, or NULL */
static const char *aa_miss;
static const char *aa_miss_src;

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
    if (aa_thumbs_exist(dh))
        return true;
    aa_miss = "would not decode";
    aa_miss_src = src->path;
    return false;
}

/* Resolve one work's cover art and bring its thumbnails in line with it.
 * `probe_path` is its first track; `dh` is the cache key and `slot` its table
 * entry. A file in /Artwork/Covers named for it comes first, then, as 'w'
 * allows, an image named after the track; then its folder's image and the
 * track's embedded art, in the order the album art source setting names. Sets
 * *aborted if a USB/shutdown/DB-busy stop was hit. Cheap once the work is
 * cached: an existence check per size, plus the image lookup and stamp under
 * aa_check_all, and no image or track is read. */
static bool aa_cache_dir(const char *probe_path, unsigned int dh,
                         struct aa_stamp *slot, const struct aa_work *w,
                         void *workbuf, size_t worksz, bool *aborted)
{
    int source = global_settings.art_cache_album_source;
    bool embedded = source != ART_SOURCE_FILES;
    struct aa_src src;

    if (!aa_check_all && slot->stamp != AA_STAMP_NONE && aa_thumbs_exist(dh))
        return true;

    if (aa_covers_central(w->artist, w->title)
        || (w->named && aa_book_file(probe_path)))
        goto from_file;
    if (source <= ART_SOURCE_FILES_FIRST && w->folder
        && aa_find_file(probe_path))
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

    if (source == ART_SOURCE_EMBEDDED_FIRST && w->folder
        && aa_find_file(probe_path))
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
    aa_miss = !embedded ? "no image file"
              : aa_reread_tags ? "no image file or embedded image"
              : "no image file; tags not read again until an Update";
    aa_miss_src = NULL;
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

static void aa_log_miss(const char *kind, const char *dir)
{
    if (aa_miss_src)
        debug_log(DEBUG_LOG_ARTCACHE, "no art: %s %s: %s %s", kind, dir,
                  aa_miss_src, aa_miss);
    else
        debug_log(DEBUG_LOG_ARTCACHE, "no art: %s %s: %s", kind, dir,
                  aa_miss);
}

/* One full generation pass: walk every track filename, dedup by directory,
 * resolve folder art, and render any missing thumbnails; then the books, then
 * the people. Returns BG_INTERRUPTED if it was aborted (USB/DB busy/no memory)
 * and BG_FAILED if the database could not be read to the end. */
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
           + IMAGE_DECODE_OVERHEAD;
}

/* ---- people ----
 *
 * A person's picture is keyed by their name, and found by the first of:
 * a file named for them in /Artwork/People; the highest folder up to three
 * above one of their tracks (its own counted) named for them; the folder
 * above one of their works, or the one above that, unless it is the volume
 * root or a library folder. A library folder holds the folders named for
 * two or more people, as Music/ does; Prince/ holding folders named for
 * Prince and for Prince & The Revolution is still Prince's. Only cover.*
 * and folder.* in the folder itself count.
 *
 * So a pass takes three walks for people: one notes every track's named
 * folders and library folders, which the album walk does when it sees the
 * whole library, one settles pictures, and one, only when needed, gives the
 * people whose named folder had no image their fallback -- which cannot be
 * tried before every named folder has been.
 *
 * The first walk also notes which folders hold more than one book of a kind,
 * whose images are then no book's cover, so a book's own cover is settled on
 * the second. */

#define AA_PEOPLE_DIR "/Artwork/People"

/* Ebooks' folders in the book-folder table, apart from audiobooks': a
 * folder holding one of each holds one book of each kind */
#define AA_EBOOK_DIRS 0x9E3779B9u

enum aa_people_stage
{
    AA_PEOPLE_COLLECT,
    AA_PEOPLE_RESOLVE,
    AA_PEOPLE_FALLBACK,
};

#define AA_PERSON_SEEN  1       /* checked against the cache and /Artwork */
#define AA_PERSON_TRIED 2       /* their named folder was looked in */
#define AA_PERSON_DONE  4       /* settled this pass */
#define AA_PERSON_ART   8       /* and has a picture */

static int aa_central_count;
static size_t aa_central_used;
/* People whose named folder had no image, for the fallback walk */
static int aa_people_waiting;
/* Moving an old key's thumbnails rather than copying them: on a whole
 * pass, once the walk has visited every key a work has */
static bool aa_people_move;
/* The previous item's folder and person, which a fallback has tried */
static unsigned int aa_people_last_dir, aa_people_last_key;
/* The album row of the track a walk is on, with its artist's name keys and,
 * in aa_name and aa_title, its artist and title. Read once per row; row -2
 * before any. */
static struct
{
    int row;
    bool ok;
    struct tagcache_album al;
    unsigned int key, alt;
} aa_row;
static struct book_index_text aa_book_text;

struct aa_people_ctx
{
    int stage;
    void *workbuf;
    size_t worksz;
    bool *aborted;
};

/* Whether either key of one name equals either key of the other */
static bool aa_keys_match(unsigned int k1, unsigned int a1,
                          unsigned int k2, unsigned int a2)
{
    return (k1 && (k1 == k2 || k1 == a2)) || (a1 && (a1 == k2 || a1 == a2));
}

static struct aa_person *aa_person_get(unsigned int key, bool claim)
{
    unsigned int i;

    if (!key)
        return NULL;
    for (i = 0; i < AA_PEOPLE_SLOTS; i++)
    {
        struct aa_person *p = &aa_people[(key + i) & (AA_PEOPLE_SLOTS - 1)];

        if (p->key == key)
            return p;
        if (p->key == 0)
        {
            if (!claim)
                return NULL;
            p->key = key;
            return p;
        }
    }
    return NULL;
}

/* The entry of 'dir' in a folder table of 'slots'; with 'who', claimed for
 * them if new */
static struct aa_libdir *aa_dir_entry(struct aa_libdir *t, unsigned int slots,
                                      unsigned int dir, unsigned int who)
{
    unsigned int i;

    if (!dir)
        return NULL;
    for (i = 0; i < slots; i++)
    {
        struct aa_libdir *l = &t[(dir + i) & (slots - 1)];

        if (l->dir == dir)
            return l;
        if (l->dir == 0)
        {
            if (!who)
                return NULL;
            l->dir = dir;
            l->who = who;
            return l;
        }
    }
    return NULL;
}

/* 'dir' holds 'who', in a folder table: two of them make 'who' 0 */
static void aa_dir_note(struct aa_libdir *t, unsigned int slots,
                        unsigned int dir, unsigned int who)
{
    struct aa_libdir *l = aa_dir_entry(t, slots, dir, who);

    if (l && l->who != who)
        l->who = 0;
}

static bool aa_dir_shared(struct aa_libdir *t, unsigned int slots,
                          unsigned int dir)
{
    struct aa_libdir *l = aa_dir_entry(t, slots, dir, 0);

    return l && l->who == 0;
}

static bool aa_is_library(unsigned int dir)
{
    return aa_dir_shared(aa_libdirs, AA_LIBDIR_SLOTS, dir);
}

/* Whether the folder of the book at 'path' stands for the book alone: it
 * holds no other book of its kind ('salt' 0 for an audiobook, AA_EBOOK_DIRS
 * for an ebook), and is not named for the book's person 'name', whose
 * picture its image is */
static bool aa_book_folder(const char *path, unsigned int salt,
                           const char *name)
{
    const char *end = strrchr(path, '/'), *start;

    if (!end || aa_dir_shared(aa_bookdirs, AA_BOOKDIR_SLOTS,
                              aa_hash_n(path, end - path) ^ salt))
        return false;
    for (start = end; start > path && start[-1] != '/'; start--)
        ;
    return !aa_keys_match(art_name_hash_n(start, end - start),
                          art_name_alt_hash_n(start, end - start),
                          art_name_hash(name), art_name_alt_hash(name));
}

static void aa_book_noted(const char *path, unsigned int salt,
                          unsigned int who)
{
    const char *end = strrchr(path, '/');

    if (end)
        aa_dir_note(aa_bookdirs, AA_BOOKDIR_SLOTS,
                    aa_hash_n(path, end - path) ^ salt, who);
}

/* The length of the volume part of 'path', "/<HDD0>", or 0 */
static size_t aa_root_len(const char *path)
{
    const char *e;

    if (path[0] != '/' || path[1] != '<' || !(e = strchr(path, '>'))
        || (e[1] && e[1] != '/'))
        return 0;
    return e + 1 - path;
}

/* Where the folder 'level' above the track or book at 'path' ends -- 0 its
 * own -- or NULL when that is the volume root or above it */
static const char *aa_level_end(const char *path, int level)
{
    const char *root = path + aa_root_len(path);
    const char *end = strrchr(path, '/');

    while (end && end > root && level-- > 0)
    {
        do
            end--;
        while (end > root && *end != '/');
    }
    return end && end > root ? end : NULL;
}

/* What one track or book says about its person: the folders above it named
 * for them, and which folders hold such folders */
static void aa_people_note(const char *path, unsigned int key, unsigned int alt)
{
    struct aa_person *p = aa_person_get(key, true);
    const char *root, *end, *start;
    int level;

    if (!p)
        return;
    p->alt = alt;
    root = path + aa_root_len(path);
    end = strrchr(path, '/');
    for (level = 0; level < 4 && end && end > root; level++)
    {
        for (start = end; start > root && start[-1] != '/'; start--)
            ;
        if (start < end
            && aa_keys_match(art_name_hash_n(start, end - start),
                             art_name_alt_hash_n(start, end - start),
                             key, alt))
        {
            if (!p->match || level > p->level)
            {
                p->match = aa_hash_n(path, end - path);
                p->level = level;
            }
            if (start - 1 > root)
                aa_dir_note(aa_libdirs, AA_LIBDIR_SLOTS,
                            aa_hash_n(path, start - 1 - path), key);
        }
        end = start - 1;
    }
}

/* The album row of the track idx_id into aa_row */
static void aa_track_row(int idx_id)
{
    int row = tagcache_album_of(idx_id);

    if (row == aa_row.row)
        return;
    aa_row.row = row;
    aa_row.key = aa_row.alt = 0;
    aa_name[0] = aa_title[0] = '\0';
    aa_row.ok = tagcache_album_get(row, &aa_row.al);
    if (!aa_row.ok)
        return;
    if (tagcache_seek_string(tag_albumartist, aa_row.al.artist_seek,
                             aa_name, sizeof(aa_name)))
    {
        aa_row.key = art_name_hash(aa_name);
        aa_row.alt = art_name_alt_hash(aa_name);
    }
    if (!tagcache_seek_string(tag_album, aa_row.al.album_seek, aa_title,
                              sizeof(aa_title)))
        aa_title[0] = '\0';
}

/* Whether the track idx_id is the first of a book, the track that gives the
 * book its key */
static bool aa_track_is_book(int idx_id)
{
    return aa_row.ok && aa_row.al.first == idx_id
           && db_spoken_album_kind(&aa_row.al) == 1;
}

/* What one track says, for the people step and the books' covers */
static void aa_track_note(const char *path, int idx_id)
{
    aa_track_row(idx_id);
    if (aa_row.key)
        aa_people_note(path, aa_row.key, aa_row.alt);
    if (aa_row.ok && db_spoken_album_kind(&aa_row.al) == 1)
        aa_book_noted(path, 0, aa_row.row + 1);
}

/* The files of /Artwork/People or, with 'covers', /Artwork/Covers, by name
 * key, after those already loaded */
static void aa_central_load(bool covers)
{
    const char *dir = covers ? ART_COVERS_DIR : AA_PEOPLE_DIR;
    DIR *d;
    struct dirent *e;

    if (!(d = opendir(dir)))
        return;
    while ((e = readdir(d)))
    {
        const char *dot = strrchr(e->d_name, '.');
        size_t len = strlen(e->d_name) + 1;
        struct aa_central *c = &aa_central[aa_central_count];

        if (e->d_name[0] == '.' || !art_image_ext(dot)
            || (dir_get_info(d, e).attribute & ATTR_DIRECTORY))
            continue;
        if (aa_central_count == AA_CENTRAL_MAX
            || aa_central_used + len > AA_CENTRAL_NAMES)
        {
            debug_log(DEBUG_LOG_ARTCACHE, "%s: too many files", dir);
            break;
        }
        c->key = art_name_hash_n(e->d_name, dot - e->d_name);
        c->alt = art_name_alt_hash_n(e->d_name, dot - e->d_name);
        if (!c->key)
            continue;
        c->covers = covers;
        c->name = aa_central_used;
        memcpy(aa_central_names + aa_central_used, e->d_name, len);
        aa_central_used += len;
        aa_central_count++;
    }
    closedir(d);
}

/* The first file of /Artwork/People named for 'p', into aa_artpath */
static bool aa_central_find(const struct aa_person *p)
{
    int i;

    for (i = 0; i < aa_central_count; i++)
    {
        const struct aa_central *c = &aa_central[i];

        if (c->covers || !aa_keys_match(c->key, c->alt, p->key, p->alt))
            continue;
        snprintf(aa_artpath, sizeof(aa_artpath), AA_PEOPLE_DIR "/%s",
                 aa_central_names + c->name);
        if (art_file_usable(aa_artpath))
            return true;
    }
    return false;
}

/* The first file of /Artwork/Covers named for 'title' by 'artist', else
 * for 'title' alone, into aa_artpath */
static bool aa_covers_central(const char *artist, const char *title)
{
    unsigned int kw = art_name_work_hash(artist, title);
    unsigned int kt = art_name_hash(title);
    int i, pass;

    if (!kt)
        return false;
    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < aa_central_count; i++)
        {
            const struct aa_central *c = &aa_central[i];

            if (!c->covers || c->key != (pass ? kt : kw))
                continue;
            snprintf(aa_artpath, sizeof(aa_artpath), ART_COVERS_DIR "/%s",
                     aa_central_names + c->name);
            if (art_file_usable(aa_artpath))
                return true;
        }
    }
    return false;
}

/* An image in the folder [path, end) itself, into aa_artpath */
static bool aa_folder_image(const char *path, const char *end)
{
    size_t n = end - path;

    if (n + 3 > sizeof(aa_probe))
        return false;
    memcpy(aa_probe, path, n);
    strcpy(aa_probe + n, "/_");
    return aa_find_file(aa_probe);
}

static bool aa_copy_file(const char *from, const char *to, void *buf,
                         size_t size)
{
    int in = open(from, O_RDONLY), out;
    ssize_t n;
    bool ok = true;

    if (in < 0)
        return false;
    out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0)
    {
        close(in);
        return false;
    }
    while ((n = read(in, buf, size)) > 0)
    {
        if (write(out, buf, n) != n)
        {
            ok = false;
            break;
        }
    }
    close(in);
    close(out);
    if (n < 0 || !ok)
    {
        remove(to);
        return false;
    }
    return true;
}

/* Every size of key 'from''s thumbnails as key 'to''s: moved, or copied
 * where 'from' is still a work's */
static bool aa_adopt(unsigned int from, unsigned int to, bool move,
                     void *buf, size_t size)
{
    int s;

    aa_generation++;
    for (s = 0; s < ART_CACHE_NUM_SIZES; s++)
    {
        aa_cache_path(aa_check_path, sizeof(aa_check_path), s, from);
        aa_cache_path(aa_out_path, sizeof(aa_out_path), s, to);
        remove(aa_out_path);
        if (move ? rename(aa_check_path, aa_out_path) != 0
                 : !aa_copy_file(aa_check_path, aa_out_path, buf, size))
            return false;
    }
    return true;
}

static void aa_person_settled(struct aa_person *p, bool art)
{
    p->flags |= AA_PERSON_DONE | (art ? AA_PERSON_ART : 0);
    aa_visit(p->key);
}

/* Settle 'p' from the image in aa_artpath, found for the item at 'path'.
 * Before the people were keyed by name, their thumbnails were keyed by the
 * folder 'old' levels above the item, up to the one above its own; those
 * made from this same image are taken over rather than made again. */
static void aa_person_fill(struct aa_person *p, struct aa_stamp *slot,
                           const char *path, int old,
                           const struct aa_people_ctx *c)
{
    unsigned int stamp = aa_art_stamp(aa_artpath);
    struct aa_src src = { .path = aa_artpath, .emb_pos = -1 };

    for (; old <= 1 && slot->stamp == AA_STAMP_NONE
           && !aa_thumbs_exist(p->key); old++)
    {
        const char *end = aa_level_end(path, old);
        unsigned int oh;
        struct aa_stamp *os;

        if (!end)
            break;
        oh = aa_hash_n(path, end - path);
        os = aa_slot(oh);
        if (os && os->stamp == stamp && aa_thumbs_exist(oh)
            && aa_adopt(oh, p->key, aa_people_move && !aa_was_visited(oh),
                        c->workbuf, c->worksz))
        {
            debug_log(DEBUG_LOG_ARTCACHE, "person kept: %s", aa_artpath);
            slot->stamp = stamp;
        }
    }
    aa_person_settled(p, aa_fill(&src, stamp, p->key, slot, c->workbuf,
                                 c->worksz, c->aborted));
}

/* One item on a walk that settles pictures. 'up' is where its fallback
 * starts: 1, the folder above a track's, or 0, a book's own. */
static void aa_people_settle(const char *path, unsigned int key, int up,
                             const struct aa_people_ctx *c)
{
    struct aa_person *p = aa_person_get(key, false);
    struct aa_stamp *slot;
    const char *end;
    unsigned int dh;
    int lv;

    if (!p || (p->flags & AA_PERSON_DONE))
        return;
    if (aa_was_visited(key) || !(slot = aa_slot(key)))
    {
        p->flags |= AA_PERSON_DONE;       /* a work's key, or no room */
        return;
    }
    if (!(p->flags & AA_PERSON_SEEN))
    {
        p->flags |= AA_PERSON_SEEN;
        if (!aa_check_all && slot->stamp != AA_STAMP_NONE
            && aa_thumbs_exist(key))
        {
            aa_person_settled(p, true);
            return;
        }
        if (aa_central_find(p))
        {
            aa_person_fill(p, slot, path, up, c);
            return;
        }
    }

    if (p->match && !(p->flags & AA_PERSON_TRIED))
    {
        end = aa_level_end(path, p->level);
        if (!end || aa_hash_n(path, end - path) != p->match)
            return;                     /* for an item under it */
        p->flags |= AA_PERSON_TRIED;
        if (aa_folder_image(path, end))
        {
            aa_person_fill(p, slot, path, up, c);
            return;
        }
        aa_people_waiting++;
    }
    if (p->match && c->stage != AA_PEOPLE_FALLBACK)
        return;

    if (!(end = strrchr(path, '/')))
        return;
    dh = aa_hash_n(path, end - path);
    if (dh == aa_people_last_dir && key == aa_people_last_key)
        return;
    aa_people_last_dir = dh;
    aa_people_last_key = key;
    for (lv = up; lv < up + 2 && (end = aa_level_end(path, lv)); lv++)
    {
        if (aa_is_library(aa_hash_n(path, end - path)))
            continue;
        if (aa_folder_image(path, end))
        {
            aa_person_fill(p, slot, path, up, c);
            return;
        }
    }
}

/* A book's first track: the book's cover, keyed by that track's path. Its
 * folder's images stand for it only as aa_book_folder() says. */
static void aa_book_track_cover(const char *path, const struct aa_people_ctx *c)
{
    unsigned int bh = aa_hash(path);
    struct aa_stamp *slot = aa_visit(bh);
    struct aa_work w = { aa_name, aa_title, aa_row.al.tracks == 1,
                         aa_book_folder(path, 0, aa_name) };

    if (slot && !aa_cache_dir(path, bh, slot, &w, c->workbuf, c->worksz,
                              c->aborted) && !*c->aborted)
        aa_log_miss("book", path);
}

/* One walk of the database's tracks for people. False if stopped. */
static bool aa_people_walk(const struct aa_people_ctx *c)
{
    struct tagcache_search tcs;
    int since_yield = 0;

    if (!tagcache_search(&tcs, tag_filename))
    {
        *c->aborted = true;
        return false;
    }
    aa_row.row = -2;
    aa_people_last_dir = aa_people_last_key = 0;
    while (tagcache_get_next(&tcs, aa_tcs_buf, sizeof(aa_tcs_buf)))
    {
        if (aa_check_abort() || tagcache_is_busy())
        {
            *c->aborted = true;
            break;
        }
        if (c->stage == AA_PEOPLE_COLLECT)
            aa_track_note(tcs.result, tcs.idx_id);
        else
        {
            aa_track_row(tcs.idx_id);
            if (c->stage == AA_PEOPLE_RESOLVE && aa_track_is_book(tcs.idx_id))
                aa_book_track_cover(tcs.result, c);
            if (aa_row.key && !*c->aborted)
                aa_people_settle(tcs.result, aa_row.key, 1, c);
        }
        if (*c->aborted)
            break;
        if (++since_yield >= 16)
        {
            since_yield = 0;
            yield();
        }
    }
    if (tcs.failed)
        *c->aborted = true;
    tagcache_search_finish(&tcs);
    return !*c->aborted;
}

/* One book's author, on a walk that settles pictures */
static bool aa_book_person(const struct book_entry *e, void *data)
{
    const struct aa_people_ctx *c = data;
    unsigned int key = art_name_hash(e->author);

    if (aa_check_abort() || tagcache_is_busy())
    {
        *c->aborted = true;
        return false;
    }
    if (key)
        aa_people_settle(e->path, key, 0, c);
    return !*c->aborted;
}

/* The database's artists with no picture, into the health list */
static void aa_people_report(void)
{
    struct tagcache_artist ar;
    int i, n = tagcache_artist_count();

    for (i = 0; i < n; i++)
    {
        const struct aa_person *p;

        if (!tagcache_artist_get(i, &ar) || !ar.art_hash)
            continue;
        aa_counts.artists++;
        p = aa_person_get(ar.art_hash, false);
        if (p && (p->flags & AA_PERSON_ART))
            aa_counts.artist_art++;
        else if (tagcache_seek_string(tag_albumartist, ar.seek, aa_name,
                                      sizeof(aa_name)))
        {
            path_list_write_record(&noart_artists, aa_name);
            debug_log(DEBUG_LOG_ARTCACHE, "no art: artist %s", aa_name);
        }
    }
}

/* Every person's picture, once the album walk and the books are done */
static bool aa_people_pass(void *workbuf, size_t worksz, bool *aborted)
{
    struct aa_people_ctx c = { AA_PEOPLE_COLLECT, workbuf, worksz, aborted };

    aa_people_move = !aa_added_only;
    if (aa_added_only && !aa_people_walk(&c))
        return false;

    aa_people_waiting = 0;
    c.stage = AA_PEOPLE_RESOLVE;
    if (!aa_people_walk(&c))
        return false;
    aa_people_last_dir = aa_people_last_key = 0;
    book_index_each(aa_book_person, &c, &aa_book_text);
    if (*aborted)
        return false;

    if (aa_people_waiting)
    {
        c.stage = AA_PEOPLE_FALLBACK;
        if (!aa_people_walk(&c))
            return false;
        aa_people_last_dir = aa_people_last_key = 0;
        book_index_each(aa_book_person, &c, &aa_book_text);
        if (*aborted)
            return false;
    }
    aa_people_report();
    return true;
}

/* ---- books ---- */

#define AA_COVER_TMP LIB_CACHE_DIR "/cover.tmp"

/* An image named after the book, `<book>.jpg` and so on, that its decoder
 * takes, into aa_artpath: how a cover is given to a book without one, or
 * replaced, which an image in the folder or inside the book does not. */
static bool aa_book_file(const char *path)
{
    static const char * const exts[] = { ".jpg", ".jpeg", ".png", ".bmp",
                                         ".gif", ".webp" };
    const char *dot = strrchr(path, '.');
    size_t stem = dot ? (size_t)(dot - path) : strlen(path);
    unsigned i;

    for (i = 0; i < ARRAYLEN(exts); i++)
    {
        if (stem + strlen(exts[i]) >= sizeof(aa_artpath))
            return false;
        memcpy(aa_artpath, path, stem);
        strcpy(aa_artpath + stem, exts[i]);
        if (art_file_usable(aa_artpath))
            return true;
    }
    return false;
}

struct aa_books_ctx
{
    void *workbuf;
    size_t worksz;
    bool *aborted;
};

/* One book of the index noted: its author for the people step, and its
 * folder for the books' covers */
static bool aa_book_note(const struct book_entry *e, void *data)
{
    (void)data;
    aa_people_note(e->path, art_name_hash(e->author),
                   art_name_alt_hash(e->author));
    aa_book_noted(e->path, AA_EBOOK_DIRS, aa_hash(e->path));
    return true;
}

/* One book of the index: a visit, as a folder has one. A file in
 * /Artwork/Covers named for it comes first, then an image named after it;
 * then its folder's image, when aa_book_folder() lets it stand for the
 * book, and the cover inside it, in the order the album art source setting
 * names. */
static bool aa_book_visit(const struct book_entry *e, void *data)
{
    struct aa_books_ctx *c = data;
    unsigned int bh = aa_hash(e->path);
    struct aa_stamp *slot;
    struct aa_src src = { .emb_pos = -1 };
    unsigned int stamp;
    bool tmp = false, folder, inside;
    int source;

    if (aa_check_abort() || tagcache_is_busy())
    {
        *c->aborted = true;
        return false;
    }
    if (!(slot = aa_visit(bh)))
        return true;
    if (!aa_check_all && slot->stamp != AA_STAMP_NONE && aa_thumbs_exist(bh))
        return true;

    source = global_settings.art_cache_album_source;
    folder = aa_book_folder(e->path, AA_EBOOK_DIRS, e->author);
    inside = source != ART_SOURCE_FILES && e->cover_at >= 0;
    if (aa_covers_central(e->author, e->title) || aa_book_file(e->path)
        || ((source <= ART_SOURCE_FILES_FIRST || !inside) && folder
            && source != ART_SOURCE_EMBEDDED && aa_find_file(e->path)))
    {
        src.path = aa_artpath;
        stamp = aa_art_stamp(aa_artpath);
    }
    else if (inside && e->cover_method == 0)
    {
        src.path = e->path;
        src.emb_pos = e->cover_at;
        src.emb_size = e->cover_csize;
        stamp = aa_art_stamp(e->path);
    }
    else if (inside)
    {
        stamp = aa_art_stamp(e->path);
        if (slot->stamp != stamp || !aa_thumbs_exist(bh))
        {
            /* Before the decode needs the work buffer */
            if (!zip_member_to_file(e->path, e->cover_at, e->cover_csize,
                                    e->cover_usize, e->cover_method,
                                    AA_COVER_TMP, c->workbuf, c->worksz))
                return true;
            tmp = true;
        }
        src.path = AA_COVER_TMP;
    }
    else
    {
        if (slot->stamp > AA_STAMP_NO_EMBED)
            aa_remove_thumbs(bh);
        slot->stamp = AA_STAMP_NONE;
        return true;
    }

    aa_book = true;
    aa_fill(&src, stamp, bh, slot, c->workbuf, c->worksz, c->aborted);
    aa_book = false;
    if (tmp)
        remove(AA_COVER_TMP);
    return !*c->aborted;
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
    aa_people = (struct aa_person *)(aa_visited + AA_SEEN_SLOTS / 8);
    aa_libdirs = (struct aa_libdir *)(aa_people + AA_PEOPLE_SLOTS);
    aa_bookdirs = aa_libdirs + AA_LIBDIR_SLOTS;
    aa_central = (struct aa_central *)(aa_bookdirs + AA_BOOKDIR_SLOTS);
    aa_central_names = (char *)(aa_central + AA_CENTRAL_MAX);
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

    aa_central_count = 0;
    aa_central_used = 0;
    aa_central_load(false);
    aa_central_load(true);
    aa_row.row = -2;
    while (tagcache_get_next(&tcs, aa_tcs_buf, sizeof(aa_tcs_buf)))
    {
        unsigned int dh;
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
        aa_track_row(tcs.idx_id);
        if ((slot = aa_visit(dh)))
        {
            struct aa_work w = { aa_name, aa_title, false, true };

            aa_counts.albums++;
            if (aa_cache_dir(tcs.result, dh, slot, &w, workbuf, worksz,
                             &aborted))
                aa_counts.album_art++;
            else if (!aborted)
            {
                path_list_write_record(&noart_albums, aa_dir);
                aa_log_miss("album", aa_dir);
            }
        }
        if (aborted)
            break;

        /* What the track says about its artist and its book, for the
         * people step: a walk of only the added tracks does not see enough,
         * and the people step takes its own */
        if (!aa_added_only)
            aa_track_note(tcs.result, tcs.idx_id);

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

    /* Every book in the index, which the orphan sweep below then spares */
    if (!aborted)
    {
        struct aa_books_ctx books = { workbuf, worksz, &aborted };

        book_index_each(aa_book_note, &books, &aa_book_text);
        book_index_each(aa_book_visit, &books, &aa_book_text);
    }
    if (!aborted && !failed)
        aa_people_pass(workbuf, worksz, &aborted);
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

static struct timeout aa_offer_tmo;

/* Whether the disk is asleep. Hosted storage reports itself never active, and
 * has no spin-up to save, so it is never asleep: waiting on it would hold the
 * offer forever. */
static bool aa_disk_asleep(void)
{
#ifdef HAVE_HOSTFS
    return false;
#else
    return !storage_disk_is_active();
#endif
}

/* Tick: an offer held for a sleeping disk goes on the queue once something
 * else has spun it up. Making the thumbnails reads the art out of the track,
 * so handling it sooner would wake the disk for nothing but this. */
static int aa_offer_poll(struct timeout *tmo)
{
    (void)tmo;
    if (aa_disk_asleep())
        return HZ;
    if (!aa_offer_posted)
    {
        aa_offer_posted = true;
        bg_task_post(&art_cache_task, AA_EVENT_OFFER);
    }
    return 0;
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

    aa_offer_posted = false;
    if (aa_offer.path[0] == '\0' || !tagcache_is_usable())
        return;
    if (aa_disk_asleep())
    {
        timeout_register(&aa_offer_tmo, aa_offer_poll, HZ, 0);
        return;
    }

    /* Copy the path out before any yield so a newer offer can't move it. */
    strlcpy(path, aa_offer.path, sizeof(path));
    src.path = path;
    src.emb_pos = aa_offer.pos;
    src.emb_size = aa_offer.size;
    src.emb_flags = aa_offer.flags;

    aa_dirname(path, dir, sizeof(dir));
    dh = aa_hash(dir);

    if (aa_thumbs_exist(dh))
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

    /* So the next pass replaces these if the folder gains an image. Unstamped
     * where the loop stopped short, so the pass makes what is missing. */
    if (aa_thumbs_exist(dh))
        aa_stamp_append(dh, AA_STAMP_EMBEDDED);

    core_unpin(wh);
    core_free(wh);
}

/* Playback thread: a track became current. If it carries embedded art, hand
 * its location to the aa thread (reusing the metadata playback already parsed) --
 * no decoding here. */
static void aa_track_change_cb(unsigned short id, void *event_data)
{
    (void)id; (void)event_data;
    struct mp3entry *id3 = audio_current_track();
    if (global_settings.art_cache_album_source == ART_SOURCE_FILES)
        return;
    if (!id3 || !id3->has_embedded_albumart ||
        !image_embedded_usable(id3->albumart.type))
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
 * source needs the pass that reads them again. aa_applied_source is the
 * source the last change was acted on for, or -1 before the first. */
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

/* Whether the stamp file's first record is this scheme's: see AA_SCHEME */
static bool aa_scheme_current(void)
{
    struct aa_stamp first;
    int fd = open(AA_STAMP_FILE, O_RDONLY);
    bool ok;

    if (fd < 0)
        return false;
    ok = lseek(fd, sizeof(struct libfile_header), SEEK_SET)
            == (off_t)sizeof(struct libfile_header)
         && read(fd, &first, sizeof(first)) == (ssize_t)sizeof(first)
         && first.key == AA_SCHEME_KEY && first.stamp == AA_SCHEME;
    close(fd);
    return ok;
}

/* bg_task.read_marks and write_marks: the stamp file's header. The marks are
 * read back at init, never started at -1: -1 matches no settled count, so
 * every boot would run a full pass with cache_busy set, holding the
 * "Building" indicator up with nothing to do. A file of the old scheme gives
 * none, once, for the pass that moves it to this one. */
static void aa_read_marks(struct bg_marks *m)
{
    struct libfile_header h;

    if (libfile_peek(AA_STAMP_FILE, AA_STAMP_MAGIC, ART_CACHE_FORMAT_VERSION,
                     &h) && aa_scheme_current())
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

#ifdef DBTOOL
/* tools/artcache_pb: one pass, run as the background task runs it, against
 * the database tagcache_tool_open() loaded. The marks it records on success
 * are the ones the player computes for the same database, so the player
 * finds the cache covered and runs no pass of its own. 'rebuild' throws
 * every thumbnail away first, as Rebuild Art Cache does. */
enum bg_result art_cache_tool_run(bool rebuild)
{
    struct tagcache_marks tm;
    struct bg_marks covered;
    enum bg_result result;

    if (rebuild)
    {
        aa_purge_thumbs();
        aa_write_marks(NULL);
    }
    aa_read_marks(&art_cache_task.done_marks);

    tagcache_get_marks(&tm);
    covered.entries = tagcache_get_stat()->total_entries;
    covered.commitid = tm.commitid;
    covered.deleted = tm.deleted_ct;

    result = aa_task_run();
    if (result == BG_DONE)
    {
        art_cache_task.done_marks = covered;
        aa_write_marks(&covered);
    }
    return result;
}
#endif /* DBTOOL */

struct bg_task art_cache_task =
{
    .name         = "art cache",
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

    /* A header per boot, so each run's lines start from a readable point --
     * and so the file exists from boot however the setting was set. Settings
     * are loaded before init_tagcache() reaches here, so the gate reads the
     * real value. */
    if (debug_log_enabled(DEBUG_LOG_ARTCACHE))
        debug_log_header(DEBUG_LOG_ARTCACHE, "started");

    /* A pass holds the decode scratch and the folder table at once, so the
     * peak declared to bg_task is both. A format bump needs nothing here: the
     * stamp file carries the format as its version, so aa_read_marks() finds
     * no marks, the task is stale, and the pass purges. */
    art_cache_task.work_bytes = aa_work_bytes() + AA_TABLE_BYTES;

    bg_task_init(&art_cache_task);

    /* Opportunistically cache the embedded art of tracks as they play, for
     * folders that have no on-disk cover (fill-only -- see aa_handle_offer). */
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, aa_track_change_cb);
}

