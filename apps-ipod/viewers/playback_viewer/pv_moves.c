/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Folders that have moved since their files were played.
 *
 * A logged path is where the file was on the day it was heard. Rename an
 * artist folder or move the library under a new root, and every play before
 * that names a file the database no longer knows -- so it falls to folder
 * guesswork, under the OLD folder's names, and its artwork is lost with it.
 *
 * This finds where such a folder went, by the names of the files in it. For
 * each logged folder with files the database lacks, every database folder
 * holding a file of the same name is a candidate, and a candidate holding
 * enough of them is taken as the folder's new home (see accept()). The
 * result is a table of old folder -> new folder, applied as a path is read;
 * the log itself is never rewritten.
 *
 * It is saved with the database state it was worked out for, and built
 * again when a track has been deleted since -- a move is a deletion and an
 * addition -- or when a track added since shares a file name with one of the
 * missing files the last build could not place, since it may be that file's
 * new home. Other additions leave it standing.
 *
 *   build    the log's folders and files, two walks of the database, the
 *            verdicts, the file
 *   use      load, apply, ident, discard
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <file.h>
#include "config.h"
#include "system/library_files.h"
#include "database/libfile.h"
#include "system/hash.h"
#include "rbpaths.h"
#include "database/tagcache.h"
#include "lang.h"
#include "settings/settings.h"    /* ID2P */
#include "widgets/splash.h"
#include "pv_log.h"
#include "pv_moves.h"

/* A libfile: struct move records, then as its tail tagcache_entry_key() of
 * the database's last entry, the count and file-name hashes of the missing
 * files left unplaced (uint32_t each; UNPLACED_ALL for more than
 * UNPLACED_MAX) and the pool of the moved folders. Its marks are the database
 * the table was worked out for. */
#define UNPLACED_MAX 512
#define UNPLACED_ALL 0xffffffffu
#define PV_MOVES_PATH    LIB_REPORT_MOVES_FILE
#define PV_MOVES_MAGIC   LIB_MOVES_MAGIC
#define PV_MOVES_VERSION LIB_MOVES_VERSION

/* Candidate folders tracked per logged folder. A file name as common as
 * "01 - Intro.mp3" votes for a folder of its own in every album that has
 * one; eviction of single-vote candidates keeps those from crowding out the
 * real one, whose votes arrive together because the database lists a folder's
 * files together. */
#define CANDS 8

/* Most moved folders a table holds, and the most pool it may carry. The table
 * is loaded below the aggregate tables, so on a 5G every byte of it is a
 * byte they lose. */
#define MOVES_MAX      512
#define MOVES_POOL_MAX 24576

struct lfile                /* a distinct file the log names */
{
    uint32_t path_h;
    uint32_t base_h;        /* of the name after the last '/' */
    int      dir;           /* into dirs[] */
    bool     found;         /* the database holds this exact path */
};

struct ldir                 /* a distinct folder the log names */
{
    uint32_t dir_h;
    uint32_t leaf_h;        /* of the folder's own name */
    int      missing;       /* of its logged files, how many are gone */
    int      matches;       /* database files sharing a missing file's name */
    int      cand;          /* first of its CANDS in cands[], or -1 */
    int      best;          /* the cands[] entry it moved to, or -1 */
};

struct cand
{
    uint32_t dir_h;
    uint32_t leaf_h;
    unsigned off;           /* the folder's path, in the scan pool */
    int      votes;         /* missing files found in it; 0 = free slot */
};

struct move
{
    uint32_t dir_h;         /* the folder as logged */
    unsigned off;           /* where it is now, in the table's pool */
};

static struct
{
    struct lfile *files;
    int *fslots;            /* by path, then (after walk 1) by file name */
    int f_n, f_cap, f_mask;
    struct ldir *dirs;
    int *dslots;
    int d_n, d_cap, d_mask;
    struct cand *cands;
    char *pool;
    unsigned pool_used, pool_cap;
    bool ram;               /* the path index answers walk 1 as files arrive */
} sc;

/* The loaded table. */
static struct move *mv;
static int *mv_slots;
static int mv_n, mv_mask;
static char *mv_pool;

/* ----------------------------------------------------------------- build */

