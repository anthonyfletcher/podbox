/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to gif_load.c.
 ****************************************************************************/
#ifndef _GIF_LOAD_H_
#define _GIF_LOAD_H_

#include "resize.h"
#include "files/byte_stream.h"

/* Decodes the first frame of the GIF that `in` yields, from its signature
 * on, into bm, with the arguments and results of read_png_stream(). Its
 * state goes in `a`, which must not overlap the first maxsize bytes of
 * bm->data; an interlaced image is put together in the room after the
 * bitmap. A transparent pixel is drawn over black. */
int read_gif_stream(bs_stream *in, bs_arena *a, struct bitmap *bm,
                    int maxsize, int format,
                    const struct custom_format *cformat);

#endif /* _GIF_LOAD_H_ */
