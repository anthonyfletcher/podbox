/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to book_resume.c: where each audiobook was left.
 ****************************************************************************/

#ifndef _BOOK_RESUME_H
#define _BOOK_RESUME_H

#include <stdbool.h>
#include <stdint.h>
#include "file.h"           /* MAX_PATH */

/* As much of a book's name as identifies it. The browser knows a book by the
 * title of the level it is on, which it holds in a buffer of this size, so a
 * longer name is cut to the same length before it is keyed or the two would
 * never match. */
#define BOOK_KEY_MAX    128

/* How many books are remembered: as many as the shelf can list, so a book
 * marked by hand keeps its mark in any library the shelf was written for. */
#define BOOK_RESUME_MAX 1024

/* How a book was left. The two marks are set by hand from the shelf, and the
 * next save of the book while it plays replaces either. */
enum book_left
{
    BOOK_LEFT_PARTWAY,          /* stopped inside 'track' */
    BOOK_LEFT_ENDED,            /* 'track' played to its end */
    BOOK_LEFT_FINISHED,         /* marked Finished */
    BOOK_LEFT_UNSTARTED,        /* marked Not started */
};

/* What one book's saved position holds. 'index' is a hint at the track's
 * place in the playlist; 'track' is what actually identifies it, since a
 * rebuilt playlist is only usually numbered the same way. */
struct book_resume
{
    uint64_t track;             /* path_key() of the chapter's file; 0 is the
                                   book's beginning */
    int  index;
    unsigned long elapsed;      /* ms into the chapter */
    unsigned long offset;       /* the codec's byte offset into it */
    enum book_left left;
};

/* The key 'book' is saved under. 'book' is its album tag, or the file's path
 * for a single-file book with no album to name it. Letter case is folded, so
 * two albums named alike but for case are one book here. */
uint64_t book_resume_key(const char *book);

/* Registers for the end of a track, which is how a book played through to its
 * last word is noticed. Once, at boot. */
void book_resume_init(void);

/* Write down where the playing track is, if it is a book and Segregate
 * Audiobooks is on. Silent about everything else, so a caller does not have
 * to ask what is playing before calling.
 *
 * Also writes down a book whose track played to its end since the last call,
 * which the audio thread can only note in RAM.
 *
 * Rewrites the file, so this is for the moment a book stops being listened
 * to -- a pause, a stop, leaving the WPS, a shutdown, and the playlist erase
 * that starts a different one -- and never for the audio thread. */
void book_resume_save(void);

/* Mark 'book' Finished, Not started, or -- with BOOK_LEFT_PARTWAY -- in
 * progress from its beginning. Marking the book that is loaded stops it, or
 * for in progress saves where it is. */
bool book_resume_mark(const char *book, enum book_left left);

/* The position saved for 'book'. False if it has none, and wherever there is
 * nothing to resume: a saved track that played to its end, a mark, or a book
 * marked in progress from its beginning. */
bool book_resume_get(const char *book, struct book_resume *pos);

/* Whatever is saved for 'book', pos->left saying what. */
bool book_resume_find(const char *book, struct book_resume *pos);

/* Every saved book, most recently played first, ended and marked ones
 * included. 'path' is the file for a book keyed by its path, NULL for one
 * keyed by its album. 'fn' returns false to stop early. */
typedef bool (*book_resume_fn)(uint64_t book, const char *path,
                               const struct book_resume *pos, void *data);
void book_resume_each(book_resume_fn fn, void *data);

#endif /* _BOOK_RESUME_H */
