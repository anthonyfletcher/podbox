/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Disk cache for cover art -- album art, artist art and ebook covers. Pre-scales each
 * image to the sizes skins ask for and stores it, so browsing does not
 * re-decode on every track. Album art comes from the album folder's image or a
 * track's embedded art, as the album art source setting orders them, and is
 * keyed by the album's names, so albums sharing a folder -- as an iTunes copy
 * of a library's do -- each keep their own; without the database in RAM it
 * is keyed by the folder. A person's picture -- an artist's or an author's --
 * is keyed by their name (art_name.h), not by a folder: from /Artwork/People,
 * else a folder above their works named for them, else the folder above a
 * work unless that is a library folder holding several people's. Each has
 * its own placeholder for when nothing is found. A book's cover, keyed by
 * the book's own path, is an image named after the book or the one inside
 * it, and keeps its shape rather than being cropped square: a cover is
 * taller than wide.
 *
 * A pass after a library update that only added tracks visits the folders of
 * those tracks alone. One after a deletion, an Update, a Rebuild or a change
 * of format visits every folder, and only that kind removes the thumbnails of
 * folders that have gone and rewrites the lists of folders with no art.
 ****************************************************************************/

#include <stdio.h>
#include <stdarg.h>
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
#include "draw/jpeg_load.h"         /* a check reports a read made again */
#include "database/path_key.h"      /* a book's path, with or without volume */
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
#define AA_BOOKDIR_SLOTS 8192   /* power of two: a Calibre library has a
                                 * folder per book */
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
 * reads the folder again for PNG. CLAIMED, the same value, is a folder whose
 * thumbnails the move to album keys has dealt with (see aa_album_adopt()). */
#define AA_STAMP_NONE     0u
#define AA_STAMP_EMBEDDED 1u
#define AA_STAMP_CLAIMED  2u
#define AA_STAMP_NO_EMBED 3u

/* The stamp file's first record says how its keys are made: key AA_SCHEME_KEY,
 * stamp AA_SCHEME. Scheme 1 keys people by name and albums by their first
 * track's folder, scheme 2 albums by name as well (tagcache_album.art_hash),
 * and a file without the record keys people by folder too. An older scheme's
 * marks are read as none, so the next pass is a whole one, which moves the
 * thumbnails to their new keys; a pass cut short leaves the scheme it found.
 * A key equal to AA_SCHEME_KEY is tabled as the one below it, as 0 is as 1. */
#define AA_SCHEME_KEY 0xFFFFFFFFu
#define AA_SCHEME     2u

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
/* A folder table: library folders, or books' folders. A table that ran out
 * of room may be missing any folder, so one not in it counts as shared. */
struct aa_dirs
{
    struct aa_libdir *t;
    unsigned int slots;
    bool full;
};
static struct aa_dirs aa_libdirs = { NULL, AA_LIBDIR_SLOTS, false };
static struct aa_dirs aa_bookdirs = { NULL, AA_BOOKDIR_SLOTS, false };
static struct aa_central *aa_central;
static char *aa_central_names;

static volatile bool cache_busy;
/* Publication errors belong to the worker's current pass/check/offer. */
static bool aa_io_failed;

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
    unsigned      serial, retries;
} aa_offer;

/* An offer is on the queue. A pass does not drain the queue, so each track
 * change posting its own would overflow it; one queued offer reads whichever
 * aa_offer holds when it is handled. The thread drops what is queued across a
 * USB session, so a pass starting and the check after USB clear it too. */
static volatile bool aa_offer_posted;

/* Queue event id: Check a File's Art, for the file in aa_chk.path; unique
 * on the thread, for bg_task_queued() (iap-artwork.c has 2) */
#define AA_EVENT_CHECK 3

enum { AA_CHECK_IDLE, AA_CHECK_POSTED, AA_CHECK_RUNNING, AA_CHECK_DONE };

/* One check: the file asked about and the report it writes, posted from the
 * UI thread, run on the aa thread, and read back once 'state' is DONE. */
static struct
{
    char path[MAX_PATH];
    char *report;
    size_t size, len;
    volatile int state;
    volatile int walked, total;     /* tracks, for the screen's meter */
} aa_chk;

/* A check settles only the person and the book whose art it makes again, and
 * writes down each step. Outside one a walk settles everyone, silently. */
static bool aa_checking;
static unsigned int aa_only_person, aa_only_book;
/* Folders above a person's works a check's fallback found no image in */
static int aa_chk_missed;
static bool aa_chk_quiet;           /* those are counted, not each written */
/* The file as the walk names it, the first track of its book, and the first
 * of its folder, which a pass settles the folder's cover from */
static char aa_chk_track[MAX_PATH], aa_chk_first[MAX_PATH];
static char aa_chk_probe[MAX_PATH];
/* That first track's album artist and album, and the file's own person */
static char aa_chk_name[TAGCACHE_BUFSZ], aa_chk_title[TAGCACHE_BUFSZ];
static char aa_chk_person[TAGCACHE_BUFSZ];

