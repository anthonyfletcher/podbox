/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * What a [Name] in a page links to. Names match by their keys (art_name.h),
 * so a link is found however it is spelt, and is looked for in this order:
 *
 *   1. a page whose whole name it is: [Spoon - Hot Thoughts]
 *   2. a person's page: [Steve Gadd]
 *   3. an album page by its title alone: [Hot Thoughts]
 *   4. a page in another folder of /Notes: [Electric Lady]
 *   5. the library: an album artist, else an album of that title
 *   6. a name two or more pages give, whose page lists where
 *
 * Anything else is plain text.
 * Two albums of one title go to the one by the artist of the page the link
 * is on, else the oldest. A trailing (qualifier) is part of the name, except
 * the four that say what kind of thing is meant: (album), (person),
 * (track), which finds a track by its title -- on the page's album, then
 * the page's artist's, then anywhere -- and (section), a heading of the page
 * the link is on. A piece's track is written "(track N)", N its database
 * entry, as titles repeat across albums and a search for one reads every
 * title in the library.
 *
 * The library's side comes from the database's RAM copy alone: its album
 * table, with each album's keys worked out once for a screen into the
 * caller's buffer, and its tracks' titles. Nothing here reads the disk.
 *
 * Parts, in order:
 *   - the library's albums, worked out for a screen
 *   - album artists, the split ones of Split Album Artists among them
 *   - finding a link
 ****************************************************************************/

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "config.h"
#include "system.h"
#include "kernel.h"                 /* yield */
#include "string-extra.h"
#include "database/tagcache.h"
#include "metadata/art_name.h"
#include "metadata/tag_trim.h"
#include "metadata/name_index.h"
#include "database/db_names.h"
#include "notes_files.h"
#include "notes_links.h"

/* ---- the library's albums, worked out for a screen --------------------- */

/* A row of the album table, as links match it */
struct nl_album
{
    uint32_t title, trimmed;    /* its title, and through Trim Titles */
    uint32_t work, twork;       /* its artist and title, both ways */
    uint32_t artist;            /* its album artist's name key */
    int32_t year;
};

static struct
{
    struct nl_album *a;
    int n;
    const struct name_index *ix;
} nl;

static char nl_buf[256], nl_trim[256];

/* The album the last track link was found on: a list's tracks come an
 * album at a time, so it is tried next */
static int nl_last_album = -1;

/* A link to a track found nowhere nearer reads every title in the library:
 * a page's first NL_ANYWHERE_MAX such links look, and the rest are plain */
#define NL_ANYWHERE_MAX 8
static int nl_anywhere;

void notes_breathe(void)
{
    static long next;

    if (TIME_AFTER(current_tick, next))
    {
        yield();
        next = current_tick + HZ / 50;
    }
}

size_t notes_links_need(void)
{
    int n = tagcache_album_count();

    return n > 0 ? n * sizeof(struct nl_album) : 0;
}

/* tag `tag` of seek `seek` into `buf`, "" for <Untagged> or none */
static bool nl_string(int tag, long seek, char *buf, size_t size)
{
    if (seek < 0 || !tagcache_seek_string(tag, seek, buf, size)
        || !strcmp(buf, UNTAGGED))
    {
        buf[0] = '\0';
        return false;
    }
    return true;
}

bool notes_links_begin(void *buf, size_t size, const struct name_index *ix)
{
    static char artist[256];
    int i, n = tagcache_album_count();

    nl.a = NULL;
    nl.n = 0;
    nl.ix = ix;
    nl_last_album = -1;
    nl_anywhere = 0;
    if (n <= 0 || n * sizeof(struct nl_album) > size)
        return false;
    nl.a = buf;
    for (i = 0; i < n; i++)
    {
        struct nl_album *e = &nl.a[i];
        struct tagcache_album row;
        const char *t;

        notes_breathe();
        memset(e, 0, sizeof(*e));
        if (!tagcache_album_get(i, &row)
            || !nl_string(tag_album, row.album_seek, nl_buf, sizeof(nl_buf)))
            continue;
        nl_string(tag_albumartist, row.artist_seek, artist, sizeof(artist));
        t = tag_trim(nl_buf, nl_trim, sizeof(nl_trim));
        e->title = art_name_hash(nl_buf);
        e->trimmed = art_name_hash(t);
        e->work = art_name_work_hash(artist[0] ? artist : NULL, nl_buf);
        e->twork = art_name_work_hash(artist[0] ? artist : NULL, t);
        e->artist = row.artist_art_hash;
        e->year = row.year;
    }
    nl.n = n;
    return true;
}

void notes_links_end(void)
{
    nl.a = NULL;
    nl.n = 0;
    nl.ix = NULL;
}