static uint32_t leaf_hash(const char *dir, size_t n)
{
    size_t i = n;

    while (i > 0 && dir[i - 1] != '/')
        i--;
    return fnv1a_bytes(dir + i, n - i);
}

/* The largest power of two at or below v, or 0. */
static int floor_pow2(size_t v)
{
    int p = 1;

    if (v < 1)
        return 0;
    while ((size_t)p * 2 <= v && p < (1 << 24))
        p <<= 1;
    return p;
}

static int dir_find_or_add(const char *path, size_t n)
{
    uint32_t h = fnv1a_bytes(path, n);
    int s = (int)(h & (unsigned)sc.d_mask);

    while (sc.dslots[s])
    {
        if (sc.dirs[sc.dslots[s] - 1].dir_h == h)
            return sc.dslots[s] - 1;
        s = (s + 1) & sc.d_mask;
    }
    if (sc.d_n >= sc.d_cap)
        return -1;

    sc.dirs[sc.d_n].dir_h   = h;
    sc.dirs[sc.d_n].leaf_h  = leaf_hash(path, n);
    sc.dirs[sc.d_n].missing = 0;
    sc.dirs[sc.d_n].matches = 0;
    sc.dirs[sc.d_n].cand    = -1;
    sc.dirs[sc.d_n].best    = -1;
    sc.dslots[s] = ++sc.d_n;
    return sc.d_n - 1;
}

/* Every distinct file the log names, once. A table that fills simply stops
 * taking files, and those are never treated as moved. */
static void log_cb(const struct pv_entry *e, void *ctx)
{
    const char *p = e->path;
    const char *slash;
    uint32_t h;
    int s, d;

    (void)ctx;
    if (!p || !p[0])
        return;
    slash = strrchr(p, '/');
    if (!slash || slash == p)
        return;

    h = fnv1a_str(p);
    s = (int)(h & (unsigned)sc.f_mask);
    while (sc.fslots[s])
    {
        if (sc.files[sc.fslots[s] - 1].path_h == h)
            return;
        s = (s + 1) & sc.f_mask;
    }
    if (sc.f_n >= sc.f_cap)
        return;

    d = dir_find_or_add(p, (size_t)(slash - p));
    if (d < 0)
        return;

    sc.files[sc.f_n].path_h = h;
    sc.files[sc.f_n].base_h = fnv1a_str(slash + 1);
    sc.files[sc.f_n].dir    = d;
    sc.files[sc.f_n].found  = sc.ram && tagcache_find_path(p) >= 0;
    sc.fslots[s] = ++sc.f_n;
}

/* Walk 1: which logged files the database still holds where they were. Only
 * without the path index, which log_cb() asks instead. */
static bool mark_found(void)
{
    struct tagcache_search tcs;
    char fname[MAX_PATH];
    int n = 0, total;

    if (!tagcache_search(&tcs, tag_filename))
        return false;

    /* Read before the walk: get_next() counts entry_count down on disk. */
    total = tcs.entry_count;
    while (tagcache_get_next(&tcs, fname, sizeof(fname)))
    {
        uint32_t h = fnv1a_str(fname);
        int s = (int)(h & (unsigned)sc.f_mask);

        while (sc.fslots[s])
        {
            struct lfile *f = &sc.files[sc.fslots[s] - 1];
            if (f->path_h == h)
            {
                f->found = true;
                break;
            }
            s = (s + 1) & sc.f_mask;
        }

        splash_progress(++n, total, "%s",
                        str(LANG_PV_MOVED_FOLDERS));
    }

    tagcache_search_finish(&tcs);
    return true;
}

static void vote(struct ldir *ld, uint32_t dir_h, const char *dir, size_t n)
{
    struct cand *c, *slot = NULL;

    if (ld->dir_h == dir_h)
        return;
    ld->matches++;
    if (ld->cand < 0)
        return;

    c = &sc.cands[ld->cand];
    for (int i = 0; i < CANDS; i++)
    {
        if (c[i].votes && c[i].dir_h == dir_h)
        {
            c[i].votes++;
            return;
        }
        if (!c[i].votes && !slot)
            slot = &c[i];
    }
    for (int i = 0; !slot && i < CANDS; i++)
        if (c[i].votes == 1)
            slot = &c[i];

    if (!slot || sc.pool_used + n + 1 > sc.pool_cap)
        return;

    memcpy(sc.pool + sc.pool_used, dir, n);
    sc.pool[sc.pool_used + n] = '\0';
    slot->dir_h  = dir_h;
    slot->leaf_h = leaf_hash(dir, n);
    slot->off    = sc.pool_used;
    slot->votes  = 1;
    sc.pool_used += (unsigned)n + 1;
}

