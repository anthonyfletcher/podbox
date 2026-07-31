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
