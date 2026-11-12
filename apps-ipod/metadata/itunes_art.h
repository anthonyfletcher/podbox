/***************************************************************************
 * GNU General Public License (version 2+)
 * Read-only iTunes artwork, used only by the art-cache worker.
 ****************************************************************************/
#ifndef _ITUNES_ART_H_
#define _ITUNES_ART_H_
#include "draw/resize.h"
#include "file.h"

#ifndef ITUNES_CONTROL
#define ITUNES_CONTROL "/iPod_Control"
#endif
#define ITUNES_ARTWORK_DB ITUNES_CONTROL "/Artwork/ArtworkDB"

struct itunes_art
{
    char path[MAX_PATH];
    off_t pos;
    unsigned short width, height, stride;
};

/* One indexed walk of each database for a pass/check; close releases both
 * indexes. A valid empty ArtworkDB succeeds without reading iTunesDB.
 * Failure leaves the adapter empty, without changing any database. */
bool itunes_art_open(bool (*stop)(void));
void itunes_art_close(void);
bool itunes_art_find(const char *path, struct itunes_art *art);
int itunes_art_decode(const struct itunes_art *art, struct bitmap *bm,
                      int maxsize, int format,
                      const struct custom_format *cformat);
#endif
