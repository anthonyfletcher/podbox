/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to db_episode.c: the order of a podcast's episodes.
 ****************************************************************************/

#ifndef _DB_EPISODE_H_
#define _DB_EPISODE_H_

#include <stdbool.h>
#include <stddef.h>

/* The order an episode's stored title gives, as tag_virt_episode answers
 * it, larger for newer: season x 10000 + episode for S01E04 or 1x04, the
 * number of Ep 12 or #12, YYYYMMDD for a date, YYYYMM00 for a month. With
 * none, tracknum where it is above 0 (the caller passes 0 for a show whose
 * episodes share track numbers), on the same scale as Ep 12; then the date
 * of the file's time (tag_mtime, seconds since 1970 of the player's local time) as
 * YYYYMMDD, or 0 where that is 0. */
long db_episode_key(const char *title, long tracknum, unsigned long mtime);

/* The title the scan stores for a podcast episode: `title` itself when it
 * gives an order, else with " (YYYY-MM-DD)" after it from `date` (the tags'
 * full release date, or NULL) or from the file name in `path`, in `buf` */
const char *db_episode_title(const char *title, const char *date,
                             const char *path, char *buf, size_t size);

/* The length of `title` (`len` bytes) without that marker at its end */
int db_episode_shown_len(const char *title, int len);

/* An MP3 episode's description, its ID3v2 COMM text, as UTF-8 in `buf`: all
 * of it, or as much as fits cut at a word and ended with an ellipsis. False
 * where the file has none this can read. */
bool db_episode_comment(const char *path, char *buf, size_t size);

#endif /* _DB_EPISODE_H_ */
