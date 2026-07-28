/* was: apps/plugins/imageviewer/jpegp/mempool.h */
#ifndef _JPEGP_MEMPOOL_H
#define _JPEGP_MEMPOOL_H

#include <inttypes.h>
#include <sys/types.h>   /* ssize_t */

extern void init_mem_pool(const unsigned char *buf, const ssize_t buf_size);
extern void *jpegp_malloc(size_t size);
extern void *jpegp_calloc(size_t nelem, size_t elem_size);
extern ssize_t freeze_mem_pool(void);
extern void clear_mem_pool(void);

#endif /* _JPEGP_MEMPOOL_H */
