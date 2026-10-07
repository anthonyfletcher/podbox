/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The database index: the flat album and artist list, for the screens that
 * want one -- Album covers, Artist portraits, the charts, Random album, and
 * the browser's year and artist orders.
 *
 * tagcache keeps the album and artist tables with the database in RAM (see
 * tagcache_album_get()), current with every commit and every play. This
 * copies them out into a caller's buffer as names and records the screens
 * read, leaving out spoken word when Segregate Audiobooks keeps it apart.
 * Nothing is built in the background or kept on disk; the lists exist while
 * the database is in RAM.
 *
 * Parts, in order:
 *   - what is left out, and copying names
 *   - the artist and album lists
 *   - single albums, for a caller with no buffer
 *   - the order table for album lists
 *   - playing an album
 ****************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "string-extra.h"
#include "config.h"
#include "system/hash.h"
#include "system.h"          /* ALIGN_BUFFER */
#include "kernel.h"
#include "lang.h"
#include "settings/settings.h"
#include "database/tagcache.h"
#include "widgets/splash.h"
#include "system/app_buffer.h"        /* scratch for ordering an album */
#include "system/app_util.h"          /* warn_on_pl_erase */
#include "playlist/playlist.h"
#include "db_summary.h"

/* ---- what is left out, and copying names ------------------------------- */

/* Spoken word, when Segregate Audiobooks keeps it out of the music: an album
 * all of whose tracks are spoken, and an artist all of whose albums are */
static bool album_hidden(const struct tagcache_album *a)
{
    return global_settings.segregate_audiobooks && a->tracks > 0
           && a->spoken == a->tracks;
}

static bool artist_hidden(const struct tagcache_artist *r)
{
    return global_settings.segregate_audiobooks && r->albums > 0
           && r->spoken_albums == r->albums;
}

/* Identity hashed from the names: an album's from its name and its artist's,
 * an artist's from its name alone */
static uint32_t name_key(const char *a, const char *b)
{
    uint32_t h = FNV1A_BASIS;

    for (; a && *a; a++)
        h = fnv1a_byte(h, (unsigned char)*a);
    h = fnv1a_byte(h, 0);
    for (; b && *b; b++)
        h = fnv1a_byte(h, (unsigned char)*b);
    return h;
}

/* A name at the end of the blob being filled; its offset into the blob, or
 * -1 when it does not fit. A value with no tag is UNTAGGED. */
static int add_name(int tag, long seek, char *blob, size_t *len, size_t cap)
{
    char name[TAGCACHE_BUFSZ];
    size_t n;

    if (!tagcache_seek_string(tag, seek, name, sizeof(name)))
        strmemccpy(name, UNTAGGED, sizeof(name));
    n = strlen(name) + 1;
    if (*len + n > cap)
        return -1;
    memcpy(blob + *len, name, n);
    *len += n;
    return *len - n;
}

/* ---- the artist and album lists ---------------------------------------- */

/* The artist half into *buf, advancing it: the array, then its names */
static int fill_artists(struct db_summary_t *t, char **buf, size_t *bufsz)
{
    struct tagcache_artist r;
    int nr = tagcache_artist_count(), n = 0;
    size_t len = 0, cap;
    char *p = ALIGN_UP(*buf, sizeof(long));
    size_t left = *bufsz - (p - *buf);

    for (int i = 0; i < nr; i++)
        if (tagcache_artist_get(i, &r) && !artist_hidden(&r))
            n++;
    if (n == 0)
        return ERROR_NO_ARTISTS;
    if (left < n * sizeof(struct artist_data))
        return ERROR_BUFFER_FULL;

    t->artist_index = (struct artist_data *)p;
    t->artist_names = p + n * sizeof(struct artist_data);
    cap = left - n * sizeof(struct artist_data);

    n = 0;
    for (int i = 0; i < nr; i++)
    {
        struct artist_data *d = &t->artist_index[n];
        int at;

        if (!tagcache_artist_get(i, &r) || artist_hidden(&r))
            continue;
        at = add_name(tag_albumartist, r.seek, t->artist_names, &len, cap);
        if (at < 0)
            return ERROR_BUFFER_FULL;
        d->name_idx = at;
        d->key = name_key(t->artist_names + at, NULL);
        d->playcount = r.playcount;
        d->lastplayed = r.lastplayed;
        d->seek = r.seek;
        d->art_hash = r.art_hash;
        n++;
    }
    t->artist_ct = n;
    t->artist_len = len;
    *bufsz -= (t->artist_names + len) - *buf;
    *buf = t->artist_names + len;
    return SUCCESS;
}