/* A line of the check's report; nothing outside a check */
static void aa_note(const char *fmt, ...) ATTRIBUTE_PRINTF(1, 2);
static void aa_note(const char *fmt, ...)
{
    va_list ap;
    size_t room;
    int n;

    if (!aa_checking || aa_chk_quiet || aa_chk.len + 2 > aa_chk.size)
        return;
    room = aa_chk.size - aa_chk.len - 1;
    va_start(ap, fmt);
    n = vsnprintf(aa_chk.report + aa_chk.len, room, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    aa_chk.len += MIN((size_t)n, room - 1);
    aa_chk.report[aa_chk.len++] = '\n';
    aa_chk.report[aa_chk.len] = '\0';
}

/* The name at the end of 'path', for a report line whose folder is known */
static const char *aa_leaf(const char *path)
{
    const char *s = strrchr(path, '/');

    return s ? s + 1 : path;
}

/* A check's thumbnail sizes share one line: each adds to it, and aa_fill()
 * writes it once they are all made */
static char aa_chk_sizes[128];

static void aa_note_size(int size_index, const char *what)
{
    size_t n = strlen(aa_chk_sizes);

    if (aa_checking)
        snprintf(aa_chk_sizes + n, sizeof(aa_chk_sizes) - n, "%s%dpx: %s",
                 n ? "   " : "", art_sizes[size_index].dim, what);
}

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

/* The table slot for 'h' if it has one, without claiming or visiting it */
static struct aa_stamp *aa_find(unsigned int h)
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
            return NULL;
        if (s->key == h)
            return s;
    }
    return NULL;
}

/* Thumbnails this pass could not reach: a folder or person skipped for want
 * of a slot, or people of the old scheme without the database in RAM. They
 * are not orphans, so the pass then removes none and keeps every stamp. */
static bool aa_unreached;

/* The scheme of the stamp file this pass began from (see AA_SCHEME). Below
 * AA_SCHEME its pass is a whole one, which reads no tags again. Below 1 it
 * keeps every thumbnail when it cannot reach the people; below 2 it moves
 * albums to their names. */
static unsigned int aa_scheme_began;
static bool aa_old_scheme;          /* people keyed by folder */
static bool aa_album_move;          /* albums keyed by folder */
static unsigned int aa_scheme_read(void);

/* Keeps the thumbnail of 'h' if it is tabled, without tabling it: the
 * slot, or NULL */
static struct aa_stamp *aa_spare(unsigned int h)
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
            return NULL;
        if (aa_stamps[at].key == h)
        {
            aa_visited[at / 8] |= 1u << (at % 8);
            return &aa_stamps[at];
        }
    }
    return NULL;
}

/* The slot for 'h' the first time this pass reaches it; NULL if the pass has
 * been here already, or the table is full (the folder is then skipped). */
static struct aa_stamp *aa_visit(unsigned int h)
{
    struct aa_stamp *s = aa_slot(h);
    unsigned int i;

    if (!s)
    {
        aa_unreached = true;
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
 * visit every folder. 'scheme' is the one the file is left in. */
static void aa_stamps_save(bool completed, unsigned int scheme)
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
    {
        aa_io_failed = true;
        return;
    }
    ok = true;
    aa_stamp_io[n].key = AA_SCHEME_KEY;
    aa_stamp_io[n++].stamp = scheme;
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
    if (!libfile_finish(&w, ok))
        aa_io_failed = true;
}

/* Record one folder's stamp outside a pass, by appending to the file. */
static void aa_stamp_append(unsigned int h, unsigned int stamp)
{
    struct aa_stamp rec = { h ? h : 1, stamp };

    if (h == AA_SCHEME_KEY)
        rec.key--;

    if (!libfile_append(AA_STAMP_FILE, AA_STAMP_MAGIC, ART_CACHE_FORMAT_VERSION,
                        sizeof(rec), &rec, 1))
        aa_io_failed = true;
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
    {
        aa_io_failed = true;
        return false;
    }

    hdr.magic = ART_CACHE_MAGIC;
    hdr.version = ART_CACHE_FORMAT_VERSION;
    hdr.width = bm->width;
    hdr.height = bm->height;
    hdr.layout = layout;
    hdr.pad = 0;
    bytes = (size_t)bm->width * bm->height * FB_DATA_SZ;

    ok = (write(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)) &&
         (write(fd, bm->data, bytes) == (ssize_t)bytes);
    if (close(fd) < 0)
        ok = false;

    if (ok)
        ok = rename(aa_tmp_path, out_path) == 0;
    if (!ok)
    {
        aa_io_failed = true;
        remove(aa_tmp_path);
    }
    else
        aa_generation++;
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
/* For a check: what became of one size, decoded from its source, and whether
 * a JPEG read failed on the way, counted since 'retries' and 'refails' */
static void aa_note_decode(int size_index, const char *what, int retries,
                           int refails)
{
    char buf[48];

    snprintf(buf, sizeof(buf), "%s%s", what,
             jpeg_read_retries == retries ? ""
             : jpeg_read_retry_failures != refails ? " (read failed twice)"
             : " (read retried: ok)");
    aa_note_size(size_index, buf);
}

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
    int retries = jpeg_read_retries, refails = jpeg_read_retry_failures;
    bool ok;

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
    {
        aa_note_decode(size_index, "failed", retries, refails);
        return false;
    }

    /* the decode should have landed on exactly tw x th, but never crop from an
     * image smaller than the crop window -- write what we got instead */
    if (cover && bm.width >= dim && bm.height >= dim)
    {
        aa_crop_center(bm.data, bm.width, bm.height, dim);
        bm.width = dim;
        bm.height = dim;
    }

    ok = aa_write_aat(out_path, &bm, size_index);
    aa_note_decode(size_index, ok ? "ok" : "not saved", retries, refails);
    return ok;
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

        if (ret > 0 && aa_write_aat(out_path, &bm, size_index))
        {
            char from[16];

            snprintf(from, sizeof(from), "from %dpx", art_sizes[s].dim);
            aa_note_size(size_index, from);
            return true;
        }
        if (ret > 0)
            return false;
    }

