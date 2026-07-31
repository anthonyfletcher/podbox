/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to file_index.c.
 ****************************************************************************/
#ifndef _FILE_INDEX_H
#define _FILE_INDEX_H

#include <stdbool.h>
#include "system/bg_task.h"

/* Start the background thread that keeps the document and image lists
 * current. Call once at startup. */
void file_index_init(void);

/* Path of the list file for one class, for path_list_load(). Absent until a
 * walk has completed. */
const char *file_index_list(bool images);

/* True while a walk is running. */
bool file_index_is_busy(void);

/* The standard Rebuild/Update triggers. Both mean the same thing here: the
 * walk rewrites both lists from scratch, so there is no partial update to
 * distinguish. */
extern struct bg_task file_index_task;

#endif /* _FILE_INDEX_H */
