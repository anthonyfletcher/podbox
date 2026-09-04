/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Playing what a Spun card is about.
 *
 * The row's cards carry NAMES -- read from a tag, or worked out from a folder
 * when the tag was missing, at the time the play was logged. They are not
 * database ids and there is no link back to one, so playing a card means
 * looking its name up in the library again, and that lookup can miss for
 * ordinary reasons: the file has since gone, the tag has been edited, or the
 * name came from a path that never matched the tag in the first place.
 *
 * So nothing is erased until a path is in hand. A count decides whether to go
 * on at all, and the playlist is then replaced by the first path that
 * actually arrives rather than in advance of it -- the count is a promise
 * about the library, not about the read that follows. Either way a card that
 * comes to nothing leaves playback alone: the one outcome worse than not
 * playing is playing the wrong thing, and an empty playlist under a track
 * that is still going is the quiet version of it.
 *
 * Parts: the clause pair that turns a name into a search; the queue that
 * defers the erase; the count that decides whether to go on; then the two
 * ways a match reaches the playlist, in track order for an album and in
 * search order for the rest.
 ****************************************************************************/

#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "file.h"
#include "lang.h"
#include "settings/settings.h"          /* ID2P */
#include "database/tagcache.h"
#include "widgets/splash.h"
#include "system/app_util.h"            /* warn_on_pl_erase */
#include "playlist/playlist.h"
#include "pv_play.h"

/* Tracks an album is ordered in one go. Past this it is played in the order
 * the search returns instead, which is the order the files were scanned --
 * close to track order for most rips and wrong for the rest. */
#define ORDER_MAX  96

struct ordered
{
    int32_t idx_id;
    int32_t key;        /* disc above track, so discs stay whole */
};

static struct ordered order[ORDER_MAX];

/* The clauses a search is given. They must outlive the search, which keeps
 * pointers to them rather than copies. */
struct match
{
    struct tagcache_search_clause name;
    struct tagcache_search_clause artist;
};

/* The playlist, opened on the first path and not before.
 *
 * The count says the library has something under this name; it does not say
 * a path will reach the playlist. The second search can fail where the first
 * did not -- the tagcache commits in the background, and tagcache_search()
 * refuses while it does -- and replacing the playlist before that is found
 * out leaves the listener with an empty one and a card that reported nothing
 * wrong. So the erase waits for a path in hand: a run that adds nothing
 * leaves what is playing exactly as it was. */
struct queue
{
    struct playlist_insert_context ctx;
    bool open;
};

static bool queue_open(struct queue *q)
{
    if (q->open)
        return true;

    if (playlist_create(NULL, NULL) < 0)
        return false;

    if (playlist_insert_context_create(NULL, &q->ctx, PLAYLIST_INSERT_LAST,
                                       false, false) < 0)
    {
        /* create() keeps the playlist lock even when it fails; release() is
         * the only thing that gives it back. */
        playlist_insert_context_release(&q->ctx);
        return false;
    }

    q->open = true;
    return true;
}

static int queue_add(struct queue *q, const char *path)
{
    if (!queue_open(q))
        return -1;

    return playlist_insert_context_add(&q->ctx, path);
}

static int name_tag(enum pv_target_kind kind)
{
    switch (kind)
    {
    case PV_TARGET_ARTIST: return tag_artist;
    case PV_TARGET_ALBUM:  return tag_album;
    default:               return tag_title;
    }
}

/* Open a filename search matching 't', narrowed by the artist when asked. */
static bool search_open(struct tagcache_search *tcs, struct match *m,
                        const struct pv_target *t, bool narrowed)
{
    if (!tagcache_search(tcs, tag_filename))
        return false;

    m->name.tag     = name_tag(t->kind);
    m->name.type    = clause_is;
    m->name.numeric = false;
    m->name.source  = source_constant;
    m->name.str     = (char *)t->name;
    tagcache_search_add_clause(tcs, &m->name);

    if (narrowed)
    {
        m->artist.tag     = tag_artist;
        m->artist.type    = clause_is;
        m->artist.numeric = false;
        m->artist.source  = source_constant;
        m->artist.str     = (char *)t->artist;
        tagcache_search_add_clause(tcs, &m->artist);
    }
    return true;
}

/* How many tracks the library has under this name. Counted before anything is
 * queued, so a card that matches nothing costs the listener nothing. */
static int count_matches(const struct pv_target *t, bool narrowed)
{
    struct tagcache_search tcs;
    struct match m;
    char buf[MAX_PATH];
    int n = 0;

    if (!search_open(&tcs, &m, t, narrowed))
        return -1;
    while (tagcache_get_next(&tcs, buf, sizeof(buf)))
        n++;
    tagcache_search_finish(&tcs);
    return n;
}

