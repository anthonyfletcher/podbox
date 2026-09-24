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

struct sound_axes;

/* A one-line summary of how 'path' sounds, for the row that opens the
 * read-out. False where there is nothing to say -- the engine is off, there
 * is no index, or this track is not in it -- and the caller then leaves the
 * row out rather than showing it empty. */
bool sound_props_summary(const char *path, char *buf, size_t len);

/* The read-out. True if it was left for the root menu, as view_text reports
 * the same thing, so a list underneath goes with it instead of redrawing. */
bool sound_props_screen(const char *path);

/* The same read-out over the mean of a set of tracks, which for a tidily
 * laid-out library is an album -- the same thing db_summary.c's art_hash
 * already treats a folder as.
 *
 * Two rows differ from a track's. Pace is the mean of the tracks whose tempo
 * is trusted and carries no steadiness, and there is no key row at all: the
 * key of an average pitch vector is the key of nothing.
 *
 * Tracks arrive one at a time because the callers enumerate differently --
 * the file browser walks a folder, the database browser walks a row's
 * subentries through a callback of its own. Call begin, then add per track,
 * then ready, and then the screen if ready said there was anything.
 *
 *   sound_props_album_ready()   false where nothing that arrived was
 *                               measured. Must be called even after an
 *                               enumeration that failed or was abandoned,
 *                               since it is what closes the index.
 *   sound_props_album_screen()  shows it, and answers the question
 *                               sound_props_screen() does -- was this left
 *                               for the root menu -- which is not the same
 *                               question as ready's. */
void sound_props_album_begin(void);
void sound_props_album_add(const char *path);
bool sound_props_album_ready(void);
bool sound_props_album_screen(void);

/* begin() and an add() for every audio file in one folder, leaving the run
 * open for the finishing calls above. */
void sound_props_album_walk(const char *dir);

/* The same average handed back rather than shown, for a mix built around an
 * album. 'path' takes one of the tracks it was taken from, which is what the
 * mix's artist rules key off -- a mean has no path of its own. Either call
 * finishes the run; both may be made, in either order. */
bool sound_props_album_result(struct sound_axes *out, char *path, size_t len);

#endif /* _SOUND_PROPS_H */
