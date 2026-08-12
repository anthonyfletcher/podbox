/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The dircache API, reporting an empty cache that never builds.
 *
 * A simulator cannot have a real one: firmware/common/dircache.c needs
 * fileobj_mgr.c, file_internal.c and dir.c, and firmware/SOURCES keeps all
 * three behind PLATFORM_NATIVE. Making it work would be a port of the internal
 * filesystem layer to hosted, not a shim.
 *
 * Every caller in apps-ipod/ treats a cache miss as "read it off the disk
 * instead" -- that is the path upstream's builds without dircache take, and
 * tagcache.c even labels it "do it the hard way". So the searches below report
 * nothing found and the callers stay correct, just slower.
 ****************************************************************************/
#include "config.h"

#ifdef SIMULATOR

#include <string.h>
#include "dircache.h"

void dircache_init(size_t last_size)
{
    (void)last_size;
}

/* Returning anything but 0 sends main.c's init_dircache() into
 * dircache_boot_wait(), which polls until it sees DIRCACHE_SCANNING and then
 * something else. This shim is never anything but IDLE, so that loop would
 * never exit and the boot bar would stop dead. 0 means "already enabled by
 * load", which skips the wait. */
int dircache_enable(void)
{
    return 0;
}

void dircache_disable(void)
{
}

void dircache_wait(void)
{
}

void dircache_suspend(void)
{
}

int dircache_resume(void)
{
    return 0;
}

void dircache_free_buffer(void)
{
}

void dircache_fileref_init(struct dircache_fileref *dcfrefp)
{
    memset(dcfrefp, 0, sizeof (*dcfrefp));
}

/* Negative is "no path for this reference". Callers fall back to opening the
 * file: playlist.c on `if (max < 0)`, tagcache.c by dropping through to
 * open_files(). */
ssize_t dircache_get_fileref_path(const struct dircache_fileref *dcfrefp,
                                  char *buf, size_t size)
{
    (void)dcfrefp;
    (void)buf;
    (void)size;
    return -1;
}

/* 0 is "not found", the same answer a real cache gives for a path it has not
 * scanned. Callers that asked for DCS_UPDATE_FILEREF simply keep the reference
 * they had. */
int dircache_search(unsigned int flags, struct dircache_fileref *dcfrefp,
                    const char *path)
{
    (void)flags;
    (void)dcfrefp;
    (void)path;
    return 0;
}

int dircache_fileref_cmp(const struct dircache_fileref *dcfrefp1,
                         const struct dircache_fileref *dcfrefp2)
{
    (void)dcfrefp1;
    (void)dcfrefp2;
    return 0;
}

void dircache_get_info(struct dircache_info *info)
{
    memset(info, 0, sizeof (*info));
    info->status = DIRCACHE_IDLE;
    info->statusdesc = "Disabled (simulator)";
}

#endif /* SIMULATOR */
