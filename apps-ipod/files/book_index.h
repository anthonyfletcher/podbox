/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to book_index.c: the title, author and cover of every ebook.
 ****************************************************************************/
#ifndef _BOOK_INDEX_H
#define _BOOK_INDEX_H

#include <stdbool.h>
#include <stdint.h>

/* Memory the documents walk takes while it brings the index up to date */
#define BOOK_INDEX_WORK_BYTES (160 * 1024)

/* One book. title and author are empty where the book names none. */
struct book_entry
{
    const char *path;
    const char *title;
    const char *author;
    int parts;                  /* spine items */
    long cover_at;              /* the cover's offset in the file, or -1 */
    uint32_t cover_csize;       /* its size as stored */
    uint32_t cover_usize;       /* and unpacked */
    int cover_method;           /* 0 stored, 8 deflated */
};

/* The documents walk's side, on the background thread: begin, then each
 * book file it passes with its directory entry's size and time, then end.
 * A book is opened only when it is new or its size or time moved. An
 * interrupted walk keeps the old entries of the books it did not reach. */
void book_index_begin(void);
void book_index_found(const char *path, unsigned long size,
                      unsigned long mtime);
/* True when anything was added, changed or removed */
bool book_index_end(bool completed);

/* Whether `path` is a book the index takes */
bool book_index_is_book(const char *path);

/* Every indexed book, until `fn` returns false. The strings last only for
 * the call. */
typedef bool (*book_index_fn)(const struct book_entry *e, void *data);
void book_index_each(book_index_fn fn, void *data);

#endif /* _BOOK_INDEX_H */