/* Whether two or more pages name `key` */
static bool nl_named_twice(uint32_t key)
{
    int i, first = -1;

    for (i = 0; nl.ix && i < nl.ix->count; i++)
        if (nl.ix->m[i].key == key)
        {
            if (first < 0)
                first = nl.ix->m[i].file;
            else if (nl.ix->m[i].file != first)
                return true;
        }
    return false;
}

/* Whether `b` is the older of two albums, an undated one never */
static bool nl_older(int b, int than)
{
    return than < 0 || (nl.a[b].year > 0
                        && (nl.a[than].year <= 0
                            || nl.a[b].year < nl.a[than].year));
}

/* The album titled `key`: by `artist` when one is, else the oldest */
static int nl_album_titled(uint32_t key, uint32_t artist)
{
    int i, best = -1;

    for (i = 0; i < nl.n; i++)
    {
        if (!key || (nl.a[i].title != key && nl.a[i].trimmed != key))
            continue;
        if (artist && nl.a[i].artist == artist)
            return i;
        if (nl_older(i, best))
            best = i;
    }
    return best;
}

static int nl_album_work(uint32_t key)
{
    int i;

    for (i = 0; key && i < nl.n; i++)
        if (nl.a[i].work == key || nl.a[i].twork == key)
            return i;
    return -1;
}

int notes_page_album(int page)
{
    const struct notes_page *pg = notes_page_get(page);
    struct tagcache_album row;
    int i;

    if (!pg || pg->kind != NOTES_PAGE_ALBUM || !nl.n)
        return -1;
    if (pg->dir)
    {
        for (i = 0; i < nl.n; i++)
            if (tagcache_album_get(i, &row) && tagcache_album_dir_key(&row) == pg->dir)
                return i;
        return -1;
    }
    i = nl_album_work(pg->key);
    return i >= 0 || pg->by ? i : nl_album_titled(pg->part, 0);
}

bool notes_album_title(int album, char *buf, size_t size)
{
    struct tagcache_album row;
    const char *t;

    if (!tagcache_album_get(album, &row)
        || !nl_string(tag_album, row.album_seek, nl_buf, sizeof(nl_buf)))
        return false;
    t = tag_trim(nl_buf, nl_trim, sizeof(nl_trim));
    strlcpy(buf, t, size);
    return true;
}

bool notes_album_artist(int album, char *buf, size_t size)
{
    struct tagcache_album row;

    return tagcache_album_get(album, &row)
           && nl_string(tag_albumartist, row.artist_seek, buf, size);
}

/* ---- album artists, the split ones of Split Album Artists among them --- */

static bool nl_key_is(uint32_t h, uint32_t key, uint32_t alt, uint32_t part)
{
    return h && (h == key || h == alt || h == part);
}

/* A split artist with no album of their own has no row in the artist table,
 * so the parts are looked through by name after it */
long notes_artist_seek(uint32_t key, uint32_t alt, uint32_t part,
                       uint32_t *art_hash)
{
    struct tagcache_artist ar;
    bool split = db_names_active();
    int i, n = tagcache_artist_count();

    for (i = 0; i < n; i++)
        if (tagcache_artist_get(i, &ar)
            && nl_key_is(ar.art_hash, key, alt, part))
        {
            if (art_hash)
                *art_hash = ar.art_hash;
            return split ? db_names_browse_seek(ar.seek) : ar.seek;
        }
    for (i = 0, n = split ? db_names_count() : 0; i < n; i++)
    {
        const char *name = db_names_name(i);
        uint32_t h = name ? art_name_hash(name) : 0;

        if (nl_key_is(h, key, alt, part))
        {
            if (art_hash)
                *art_hash = h;
            return db_names_handle(i);
        }
    }
    return -1;
}

bool notes_is_artist(long seek)
{
    return seek >= 0 || db_names_is_handle(seek);
}

bool notes_album_by(int album, long seek)
{
    struct tagcache_album row;

    return tagcache_album_get(album, &row)
           && (row.artist_seek == seek
               || (db_names_is_handle(seek)
                   && db_names_match(seek, row.artist_seek)));
}

/* ---- finding a link ---------------------------------------------------- */

#define NL_ANY    0
#define NL_ALBUM  1
#define NL_PERSON 2
#define NL_TRACK  3
#define NL_SECTION 4

/* The reserved qualifier `len` bytes of `name` end with, NL_ANY for none;
 * a list's "(track N)" is NL_TRACK with the database entry N in `entry`,
 * else -1 */
