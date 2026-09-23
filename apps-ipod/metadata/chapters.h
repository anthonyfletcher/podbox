/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to chapters.c.
 ****************************************************************************/

#ifndef _CHAPTERS_H_
#define _CHAPTERS_H_

#include <stdbool.h>
#include "metadata.h"
#include "cuesheet.h"

/* Fewer entries than this is not a chapter list: a book split into one part
   has nothing to show, and a lone entry is how some encoders mark delay. */
#define MIN_CHAPTERS 2

/* whether "path" names a file that could carry chapter marks */
bool chapters_possible(const char *path);

/* read the chapter marks out of the file "id3" describes into "cue" */
bool parse_chapters(struct mp3entry *id3, struct cuesheet *cue);

/* The same for a book that is not playing, whose tags are not to hand:
   "book" and "author" name it on the list's title, and either may be NULL. */
bool parse_chapters_path(const char *path, const char *book,
                         const char *author, struct cuesheet *cue);

#endif
