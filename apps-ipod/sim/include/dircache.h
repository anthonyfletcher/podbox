/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Simulator shadow of firmware/include/dircache.h.
 *
 * firmware/common/dircache.c is not built in a sim -- it needs fileobj_mgr.c,
 * file_internal.c and dir.c, all of which firmware/SOURCES keeps behind
 * PLATFORM_NATIVE -- so the real header declares nothing but two macros when
 * HAVE_DIRCACHE is off. apps-ipod/ calls the API unconditionally, so the
 * shapes are reproduced here and apps-ipod/sim/shim-dircache.c implements
 * them. See ../README.md.
 *
 * Keep the struct layouts identical to the real header. A mismatch is not a
 * build error, it is a silently wrong layout.
 *
 * This file does NOT use the real header's include guard, and includes it
 * first. Both are deliberate: firmware/include/file_internal.h includes
 * "dircache.h" from its own directory, so a translation unit that reaches
 * file_internal.h first (main.c does) has already taken the real header and
 * defined its guard. Owning a separate guard means the declarations below land
 * either way.
 ****************************************************************************/
#ifndef PODBOX_SIM_DIRCACHE_H
#define PODBOX_SIM_DIRCACHE_H

#include_next "dircache.h"

#ifndef HAVE_DIRCACHE

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef uint32_t dc_serial_t;

struct dircache_file
{
    int         idx;
    dc_serial_t serialnum;
};

struct dircache_fileref
{
    struct dircache_file dcfile;
    dc_serial_t          serialhash;
};

enum dircache_status
{
    DIRCACHE_IDLE     = 0,
    DIRCACHE_SCANNING = 1,
    DIRCACHE_READY    = 2,
};

enum dircache_search_flags
{
    DCS_FILEREF        = 0x01,
    _DCS_VERIFY_FLAG   = 0x02,
    DCS_FILEREF_VERIFY = 0x03,
    DCS_CACHED_PATH    = 0x04,
    _DCS_STORAGE_FLAG  = 0x08,
    DCS_STORAGE_PATH   = 0x0c,
    DCS_UPDATE_FILEREF = 0x10,
};

struct dircache_info
{
    enum dircache_status status;
    const char   *statusdesc;
    size_t       last_size;
    size_t       size;
    size_t       sizeused;
    size_t       size_limit;
    size_t       reserve;
    size_t       reserve_used;
    unsigned int entry_count;
    long         build_ticks;
};

void dircache_init(size_t last_size);
void dircache_wait(void);
void dircache_suspend(void);
int  dircache_resume(void);
int  dircache_enable(void);
void dircache_disable(void);
void dircache_free_buffer(void);
void dircache_fileref_init(struct dircache_fileref *dcfrefp);
ssize_t dircache_get_fileref_path(const struct dircache_fileref *dcfrefp,
                                  char *buf, size_t size);
int dircache_search(unsigned int flags, struct dircache_fileref *dcfrefp,
                    const char *path);
int dircache_fileref_cmp(const struct dircache_fileref *dcfrefp1,
                         const struct dircache_fileref *dcfrefp2);
void dircache_get_info(struct dircache_info *info);
bool dircache_is_ready(void);
int dircache_foreach_name(bool (*cb)(const char *name, int idx,
                                     unsigned int attr, void *ctx),
                          void *ctx);
ssize_t dircache_get_index_path(int idx, char *buf, size_t size);

/* The real header's !HAVE_DIRCACHE branch defines these the other way round,
 * to strip dircache arguments out of the call sites upstream guards.
 * apps-ipod/ does not guard them, so they keep their arguments. */
#undef IF_DIRCACHE
#undef IFN_DIRCACHE
#define IF_DIRCACHE(...)  __VA_ARGS__
#define IFN_DIRCACHE(...)

#endif /* !HAVE_DIRCACHE */

#endif /* PODBOX_SIM_DIRCACHE_H */
