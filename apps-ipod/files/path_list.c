/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A file of paths, a libfile of them each NUL-terminated, read into memory
 * and indexed.
 *
 * Two features keep lists of this shape -- the folders the artwork cache found
 * nothing for, and the documents and images the file index found -- and both
 * want the same three things: read it into a buffer, address the lines, give
 * the buffer back. That is all this is. It knows nothing about what the paths
 * mean.
 *
 * The buffer is claimed per open and released on close, so a list costs
 * nothing while its screen is shut.
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "file.h"
#include "system/app_buffer.h"
#include "system/library_files.h"
#include "path_list.h"

/* One name for the claim, because only one list is ever loaded at a time --
 * the documents/images browser and the artwork health screen cannot both be
 * up. app_buffer compares owners by string, so the same literal must be used
 * to claim and to release. */
#define PATH_LIST_OWNER "path list"

bool path_list_load(struct path_list *pl, const char *file, int max_entries)
{
    struct libfile_header h;
    size_t cap;
    off_t size;
    int fd, i;

    pl->held = false;
    pl->text = NULL;
    pl->count = 0;
    pl->truncated = false;

    if (max_entries > PATH_LIST_MAX)
        max_entries = PATH_LIST_MAX;

    fd = libfile_open(file, LIB_PATHS_MAGIC, LIB_PATHS_VERSION, 1, &h, NULL);
    if (fd < 0)
        return false;

    size = h.count;
    if (size <= 0)
    {
        close(fd);
        return false;
    }

    pl->text = app_claim_buffer(&cap, PATH_LIST_OWNER);
    pl->held = true;

    /* One byte kept back so the last path is terminated even in a file cut
     * short. A file bigger than that is read as far as it fits -- the reader
     * only ever shows PATH_LIST_MAX paths anyway. */
    if ((size_t)size > cap - 1)
    {
        size = (off_t)(cap - 1);
        pl->truncated = true;
    }

    if (read(fd, pl->text, size) != (ssize_t)size)
    {
        close(fd);
        path_list_free(pl);
        return false;
    }
    close(fd);
    pl->text[size] = '\0';

    /* Cutting the read short lands mid-path. Drop that fragment rather than
     * offering a half a filename the caller would fail to open. */
    if (pl->truncated)
    {
        while (size > 0 && pl->text[size - 1] != '\0')
            size--;
        pl->text[size] = '\0';
    }

    /* Each path is recorded by where it starts */
    for (i = 0; i < (int)size; i++)
    {
        if (i == 0 || pl->text[i - 1] == '\0')
        {
            if (pl->count == max_entries)
            {
                pl->truncated = true;
                break;
            }
            pl->line[pl->count++] = i;
        }
    }

    if (pl->count == 0)
    {
        path_list_free(pl);
        return false;
    }
    return true;
}

void path_list_free(struct path_list *pl)
{
    if (pl->held)
        app_release_buffer(PATH_LIST_OWNER);

    pl->held = false;
    pl->text = NULL;
    pl->count = 0;
    pl->truncated = false;
}

const char *path_list_get(const struct path_list *pl, int index)
{
    if (index < 0 || index >= pl->count)
        return "";
    return pl->text + pl->line[index];
}

const char *path_list_leaf(const struct path_list *pl, int index)
{
    const char *path = path_list_get(pl, index);
    const char *slash = strrchr(path, '/');

    /* Something at the volume root has nothing after the slash; show the path
     * rather than an empty row. */
    return (slash && slash[1]) ? slash + 1 : path;
}

/* ---- writing a list ---------------------------------------------------- */

/* 'path' is the open/closed flag, not just the name.
 *
 * Trap: writers are file-scope statics, so an untouched one is all zeroes --
 * and a zeroed descriptor is 0, a perfectly good one, not -1. Only _open()
 * ever sets 'path', so testing that is what makes a zeroed writer safe. The
 * artwork cache reaches _close() without _open() on its no-memory path. */
static bool writer_is_open(const struct path_list_writer *w)
{
    return w->path != NULL;
}

bool path_list_write_open(struct path_list_writer *w, const char *file)
{
    bool ok = libfile_begin(&w->lf, file, LIB_PATHS_MAGIC, LIB_PATHS_VERSION,
                            1, NULL);

    w->count = 0;
    w->path = ok ? file : NULL;
    return ok;
}

/* Lines past what a reader would keep are dropped rather than written: the
 * reader stops at PATH_LIST_MAX and flags the list truncated, so the rest
 * would only ever be disk. */
void path_list_write_record(struct path_list_writer *w, const char *line)
{
    if (!writer_is_open(w) || w->count >= PATH_LIST_MAX)
        return;

    size_t len = strlen(line) + 1;

    if (libfile_write(&w->lf, line, len, len))
        w->count++;
}

bool path_list_write_full(const struct path_list_writer *w)
{
    return w->count >= PATH_LIST_MAX;
}

void path_list_write_close(struct path_list_writer *w, bool completed)
{
    /* Never opened, or closed already: there is nothing to publish or clean
     * up, and -- the point of the check -- nothing that would justify
     * touching the published list. */
    if (!writer_is_open(w))
        return;

    libfile_finish(&w->lf, completed);
    w->path = NULL;
}
