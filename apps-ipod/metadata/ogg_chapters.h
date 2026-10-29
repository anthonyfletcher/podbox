/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to ogg_chapters.c.
 ****************************************************************************/

#ifndef _OGG_CHAPTERS_H_
#define _OGG_CHAPTERS_H_

#include <stdbool.h>
#include "cuesheet.h"

/* whether "path" names an Ogg Vorbis or Opus book that could carry chapter
   comments; "spoken" is whether it is spoken word, which it has to be */
bool ogg_chapters_possible(const char *path, bool spoken);

/* fill "cue"'s entries from the file's chapter comments, returning how many */
int read_ogg_chapters(const char *path, struct cuesheet *cue);

#endif
