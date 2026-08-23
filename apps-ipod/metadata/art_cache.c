/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Disk cache for cover art -- BOTH album art and artist art. Pre-scales each
 * image to the sizes skins ask for and stores it, so browsing does not
 * re-decode on every track. Album art comes from the album folder, artist art
 * from its parent; each has its own placeholder for when nothing is found.
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "config.h"


#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "core_alloc.h"
#include "string-extra.h"
#include "file.h"
#include "dir.h"
#include "pathfuncs.h"
#include "rbpaths.h"
#include "metadata.h"
#include "albumart.h"
#include "art_cache.h"
#include "art_sizes.h"
#include "settings/settings.h"  /* global_settings.art_cache_fast_build */
#include "system/debug_log.h"
#include "database/tagcache.h"
#include "system/bg_task.h"         /* the caching pass runs as one */
#include "files/path_list.h"        /* the "found nothing" lists */
#include "lcd.h"
#include "draw/bmp.h"
#include "draw/img_filter.h"
#include "bitmaps/podboxnoart.h" /* shared "no art" placeholder for aa_ensure_fallback */
#include "draw/jpeg_load.h"
#include "usb.h"
#include "events.h"
#include "system/appevents.h"
#include "audio.h"
#include "cpu.h"

/* Define LOGF_ENABLE to enable logf output in this file */
/*#define LOGF_ENABLE*/
#include "logf.h"

#define THUMBCACHE_DIR ROCKBOX_DIR "/thumbcache"
#define AA_VERSION_FILE THUMBCACHE_DIR "/format.txt"
/* Folders a pass found no art for, one path per line, for the health screen
 * (screens/system/art_health.c). Written to a .tmp and renamed only when a
 * pass finishes, so an aborted pass leaves the previous -- complete -- list
 * standing rather than a partial one that reads as "the rest are fine". */
#define AA_NOART_ALBUMS  THUMBCACHE_DIR "/noart_albums.lst"
#define AA_NOART_ARTISTS THUMBCACHE_DIR "/noart_artists.lst"
/* Entry count the cache was last completed for, so a restart with an
 * unchanged library does not re-walk the whole database. */
#define AA_DONE_FILE    THUMBCACHE_DIR "/done.txt"

/* On-disk thumbnail format (struct art_cache_header + native pixels, in the
 * order the header names) is declared in art_cache.h so consumers can read it.
 * A magic/version lets a future format change be detected per file rather than
 * needing a global cache wipe. */

/* Directory-dedup "seen" set: open-addressed table of directory-path hashes.
 * Sized generously; if a library exceeds this, extra directories simply get
 * re-resolved (still idempotent -- generation skips existing thumbnails). Holds
 * both album folders and their parent (artist) folders now, so it carries more
 * entries per pass than album-only did. */
#define AA_SEEN_SLOTS 16384  /* power of two */

/* Generous stack: the JPEG decoder called from a pass has deep frames. */
#define AA_STACK_SIZE (DEFAULT_STACK_SIZE + 0x2000)
static long aa_stack[AA_STACK_SIZE / sizeof(long)];
static const char aa_thread_name[] = "aacache";
static unsigned int aa_thread_id;
static struct event_queue aa_queue;

static volatile bool cache_busy;

/* What the current (or last completed) pass has covered, and the folder it is
 * on. Every one of these is a count of a branch the pass already takes, so
 * keeping them costs nothing beyond the increment; aa_dir is the scratch
 * buffer the pass already keeps the current directory in. */
static struct art_cache_counts aa_counts;

/* Scratch id3 used only to feed search_albumart_files(); kept out of the
 * thread stack because struct mp3entry is large. */
static struct mp3entry aa_id3;

/* Scratch buffers used only by the aacache thread (aa_run_pass /
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

/* The shared placeholder thumbnail for a size. The "_" prefix cannot collide
 * with an %08x hash filename, so it lives alongside the real thumbnails. */
