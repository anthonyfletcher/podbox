/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Banners for bios and liner notes: the owner's banner art from /Notes,
 * cached at the banner's size, and the places the owner has moved a
 * portrait's banner to.
 *
 * Banner art is not in the database, so no library change brings it to the
 * art cache. The art cache sweeps /Notes at the end of each pass and when
 * asked to (art_cache_banners_changed()): a sweep compares each image's
 * stamp -- its path, size and time -- with the one its banner was made from,
 * makes what is new or changed, and removes what has gone. The stamps are a
 * libfile beside the banners, of this cache format's version, so a format
 * change makes them all again.
 *
 * Parts, in order:
 *   - the stamps
 *   - making one banner: cropped to fill 320 x 132, centred across and a
 *     third of the way down
 *   - the sweep: People, Albums, and /Notes's other folders
 *   - lookups by person, by album and by page
 *   - the moved banners, user/banners.dat
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "lcd.h"
#include "system/library_files.h"
#include "database/libfile.h"
#include "draw/bmp.h"
#include "draw/image_load.h"
#include "albumart.h"           /* art_image_ext */
#include "art_cache.h"          /* the .aat header */
#include "art_sizes.h"          /* what the decode buffer holds */
#include "art_name.h"
#include "art_banner.h"
#include "settings/settings.h"

#define BANNER_DIR      LIB_ART_DIR "/banner"
#define BANNER_STAMPS   BANNER_DIR "/stamps.dat"
#define BANNER_MAX      256     /* banners kept; more are not cached */
#define BANNER_PATH     64      /* a banner's .aat path, with room */

/* ---- the stamps -------------------------------------------------------- */

struct banner_rec
{
    uint32_t key;       /* the file's name key, and its .aat's name; another
                           folder's is banner_other_key() */
    uint32_t alt;       /* a person's "Surname, First" key, or 0 */
    uint32_t stamp;     /* of the image it was made from */
    uint8_t kind;       /* BANNER_PERSON, BANNER_ALBUM or BANNER_OTHER */
    uint8_t made;       /* 0 when the image would not decode */
    uint8_t pad[2];
};

static struct banner_rec banner_old[BANNER_MAX], banner_new[BANNER_MAX];
static bool banner_seen[BANNER_MAX];
static char banner_path[MAX_PATH], banner_tmp[MAX_PATH], banner_dir[MAX_PATH];

/* A page in another folder is its name in its folder, so two folders can
 * each have one of a name */
static uint32_t banner_other_key(uint32_t name, uint32_t folder)
{
    uint32_t key = name ^ (folder * 0x9E3779B1u);

    return key ? key : 1;
}

/* A person's and an album of the same name are two files */
static void banner_aat_path(char *out, size_t size, uint32_t key, int kind)
{
    snprintf(out, size, BANNER_DIR "/%c%08x.aat",
             kind == BANNER_PERSON ? 'p' : kind == BANNER_ALBUM ? 'a' : 'o',
             (unsigned int)key);
}

/* The stamps, into `recs`; their number, 0 for none or a file of another
 * format */
static int banner_load(struct banner_rec *recs)
{
    struct libfile_header h;
    int fd = libfile_open(BANNER_STAMPS, LIB_BANNERS_MAGIC,
                          ART_CACHE_FORMAT_VERSION, sizeof(*recs), &h, NULL);
    int n = 0;

    if (fd < 0)
        return 0;
    if (h.count <= BANNER_MAX)
    {
        ssize_t want = (ssize_t)(h.count * sizeof(*recs));

        if (read(fd, recs, want) == want)
            n = h.count;
    }
    close(fd);
    return n;
}

static void banner_save(const struct banner_rec *recs, int n)
{
    struct libfile_writer w;
    struct libfile_marks m;

    libfile_no_marks(&m);
    if (libfile_begin(&w, BANNER_STAMPS, LIB_BANNERS_MAGIC,
                      ART_CACHE_FORMAT_VERSION, sizeof(*recs), &m))
        libfile_finish(&w, libfile_write(&w, recs, n * sizeof(*recs), n));
}

/* An image's stamp: its path, size and time, hashed. Equality is all it is
 * compared for, so a replacement copied in with an older time still counts
 * as changed. */
static uint32_t banner_stamp(const char *path, const struct dirinfo *info)
{
    uint32_t h = 0x811C9DC5u;

    while (*path)
        h = (h ^ (unsigned char)*path++) * 16777619u;
    h = (h ^ (uint32_t)info->size) * 16777619u;
    h = (h ^ (uint32_t)info->mtime) * 16777619u;
    return h ? h : 1;
}

