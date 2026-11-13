/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The one entry point for decoding album art: reads the image's first bytes,
 * with any ID3 unsync or Vorbis base64 undone, and hands it to the JPEG, PNG,
 * GIF, WebP or BMP reader by what those bytes are rather than by the file's
 * name or the tag's MIME type. WebP, like BMP, is read from a file only,
 * never from inside a tag.
 *
 *   sources     - the image's bytes as a stream: a byte range of the file,
 *                 through the unsync stage, or Ogg packets through base64
 *   image_load  - sniffs, then dispatches
 *
 * The PNG and GIF readers' state, and the stream they read, sit at the end
 * of the caller's buffer; the bitmap and the scaler's lines take the rest.
 ****************************************************************************/

#include <string.h>
#include "system.h"
#include "file.h"
#include "metadata_common.h"
#include "bmp.h"
#include "jpeg_load.h"
#include "png_load.h"
#include "gif_load.h"
#include "webp_load.h"
#include "image_load.h"

/* vorbis.c rounds a base64 picture's position up to a whole group of four
 * characters, which can land up to two bytes into the image. One group
 * earlier, the image starts 1-3 bytes into what is decoded. */
#define B64_BACK 4

/* id3tags.c places an unsynchronised image at its frame's start plus the
 * frame header's decoded length, so a header unsync added a zero to (a
 * UTF-16 description's byte order mark) leaves the image a little further
 * on. It is looked for in the first UNSYNC_SLACK bytes. The length counts
 * decoded bytes, so it does not limit the raw read either. */
#define UNSYNC_SLACK 64

enum image_kind {
    IMAGE_UNKNOWN, IMAGE_JPEG, IMAGE_PNG, IMAGE_GIF, IMAGE_WEBP, IMAGE_BMP
};

/* ---- sources --------------------------------------------------------- */

struct fd_st {
    int   fd;
    off_t left;                 /* < 0: to the end of the file */
    off_t start, len;           /* where it began, and left then */
};

static int fd_pull(bs_stream *s, uint8_t *buf, size_t n, size_t *out)
{
    struct fd_st *f = s->st;
    ssize_t got;

    if (f->left >= 0 && (off_t)n > f->left)
        n = f->left;
    if (!n)
        return BS_OK;
    got = read(f->fd, buf, n);
    if (got < 0)
        return BS_ERR_IO;
    if (f->left >= 0)
        f->left -= got;
    *out = got;
    return BS_OK;
}

/* Back to the picture's first byte, for a decoder that reads it twice */
static int fd_reset(bs_stream *s)
{
    struct fd_st *f = s->st;

    if (lseek(f->fd, f->start, SEEK_SET) != f->start)
        return BS_ERR_IO;
    f->left = f->len;
    return BS_OK;
}

struct ogg_st {
    struct ogg_file ogg;
    off_t left;
};

static int ogg_pull(bs_stream *s, uint8_t *buf, size_t n, size_t *out)
{
    struct ogg_st *o = s->st;
    ssize_t got;

    if (o->left >= 0 && (off_t)n > o->left)
        n = o->left;
    if (!n)
        return BS_OK;
    got = ogg_file_read(&o->ogg, buf, n);
    if (got < 0)
        return BS_ERR_IO;
    if (o->left >= 0)
        o->left -= got;
    *out = got;
    return BS_OK;
}

