/***************************************************************************
 * Original code from RockBox
 * was: apps/recorder/albumart.h
 * Copyright (C) 2007 Nicolas Pennequin
 * GNU General Public License (version 2+)
 *
 * Interface to albumart.c.
 ****************************************************************************/

#ifndef _ALBUMART_H_
#define _ALBUMART_H_


#include <stdbool.h>
#include "metadata.h"
#include "skin/skin_engine.h"

/* Look for albumart bitmap in the same dir as the track and in its parent dir.
 * Calls size_func to get the dimensions to look for
 * Stores the found filename in the buf parameter.
 * Returns true if a bitmap was found, false otherwise */
bool find_albumart(const struct mp3entry *id3, char *buf, int buflen,
                    const struct dim *dim);

bool search_albumart_files(const struct mp3entry *id3, const char *size_string,
                           char *buf, int buflen);

void get_albumart_size(struct bitmap *bmp);

/* Where a track's cover comes from, by the Album Art setting's rules: the
 * preference (AA_PREFER_*) names the source tried first, and the others
 * follow in the order playback tries them -- art embedded in the file (a
 * JPEG, the one kind decoded from inside a file), an image file as
 * search_albumart_files() finds it, then the thumbnail cache. as_stored asks
 * for a JPEG exactly as the source holds it, or the cache's raw thumbnail,
 * for a caller that sends the bytes on rather than decoding them. False when
 * no source has art. */
enum { AA_SOURCE_NONE, AA_SOURCE_EMBEDDED, AA_SOURCE_FILE, AA_SOURCE_CACHE };
struct albumart_source {
    int kind;               /* AA_SOURCE_* */
    char path[MAX_PATH];    /* the file to read */
    off_t pos;              /* where the image starts in it */
    int size;               /* its length, or -1 for the whole file */
};
bool albumart_find_source(const struct mp3entry *id3, int preference,
                          bool as_stored, struct albumart_source *src);


#endif /* _ALBUMART_H_ */