/* The listed artist with this seek, by its place in seek order */
static const struct artist_data *find_artist(const struct db_summary_t *t,
                                             long seek)
{
    int lo = 0, hi = t->artist_ct - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;

        if (t->artist_index[mid].seek == seek)
            return &t->artist_index[mid];
        if (t->artist_index[mid].seek < seek)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* The index's own order: by artist, then by album name */
static int compare_index_order(const void *a_v, const void *b_v)
{
    const struct album_data *a = a_v, *b = b_v;

    if (a->artist_idx != b->artist_idx)
        return a->artist_idx < b->artist_idx ? -1 : 1;
    return a->name_idx - b->name_idx;
}

static void stamp(struct db_summary_t *t)
{
    struct tagcache_marks marks;

    tagcache_get_marks(&marks);
    t->commitid = marks.commitid;
    t->serial = marks.serial;
    t->deleted = marks.deleted_ct;
}

int db_summary_build_into(struct db_summary_t *t, void *buf, size_t buf_sz)
{
    struct tagcache_album a;
    char *p = buf;
    size_t left = buf_sz, len = 0, cap;
    int na = tagcache_album_count(), n = 0, ret;

    memset(t, 0, sizeof(*t));
    t->album_untagged_seek = -1;
    if (na == 0)
        return ERROR_NO_ALBUMS;
    stamp(t);

    ret = fill_artists(t, &p, &left);
    if (ret == ERROR_BUFFER_FULL)
        return ret;
    if (ret != SUCCESS)
        t->artist_ct = 0;

    for (int i = 0; i < na; i++)
        if (tagcache_album_get(i, &a) && !album_hidden(&a))
            n++;
    if (n == 0)
        return ERROR_NO_ALBUMS;

    p = ALIGN_UP(p, sizeof(long));
    left = buf_sz - (p - (char *)buf);
    if (left < n * sizeof(struct album_data))
        return ERROR_BUFFER_FULL;
    t->album_index = (struct album_data *)p;
    t->album_names = p + n * sizeof(struct album_data);
    cap = left - n * sizeof(struct album_data);

    n = 0;
    for (int i = 0; i < na; i++)
    {
        struct album_data *d = &t->album_index[n];
        const struct artist_data *ar;
        int at;

        if (!tagcache_album_get(i, &a) || album_hidden(&a))
            continue;
        at = add_name(tag_album, a.album_seek, t->album_names, &len, cap);
        if (at < 0)
            return ERROR_BUFFER_FULL;
        ar = find_artist(t, a.artist_seek);
        d->name_idx = at;
        d->artist_idx = ar ? ar->name_idx : 0;
        d->key = name_key(t->album_names + at,
                          ar ? t->artist_names + ar->name_idx : NULL);
        d->year = a.year;
        d->playcount = a.playcount;
        d->lastplayed = a.lastplayed;
        d->artist_seek = a.artist_seek;
        d->seek = a.album_seek;
        d->art_hash = a.art_hash;
        d->artist_art_hash = a.artist_art_hash;
        if (t->album_untagged_seek < 0
            && !strcmp(t->album_names + at, UNTAGGED))
        {
            t->album_untagged_seek = a.album_seek;
            t->album_untagged_idx = at;
        }
        n++;
    }
    t->album_ct = n;
    t->album_len = len;
    qsort(t->album_index, n, sizeof(*t->album_index), compare_index_order);

    t->buf = t->album_names + len;
    t->buf_sz = buf_sz - ((char *)t->buf - (char *)buf);
    return SUCCESS;
}

int db_summary_load_artists(struct db_summary_t *t, void **buf,
                            size_t *bufsz)
{
    char *p = *buf;
    int ret;

    memset(t, 0, sizeof(*t));
    if (tagcache_artist_count() == 0)
        return ERROR_NO_ARTISTS;
    stamp(t);
    ret = fill_artists(t, &p, bufsz);
    if (ret == SUCCESS)
        *buf = p;
    return ret;
}

/* ---- single albums, for a caller with no buffer ------------------------ */

int db_summary_reader_open(struct db_summary_reader *r)
{
    struct tagcache_album a;
    int na = tagcache_album_count();

    r->album_ct = 0;
    for (int i = 0; i < na; i++)
        if (tagcache_album_get(i, &a) && !album_hidden(&a))
            r->album_ct++;
    return r->album_ct ? SUCCESS : ERROR_NO_ALBUMS;
}

void db_summary_reader_close(struct db_summary_reader *r)
{
    r->album_ct = 0;
}

bool db_summary_read_album(struct db_summary_reader *r, int n,
                           struct album_data *out)
{
    struct tagcache_album a;
    int na = tagcache_album_count();

    if (n < 0 || n >= r->album_ct)
        return false;
    for (int i = 0; i < na; i++)
    {
        if (!tagcache_album_get(i, &a) || album_hidden(&a) || n-- > 0)
            continue;
        memset(out, 0, sizeof(*out));
        out->year = a.year;
        out->playcount = a.playcount;
        out->lastplayed = a.lastplayed;
        out->artist_seek = a.artist_seek;
        out->seek = a.album_seek;
        out->art_hash = a.art_hash;
        out->artist_art_hash = a.artist_art_hash;
        return true;
    }
    return false;
}

/* ---- the order table for album lists ----------------------------------- */

/* Artist rows by name, while the ranks are worked out */
static bool order_articles;

static int compare_artist_rows(const void *a_v, const void *b_v)
{
    struct tagcache_artist a, b;
    char an[TAGCACHE_BUFSZ], bn[TAGCACHE_BUFSZ];
    int ra = *(const int *)a_v, rb = *(const int *)b_v;

    if (!tagcache_artist_get(ra, &a) || !tagcache_artist_get(rb, &b)
        || !tagcache_seek_string(tag_albumartist, a.seek, an, sizeof(an))
        || !tagcache_seek_string(tag_albumartist, b.seek, bn, sizeof(bn)))
        return ra - rb;
    int res = order_articles ? strcasecmp(tagcache_sort_name(an),
                                          tagcache_sort_name(bn))
                             : 0;
    return res ? res : ra - rb;
}

int db_summary_read_order_table(struct db_summary_order *out, int max,
                                bool ignore_articles)
{
    struct tagcache_album a;
    int na = tagcache_album_count(), nr = tagcache_artist_count();
    int n = 0;
    int *rank = NULL;

    if (na == 0)
        return ERROR_NO_ALBUMS;

    /* One entry an album name, the albums under it sharing their name's: the
     * latest year among them, and the first artist's place */
    for (int i = 0; i < na; i++)
    {
        if (!tagcache_album_get(i, &a) || album_hidden(&a))
            continue;
        if (n > 0 && out[n - 1].seek == a.album_seek)
        {
            if (a.year > out[n - 1].year)
                out[n - 1].year = a.year;
            continue;
        }
        if (n == max)
            return ERROR_BUFFER_FULL;
        out[n].seek = a.album_seek;
        out[n].year = a.year;
        out[n].artist = tagcache_artist_find(a.artist_seek);
        n++;
    }

    /* Artists are in name order already, articles and all, being in seek
     * order. Stepping past articles needs them sorted again, in the space the
     * table does not use; without room they keep the database's order. */
    if (ignore_articles && nr > 0
        && (size_t)(max - n) * sizeof(*out) >= 2 * nr * sizeof(int))
    {
        int *order = (int *)&out[n];

        rank = order + nr;
        for (int i = 0; i < nr; i++)
            order[i] = i;
        order_articles = true;
        qsort(order, nr, sizeof(*order), compare_artist_rows);
        for (int i = 0; i < nr; i++)
            rank[order[i]] = i;
    }

    for (int i = 0; i < n; i++)
    {
        int r = out[i].artist;

        if (r < 0)
            out[i].artist = DB_SUMMARY_NO_ARTIST;
        else
            out[i].artist = MIN(rank ? rank[r] : r, DB_SUMMARY_NO_ARTIST - 1);
    }
    return n;
}

/* ---- playing an album -------------------------------------------------- */

/* One track, while the album is being put in order. Only the index id is kept,
 * not the path: a path is MAX_PATH and there is no reason to hold every one of
 * them when tagcache_retrieve() can fetch each again in the order wanted. */
struct album_track {
    int32_t idx_id;
    int32_t key;
};

static int compare_album_tracks(const void *a_v, const void *b_v)
{
    const struct album_track *a = a_v;
    const struct album_track *b = b_v;
    return a->key - b->key;
}

int db_summary_play_album(const struct album_data *album)
{
    struct tagcache_search tcs;
    struct playlist_insert_context context;
    char buf[MAX_PATH];
    struct album_track *list;
    size_t list_sz = 0;
    int cap, found = 0, added = 0;
    bool sorted;

    if (!warn_on_pl_erase())
        return -1;
    if (playlist_create(NULL, NULL) < 0)
        return -1;

    cpu_boost(true);
    if (!tagcache_search(&tcs, tag_filename))
    {
        cpu_boost(false);
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        return -1;
    }
    if (playlist_insert_context_create(NULL, &context, PLAYLIST_INSERT_LAST,
                                       false, false) < 0)
    {
        /* create() keeps the playlist lock even when it fails; release()
         * is the only thing that gives it back. */
        playlist_insert_context_release(&context);
        tagcache_search_finish(&tcs);
        cpu_boost(false);
        return -1;
    }

    /* The album's tracks: its name, and its artist where it has one. */
    tagcache_search_add_filter(&tcs, tag_album, album->seek);
    if (album->artist_idx >= 0)
        tagcache_search_add_filter(&tcs, tag_albumartist, album->artist_seek);

    /* A search returns entries in master-index order -- the order the files
     * were scanned, which is close to track order for most rips and wrong for
     * the rest. The browser's track list is sorted by tagnavi's
     * "%02d%04d%s" discnum/tracknum format, and playing an album has to come
     * out in the order that list shows, so collect the album first and sort it
     * before any of it reaches the playlist. */
    list = app_get_buffer(&list_sz, "album play");
    cap = list ? (int)(list_sz / sizeof(*list)) : 0;
    sorted = (cap > 0);

    while (tagcache_get_next(&tcs, buf, sizeof(buf)))
    {
        int disc, track;

        if (found >= cap)
        {
            /* Nowhere to put the rest of the album, so it cannot be ordered
             * as a whole. Play it in search order rather than not at all. */
            sorted = false;
            break;
        }
        disc  = tagcache_get_numeric(&tcs, tag_discnumber);
        track = tagcache_get_numeric(&tcs, tag_tracknumber);
        if (disc < 0)
            disc = 0;
        if (track < 0)
            track = 0;
        list[found].idx_id = tcs.idx_id;
        /* Disc above track, so disc 2's track 1 follows disc 1's last. An
         * untagged disc or track sorts as 0, which puts it first -- the same
         * place the browser's format string puts it. Both are masked: these
         * come from file tags, so a nonsense value must not shift into the
         * sign bit and sort the track to the front. */
        list[found].key = ((disc & 0x7fff) << 16) | (track & 0xffff);
        found++;
    }

    if (sorted)
    {
        qsort(list, found, sizeof(*list), compare_album_tracks);
        for (int i = 0; i < found; i++)
        {
            if (!tagcache_retrieve(&tcs, list[i].idx_id, tag_filename,
                                   buf, sizeof(buf)))
                continue;
            if (playlist_insert_context_add(&context, buf) < 0)
                break;
            added++;
        }
    }
    else
    {
        /* Restart: the walk above consumed part or all of the search. */
        tagcache_search_finish(&tcs);
        if (tagcache_search(&tcs, tag_filename))
        {
            tagcache_search_add_filter(&tcs, tag_album, album->seek);
            if (album->artist_idx >= 0)
                tagcache_search_add_filter(&tcs, tag_albumartist,
                                           album->artist_seek);
            while (tagcache_get_next(&tcs, buf, sizeof(buf)))
            {
                if (playlist_insert_context_add(&context, buf) < 0)
                    break;
                added++;
            }
        }
    }

    playlist_insert_context_release(&context);
    tagcache_search_finish(&tcs);
    cpu_boost(false);

    if (added <= 0)
        return -1;

    playlist_start(0, 0, 0);
    return added;
}

/* The album the carousel asked for, held across its teardown. */
static struct album_data pending_play;
static bool pending_play_armed;

void db_summary_play_album_on_exit(const struct album_data *album)
{
    pending_play = *album;
    pending_play_armed = true;
}

int db_summary_play_pending(void)
{
    if (!pending_play_armed)
        return -1;
    pending_play_armed = false;
    return db_summary_play_album(&pending_play);
}
