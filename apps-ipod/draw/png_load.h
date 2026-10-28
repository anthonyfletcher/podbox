/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to png_load.c.
 ****************************************************************************/
#ifndef _PNG_LOAD_H_
#define _PNG_LOAD_H_

#include <stdbool.h>
#include "resize.h"
#include "files/byte_stream.h"

/* The longest row the reader takes, filter byte included: 6826 pixels of
 * 8-bit RGB, 5119 of RGBA, 2559 of 16-bit RGBA. */
#define PNG_MAX_ROW_BYTES (20 * 1024)

/* The arena read_png_stream() needs: the inflater (about 35 KiB), its own
 * state and two rows. */
#define PNG_DECODE_OVERHEAD (38 * 1024 + 2 * PNG_MAX_ROW_BYTES)

/* Decodes the PNG that `in` yields, from its signature on, into bm, with the
 * arguments and results of read_jpeg_fd(). Its state goes in `a`, which must
 * not overlap the first maxsize bytes of bm->data. An interlaced image is
 * read only as far as the target size needs, at a size that fits the rest
 * of the buffer. A transparent pixel is drawn over black. */
int read_png_stream(bs_stream *in, bs_arena *a, struct bitmap *bm,
                    int maxsize, int format,
                    const struct custom_format *cformat);

/* Whether fd holds, from its start, a PNG that read_png_stream() decodes.
 * Reads the signature and the header only. */
bool png_usable(int fd);

#endif /* _PNG_LOAD_H_ */