static void aa_fallback_path(char *out, int out_len, int size_index)
{
    snprintf(out, out_len, THUMBCACHE_DIR "/%s/_fallback.aat",
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

/* ---------------------------------------------------------------------- *
 * Reading a cached thumbnail                                              *
 * ---------------------------------------------------------------------- */

int art_cache_load_aat(int fd, struct bitmap *bm, int max_size,
                       const struct img_filter *filter)
{
    struct art_cache_header hdr;
    int dw = bm->width, dh = bm->height;
    int sw, sh, dy, dx;

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
     * takes it as a size_t. 300 is the largest the cache ever writes. */
    if (sw <= 0 || sh <= 0 || sw > ART_CACHE_MAX_DIM ||
        sh > ART_CACHE_MAX_DIM ||
        dw <= 0 || dh <= 0 || dw > sw || dh > sh)   /* downscale only */
        return -1;
    if ((size_t)dw * dh * FB_DATA_SZ > (size_t)max_size)
        return -1;

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
            return -1;
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

    if (filter)
        img_filter_apply_banded((fb_data *)bm->data, dw, dh, filter);

    return dw * dh * FB_DATA_SZ;
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

/* Delete every cached thumbnail of every size (the directories themselves stay).
 * Used only when the on-disk format changes. */
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

    for (i = 0; i < ART_CACHE_NUM_SIZES; i++)
    {
        DIR *d;
        struct dirent *e;

        snprintf(dirpath, sizeof(dirpath), THUMBCACHE_DIR "/%s",
                 art_sizes[i].name);
        d = opendir(dirpath);
        if (!d)
            continue;

        while ((e = readdir(d)))
        {
            if (e->d_name[0] == '.')
                continue;
            snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, e->d_name);
            remove(filepath);
            yield();
        }
        closedir(d);
    }
}

/* The format version stamped alongside the cache, or -1 if there is none. */
static int aa_stamped_format(void)
{
    char buf[16];
    int fd, n, ver = -1;

    fd = open(AA_VERSION_FILE, O_RDONLY);
    if (fd >= 0)
    {
        n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0)
        {
            buf[n] = '\0';
            ver = atoi(buf);
        }
        close(fd);
    }
    return ver;
}

/* The generator decides "already cached?" with a bare file_exists(), and the
 * reader rejects any file whose header version doesn't match. So on a format
 * bump the stale files would be skipped forever *and* refused at render time -
 * every cover would silently go blank. Stamp the format version alongside the
 * cache and purge the thumbnails whenever it moves.
 *
 * Being inside the pass, this only ever runs once the pass has been allowed to
 * start; art_cache_init() is what makes sure a bump allows it. */
