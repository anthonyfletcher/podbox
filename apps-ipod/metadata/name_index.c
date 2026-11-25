/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The names index: every name bracketed in the notes' pages -- `[Name]`,
 * which the Markdown back end marks TS_NAME_OPEN and TS_NAME_CLOSE -- with
 * where each is named. The notes screen's {{mentions}} come from it, and
 * an album's other pages from the album each page's front matter names.
 *
 * A step on the shared background thread (system/bg_task.c), as the file
 * index is: it runs after boot and after a USB session, the times files in
 * /Notes come and go, and whenever the file index's walk has listed the
 * pages again (notes_files.h). It fingerprints the pages -- each one's path,
 * size and time, as the walk found them -- and reads them again only when
 * that changed. A change to an image in /Notes or one of its folders asks
 * the art cache for a banner sweep (art_banner.h), which no library change
 * would otherwise bring.
 *
 * LIB_NAMES_FILE is a libfile: the mentions as records, sorted by name key,
 * and a tail holding fences (every NI_FENCE-th key of the mentions and of
 * the names), the files (each a page of the page index, by number and by a
 * hash of its path), the files whose front matter names an album, the names
 * (each key once, with its name as first written and how many files name
 * it) and the strings. Its marks hold the two fingerprints. It lives in the
 * cache: it is made entirely from the owner's files.
 *
 * Nothing holds it whole, so it has no size limit but NI_FILES_MAX and
 * NI_MENTIONS_MAX. A build sorts the mentions NI_RUN at a time into runs in
 * a scratch file and merges them; a reader finds a key through the fences
 * and reads the blocks it needs into a cache in the caller's buffer.
 *
 * Parts, in order:
 *   - the fingerprints
 *   - building: the pages read into runs, the runs merged, the tail
 *   - the task
 *   - reading
 ****************************************************************************/

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "usb.h"
#include "core_alloc.h"
#include "system/library_files.h"
#include "system/bg_task.h"
#include "settings/settings.h"
#include "files/file_index.h"       /* file_index_task */
#include "system/debug_log.h"
#include "database/libfile.h"
#include "database/tagcache.h"
#include "viewers/text_viewer/txt_source.h"
#include "viewers/text_viewer/ts_io_core.h"
#include "viewers/notes_viewer/notes_files.h"   /* the pages */
#include "art_name.h"
#include "art_banner.h"
#include "art_cache.h"
#include "albumart.h"           /* art_image_ext */
#include "name_index.h"

#define NI_VERSION      5
#define NI_FILES_MAX    0xffff      /* a mention's file is 16 bits */
#define NI_RUN          8192        /* mentions sorted in memory at once */
#define NI_RUN_POOL     (64 * 1024) /* their names */
#define NI_RUN_FILES    1024
#define NI_RUNS_MAX     128
#define NI_MENTIONS_MAX (NI_RUNS_MAX * NI_RUN)
#define NI_FENCE        256         /* records from one fence to the next */
#define NI_FENCES_MAX   (NI_MENTIONS_MAX / NI_FENCE)
#define NI_HEADS_MAX    0xfffe      /* a file's headings: a section is 16 bits */
#define NI_CHUNK        2048
#define NI_TEXT_MAX     128         /* a name or a heading, kept */
#define NI_OUT          (341 * sizeof(struct name_mention)) /* whole records */
#define NI_RECORD_MAX   (sizeof(struct name_mention) + 1 + NI_TEXT_MAX)
#define NI_BLOCK        4096        /* a reader's cache block */
#define NI_SLOTS_MAX    64

#define NI_RUNS_FILE    LIB_CACHE_DIR "/names.run"
#define NI_POOL_FILE    LIB_CACHE_DIR "/names.str"
#define NI_NAMES_FILE   LIB_CACHE_DIR "/names.nam"

/* The tail's own header */
struct ni_tail
{
    uint32_t nfiles, nalbums, nnames, nmfence, nnfence, pool;
};

/* A name: its key, its name as first written, and how many files name it */
struct ni_name
{
    uint32_t key;
    uint32_t name;              /* into the strings, or NAME_NO_STRING */
    uint16_t files;             /* at most 0xffff */
    uint16_t pad;
};

/* A mention while a run is read, and its name as written there */
struct ni_entry
{
    struct name_mention m;
    uint32_t name;              /* into the run's pool */
};

/* One run in NI_RUNS_FILE: its files, the files that name an album, then
 * its mentions in order, each followed by a length byte and, for the first
 * of a key, the name */
struct ni_run
{
    uint32_t off;
    uint32_t bytes;             /* of the mentions */
    uint32_t count;
    uint16_t nfiles, nalbums;
};

/* A file written through a buffer: a libfile, or a plain one */
struct ni_out
{
    int fd;
    struct libfile_writer *lw;
    uint16_t record;            /* the records' size, 0 for none */
    bool ok;
    uint32_t n, total;
    char buf[NI_OUT];
};

