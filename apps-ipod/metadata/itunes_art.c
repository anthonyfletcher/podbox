/***************************************************************************
 * GNU General Public License (version 2+)
 * Read-only iTunesDB -> ArtworkDB -> ithmb adapter. Record layouts and RGB565
 * format identifiers follow libgpod's db-itunes-parser.h and itdb_device.c.
 * Every child is bounded by its parent; no file or directory is inferred from
 * a track's containing folder.
 ****************************************************************************/
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "file.h"
#include "kernel.h"
#include "string-extra.h"
#include "core_alloc.h"
#include "database/path_key.h"
#include "draw/bmp.h"
#include "itunes_art.h"

#ifndef ITUNES_CONTROL
#define ITUNES_CONTROL "/iPod_Control"
#endif

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint64_t le64(const unsigned char *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

/* Small field and UTF-16 reads share a block instead of seeking per word. */
static unsigned char readbuf[4096];
static int readfd = -1;
static off_t readpos;
static ssize_t readlen;
static bool read_at(int fd, off_t at, void *buf, size_t n)
{
    unsigned char *out = buf;
    if (at < 0)
        return false;
    while (n)
    {
        if (readfd != fd || at < readpos || at - readpos >= readlen)
        {
            readfd = fd;
            readpos = at;
            if (lseek(fd, at, SEEK_SET) != at)
                return false;
            readlen = read(fd, readbuf, sizeof(readbuf));
            if (readlen <= 0)
                return false;
        }
        size_t take = MIN(n, (size_t)(readlen - (at - readpos)));
        memcpy(out, readbuf + (at - readpos), take);
        at += take;
        out += take;
        n -= take;
    }
    return true;
}

struct record
{
    off_t at, body, end;
    unsigned char h[40];
};

static bool record_at(int fd, off_t at, off_t end, const char *kind,
                      struct record *r, bool list)
{
    memset(r, 0, sizeof(*r));
    if (at < 0 || at > end || end - at < 12 || !read_at(fd, at, r->h, 12) ||
        memcmp(r->h, kind, 4))
        return false;
    uint32_t header = le32(r->h + 4);
    uint32_t size = list ? (uint32_t)(end - at) : le32(r->h + 8);
    if (header < 12 || size < header || size > (uint64_t)(end - at))
        return false;
    r->at = at;
    r->body = at + header;
    r->end = at + size;
    size_t n = MIN(header, sizeof(r->h));
    return read_at(fd, at, r->h, n);
}

/* UTF-16LE database strings are converted to UTF-8, including surrogate pairs.
 */
static bool string_at(int fd, off_t at, unsigned bytes, bool wide, char *out,
                      size_t cap)
{
    size_t used = 0;
    if (bytes > 4096 || (wide && (bytes & 1)))
        return false;
    while (bytes)
    {
        unsigned char b[4];
        unsigned unit = wide ? 2 : 1;
        if (!read_at(fd, at, b, unit))
            return false;
        at += unit;
        bytes -= unit;
        uint32_t c = wide ? b[0] | b[1] << 8 : b[0];
        if (!c)
            break;
        if (wide && c >= 0xd800 && c <= 0xdbff)
        {
            if (bytes < 2 || !read_at(fd, at, b, 2))
                return false;
            uint32_t low = b[0] | b[1] << 8;
            if (low < 0xdc00 || low > 0xdfff)
                return false;
            c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
            bytes -= 2;
            at += 2;
        }
        if (wide && c >= 0xdc00 && c <= 0xdfff)
            return false;
        if (!wide || c < 128)
        {
            if (used + 1 >= cap)
                return false;
            out[used++] = c == ':' ? '/' : c;
        }
        else
        {
            unsigned n = c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
            if (used + n >= cap)
                return false;
            for (unsigned j = n - 1; j; j--)
            {
                out[used + j] = 0x80 | (c & 63);
                c >>= 6;
            }
            out[used] = (n == 2 ? 0xc0 : n == 3 ? 0xe0 : 0xf0) | c;
            used += n;
        }
    }
    out[used] = 0;
    return true;
}

struct track_key
{
    uint64_t hash, dbid;
    off_t text;
    unsigned bytes, link;
};
struct image_key
{
    uint64_t id;
    off_t at;
    bool link;
};
static int tracks_fd = -1, images_fd = -1;
static int tracks_handle = -1, images_handle = -1;
static struct track_key *tracks;
static struct image_key *images;
static unsigned track_count, image_count;
static off_t images_end;

void itunes_art_close(void)
{
    if (tracks_handle > 0)
    {
        core_unpin(tracks_handle);
        core_free(tracks_handle);
    }
    if (images_handle > 0)
    {
        core_unpin(images_handle);
        core_free(images_handle);
    }
    if (tracks_fd >= 0)
        close(tracks_fd);
    if (images_fd >= 0)
        close(images_fd);
    tracks_handle = images_handle = tracks_fd = images_fd = -1;
    tracks = NULL;
    images = NULL;
    track_count = image_count = 0;
    readfd = -1;
    readlen = 0;
}

static int compare_tracks(const void *a, const void *b)
{
    uint64_t x = ((const struct track_key *)a)->hash;
    uint64_t y = ((const struct track_key *)b)->hash;
    return (x > y) - (x < y);
}
static int compare_images(const void *a, const void *b)
{
    const struct image_key *x = a, *y = b;
    if (x->link != y->link)
        return x->link ? 1 : -1;
    return (x->id > y->id) - (x->id < y->id);
}

static bool find_list(int fd, const char *kind, const char *listkind,
                      struct record *list)
{
    struct record root, section;
    if (!record_at(fd, 0, ffilesize(fd), kind, &root, false))
        return false;
    for (off_t at = root.body; at < root.end; at = section.end)
    {
        if (!record_at(fd, at, root.end, "mhsd", &section, false)
            || section.body - section.at < 16)
            return false;
        if (le32(section.h + 12) == 1)
            return record_at(fd, section.body, section.end, listkind,
                              list, true);
    }
    return false;
}

/* The advertised count bounds allocation and every append. No source file
 * is traversed again to size its index. Malformed counts fail closed. */
bool itunes_art_open(bool (*stop)(void))
{
    struct record list, track, text, image;
    unsigned capacity;
    char path[MAX_PATH];
    itunes_art_close();
    images_fd = open(ITUNES_ARTWORK_DB, O_RDONLY);
    if (images_fd < 0 || !find_list(images_fd, "mhfd", "mhli", &list))
        goto fail;
    images_end = list.end;
    capacity = le32(list.h + 8);
    if (!capacity)
    {
        if (list.body != list.end || (stop && stop()))
            goto fail;
        itunes_art_close();
        return true;
    }
    if (capacity > (uint64_t)(list.end - list.body) / 28 ||
        capacity > INT_MAX / (2 * sizeof(*images)))
        goto fail;
    if (core_allocatable() < (size_t)capacity * 2 * sizeof(*images))
        goto fail;
    images_handle = core_alloc((size_t)capacity * 2 * sizeof(*images));
    if (images_handle <= 0)
        goto fail;
    images = core_get_data_pinned(images_handle);
    for (off_t at = list.body; at < list.end; at = image.end)
    {
        if (stop && stop())
            goto fail;
        if (!record_at(images_fd, at, list.end, "mhii", &image, false) ||
            image.body - image.at < 28 || image_count / 2 == capacity)
            goto fail;
        images[image_count++] = (struct image_key){le64(image.h + 20), at, false};
        images[image_count++] = (struct image_key){le32(image.h + 16), at, true};
        yield();
    }
    qsort(images, image_count, sizeof(*images), compare_images);
    tracks_fd = open(ITUNES_CONTROL "/iTunes/iTunesDB", O_RDONLY);
    if (tracks_fd < 0 || !find_list(tracks_fd, "mhbd", "mhlt", &list))
        goto fail;
    capacity = le32(list.h + 8);
    if (!capacity || capacity > (uint64_t)(list.end - list.body) / 0x78 ||
        capacity > INT_MAX / sizeof(*tracks))
        goto fail;
    if (core_allocatable() < (size_t)capacity * sizeof(*tracks))
        goto fail;
    tracks_handle = core_alloc((size_t)capacity * sizeof(*tracks));
    if (tracks_handle <= 0)
        goto fail;
    tracks = core_get_data_pinned(tracks_handle);
    for (off_t at = list.body; at < list.end; at = track.end)
    {
        unsigned char ids[8];
        if (stop && stop())
            goto fail;
        if (!record_at(tracks_fd, at, list.end, "mhit", &track, false) ||
            track.body - track.at < 0x78)
            goto fail;
        for (off_t q = track.body; q < track.end; q = text.end)
        {
            if (!record_at(tracks_fd, q, track.end, "mhod", &text, false) ||
                text.body - text.at < 16)
                goto fail;
            if (le32(text.h + 12) != 2 || text.end - text.body < 16)
                continue;
            unsigned char h[16];
            if (!read_at(tracks_fd, text.body, h, sizeof(h)))
                goto fail;
            unsigned bytes = le32(h + 4);
            if (bytes > (uint64_t)(text.end - text.body - 16) ||
                !string_at(tracks_fd, text.body + 16, bytes, true,
                            path, sizeof(path)))
                continue;
            if (track_count == capacity ||
                !read_at(tracks_fd, track.at + 0x70, ids, 8))
                goto fail;
            struct track_key *key = &tracks[track_count++];
            key->hash = path_key(path);
            key->dbid = le64(ids);
            key->text = text.body + 16;
            key->bytes = bytes;
            key->link = 0;
            if (track.body - track.at >= 0x164)
            {
                if (!read_at(tracks_fd, track.at + 0x160, ids, 4))
                    goto fail;
                key->link = le32(ids);
            }
            break;
        }
        yield();
    }
    qsort(tracks, track_count, sizeof(*tracks), compare_tracks);
    return true;
fail:
    itunes_art_close();
    return false;
}

static bool thumbnail(int fd, const struct record *image,
                      struct itunes_art *src)
{
    struct record data, thumb, name;
    bool found = false;
    for (off_t at = image->body; at < image->end; at = data.end)
    {
        if (!record_at(fd, at, image->end, "mhod", &data, false))
            return false;
        if (data.body - data.at < 16 ||
            (data.h[12] | data.h[13] << 8) != 2 ||
            !record_at(fd, data.body, data.end, "mhni", &thumb, false))
            continue;
        if (thumb.body - thumb.at < 36)
            continue;
        unsigned format = le32(thumb.h + 16), dim;
        switch (format)
        {
        case 1061:
            dim = 56;
            break;
        case 1055:
            dim = 128;
            break;
        case 1060:
            dim = 320;
            break;
        default:
            continue;
        }
        unsigned height = thumb.h[32] | thumb.h[33] << 8;
        unsigned width = thumb.h[34] | thumb.h[35] << 8;
        unsigned top = thumb.h[28] | thumb.h[29] << 8;
        unsigned left = thumb.h[30] | thumb.h[31] << 8;
        unsigned bytes = le32(thumb.h + 24);
        if (!width || !height || width + left > dim || height + top > dim ||
            bytes < dim * dim * 2 || (found && src->width >= width))
            continue;
        if (!record_at(fd, thumb.body, thumb.end, "mhod", &name, false) ||
            (name.h[12] | name.h[13] << 8) != 3 || name.body - name.at < 36)
            continue;
        unsigned n = le32(name.h + 24);
        char filename[MAX_PATH];
        if (n > (uint64_t)(name.end - name.at - 36) ||
            !string_at(fd, name.at + 36, n, name.h[28] == 2, filename,
                       sizeof(filename)))
            continue;
        const char *base = filename[0] == '/' ? filename + 1 : filename;
        if (!*base || strchr(base, '/') || strstr(base, ".."))
            continue;
        char candidate[MAX_PATH];
        if (snprintf(candidate, sizeof(candidate), ITUNES_CONTROL "/Artwork/%s",
                     base) >= (int)sizeof(candidate))
            continue;
        int pixels = open(candidate, O_RDONLY);
        off_t pos = le32(thumb.h + 20);
        bool valid = pixels >= 0 && pos >= 0 && pos <= ffilesize(pixels) &&
                     bytes <= (uint64_t)(ffilesize(pixels) - pos);
        if (pixels >= 0)
            close(pixels);
        if (!valid)
            continue;
        strlcpy(src->path, candidate, sizeof(src->path));
        src->pos = pos + (top * dim + left) * 2;
        src->width = width;
        src->height = height;
        src->stride = dim;
        found = true;
    }
    return found;
}

bool itunes_art_find(const char *path, struct itunes_art *src)
{
    if (!path || !tracks || !images)
        return false;
    struct track_key wanted = { .hash = path_key(path) };
    unsigned lo = 0, hi = track_count;
    while (lo < hi)
    {
        unsigned mid = lo + (hi - lo) / 2;
        if (tracks[mid].hash < wanted.hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    struct track_key *key = tracks + lo;
    for (; key < tracks + track_count && key->hash == wanted.hash; key++)
    {
        char candidate[MAX_PATH];
        if (!string_at(tracks_fd, key->text, key->bytes, true,
                        candidate, sizeof(candidate)) ||
            strcasecmp(path_key_strip(path), path_key_strip(candidate)))
            continue;
        struct image_key id = { .id = key->link ? key->link : key->dbid,
                                .link = key->link != 0 };
        if (!id.id)
            continue;
        lo = 0;
        hi = image_count;
        while (lo < hi)
        {
            unsigned mid = lo + (hi - lo) / 2;
            if (compare_images(&images[mid], &id) < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        struct image_key *found = images + lo;
        if (lo < image_count && compare_images(found, &id) == 0)
        {
            struct record image;
            return record_at(images_fd, found->at, images_end, "mhii",
                              &image, false) && thumbnail(images_fd, &image, src);
        }
    }
    return false;
}

int itunes_art_decode(const struct itunes_art *art, struct bitmap *bm,
                      int maxsize, int format,
                      const struct custom_format *cformat)
{
    struct dim source = {art->width, art->height};
    if (!source.width || !source.height || source.width > 320 ||
        source.height > 320 || art->stride < art->width || art->stride > 320)
        return -1;
    if (!(format & FORMAT_RESIZE))
    {
        bm->width = source.width;
        bm->height = source.height;
    }
    else if (format & FORMAT_KEEP_ASPECT)
    {
        struct dim target = {bm->width, bm->height};
        recalc_dimension(&target, &source);
        bm->width = target.width;
        bm->height = target.height;
    }
    if (maxsize < 0 || bm->width <= 0 || bm->height <= 0 ||
        bm->width > 320 || bm->height > 320 ||
        (size_t)bm->width * bm->height * FB_DATA_SZ > (size_t)maxsize)
        return -1;
    int bytes = bm->width * bm->height * FB_DATA_SZ;
    if (format & FORMAT_RETURN_SIZE)
        return bytes;
    int fd = open(art->path, O_RDONLY);
    if (fd < 0)
        return -1;
    unsigned char row[640];
    for (int y = 0; y < bm->height; y++)
    {
        off_t at = art->pos + (off_t)(y * art->height / bm->height) *
                             art->stride * 2;
        if (!image_progress(cformat, y, bm->height) ||
            lseek(fd, at, SEEK_SET) != at ||
            read(fd, row, art->width * 2) != (ssize_t)(art->width * 2))
        {
            bytes = -1;
            break;
        }
        fb_data *out = (fb_data *)bm->data + (size_t)y * bm->width;
        for (int x = 0; x < bm->width; x++)
        {
            int sx = x * art->width / bm->width;
            unsigned px = row[sx * 2] | row[sx * 2 + 1] << 8;
            out[x] = LCD_RGBPACK(((px >> 11) & 31) * 255 / 31,
                                 ((px >> 5) & 63) * 255 / 63,
                                 (px & 31) * 255 / 31);
        }
        yield();
    }
    close(fd);
    return bytes;
}
