/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to unreadable.c.
 ****************************************************************************/
#ifndef _UNREADABLE_H
#define _UNREADABLE_H

#include <stdbool.h>

/* Around one walk: begin before the first file, end after the last, with
 * 'complete' false for a walk that did not reach every folder. */
void unreadable_begin(void);
void unreadable_end(bool complete);

/* Whether the parser already refused this file as it is now. True also
 * lists it again; the walk then skips the file. */
bool unreadable_known(const char *path, unsigned long mtime);

/* The parser refused this file */
void unreadable_add(const char *path, unsigned long mtime);

/* Forget every refusal, so the next walk tries each file again */
void unreadable_forget(void);

#endif /* _UNREADABLE_H */