/* A run as the merge reads it */
struct ni_reader
{
    uint32_t pos, end;          /* what is left of the run in the file */
    uint32_t len, at, size;     /* bytes in buf, the next taken, room */
    char *buf;
    struct name_mention m;      /* its next mention */
    const char *name;           /* that mention's name, or NULL */
    size_t nlen;
    bool live;
};

/* What a pass works in: one core_alloc() block, carved up */
struct ni_work
{
    union
    {
        struct
        {
            struct ni_entry e[NI_RUN];
            char pool[NI_RUN_POOL];
            struct name_file files[NI_RUN_FILES];
            uint16_t albums[NI_RUN_FILES];
            char arena[TS_ARENA_PLAIN];
            char chunk[NI_CHUNK];
        } r;
        char merge[1];          /* the readers and their buffers */
    } u;
    struct ni_run runs[NI_RUNS_MAX];
    uint32_t mfence[NI_FENCES_MAX], nfence[NI_FENCES_MAX];
    struct ni_out runfile, pool, names, out;
    int nruns, count, nfiles, nalbums;
    uint32_t used;              /* of the run's pool */
    uint32_t files, mentions;   /* in all */
    bool full;                  /* a mention or a file was left out */
    bool stopped;               /* in the merge */
};

/* The merge reads every run at once through the reading's memory */
_Static_assert(sizeof(((struct ni_work *)0)->u) / NI_RUNS_MAX
               >= sizeof(struct ni_reader) + 2 * NI_RECORD_MAX,
               "the runs do not merge in the memory they are read in");

extern struct bg_task name_index_task;

/* ---- the fingerprints -------------------------------------------------- */

static uint32_t ni_mix(uint32_t h, uint32_t v)
{
    return (h ^ v) * 16777619u;
}

/* One banner folder's images into the fingerprint at `ctx` */
static bool ni_fingerprint_dir(const char *dir, int kind, uint32_t folder,
                               void *ctx)
{
    uint32_t *h = ctx;
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return true;
    while ((e = readdir(d)))
    {
        const char *dot = strrchr(e->d_name, '.'), *c;
        struct dirinfo info = dir_get_info(d, e);

        if (e->d_name[0] == '.' || (info.attribute & ATTR_DIRECTORY)
            || !dot || !art_image_ext(dot))
            continue;
        *h = ni_mix(*h, kind);
        *h = ni_mix(*h, folder);
        for (c = e->d_name; *c; c++)
            *h = ni_mix(*h, (unsigned char)*c);
        *h = ni_mix(*h, info.size);
        *h = ni_mix(*h, info.mtime);
    }
    closedir(d);
    return true;
}

/* The pages, and the images of the banner folders, each as one number */
static void ni_fingerprint(uint32_t *text, uint32_t *images)
{
    int i, n = notes_page_count();

    *text = *images = 0x811C9DC5u;
    for (i = 0; i < n; i++)
    {
        const struct notes_page *pg = notes_page_get(i);

        if (!pg)
            break;
        *text = ni_mix(*text, pg->hash);
        *text = ni_mix(*text, pg->stamp);
    }
    art_banner_each_dir(ni_fingerprint_dir, images);
}

/* ---- building ---------------------------------------------------------- */

static void ni_out_open(struct ni_out *o, int fd, struct libfile_writer *lw,
                        uint16_t record)
{
    o->fd = fd;
    o->lw = lw;
    o->record = record;
    o->ok = fd >= 0 || lw;
    o->n = o->total = 0;
}

static void ni_flush(struct ni_out *o)
{
    if (o->n && o->ok)
        o->ok = o->lw ? libfile_write(o->lw, o->buf, o->n,
                                      o->record ? o->n / o->record : 0)
                      : write(o->fd, o->buf, o->n) == (ssize_t)o->n;
    o->n = 0;
}

static void ni_put(struct ni_out *o, const void *data, size_t len)
{
    const char *p = data;

    while (len)
    {
        size_t n = MIN(len, NI_OUT - o->n);

        memcpy(o->buf + o->n, p, n);
        o->n += n;
        o->total += n;
        p += n;
        len -= n;
        if (o->n == NI_OUT)
            ni_flush(o);
    }
}

/* Flushed and closed; false when anything written to it failed */
static bool ni_out_close(struct ni_out *o)
{
    ni_flush(o);
    if (o->fd >= 0)
        close(o->fd);
    o->fd = -1;
    return o->ok;
}

/* `len` bytes at `off` of `fd` copied to `o`, through `buf` */
static void ni_copy(struct ni_out *o, int fd, uint32_t off, uint32_t len,
                    char *buf, size_t size)
{
    if (len && lseek(fd, off, SEEK_SET) != (off_t)off)
        o->ok = false;
    while (len && o->ok)
    {
        ssize_t got = read(fd, buf, MIN(len, size));

        if (got <= 0)
        {
            o->ok = false;
            break;
        }
        ni_put(o, buf, got);
        len -= got;
    }
}

static void ni_pad(struct ni_out *o)
{
    static const char zero[3];

    ni_put(o, zero, ALIGN_UP(o->total, 4) - o->total);
}

