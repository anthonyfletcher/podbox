/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A cart's own stored files on the player: struct arcade_store, which
 * store.* goes through. Each is name.json in /.rockbox/games/data/<cart
 * id>/, written beside the old one and renamed over it, so a full disk
 * leaves the old one whole. The area is bounded (ARCADE_STORE_FILES,
 * ARCADE_STORE_BYTES), a file's own old bytes aside when it is replaced.
 * FAT folds case, so two names differing only in case are one file.
 ****************************************************************************/
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>                 /* qsort */
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "dir.h"
#include "games/arcade/arcade.h"
#include "games/arcade/shell.h"
#include "games/arcade/shell_store.h"

#define DATA_DIR "/.rockbox/games/data"

/* The running cart's folder */
static char folder[MAX_PATH];

void shell_store_start(const struct vm *vm)
{
    shell_id_path(folder, sizeof(folder), DATA_DIR, arcade_id(vm), "");
}

static void file_path(const char *name, char *path, size_t size)
{
    snprintf(path, size, "%s/%s.json", folder, name);
}

static int32_t st_read(void *ctx, const char *name, void *buf, int32_t cap)
{
    (void)ctx;
    char path[MAX_PATH];
    file_path(name, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    off_t n = ffilesize(fd);
    if (cap > 0)
        n = read(fd, buf, n < cap ? (size_t)n : (size_t)cap);
    close(fd);
    return n < 0 ? -1 : (int32_t)n;
}

/* The folder's .json files: their names (when names is not NULL) and the
 * count; *bytes their size in all, but for skip's */
static int scan(char (*names)[33], int max, const char *skip, long *bytes)
{
    int n = 0;
    *bytes = 0;
    DIR *d = opendir(folder);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        struct dirinfo info = dir_get_info(d, e);
        size_t len = strlen(e->d_name);
        if ((info.attribute & ATTR_DIRECTORY) || len <= 5 || len - 5 > 32 ||
            strcasecmp(e->d_name + len - 5, ".json"))
            continue;
        if (skip && strlen(skip) == len - 5 &&
            !strncasecmp(e->d_name, skip, len - 5))
            continue;
        *bytes += (long)info.size;
        if (names && n < max) {
            memcpy(names[n], e->d_name, len - 5);
            names[n][len - 5] = 0;
        }
        n++;
    }
    closedir(d);
    return n;
}

static int st_write(void *ctx, const char *name, const void *buf, int32_t n)
{
    (void)ctx;
    char path[MAX_PATH];
    long bytes;
    int others = scan(NULL, 0, name, &bytes);
    if (others >= ARCADE_STORE_FILES || bytes + n > ARCADE_STORE_BYTES)
        return 0;
    mkdir("/.rockbox/games");
    mkdir(DATA_DIR);
    file_path(name, path, sizeof(path));
    return shell_write_file(folder, path, buf, (size_t)n, NULL, 0);
}

static int st_remove(void *ctx, const char *name)
{
    (void)ctx;
    char path[MAX_PATH];
    file_path(name, path, sizeof(path));
    return remove(path) == 0;
}

static int by_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int st_list(void *ctx, char (*names)[33], int max)
{
    (void)ctx;
    long bytes;
    int n = scan(names, max, NULL, &bytes);
    if (n > max)
        n = max;
    qsort(names, (size_t)n, sizeof(names[0]), by_name);
    return n;
}

const struct arcade_store shell_store = {
    st_read, st_write, st_remove, st_list
};
