/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The page index: every page of the notes, where it is and the keys of its
 * name. A page is a .md or .txt file anywhere in /Notes -- People/ a person,
 * Albums/<Artist - Album> an album, any other folder anything else -- or
 * one beside the music, named as though it were in /Notes: bio.md in a
 * folder is the person the folder is named for, and notes.md in
 * <Artist>/<Album> the page "Artist - Album". Names are matched by the keys
 * /Artwork's pictures are (art_name.h), so capitals, accents, symbols, "The"
 * and "Surname, First" do not matter, and a page in /Notes wins over the
 * same page beside the music.
 *
 * The index is built by the file index's walk (file_index.c), which passes
 * every one of these files, saved, and read at boot: so a lookup reads no
 * folder, and the context menus' notes rows cost nothing to ask about.
 * Until a walk has written it, there are no pages. A walk stages what it
 * finds in a file and the index in memory is replaced only when the walk
 * completes, so the pages answer throughout it; for the moment of the swap
 * notes_page_get() returns NULL, which every caller checks.
 *
 * Parts, in order:
 *   - the index: built by the walk, saved, read at boot, asked
 *   - the lookups for a track and an album
 ****************************************************************************/

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "config.h"
#include "string-extra.h"
#include "file.h"
#include "dir.h"
#include "database/tagcache.h"
#include "database/path_key.h"
#include "system/library_files.h"
#include "settings/settings.h"
#include "metadata/art_name.h"
#include "metadata/name_index.h"   /* name_index_pages_changed */
#include "system/debug_log.h"
#include "notes_files.h"
#include "notes_links.h"            /* notes_is_artist */

/* ---- the index --------------------------------------------------------- */

#define NW_FILE      LIB_CACHE_DIR "/notes.dat"
#define NW_STAGE     LIB_CACHE_DIR "/notes.new"  /* a walk's pages so far */
#define NW_MAGIC     0x4e4f5433     /* "NOT3" */
#define NW_PAGES_MAX 2048
#define NW_POOL      (64 * 1024)    /* their paths */

static struct
{
    bool loaded;                /* the index answers; else there are none */
    int npages, pool_used;
    struct notes_page pages[NW_PAGES_MAX];
    char pool[NW_POOL];
} nw;

struct nw_header
{
    uint32_t magic;
    int32_t npages, pool_used;
};

/* The size and time of the file the walk is passing */
static uint32_t nw_size, nw_mtime;

/* The walk's staging file: each page as a struct notes_page, its `path`
 * the length of the path that follows it */
static int nw_stage = -1;
static int nw_staged, nw_staged_pool;

/* A text file's extension: 2 for .md, 1 for .txt, 0 for anything else */
static int notes_ext_rank(const char *dot)
{
    if (!dot)
        return 0;
    if (!strcasecmp(dot, ".md"))
        return 2;
    return !strcasecmp(dot, ".txt");
}

size_t notes_bare_len(const char *name, size_t len)
{
    size_t i;

    if (len < 4 || name[len - 1] != ')')
        return len;
    for (i = len - 1; i > 0 && name[i] != '('; i--)
        ;
    return i > 1 && name[i - 1] == ' ' ? i - 1 : len;
}

/* The key of "artist - title" given as two runs of bytes; the title's alone
 * with no artist */
static unsigned int nw_work_key(const char *artist, size_t alen,
                                const char *title, size_t tlen)
{
    static char a[MAX_PATH], t[MAX_PATH];

    snprintf(a, sizeof(a), "%.*s", (int)alen, artist);
    snprintf(t, sizeof(t), "%.*s", (int)tlen, title);
    return art_name_work_hash(alen ? a : NULL, t);
}

bool notes_where_load(void)
{
    struct nw_header h;
    int fd = open(NW_FILE, O_RDONLY);
    bool ok;

    nw.loaded = false;
    if (fd < 0)
        return false;
    ok = read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h) && h.magic == NW_MAGIC
         && h.npages >= 0 && h.npages <= NW_PAGES_MAX
         && h.pool_used >= 0 && h.pool_used <= NW_POOL
         && read(fd, nw.pages, h.npages * sizeof(nw.pages[0]))
            == (ssize_t)(h.npages * sizeof(nw.pages[0]))
         && read(fd, nw.pool, h.pool_used) == h.pool_used;
    close(fd);
    if (ok)
    {
        nw.npages = h.npages;
        nw.pool_used = h.pool_used;
        nw.loaded = true;
    }
    return ok;
}

