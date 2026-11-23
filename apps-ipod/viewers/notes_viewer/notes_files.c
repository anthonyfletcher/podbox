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
 * Only the pages' keys are kept in memory. Their paths stay in the saved
 * index and are read from it a block at a time, which only opening a page
 * or listing pages by name needs: a lookup never does.
 *
 * Parts, in order:
 *   - the index: built by the walk, saved, read at boot, asked
 *   - the lookups for a track and an album
 *   - a page's front matter
 ****************************************************************************/

#include <stdbool.h>
#include <stdlib.h>                 /* qsort */
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "config.h"
#include "system.h"                 /* MIN */
#include "kernel.h"                 /* the paths' lock */
#include "core_alloc.h"
#include "file.h"
#include "dir.h"
#include "database/tagcache.h"
#include "database/path_key.h"
#include "system/library_files.h"
#include "system/strutil.h"         /* BOM_UTF_8 */
#include "settings/settings.h"
#include "metadata/art_name.h"
#include "metadata/name_index.h"   /* name_index_pages_changed */
#include "system/debug_log.h"
#include "notes_files.h"
#include "notes_links.h"            /* notes_is_artist */

/* ---- the index --------------------------------------------------------- */

#define NW_FILE      LIB_CACHE_DIR "/notes.dat"
#define NW_STAGE     LIB_CACHE_DIR "/notes.new"  /* a walk's pages so far */
#define NW_MAGIC     0x4e4f5435     /* "NOT5" */
#define NW_PAGES_MAX 0xffff         /* a page is 16 bits in the names index */
#define NW_BLOCK     4096           /* of the paths, read at once */

/* The tables of page numbers: NOTES_BY_KEY, _ALT and _PART, and this */
#define NW_BY_DIR    3
#define NW_TABLES    4

/* The pages are in the order the walk found them, which is the order their
 * paths are saved in, so a pass over every page reads the paths straight
 * through. Each table holds the numbers of the pages with that field set,
 * sorted by the field then by page, so a lookup is a binary search and gives
 * the pages in page order. All of it lives in one block that does not move,
 * sized when the index is read or replaced. */
static struct
{
    bool loaded;                /* the index answers; else there are none */
    unsigned int generation;    /* counts each index read or replacing one */
    unsigned int published;     /* the same, counted once it answers */
    int npages;
    int count[NW_TABLES];
    int pool_used;
    uint32_t pool_at;           /* where the paths start in NW_FILE */
    int handle;                 /* the block, 0 for none */
    size_t size;
    struct notes_page *pages;
    uint16_t *table[NW_TABLES];
} nw;

struct nw_header
{
    uint32_t magic;
    int32_t npages, pool_used;
    int32_t nalt, npart, ndir;  /* pages with each set */
};

/* The size and time of the file the walk is passing, as one number */
static uint32_t nw_stamp;

/* The walk's staging file: each page as a struct notes_page, its `path`
 * the length of the path that follows it */
static int nw_stage = -1;
static int nw_staged, nw_staged_alt, nw_staged_part, nw_staged_dir;
static bool nw_full;            /* a page was left out */

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

/* The block laid out for an index of these sizes */
static void nw_carve(int npages, int nalt, int npart, int ndir)
{
    uint16_t *t;
    int i;

    nw.pages = core_get_data(nw.handle);
    nw.npages = npages;
    nw.count[NOTES_BY_KEY] = npages;
    nw.count[NOTES_BY_ALT] = nalt;
    nw.count[NOTES_BY_PART] = npart;
    nw.count[NW_BY_DIR] = ndir;
    t = (uint16_t *)(nw.pages + npages);
    for (i = 0; i < NW_TABLES; i++)
    {
        nw.table[i] = t;
        t += nw.count[i];
    }
}

/* Room for an index of these sizes, while none is loaded. A block too
 * small is replaced by one an eighth larger than needed, so a few more
 * pages next time fit; taken while music plays, that costs a rebuffer.
 * With no pages there is no block. False when there is no room. */
