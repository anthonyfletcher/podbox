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

    switch (kind)
    {
    case IMAGE_PNG:
    case IMAGE_GIF:
        a.used = 0;
        s = image_stream(&a, src);
        if (!s || bs_skip(s, at) != BS_OK)
            return -1;
        return (kind == IMAGE_PNG ? read_png_stream : read_gif_stream)
                   (s, &a, bm, a.base - (uint8_t *)bm->data, format, cformat);
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
        /* and anything unrecognised, as before there was a choice */
        lseek(src->fd, src->pos, SEEK_SET);
        return clip_jpeg_fd(src->fd, src->flags, src->len < 0 ? 0 : src->len,
                            bm, maxsize, format, cformat);
    }
}
