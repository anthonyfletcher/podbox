/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/jpegp/rb_glue.h
 * Glue for the bundled progressive-JPEG (RAINBOW) decoder, core build.
 * The decoder allocates from a simple bump pool over the image buffer; its
 * malloc/calloc are remapped to the pool so the firmware allocator is untouched.
 * GNU General Public License (version 2+)
 *
 * Adapts the vendored jpeg81 decoder to this build.
 ****************************************************************************/

#ifndef _JPEGP_GLUE_H_
#define _JPEGP_GLUE_H_

#include <string.h>
#include "file.h"       /* read/lseek/open/close, O_RDONLY */
#include "mempool.h"    /* jpegp_malloc/jpegp_calloc + pool control */

/* a define from rbunicode.h would clash with jpeg81.h's struct COMP */
#undef COMP

/* the decoder's dynamic allocations come from the bump pool */
#define malloc(a)     jpegp_malloc((a))
#define calloc(a,b)   jpegp_calloc((a),(b))

/* the decoder's debug printf goes nowhere in a normal build */
#undef printf
#define printf(...)

#endif /* _JPEGP_GLUE_H_ */
