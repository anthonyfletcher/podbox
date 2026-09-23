/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to id3_chapters.c.
 ****************************************************************************/

#ifndef _ID3_CHAPTERS_H_
#define _ID3_CHAPTERS_H_

#include <stdbool.h>
#include "metadata.h"
#include "cuesheet.h"

/* whether "path" names a file that could carry ID3 chapter frames */
bool id3_chapters_possible(const char *path);

/* fill "cue"'s entries from the file's CHAP frames, returning how many */
int read_id3_chapters(const char *path, struct cuesheet *cue);

#endif
