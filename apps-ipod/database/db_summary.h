/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to db_summary.c: the album and artist summary derived from the
 * tag database.
 ****************************************************************************/
#ifndef _DB_SUMMARY_H
#define _DB_SUMMARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "database/tagcache.h"
#include "system/bg_task.h"

/* What a build or a load returns. Negative is failure. The carousel's
 * build_index() hands these straight back, so they reach the engine too. */
#define SUCCESS              0
#define ERROR_NO_ALBUMS     -1
#define ERROR_BUFFER_FULL   -2
#define ERROR_NO_ARTISTS    -3
#define ERROR_USER_ABORT    -4

/* One album. Filled by a db_summary_build*(), read by whoever asked for the
 * index -- Album covers, the album charts, the browser's year sort. */
struct album_data {
    int name_idx;     /* offset to the album name */
    int artist_idx;   /* offset to the artist name */
    /* Identity that survives a database commit, hashed from the two names
     * above. The seeks below do not survive one: any commit that adds a track
     * re-sorts the whole album tagfile and moves every seek in it. So this is
     * what an entry is matched by when figures are carried from one build of
     * the index to the next. */
    uint32_t key;
    int year;         /* album year */
    /* Playback history, summarised over the album's tracks by
     * assign_album_stats(). The album charts sort on these; nothing else
     * reads them. Both are 0 for an album that has never been played, which
     * is why the charts exclude 0 rather than showing it at one end. */
    int playcount;    /* total plays across the album */
    long lastplayed;  /* the most recent of its tracks */
    long artist_seek; /* artist taglist position */
    long seek;        /* album taglist position */
    /* art_cache key for the folder holding this album's tracks, resolved once
     * during the build. A slide load is then a file open, where finding the
     * folder from the seeks above would be a database search per slide. 0 if
     * the build could not resolve it. */
    unsigned int art_hash;
    /* That folder's parent, which is the artist's own. Carried here because it
     * comes from the same path for free, and the artist list is filled by
     * rolling the album list up rather than by walking the database again. */
    unsigned int artist_art_hash;
};

struct artist_data {
    int name_idx; /* offset to the artist name */
    uint32_t key; /* as struct album_data's, from the name alone */
    /* As struct album_data's pair, summarised over everything by this artist.
     * "Artist" here is the album artist -- the tag this list is built from
     * (see build_artist_index()) -- so a guest appearance counts towards the
     * record's artist, not the guest. */
    int playcount;
    long lastplayed;
    long seek;    /* artist taglist position */
    /* As struct album_data's field, for the artist's own folder -- the parent
     * of its albums', under the <artist>/<album>/<track> layout this list
     * assumes. */
    unsigned int art_hash;
};

/* The whole index: the two arrays, the name blobs they index into, and what
 * the database looked like when it was built. Written to disk verbatim, so
 * see save_album_index() before adding a field. */
struct db_summary_t {
    uint32_t            header; /*INDEX_HDR*/
    /* Signed, not unsigned, and that is load-bearing. The builder writes the
     * "untagged" entries backwards from the start of each array, indexing with
     * -artist_ct / -album_ct (db_summary.c, write_artist_entry()). As uint16_t
     * these promoted to int and negated correctly; as an unsigned 32-bit type
     * the negation would wrap to about 4 billion and index off the end of the
     * buffer. int32_t raises the ceiling from 65535 to 2^31 and leaves every
     * existing expression meaning what it meant. */
    int32_t             artist_ct;
    int32_t             album_ct;
    /* What the database looked like when this index was built. Both come from
     * tagcache_get_marks(): commitid moves when tracks are added, serial once
     * per logged play. They say what has happened since, so a later build can
     * tell which entries still need their figures recomputed. */
    int32_t             commitid;
    int32_t             serial;
    /* Entries tagcache had flagged deleted. A deletion moves figures that
     * cannot be matched back to an album, so a build that finds this changed
     * summarises everything rather than carrying anything across. */
    int32_t             deleted;

    char               *artist_names;
    struct artist_data *artist_index;
    size_t              artist_len;

    unsigned int        album_untagged_idx;
    char               *album_names;
    struct album_data  *album_index;
    size_t              album_len;
    long                album_untagged_seek;

    void * buf;
    size_t buf_sz;
};

/* Fill the caller's index from the saved file, rebuilding it from tagcache
 * first if there is none or it predates the current cache version. SUCCESS or
 * an ERROR_*.
 *
 * Only one build may run at a time. A caller arriving while the background
 * pass is running waits for it and then reads what it wrote, rather than
 * stopping it and repeating the work -- waiting is the quicker of the two,
 * since this path redraws for every album and that one does not. The pass is
 * asked to stop only if the user abandons the wait.
 *
 * The buffer is the caller's: the carousel passes the app buffer it has
 * claimed, the background pass passes memory of its own. Nothing else differs
 * between the two, which is the point. */
int db_summary_build_into(struct db_summary_t *target, void *buf, size_t buf_sz);

