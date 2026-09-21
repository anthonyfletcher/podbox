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

/* whether "path" names a file that could carry chapter marks */
bool mp4_chapters_possible(const char *path);

/* read the chapter list out of the file "id3" describes into "cue" */
bool parse_mp4_chapters(struct mp3entry *id3, struct cuesheet *cue);

#endif