static int nl_reserved(const char *name, size_t len, size_t bare, int *entry)
{
    static const char *const words[] = { "album", "person", "track",
                                         "section" };
    const char *q = name + bare + 2;
    size_t qlen = len - bare - 3;
    unsigned i;

    *entry = -1;
    if (bare == len)
        return NL_ANY;
    for (i = 0; i < ARRAYLEN(words); i++)
        if (qlen == strlen(words[i]) && !strncasecmp(q, words[i], qlen))
            return i + 1;
    if (qlen > 6 && qlen <= 6 + 9 && !strncasecmp(q, "track ", 6))
    {
        int n = 0;

        for (i = 6; i < qlen && q[i] >= '0' && q[i] <= '9'; i++)
            n = n * 10 + q[i] - '0';
        if (i == qlen)
        {
            *entry = n;
            return NL_TRACK;
        }
    }
    return NL_ANY;
}

const char *notes_dash(const char *name, size_t len)
{
    const char *s;

    for (s = name; s + 3 <= name + len; s++)
        if (!strncmp(s, " - ", 3))
            return s;
    return NULL;
}

/* The key of "Artist - Title": `len` bytes of `name`, parted at `dash` */
static uint32_t nl_work_key(const char *name, size_t len, const char *dash)
{
    static char a[256], b[256];

    snprintf(a, sizeof(a), "%.*s", (int)(dash - name), name);
    snprintf(b, sizeof(b), "%.*s", (int)(name + len - dash - 3), dash + 3);
    return art_name_work_hash(a, b);
}

/* Whether entry `idx` is titled `key`, as tagged or through Trim Titles */
static bool nl_titled(int idx, uint32_t key)
{
    const char *t;

    if (!tagcache_entry_string(idx, tag_title, nl_buf, sizeof(nl_buf)))
        return false;
    if (art_name_hash(nl_buf) == key)
        return true;
    t = tag_trim(nl_buf, nl_trim, sizeof(nl_trim));
    return t != nl_buf && art_name_hash(t) == key;
}

/* The track of album row `album` titled `key`, or -1 */
static int nl_track_in(int album, uint32_t key)
{
    struct tagcache_album row;
    int idx;

    if (album < 0 || !tagcache_album_get(album, &row))
        return -1;
    for (idx = row.first; idx <= row.last; idx++)
    {
        notes_breathe();
        if (tagcache_album_of(idx) == album && nl_titled(idx, key))
            return idx;
    }
    return -1;
}

static bool nl_track_at(int album, uint32_t key, struct notes_target *t)
{
    if ((t->track = nl_track_in(album, key)) < 0)
        return false;
    t->album = nl_last_album = album;
    return true;
}

/* A track titled `key`: on `album`, then `artist`'s albums, then for
 * `anywhere` the rest */
static bool nl_track(uint32_t key, uint32_t artist, int album, bool anywhere,
                     struct notes_target *t)
{
    int i, last = nl_last_album;

    if (nl_track_at(album, key, t)
        || (last != album && last < nl.n && nl_track_at(last, key, t)))
        return true;
    for (i = 0; artist && i < nl.n; i++)
        if (i != album && i != last && nl.a[i].artist == artist
            && nl_track_at(i, key, t))
            return true;
    if (anywhere && nl_anywhere++ >= NL_ANYWHERE_MAX)
        return false;
    for (i = 0; anywhere && i < nl.n; i++)
        if (i != album && i != last
            && !(artist && nl.a[i].artist == artist)
            && nl_track_at(i, key, t))
            return true;
    return false;
}

/* The best page of `kind` (0 for any) that `match` takes */
static int nl_page(int kind, bool (*match)(const struct notes_page *,
                                           const void *), const void *arg)
{
    int i, best = -1, n = notes_page_count();

    for (i = 0; i < n; i++)
    {
        const struct notes_page *pg = notes_page_get(i);

        if ((!kind || pg->kind == kind) && match(pg, arg)
            && notes_page_better(pg, best < 0 ? NULL : notes_page_get(best)))
            best = i;
    }
    return best;
}

struct nl_keys
{
    uint32_t key, alt, work, artist;
};

/* An album page named for its title alone is found by its title, where the
 * page's artist decides between two */
static bool nl_whole(const struct notes_page *pg, const void *arg)
{
    const struct nl_keys *k = arg;

    if (pg->kind == NOTES_PAGE_ALBUM && !pg->by)
        return false;
    return pg->key == k->key || (k->work && pg->key == k->work);
}

static bool nl_person(const struct notes_page *pg, const void *arg)
{
    const struct nl_keys *k = arg;

    return pg->key == k->key || pg->alt == k->key || pg->part == k->key
           || (k->alt && (pg->key == k->alt || pg->alt == k->alt));
}

static bool nl_other(const struct notes_page *pg, const void *arg)
{
    const struct nl_keys *k = arg;

    return pg->key == k->key || pg->part == k->key;
}

/* An album page titled k->key: by k->artist when one is, else the oldest;
 * of two pages of one album, the better (notes_page_better()) */