/* Claim memory and fill 'target' from the saved index, building it first if
 * there is none. For a caller that just wants to read the album list and has
 * no buffer of its own -- unlike the carousel, which has already claimed the
 * app buffer for its slides. SUCCESS, or an ERROR_*.
 *
 * On success '*handle' is the claim, and db_summary_release() must be given
 * it once the caller has finished reading: 'target' points into that memory,
 * so nothing it holds outlives the release. */
int db_summary_acquire(struct db_summary_t *target, int *handle);
void db_summary_release(int handle);

/* Read single records out of the saved index without holding it.
 *
 * db_summary_acquire() above needs 384K of core memory, and the audio buffer
 * is what gives it up -- which stops playback and rebuffers the current track
 * (see shrink_callback() in playback.c). That is a fair price for the whole
 * list. It is a poor one for a caller that wants one fixed-size record, so
 * those seek to it in the file instead.
 *
 * A reader holds the build lock for its whole lifetime, so the background pass
 * cannot swap the file out underneath it. Open one, take what you need, close
 * it; do not put a screen up while one is open.
 *
 * The struct is here rather than opaque so the caller can keep one on its own
 * stack. Its fields are private. */
struct db_summary_reader {
    int fd;
    int album_ct;      /* albums the file holds */
    long album_off;    /* where the first of them starts */
};

/* Open the saved index, building one first if there is none to read. SUCCESS,
 * or an ERROR_*. On success, and only then, the caller must
 * match this with db_summary_reader_close(). */
int db_summary_reader_open(struct db_summary_reader *r);
void db_summary_reader_close(struct db_summary_reader *r);

/* Album n of r->album_ct into *out. False if it could not be read. */
bool db_summary_read_album(struct db_summary_reader *r, int n,
                           struct album_data *out);

/* One album's release year, against the taglist position the database browser
 * knows it by. */
struct db_summary_year {
    long seek;
    int  year;
};

/* Fill 'out' with every album's pair, sorted by seek so the caller can binary
 * search it. Returns how many were written, or a negative ERROR_*.
 *
 * For sorting album lists by year. The browser cannot read the year from the
 * database itself -- a unique tag's rows carry no index entry for a numeric
 * tag to be read from -- and the year here is the better one anyway: the
 * maximum across the album's tracks, which is what Album covers sorts on.
 *
 * Refuses when the saved index predates the current database commit. A commit
 * that adds a track re-sorts the album tagfile and moves every seek in it, so a
 * stale table would join cleanly against the wrong albums and produce a
 * plausible, wrong order. Name order is the right answer in that case. */
int db_summary_read_year_table(struct db_summary_year *out, int max);

/* Build only the artist half into 'target', from *buf, advancing it. The
 * artist carousel uses this on its own, without an album list. Serialises
 * against the background pass exactly as db_summary_build_into() does -- the
 * index being built is a single shared slot, so there is no unlocked way in. */
/* Read just the artist half of the saved index into *buf, advancing it. The
 * quick way to get an artist list: no database walk, and the playback figures
 * come with it already summarised. The album halves of the file are skipped,
 * so they cost no memory.
 *
 * SUCCESS, or an ERROR_* when there is no usable saved index -- in which case
 * the caller can fall back to building one with db_summary_build_artists()
 * below. Serialises against the background pass, which rewrites the file. */
int db_summary_load_artists(struct db_summary_t *target,
                          void **buf, size_t *bufsz);

/* Build the artist half from the database instead. Slower, and needed only
 * when there is no saved index to read.
 *
 * 'with_stats' also fills each artist's playcount/lastplayed, which this path
 * cannot get for free -- it has no album list to roll up from, so it costs a
 * filtered database search per artist. Ask for it only if something is going
 * to sort or rank on it. */
int db_summary_build_artists(struct db_summary_t *target,
                              struct tagcache_search *tcs,
                              void **buf, size_t *bufsz, bool with_stats);

/* Record that a track finished, so the album and artist figures move without
 * the index being rebuilt for it. 'serial' is the value tagcache_increase_
 * serial() just returned, which is also what it stores as lastplayed.
 *
 * Both names come from the track's own tags, so this costs no database work:
 * it appends twelve bytes and returns. A reader applies whatever arrived
 * after the index it just loaded was written. */
void db_summary_log_play(const char *album, const char *albumartist,
                       long serial);

/* qsort comparator over struct album_data, in the carousel's display order.
 * Public because the sort order can be changed from the carousel itself. */
int compare_albums(const void *a_v, const void *b_v);

/* Start the background pass that keeps the saved index current. Called once
 * at startup; it builds only while the database is idle and settled. */
void db_summary_init(void);

/* That pass, for the standard bg_task_rebuild()/bg_task_update() triggers.
 * It outranks the artwork cache -- see the ranking note in db_summary.c. */
extern struct bg_task db_summary_task;

/* True while that pass is building -- for anything that should stay off the
 * disk while it runs. */
bool db_summary_is_busy(void);

/* The build step the background pass last reported ("2/5 Find Albums" and so
 * on), or "" if it has not run. Held over after a pass, so it also says what
 * the last one finished on. */
const char *db_summary_activity(void);

/* That pass's last reported position within the step. Both zero when idle. */
void db_summary_progress(int *done, int *total);

/* Discard the background pass's record of what it last built, so it runs
 * again on its next tick. */
void db_summary_invalidate(void);

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
