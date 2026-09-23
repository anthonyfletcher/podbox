/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to mp4_chapters.c.
 ****************************************************************************/

#ifndef _MP4_CHAPTERS_H_
#define _MP4_CHAPTERS_H_

#include <stdbool.h>
#include "metadata.h"
#include "cuesheet.h"

/* whether "path" names a file that could carry MP4 chapter marks */
bool mp4_chapters_possible(const char *path);

/* fill "cue"'s entries from the file's chapter list, returning how many */
int read_mp4_chapters(const char *path, struct cuesheet *cue);

#endif