static int ni_order(const struct name_mention *x, const struct name_mention *y)
{
    if (x->key != y->key)
        return x->key < y->key ? -1 : 1;
    if (x->file != y->file)
        return x->file < y->file ? -1 : 1;
    return x->at < y->at ? -1 : x->at > y->at;
}

static int ni_compare(const void *a, const void *b)
{
    return ni_order(&((const struct ni_entry *)a)->m,
                    &((const struct ni_entry *)b)->m);
}

/* The run read so far into NI_RUNS_FILE, sorted; false when there is no
 * room for another. The last run is kept for the end of the pages. */
static bool ni_flush_run(struct ni_work *w, bool last)
{
    struct ni_run *run;
    int i;

    if (!w->count && !w->nfiles)
        return true;
    if (w->nruns >= NI_RUNS_MAX - !last)
        return false;
    run = &w->runs[w->nruns++];
    qsort(w->u.r.e, w->count, sizeof(w->u.r.e[0]), ni_compare);
    run->off = w->runfile.total;
    run->count = w->count;
    run->nfiles = w->nfiles;
    run->nalbums = w->nalbums;
    ni_put(&w->runfile, w->u.r.files, w->nfiles * sizeof(w->u.r.files[0]));
    ni_put(&w->runfile, w->u.r.albums, w->nalbums * sizeof(uint16_t));
    for (i = 0; i < w->count; i++)
    {
        const struct ni_entry *e = &w->u.r.e[i];
        bool first = !i || e[-1].m.key != e->m.key;
        uint8_t len = first ? strlen(w->u.r.pool + e->name) : 0;

        ni_put(&w->runfile, &e->m, sizeof(e->m));
        ni_put(&w->runfile, &len, 1);
        ni_put(&w->runfile, w->u.r.pool + e->name, len);
    }
    run->bytes = w->runfile.total - run->off
                 - w->nfiles * sizeof(w->u.r.files[0])
                 - w->nalbums * sizeof(uint16_t);
    w->count = w->nfiles = w->nalbums = 0;
    w->used = 0;
    return true;
}

/* `n` bytes of `s` in the strings; where they start */
static uint32_t ni_string(struct ni_work *w, const char *s, size_t n)
{
    uint32_t at = w->pool.total;

    ni_put(&w->pool, s, n);
    ni_put(&w->pool, "", 1);
    return at;
}

static void ni_add_mention(struct ni_work *w, uint32_t key, int section,
                           uint32_t at, const char *name, size_t n)
{
    struct ni_entry *e;

    if (w->mentions == NI_MENTIONS_MAX)
    {
        w->full = true;
        return;
    }
    if ((w->count == NI_RUN || w->used + n + 1 > NI_RUN_POOL)
        && !ni_flush_run(w, false))
    {
        w->full = true;
        return;
    }
    e = &w->u.r.e[w->count++];
    e->m.key = key;
    e->m.file = w->files - 1;
    e->m.section = section;
    e->m.at = at;
    e->name = w->used;
    memcpy(w->u.r.pool + w->used, name, n);
    w->u.r.pool[w->used + n] = '\0';
    w->used += n + 1;
    w->mentions++;
}

/* One text file read through the Markdown back end, its names recorded.
 * ts_read() never splits a UTF-8 sequence, so a mark is never cut across
 * two chunks. A heading's text takes in the names within it. */
static void ni_read_file(struct ni_work *w, struct name_file *fr,
                         const char *path)
{
    ts_config cfg = ts_config_default();
    struct ts_core_file f;
    ts_source *src;
    bs_io io;
    char head[NI_TEXT_MAX], name[NI_TEXT_MAX];
    char *chunk = w->u.r.chunk;
    uint32_t heads;
    int hlen = 0, nlen = 0;
    bool in_head = false, in_name = false;
    int section = NAME_NONE;
    uint32_t pos = 0, name_at = 0;
    long got;

    int fd = open(path, O_RDONLY);

    if (fd >= 0)
    {
        got = read(fd, chunk, NI_CHUNK - 1);
        close(fd);
        chunk[MAX(got, 0)] = '\0';
        if ((got = notes_front_value(chunk, NOTES_FRONT_ALBUM, name,
                                     sizeof(name))) > 0)
        {
            fr->album = ni_string(w, name, got);
            w->u.r.albums[w->nalbums++] = w->files - 1;
        }
    }
    /* `fr` is the run's: a run that fills is written out under it */
    fr->heads = heads = w->pool.total;
    cfg.force_format = TS_FMT_MARKDOWN;
    if (ts_io_core(&io, &f, path) != BS_OK)
        return;
    if (ts_open(&src, &io, path, w->u.r.arena, sizeof(w->u.r.arena), &cfg)
        != BS_OK)
    {
        io.close(io.ctx);
        return;
    }
    while ((got = ts_read(src, chunk, NI_CHUNK)) > 0)
    {
        long i;

        yield();                /* a long file is read a chunk at a time */
        for (i = 0; i < got; i++, pos++)
        {
            unsigned char c = chunk[i];

            if (c == 0xee && i + 2 < got
                && (unsigned char)chunk[i + 1] == 0x80)
            {
                unsigned char m = chunk[i + 2];

                if (m == 0x82)                  /* a heading */
                {
                    in_head = true;
                    hlen = 0;
                }
                else if (m == 0x89)             /* a name */
                {
                    in_name = true;
                    nlen = 0;
                    name_at = pos;
                }
                else if (m == 0x83 && in_head)
                {
                    uint32_t at = ni_string(w, head, hlen) - heads;

                    section = at <= NI_HEADS_MAX ? (int)at : NAME_NONE;
                    in_head = false;
                }
                else if (m == 0x8a && in_name)
                {
                    uint32_t key = art_name_hash_n(name, nlen);

                    if (key)
                        ni_add_mention(w, key, section, name_at, name, nlen);
                    in_name = false;
                }
                i += 2;
                pos += 2;
                continue;
            }
            if (c == '\n')
                continue;
            if (in_head && hlen < NI_TEXT_MAX - 1)
                head[hlen++] = c;
            if (in_name && nlen < NI_TEXT_MAX - 1)
                name[nlen++] = c;
        }
    }
    ts_close(src);
}