    return false;
}

/* Render one missing thumbnail by the configured strategy. */
static void aa_generate_size(const struct aa_src *src, int size_index,
                             unsigned int dh, const char *out_path,
                             void *workbuf, size_t workbuf_sz)
{
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
    if (art_folder_file(probe_path, aa_artpath, sizeof(aa_artpath)))
    {
        aa_note("Folder image: %s", aa_leaf(aa_artpath));
        return true;
    }
    aa_note("Folder image: none");
    return false;
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
    {
        aa_note("Tags: no image");
        return false;
    }
    aa_note("Tags: image, %lu KB",
            (unsigned long)(aa_id3.albumart.size + 1023) / 1024);
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
    if (aa_chk_sizes[0])
    {
        aa_note("%s", aa_chk_sizes);
        aa_chk_sizes[0] = '\0';
    }
    /* An image that would not decode leaves the folder bare, and is counted
     * and listed as one with no art. */
    if (aa_thumbs_exist(dh))
        return true;
    aa_miss = "would not decode";
    aa_miss_src = src->path;
    aa_note("No thumbnail: %s would not decode", aa_leaf(src->path));
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

    aa_note("Source: %s",
            source == ART_SOURCE_FILES       ? "files only" :
            source == ART_SOURCE_FILES_FIRST ? "files, then tags" :
            source == ART_SOURCE_EMBEDDED    ? "tags only" :
                                               "tags, then files");
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
    aa_note("No cover found");
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
 * a file named for them in /Artwork/People; the highest of a track's own
 * folder and the three above it named for them; the folder
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
    if (claim)
    {
        if (!aa_unreached)
            debug_log(DEBUG_LOG_ARTCACHE, "too many people");
        aa_unreached = true;
    }
    return NULL;
}

/* The entry of 'dir' in folder table 'd'; with 'who', claimed for them if
 * new */