static int nl_album_page(const struct nl_keys *k)
{
    int i, best = -1, best_row = -1, by = -1, n = notes_page_count();

    for (i = 0; i < n; i++)
    {
        const struct notes_page *pg = notes_page_get(i);
        int row;

        if (pg->kind != NOTES_PAGE_ALBUM || pg->part != k->key)
            continue;
        if (k->artist && pg->by == k->artist)
        {
            if (notes_page_better(pg, by < 0 ? NULL : notes_page_get(by)))
                by = i;
            continue;
        }
        row = notes_page_album(i);
        if (best < 0 || (row >= 0 && row != best_row && nl_older(row, best_row))
            || (row == best_row && notes_page_better(pg, notes_page_get(best))))
        {
            best = i;
            best_row = row;
        }
    }
    return by >= 0 ? by : best;
}

/* A written page: an album's with its library album, a person's with the
 * library album artist it is about */
static bool nl_found_page(int page, struct notes_target *t)
{
    const struct notes_page *pg = notes_page_get(page);

    if (!pg)
        return false;
    t->to = NOTES_TO_PAGE;
    t->page = page;
    t->album = notes_page_album(page);
    if (pg->kind == NOTES_PAGE_PERSON)
        t->artist_seek = notes_artist_seek(pg->key, pg->alt, pg->part, NULL);
    return true;
}

bool notes_link_find(const char *name, size_t len, uint32_t artist,
                     int album, struct notes_target *t)
{
    size_t bare = notes_bare_len(name, len);
    int entry, kind = nl_reserved(name, len, bare, &entry), i;
    const char *dash;
    struct nl_keys k;

    t->to = NOTES_TO_NONE;
    t->page = t->album = t->track = -1;
    t->artist_seek = -1;
    if (kind != NL_ANY)
        len = bare;
    dash = notes_dash(name, len);
    k.key = t->key = art_name_hash_n(name, len);
    k.alt = art_name_alt_hash_n(name, len);
    k.artist = artist;
    k.work = 0;
    if (!k.key)
        return false;

    if (kind == NL_SECTION)
    {
        t->to = NOTES_TO_SECTION;
        return true;
    }
    if (kind == NL_TRACK)
    {
        /* The entry a list named, while it still has that title; else the
         * whole name: a title may hold " - " of its own, as "Pink Up -
         * Live" does; then "Artist - Title" */
        if ((i = tagcache_album_of(entry)) >= 0 && nl_titled(entry, k.key))
        {
            t->track = entry;
            t->album = nl_last_album = i;
        }
        else if (!nl_track(k.key, artist, album, !dash, t)
            && !(dash && nl_track(art_name_hash_n(dash + 3,
                                                  name + len - dash - 3),
                                  art_name_hash_n(name, dash - name), -1,
                                  true, t))
            && !(dash && nl_track(k.key, 0, -1, true, t)))
            return false;
        t->to = NOTES_TO_TRACK;
        t->page = -1;
        return true;
    }
    if (dash)
        k.work = nl_work_key(name, len, dash);

    /* 1 to 4: the pages */
    if (nl_found_page(nl_page(kind == NL_ALBUM ? NOTES_PAGE_ALBUM
                              : kind == NL_PERSON ? NOTES_PAGE_PERSON : 0,
                              nl_whole, &k), t))
        return true;
    if (kind != NL_ALBUM
        && nl_found_page(nl_page(NOTES_PAGE_PERSON, nl_person, &k), t))
        return true;
    if (kind != NL_PERSON && nl_found_page(nl_album_page(&k), t))
        return true;
    if (kind == NL_ANY
        && nl_found_page(nl_page(NOTES_PAGE_OTHER, nl_other, &k), t))
        return true;

    /* 5: the library */
    if (kind != NL_ALBUM
        && (t->artist_seek = notes_artist_seek(k.key, k.alt, 0, NULL)) != -1)
    {
        t->to = NOTES_TO_ARTIST;
        return true;
    }
    if (kind != NL_PERSON)
    {
        t->album = k.work ? nl_album_work(k.work) : -1;
        if (t->album < 0)
            t->album = nl_album_titled(k.key, artist);
        if (t->album >= 0)
        {
            t->to = NOTES_TO_ALBUM;
            return true;
        }
    }

    /* 6: a name more than one page gives */
    if (kind == NL_ANY && nl_named_twice(k.key))
    {
        t->to = NOTES_TO_NAME;
        return true;
    }
    return false;
}

int notes_album_named(const char *name, size_t len, uint32_t artist)
{
    const char *dash = notes_dash(name, len);
    int i;

    if (dash && (i = nl_album_work(nl_work_key(name, len, dash))) >= 0)
        return i;
    return nl_album_titled(art_name_hash_n(name, len), artist);
}
