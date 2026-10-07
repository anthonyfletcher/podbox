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
/* Changes whenever the library does; 0 when it cannot be read or Accessory
 * Browsing is Off. */
uint32_t iap_library_revision(void);
uint64_t iap_library_key(const char *path);
/* Plays the tracks named by n keys, big-endian, from the start'th. False when
 * none is in the database or a list is still being built. */
bool iap_library_play_keys(const uint8_t *keys, size_t n, uint32_t start);
/* An iAP2 car's shuffle and repeat buttons, as iAP1 accessories set them:
 * shuffle on or off, and repeat off, all, one, off. */
void iap_library_shuffle_toggle(void);
void iap_library_repeat_next(void);

/* The playlists as iAP2 sends them, in the Playlist category's order: the
 * Queue; the folder Audiobooks, then each book; then the saved playlists.
 * Moods and Journeys are iAP1's alone. All are read together on the
 * library's worker: ask, then iap_library_playlists() gives them once read
 * (-1 until then), and done lets them go. The Queue on its own, in the order it plays, is read the
 * same way; it is cut short past a few hundred tracks, which a count below
 * playlist_amount() shows. */
enum { IAP_LIST_IDLE, IAP_LIST_BUSY, IAP_LIST_READY };
#define IAP_LIBRARY_LISTS_MAX 64   /* playlists, at most */
struct iap_library_list {
    uint64_t id;            /* the same at every connection */
    uint64_t parent;        /* the folder it is in, or 0 */
    bool folder;
    char name[64];
    const uint64_t *keys;   /* its tracks, in order */
    uint32_t count;
};
/* False while a read is under way. A read rewrites the lists the last one
 * gave, so not while a transfer is sending from them. */
bool iap_library_playlists_ask(void);
int iap_library_playlists(const struct iap_library_list **all);
void iap_library_playlists_done(void);
bool iap_library_queue_ask(void);
/* NULL until read */
const struct iap_library_list *iap_library_queue(void);
void iap_library_queue_done(void);

/* A track's artwork, a JPEG as the file holds it, read on the library's
 * worker: find starts it (false when the worker cannot take it now); state
 * says whether it was found and how big it is; chunk gives the bytes read
 * so far, none until more are, and next frees them for the worker; stop
 * gives up. Prefetch, called while no cover is being sent, encodes the next
 * track's ahead when it is due. */
enum { IAP_ART_NONE, IAP_ART_FINDING, IAP_ART_FOUND, IAP_ART_FAILED };
struct mp3entry;
bool iap_library_artwork_find(const struct mp3entry *id3);
int iap_library_artwork_state(uint32_t *size);
size_t iap_library_artwork_chunk(const uint8_t **data);
void iap_library_artwork_next(void);
void iap_library_artwork_stop(void);
void iap_library_artwork_prefetch(void);

/* The name every iAP transport gives an accessory: the first line of
 * player_name.txt as load read it at boot, PodBox's written there when it has
 * none of its own. */
#define IAP_PLAYER_NAME_DEFAULT "PodBox"
void iap_player_name_load(void);
void iap_player_name(char *buf, size_t size);

/* Drops a UTF-8 character that copying a string cut short */
void iap_utf8_cut(char *s);

#endif /* _IAP_LIBRARY_H_ */