void notes_where_begin(void)
{
    if (nw_stage >= 0)
        close(nw_stage);
    nw_stage = open(NW_STAGE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    nw_staged = nw_staged_pool = 0;
}

static void nw_add(int kind, uint32_t key, uint32_t alt, uint32_t part,
                   uint32_t by, uint64_t dir, const char *path, int rank)
{
    struct notes_page pg;
    size_t size = strlen(path) + 1;

    if (!key || nw_stage < 0 || nw_staged == NW_PAGES_MAX
        || nw_staged_pool + size > NW_POOL)
        return;
    memset(&pg, 0, sizeof(pg));
    pg.key = key;
    pg.alt = alt;
    pg.part = part;
    pg.by = by;
    pg.dir = dir;
    pg.path = size;
    pg.size = nw_size;
    pg.mtime = nw_mtime;
    pg.kind = kind;
    pg.rank = rank;
    if (write(nw_stage, &pg, sizeof(pg)) != (ssize_t)sizeof(pg)
        || write(nw_stage, path, size) != (ssize_t)size)
    {
        close(nw_stage);            /* a short index would lose pages */
        nw_stage = -1;
        return;
    }
    nw_staged++;
    nw_staged_pool += size;
}

/* The staging file into the index in memory; false when it cannot be read
 * whole */
static bool nw_unstage(void)
{
    int fd = open(NW_STAGE, O_RDONLY), i;
    bool ok = fd >= 0;

    nw.npages = nw.pool_used = 0;
    for (i = 0; ok && i < nw_staged; i++)
    {
        struct notes_page *pg = &nw.pages[i];
        uint32_t size;

        ok = read(fd, pg, sizeof(*pg)) == (ssize_t)sizeof(*pg)
             && (size = pg->path) > 0 && nw.pool_used + size <= NW_POOL
             && read(fd, nw.pool + nw.pool_used, size) == (ssize_t)size;
        if (!ok)
            break;
        nw.pool[nw.pool_used + size - 1] = '\0';
        pg->path = nw.pool_used;
        nw.pool_used += size;
        nw.npages++;
    }
    if (fd >= 0)
        close(fd);
    return ok;
}

/* A page in /Notes: `rel` is its path under /Notes, `leaf` its file name
 * and `len` that without the extension */
static void nw_found_in_notes(const char *path, const char *rel,
                              const char *leaf, size_t len, int rank)
{
    const char *slash = strchr(rel, '/');
    size_t flen = slash && slash < leaf ? (size_t)(slash - rel) : 0;
    size_t bare = notes_bare_len(leaf, len);
    uint32_t part = bare < len ? art_name_hash_n(leaf, bare) : 0;

    if (flen == 6 && !strncasecmp(rel, "People", 6))
        nw_add(NOTES_PAGE_PERSON, art_name_hash_n(leaf, len),
               art_name_alt_hash_n(leaf, len), part, 0, 0, path, rank);
    else if (flen == 6 && !strncasecmp(rel, "Albums", 6))
    {
        const char *dash = NULL, *s;

        for (s = leaf; s + 3 <= leaf + len; s++)
            if (!strncmp(s, " - ", 3))
            {
                dash = s;
                break;
            }
        if (dash)
            nw_add(NOTES_PAGE_ALBUM,
                   nw_work_key(leaf, dash - leaf, dash + 3,
                               leaf + len - dash - 3),
                   0, art_name_hash_n(dash + 3, leaf + len - dash - 3),
                   art_name_hash_n(leaf, dash - leaf), 0, path, rank);
        else
            nw_add(NOTES_PAGE_ALBUM, nw_work_key(NULL, 0, leaf, len), 0,
                   art_name_hash_n(leaf, len), 0, 0, path, rank);
    }
    else
        nw_add(NOTES_PAGE_OTHER, art_name_hash_n(leaf, len),
               art_name_alt_hash_n(leaf, len), part,
               flen ? art_name_hash_n(rel, flen) : 0, 0, path, rank);
}

/* bio.* or notes.* beside the music, in the folder `folder` (`flen` bytes
 * of `p`, its last component `name`) */
static void nw_found_beside(const char *path, const char *p, size_t flen,
                            bool bio, int rank)
{
    static char dir[MAX_PATH];
    const char *name = p + flen, *parent;
    uint64_t key;

    while (name > p && name[-1] != '/')
        name--;
    if (name == p + flen)
        return;
    snprintf(dir, sizeof(dir), "%.*s", (int)flen, p);
    key = path_key(dir);
    if (bio)
    {
        nw_add(NOTES_PAGE_PERSON, art_name_hash_n(name, p + flen - name),
               art_name_alt_hash_n(name, p + flen - name), 0, 0, key, path,
               rank);
        return;
    }
    for (parent = name - 1; parent > p && parent[-1] != '/'; parent--)
        ;
    nw_add(NOTES_PAGE_ALBUM,
           nw_work_key(parent, name > parent ? name - 1 - parent : 0,
                       name, p + flen - name),
           0, art_name_hash_n(name, p + flen - name),
           name > parent ? art_name_hash_n(parent, name - 1 - parent) : 0,
           key, path, rank);
}

void notes_where_found(const char *path, uint32_t size, uint32_t mtime)
{
    const char *p = path_key_strip(path), *leaf = strrchr(p, '/'), *dot;
    size_t len;
    int rank;

    nw_size = size;
    nw_mtime = mtime;
    if (!leaf)
        return;
    leaf++;
    dot = strrchr(leaf, '.');
    if (!(rank = notes_ext_rank(dot)) || leaf[0] == '.')
        return;
    len = dot - leaf;
    if (!strncasecmp(p, NOTES_DIR "/", sizeof(NOTES_DIR)))
        nw_found_in_notes(path, p + sizeof(NOTES_DIR), leaf, len, rank);
    else if ((len == 5 && !strncasecmp(leaf, "notes", 5))
             || (len == 3 && !strncasecmp(leaf, "bio", 3)))
        nw_found_beside(path, p, leaf - 1 - p, len == 3, rank);
}

void notes_where_end(bool completed)
{
    struct nw_header h;
    bool staged = nw_stage >= 0;
    bool ok = false;
    int fd;

    if (staged)
        close(nw_stage);
    nw_stage = -1;
    /* An interrupted walk, or one whose staging failed, keeps the last
     * complete index */
    if (!completed || !staged)
    {
        remove(NW_STAGE);
        if (completed)
            debug_log(DEBUG_LOG_TASKS, "notes: %s not written", NW_STAGE);
        return;
    }

    nw.loaded = false;
    if (!nw_unstage())
    {
        remove(NW_STAGE);
        debug_log(DEBUG_LOG_TASKS, "notes: %s not read", NW_STAGE);
        notes_where_load();
        return;
    }
    nw.loaded = true;
    remove(NW_STAGE);

    h.magic = NW_MAGIC;
    h.npages = nw.npages;
    h.pool_used = nw.pool_used;
    fd = open(NW_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        ok = write(fd, &h, sizeof(h)) == (ssize_t)sizeof(h)
            && write(fd, nw.pages, h.npages * sizeof(nw.pages[0]))
               == (ssize_t)(h.npages * sizeof(nw.pages[0]))
            && write(fd, nw.pool, h.pool_used) == h.pool_used;
        close(fd);
        if (ok)
            rename(NW_FILE ".tmp", NW_FILE);
        else
            remove(NW_FILE ".tmp");
    }
    /* Unsaved, the next boot walks again to make it */
    if (!ok)
        debug_log(DEBUG_LOG_TASKS, "notes: %s not written", NW_FILE);
    name_index_pages_changed();
}

int notes_page_count(void)
{
    return nw.loaded && global_settings.liner_notes ? nw.npages : 0;
}

const struct notes_page *notes_page_get(int i)
{
    return i >= 0 && i < notes_page_count() ? &nw.pages[i] : NULL;
}

const char *notes_page_path(const struct notes_page *pg)
{
    return nw.pool + pg->path;
}

bool notes_page_better(const struct notes_page *a, const struct notes_page *b)
{
    if (!b)
        return true;
    if (!a->dir != !b->dir)
        return !a->dir;             /* /Notes wins over beside the music */
    return a->rank > b->rank;
}

/* Whether either of `key` and `alt` is either key of the person `pg` */
static bool nw_person_is(const struct notes_page *pg, unsigned int key,
                         unsigned int alt)
{
    return (key && (pg->key == key || pg->alt == key || pg->part == key))
           || (alt && (pg->key == alt || pg->alt == alt));
}

/* The best page in /Notes of `kind` matching `key` or `alt`: for a person
 * either key of its name, its name without the qualifier among them; for an
 * album its artist and title, else its title */
static const struct notes_page *nw_match(int kind, unsigned int key,
                                         unsigned int alt)
{
    const struct notes_page *found = NULL;
    int i, n = notes_page_count();

    for (i = 0; i < n; i++)
    {
        const struct notes_page *pg = &nw.pages[i];

        if (pg->kind != kind || pg->dir || !notes_page_better(pg, found))
            continue;
        if (kind == NOTES_PAGE_PERSON ? nw_person_is(pg, key, alt)
                                      : (key && (pg->key == key
                                                 || pg->part == key)))
            found = pg;
    }
    return found;
}

/* The best page beside the music in the folder whose key is `dir` */
static const struct notes_page *nw_beside(uint64_t dir, int kind)
{
    const struct notes_page *found = NULL;
    int i, n = notes_page_count();

    for (i = 0; i < n; i++)
        if (nw.pages[i].dir == dir && nw.pages[i].kind == kind
            && notes_page_better(&nw.pages[i], found))
            found = &nw.pages[i];
    return found;
}

/* ---- the lookups for a track and an album ------------------------------ */

/* The page beside the music in the first `dirlen` bytes of `dir` */
static const struct notes_page *nw_in_dir(const char *dir, size_t dirlen,
                                          int kind)
{
    static char folder[MAX_PATH];

    snprintf(folder, sizeof(folder), "%.*s", (int)dirlen, dir);
    return nw_beside(path_key(folder), kind);
}

void notes_page_name(const struct notes_page *pg, char *name, size_t size)
{
    const char *path = notes_page_path(pg), *end = strrchr(path, '/');
    const char *start;

    if (pg->dir)
    {
        for (start = end; start > path && start[-1] != '/'; start--)
            ;
        snprintf(name, size, "%.*s", (int)(end - start), start);
        return;
    }
    start = end + 1;
    end = strrchr(start, '.');
    snprintf(name, size, "%.*s", (int)(end - start), start);
}

/* The files beside the track at `track`: the album's notes in its folder,
 * the artist's bio in the folder above when that is named for one of the
 * keys */
static void notes_beside(const char *track, unsigned int k1, unsigned int a1,
                         unsigned int k2, unsigned int a2,
                         struct notes_pages *p)
{
    const char *album_end = track ? strrchr(track, '/') : NULL;
    const char *artist_end;
    const struct notes_page *pg;

    if (!album_end || album_end == track)
        return;
    if (!(p->have & NOTES_ALBUM)
        && (pg = nw_in_dir(track, album_end - track, NOTES_PAGE_ALBUM)))
    {
        strlcpy(p->album, notes_page_path(pg), MAX_PATH);
        p->have |= NOTES_ALBUM;
    }

    for (artist_end = album_end - 1; artist_end > track && *artist_end != '/';
         artist_end--)
        ;
    if (p->have & NOTES_BIO || artist_end <= track)
        return;
    pg = nw_in_dir(track, artist_end - track, NOTES_PAGE_PERSON);
    if (!pg || !(nw_person_is(pg, k1, a1) || nw_person_is(pg, k2, a2)))
        return;
    strlcpy(p->bio, notes_page_path(pg), MAX_PATH);
    notes_page_name(pg, p->person, MAX_PATH);
    p->have |= NOTES_BIO;
}

int notes_find_names(const char *albumartist, const char *artist,
                     const char *album, const char *track,
                     struct notes_pages *p)
{
    const char *by = albumartist && albumartist[0] ? albumartist : artist;
    unsigned int k1 = art_name_hash(albumartist), a1 = art_name_alt_hash(albumartist);
    unsigned int k2 = art_name_hash(artist), a2 = art_name_alt_hash(artist);
    const struct notes_page *pg;

    p->have = 0;
    p->bio[0] = p->person[0] = p->album[0] = '\0';
    if (!global_settings.liner_notes)
        return 0;

    /* The album artist's bio, else the track artist's */
    if ((pg = nw_match(NOTES_PAGE_PERSON, k1, a1))
        || (k2 != k1 && (pg = nw_match(NOTES_PAGE_PERSON, k2, a2))))
    {
        strlcpy(p->bio, notes_page_path(pg), MAX_PATH);
        notes_page_name(pg, p->person, MAX_PATH);
        p->have |= NOTES_BIO;
    }

    /* As /Artwork/Covers is matched: artist and album, else the album */
    if (album && album[0]
        && ((pg = nw_match(NOTES_PAGE_ALBUM, art_name_work_hash(by, album), 0))
            || (pg = nw_match(NOTES_PAGE_ALBUM, art_name_hash(album), 0))))
    {
        strlcpy(p->album, notes_page_path(pg), MAX_PATH);
        p->have |= NOTES_ALBUM;
    }

    if (p->have != (NOTES_BIO | NOTES_ALBUM))
        notes_beside(track, k1, a1, k2, a2, p);
    return p->have;
}

bool notes_album_read(struct tagcache_search *tcs, struct notes_album *a)
{
    long year;

    if (!tagcache_get_next(tcs, a->path, sizeof(a->path)))
        return false;
    if (!tagcache_retrieve_two(tcs, tcs->idx_id,
                               tag_albumartist, a->albumartist,
                               sizeof(a->albumartist),
                               tag_album, a->album, sizeof(a->album)))
        a->albumartist[0] = a->album[0] = '\0';
    if (!tagcache_retrieve(tcs, tcs->idx_id, tag_artist, a->artist,
                           sizeof(a->artist)))
        a->artist[0] = '\0';
    /* <Untagged> names nothing */
    if (!strcmp(a->albumartist, UNTAGGED))
        a->albumartist[0] = '\0';
    if (!strcmp(a->album, UNTAGGED))
        a->album[0] = '\0';
    if (!strcmp(a->artist, UNTAGGED))
        a->artist[0] = '\0';
    year = tagcache_get_numeric(tcs, tag_year);
    if (year > 0)
        snprintf(a->year, sizeof(a->year), "%ld", year);
    else
        a->year[0] = '\0';
    return true;
}

bool notes_album_of(long album_seek, long artist_seek, struct notes_album *a)
{
    struct tagcache_search tcs;
    bool found;

    if (album_seek < 0 || !global_settings.liner_notes
        || !tagcache_is_usable()
        || !tagcache_search(&tcs, tag_filename))
        return false;
    tagcache_search_add_filter(&tcs, tag_album, album_seek);
    if (notes_is_artist(artist_seek))
        tagcache_search_add_filter(&tcs, tag_albumartist, artist_seek);
    found = notes_album_read(&tcs, a);
    tagcache_search_finish(&tcs);
    return found;
}

int notes_find_album(const struct notes_album *a, struct notes_pages *p)
{
    return notes_find_names(a->albumartist, a->artist, a->album, a->path, p);
}

bool notes_has_bio(const char *name)
{
    unsigned int key, alt;
    int i, n = notes_page_count();

    if (!name)
        return false;
    key = art_name_hash(name);
    alt = art_name_alt_hash(name);
    for (i = 0; i < n; i++)
        if (nw.pages[i].kind == NOTES_PAGE_PERSON
            && nw_person_is(&nw.pages[i], key, alt))
            return true;
    return false;
}