/* The run's next mention into r->m; false at its end */
static bool ni_advance(struct ni_reader *r, int fd)
{
    uint8_t len;

    if (r->len - r->at < NI_RECORD_MAX && r->pos < r->end)
    {
        uint32_t keep = r->len - r->at;
        ssize_t got;

        memmove(r->buf, r->buf + r->at, keep);
        r->len = keep;
        r->at = 0;
        got = lseek(fd, r->pos, SEEK_SET) == (off_t)r->pos
              ? read(fd, r->buf + keep, MIN(r->size - keep, r->end - r->pos))
              : -1;
        if (got <= 0)
            return r->live = false;
        r->len += got;
        r->pos += got;
    }
    if (r->len - r->at < sizeof(r->m) + 1)
        return r->live = false;
    memcpy(&r->m, r->buf + r->at, sizeof(r->m));
    len = r->buf[r->at + sizeof(r->m)];
    if (r->len - r->at < sizeof(r->m) + 1 + len)
        return r->live = false;
    r->name = len ? r->buf + r->at + sizeof(r->m) + 1 : NULL;
    r->nlen = len;
    r->at += sizeof(r->m) + 1 + len;
    return r->live = true;
}

/* A name done with: into the names, and a fence every NI_FENCE */
static void ni_put_name(struct ni_work *w, uint32_t *nnames,
                        const struct ni_name *nm)
{
    if (*nnames % NI_FENCE == 0)
        w->nfence[*nnames / NI_FENCE] = nm->key;
    ni_put(&w->names, nm, sizeof(*nm));
    (*nnames)++;
}

/* The runs merged into `w->out` in order, `count` of them, the names into
 * NI_NAMES_FILE and their names into the strings; false when stopped or
 * when a run is not read to its end */
static bool ni_merge(struct ni_work *w, int fd, uint32_t *nmentions,
                     uint32_t *nnames)
{
    struct ni_reader *r = (struct ni_reader *)w->u.merge;
    char *buf = (char *)(r + w->nruns);
    size_t size = ALIGN_DOWN((sizeof(w->u) - w->nruns * sizeof(*r))
                             / MAX(w->nruns, 1), 4);
    struct ni_name nm = { 0, NAME_NO_STRING, 0, 0 };
    uint32_t count = 0;
    int i, last = -1;
    bool any = false;

    if (size < 2 * NI_RECORD_MAX)
        return false;
    for (i = 0; i < w->nruns; i++)
    {
        r[i].pos = w->runs[i].off
                   + w->runs[i].nfiles * sizeof(struct name_file)
                   + w->runs[i].nalbums * sizeof(uint16_t);
        r[i].end = r[i].pos + w->runs[i].bytes;
        r[i].buf = buf + i * size;
        r[i].size = size;
        r[i].len = r[i].at = 0;
        ni_advance(&r[i], fd);
    }
    *nnames = 0;
    while (w->out.ok && w->names.ok && w->pool.ok)
    {
        struct ni_reader *min = NULL;

        for (i = 0; i < w->nruns; i++)
            if (r[i].live && (!min || ni_order(&r[i].m, &min->m) < 0))
                min = &r[i];
        if (!min || count == NI_MENTIONS_MAX)
            break;
        if (count % NI_FENCE == 0)
            w->mfence[count / NI_FENCE] = min->m.key;
        ni_put(&w->out, &min->m, sizeof(min->m));
        count++;
        /* The first of a key is the earliest, which its run kept the
         * name of */
        if (!any || min->m.key != nm.key)
        {
            if (any)
                ni_put_name(w, nnames, &nm);
            any = true;
            nm.key = min->m.key;
            nm.name = min->name ? ni_string(w, min->name, min->nlen)
                                : NAME_NO_STRING;
            nm.files = 0;
            last = -1;
        }
        if (min->m.file != last && nm.files < 0xffff)
            nm.files++;
        last = min->m.file;
        ni_advance(min, fd);
        if (count % 256 == 0)
        {
            if (bg_task_should_stop(&name_index_task))
            {
                w->stopped = true;
                return false;
            }
            yield();
        }
    }
    if (any)
        ni_put_name(w, nnames, &nm);
    *nmentions = count;
    for (i = 0; i < w->nruns; i++)
        if (r[i].live || r[i].pos != r[i].end || r[i].at != r[i].len)
            return false;
    return w->out.ok && w->names.ok && w->pool.ok;
}

