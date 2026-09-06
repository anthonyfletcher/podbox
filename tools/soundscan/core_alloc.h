/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Stub: the allocator, as plain functions.
 *
 * The tree's core_alloc.h makes core_get_data() a static inline over a buflib
 * context -- it indexes core_ctx's handle table directly. That cannot be
 * stood in for by defining a function of the same name: the inline wins at
 * every call site, and reads a context this tool does not have. The result is
 * a segfault on the first lookup, or worse if the dummy context happens to be
 * big enough to index into.
 *
 * So the declarations are replaced rather than the definitions, and stubs.c
 * supplies malloc-backed versions. -I. comes first in the include list, which
 * is what makes this header the one that is found.
 ****************************************************************************/

#ifndef SOUNDSCAN_CORE_ALLOC_H
#define SOUNDSCAN_CORE_ALLOC_H

#include <stddef.h>

int    core_alloc(size_t size);
int    core_alloc_ex(size_t size, void *ops);
void  *core_get_data(int handle);
int    core_free(int handle);
void   core_pin(int handle);
void   core_unpin(int handle);
size_t core_allocatable(void);

#endif /* SOUNDSCAN_CORE_ALLOC_H */
