/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A file of paths, one per line, read into memory and indexed by line.
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
#include "core_alloc.h"
#include "path_list.h"

bool path_list_load(struct path_list *pl, const char *file, int max_entries)
{
    off_t size;
    int fd, i;

    pl->handle = 0;
    pl->text = NULL;
    pl->count = 0;
    pl->truncated = false;

    if (max_entries > PATH_LIST_MAX)
        max_entries = PATH_LIST_MAX;

    fd = open(file, O_RDONLY);
    if (fd < 0)
        return false;

    size = filesize(fd);
    if (size <= 0)
    {
        close(fd);
        return false;
    }

    /* One spare byte so the last line is terminated even with no trailing
     * newline. */
    pl->handle = core_alloc(size + 1);
    if (pl->handle <= 0)
    {
        close(fd);
        pl->handle = 0;
        return false;
    }

    /* Pinned: read() yields, and a handle with no ops is movable. */
    pl->text = core_get_data_pinned(pl->handle);
    if (read(fd, pl->text, size) != (ssize_t)size)
    {
        close(fd);
        path_list_free(pl);
        return false;
    }
    close(fd);
    pl->text[size] = '\0';

    /* Split in place. A line is recorded by where it starts; its newline
     * becomes the terminator of the one before. */
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
        if (pl->text[i] == '\n')
            pl->text[i] = '\0';
    }

    /* A trailing newline leaves an empty final entry; drop it. */
    if (pl->count > 0 && pl->text[pl->line[pl->count - 1]] == '\0')
        pl->count--;

    if (pl->count == 0)
    {
        path_list_free(pl);
        return false;
    }
    return true;
}

void path_list_free(struct path_list *pl)
{
    if (pl->handle > 0)
    {
        core_put_data_pinned(pl->text);
        core_free(pl->handle);
    }
    pl->handle = 0;
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

static void tmp_name(const char *file, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%s.tmp", file);
}

/* 'path' is the open/closed flag, not just the name.
 *
 * Trap: writers are file-scope statics, so an untouched one is all zeroes --
 * and a zeroed 'fd' is 0, a perfectly good descriptor, not -1. Testing fd
 * alone would make _close() on a writer that was never opened close someone
 * else's file and then rename a ".tmp" built from a NULL name. Only _open()
 * ever sets 'path', so testing that is what makes a zeroed writer safe. The
 * artwork cache reaches _close() without _open() on its no-memory path. */
static bool writer_is_open(const struct path_list_writer *w)
{
    return w->path != NULL;
}

bool path_list_write_open(struct path_list_writer *w, const char *file)
{
    char tmp[MAX_PATH];

    w->count = 0;
    tmp_name(file, tmp, sizeof(tmp));
    w->fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    w->path = (w->fd >= 0) ? file : NULL;

    return w->fd >= 0;
}

/* Lines past what a reader would keep are dropped rather than written: the
 * reader stops at PATH_LIST_MAX and flags the list truncated, so the rest
 * would only ever be disk. */
void path_list_write_record(struct path_list_writer *w, const char *line)
{
    if (!writer_is_open(w) || w->count >= PATH_LIST_MAX)
        return;

    fdprintf(w->fd, "%s\n", line);
    w->count++;
}

bool path_list_write_full(const struct path_list_writer *w)
{
    return w->count >= PATH_LIST_MAX;
}

void path_list_write_close(struct path_list_writer *w, bool completed)
{
    char tmp[MAX_PATH];

    /* Never opened, or closed already: there is no .tmp to publish or clean
     * up, and -- the point of the check -- nothing that would justify removing
     * the published list. */
    if (!writer_is_open(w))
        return;

    close(w->fd);
    tmp_name(w->path, tmp, sizeof(tmp));

    if (completed)
    {
        remove(w->path);
        rename(tmp, w->path);
    }
    else
        remove(tmp);

    w->fd = -1;
    w->path = NULL;
}