/* The index written from the runs */
static bool ni_write(struct ni_work *w, uint32_t text_fp, uint32_t img_fp)
{
    struct libfile_writer lw;
    struct libfile_marks marks = { (int32_t)text_fp, (int32_t)img_fp, 0 };
    struct ni_tail t;
    uint32_t nmentions = 0, nnames = 0;
    int fd, i;
    bool ok;

    if (!ni_out_close(&w->runfile)
        || (fd = open(NI_RUNS_FILE, O_RDONLY)) < 0)
        return false;
    if (!libfile_begin(&lw, LIB_NAMES_FILE, LIB_NAMES_MAGIC, NI_VERSION,
                       sizeof(struct name_mention), &marks))
    {
        close(fd);
        return false;
    }
    ni_out_open(&w->out, -1, &lw, sizeof(struct name_mention));
    ni_out_open(&w->names, open(NI_NAMES_FILE, O_WRONLY | O_CREAT | O_TRUNC,
                                0666), NULL, 0);
    ok = ni_merge(w, fd, &nmentions, &nnames);
    ok = ni_out_close(&w->names) && ok;
    ok = ni_out_close(&w->pool) && ok;

    /* The tail, its parts copied in through the merge's memory */
    ni_flush(&w->out);
    w->out.record = 0;
    t.nfiles = w->files;
    t.nalbums = 0;
    for (i = 0; i < w->nruns; i++)
        t.nalbums += w->runs[i].nalbums;
    t.nnames = nnames;
    t.nmfence = (nmentions + NI_FENCE - 1) / NI_FENCE;
    t.nnfence = (nnames + NI_FENCE - 1) / NI_FENCE;
    t.pool = w->pool.total;
    ni_put(&w->out, &t, sizeof(t));
    ni_put(&w->out, w->mfence, t.nmfence * sizeof(uint32_t));
    ni_put(&w->out, w->nfence, t.nnfence * sizeof(uint32_t));
    for (i = 0; ok && i < w->nruns; i++)
        ni_copy(&w->out, fd, w->runs[i].off,
                w->runs[i].nfiles * sizeof(struct name_file),
                w->u.merge, sizeof(w->u));
    for (i = 0; ok && i < w->nruns; i++)
        ni_copy(&w->out, fd, w->runs[i].off
                             + w->runs[i].nfiles * sizeof(struct name_file),
                w->runs[i].nalbums * sizeof(uint16_t),
                w->u.merge, sizeof(w->u));
    ni_pad(&w->out);
    close(fd);
    if (ok && (fd = open(NI_NAMES_FILE, O_RDONLY)) >= 0)
    {
        ni_copy(&w->out, fd, 0, nnames * sizeof(struct ni_name),
                w->u.merge, sizeof(w->u));
        close(fd);
    }
    else
        ok = false;
    if (ok && (fd = open(NI_POOL_FILE, O_RDONLY)) >= 0)
    {
        ni_copy(&w->out, fd, 0, t.pool, w->u.merge, sizeof(w->u));
        close(fd);
    }
    else
        ok = false;
    ni_pad(&w->out);
    ni_flush(&w->out);
    ok = libfile_finish(&lw, ok && w->out.ok);
    if (ok)
        debug_log(DEBUG_LOG_TASKS, "%s: %lu files, %lu mentions, %lu names,"
                  " %d runs%s", name_index_task.name, (unsigned long)t.nfiles,
                  (unsigned long)nmentions, (unsigned long)nnames,
                  w->nruns, w->full ? ", some left out" : "");
    return ok;
}

/* The index made again from the text files; false when stopped. One that
 * cannot be written waits for the next request rather than the next tick,
 * which would read every file again to fail again. */
static bool ni_build(struct ni_work *w, uint32_t text_fp, uint32_t img_fp)
{
    static char path[MAX_PATH];
    unsigned int gen = notes_pages_generation();
    int i, n = notes_page_count();
    bool done = true;

    w->nruns = w->count = w->nfiles = w->nalbums = 0;
    w->used = w->files = w->mentions = 0;
    w->full = w->stopped = false;
    ni_out_open(&w->runfile, open(NI_RUNS_FILE, O_WRONLY | O_CREAT | O_TRUNC,
                                  0666), NULL, 0);
    ni_out_open(&w->pool, open(NI_POOL_FILE, O_WRONLY | O_CREAT | O_TRUNC,
                               0666), NULL, 0);
    for (i = 0; i < n && w->runfile.ok && w->pool.ok; i++)
    {
        const struct notes_page *pg = notes_page_get(i);
        struct name_file *fr;

        /* A walk that completes meanwhile renumbers the pages and asks for
         * this again */
        if (!pg || notes_pages_generation() != gen
            || bg_task_should_stop(&name_index_task))
        {
            done = false;
            break;
        }
        if (w->files == NI_FILES_MAX)
        {
            w->full = true;
            break;
        }
        if (w->nfiles == NI_RUN_FILES && !ni_flush_run(w, false))
        {
            w->full = true;
            break;
        }
        fr = &w->u.r.files[w->nfiles++];
        memset(fr, 0, sizeof(*fr));
        fr->hash = pg->hash;
        fr->page = i;
        fr->album = NAME_NO_STRING;
        w->files++;
        if (notes_page_path(pg, path, sizeof(path)))
            ni_read_file(w, fr, path);
        yield();
    }
    if (done && !(ni_flush_run(w, true) && ni_write(w, text_fp, img_fp))
        && !w->stopped)
        debug_log(DEBUG_LOG_TASKS, "%s: %s not written",
                  name_index_task.name, LIB_NAMES_FILE);
    ni_out_close(&w->runfile);
    ni_out_close(&w->pool);
    remove(NI_RUNS_FILE);
    remove(NI_POOL_FILE);
    remove(NI_NAMES_FILE);
    return done && !w->stopped;
}

