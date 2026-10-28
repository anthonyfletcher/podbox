/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Byte streams with a fixed memory cost: a bump arena, a pull stream, a
 * stream over a byte range of a file, and the stages that undo an encoding
 * on the way through -- DEFLATE/zlib, ID3 unsynchronisation, base64. The
 * text viewer's extraction engine is built on these, and so is anything that
 * reads compressed or encoded data out of another file.
 ****************************************************************************/
#ifndef BYTE_STREAM_H
#define BYTE_STREAM_H

#include <stddef.h>
#include <stdint.h>

typedef long long bs_off_t;   /* map to off_t on Rockbox targets */

/* ---- errors ---------------------------------------------------------- */

enum {
    BS_OK          =  0,
    BS_ERR_IO      = -1,   /* read/seek failed                            */
    BS_ERR_NOMEM   = -2,   /* arena too small                             */
    BS_ERR_FORMAT  = -3,   /* file is corrupt or not what it claimed      */
    BS_ERR_UNSUP   = -4,   /* recognised, but this variant isn't handled  */
    BS_ERR_INVAL   = -5    /* bad argument                                */
};

/* ---- I/O vtable ------------------------------------------------------ */

/* How a stream reaches a file. ts_io_core() (viewers/text_viewer/ts_io_core.c)
 * implements it over the core file API. */
typedef struct bs_io {
    long      (*read )(void *ctx, void *buf, size_t n);      /* <0 on error  */
    bs_off_t  (*seek )(void *ctx, bs_off_t off, int whence); /* SEEK_* codes */
    bs_off_t  (*size )(void *ctx);
    void      (*close)(void *ctx);
    void       *ctx;
} bs_io;

/* ---- bump arena ------------------------------------------------------ */

typedef struct {
    uint8_t *base;
    size_t   size, used;
} bs_arena;

void *bs_alloc(bs_arena *a, size_t n);          /* pointer-aligned, zeroed */

/* ---- byte stream ----------------------------------------------------- */

/* pull() fills up to n bytes. *out == 0 means clean EOF. Returns BS_OK or a
 * negative error. Streams are chained; each stage owns bounded state in the
 * arena, so a whole pipeline costs a fixed number of kilobytes regardless of
 * how large the data is. */
typedef struct bs_stream {
    int (*pull)(struct bs_stream *s, uint8_t *buf, size_t n, size_t *out);
    int (*reset)(struct bs_stream *s);          /* may be NULL */
    void *st;
} bs_stream;

static inline int bs_pull(bs_stream *s, uint8_t *b, size_t n, size_t *o)
{
    *o = 0;
    return s->pull(s, b, n, o);
}

/* Exactly n bytes, or BS_ERR_FORMAT if the stream ends first */
int bs_read(bs_stream *s, void *buf, size_t n);
int bs_skip(bs_stream *s, unsigned long n);

/* ---- file-backed stream --------------------------------------------- */

/* Reads [start, start+len) of the file. len < 0 means "to end of file". */
bs_stream *bs_file_stream(bs_arena *a, const bs_io *io,
                          bs_off_t start, bs_off_t len);
int bs_file_stream_retarget(bs_stream *s, bs_off_t start, bs_off_t len);

/* ---- inflate stage --------------------------------------------------- */

#define BS_INFLATE_RAW  0
#define BS_INFLATE_ZLIB 1
bs_stream *bs_inflate_stream(bs_arena *a, bs_stream *src, int wrapper);
int  bs_inflate_finished(const bs_stream *s);
void bs_inflate_restart(bs_stream *s, int wrapper);   /* reuse, no realloc */
/* Bytes of the source consumed so far -- lets the PDF scanner resume after a
 * stream whose /Length was an indirect reference. */
bs_off_t bs_inflate_src_used(const bs_stream *s);

/* ---- decoding stages ------------------------------------------------- */

/* Drops the 0x00 that ID3 unsynchronisation puts after every 0xFF. */
bs_stream *bs_unsync_stream(bs_arena *a, bs_stream *src);
/* Decodes base64, skipping whitespace and line breaks. The first '=' ends
 * the data; any other character outside the alphabet is BS_ERR_FORMAT. */
bs_stream *bs_base64_stream(bs_arena *a, bs_stream *src);

#endif /* BYTE_STREAM_H */
