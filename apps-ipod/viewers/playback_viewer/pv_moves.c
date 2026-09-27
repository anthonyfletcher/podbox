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
 * It is built right after a name-map sweep, which is when the database has
 * changed, and saved keyed to the same database state as the map.
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
#include "system/hash.h"
#include "rbpaths.h"
#include "database/tagcache.h"
#include "widgets/splash.h"
#include "pv_log.h"
#include "pv_moves.h"

#define PV_MOVES_PATH  ROCKBOX_DIR "/pv_moves.dat"
#define PV_MOVES_MAGIC 0x50564d31UL   /* "PVM1" */

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
    sc.files[sc.f_n].found  = false;
    sc.fslots[s] = ++sc.f_n;
}

/* Walk 1: which logged files the database still holds where they were. */
static bool mark_found(void)
{
    struct tagcache_search tcs;
    char fname[MAX_PATH];
    int n = 0;

    if (!tagcache_search(&tcs, tag_filename))
        return false;

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

        if ((++n & 255) == 0)
            splashf(0, "Looking for moved folders (%d)", n);
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
    int n = 0;

    if (!tagcache_search(&tcs, tag_filename))
        return false;

    while (tagcache_get_next(&tcs, fname, sizeof(fname)))
    {
        const char *slash = strrchr(fname, '/');
        uint32_t bh, dh;
        int s;

        if ((++n & 255) == 0)
            splashf(0, "Looking for moved folders (%d)", n);
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

static void moves_save(const struct move *m, int n, const char *pool,
                       unsigned pool_bytes, int db_entries, long db_commit)
{
    unsigned long hdr[6];
    size_t m_bytes = (size_t)n * sizeof(struct move);
    uint32_t sum = fnv1a_bytes(m, m_bytes);
    bool ok;
    int fd;

    for (unsigned i = 0; i < pool_bytes; i++)
        sum = fnv1a_byte(sum, (unsigned char)pool[i]);

    hdr[0] = PV_MOVES_MAGIC;
    hdr[1] = (unsigned long)db_entries;
    hdr[2] = (unsigned long)db_commit;
    hdr[3] = (unsigned long)n;
    hdr[4] = pool_bytes;
    hdr[5] = sum ? sum : 1;

    fd = open(PV_MOVES_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    ok = write(fd, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
      && write(fd, m, m_bytes) == (ssize_t)m_bytes
      && write(fd, pool, pool_bytes) == (ssize_t)pool_bytes;
    close(fd);

    if (!ok)
        remove(PV_MOVES_PATH);
}

void pv_moves_build(void *scratch, size_t size, int db_entries, long db_commit)
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

    splashf(0, "Looking for moved folders");
    if (pv_log_read(PV_SRC_PLAYBACK, log_cb, NULL) < 0 || sc.f_n == 0)
        return;

    if (!mark_found())
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
        const struct cand *c, *best = NULL;
        const char *dir;
        unsigned len;

        if (!ld->missing || ld->cand < 0)
            continue;
        c = &sc.cands[ld->cand];
        for (int k = 0; k < CANDS; k++)
            if (c[k].votes && (!best || c[k].votes > best->votes))
                best = &c[k];
        if (!best || !accept(ld, best))
            continue;

        dir = sc.pool + best->off;
        len = (unsigned)strlen(dir) + 1;
        if (out_used + len > out_cap)
            break;
        memcpy(out_pool + out_used, dir, len);
        out[n_moves].dir_h = ld->dir_h;
        out[n_moves].off = out_used;
        out_used += len;
        n_moves++;
    }

    moves_save(out, n_moves, out_pool, out_used, db_entries, db_commit);
}

/* ------------------------------------------------------------------- use */

static bool read_hdr(int fd, unsigned long hdr[6], int db_entries,
                     long db_commit)
{
    return read(fd, hdr, 6 * sizeof(unsigned long))
               == (ssize_t)(6 * sizeof(unsigned long))
        && hdr[0] == PV_MOVES_MAGIC
        && (int)hdr[1] == db_entries
        && (long)hdr[2] == db_commit
        && hdr[3] <= MOVES_MAX
        && hdr[4] <= MOVES_POOL_MAX;
}

bool pv_moves_stale(int db_entries, long db_commit)
{
    unsigned long hdr[6];
    int fd = open(PV_MOVES_PATH, O_RDONLY);
    bool ok;

    if (fd < 0)
        return true;
    ok = read_hdr(fd, hdr, db_entries, db_commit);
    close(fd);
    return !ok;
}

void pv_moves_forget(void)
{
    mv = NULL;
    mv_slots = NULL;
    mv_pool = NULL;
    mv_n = 0;
}

size_t pv_moves_load(void *buf, size_t size, int db_entries, long db_commit)
{
    unsigned long hdr[6];
    size_t need = 0;
    int fd, n, slots;

    pv_moves_forget();
    fd = open(PV_MOVES_PATH, O_RDONLY);
    if (fd < 0)
        return 0;

    if (read_hdr(fd, hdr, db_entries, db_commit) && hdr[3] > 0 && hdr[4] > 0)
    {
        n = (int)hdr[3];
        slots = next_pow2(n * 2);
        need = (size_t)n * sizeof(struct move) + (size_t)slots * sizeof(int)
             + hdr[4];

        if (need <= size)
        {
            struct move *m = buf;
            int *sl = (int *)((char *)buf + (size_t)n * sizeof(struct move));
            char *pool = (char *)sl + (size_t)slots * sizeof(int);
            bool ok = read(fd, m, (size_t)n * sizeof(struct move))
                          == (ssize_t)((size_t)n * sizeof(struct move))
                   && read(fd, pool, hdr[4]) == (ssize_t)hdr[4]
                   && pool[hdr[4] - 1] == '\0';

            for (int i = 0; ok && i < n; i++)
                ok = m[i].off < hdr[4];

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

unsigned long pv_moves_ident(int db_entries, long db_commit)
{
    unsigned long hdr[6];
    int fd = open(PV_MOVES_PATH, O_RDONLY);
    bool ok;

    if (fd < 0)
        return 0;
    ok = read_hdr(fd, hdr, db_entries, db_commit);
    close(fd);
    return (ok && hdr[3] > 0) ? hdr[5] : 0;
}

void pv_moves_discard(void)
{
    remove(PV_MOVES_PATH);
}
