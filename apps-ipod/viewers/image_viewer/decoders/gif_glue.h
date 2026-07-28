/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/imageviewer/gif/rb_glue.h
 * Copyright (c) 2012 Marcin Bukat
 *
 * Glue between the bundled giflib and Rockbox (core build). Mem/str ops are
 * plain libc, file ops map to the core file API, and giflib's heap goes through
 * the vendored TLSF pool set up over the image buffer.
 * GNU General Public License (version 2+)
 *
 * Adapts vendored giflib to this build: memory via the TLSF pool, and the
 * file access it expects.
 ****************************************************************************/

#ifndef _GIF_GLUE_H_
#define _GIF_GLUE_H_

#include <string.h>   /* memset, memmove, memcmp, strncmp */
#include <limits.h>
#include "file.h"     /* read, close, open, O_RDONLY */
#include "../tlsf.h"  /* tlsf heap over the image buffer */

/* giflib is written against stdio-style file I/O; it passes an int fd as the
 * "stream". Map the calls it uses to the core file API. */
#define fread(ptr, size, nmemb, stream) read((stream), (ptr), (size)*(nmemb))
#define fclose(stream) close(stream)
#define fdopen(a,b) ((a))

/* giflib's dynamic allocations come out of the TLSF pool that the gif decoder
 * initialises over the image buffer (init_memory_pool). */
#define malloc(a)     tlsf_malloc((a))
#define free(a)       tlsf_free((a))
#define realloc(a, b) tlsf_realloc((a),(b))
#define calloc(a,b)   tlsf_calloc((a),(b))

#ifndef SIZE_MAX
#define SIZE_MAX INT_MAX
#endif

#endif /* _GIF_GLUE_H_ */