/* The bytes of src, decoded, from B64_BACK characters early for base64 */
static bs_stream *image_stream(bs_arena *a, const struct image_src *src)
{
    bs_stream *s = bs_alloc(a, sizeof *s);

    if (!s)
        return NULL;
    s->reset = NULL;
    if (src->flags & AA_FLAG_VORBIS_BASE64)
    {
        struct ogg_st *o = bs_alloc(a, sizeof *o);
        unsigned char *probe = bs_alloc(a, 92);
        off_t pic = src->pos - B64_BACK;
        bool found;

        if (!o || !probe)
            return NULL;
        /* to the picture through the Ogg pages, as clip_jpeg_fd() goes */
        ogg_file_init(&o->ogg, src->fd,
                      get_ogg_format_and_move_to_comments(src->fd, probe), 0);
        do
        {
            off_t ahead = pic - lseek(src->fd, 0, SEEK_CUR);
            found = ahead <= o->ogg.packet_remaining;
            if (ogg_file_read(&o->ogg, NULL,
                              found ? ahead : o->ogg.packet_remaining) < 0)
                return NULL;
        } while (!found);
        o->left = src->len < 0 ? -1 : src->len + B64_BACK;
        s->pull = ogg_pull;
        s->st = o;
        return bs_base64_stream(a, s);
    }

    struct fd_st *f = bs_alloc(a, sizeof *f);
    if (!f || lseek(src->fd, src->pos, SEEK_SET) != src->pos)
        return NULL;
    f->fd = src->fd;
    f->left = (src->flags & AA_FLAG_ID3_UNSYNC) ? -1 : src->len;
    f->start = src->pos;
    f->len = f->left;
    s->pull = fd_pull;
    s->reset = fd_reset;
    s->st = f;
    if (src->flags & AA_FLAG_ID3_UNSYNC)
        return bs_unsync_stream(a, s);
    return s;
}

/* ---- image_load ------------------------------------------------------ */

/* What the first bytes are, and at which of them it starts */
static enum image_kind sniff(const uint8_t *b, int tries, int *at)
{
    static const uint8_t png_sig[8] =
        { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

    if (b[0] == 'B' && b[1] == 'M')
    {
        *at = 0;
        return IMAGE_BMP;
    }
    for (*at = 0; *at < tries; (*at)++)
    {
        const uint8_t *p = b + *at;
        if (p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff)
            return IMAGE_JPEG;
        if (!memcmp(p, png_sig, sizeof png_sig))
            return IMAGE_PNG;
        if (!memcmp(p, "GIF8", 4))
            return IMAGE_GIF;
        if (!memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4))
            return IMAGE_WEBP;
    }
    return IMAGE_UNKNOWN;
}

/* APP1 is parsed as a bounded TIFF directory, without keeping the segment. */
static uint16_t exif_u16(const uint8_t *p, bool le)
{
    return le ? p[0] | p[1] << 8 : p[0] << 8 | p[1];
}