/* Walk 2: every database file named like a missing one votes for its folder
 * as that file's new home. fslots is keyed by file name by now, and holds
 * only the missing files. */
static bool collect_votes(void)
{
    struct tagcache_search tcs;
    char fname[MAX_PATH];
    int n = 0, total;

    if (!tagcache_search(&tcs, tag_filename))
        return false;

    /* Read before the walk: get_next() counts entry_count down on disk. */
    total = tcs.entry_count;
    while (tagcache_get_next(&tcs, fname, sizeof(fname)))
    {
        const char *slash = strrchr(fname, '/');
        uint32_t bh, dh;
        int s;

        splash_progress(++n, total, "%s",
                        str(LANG_PV_MOVED_FOLDERS));
        if (!slash || slash == fname)
            continue;

        bh = fnv1a_str(slash + 1);
        dh = fnv1a_bytes(fname, (size_t)(slash - fname));
        s = (int)(bh & (unsigned)sc.f_mask);
        while (sc.fslots[s])
        {
            struct lfile *f = &sc.files[sc.fslots[s] - 1];
            if (f->base_h == bh)
                vote(&sc.dirs[f->dir], dh, fname, (size_t)(slash - fname));
            s = (s + 1) & sc.f_mask;
        }
    }

    tagcache_search_finish(&tcs);
    return true;
}

/* Whether 'best' is where the folder went.
 *
 * It must hold more than half of the folder's missing files: a folder whose
 * files scattered has no one new home, and the old names are then the better
 * answer. Two or three names shared with some other folder are too little to
 * go on unless the folder kept its own name as well -- "01 - Intro.mp3" and
 * "02 - Interlude.mp3" sit in more than one album. A folder with a single
 * missing file needs that file's name to be unique in the database. */
static bool accept(const struct ldir *ld, const struct cand *best)
{
    bool same_leaf = best->leaf_h == ld->leaf_h;

    if (ld->missing == 1)
        return ld->matches == 1 && same_leaf;
    if (best->votes * 2 <= ld->missing)
        return false;
    return best->votes >= 3 || same_leaf;
}

/* The missing files left unplaced, by name */
static uint32_t unplaced[UNPLACED_MAX];
static uint32_t unplaced_n;

static void moves_save(const struct move *m, int n, const char *pool,
                       unsigned pool_bytes, const struct pv_moves_db *db)
{
    struct libfile_writer w;
    struct libfile_marks marks;
    uint64_t last_key = tagcache_entry_key(db->entries - 1);

    libfile_no_marks(&marks);
    marks.entries = db->entries;
    marks.commitid = db->commit;
    marks.deleted = db->deleted;
    if (libfile_begin(&w, PV_MOVES_PATH, PV_MOVES_MAGIC, PV_MOVES_VERSION,
                      sizeof(struct move), &marks))
        libfile_finish(&w,
                       libfile_write(&w, m, (size_t)n * sizeof(struct move), n)
                       && libfile_write(&w, &last_key, sizeof(last_key), 0)
                       && libfile_write(&w, &unplaced_n, sizeof(unplaced_n), 0)
                       && (unplaced_n == UNPLACED_ALL
                           || libfile_write(&w, unplaced,
                                            unplaced_n * sizeof(uint32_t), 0))
                       && libfile_write(&w, pool, pool_bytes, 0));
}

