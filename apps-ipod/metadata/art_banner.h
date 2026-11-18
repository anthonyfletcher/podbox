/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to art_banner.c: the owner's banner art for bios and liner
 * notes, cached at the banner's size, and where the owner has moved a
 * portrait's banner to.
 ****************************************************************************/

#ifndef _ART_BANNER_H_
#define _ART_BANNER_H_

#include <stdbool.h>
#include <stddef.h>

/* A banner's size on screen, and so in the cache */
#define ART_BANNER_W 320
#define ART_BANNER_H 132

/* Where the owner puts bios, notes and their banner art */
#define NOTES_PEOPLE_DIR "/Notes/People"
#define NOTES_ALBUMS_DIR "/Notes/Albums"

/* Caches the banner art in /Notes that is new or changed since the last
 * sweep, and drops what has gone. Runs on the art cache's thread, with its
 * decode buffer. Stops early, keeping what it has done, when `stop` says so;
 * false then. */
bool art_banner_sweep(void *workbuf, size_t worksz, bool (*stop)(void));

/* Forgets every cached banner, so the next sweep makes them all again */
void art_banner_forget(void);

/* The cached banner of the person called `name`, or of the album `title` by
 * `artist` (or by no one), as a path into `out`. False when there is none,
 * or it is not made yet. */
bool art_banner_person(const char *name, char *out, size_t size);
bool art_banner_album(const char *artist, const char *title, char *out,
                      size_t size);

/* Where the owner moved the banner kept under `key` -- a person's name key,
 * or an album's work key (art_name.h) -- to: its top, in 1/1024ths of the
 * picture's height, or -1 for not moved. */
int art_banner_offset(unsigned int key);

/* Saves `top`, or with -1 forgets it. False if the file could not be
 * written. */
bool art_banner_set_offset(unsigned int key, int top);

#endif /* _ART_BANNER_H_ */
