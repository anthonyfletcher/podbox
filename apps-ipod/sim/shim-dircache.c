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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "string-extra.h"     /* strlcpy */
#include "file.h"             /* MAX_PATH */
#include "dir.h"              /* the walk the shim reads instead of a cache */
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


/** Walking the whole cache **/

/* The one part of this file that does not report an empty cache. A screen
 * built on the walk would otherwise be missing from the simulator entirely --
 * not degraded, absent -- and it is a screen, so the simulator is where it is
 * looked at. The walk reads simdisk instead, which is small enough that the
 * cost the real one is designed around does not arise.
 *
 * Indexes are 1-based, as the real cache's are, and name a slot in the table
 * the walk fills. They mean nothing after the next walk, which is the same
 * lifetime the real ones have. */
#define SHIM_MAX_ENTRIES 20000
#define SHIM_MAX_DEPTH   15

static char **shim_paths;
static int    shim_count;

static void shim_free_paths(void)
{
    for (int i = 0; i < shim_count; i++)
        free(shim_paths[i]);
    shim_count = 0;
}

/* Records 'path' and returns its 1-based index, or 0 if the table is full. */
static int shim_record(const char *path)
{
    if (shim_count >= SHIM_MAX_ENTRIES)
        return 0;

    if (!shim_paths)
    {
        shim_paths = malloc(SHIM_MAX_ENTRIES * sizeof (*shim_paths));
        if (!shim_paths)
            return 0;
    }

    shim_paths[shim_count] = strdup(path);
    if (!shim_paths[shim_count])
        return 0;

    return ++shim_count;
}

bool dircache_is_ready(void)
{
    return true;
}

/* Depth-first, mirroring the order a real build produces. Returns false once
 * the callback has asked to stop, which unwinds the recursion. */
static bool shim_walk(char *path, int depth,
                      bool (*cb)(const char *name, int idx,
                                 unsigned int attr, void *ctx),
                      void *ctx, int *count)
{
    DIR *dir;
    struct dirent *entry;
    size_t len = strlen(path);

    if (depth > SHIM_MAX_DEPTH)
        return true;

    dir = opendir(path);
    if (!dir)
        return true;

    while ((entry = readdir(dir)))
    {
        struct dirinfo info;
        bool go_on;

        if (entry->d_name[0] == '.')
            continue;

        if (len + 1 + strlen((char *)entry->d_name) >= MAX_PATH)
            continue;
        snprintf(path + len, MAX_PATH - len, "%s%s",
                 len > 1 ? "/" : "", (char *)entry->d_name);

        info = dir_get_info(dir, entry);

        (*count)++;
        go_on = cb((char *)entry->d_name, shim_record(path), info.attribute,
                   ctx);

        if (go_on && (info.attribute & ATTR_DIRECTORY))
            go_on = shim_walk(path, depth + 1, cb, ctx, count);

        path[len] = '\0';

        if (!go_on)
        {
            closedir(dir);
            return false;
        }
    }

    closedir(dir);
    return true;
}

int dircache_foreach_name(bool (*cb)(const char *name, int idx,
                                     unsigned int attr, void *ctx),
                          void *ctx)
{
    char path[MAX_PATH] = "/";
    int count = 0;

    shim_free_paths();
    shim_walk(path, 0, cb, ctx, &count);

    return count;
}

ssize_t dircache_get_index_path(int idx, char *buf, size_t size)
{
    if (idx < 1 || idx > shim_count)
        return -1;

    return strlcpy(buf, shim_paths[idx - 1], size);
}

#endif /* SIMULATOR */