void pv_moves_build(void *scratch, size_t size, const struct pv_moves_db *db)
{
    char *p = scratch;
    size_t used;
    int fs, ds, missing_dirs = 0, n_moves = 0;
    struct move *out;
    char *out_pool;
    unsigned out_used = 0, out_cap;
    size_t rest;

    /* At most half for the files (16 bytes and two slots of 4 each), an
     * eighth for the folders (20 and two slots), the rest for candidates and
     * their paths. */
    p = (char *)(((size_t)p + 3u) & ~(size_t)3u);
    size -= (size_t)(p - (char *)scratch);
    fs = floor_pow2(size / 24);
    ds = floor_pow2(size / 112);
    if (fs < 64 || ds < 16)
        return;

    memset(&sc, 0, sizeof(sc));
    sc.files  = (struct lfile *)p;
    sc.f_cap  = fs / 2;
    sc.fslots = (int *)(p + (size_t)sc.f_cap * sizeof(struct lfile));
    sc.f_mask = fs - 1;
    used = (size_t)sc.f_cap * sizeof(struct lfile) + (size_t)fs * sizeof(int);
    sc.dirs   = (struct ldir *)(p + used);
    sc.d_cap  = ds / 2;
    sc.dslots = (int *)(p + used + (size_t)sc.d_cap * sizeof(struct ldir));
    sc.d_mask = ds - 1;
    used += (size_t)sc.d_cap * sizeof(struct ldir) + (size_t)ds * sizeof(int);
    memset(sc.fslots, 0, (size_t)fs * sizeof(int));
    memset(sc.dslots, 0, (size_t)ds * sizeof(int));

    splash(0, ID2P(LANG_PV_MOVED_FOLDERS));
    sc.ram = tagcache_is_in_ram();
    if (pv_log_read(PV_SRC_PLAYBACK, log_cb, NULL) < 0 || sc.f_n == 0)
        return;

    if (!sc.ram && !mark_found())
        return;

    /* From here the slots index the missing files by name. */
    memset(sc.fslots, 0, (size_t)fs * sizeof(int));
    for (int i = 0; i < sc.f_n; i++)
    {
        struct lfile *f = &sc.files[i];
        int s;

        if (f->found)
            continue;
        if (sc.dirs[f->dir].missing++ == 0)
            missing_dirs++;
        s = (int)(f->base_h & (unsigned)sc.f_mask);
        while (sc.fslots[s])
            s = (s + 1) & sc.f_mask;
        sc.fslots[s] = i + 1;
    }

    /* Candidates for as many missing folders as half the rest holds; the
     * other half keeps their paths. A folder left without is never matched,
     * which is the safe way to run short. */
    rest = size - used;
    {
        int groups = (int)(rest / 2 / (CANDS * sizeof(struct cand)));
        size_t c_bytes;

        if (groups > missing_dirs)
            groups = missing_dirs;
        c_bytes = (size_t)groups * CANDS * sizeof(struct cand);
        sc.cands = (struct cand *)(p + used);
        memset(sc.cands, 0, c_bytes);
        sc.pool = p + used + c_bytes;
        sc.pool_cap = (unsigned)(rest - c_bytes);

        for (int i = 0, g = 0; i < sc.d_n && g < groups; i++)
            if (sc.dirs[i].missing)
                sc.dirs[i].cand = CANDS * g++;
    }

    if (missing_dirs && !collect_votes())
        return;

    /* Where each folder went, decided while the files are still there to
     * say which missing ones are left over */
    for (int i = 0; i < sc.d_n; i++)
    {
        struct ldir *ld = &sc.dirs[i];
        const struct cand *c;

        if (!ld->missing || ld->cand < 0)
            continue;
        c = &sc.cands[ld->cand];
        for (int k = 0; k < CANDS; k++)
            if (c[k].votes && (ld->best < 0 || c[k].votes > c[ld->best].votes))
                ld->best = k;
        if (ld->best >= 0 && !accept(ld, &c[ld->best]))
            ld->best = -1;
    }

    /* A log with more files than the table took may have missing ones it
     * never saw, so it counts as leaving every name unplaced. */
    unplaced_n = sc.f_n >= sc.f_cap ? UNPLACED_ALL : 0;
    for (int i = 0; i < sc.f_n && unplaced_n != UNPLACED_ALL; i++)
    {
        const struct lfile *f = &sc.files[i];

        if (f->found || sc.dirs[f->dir].best >= 0)
            continue;
        if (unplaced_n == UNPLACED_MAX)
            unplaced_n = UNPLACED_ALL;
        else
            unplaced[unplaced_n++] = f->base_h;
    }

    /* The files and their slots are finished with: the table is assembled
     * where they were. */
    out = (struct move *)sc.files;
    out_pool = (char *)sc.fslots;
    out_cap = (unsigned)fs * sizeof(int);
    if (out_cap > MOVES_POOL_MAX)
        out_cap = MOVES_POOL_MAX;

    for (int i = 0; i < sc.d_n && n_moves < MOVES_MAX
                    && n_moves < sc.f_cap; i++)
    {
        const struct ldir *ld = &sc.dirs[i];
        const char *dir;
        unsigned len;

        if (ld->best < 0)
            continue;

        dir = sc.pool + sc.cands[ld->cand + ld->best].off;
        len = (unsigned)strlen(dir) + 1;
        if (out_used + len > out_cap)
            break;
        memcpy(out_pool + out_used, dir, len);
        out[n_moves].dir_h = ld->dir_h;
        out[n_moves].off = out_used;
        out_used += len;
        n_moves++;
    }

    moves_save(out, n_moves, out_pool, out_used, db);
}