/* ---- making one banner ------------------------------------------------- */

static int banner_read(const char *path, struct bitmap *bm, int fmt, int w,
                       int h, void *buf, size_t size)
{
    struct image_src src = { open(path, O_RDONLY), 0, -1, 0 };
    int rc;

    memset(bm, 0, sizeof(*bm));
    bm->data = buf;
    bm->width = w;
    bm->height = h;
    bm->format = FORMAT_NATIVE;
    if (src.fd < 0)
        return -1;
    rc = image_load(&src, bm, (int)size, fmt, NULL);
    close(src.fd);
    return rc;
}

/* The banner of the image at `path`, written to `out`. The image is scaled
 * to cover the banner and cropped: centred across, and a third of the way
 * down the slack, where heads are. One too elongated for the decode buffer
 * is fitted inside the banner instead. */
static bool banner_make(const char *path, const char *out, void *buf,
                        size_t size)
{
    struct art_cache_header hdr;
    struct bitmap bm;
    int fmt = FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_DITHER;
    int tw = ART_BANNER_W, th = ART_BANNER_H, sw, sh, fd;
    bool cover = false, ok;
    size_t bytes;

    if (banner_read(path, &bm, FORMAT_NATIVE | FORMAT_RETURN_SIZE,
                    tw, th, buf, size) > 0 && bm.width > 0 && bm.height > 0)
    {
        sw = bm.width;
        sh = bm.height;
        if ((long)sw * ART_BANNER_H >= (long)sh * ART_BANNER_W)
        {
            th = ART_BANNER_H;                      /* wider than a banner */
            tw = MAX((sw * ART_BANNER_H + sh / 2) / sh, ART_BANNER_W);
        }
        else
        {
            tw = ART_BANNER_W;
            th = MAX((sh * ART_BANNER_W + sw / 2) / sw, ART_BANNER_H);
        }
        cover = (long)tw * th <= ART_CACHE_COVER_MAX_PX
                && tw <= ART_CACHE_COVER_MAX_W;
    }
    if (!cover)
    {
        tw = ART_BANNER_W;
        th = ART_BANNER_H;
        fmt |= FORMAT_KEEP_ASPECT;
    }

    if (banner_read(path, &bm, fmt, tw, th, buf, size) <= 0
        || bm.width <= 0 || bm.height <= 0)
        return false;

    if (cover && bm.width >= ART_BANNER_W && bm.height >= ART_BANNER_H)
    {
        fb_data *px = (fb_data *)bm.data;
        int x0 = (bm.width - ART_BANNER_W) / 2;
        int y0 = (bm.height - ART_BANNER_H) * 85 / 256;
        int y;

        for (y = 0; y < ART_BANNER_H; y++)
            memmove(px + (size_t)y * ART_BANNER_W,
                    px + (size_t)(y + y0) * bm.width + x0,
                    (size_t)ART_BANNER_W * FB_DATA_SZ);
        bm.width = ART_BANNER_W;
        bm.height = ART_BANNER_H;
    }

    /* Written whole under another name and renamed, so a reader never sees
     * one part-written */
    snprintf(banner_tmp, sizeof(banner_tmp), "%s.new", out);
    fd = open(banner_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    hdr.magic = ART_CACHE_MAGIC;
    hdr.version = ART_CACHE_FORMAT_VERSION;
    hdr.width = bm.width;
    hdr.height = bm.height;
    hdr.layout = AA_ROWS;
    hdr.pad = 0;
    bytes = (size_t)bm.width * bm.height * FB_DATA_SZ;
    ok = write(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
         && write(fd, bm.data, bytes) == (ssize_t)bytes;
    if (close(fd) < 0)
        ok = false;
    if (ok)
        ok = rename(banner_tmp, out) == 0;
    if (!ok)
        remove(banner_tmp);
    return ok;
}

/* ---- the sweep --------------------------------------------------------- */

static int banner_find(const struct banner_rec *recs, int n, uint32_t key,
                       int kind)
{
    int i;

    for (i = 0; i < n; i++)
        if (recs[i].key == key && recs[i].kind == kind)
            return i;
    return -1;
}

bool art_banner_each_dir(bool (*fn)(const char *dir, int kind,
                                    uint32_t folder, void *ctx), void *ctx)
{
    DIR *d;
    struct dirent *e;
    bool go_on;

    if (!fn(NOTES_PEOPLE_DIR, BANNER_PERSON, 0, ctx)
        || !fn(NOTES_ALBUMS_DIR, BANNER_ALBUM, 0, ctx)
        || !fn(NOTES_DIR, BANNER_OTHER, 0, ctx))
        return false;
    d = opendir(NOTES_DIR);
    if (!d)
        return true;
    go_on = true;
    while (go_on && (e = readdir(d)))
    {
        struct dirinfo info = dir_get_info(d, e);

        if (e->d_name[0] == '.' || !(info.attribute & ATTR_DIRECTORY)
            || !strcasecmp(e->d_name, "People")
            || !strcasecmp(e->d_name, "Albums"))
            continue;
        snprintf(banner_dir, sizeof(banner_dir), NOTES_DIR "/%s", e->d_name);
        go_on = fn(banner_dir, BANNER_OTHER, art_name_hash(e->d_name), ctx);
    }
    closedir(d);
    return go_on;
}

struct banner_sweep
{
    int n_old, n_new;
    bool changed;
    void *buf;
    size_t size;
    bool (*stop)(void);
};

/* One folder of /Notes: each image's banner kept, made or made again, into
 * banner_new. False when `stop` stopped it. */
static bool banner_sweep_dir(const char *dir, int kind, uint32_t folder,
                             void *ctx)
{
    struct banner_sweep *s = ctx;
    DIR *d = opendir(dir);
    struct dirent *e;
    bool go_on = true;

    if (!d)
        return true;
    while ((e = readdir(d)))
    {
        const char *dot = strrchr(e->d_name, '.');
        struct dirinfo info = dir_get_info(d, e);
        struct banner_rec rec;
        char aat[BANNER_PATH];
        int o;

        if (e->d_name[0] == '.' || !dot || !art_image_ext(dot)
            || (info.attribute & ATTR_DIRECTORY))
            continue;
        if (s->stop())
        {
            go_on = false;
            break;
        }

        memset(&rec, 0, sizeof(rec));
        rec.kind = kind;
        rec.key = art_name_hash_n(e->d_name, dot - e->d_name);
        rec.alt = kind == BANNER_PERSON
                  ? art_name_alt_hash_n(e->d_name, dot - e->d_name) : 0;
        if (rec.key && kind == BANNER_OTHER)
            rec.key = banner_other_key(rec.key, folder);
        if (!rec.key || banner_find(banner_new, s->n_new, rec.key, kind) >= 0)
            continue;                       /* no name, or a second image */
        if (s->n_new == BANNER_MAX)
            break;                  /* full: the rest have no banner */

        snprintf(banner_path, sizeof(banner_path), "%s/%s", dir, e->d_name);
        rec.stamp = banner_stamp(banner_path, &info);
        o = banner_find(banner_old, s->n_old, rec.key, kind);
        if (o >= 0)
            banner_seen[o] = true;

        banner_aat_path(aat, sizeof(aat), rec.key, kind);
        if (o >= 0 && banner_old[o].stamp == rec.stamp
            && (!banner_old[o].made || file_exists(aat)))
        {
            banner_new[s->n_new++] = banner_old[o];
            continue;
        }

        rec.made = banner_make(banner_path, aat, s->buf, s->size);
        banner_new[s->n_new++] = rec;
        s->changed = true;
        yield();
    }
    closedir(d);
    return go_on;
}

bool art_banner_sweep(void *workbuf, size_t worksz, bool (*stop)(void))
{
    struct banner_sweep s = { 0, 0, false, workbuf, worksz, stop };
    int i;
    bool done;

    if (!global_settings.liner_notes)
        return true;                /* nothing to do: not stopped */
    s.n_old = banner_load(banner_old);

    mkdir(BANNER_DIR);
    memset(banner_seen, 0, sizeof(banner_seen));
    done = art_banner_each_dir(banner_sweep_dir, &s);

    for (i = 0; i < s.n_old; i++)
    {
        if (banner_seen[i])
            continue;
        if (done)
        {
            char aat[BANNER_PATH];

            banner_aat_path(aat, sizeof(aat), banner_old[i].key,
                            banner_old[i].kind);
            remove(aat);                    /* its image has gone */
            s.changed = true;
        }
        else if (s.n_new < BANNER_MAX)      /* not reached: kept as it was */
            banner_new[s.n_new++] = banner_old[i];
    }
    if (s.changed)
        banner_save(banner_new, s.n_new);
    return done;
}

void art_banner_forget(void)
{
    int n = banner_load(banner_old), i;

    for (i = 0; i < n; i++)
    {
        char aat[BANNER_PATH];

        banner_aat_path(aat, sizeof(aat), banner_old[i].key,
                        banner_old[i].kind);
        remove(aat);
    }
    remove(BANNER_STAMPS);
}

/* ---- lookups ----------------------------------------------------------- */

/* The made banner whose key or alt is either of `key` and `alt`, among
 * those of `kind`, as a path into `out` */
static bool banner_lookup(int kind, uint32_t key, uint32_t alt, char *out,
                          size_t size)
{
    struct banner_rec rec;
    struct libfile_header h;
    int fd;
    uint32_t i;
    bool found = false;

    if (!key && !alt)
        return false;
    fd = libfile_open(BANNER_STAMPS, LIB_BANNERS_MAGIC,
                      ART_CACHE_FORMAT_VERSION, sizeof(rec), &h, NULL);
    if (fd < 0)
        return false;
    for (i = 0; i < h.count && !found; i++)
    {
        if (read(fd, &rec, sizeof(rec)) != (ssize_t)sizeof(rec))
            break;
        if (rec.kind != kind || !rec.made)
            continue;
        found = (key && (rec.key == key || rec.alt == key))
                || (alt && (rec.key == alt || rec.alt == alt));
    }
    close(fd);
    if (found)
        banner_aat_path(out, size, rec.key, kind);
    return found;
}

bool art_banner_person(const char *name, char *out, size_t size)
{
    return banner_lookup(BANNER_PERSON, art_name_hash(name),
                         art_name_alt_hash(name), out, size);
}

/* As /Artwork/Covers is matched: the artist and title together, else the
 * title alone */
bool art_banner_album(const char *artist, const char *title, char *out,
                      size_t size)
{
    return banner_lookup(BANNER_ALBUM, art_name_work_hash(artist, title), 0,
                         out, size)
           || banner_lookup(BANNER_ALBUM, art_name_hash(title), 0, out, size);
}

bool art_banner_page(uint32_t name_key, uint32_t folder_key, char *out,
                     size_t size)
{
    return name_key
           && banner_lookup(BANNER_OTHER,
                            banner_other_key(name_key, folder_key), 0, out,
                            size);
}

/* ---- the moved banners ------------------------------------------------- */

struct banner_offset
{
    uint32_t key;       /* a person's art_name_hash(), an album's
                         * art_name_work_hash() */
    uint16_t top;       /* the band's top, in 1/1024ths of the height */
    uint16_t pad;
};

#define BANNER_OFFSETS_MAX 512

static struct banner_offset banner_offsets[BANNER_OFFSETS_MAX];

static int banner_offsets_load(void)
{
    struct libfile_header h;
    int fd = libfile_open(LIB_BANNER_OFFSETS_FILE, LIB_BANNER_OFFSETS_MAGIC,
                          LIB_BANNER_OFFSETS_VERSION,
                          sizeof(struct banner_offset), &h, NULL);
    int n = 0;

    if (fd < 0)
        return 0;
    if (h.count <= BANNER_OFFSETS_MAX)
    {
        ssize_t want = (ssize_t)(h.count * sizeof(struct banner_offset));

        if (read(fd, banner_offsets, want) == want)
            n = h.count;
    }
    close(fd);
    return n;
}

int art_banner_offset(unsigned int key)
{
    int n = banner_offsets_load(), i;

    for (i = 0; i < n; i++)
        if (banner_offsets[i].key == key)
            return banner_offsets[i].top;
    return -1;
}

bool art_banner_set_offset(unsigned int key, int top)
{
    struct libfile_writer w;
    struct libfile_marks m;
    int n = banner_offsets_load(), i;

    for (i = 0; i < n && banner_offsets[i].key != key; i++)
        ;
    if (top < 0)
    {
        if (i == n)
            return true;                    /* nothing to forget */
        banner_offsets[i] = banner_offsets[--n];
    }
    else
    {
        if (i == n)
        {
            if (n == BANNER_OFFSETS_MAX)
                return false;
            n++;
        }
        banner_offsets[i].key = key;
        banner_offsets[i].top = MIN(top, 1023);
        banner_offsets[i].pad = 0;
    }

    libfile_no_marks(&m);
    return libfile_begin(&w, LIB_BANNER_OFFSETS_FILE, LIB_BANNER_OFFSETS_MAGIC,
                         LIB_BANNER_OFFSETS_VERSION,
                         sizeof(struct banner_offset), &m)
           && libfile_finish(&w, libfile_write(&w, banner_offsets,
                                               n * sizeof(struct banner_offset),
                                               n));
}