/* ---- the task ---------------------------------------------------------- */

static bool ni_wanted;
static bool ni_on;              /* the setting, as the task last saw it */

static bool ni_whole(const struct libfile_header *h);

/* One pass; false when it has to be tried again */
static bool ni_run(void)
{
    struct libfile_header h;
    uint32_t text_fp, img_fp;
    bool have = libfile_peek(LIB_NAMES_FILE, LIB_NAMES_MAGIC, NI_VERSION, &h);
    int handle;
    bool done;

    ni_fingerprint(&text_fp, &img_fp);
    if (!have || (uint32_t)h.marks.commitid != img_fp)
        art_cache_banners_changed();
    if (have && (uint32_t)h.marks.entries == text_fp && ni_whole(&h))
    {
        if ((uint32_t)h.marks.commitid != img_fp)
        {
            struct libfile_marks m = h.marks;

            m.commitid = (int32_t)img_fp;
            libfile_set_marks(LIB_NAMES_FILE, LIB_NAMES_MAGIC, NI_VERSION,
                              &m);
        }
        return true;
    }

    if (core_allocatable() < sizeof(struct ni_work))
        return false;
    handle = core_alloc(sizeof(struct ni_work));
    if (handle <= 0)
        return false;
    debug_log(DEBUG_LOG_TASKS, "%s: reading /Notes", name_index_task.name);
    done = ni_build(core_get_data_pinned(handle), text_fp, img_fp);
    core_unpin(handle);
    core_free(handle);
    debug_log(DEBUG_LOG_TASKS, "%s: %s", name_index_task.name,
              done ? "done" : "interrupted");
    return done;
}

/* bg_task.tick: as the file index's, while the database leaves the disk
 * alone, no host wants it, and nothing that outranks this is waiting */
static void ni_tick(void)
{
    /* Read here rather than in a setting callback: Reset Settings runs no
     * callbacks. Switched, the file index walks again, since /Notes is a
     * folder of books while the setting is off; on, the index is read. */
    if (ni_on != global_settings.liner_notes)
    {
        ni_on = global_settings.liner_notes;
        if (ni_on)
            ni_wanted = true;
        bg_task_update(&file_index_task);
    }
    if (ni_wanted && ni_on
        && !tagcache_is_busy() && !usb_host_wants_storage()
        && !bg_task_preempted(&name_index_task))
    {
        ni_wanted = false;
        if (!ni_run())
            ni_wanted = true;
    }
}

/* bg_task.request: after boot, after a USB session, and on a rebuild alike.
 * The fingerprints decide whether there is anything to do. */
static void ni_request(bool rebuild)
{
    (void)rebuild;
    ni_wanted = true;
}

struct bg_task name_index_task =
{
    .name       = "names index",
    .rank       = BG_RANK_FILES,
    .work_bytes = sizeof(struct ni_work),
    .request    = ni_request,
    .tick       = ni_tick,
};

void name_index_pages_changed(void)
{
    ni_wanted = true;
}

void name_index_init(void)
{
    bg_task_init(&name_index_task);
    ni_wanted = true;
    ni_on = global_settings.liner_notes;
}

/* ---- reading ----------------------------------------------------------- */

struct ni_slot
{
    uint32_t block;             /* NI_NO_BLOCK for none */
    uint32_t last;              /* when it was last used */
};
#define NI_NO_BLOCK 0xffffffffu

/* Block `block` of the file, read when the cache does not hold it */
static const char *ni_block(struct name_index *ix, uint32_t block)
{
    struct ni_slot *slots = ix->slots;
    uint32_t off = block * NI_BLOCK, n;
    char *data;
    int i, old = 0;

    for (i = 0; i < ix->nslots; i++)
    {
        if (slots[i].block == block)
        {
            slots[i].last = ++ix->tick;
            return ix->data + i * NI_BLOCK;
        }
        if (slots[i].last < slots[old].last)
            old = i;
    }
    data = ix->data + old * NI_BLOCK;
    n = MIN((uint32_t)NI_BLOCK, ix->size - off);
    slots[old].block = NI_NO_BLOCK;
    if (lseek(ix->fd, off, SEEK_SET) != (off_t)off
        || read(ix->fd, data, n) != (ssize_t)n)
        return NULL;
    slots[old].block = block;
    slots[old].last = ++ix->tick;
    return data;
}

