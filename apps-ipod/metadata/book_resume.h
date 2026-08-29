/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to book_resume.c: where each audiobook was left.
 ****************************************************************************/

#ifndef _BOOK_RESUME_H
#define _BOOK_RESUME_H

#include <stdbool.h>
#include "file.h"           /* MAX_PATH */

/* As much of a book's name as identifies it. The browser knows a book by the
 * title of the level it is on, which it holds in a buffer of this size, so a
 * longer name has to be cut to the same length on the way in or the two would
 * never match. */
#define BOOK_KEY_MAX    128

/* What one book's saved position holds. 'index' is a hint at the track's
 * place in the playlist; 'track' is what actually identifies it, since a
 * rebuilt playlist is only usually numbered the same way. */
struct book_resume
{
    char track[MAX_PATH];       /* the chapter's file */
    int  index;
    unsigned long elapsed;      /* ms into the chapter */
    unsigned long offset;       /* the codec's byte offset into it */
};

/* Write down where the playing track is, if it is a book and Segregate
 * Audiobooks is on. Silent about everything else, so a caller does not have
 * to ask what is playing before calling.
 *
 * Rewrites the file, so this is for the moment a book stops being listened
 * to -- a pause, a stop, leaving the WPS, a shutdown, and the playlist erase
 * that starts a different one -- and never for the audio thread. */
void book_resume_save(void);

/* The position saved for 'book' -- its album tag, or the file's path for a
 * single-file book with no album to name it. False if it has none. */
bool book_resume_get(const char *book, struct book_resume *pos);

#endif /* _BOOK_RESUME_H */
