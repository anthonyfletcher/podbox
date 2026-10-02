/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The library as an iAP accessory browses it, over USB or the dock's serial
 * line: Playlists, Genres, Artists, Composers, Albums, Audiobooks and Songs,
 * counted, named and played by index. The types are iAP's database
 * categories (Playlist 1, Artist 2, Album 3, Genre 4, Track 5, Composer 6,
 * Audiobook 7); any other is not offered.
 ****************************************************************************/

#ifndef _IAP_LIBRARY_H_
#define _IAP_LIBRARY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SelectDBRecord's sort order, for a selection that names none */
#define IAP_LIBRARY_SORT_DEFAULT 0xFF

/* False for a category not offered, or one that cannot be read now. */
bool iap_library_count(int type, uint32_t *count);
bool iap_library_name(int type, uint32_t index, char *buf, size_t size);
/* An index of 0xFFFFFFFF steps back up a level. 'sort' is the order the
 * songs under the selection are to come in. */
bool iap_library_select(int type, uint32_t index, int sort);
/* Back to the top: nothing selected, and Songs is the Queue. */
void iap_library_reset(void);
/* Whether the Track category is the Queue rather than a database selection.
 * The Queue's tracks are named and chosen by the caller. */
bool iap_library_tracks_are_queue(void);
/* Whether a chosen song list is still being made into the Queue; it plays
 * when that finishes, so there is nothing to press Play on meanwhile. */
bool iap_library_building(void);
/* Frees the lists; called when the accessory goes. */
void iap_library_close(void);

/* The library as iAP2 sends it: every track, while the database is in RAM.
 * A track's key is path_key() of its file, so it survives a rebuild and
 * names the same track at every connection. */
struct iap_library_track {
    uint64_t key;       /* 0: no track in this slot, skip it */
    const char *title, *album, *artist, *albumartist, *genre, *composer;
    uint64_t album_id, artist_id, albumartist_id, genre_id, composer_id;
    uint32_t length;    /* ms */
    int tracknum, discnum;
};
/* Slots run from 0 to the count; false past the end or once the database
 * leaves RAM. A name is NULL when untagged, and good until the next call. */
int iap_library_track_slots(void);
bool iap_library_track(int n, struct iap_library_track *t);
/* Changes whenever the library does; 0 when it cannot be read. */
uint32_t iap_library_revision(void);
uint64_t iap_library_key(const char *path);
/* Plays the tracks named by n keys, big-endian, from the start'th. False when
 * none is in the database or a list is still being built. */
bool iap_library_play_keys(const uint8_t *keys, size_t n, uint32_t start);

#endif /* _IAP_LIBRARY_H_ */