static void aa_check_format_version(void)
{
    char buf[16];
    int fd, n;
    int ver = aa_stamped_format();

    if (ver == ART_CACHE_FORMAT_VERSION)
        return;

    logf("albumart cache: format %d -> %d, purging", ver,
         ART_CACHE_FORMAT_VERSION);
    aa_purge_thumbs();

    fd = open(AA_VERSION_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        n = snprintf(buf, sizeof(buf), "%d\n", ART_CACHE_FORMAT_VERSION);
        write(fd, buf, n);
        close(fd);
    }
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

/* Returns true if 'h' was already present; otherwise records it and returns
 * false. h==0 is remapped so 0 can mark an empty slot. */
static bool aa_seen(unsigned int *seen, unsigned int h)
{
    unsigned int i, idx;
    if (h == 0)
        h = 1;
    idx = h & (AA_SEEN_SLOTS - 1);
    for (i = 0; i < AA_SEEN_SLOTS; i++)
    {
        unsigned int slot = (idx + i) & (AA_SEEN_SLOTS - 1);
        if (seen[slot] == 0)
        {
            seen[slot] = h;
            return false;
        }
        if (seen[slot] == h)
            return true;
    }
    return true; /* table full -> treat as present */
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

    fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
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

    if (!ok)
        remove(out_path);
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
 * pending, or a task that outranks this one is waiting to start. Uses
 * queue_peek so the event stays queued for the thread loop to actually
 * acknowledge -- we just need to stop touching the disk promptly.
 *
 * The rank check is what keeps the album index from queueing behind a full
 * artwork pass. The index is short and the carousel can be waiting on it,
 * while a pass here can run for minutes and has the memory the index needs
 * pinned for the whole of it. Standing down costs nothing: the pass resumes
 * from where the thumbnails on disk leave it. */
static bool aa_check_abort(void)
{
    struct queue_event ev;

    if (bg_task_preempted(&art_cache_task))
        return true;

    /* Trap: the queue alone is too late for a host. Nothing arrives on it
     * until SET_CONFIGURATION, by which point a pass holding the CPU has
     * already cost the host SET_ADDRESS. */
    if (usb_host_is_present())
        return true;

    if (!queue_peek(&aa_queue, &ev))
        return false;
    switch (ev.id)
    {
        case SYS_USB_CONNECTED:
        case SYS_POWEROFF:
        case SYS_REBOOT:
            return true;
    }
    return false;
}

/* Resolve one folder's cover art and render any of its thumbnail sizes that
 * don't exist yet. `probe_path` is a track filename under the folder (real for
 * an album folder, synthetic "<dir>/_" for an artist folder) that
 * search_albumart_files() strips down to the folder to locate cover.bmp /
 * folder.jpg; `dh` is that folder's hash (the cache key). Sets *aborted if a
 * USB/shutdown/DB-busy stop was hit mid-decode. A cheap no-op once every size
 * already exists (dircache-served file_exists checks, no art re-resolution). */
static bool aa_cache_dir(const char *probe_path, unsigned int dh,
                         void *workbuf, size_t worksz, bool *aborted)
{
    int s;
    bool all_exist = true;

    for (s = 0; s < ART_CACHE_NUM_SIZES; s++)
    {
        aa_cache_path(aa_check_path, sizeof(aa_check_path), s, dh);
        if (!file_exists(aa_check_path))
        {
            all_exist = false;
            break;
        }
    }
    if (all_exist)
        return true;

    /* Only now (a thumbnail is missing) resolve this folder's cover art.
     * album/albumartist left NULL: only folder-based art is searched
     * (cover.bmp, folder.jpg, ../cover.bmp). */
    memset(&aa_id3, 0, sizeof(aa_id3));
    strlcpy(aa_id3.path, probe_path, sizeof(aa_id3.path));
    if (!search_albumart_files(&aa_id3, "", aa_artpath, sizeof(aa_artpath)))
        return false;   /* no cover art in this folder */

    struct aa_src src = { aa_artpath, -1, 0, 0 };  /* folder image on disk */
    int order[ART_CACHE_NUM_SIZES];
    int i;

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
        aa_generate_size(&src, s, dh, aa_out_path, workbuf, worksz);
        yield();
    }
    return true;
}

/* One full generation pass: walk every track filename, dedup by directory,
 * resolve folder art, and render any missing thumbnails -- both the track's own
 * (album) folder and its parent (artist) folder, for libraries laid out as
 * <artist>/<album>/<track>. Returns true if the pass ran to completion, false if
 * it was aborted (USB/DB busy/no memory) and should be retried later. */
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

static bool aa_run_pass(void)
{
    struct tagcache_search tcs;
    size_t worksz;
    int wh, sh;
    void *workbuf;
    unsigned int *seen;
    bool aborted = false;
    int since_yield = 0;

    /* An AA_FIT_COVER decode stages a non-square image before cropping it
     * square, so the buffer is sized for the widest one aa_cover_dim() will
     * pass: the largest size at COVER_MAX_ASPECT. That fixes both of the terms
     * BM_SCALED_SIZE adds up -- the pixels, and the scaler's line buffers,
     * which depend on the width alone -- and those are the two bounds
     * aa_cover_dim() tests a stage against. See art_sizes.h. */
    worksz = BM_SCALED_SIZE(ART_CACHE_MAX_DIM *
                                ART_CACHE_COVER_MAX_ASPECT,
                            ART_CACHE_MAX_DIM, FORMAT_NATIVE, 0);
    worksz += JPEG_DECODE_OVERHEAD;

    wh = core_alloc(worksz);
    if (wh <= 0)
        return false; /* not enough free memory right now; retry later */

    sh = core_alloc(AA_SEEN_SLOTS * sizeof(unsigned int));
    if (sh <= 0)
    {
        core_free(wh);
        return false;
    }

    workbuf = core_get_data_pinned(wh);
    seen = core_get_data_pinned(sh);
    memset(seen, 0, AA_SEEN_SLOTS * sizeof(unsigned int));

    aa_ensure_dirs();
    aa_check_format_version();
    aa_ensure_fallback(workbuf, worksz);

    if (!tagcache_search(&tcs, tag_filename))
    {
        aborted = true;
        goto out;
    }

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
         * (aa_seen records it), so later tracks of the same album are cheap. */
        aa_dirname(tcs.result, aa_dir, sizeof(aa_dir));
        dh = aa_hash(aa_dir);
        if (!aa_seen(seen, dh))
        {
            aa_counts.albums++;
            if (aa_cache_dir(tcs.result, dh, workbuf, worksz, &aborted))
                aa_counts.album_art++;
            else if (!aborted)
                path_list_write_record(&noart_albums, aa_dir);
        }
        if (aborted)
            break;

        /* The parent folder (the artist), cached from <artist>/folder.jpg etc.
         * for <artist>/<album>/<track> layouts. Deduped independently of the
         * album so an artist whose first album has no cover still gets resolved.
         * Skipped when there is no distinct parent (flat/rooted layouts). */
        aa_dirname(aa_dir, aa_artist_dir, sizeof(aa_artist_dir));
        if (aa_artist_dir[0] && strcmp(aa_artist_dir, aa_dir) != 0)
        {
            ah = aa_hash(aa_artist_dir);
            if (!aa_seen(seen, ah))
            {
                snprintf(aa_probe, sizeof(aa_probe), "%s/_", aa_artist_dir);
                aa_counts.artists++;
                if (aa_cache_dir(aa_probe, ah, workbuf, worksz, &aborted))
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
    tagcache_search_finish(&tcs);
    cpu_boost(false); /* balances the boost above (skipped on the goto-out path) */

out:
    noart_close(!aborted);
    core_unpin(wh);
    core_unpin(sh);
    core_free(sh);
    core_free(wh);
    cache_busy = false;
    return !aborted;
}

/* Cache the offered track's embedded art into any of its folder's thumbnails
 * that don't exist yet. Fill-only: never overwrites art already cached (folder
 * images are the preferred on-disk source) -- this just gives a coverless folder
 * the art playback already had parsed for the WPS, at no decode cost to it. */
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

    worksz = BM_SCALED_SIZE(ART_CACHE_MAX_DIM *
                                ART_CACHE_COVER_MAX_ASPECT,
                            ART_CACHE_MAX_DIM, FORMAT_NATIVE, 0);
    worksz += JPEG_DECODE_OVERHEAD;
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
        s = order[i];
        aa_cache_path(aa_check_path, sizeof(aa_check_path), s, dh);
        if (file_exists(aa_check_path))
            continue;
        aa_cache_path(aa_out_path, sizeof(aa_out_path), s, dh);
        aa_generate_size(&src, s, dh, aa_out_path, workbuf, worksz);
        yield();
    }

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
    if (!id3 || !id3->has_embedded_albumart ||
        (id3->albumart.type & AA_CLEAR_FLAGS_MASK) != AA_TYPE_JPG)
        return;

    strlcpy(aa_offer.path, id3->path, sizeof(aa_offer.path));
    aa_offer.pos = id3->albumart.pos;
    aa_offer.size = id3->albumart.size;
    aa_offer.flags = id3->albumart.type;
    queue_post(&aa_queue, AA_EVENT_OFFER, 0);
}

/* bg_task.run: one pass, with the tracing the pass itself does not do. */
static bool aa_task_run(void)
{
    bool done;

    debug_log(DEBUG_LOG_ARTCACHE, "starting pass");
    done = aa_run_pass();
    debug_log(DEBUG_LOG_ARTCACHE,
              done ? "pass complete, idle" : "pass interrupted");
    return done;
}

/* bg_task.handle_event: everything on the queue that is ours rather than the
 * tick's -- which is just the offer posted by the track-change hook. */
static void aa_task_event(const struct queue_event *ev)
{
    if (ev->id == AA_EVENT_OFFER)
        aa_handle_offer();
}

/* Ranked below the album index: a full pass here runs for minutes and pins the
 * memory the index wants, so this is the one that stands down. The marker is
 * read back at init rather than started at -1 -- starting at -1 meant the first
 * settled count after a boot never matched, so a full pass ran on every startup,
 * walking the whole database with cache_busy set, which is what held the
 * "Building" indicator up with nothing actually to do. */
struct bg_task art_cache_task =
{
    .done_file    = AA_DONE_FILE,
    .rank         = BG_RANK_ART,
    .run          = aa_task_run,
    .purge        = aa_purge_thumbs,
    .handle_event = aa_task_event,
};

static void aa_thread(void)
{
    while (1)
        bg_task_tick(&art_cache_task, &aa_queue);
}

void art_cache_init(void)
{
    cache_busy = false;
    queue_init(&aa_queue, true);

    /* A format bump leaves every cached thumbnail unreadable, but it does not
     * move the database's entry count -- so the marker still says this library
     * is covered, and the pass that would purge and regenerate them would
     * never be run to notice. Every cover goes blank and stays blank.
     *
     * So drop the marker here, before bg_task_init() reads it, which is enough
     * to make the task stale; the purge itself stays inside the pass. Boot is
     * the only place this needs checking, the version being a compile-time
     * constant that can only move across a firmware update. */
    if (aa_stamped_format() != ART_CACHE_FORMAT_VERSION)
        remove(AA_DONE_FILE);

    bg_task_init(&art_cache_task);
    aa_thread_id = create_thread(aa_thread, aa_stack, sizeof(aa_stack), 0,
                                 aa_thread_name IF_PRIO(, PRIORITY_BACKGROUND)
                                 IF_COP(, CPU));
    (void)aa_thread_id;

    /* Opportunistically cache the embedded art of tracks as they play, for
     * folders that have no on-disk cover (fill-only -- see aa_handle_offer). */
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, aa_track_change_cb);
}

