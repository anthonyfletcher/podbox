/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to db_summary.c: the album and artist lists, copied out of the
 * tables tagcache keeps with the database in RAM.
 ****************************************************************************/
#ifndef _DB_SUMMARY_H
#define _DB_SUMMARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "database/tagcache.h"

/* What a build or a load returns. Negative is failure. The carousel's
 * build_index() hands these straight back, so they reach the engine too. */
#define SUCCESS              0
#define ERROR_NO_ALBUMS     -1
#define ERROR_BUFFER_FULL   -2
#define ERROR_NO_ARTISTS    -3
#define ERROR_USER_ABORT    -4

/* One album: one album name with one album artist. Read by Album covers, the
 * album charts and Random album.
 *
 * album_index[] comes back ordered by artist and then album name. That is the
 * index's own order and nobody's display order: a screen that wants some
 * other arrangement qsort()s the array itself. */
struct album_data {
    int name_idx;     /* offset to the album name */
    int artist_idx;   /* offset to the artist name */
    uint32_t key;     /* hashed from the two names */
    int year;         /* the latest of its tracks' */
    /* Playback history over the album's tracks. Both are 0 for an album that
     * has never been played, which is why the charts exclude 0 rather than
     * showing it at one end. */
    int playcount;
    long lastplayed;
    long artist_seek; /* album artist taglist position */
    long seek;        /* album taglist position */
    /* art_cache keys for the folder holding the album's first track, and for
     * the folder above it, which is the artist's own; 0 for none */
    unsigned int art_hash;
    unsigned int artist_art_hash;
};

struct artist_data {
    int name_idx; /* offset to the artist name */
    uint32_t key; /* hashed from the name */
    /* As struct album_data's pair, over everything by this album artist, so a
     * guest appearance counts towards the record's artist, not the guest */
    int playcount;
    long lastplayed;
    long seek;    /* album artist taglist position */
    unsigned int art_hash;  /* its first album's artist_art_hash */
};

/* The lists, and the database commit they were copied at. Seeks are seeks
 * only for that commit. */
struct db_summary_t {
    int32_t             artist_ct;
    int32_t             album_ct;
    int32_t             commitid;
    int32_t             serial;
    int32_t             deleted;

    char               *artist_names;
    struct artist_data *artist_index;
    size_t              artist_len;

    unsigned int        album_untagged_idx;
    char               *album_names;
    struct album_data  *album_index;
    size_t              album_len;
    long                album_untagged_seek;    /* -1 for none */

    /* What is left of the caller's buffer past the lists */
    void * buf;
    size_t buf_sz;
};

/* Fill 'target' with both lists from the caller's buffer. SUCCESS, or an
 * ERROR_* -- ERROR_NO_ALBUMS also while the database is not in RAM. Spoken
 * word is left out when Segregate Audiobooks keeps it apart. */
int db_summary_build_into(struct db_summary_t *target, void *buf, size_t buf_sz);

/* Only the artist list, from *buf, advancing it. SUCCESS or an ERROR_*. */
int db_summary_load_artists(struct db_summary_t *target,
                            void **buf, size_t *bufsz);

/* Single albums, for a caller with no buffer to hold the list. Open, take
 * what you need, close. */
struct db_summary_reader {
    int album_ct;
};

int db_summary_reader_open(struct db_summary_reader *r);
void db_summary_reader_close(struct db_summary_reader *r);

/* Album n of r->album_ct into *out, its names left unset. False if there is
 * no such album. */
bool db_summary_read_album(struct db_summary_reader *r, int n,
                           struct album_data *out);

/* What an album list is ordered by, against the taglist position the database
 * browser knows each album by. */
struct db_summary_order {
    long seek;
    int  year;
    /* The album artist's place in name order, counting from 0; equal for two
     * albums by the same artist. DB_SUMMARY_NO_ARTIST for an album with none. */
    int  artist;
};
#define DB_SUMMARY_NO_ARTIST 99999

/* Fill 'out' with an entry per album name, sorted by seek so the caller can
 * binary search it. Returns how many were written, or a negative ERROR_*.
 * 'ignore_articles' places artists as Sort Ignoring Articles does.
 *
 * For ordering album lists by year or by artist. The browser cannot read the
 * year from the database itself -- a unique tag's rows carry no index entry for
 * a numeric tag to be read from -- and the year here is the better one anyway:
 * the latest across the album's tracks, which is what Album covers sorts on. */
int db_summary_read_order_table(struct db_summary_order *out, int max,
                                bool ignore_articles);

/* Build the current playlist from one album's tracks and start it, in the same
 * disc/track order the browser's track list shows them in. Returns the number
 * of tracks queued, or a negative value if nothing could be played.
 *
 * Scratch memory comes from the app buffer, which panics if a screen still
 * holds a claim on it -- so this cannot be called from inside the carousel.
 * db_summary_play_album_on_exit() exists for that: it records the album, and
 * album_covers() calls db_summary_play_pending() once carousel_run() has
 * returned and released the claim. */
int  db_summary_play_album(const struct album_data *album);
void db_summary_play_album_on_exit(const struct album_data *album);
int  db_summary_play_pending(void);

#endif /* _DB_SUMMARY_H */
