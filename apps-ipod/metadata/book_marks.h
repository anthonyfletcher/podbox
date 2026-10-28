/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to book_marks.c: the bookmarks inside audiobooks.
 ****************************************************************************/

#ifndef _BOOK_MARKS_H
#define _BOOK_MARKS_H

#include <stdbool.h>
#include <stdint.h>

/* Marks across every book. Adding past it fails rather than dropping one. */
#define BOOK_MARKS_MAX  1024

/* The longest label: a chapter's title and the time */
#define BOOK_MARK_LABEL_MAX 96

struct book_mark
{
    const char *path;           /* the chapter's file */
    const char *label;
    unsigned long elapsed;      /* ms into it */
    unsigned long offset;       /* the codec's byte offset into it */
};

/* Once, at boot */
void book_marks_init(void);

/* Mark where the loaded book is playing. False if nothing that is a book is
 * loaded, the store is full or the file could not be written. */
bool book_marks_add_playing(void);

/* A mark on 'book' (book_resume_key()), labelled as Create Bookmark labels
 * one; false if it could not be written. For bookmarks carried over from
 * another store. */
bool book_marks_add(uint64_t book, const char *path, unsigned long elapsed,
                    unsigned long offset, const char *title);

/* Whether book id 'book' has any marks */
bool book_marks_any(const char *book);

/* The marks of book id 'book', newest first, until 'fn' returns false. The
 * strings last only for the call. */
typedef bool (*book_marks_fn)(const struct book_mark *m, void *data);
void book_marks_each(const char *book, book_marks_fn fn, void *data);

/* Deletes the mark of 'book' at that chapter and time */
bool book_marks_delete(const char *book, const char *path,
                       unsigned long elapsed);

#endif /* _BOOK_MARKS_H */
