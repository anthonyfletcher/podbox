/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to sound_props.c: the Playlist Engine's measurements of one
 * track, said in words.
 ****************************************************************************/

#ifndef _SOUND_PROPS_H
#define _SOUND_PROPS_H

#include <stdbool.h>
#include <stddef.h>

/* A one-line summary of how 'path' sounds, for the row that opens the
 * read-out. False where there is nothing to say -- the engine is off, there
 * is no index, or this track is not in it -- and the caller then leaves the
 * row out rather than showing it empty. */
bool sound_props_summary(const char *path, char *buf, size_t len);

/* The read-out. True if it was left for the root menu, as view_text reports
 * the same thing, so a list underneath goes with it instead of redrawing. */
bool sound_props_screen(const char *path);

#endif /* _SOUND_PROPS_H */