/* ------------------------------------------------------------------- use */

/* Whether a track added since entry 'from' -- new entries are appended --
 * bears the name of an unplaced missing file */
static bool added_matches(int from, int to)
{
    struct tagcache_search tcs;
    char fname[MAX_PATH];
    bool hit = false;

    if (!tagcache_search(&tcs, tag_filename))
        return true;
    /* With the database in RAM, which a deleted count needs, a filename
     * search walks the index, so the range holds */
    tagcache_search_set_range(&tcs, from, to - 1);
    while (!hit && tagcache_get_next(&tcs, fname, sizeof(fname)))
    {
        const char *slash = strrchr(fname, '/');
        uint32_t h = fnv1a_str(slash ? slash + 1 : fname);

        for (uint32_t i = 0; i < unplaced_n && !hit; i++)
            hit = unplaced[i] == h;
    }
    tagcache_search_finish(&tcs);
    return hit;
}

/* The table's header, if the table still serves this database: the one it
 * was worked out for, or that one with tracks only added since, none of them
 * named like a file the table could not place. A Rebuild renumbers the
 * entries and can restore the commit id and count, so both cases test the
 * file at the old last entry; a key of 0 (off RAM) matches only itself. */
static bool read_hdr(struct libfile_header *h, const struct pv_moves_db *db)
{
    uint64_t last_key;
    uint32_t tail;
    int fd;
    bool ok, same;

    if (!libfile_peek(PV_MOVES_PATH, PV_MOVES_MAGIC, PV_MOVES_VERSION, h)
        || h->count > MOVES_MAX)
        return false;
    same = h->marks.entries == db->entries && h->marks.commitid == db->commit;
    if (h->marks.deleted != db->deleted
        || (!same && (db->deleted < 0 || db->entries <= h->marks.entries
                      || db->commit < h->marks.commitid)))
        return false;

    fd = libfile_open(PV_MOVES_PATH, PV_MOVES_MAGIC, PV_MOVES_VERSION,
                      sizeof(struct move), h, &tail);
    if (fd < 0)
        return false;
    ok = tail >= sizeof(last_key) + sizeof(unplaced_n)
         && lseek(fd, (off_t)h->count * sizeof(struct move), SEEK_CUR) >= 0
         && read(fd, &last_key, sizeof(last_key)) == (ssize_t)sizeof(last_key)
         && (same || last_key != 0)
         && tagcache_entry_key(h->marks.entries - 1) == last_key
         && (same
             || (read(fd, &unplaced_n, sizeof(unplaced_n))
                     == (ssize_t)sizeof(unplaced_n)
                 && unplaced_n <= UNPLACED_MAX
                 && read(fd, unplaced, unplaced_n * sizeof(uint32_t))
                     == (ssize_t)(unplaced_n * sizeof(uint32_t))));
    close(fd);
    return ok && (same || unplaced_n == 0
                  || !added_matches(h->marks.entries, db->entries));
}

bool pv_moves_stale(const struct pv_moves_db *db)
{
    struct libfile_header h;

    return !read_hdr(&h, db);
}