static bool nw_room(int npages, int nalt, int npart, int ndir)
{
    size_t need = npages * sizeof(nw.pages[0])
                  + (npages + nalt + npart + ndir) * sizeof(uint16_t);
    int handle;

    if (!npages)
    {
        if (nw.handle > 0)
            core_free(nw.handle);
        nw.handle = 0;
        nw.size = 0;
        nw.pages = NULL;
        nw.npages = 0;
        memset(nw.count, 0, sizeof(nw.count));
        memset(nw.table, 0, sizeof(nw.table));
        return true;
    }
    if (nw.handle <= 0 || need > nw.size)
    {
        size_t size = need + need / 8;

        handle = core_alloc_ex(size, &buflib_ops_locked);
        if (handle <= 0)
        {
            debug_log(DEBUG_LOG_TASKS, "notes: no room for %d pages",
                      npages);
            return false;
        }
        if (nw.handle > 0)
            core_free(nw.handle);
        nw.handle = handle;
        nw.size = size;
    }
    nw_carve(npages, nalt, npart, ndir);
    return true;
}

/* Page `page`'s field `table` is sorted by, 0 for none */
static uint64_t nw_value(int table, int page)
{
    const struct notes_page *pg = &nw.pages[page];

    switch (table)
    {
        case NOTES_BY_KEY:  return pg->key;
        case NOTES_BY_ALT:  return pg->alt;
        case NOTES_BY_PART: return pg->part;
        default:            return pg->dir;
    }
}

static int nw_sorting;          /* the table qsort() is sorting */

static int nw_order(const void *a, const void *b)
{
    int x = *(const uint16_t *)a, y = *(const uint16_t *)b;
    uint64_t vx = nw_value(nw_sorting, x), vy = nw_value(nw_sorting, y);

    if (vx != vy)
        return vx < vy ? -1 : 1;
    return x - y;
}

/* The tables made from the pages; false when the pages do not have the
 * counts the block was laid out for */
static bool nw_index(void)
{
    int at[NW_TABLES] = { 0 }, i, t;

    for (i = 0; i < nw.npages; i++)
        for (t = 0; t < NW_TABLES; t++)
            if (nw_value(t, i) && at[t]++ < nw.count[t])
                nw.table[t][at[t] - 1] = i;
    for (t = 0; t < NW_TABLES; t++)
    {
        if (at[t] != nw.count[t])
            return false;
        nw_sorting = t;
        qsort(nw.table[t], at[t], sizeof(uint16_t), nw_order);
    }
    return true;
}

/* Whether the index read from the file holds together: every path inside
 * the paths, every kind one there is, and the counts its header gives */
static bool nw_valid(int pool_used)
{
    int i;

    for (i = 0; i < nw.npages; i++)
    {
        const struct notes_page *pg = &nw.pages[i];

        if (pg->path >= (uint32_t)pool_used
            || pg->kind < NOTES_PAGE_PERSON || pg->kind > NOTES_PAGE_OTHER)
            return false;
    }
    return nw_index();
}

bool notes_where_load(void)
{
    struct nw_header h;
    int fd = open(NW_FILE, O_RDONLY);
    size_t bytes;
    bool ok;

    nw.loaded = false;
    nw.generation++;
    nw.published++;
    if (fd < 0)
        return false;
    ok = read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h) && h.magic == NW_MAGIC
         && h.npages >= 0 && h.npages <= NW_PAGES_MAX && h.pool_used >= 0
         && h.nalt >= 0 && h.nalt <= h.npages
         && h.npart >= 0 && h.npart <= h.npages
         && h.ndir >= 0 && h.ndir <= h.npages
         && ffilesize(fd) == (off_t)(sizeof(h) + h.npages * sizeof(nw.pages[0])
                                     + h.pool_used);
    /* With the setting off the file is good but nothing is held */
    if (ok && !global_settings.liner_notes)
    {
        close(fd);
        nw_room(0, 0, 0, 0);
        return true;
    }
    bytes = ok ? h.npages * sizeof(nw.pages[0]) : 0;
    ok = ok && nw_room(h.npages, h.nalt, h.npart, h.ndir)
         && (!bytes || read(fd, nw.pages, bytes) == (ssize_t)bytes)
         && nw_valid(h.pool_used);
    close(fd);
    if (ok)
    {
        nw.pool_used = h.pool_used;
        nw.pool_at = sizeof(h) + bytes;
        nw.loaded = true;
    }
    return ok;
}

unsigned int notes_pages_generation(void)
{
    return nw.published;
}