static int compare_ordered(const void *a_v, const void *b_v)
{
    const struct ordered *a = a_v;
    const struct ordered *b = b_v;
    return a->key - b->key;
}

/* Every match, in the order the search returns them, at most 'limit' of them
 * (0 for all). Folder by folder, which is album by album for an artist. */
static int insert_in_search_order(struct queue *q,
                                  const struct pv_target *t, bool narrowed,
                                  int limit)
{
    struct tagcache_search tcs;
    struct match m;
    char path[MAX_PATH];
    int added = 0;

    if (!search_open(&tcs, &m, t, narrowed))
        return 0;

    while (tagcache_get_next(&tcs, path, sizeof(path)))
    {
        if (queue_add(q, path) < 0)
            break;
        added++;
        if (limit && added >= limit)
            break;
    }

    tagcache_search_finish(&tcs);
    return added;
}

/* An album in the order its track list shows it: by tagnavi's disc-then-track
 * key, so playing an album from here and from the browser come out the same.
 * Returns -1 when the album is too big to order in one go. */
static int insert_in_track_order(struct queue *q,
                                 const struct pv_target *t, bool narrowed)
{
    struct tagcache_search tcs;
    struct match m;
    char path[MAX_PATH];
    int found = 0, added = 0;

    if (!search_open(&tcs, &m, t, narrowed))
        return 0;

    while (tagcache_get_next(&tcs, path, sizeof(path)))
    {
        int disc, track;

        if (found >= ORDER_MAX)
        {
            tagcache_search_finish(&tcs);
            return -1;
        }
        disc  = tagcache_get_numeric(&tcs, tag_discnumber);
        track = tagcache_get_numeric(&tcs, tag_tracknumber);
        if (disc < 0)  disc = 0;
        if (track < 0) track = 0;
        order[found].idx_id = tcs.idx_id;
        /* Both masked: these come from file tags, so a nonsense value must
         * not shift into the sign bit and sort the track to the front. */
        order[found].key = ((disc & 0x7fff) << 16) | (track & 0xffff);
        found++;
    }

    qsort(order, found, sizeof(*order), compare_ordered);

    for (int i = 0; i < found; i++)
    {
        if (!tagcache_retrieve(&tcs, order[i].idx_id, tag_filename,
                               path, sizeof(path)))
            continue;
        if (queue_add(q, path) < 0)
            break;
        added++;
    }

    tagcache_search_finish(&tcs);
    return added;
}

int pv_play_target(const struct pv_target *t)
{
    struct queue q;
    bool narrowed;
    int found, added;

    if (!t || t->kind == PV_TARGET_NONE || !t->name || !t->name[0])
        return -1;

    if (!tagcache_is_usable())
    {
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        return -1;
    }

    cpu_boost(true);

    /* Narrowed by the artist first, because any number of albums and songs
     * share a title. Widening when that finds nothing is what makes a
     * disagreeing artist a looser match rather than no match at all: Spun's
     * artist may have come from a folder name that the tag never matched. */
    narrowed = t->kind != PV_TARGET_ARTIST && t->artist && t->artist[0];
    found = count_matches(t, narrowed);
    if (found == 0 && narrowed)
    {
        narrowed = false;
        found = count_matches(t, false);
    }

    if (found <= 0)
    {
        cpu_boost(false);
        return found < 0 ? -1 : 0;
    }

    /* Asked here, where nothing is held. The question is the listener's to
     * answer and the answer costs them a playlist, so it is put before the
     * second search rather than inside it -- a prompt raised with a tagcache
     * search open holds the read lock for as long as the listener thinks. */
    if (!warn_on_pl_erase())
    {
        cpu_boost(false);
        return -1;
    }

    q.open = false;

    if (t->kind == PV_TARGET_ALBUM)
    {
        added = insert_in_track_order(&q, t, narrowed);
        if (added < 0)
            added = insert_in_search_order(&q, t, narrowed, 0);
    }
    else
    {
        /* One track for a song. A title that appears on several albums is
         * still one song to the card that named it, and queueing every copy
         * would play the same thing three times over. */
        added = insert_in_search_order(&q, t, narrowed,
                                       t->kind == PV_TARGET_SONG ? 1 : 0);
    }

    if (q.open)
        playlist_insert_context_release(&q.ctx);
    cpu_boost(false);

    /* Nothing added means nothing opened, so there is nothing to put back:
     * whatever was playing is still playing, over the playlist it had. */
    if (added <= 0)
        return 0;

    playlist_start(0, 0, 0);
    return added;
}