static struct aa_libdir *aa_dir_entry(struct aa_dirs *d, unsigned int dir,
                                      unsigned int who)
{
    unsigned int i;

    if (!dir)
        return NULL;
    for (i = 0; i < d->slots; i++)
    {
        struct aa_libdir *l = &d->t[(dir + i) & (d->slots - 1)];

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
    if (who)
    {
        if (!d->full)
            debug_log(DEBUG_LOG_ARTCACHE, "too many folders");
        d->full = true;
    }
    return NULL;
}

/* 'dir' holds 'who', in a folder table: two of them make 'who' 0 */
static void aa_dir_note(struct aa_dirs *d, unsigned int dir, unsigned int who)
{
    struct aa_libdir *l = aa_dir_entry(d, dir, who);

    if (l && l->who != who)
        l->who = 0;
}

static bool aa_dir_shared(struct aa_dirs *d, unsigned int dir)
{
    struct aa_libdir *l = aa_dir_entry(d, dir, 0);

    return l ? l->who == 0 : d->full;
}

static bool aa_is_library(unsigned int dir)
{
    return aa_dir_shared(&aa_libdirs, dir);
}

/* Whether the folder of the book at 'path' stands for the book alone: it
 * holds no other book of its kind ('salt' 0 for an audiobook, AA_EBOOK_DIRS
 * for an ebook), and is not named for the book's person 'name', whose
 * picture its image is */
static bool aa_book_folder(const char *path, unsigned int salt,
                           const char *name)
{
    const char *end = strrchr(path, '/'), *start;

    if (!end || aa_dir_shared(&aa_bookdirs, aa_hash_n(path, end - path) ^ salt))
    {
        aa_note("Book folder: holds other books, not used");
        return false;
    }
    for (start = end; start > path && start[-1] != '/'; start--)
        ;
    if (aa_keys_match(art_name_hash_n(start, end - start),
                      art_name_alt_hash_n(start, end - start),
                      art_name_hash(name), art_name_alt_hash(name)))
    {
        aa_note("Book folder: the author's, not used");
        return false;
    }
    return true;
}

static void aa_book_noted(const char *path, unsigned int salt,
                          unsigned int who)
{
    const char *end = strrchr(path, '/');

    if (end)
        aa_dir_note(&aa_bookdirs, aa_hash_n(path, end - path) ^ salt, who);
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
                aa_dir_note(&aa_libdirs, aa_hash_n(path, start - 1 - path),
                            key);
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
        {
            aa_note("Artwork/People: %s", aa_leaf(aa_artpath));
            return true;
        }
    }
    aa_note("Artwork/People: none");
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
    {
        aa_note("Artwork/Covers: no title to match");
        return false;
    }
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
            {
                aa_note("Artwork/Covers: %s", aa_leaf(aa_artpath));
                return true;
            }
        }
    }
    aa_note("Artwork/Covers: none");
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

    /* A check makes the picture again rather than taking one over */
    for (; !aa_checking && old <= 1 && slot->stamp == AA_STAMP_NONE
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
        aa_note("No picture: the name clashes with a work's");
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
        if (!p->match)
            aa_note("Named folder: none");
    }

    if (p->match && !(p->flags & AA_PERSON_TRIED))
    {
        end = aa_level_end(path, p->level);
        if (!end || aa_hash_n(path, end - path) != p->match)
            return;                     /* for an item under it */
        p->flags |= AA_PERSON_TRIED;
        aa_note("Named folder: %.*s", (int)(end - path), path);
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
        /* A check counts the folders without an image, of which a
         * prolific person has many, rather than writing each down */
        aa_chk_quiet = true;
        if (aa_folder_image(path, end))
        {
            aa_chk_quiet = false;
            aa_note("Folder above: %s", aa_artpath);
            aa_person_fill(p, slot, path, up, c);
            return;
        }
        aa_chk_quiet = false;
        aa_chk_missed++;
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

/* One book's author, kept on a pass without the RAM copy */
static bool aa_author_spare(const struct book_entry *e, void *data)
{
    bool *aborted = data;
    unsigned int key = art_name_hash(e->author);

    if (aa_check_abort() || tagcache_is_busy())
    {
        *aborted = true;
        return false;
    }
    if (key)
        aa_spare(key);
    return true;
}

/* The people step without the RAM copy, which the album rows it settles
 * pictures through need: no picture is looked for, but those of the album
 * artists and authors still in the library are kept, and the artists with
 * none are listed */
static void aa_people_spare(bool *aborted)
{
    struct tagcache_search tcs;
    int since_yield = 0;

    if (aa_old_scheme)
    {
        debug_log(DEBUG_LOG_ARTCACHE, "people kept: old scheme, no RAM copy");
        aa_unreached = true;
        return;
    }
    if (!tagcache_search(&tcs, tag_albumartist))
    {
        aa_unreached = true;
        return;
    }
    while (tagcache_get_next(&tcs, aa_tcs_buf, sizeof(aa_tcs_buf)))
    {
        unsigned int key = art_name_hash(tcs.result);

        if (aa_check_abort() || tagcache_is_busy())
        {
            *aborted = true;
            break;
        }
        if (key && strcmp(tcs.result, UNTAGGED))
        {
            const struct aa_stamp *s = aa_spare(key);

            aa_counts.artists++;
            if (s && s->stamp != AA_STAMP_NONE)
                aa_counts.artist_art++;
            else
                path_list_write_record(&noart_artists, tcs.result);
        }
        if (++since_yield >= 16)
        {
            since_yield = 0;
            yield();
        }
    }
    if (tcs.failed && !*aborted)
        aa_unreached = true;
    tagcache_search_finish(&tcs);
    if (!*aborted)
        book_index_each(aa_author_spare, aborted, &aa_book_text);
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
            if (c->stage == AA_PEOPLE_RESOLVE && aa_track_is_book(tcs.idx_id)
                && (!aa_checking || aa_hash(tcs.result) == aa_only_book))
                aa_book_track_cover(tcs.result, c);
            if (aa_row.key && !*c->aborted
                && (!aa_checking || aa_row.key == aa_only_person))
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
    if (key && (!aa_checking || key == aa_only_person))
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
        {
            aa_note("Named after the file: %s", aa_leaf(aa_artpath));
            return true;
        }
    }
    aa_note("Named after the file: none");
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
        aa_note("Inside the book: %lu KB",
                (unsigned long)(e->cover_csize + 1023) / 1024);
        src.path = e->path;
        src.emb_pos = e->cover_at;
        src.emb_size = e->cover_csize;
        stamp = aa_art_stamp(e->path);
    }
    else if (inside)
    {
        stamp = aa_art_stamp(e->path);
        aa_note("Inside the book: %lu KB",
                (unsigned long)(e->cover_usize + 1023) / 1024);
        if (slot->stamp != stamp || !aa_thumbs_exist(bh))
        {
            /* Before the decode needs the work buffer */
            if (!zip_member_to_file(e->path, e->cover_at, e->cover_csize,
                                    e->cover_usize, e->cover_method,
                                    AA_COVER_TMP, c->workbuf, c->worksz))
            {
                aa_note("No cover: it would not unpack");
                return true;
            }
            tmp = true;
        }
        src.path = AA_COVER_TMP;
    }
    else
    {
        aa_note(source == ART_SOURCE_FILES
                ? "No cover found (Source does not look inside books)"
                : "No cover found");
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

/* On the pass that moves albums from their folders' keys to their names'
 * (see AA_SCHEME), for each track whose album key 'dh' is not its folder's
 * 'fh'. A folder's thumbnails and stamp were made from its first track, so
 * the album of that track takes them over, if it has none of its own yet;
 * 'slot' is that album's slot when this track is the album's first, else
 * NULL. A folder is claimed once, by its first track, and any other album
 * in it makes its own. Thumbnails stamped EMBEDDED can instead be a played
 * track's, offered for a folder whose first track had none: they are taken
 * only when 'path', that first track, holds an image itself. */
static void aa_album_adopt(unsigned int fh, unsigned int dh,
                           struct aa_stamp *slot, const char *path,
                           void *buf, size_t size)
{
    struct aa_stamp *os = aa_find(fh);
    struct aa_src src;

    if (!os || os->stamp == AA_STAMP_CLAIMED || aa_was_visited(fh))
        return;
    if (slot && slot->stamp == AA_STAMP_NONE && !aa_thumbs_exist(dh)
        && (os->stamp != AA_STAMP_EMBEDDED || aa_find_embedded(path, &src))
        && (!aa_thumbs_exist(fh) || aa_adopt(fh, dh, true, buf, size)))
    {
        slot->stamp = os->stamp;
        debug_log(DEBUG_LOG_ARTCACHE, "album kept: %s", aa_dir);
    }
    os->stamp = AA_STAMP_CLAIMED;
}

/* Lay the tables a pass keeps out in 'mem', AA_TABLE_BYTES of it, empty */
static void aa_tables_init(void *mem)
{
    aa_stamps = mem;
    aa_visited = (unsigned char *)(aa_stamps + AA_SEEN_SLOTS);
    aa_people = (struct aa_person *)(aa_visited + AA_SEEN_SLOTS / 8);
    aa_libdirs.t = (struct aa_libdir *)(aa_people + AA_PEOPLE_SLOTS);
    aa_bookdirs.t = aa_libdirs.t + AA_LIBDIR_SLOTS;
    aa_central = (struct aa_central *)(aa_bookdirs.t + AA_BOOKDIR_SLOTS);
    aa_central_names = (char *)(aa_central + AA_CENTRAL_MAX);
    memset(aa_stamps, 0, AA_TABLE_BYTES);
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

    aa_io_failed = false;
    worksz = aa_work_bytes();
    /* No marks to have covered: a trigger, a first pass, a format bump or the
     * old scheme. An upgrade only renames, so its pass reads no tags again. */
    aa_scheme_began = aa_format_current() ? aa_scheme_read() : AA_SCHEME;
    aa_old_scheme = aa_scheme_began < 1;
    aa_album_move = aa_scheme_began < 2;
    aa_check_all = dircache_is_ready()
                || art_cache_task.done_marks.entries < 0;
    aa_reread_tags = art_cache_task.done_marks.entries < 0
                     && aa_scheme_began >= AA_SCHEME;
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
    aa_tables_init(core_get_data_pinned(sh));

    /* Includes placeholders and migration before the library walk. */
    cache_busy = true;
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
        cache_busy = false;
        return BG_INTERRUPTED;
    }
    aa_unreached = aa_libdirs.full = aa_bookdirs.full = false;
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
        unsigned int dh, fh;
        struct aa_stamp *slot;

        /* Abort promptly on USB/shutdown (the thread loop then acknowledges)
         * or yield the disk to an incoming database commit, so a long pass
         * never blocks either. */
        if (aa_check_abort() || tagcache_is_busy())
        {
            aborted = true;
            break;
        }

        /* The track's album, by the key its row has -- a book's by its
         * folder, its own cover being made on the books' walk -- or without
         * the RAM copy by its folder. Skip the per-album work entirely once
         * the album has been visited this pass (aa_visit records it), so its
         * later tracks are cheap. */
        aa_dirname(tcs.result, aa_dir, sizeof(aa_dir));
        fh = dh = aa_hash(aa_dir);
        aa_track_row(tcs.idx_id);
        if (aa_row.ok && aa_row.al.art_hash
            && db_spoken_album_kind(&aa_row.al) != 1)
            dh = aa_row.al.art_hash;
        slot = aa_visit(dh);
        if (aa_album_move && dh != fh)
            aa_album_adopt(fh, dh, slot, tcs.result, workbuf, worksz);
        if (slot)
        {
            struct aa_work w = { aa_name, aa_title, false, true };

            /* No album row without the RAM copy: the names /Artwork/Covers
             * is matched on come from the disk, once a folder */
            if (!aa_row.ok)
                tagcache_retrieve_two(&tcs, tcs.idx_id,
                                      tag_albumartist, aa_name,
                                      sizeof(aa_name),
                                      tag_album, aa_title, sizeof(aa_title));
            aa_counts.albums++;
            if (aa_cache_dir(tcs.result, dh, slot, &w, workbuf, worksz,
                             &aborted))
                aa_counts.album_art++;
            else if (!aborted)
            {
                /* Its folder and its name: the folder may hold others */
                static char line[MAX_PATH];

                if (dh != fh && aa_title[0] && strcmp(aa_title, UNTAGGED))
                {
                    /* the list shows what follows the last '/', so one
                     * in the name ("Live/Dead") becomes a '-' */
                    size_t d = strlen(aa_dir) + 1;

                    snprintf(line, sizeof(line), "%s/%s", aa_dir, aa_title);
                    for (char *p = line + MIN(d, strlen(line)); *p; p++)
                        if (*p == '/')
                            *p = '-';
                }
                else
                    strlcpy(line, aa_dir, sizeof(line));
                path_list_write_record(&noart_albums, line);
                aa_log_miss("album", line);
            }
        }
        if (aborted)
            break;

        /* What the track says about its artist and its book, for the
         * people step: a walk of only the added tracks does not see enough,
         * and the people step takes its own */
        if (!aa_added_only)
            aa_track_note(tcs.result, tcs.idx_id);
        /* A book's cover is keyed by its first track, which only an album
         * row says: without one, every track's is kept */
        if (!aa_row.ok)
            aa_spare(aa_hash(tcs.result));

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
    /* People and audiobook covers are found through the album rows, which
     * only the RAM copy has. Refused one, the pass keeps those still there. */
    if (!aborted && !failed)
    {
        if (!tagcache_is_in_ram())
            aa_people_spare(&aborted);
        else
            aa_people_pass(workbuf, worksz, &aborted);
    }
    cpu_boost(false); /* balances the boost above (skipped on the goto-out path) */

out:
    /* What the walk did not visit is not missing when it saw only the
     * added tracks, or could not reach all it saw */
    completed = !aborted && !failed && !aa_io_failed
                && !aa_added_only && !aa_unreached;
    noart_close(completed);
    if (completed)
        aa_remove_orphans();
    /* An interrupted move to new keys is taken up again by the next pass.
     * Without the RAM copy nothing moves, and the folders' keys are the ones
     * in use, so that pass records the scheme all the same. */
    aa_stamps_save(completed, aborted || failed || aa_io_failed
                              ? aa_scheme_began : AA_SCHEME);
    aa_stamps = NULL;
    aa_visited = NULL;
    core_unpin(wh);
    core_unpin(sh);
    core_free(sh);
    core_free(wh);
    cache_busy = false;
    if (failed || aa_io_failed)
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

/* At most three retries for this offer; a newer track owns its own budget. */
static void aa_offer_retry(unsigned serial)
{
    if (serial == aa_offer.serial && aa_offer.retries < 3)
    {
        aa_offer.retries++;
        timeout_register(&aa_offer_tmo, aa_offer_poll, HZ, 0);
    }
}

/* Cache the offered track's embedded art into any of its album's thumbnails
 * that don't exist yet. Fill-only: never overwrites art already cached (folder
 * images are the preferred on-disk source) -- this just gives a coverless album
 * the art playback already had parsed for the WPS, at no decode cost to it.
 * Stamped EMBEDDED, so a pass that later finds folder art replaces it. */
static void aa_handle_offer(void)
{
    char path[MAX_PATH];
    unsigned int dh;
    size_t worksz;
    int s, wh;
    void *workbuf;
    struct aa_src src;
    unsigned serial = aa_offer.serial;

    timeout_cancel(&aa_offer_tmo);
    aa_io_failed = false;
    aa_offer_posted = false;
    if (global_settings.art_cache_album_source == ART_SOURCE_FILES ||
        aa_offer.path[0] == '\0' || !tagcache_is_usable())
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

    dh = art_cache_track_key(path);

    if (!dh || aa_thumbs_exist(dh))
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
    {
        aa_offer_retry(serial);
        return;
    }
    wh = core_alloc(worksz);
    if (wh <= 0)
    {
        aa_offer_retry(serial);
        return;
    }
    workbuf = core_get_data_pinned(wh);

    cache_busy = true;
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
    cache_busy = false;
}

/* ---- checking one file ----
 *
 * Check a File's Art: one file's art made again as a pass makes it, each step
 * written into a report for the screen. Its walk is a pass's -- every track,
 * for what each says about its person and its book's folder -- so a person is
 * found exactly as a pass finds them; then only the file's own album, book
 * and person are settled. A book's cover and its person are settled on walks
 * of their own, so that the report keeps them apart. */

static bool aa_chk_ebook;

/* The ebook asked about, if 'e' is it: its cover made again, its author kept
 * for the person step. An entry lasts only for its call, so the work is done
 * here. */
static bool aa_check_ebook(const struct book_entry *e, void *data)
{
    struct aa_books_ctx *c = data;
    unsigned int bh = aa_hash(e->path);
    struct aa_stamp *slot;

    if (strcasecmp(path_key_strip(e->path), path_key_strip(aa_chk.path)))
        return true;
    aa_chk_ebook = true;
    strlcpy(aa_chk_person, e->author ? e->author : "", sizeof(aa_chk_person));
    aa_note("Ebook: %s, %s", e->title ? e->title : "", aa_chk_person);
    aa_note("\nBOOK COVER");
    if ((slot = aa_slot(bh)))
    {
        aa_remove_thumbs(bh);
        slot->stamp = AA_STAMP_NONE;
        aa_book_visit(e, c);
        aa_stamp_append(bh, slot->stamp);
    }
    return false;
}

/* The person aa_only_person, settled again as a pass's people step settles
 * them */
static void aa_check_person(void *workbuf, size_t worksz, bool *aborted)
{
    struct aa_people_ctx c = { AA_PEOPLE_RESOLVE, workbuf, worksz, aborted };
    struct aa_stamp *slot = aa_slot(aa_only_person);
    struct aa_person *p;

    if (!slot)
        return;
    aa_remove_thumbs(aa_only_person);
    slot->stamp = AA_STAMP_NONE;
    aa_chk_missed = 0;
    aa_people_waiting = 0;
    if (aa_people_walk(&c))
    {
        aa_people_last_dir = aa_people_last_key = 0;
        book_index_each(aa_book_person, &c, &aa_book_text);
    }
    if (!*aborted && aa_people_waiting)
    {
        c.stage = AA_PEOPLE_FALLBACK;
        if (aa_people_walk(&c))
        {
            aa_people_last_dir = aa_people_last_key = 0;
            book_index_each(aa_book_person, &c, &aa_book_text);
        }
    }
    if (*aborted)
        return;
    if (aa_chk_missed)
        aa_note("Folders above: %d, no image", aa_chk_missed);
    p = aa_person_get(aa_only_person, false);
    if (!p || !(p->flags & AA_PERSON_ART))
        aa_note("No picture found");
    aa_stamp_append(aa_only_person, slot->stamp);
}

/* The book of the audiobook track asked about, by its first track */
static void aa_check_audiobook(void *workbuf, size_t worksz, bool *aborted)
{
    struct aa_people_ctx c = { AA_PEOPLE_RESOLVE, workbuf, worksz, aborted };
    unsigned int bh = aa_hash(aa_chk_first);
    struct aa_stamp *slot = aa_slot(bh);

    aa_note("\nBOOK COVER");
    aa_note("From first track: %s", aa_leaf(aa_chk_first));
    if (!slot)
        return;
    aa_remove_thumbs(bh);
    slot->stamp = AA_STAMP_NONE;
    aa_only_book = bh;
    aa_people_walk(&c);
    aa_only_book = 0;
    aa_stamp_append(bh, slot->stamp);
}

/* aa thread: the check posted by art_cache_check() */
static void aa_run_check(void)
{
    struct tagcache_search tcs;
    struct aa_books_ctx books;
    size_t worksz = aa_work_bytes();
    void *workbuf;
    int wh, sh, idx = -1, first = -1, kind = 0, row = -1;
    unsigned int person = 0, key = 0;
    bool aborted = false, ram = tagcache_is_in_ram();
    /* The file's folder, without the volume, to know its tracks by */
    const char *dir = path_key_strip(aa_chk.path);
    const char *dir_end = strrchr(dir, '/');

    aa_io_failed = false;
    aa_chk.state = AA_CHECK_RUNNING;
    aa_chk.walked = 0;
    aa_chk.total = tagcache_get_stat()->total_entries;
    aa_checking = true;
    aa_chk_quiet = aa_chk_ebook = false;
    aa_only_person = aa_only_book = 0;
    aa_chk_track[0] = aa_chk_first[0] = aa_chk_probe[0] = '\0';
    aa_chk_name[0] = aa_chk_title[0] = aa_chk_person[0] = '\0';
    aa_chk_sizes[0] = '\0';
    aa_note("%s", aa_leaf(aa_chk.path));

    wh = core_alloc(worksz);
    sh = wh > 0 ? core_alloc(AA_TABLE_BYTES) : -1;
    if (sh <= 0)
    {
        if (wh > 0)
            core_free(wh);
        aa_note("Not enough free memory right now");
        goto done;
    }
    workbuf = core_get_data_pinned(wh);
    aa_tables_init(core_get_data_pinned(sh));
    books.workbuf = workbuf;
    books.worksz = worksz;
    books.aborted = &aborted;
    cache_busy = true;
    cpu_boost(true);

    aa_ensure_dirs();
    if (!aa_check_format_version())
    {
        aa_note("The art cache is being rebuilt: try again after");
        goto out;
    }
    aa_check_all = aa_reread_tags = true;
    aa_added_only = aa_people_move = false;
    aa_unreached = aa_libdirs.full = aa_bookdirs.full = false;
    aa_stamps_load();
    aa_central_count = 0;
    aa_central_used = 0;
    aa_central_load(false);
    aa_central_load(true);

    if (tagcache_find_index(&tcs, aa_chk.path))
    {
        idx = tcs.idx_id;
        aa_row.row = -2;
        aa_track_row(idx);
        if (aa_row.ok)
        {
            kind = db_spoken_album_kind(&aa_row.al);
            first = aa_row.al.first;
            person = aa_row.key;
            strlcpy(aa_chk_person, aa_name, sizeof(aa_chk_person));
            strlcpy(aa_chk_title, aa_title, sizeof(aa_chk_title));
            /* Its album by its row, as a pass keys it */
            if (kind != 1 && aa_row.al.art_hash)
            {
                row = aa_row.row;
                key = aa_row.al.art_hash;
            }
        }
        else
            tagcache_retrieve_two(&tcs, idx, tag_albumartist, aa_chk_person,
                                  sizeof(aa_chk_person), tag_album,
                                  aa_chk_title, sizeof(aa_chk_title));
        tagcache_search_finish(&tcs);
        aa_note("%s: %s, %s", kind == 1 ? "Audiobook"
                : kind == 2 ? "Podcast" : "Music", aa_chk_title,
                aa_chk_person);
    }

    /* Every track, as a pass reads them, for who is in which folder */
    if (!tagcache_search(&tcs, tag_filename))
    {
        aa_note("The database could not be read");
        goto out;
    }
    aa_row.row = -2;
    while (tagcache_get_next(&tcs, aa_tcs_buf, sizeof(aa_tcs_buf)))
    {
        if (aa_check_abort() || tagcache_is_busy())
        {
            aborted = true;
            break;
        }
        aa_track_note(tcs.result, tcs.idx_id);
        if (tcs.idx_id == idx)
            strlcpy(aa_chk_track, tcs.result, sizeof(aa_chk_track));
        if (tcs.idx_id == first)
            strlcpy(aa_chk_first, tcs.result, sizeof(aa_chk_first));
        if (idx >= 0 && !aa_chk_probe[0] && dir_end)
        {
            const char *p = path_key_strip(tcs.result);
            const char *end = strrchr(p, '/');

            if (row >= 0 ? aa_row.row == row
                : end && end - p == dir_end - dir
                  && !strncasecmp(p, dir, end - p))
            {
                strlcpy(aa_chk_probe, tcs.result, sizeof(aa_chk_probe));
                if (aa_row.ok)
                {
                    strlcpy(aa_chk_name, aa_name, sizeof(aa_chk_name));
                    strlcpy(aa_chk_title, aa_title, sizeof(aa_chk_title));
                }
                else
                    tagcache_retrieve_two(&tcs, tcs.idx_id, tag_albumartist,
                                          aa_chk_name, sizeof(aa_chk_name),
                                          tag_album, aa_chk_title,
                                          sizeof(aa_chk_title));
            }
        }
        if ((++aa_chk.walked & 15) == 0)
            yield();
    }
    tagcache_search_finish(&tcs);
    if (aborted)
        goto out;
    book_index_each(aa_book_note, &books, &aa_book_text);

    if (aa_chk_track[0] && aa_chk_probe[0])
    {
        struct aa_work w = { aa_chk_name, aa_chk_title, false, true };
        struct aa_stamp *slot;
        unsigned int dh;

        aa_dirname(aa_chk_probe, aa_dir, sizeof(aa_dir));
        dh = key ? key : aa_hash(aa_dir);
        aa_note("\nALBUM COVER");
        aa_note("Folder: %s", aa_dir);
        if (strcmp(aa_chk_probe, aa_chk_track))
            aa_note("From first track: %s", aa_leaf(aa_chk_probe));
        if ((slot = aa_visit(dh)))
        {
            aa_remove_thumbs(dh);
            slot->stamp = AA_STAMP_NONE;
            aa_cache_dir(aa_chk_probe, dh, slot, &w, workbuf, worksz,
                         &aborted);
            aa_stamp_append(dh, slot->stamp);
        }
        if (!aborted && kind == 1 && ram && aa_chk_first[0])
            aa_check_audiobook(workbuf, worksz, &aborted);
    }
    else
    {
        book_index_each(aa_check_ebook, &books, &aa_book_text);
        if (!aa_chk_ebook)
        {
            aa_note("Not in the library. If it is new, update the library "
                    "first");
            goto out;
        }
        person = art_name_hash(aa_chk_person);
    }
    if (aborted)
        goto out;

    if (!ram)
        aa_note("\nDatabase not in RAM: pictures%s not checked",
                kind == 1 ? " and book covers" : "");
    else if (!person)
        aa_note("\nNo %s tag, so no picture", kind == 1 || aa_chk_ebook
                ? "author" : "album artist");
    else
    {
        aa_note("\n%s: %s", kind == 1 || aa_chk_ebook ? "AUTHOR"
                : "ARTIST", aa_chk_person);
        aa_only_person = person;
        aa_check_person(workbuf, worksz, &aborted);
    }

out:
    cpu_boost(false);
    cache_busy = false;
    if (aborted)
        aa_note("\nStopped: the disk was needed elsewhere");
    else if (aa_io_failed)
        aa_note("\nArtwork could not be saved.");
    else if (aa_chk_track[0] || aa_chk_ebook)
        aa_note("\nNow Playing updates on the next track.");
    aa_stamps = NULL;
    aa_visited = NULL;
    core_unpin(wh);
    core_unpin(sh);
    core_free(sh);
    core_free(wh);
done:
    aa_checking = false;
    aa_only_person = aa_only_book = 0;
    aa_chk.state = AA_CHECK_DONE;
}

bool art_cache_check(const char *path, char *report, size_t size)
{
    if (aa_chk.state == AA_CHECK_RUNNING
        || (aa_chk.state == AA_CHECK_POSTED
            && bg_task_queued(&art_cache_task, AA_EVENT_CHECK))
        || size < 2)
        return false;
    strlcpy(aa_chk.path, path, sizeof(aa_chk.path));
    aa_chk.report = report;
    aa_chk.size = size;
    aa_chk.len = 0;
    report[0] = '\0';
    aa_chk.walked = aa_chk.total = 0;
    aa_chk.state = AA_CHECK_POSTED;
    bg_task_post(&art_cache_task, AA_EVENT_CHECK);
    return true;
}

bool art_cache_check_finished(int *walked, int *total)
{
    *walked = aa_chk.walked;
    *total = aa_chk.total;
    return aa_chk.state == AA_CHECK_DONE;
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
    aa_offer.serial++;
    aa_offer.retries = 0;
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

    /* The RAM copy loads after the database is usable: a pass begun before
     * it would find nobody's picture. Waited for unless it was refused. */
    if (!tagcache_is_in_ram() && !tagcache_ram_refused())
        return BG_INTERRUPTED;
    debug_log(DEBUG_LOG_ARTCACHE, "starting pass");
    aa_offer_posted = false;
    result = aa_run_pass();
    const char *how;
    if (result == BG_DONE)
        how = "pass complete, idle";
    else if (result != BG_FAILED)
        how = "pass interrupted";
    else if (aa_io_failed)
        how = "pass failed: artwork write";
    else
        how = "pass failed: database unreadable";
    debug_log(DEBUG_LOG_ARTCACHE, "%s", how);
    return result;
}

/* bg_task.handle_event: everything on the queue that is ours rather than the
 * tick's -- the offer posted by the track-change hook, and a check. */
static void aa_task_event(const struct queue_event *ev)
{
    if (ev->id == AA_EVENT_OFFER)
        aa_handle_offer();
    else if (ev->id == AA_EVENT_CHECK)
        aa_run_check();
}

/* The scheme the stamp file's first record names, 0 for none: see AA_SCHEME */
static unsigned int aa_scheme_read(void)
{
    struct aa_stamp first;
    int fd = open(AA_STAMP_FILE, O_RDONLY);
    bool ok;

    if (fd < 0)
        return 0;
    ok = lseek(fd, sizeof(struct libfile_header), SEEK_SET)
            == (off_t)sizeof(struct libfile_header)
         && read(fd, &first, sizeof(first)) == (ssize_t)sizeof(first)
         && first.key == AA_SCHEME_KEY;
    close(fd);
    return ok ? first.stamp : 0;
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
                     &h) && aa_scheme_read() == AA_SCHEME)
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