void notes_where_begin(void)
{
    if (nw_stage >= 0)
        close(nw_stage);
    nw_stage = open(NW_STAGE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    nw_staged = nw_staged_alt = nw_staged_part = nw_staged_dir = 0;
    nw_full = false;
}

static void nw_add(int kind, uint32_t key, uint32_t alt, uint32_t part,
                   uint32_t by, uint64_t dir, const char *path, int rank)
{
    struct notes_page pg;
    size_t size = strlen(path) + 1;

    if (!key || nw_stage < 0)
        return;
    if (nw_staged == NW_PAGES_MAX)
    {
        nw_full = true;
        return;
    }
    memset(&pg, 0, sizeof(pg));
    pg.key = key;
    pg.alt = alt;
    pg.part = part;
    pg.by = by;
    pg.dir = dir;
    pg.path = size;
    pg.hash = name_index_path_hash(path);
    pg.stamp = nw_stamp;
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
    nw_staged_alt += !!alt;
    nw_staged_part += !!part;
    nw_staged_dir += !!dir;
}

/* The staging file's pages into the index in memory, each path given its
 * place among the saved index's, and their tables made; false when it
 * cannot be read whole */
static bool nw_unstage(void)
{
    int fd = open(NW_STAGE, O_RDONLY), i;
    bool ok = fd >= 0;

    nw.pool_used = 0;
    for (i = 0; ok && i < nw.npages; i++)
    {
        struct notes_page *pg = &nw.pages[i];
        uint32_t size;

        ok = read(fd, pg, sizeof(*pg)) == (ssize_t)sizeof(*pg)
             && (size = pg->path) > 0 && size <= MAX_PATH
             && lseek(fd, size, SEEK_CUR) >= 0;
        if (!ok)
            break;
        pg->path = nw.pool_used;
        nw.pool_used += size;
    }
    if (fd >= 0)
        close(fd);
    nw.pool_at = sizeof(struct nw_header) + nw.npages * sizeof(nw.pages[0]);
    return ok && nw_index();
}

/* The index saved: its pages, then their paths copied from the staging
 * file in the order they were staged; false when it is not */
static bool nw_save(void)
{
    static char path[MAX_PATH];
    struct nw_header h = { NW_MAGIC, nw.npages, nw.pool_used,
                           nw.count[NOTES_BY_ALT], nw.count[NOTES_BY_PART],
                           nw.count[NW_BY_DIR] };
    size_t bytes = nw.npages * sizeof(nw.pages[0]);
    uint32_t copied = 0;
    int in = open(NW_STAGE, O_RDONLY), out = -1, i;
    bool ok = in >= 0
              && (out = open(NW_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC,
                             0666)) >= 0
              && write(out, &h, sizeof(h)) == (ssize_t)sizeof(h)
              && write(out, nw.pages, bytes) == (ssize_t)bytes;

    for (i = 0; ok && i < nw.npages; i++)
    {
        struct notes_page pg;
        uint32_t size;

        ok = read(in, &pg, sizeof(pg)) == (ssize_t)sizeof(pg)
             && (size = pg.path) > 0 && size <= MAX_PATH
             && read(in, path, size) == (ssize_t)size;
        if (ok)
        {
            path[size - 1] = '\0';
            ok = write(out, path, size) == (ssize_t)size;
            copied += size;
        }
    }
    ok = ok && copied == (uint32_t)nw.pool_used;
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    if (ok)
        ok = rename(NW_FILE ".tmp", NW_FILE) == 0;
    if (!ok)
        remove(NW_FILE ".tmp");
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
        const char *dash = notes_dash(leaf, len);

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

    /* Switching the setting walks again, so with it off nothing is kept */
    if (!global_settings.liner_notes || !leaf)
        return;
    nw_stamp = (size ^ 0x811C9DC5u) * 16777619u;
    nw_stamp = (nw_stamp ^ mtime) * 16777619u;
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
    bool staged = nw_stage >= 0;

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

    /* The paths are read from the saved index, so one that cannot be saved
     * leaves the last */
    nw.loaded = false;
    nw.generation++;
    if (!nw_room(nw_staged, nw_staged_alt, nw_staged_part, nw_staged_dir)
        || !nw_unstage() || !nw_save())
    {
        remove(NW_STAGE);
        debug_log(DEBUG_LOG_TASKS, "notes: %s not written", NW_FILE);
        notes_where_load();
        return;
    }
    nw.loaded = true;
    nw.published++;
    remove(NW_STAGE);
    if (nw_full)
        debug_log(DEBUG_LOG_TASKS, "notes: past %d pages, the rest left out",
                  NW_PAGES_MAX);
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

/* The block of the saved paths last read, and the index it is of. The
 * background thread reads paths as the screens do, so it is locked. */
static struct
{
    struct mutex lock;
    bool ready;
    unsigned int generation;    /* 0 for none */
    uint32_t block, len;
    char data[NW_BLOCK];
} nw_read;

static bool nw_read_block(uint32_t block, unsigned int generation)
{
    int fd = open(NW_FILE, O_RDONLY);
    ssize_t got = -1;

    nw_read.generation = 0;
    if (fd < 0)
        return false;
    if (lseek(fd, block * NW_BLOCK, SEEK_SET) == (off_t)(block * NW_BLOCK))
        got = read(fd, nw_read.data, NW_BLOCK);
    close(fd);
    /* An index replaced meanwhile may have been read instead */
    if (got <= 0 || nw.generation != generation)
        return false;
    nw_read.generation = generation;
    nw_read.block = block;
    nw_read.len = got;
    return true;
}

bool notes_page_path(const struct notes_page *pg, char *buf, size_t size)
{
    unsigned int generation = nw.generation;
    uint32_t off, end;
    size_t n = 0;
    bool ok = true;

    if (!size)
        return false;
    buf[0] = '\0';
    if (!nw.loaded || pg < nw.pages || pg >= nw.pages + nw.npages)
        return false;
    if (!nw_read.ready)
    {
        mutex_init(&nw_read.lock);
        nw_read.ready = true;
    }
    mutex_lock(&nw_read.lock);
    end = nw.pool_at + nw.pool_used;
    for (off = nw.pool_at + pg->path; n + 1 < size; off++)
    {
        uint32_t block = off / NW_BLOCK;
        char c;

        if (off >= end
            || ((nw_read.generation != generation || nw_read.block != block)
                && !nw_read_block(block, generation))
            || off % NW_BLOCK >= nw_read.len)
        {
            ok = false;
            break;
        }
        if (!(c = nw_read.data[off % NW_BLOCK]))
            break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    mutex_unlock(&nw_read.lock);
    return ok && n;
}

bool notes_page_better(const struct notes_page *a, const struct notes_page *b)
{
    if (!b)
        return true;
    if (!a->dir != !b->dir)
        return !a->dir;             /* /Notes wins over beside the music */
    return a->rank > b->rank;
}

bool notes_page_wins(int page, int best)
{
    const struct notes_page *a = notes_page_get(page);
    const struct notes_page *b = notes_page_get(best);

    if (!a)
        return false;
    if (!b || notes_page_better(a, b))
        return true;
    return !notes_page_better(b, a) && page < best;
}

/* The next page in `table` whose field is `value`; the first is found by a
 * binary search */
static int nw_next(int table, uint64_t value, int *at)
{
    const uint16_t *t = nw.table[table];
    int n = nw.count[table], i = *at;

    if (!value || !notes_page_count())
        return -1;
    if (i < 0)
    {
        int hi = n;

        for (i = 0; i < hi; )
        {
            int mid = i + (hi - i) / 2;

            if (nw_value(table, t[mid]) < value)
                i = mid + 1;
            else
                hi = mid;
        }
    }
    if (i >= n || nw_value(table, t[i]) != value)
        return -1;
    *at = i + 1;
    return t[i];
}

int notes_pages_by(int field, uint32_t value, int *at)
{
    return nw_next(field, value, at);
}

int notes_pages_in(uint64_t dir, int *at)
{
    return nw_next(NW_BY_DIR, dir, at);
}

/* Whether either of `key` and `alt` is either key of the person `pg` */
static bool nw_person_is(const struct notes_page *pg, unsigned int key,
                         unsigned int alt)
{
    return (key && (pg->key == key || pg->alt == key || pg->part == key))
           || (alt && (pg->key == alt || pg->alt == alt));
}

/* Where a person's or an album's page may be found: by which key, for the
 * key or the alt given */
static const struct { int field; bool alt; } nw_probes[] = {
    { NOTES_BY_KEY, false }, { NOTES_BY_ALT, false },
    { NOTES_BY_PART, false }, { NOTES_BY_KEY, true }, { NOTES_BY_ALT, true },
};

/* The best page in /Notes of `kind` matching `key` or `alt`: for a person
 * either key of its name, its name without the qualifier among them; for an
 * album its artist and title, else its title */
static const struct notes_page *nw_match(int kind, unsigned int key,
                                         unsigned int alt)
{
    int best = -1, i, p, at;

    for (p = 0; p < (int)ARRAYLEN(nw_probes); p++)
    {
        if (kind == NOTES_PAGE_ALBUM && (nw_probes[p].alt
                                         || nw_probes[p].field == NOTES_BY_ALT))
            continue;
        for (at = -1; (i = notes_pages_by(nw_probes[p].field,
                                          nw_probes[p].alt ? alt : key,
                                          &at)) >= 0; )
        {
            const struct notes_page *pg = &nw.pages[i];

            if (pg->kind == kind && !pg->dir && notes_page_wins(i, best)
                && (kind == NOTES_PAGE_PERSON ? nw_person_is(pg, key, alt)
                                              : (key && (pg->key == key
                                                         || pg->part == key))))
                best = i;
        }
    }
    return best >= 0 ? &nw.pages[best] : NULL;
}

/* ---- the lookups for a track and an album ------------------------------ */

/* The best page beside the music in the first `dirlen` bytes of `dir` */
static const struct notes_page *nw_in_dir(const char *dir, size_t dirlen,
                                          int kind)
{
    static char folder[MAX_PATH];
    int best = -1, i, at = -1;

    snprintf(folder, sizeof(folder), "%.*s", (int)dirlen, dir);
    while ((i = notes_pages_in(path_key(folder), &at)) >= 0)
        if (nw.pages[i].kind == kind && notes_page_wins(i, best))
            best = i;
    return best >= 0 ? &nw.pages[best] : NULL;
}

void notes_page_name(const struct notes_page *pg, char *name, size_t size)
{
    char path[MAX_PATH];
    const char *end, *start;

    name[0] = '\0';
    if (!notes_page_path(pg, path, sizeof(path))
        || !(end = strrchr(path, '/')))
        return;
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
        p->album = pg - nw.pages;
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
    p->bio = pg - nw.pages;
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
    p->bio = p->album = -1;
    if (!global_settings.liner_notes)
        return 0;

    /* The album artist's bio, else the track artist's */
    if ((pg = nw_match(NOTES_PAGE_PERSON, k1, a1))
        || (k2 != k1 && (pg = nw_match(NOTES_PAGE_PERSON, k2, a2))))
    {
        p->bio = pg - nw.pages;
        p->have |= NOTES_BIO;
    }

    /* As /Artwork/Covers is matched: artist and album, else the album */
    if (album && album[0]
        && ((pg = nw_match(NOTES_PAGE_ALBUM, art_name_work_hash(by, album), 0))
            || (pg = nw_match(NOTES_PAGE_ALBUM, art_name_hash(album), 0))))
    {
        p->album = pg - nw.pages;
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
    int i, p, at;

    if (!name)
        return false;
    key = art_name_hash(name);
    alt = art_name_alt_hash(name);
    for (p = 0; p < (int)ARRAYLEN(nw_probes); p++)
        for (at = -1; (i = notes_pages_by(nw_probes[p].field,
                                          nw_probes[p].alt ? alt : key,
                                          &at)) >= 0; )
            if (nw.pages[i].kind == NOTES_PAGE_PERSON
                && nw_person_is(&nw.pages[i], key, alt))
                return true;
    return false;
}

/* ---- a page's front matter --------------------------------------------- */

bool notes_yaml_line(const char *s)
{
    int n;

    if (*s == '\n' || *s == '\r' || !*s || strchr(" \t-#.", *s))
        return true;
    for (n = 0; n < 32; n++, s++)
    {
        unsigned char c = *s;

        if (!((c >= '0' && c <= '9')
              || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')
              || c >= 0x80 || (c && strchr("_- .\"'", c))))
            break;
    }
    return *s == ':';
}

size_t notes_front_value(const char *text, const char *key, char *buf,
                         size_t size)
{
    size_t klen = strlen(key);
    const char *s = text;

    buf[0] = '\0';
    if (!strncmp(s, BOM_UTF_8, BOM_UTF_8_SIZE))
        s += BOM_UTF_8_SIZE;
    if (strncmp(s, "---\n", 4) && strncmp(s, "---\r\n", 5))
        return 0;
    while ((s = strchr(s, '\n')) && *++s)
    {
        const char *v = s + klen + 1, *end;
        size_t n;

        if (!strncmp(s, "---", 3) || !strncmp(s, "...", 3)
            || !notes_yaml_line(s))
            break;
        if (strncasecmp(s, key, klen) || s[klen] != ':')
            continue;
        while (*v == ' ' || *v == '\t')
            v++;
        for (end = v; *end && *end != '\n' && *end != '\r'; end++)
            ;
        while (end > v && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if (end - v >= 2 && (*v == '"' || *v == '\'') && end[-1] == *v)
        {
            v++;
            end--;
        }
        n = MIN((size_t)(end - v), size - 1);
        memcpy(buf, v, n);
        buf[n] = '\0';
        return n;
    }
    return 0;
}