/* `len` bytes at `off`; false past the end or when a read fails */
static bool ni_read(struct name_index *ix, uint32_t off, void *buf,
                    size_t len)
{
    char *p = buf;

    if (ix->fd < 0 || off > ix->size || len > ix->size - off)
        return false;
    while (len)
    {
        uint32_t at = off % NI_BLOCK;
        size_t n = MIN(len, NI_BLOCK - at);
        const char *data = ni_block(ix, off / NI_BLOCK);

        if (!data)
            return false;
        memcpy(p, data + at, n);
        p += n;
        off += n;
        len -= n;
    }
    return true;
}

/* The first of the `n` records of `size` bytes at `base` whose key, its
 * first four bytes, is `key` or above; `n` when there is none. Every
 * NI_FENCE-th record's key is at `fence`. */
static uint32_t ni_lower(struct name_index *ix, uint32_t base, uint32_t n,
                         uint32_t size, uint32_t fence, uint32_t nfence,
                         uint32_t key)
{
    uint32_t lo = 0, hi = nfence, k;

    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;

        if (!ni_read(ix, fence + mid * sizeof(k), &k, sizeof(k)))
            return n;
        if (k < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return 0;
    /* Between the last fence below the key and the next */
    hi = MIN(lo * NI_FENCE, n);
    lo = (lo - 1) * NI_FENCE;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;

        if (!ni_read(ix, base + mid * size, &k, sizeof(k)))
            return n;
        if (k < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* The parts of an index of `count` mentions and the tail `t` into `ix`,
 * whose size is the file's; false unless they end where the file does */
static bool ni_layout(struct name_index *ix, uint32_t count,
                      const struct ni_tail *t)
{
    if (t->nfiles > NI_FILES_MAX || t->nalbums > t->nfiles
        || t->nnames > count
        || t->nmfence != (count + NI_FENCE - 1) / NI_FENCE
        || t->nnfence != (t->nnames + NI_FENCE - 1) / NI_FENCE
        || t->pool > ix->size)
        return false;
    ix->count = count;
    ix->mentions = sizeof(struct libfile_header);
    ix->mfence = ix->mentions + count * sizeof(struct name_mention)
                 + sizeof(*t);
    ix->nfence = ix->mfence + t->nmfence * sizeof(uint32_t);
    ix->files = ix->nfence + t->nnfence * sizeof(uint32_t);
    ix->albums = ix->files + t->nfiles * sizeof(struct name_file);
    ix->names = ix->albums + ALIGN_UP(t->nalbums * sizeof(uint16_t), 4);
    ix->pool = ix->names + t->nnames * sizeof(struct ni_name);
    ix->pool_end = ix->pool + t->pool;
    ix->nfiles = t->nfiles;
    ix->nalbums = t->nalbums;
    ix->nnames = t->nnames;
    return ix->pool_end <= ix->size && ALIGN_UP(ix->pool_end, 4) == ix->size;
}

/* Whether the saved index that `h` heads is all there, read without a
 * cache: a file cut short is built again */
static bool ni_whole(const struct libfile_header *h)
{
    struct name_index ix;
    struct ni_tail t;
    int fd = open(LIB_NAMES_FILE, O_RDONLY);
    off_t end = sizeof(*h) + (off_t)h->count * sizeof(struct name_mention);
    bool ok;

    off_t size;

    if (fd < 0)
        return false;
    memset(&ix, 0, sizeof(ix));
    size = ffilesize(fd);
    ix.size = size > 0 ? (uint32_t)size : 0;
    ok = h->count <= NI_MENTIONS_MAX
         && lseek(fd, end, SEEK_SET) == end
         && read(fd, &t, sizeof(t)) == (ssize_t)sizeof(t)
         && ni_layout(&ix, h->count, &t);
    close(fd);
    return ok;
}

bool name_index_open(void *buf, size_t size, struct name_index *ix)
{
    struct libfile_header h;
    struct ni_tail t;
    uintptr_t start = ALIGN_UP((uintptr_t)buf, 4);
    size_t room = size > start - (uintptr_t)buf
                  ? size - (start - (uintptr_t)buf) : 0;
    off_t fsize;
    uint32_t end;
    int i;

    memset(ix, 0, sizeof(*ix));
    ix->fd = -1;
    ix->nslots = MIN(room / (NI_BLOCK + sizeof(struct ni_slot)),
                     (size_t)NI_SLOTS_MAX);
    if (!ix->nslots)
        return false;
    ix->slots = (struct ni_slot *)start;
    ix->data = (char *)(ix->slots + ix->nslots);
    for (i = 0; i < ix->nslots; i++)
    {
        ix->slots[i].block = NI_NO_BLOCK;
        ix->slots[i].last = 0;
    }

    /* The header and the tail's own, read and checked as a whole: every
     * part where the counts put it, ending where the file does. The
     * checksum is not read, which would mean reading all of it; every
     * read is checked against the file's end instead. */
    ix->fd = open(LIB_NAMES_FILE, O_RDONLY);
    if (ix->fd < 0)
        return false;
    fsize = ffilesize(ix->fd);
    ix->size = fsize > 0 ? (uint32_t)fsize : 0;
    if (ix->size < sizeof(h) + sizeof(t)
        || !ni_read(ix, 0, &h, sizeof(h))
        || h.magic != LIB_NAMES_MAGIC || h.version != NI_VERSION
        || h.record_size != sizeof(struct name_mention)
        || h.count > NI_MENTIONS_MAX)
        goto bad;
    end = sizeof(h) + h.count * sizeof(struct name_mention);
    if (!ni_read(ix, end, &t, sizeof(t)) || !ni_layout(ix, h.count, &t))
        goto bad;
    return true;

  bad:
    name_index_close(ix);
    return false;
}

void name_index_close(struct name_index *ix)
{
    if (ix->fd >= 0)
        close(ix->fd);
    ix->fd = -1;
}

uint32_t name_index_first(struct name_index *ix, uint32_t key)
{
    return ni_lower(ix, ix->mentions, ix->count, sizeof(struct name_mention),
                    ix->mfence, (ix->count + NI_FENCE - 1) / NI_FENCE, key);
}

bool name_index_mention(struct name_index *ix, uint32_t i,
                        struct name_mention *m)
{
    return i < ix->count
           && ni_read(ix, ix->mentions + i * sizeof(*m), m, sizeof(*m));
}

bool name_index_names_in(struct name_index *ix, uint32_t key,
                         uint32_t file)
{
    uint32_t lo = name_index_first(ix, key);
    uint32_t hi = key == 0xffffffffu ? ix->count
                                     : name_index_first(ix, key + 1);
    struct name_mention m;

    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;

        if (!name_index_mention(ix, mid, &m))
            return false;
        if (m.file < file)
            lo = mid + 1;
        else
            hi = mid;
    }
    return name_index_mention(ix, lo, &m) && m.key == key && m.file == file;
}

bool name_index_file(struct name_index *ix, uint32_t file,
                     struct name_file *f)
{
    return file < ix->nfiles
           && ni_read(ix, ix->files + file * sizeof(*f), f, sizeof(*f));
}

int name_index_album_file(struct name_index *ix, uint32_t i)
{
    uint16_t file;

    return i < ix->nalbums
           && ni_read(ix, ix->albums + i * sizeof(file), &file, sizeof(file))
           ? file : -1;
}

bool name_index_string(struct name_index *ix, uint32_t off, char *buf,
                       size_t size)
{
    uint32_t n;

    buf[0] = '\0';
    if (off == NAME_NO_STRING || off >= ix->pool_end - ix->pool || size < 2)
        return false;
    n = MIN(size - 1, ix->pool_end - ix->pool - off);
    if (!ni_read(ix, ix->pool + off, buf, n))
    {
        buf[0] = '\0';
        return false;
    }
    buf[n] = '\0';
    return true;
}

bool name_index_section(struct name_index *ix, const struct name_file *f,
                        const struct name_mention *m, char *buf, size_t size)
{
    buf[0] = '\0';
    return m->section != NAME_NONE
           && name_index_string(ix, f->heads + m->section, buf, size);
}

/* The name `key`, false when there is none */
static bool ni_name(struct name_index *ix, uint32_t key, struct ni_name *nm)
{
    uint32_t i = ni_lower(ix, ix->names, ix->nnames, sizeof(*nm), ix->nfence,
                          (ix->nnames + NI_FENCE - 1) / NI_FENCE, key);

    return i < ix->nnames
           && ni_read(ix, ix->names + i * sizeof(*nm), nm, sizeof(*nm))
           && nm->key == key;
}

bool name_index_name(struct name_index *ix, uint32_t key, char *buf,
                     size_t size)
{
    struct ni_name nm;

    buf[0] = '\0';
    return ni_name(ix, key, &nm) && name_index_string(ix, nm.name, buf, size);
}

int name_index_files_naming(struct name_index *ix, uint32_t key)
{
    struct ni_name nm;

    return ni_name(ix, key, &nm) ? nm.files : 0;
}

uint32_t name_index_path_hash(const char *path)
{
    uint32_t h = 0x811C9DC5u;

    while (*path)
        h = ni_mix(h, (unsigned char)*path++);
    return h;
}

/* The index opened with a cache of one block, for a menu deciding whether
 * to offer a row: a few reads, with the disk spinning or not */
bool name_index_named(uint32_t key, uint32_t alt)
{
    static char buf[NI_BLOCK + sizeof(struct ni_slot) + 4];
    struct name_index ix;
    bool found;

    if (!global_settings.liner_notes || !(key || alt)
        || !name_index_open(buf, sizeof(buf), &ix))
        return false;
    found = (key && name_index_files_naming(&ix, key))
            || (alt && name_index_files_naming(&ix, alt));
    name_index_close(&ix);
    return found;
}