void pv_moves_forget(void)
{
    mv = NULL;
    mv_slots = NULL;
    mv_pool = NULL;
    mv_n = 0;
}

size_t pv_moves_load(void *buf, size_t size, const struct pv_moves_db *db)
{
    struct libfile_header h;
    uint32_t pool_bytes;
    off_t names_skip;
    size_t need = 0;
    int fd, n, slots;

    pv_moves_forget();
    if (!read_hdr(&h, db))
        return 0;
    fd = libfile_open(PV_MOVES_PATH, PV_MOVES_MAGIC, PV_MOVES_VERSION,
                      sizeof(struct move), &h, &pool_bytes);
    if (fd < 0)
        return 0;

    /* The key and the unplaced names lead the tail, ahead of the pool: read
     * past them, then back to the records */
    {
        uint32_t names;
        off_t skip;

        if (lseek(fd, (off_t)h.count * sizeof(struct move) + sizeof(uint64_t),
                  SEEK_CUR) < 0
            || read(fd, &names, sizeof(names)) != (ssize_t)sizeof(names))
        {
            close(fd);
            return 0;
        }
        skip = sizeof(uint64_t) + sizeof(names)
             + (names == UNPLACED_ALL ? 0 : (off_t)names * sizeof(uint32_t));
        pool_bytes = pool_bytes > (uint32_t)skip ? pool_bytes - (uint32_t)skip : 0;
        if (lseek(fd, sizeof(struct libfile_header), SEEK_SET) < 0)
        {
            close(fd);
            return 0;
        }
        names_skip = skip;
    }

    if (h.count > 0 && pool_bytes > 0 && pool_bytes <= MOVES_POOL_MAX)
    {
        n = (int)h.count;
        slots = next_pow2(n * 2);
        need = (size_t)n * sizeof(struct move) + (size_t)slots * sizeof(int)
             + pool_bytes;

        if (need <= size)
        {
            struct move *m = buf;
            int *sl = (int *)((char *)buf + (size_t)n * sizeof(struct move));
            char *pool = (char *)sl + (size_t)slots * sizeof(int);
            bool ok = read(fd, m, (size_t)n * sizeof(struct move))
                          == (ssize_t)((size_t)n * sizeof(struct move))
                   && lseek(fd, names_skip, SEEK_CUR) >= 0
                   && read(fd, pool, pool_bytes) == (ssize_t)pool_bytes
                   && pool[pool_bytes - 1] == '\0';

            for (int i = 0; ok && i < n; i++)
                ok = m[i].off < pool_bytes;

            if (ok)
            {
                memset(sl, 0, (size_t)slots * sizeof(int));
                for (int i = 0; i < n; i++)
                {
                    int s = (int)(m[i].dir_h & (unsigned)(slots - 1));
                    while (sl[s])
                        s = (s + 1) & (slots - 1);
                    sl[s] = i + 1;
                }
                mv = m;
                mv_slots = sl;
                mv_pool = pool;
                mv_mask = slots - 1;
                mv_n = n;
            }
        }
    }

    close(fd);
    return mv_n ? (need + 3u) & ~(size_t)3u : 0;
}

const char *pv_moves_apply(const char *path)
{
    static char moved[MAX_PATH];
    const char *slash;
    uint32_t h;
    int s;

    if (!mv_n || !path)
        return NULL;
    slash = strrchr(path, '/');
    if (!slash || slash == path)
        return NULL;

    h = fnv1a_bytes(path, (size_t)(slash - path));
    s = (int)(h & (unsigned)mv_mask);
    while (mv_slots[s])
    {
        const struct move *m = &mv[mv_slots[s] - 1];
        if (m->dir_h == h)
        {
            snprintf(moved, sizeof(moved), "%s%s", mv_pool + m->off, slash);
            return moved;
        }
        s = (s + 1) & mv_mask;
    }
    return NULL;
}

unsigned long pv_moves_ident(const struct pv_moves_db *db)
{
    struct libfile_header h;

    if (!read_hdr(&h, db) || h.count == 0)
        return 0;
    return h.checksum ? h.checksum : 1;
}

void pv_moves_discard(void)
{
    remove(PV_MOVES_PATH);
}
