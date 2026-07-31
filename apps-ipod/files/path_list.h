/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to path_list.c.
 ****************************************************************************/
#ifndef _PATH_LIST_H
#define _PATH_LIST_H

#include <stdbool.h>

/* Hard ceiling on lines held, whatever a caller asks for. These lists are
 * meant to be read by a person; past a few hundred the screen is not the
 * right tool and the offset table is not worth the memory. */
#define PATH_LIST_MAX 500

struct path_list {
    int   handle;                  /* the claim; 0 when nothing is held */
    char *text;                    /* the file, newlines turned into NULs */
    int   line[PATH_LIST_MAX];     /* offset of each path into text */
    int   count;
    bool  truncated;               /* more lines than were kept */
};

/* Read 'file' and index its lines. False when there is nothing to show -- no
 * such file, empty, or no memory -- having claimed nothing. On success the
 * caller must path_list_free() when done: every pointer handed out points
 * into that memory. */
bool path_list_load(struct path_list *pl, const char *file, int max_entries);
void path_list_free(struct path_list *pl);

/* The whole path, and just its last component. Both return "" out of range,
 * so a list callback can hand the result straight back. */
const char *path_list_get(const struct path_list *pl, int index);
const char *path_list_leaf(const struct path_list *pl, int index);

#endif /* _PATH_LIST_H */