static uint32_t exif_u32(const uint8_t *p, bool le)
{
    return le ? (uint32_t)p[0] | (uint32_t)p[1] << 8 |
                (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24
              : (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                (uint32_t)p[2] << 8 | p[3];
}

static int jpeg_orientation(bs_stream *s, int skip)
{
    uint8_t b[14];
    if (bs_skip(s, skip) != BS_OK || bs_read(s, b, 2) != BS_OK ||
        b[0] != 0xff || b[1] != 0xd8)
        return 1;
    for (int marker = 0; marker < 256; marker++)
    {
        if (bs_read(s, b, 2) != BS_OK || b[0] != 0xff)
            return 1;
        while (b[1] == 0xff)
            if (bs_read(s, b + 1, 1) != BS_OK)
                return 1;
        int kind = b[1];
        if (kind == 0xda || kind == 0xd9)
            return 1;
        if (kind == 1 || (kind >= 0xd0 && kind <= 0xd7))
            continue;
        if (bs_read(s, b, 2) != BS_OK)
            return 1;
        unsigned left = (b[0] << 8) | b[1];
        if (left < 2)
            return 1;
        left -= 2;
        if (kind == 0xe1 && left >= sizeof(b))
        {
            if (bs_read(s, b, sizeof(b)) != BS_OK)
                return 1;
            left -= sizeof(b);
            bool le = b[6] == 'I' && b[7] == 'I';
            if (!memcmp(b, "Exif\0\0", 6) &&
                (le || (b[6] == 'M' && b[7] == 'M')) &&
                exif_u16(b + 8, le) == 42)
            {
                uint32_t offset = exif_u32(b + 10, le);
                if (offset < 8 || offset - 8 > left ||
                    left - (offset - 8) < 2 ||
                    bs_skip(s, offset - 8) != BS_OK ||
                    bs_read(s, b, 2) != BS_OK)
                    return 1;
                left -= offset - 8 + 2;
                unsigned count = exif_u16(b, le);
                for (unsigned i = 0; i < count && left >= 12; i++)
                {
                    if (bs_read(s, b, 12) != BS_OK)
                        return 1;
                    left -= 12;
                    if (exif_u16(b, le) == 0x112 &&
                        exif_u16(b + 2, le) == 3 &&
                        exif_u32(b + 4, le) == 1)
                    {
                        int value = exif_u16(b + 8, le);
                        return value >= 1 && value <= 8 ? value : 1;
                    }
                }
            }
        }
        if (bs_skip(s, left) != BS_OK)
            return 1;
    }
    return 1;
}

/* Enough arena for any of image_stream()'s streams, base64 the largest:
 * where the PNG reader's does not fit, a JPEG's orientation is read with
 * this much, so a small decode is turned as its size probe said */
#define ORIENT_ARENA 1024

/* The scratch a quarter turn needs past the bitmap: one bit a pixel */
static size_t orient_marks(int w, int h)
{
    return ((size_t)w * h + 7) / 8;
}

static inline void swap_pixel(fb_data *a, fb_data *b)
{
    fb_data t = *a;
    *a = *b;
    *b = t;
}

/* The bitmap turned or mirrored in place for an EXIF orientation: 1 done,
   0 left as it is for want of room, -1 cancelled part way. A mirror or a
   half turn swaps pixels in pairs; a quarter turn follows each cycle of the
   transpose, marking where it has been in a bit a pixel past the bitmap. */
static int image_orient(struct bitmap *bm, int maxsize, int orientation,
                        const struct custom_format *format)
{
    const int w = bm->width, h = bm->height;
    const size_t count = (size_t)w * h, bytes = count * sizeof(fb_data);
    fb_data *const pixels = (fb_data *)bm->data;

    if (w < 1 || h < 1 || bytes > (size_t)maxsize)
        return 0;

    if (orientation < 5)
    {
        for (int y = 0; y < h; y++)
        {
            if (!image_progress(format, y, h))
                return -1;
            fb_data *a = pixels + (size_t)y * w, *b;
            switch (orientation)
            {
            case 2:                         /* each row end for end */
                for (b = a + w - 1; a < b; a++, b--)
                {
                    swap_pixel(a, b);
                }
                break;
            case 3:                         /* the whole, end for end */
                if (2 * y >= h)
                    return 1;
                b = pixels + (size_t)(h - 1 - y) * w + w - 1;
                if (2 * y + 1 == h)         /* the middle row with itself */
                    for (; a < b; a++, b--)
                    {
                        swap_pixel(a, b);
                    }
                else
                    for (int x = 0; x < w; x++, a++, b--)
                    {
                        swap_pixel(a, b);
                    }
                break;
            case 4:                         /* the rows top for bottom */
                if (2 * y + 1 >= h)
                    return 1;
                b = pixels + (size_t)(h - 1 - y) * w;
                for (int x = 0; x < w; x++, a++, b++)
                {
                    swap_pixel(a, b);
                }
                break;
            default:
                return 1;
            }
        }
        return 1;
    }

    if (w == 1)
    {
        /* a column turned is a row in the same order, or the reverse */
        if (orientation == 6 || orientation == 7)
            for (fb_data *a = pixels, *b = pixels + h - 1; a < b; a++, b--)
            {
                swap_pixel(a, b);
            }
        bm->width = h;
        bm->height = 1;
        return 1;
    }

    const size_t marks = orient_marks(w, h);
    if (bytes + marks > (size_t)maxsize || (uint64_t)count * w > 0xffffffffu)
        return 0;
    uint8_t *const seen = bm->data + bytes;
    memset(seen, 0, marks);

    /* x and y of a place without dividing, which ARM does in software: a
       multiply by 2^32 / w rounded up gives at / w exactly while at * w is
       under 2^32, which every bitmap here is far inside */
    const uint32_t recip = 0xffffffffu / (uint32_t)w + 1;
    size_t row_end = 0;
    int row = 0;

    for (size_t start = 0; start < count; start++)
    {
        if (start == row_end)
        {
            if (!image_progress(format, row++, h))
                return -1;
            row_end += w;
        }
        if (seen[start >> 3] & (1u << (start & 7)))
            continue;
        size_t at = start;
        fb_data held = pixels[at];
        do
        {
            int y = (int)(((uint64_t)at * recip) >> 32);
            int x = (int)at - y * w, nx, ny;
            switch (orientation)
            {
            case 5:  nx = y;         ny = x;         break;
            case 6:  nx = h - 1 - y; ny = x;         break;
            case 7:  nx = h - 1 - y; ny = w - 1 - x; break;
            default: nx = y;         ny = w - 1 - x; break;  /* 8 */
            }
            size_t next = (size_t)ny * h + nx;
            fb_data displaced = pixels[next];
            pixels[next] = held;
            held = displaced;
            seen[at >> 3] |= 1u << (at & 7);
            at = next;
        } while (at != start);
    }
    bm->width = h;
    bm->height = w;
    return 1;
}

int image_load(const struct image_src *src, struct bitmap *bm, int maxsize,
               int format, const struct custom_format *cformat)
{
    unsigned char *end = (unsigned char *)bm->data + maxsize;
    int tries = (src->flags & AA_FLAG_VORBIS_BASE64) ? B64_BACK
                : (src->flags & AA_FLAG_ID3_UNSYNC) ? UNSYNC_SLACK : 1;
    enum image_kind kind = IMAGE_UNKNOWN;
    uint8_t head[12 + UNSYNC_SLACK];
    bs_arena a;
    bs_stream *s;
    int at = 0;

    a.base = (uint8_t *)((uintptr_t)(end - PNG_DECODE_OVERHEAD) & ~7);
    a.size = end - a.base;
    a.used = 0;
    if (maxsize >= (int)(PNG_DECODE_OVERHEAD + 8))
    {
        s = image_stream(&a, src);
        if (s && bs_read(s, head, 11 + tries) == BS_OK)
            kind = sniff(head, tries, &at);
    }

    bool jpeg = kind == IMAGE_JPEG;
    if (maxsize < (int)(PNG_DECODE_OVERHEAD + 8) &&
        maxsize >= (int)(ORIENT_ARENA + 8))
    {
        a.base = (uint8_t *)((uintptr_t)(end - ORIENT_ARENA) & ~7);
        a.size = end - a.base;
        a.used = 0;
        s = image_stream(&a, src);
        jpeg = s && bs_read(s, head, 11 + tries) == BS_OK &&
               sniff(head, tries, &at) == IMAGE_JPEG;
    }

    int orientation = 1;
    if (jpeg)
    {
        a.used = 0;
        s = image_stream(&a, src);
        if (s)
            orientation = jpeg_orientation(s, at);
    }

    switch (kind)
    {
    case IMAGE_PNG:
    case IMAGE_GIF:
        a.used = 0;
        s = image_stream(&a, src);
        if (!s || bs_skip(s, at) != BS_OK)
            return -1;
        if (kind == IMAGE_PNG)
            return read_png_stream(s, &a, bm, a.base - (uint8_t *)bm->data,
                                   format, cformat);
        return read_gif_stream(s, at, &a, bm, a.base - (uint8_t *)bm->data,
                               format, cformat);
    case IMAGE_WEBP:
        if (src->flags)
            return -1;
        return read_webp_fd(src->fd, src->pos, src->len, bm, maxsize, format,
                            cformat);
    case IMAGE_BMP:
        if (src->pos || src->flags)
            return -1;
        lseek(src->fd, 0, SEEK_SET);
        return read_bmp_fd(src->fd, bm, maxsize, format, cformat);
    default:
        /* and anything unrecognised: JPEG is the default */
        lseek(src->fd, src->pos, SEEK_SET);
        if (orientation >= 5)
        {
            int w = bm->width;
            bm->width = bm->height;
            bm->height = w;
        }
        int rc = clip_jpeg_fd(src->fd, src->flags,
                             src->len < 0 ? 0 : src->len,
                             bm, maxsize, format, cformat);
        if (rc > 0 && orientation != 1)
        {
            if (format & FORMAT_RETURN_SIZE)
            {
                if (orientation >= 5)
                {
                    int w = bm->width;
                    bm->width = bm->height;
                    bm->height = w;
                    /* and the scratch the turn needs */
                    rc += orient_marks(w, bm->width);
                }
            }
            /* without room to turn it, the image is shown as it was taken */
            else if (image_orient(bm, maxsize, orientation, cformat) < 0)
                return -1;
        }
        return rc;
    }
}
