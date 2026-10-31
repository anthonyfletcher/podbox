/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to image_load.c: one call that decodes a JPEG, PNG, GIF, WebP or
 * BMP, whole file or embedded in another, chosen by its content.
 ****************************************************************************/
#ifndef _IMAGE_LOAD_H_
#define _IMAGE_LOAD_H_

#include <stdbool.h>
#include <sys/types.h>
#include "resize.h"
#include "metadata.h"
#include "jpeg_load.h"
#include "png_load.h"

/* An image as a byte range of an open file. flags are AA_FLAG_ID3_UNSYNC or
 * AA_FLAG_VORBIS_BASE64 for embedded art, whose bytes need undoing first. */
struct image_src {
    int      fd;
    off_t    pos;     /* start of the image in the file */
    off_t    len;     /* its length, or -1 for "to end of file" */
    unsigned flags;
};

/* Memory a decode needs beyond the bitmap and the scaler's lines, for any
 * format image_load() reads */
#define IMAGE_DECODE_OVERHEAD MAX(JPEG_DECODE_OVERHEAD, \
                                  (size_t)PNG_DECODE_OVERHEAD)

/* Decodes src into bm: the same arguments and results as read_jpeg_fd().
 * Embedded BMP is refused. */
int image_load(const struct image_src *src, struct bitmap *bm, int maxsize,
               int format, const struct custom_format *cformat);

/* Whether embedded art of this mp3_albumart type is one image_load() reads */
static inline bool image_embedded_usable(int aa_type)
{
    int type = aa_type & AA_CLEAR_FLAGS_MASK;
    return type == AA_TYPE_JPG || type == AA_TYPE_PNG;
}

#endif /* _IMAGE_LOAD_H_ */
