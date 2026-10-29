/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to webp_load.c.
 ****************************************************************************/
#ifndef _WEBP_LOAD_H_
#define _WEBP_LOAD_H_

#include <sys/types.h>
#include "resize.h"

/* Decodes the lossy WebP in [pos, pos + len) of fd (len < 0: to the end of
 * the file) into bm, with the arguments and results of read_png_stream().
 * Its rows and state go in the room after the bitmap, about 36 bytes for
 * each pixel of the image's width. */
int read_webp_fd(int fd, off_t pos, off_t len, struct bitmap *bm,
                 int maxsize, int format, const struct custom_format *cformat);

#endif /* _WEBP_LOAD_H_ */
