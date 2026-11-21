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
 * that changed. A change to an image of /Notes/People or /Notes/Albums asks
 * the art cache for a banner sweep (art_banner.h), which no library change
 * would otherwise bring.
 *
 * LIB_NAMES_FILE is a libfile: the mentions as records, sorted by name key,
 * and a tail holding the files (each a page of the page index, by number
 * and by a hash of its path), the strings (section headings, names as first
 * written) and which string each name key is. Its marks hold the two
 * fingerprints. It lives in the cache: it is made entirely from the owner's
 * files.
 *
 * Parts, in order:
 *   - the fingerprints
 *   - building: one file read for its names, then the index written
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

#define NI_VERSION      4
#define NI_FILES_MAX    2048        /* the page index's NW_PAGES_MAX */
#define NI_MENTIONS_MAX 4096
#define NI_STRINGS_MAX  4096
#define NI_NAMES_MAX    2048
#define NI_POOL         (32 * 1024)
#define NI_CHUNK        2048
#define NI_TEXT_MAX     128         /* a name or a heading, kept */

/* The tail's own header */
struct ni_tail
{
    uint32_t nfiles, nstrings, nnames, pool;
};

/* What a pass works in: one core_alloc() block, carved up */
struct ni_work
{
    struct name_file files[NI_FILES_MAX];
    struct name_mention m[NI_MENTIONS_MAX];
    uint32_t str_off[NI_STRINGS_MAX];
    uint32_t name_key[NI_NAMES_MAX];
    uint16_t name_str[NI_NAMES_MAX];
    char pool[NI_POOL];
    char arena[TS_ARENA_PLAIN];
    char chunk[NI_CHUNK];
    int nfiles, count, nstrings, nnames;
    uint32_t used;
};

/* The largest index the caps allow loads into NAME_INDEX_LOAD_BYTES */
_Static_assert(NI_MENTIONS_MAX * sizeof(struct name_mention)
               + sizeof(struct ni_tail)
               + NI_FILES_MAX * sizeof(struct name_file)
               + NI_STRINGS_MAX * sizeof(uint32_t)
               + NI_NAMES_MAX * (sizeof(uint32_t) + sizeof(uint16_t))
               + NI_POOL + 16 <= NAME_INDEX_LOAD_BYTES,
               "the largest names index does not load");

static const char *const ni_dirs[] = { NOTES_PEOPLE_DIR, NOTES_ALBUMS_DIR };

extern struct bg_task name_index_task;

/* ---- the fingerprints -------------------------------------------------- */

static uint32_t ni_mix(uint32_t h, uint32_t v)
{
    return (h ^ v) * 16777619u;
}

/* The pages, and the images of the two folders, each as one number */
static void ni_fingerprint(uint32_t *text, uint32_t *images)
{
    int i, n = notes_page_count();

    *text = *images = 0x811C9DC5u;
    for (i = 0; i < n; i++)
    {
        const struct notes_page *pg = notes_page_get(i);
        const char *c;

        if (!pg)
            break;
        for (c = notes_page_path(pg); *c; c++)
            *text = ni_mix(*text, (unsigned char)*c);
        *text = ni_mix(*text, pg->size);
        *text = ni_mix(*text, pg->mtime);
    }
    for (i = 0; i < (int)ARRAYLEN(ni_dirs); i++)
    {
        DIR *d = opendir(ni_dirs[i]);
        struct dirent *e;

        if (!d)
            continue;
        while ((e = readdir(d)))
        {
            const char *dot = strrchr(e->d_name, '.'), *c;
            struct dirinfo info = dir_get_info(d, e);
            uint32_t *h;

            if (e->d_name[0] == '.' || (info.attribute & ATTR_DIRECTORY)
                || !dot || !art_image_ext(dot))
                continue;
            h = images;
            *h = ni_mix(*h, i);
            for (c = e->d_name; *c; c++)
                *h = ni_mix(*h, (unsigned char)*c);
            *h = ni_mix(*h, info.size);
            *h = ni_mix(*h, info.mtime);
        }
        closedir(d);
    }
}

/* ---- building ---------------------------------------------------------- */

/* `s`, `n` bytes, as a string of the index, the same one when it is there */
static int ni_string(struct ni_work *w, const char *s, size_t n)
{
    int i;

    for (i = 0; i < w->nstrings; i++)
    {
        const char *t = w->pool + w->str_off[i];

        if (strlen(t) == n && !memcmp(t, s, n))
            return i;
    }
    if (w->nstrings == NI_STRINGS_MAX || w->used + n + 1 > NI_POOL)
        return NAME_NONE;
    w->str_off[w->nstrings] = w->used;
    memcpy(w->pool + w->used, s, n);
    w->pool[w->used + n] = '\0';
    w->used += n + 1;
    return w->nstrings++;
}

static void ni_add_name(struct ni_work *w, uint32_t key, const char *s,
                        size_t n)
{
    int i, str;

    for (i = 0; i < w->nnames; i++)
        if (w->name_key[i] == key)
            return;                     /* as first written */
    if (w->nnames == NI_NAMES_MAX
        || (str = ni_string(w, s, n)) == NAME_NONE)
        return;
    w->name_key[w->nnames] = key;
    w->name_str[w->nnames++] = str;
}

/* One text file read through the Markdown back end, its names recorded.
 * ts_read() never splits a UTF-8 sequence, so a mark is never cut across
 * two chunks. A heading's text takes in the names within it. */
static void ni_read_file(struct ni_work *w, int file, const char *path)
{
    ts_config cfg = ts_config_default();
    struct ts_core_file f;
    ts_source *src;
    bs_io io;
    char head[NI_TEXT_MAX], name[NI_TEXT_MAX];
    int hlen = 0, nlen = 0;
    bool in_head = false, in_name = false;
    int section = NAME_NONE;
    uint32_t pos = 0, name_at = 0;
    long got;

    int fd = open(path, O_RDONLY);

    if (fd >= 0)
    {
        got = read(fd, w->chunk, sizeof(w->chunk) - 1);
        close(fd);
        w->chunk[MAX(got, 0)] = '\0';
        if ((got = notes_front_value(w->chunk, NOTES_FRONT_ALBUM, name,
                                     sizeof(name))) > 0)
            w->files[file].album = ni_string(w, name, got);
    }
    cfg.force_format = TS_FMT_MARKDOWN;
    if (ts_io_core(&io, &f, path) != BS_OK)
        return;
    if (ts_open(&src, &io, path, w->arena, sizeof(w->arena), &cfg) != BS_OK)
    {
        io.close(io.ctx);
        return;
    }
    while ((got = ts_read(src, w->chunk, sizeof(w->chunk))) > 0)
    {
        long i;

        yield();                /* a long file is read a chunk at a time */
        for (i = 0; i < got; i++, pos++)
        {
            unsigned char c = w->chunk[i];

            if (c == 0xee && i + 2 < got
                && (unsigned char)w->chunk[i + 1] == 0x80)
            {
                unsigned char m = w->chunk[i + 2];

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
                    section = ni_string(w, head, hlen);
                    in_head = false;
                }
                else if (m == 0x8a && in_name)
                {
                    uint32_t key = art_name_hash_n(name, nlen);

                    if (key && w->count < NI_MENTIONS_MAX)
                    {
                        struct name_mention *nm = &w->m[w->count++];

                        nm->key = key;
                        nm->file = file;
                        nm->section = section;
                        nm->at = name_at;
                        ni_add_name(w, key, name, nlen);
                    }
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

static int ni_compare(const void *a, const void *b)
{
    const struct name_mention *x = a, *y = b;

    if (x->key != y->key)
        return x->key < y->key ? -1 : 1;
    if (x->file != y->file)
        return x->file < y->file ? -1 : 1;
    return x->at < y->at ? -1 : x->at > y->at;
}

static bool ni_write(struct ni_work *w, uint32_t text_fp, uint32_t img_fp)
{
    struct libfile_writer lw;
    struct libfile_marks marks = { (int32_t)text_fp, (int32_t)img_fp, 0 };
    struct ni_tail t = { w->nfiles, w->nstrings, w->nnames, w->used };
    bool ok;

    if (!libfile_begin(&lw, LIB_NAMES_FILE, LIB_NAMES_MAGIC, NI_VERSION,
                       sizeof(struct name_mention), &marks))
        return false;
    ok = libfile_write(&lw, w->m, w->count * sizeof(w->m[0]), w->count)
         && libfile_write(&lw, &t, sizeof(t), 0)
         && libfile_write(&lw, w->files, w->nfiles * sizeof(w->files[0]), 0)
         && libfile_write(&lw, w->str_off, w->nstrings * sizeof(uint32_t), 0)
         && libfile_write(&lw, w->name_key, w->nnames * sizeof(uint32_t), 0)
         && libfile_write(&lw, w->name_str,
                          ALIGN_UP(w->nnames * sizeof(uint16_t), 4), 0)
         && libfile_write(&lw, w->pool, ALIGN_UP(w->used, 4), 0);
    return libfile_finish(&lw, ok);
}

/* The index made again from the text files; false when stopped. One that
 * cannot be written waits for the next request rather than the next tick,
 * which would read every file again to fail again. */
static bool ni_build(struct ni_work *w, uint32_t text_fp, uint32_t img_fp)
{
    int i, n = notes_page_count();

    w->nfiles = w->count = w->nstrings = w->nnames = 0;
    w->used = 0;
    for (i = 0; i < n && w->nfiles < NI_FILES_MAX; i++)
    {
        const struct notes_page *pg = notes_page_get(i);
        struct name_file *fr = &w->files[w->nfiles];

        if (!pg)
            return false;               /* the index is being replaced */
        memset(fr, 0, sizeof(*fr));
        fr->page = i;
        fr->album = NAME_NONE;
        fr->hash = name_index_path_hash(notes_page_path(pg));
        w->nfiles++;
    }

    /* A walk that completes meanwhile renumbers the pages and asks for
     * this again: a page that is no longer the same file is skipped */
    for (i = 0; i < w->nfiles; i++)
    {
        const struct notes_page *pg;

        if (bg_task_should_stop(&name_index_task))
            return false;
        pg = notes_page_get(w->files[i].page);
        if (pg && name_index_path_hash(notes_page_path(pg))
                  == w->files[i].hash)
            ni_read_file(w, i, notes_page_path(pg));
        yield();
    }
    qsort(w->m, w->count, sizeof(w->m[0]), ni_compare);
    if (!ni_write(w, text_fp, img_fp))
        debug_log(DEBUG_LOG_TASKS, "%s: %s not written",
                  name_index_task.name, LIB_NAMES_FILE);
    return true;
}

/* ---- the task ---------------------------------------------------------- */

static bool ni_wanted;
static bool ni_on;              /* the setting, as the task last saw it */

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
    if (have && (uint32_t)h.marks.entries == text_fp)
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

bool name_index_load(void *buf, size_t size, struct name_index *ix)
{
    struct libfile_header h;
    struct ni_tail t;
    uint32_t tail;
    size_t mbytes;
    char *p = buf;
    int fd = libfile_open(LIB_NAMES_FILE, LIB_NAMES_MAGIC, NI_VERSION,
                          sizeof(struct name_mention), &h, &tail);
    int i;
    bool ok;

    memset(ix, 0, sizeof(*ix));
    if (fd < 0)
        return false;
    if (h.count > NI_MENTIONS_MAX)
    {
        close(fd);
        return false;
    }
    mbytes = h.count * sizeof(struct name_mention);
    ok = mbytes + tail <= size && read(fd, p, mbytes + tail)
                                  == (ssize_t)(mbytes + tail);
    close(fd);
    if (!ok || tail < sizeof(t))
        return false;

    /* A damaged file is refused rather than trusted: every count within
     * its cap, every offset within the strings */
    memcpy(&t, p + mbytes, sizeof(t));
    if (t.nfiles > NI_FILES_MAX || t.nstrings > NI_STRINGS_MAX
        || t.nnames > NI_NAMES_MAX || t.pool > NI_POOL)
        return false;
    ix->m = (const struct name_mention *)p;
    ix->count = h.count;
    ix->files = (const struct name_file *)(p + mbytes + sizeof(t));
    ix->str_off = (const uint32_t *)(ix->files + t.nfiles);
    ix->name_key = ix->str_off + t.nstrings;
    ix->name_str = (const uint16_t *)(ix->name_key + t.nnames);
    ix->strings = (const char *)ix->name_str
                  + ALIGN_UP(t.nnames * sizeof(uint16_t), 4);
    if (ix->strings + t.pool > p + mbytes + tail
        || (t.pool && ix->strings[t.pool - 1] != '\0'))
        return false;
    for (i = 0; i < (int)t.nstrings; i++)
        if (ix->str_off[i] >= t.pool)
            return false;
    for (i = 0; i < (int)t.nnames; i++)
        if (ix->name_str[i] >= t.nstrings)
            return false;
    for (i = 0; i < (int)t.nfiles; i++)
        if (ix->files[i].album != NAME_NONE
            && ix->files[i].album >= t.nstrings)
            return false;
    ix->nfiles = t.nfiles;
    ix->nstrings = t.nstrings;
    ix->nnames = t.nnames;
    return true;
}

uint32_t name_index_path_hash(const char *path)
{
    uint32_t h = 0x811C9DC5u;

    while (*path)
        h = ni_mix(h, (unsigned char)*path++);
    return h;
}

const char *name_index_name(const struct name_index *ix, uint32_t key)
{
    int i;

    for (i = 0; i < ix->nnames; i++)
        if (ix->name_key[i] == key)
            return ix->strings + ix->str_off[ix->name_str[i]];
    return NULL;
}

/* The records are sorted by key: read a block at a time, and only as far
 * as the greater of the two keys */
bool name_index_named(uint32_t key, uint32_t alt)
{
    static struct name_mention m[64];
    struct libfile_header h;
    uint32_t i, j, n = 0, last = MAX(key, alt);
    bool found = false, past = false;
    int fd;

    if (!global_settings.liner_notes || !last)
        return false;
    fd = libfile_open(LIB_NAMES_FILE, LIB_NAMES_MAGIC, NI_VERSION,
                      sizeof(m[0]), &h, NULL);
    if (fd < 0)
        return false;
    for (i = 0; i < h.count && !found && !past; i += n)
    {
        ssize_t got = read(fd, m, MIN(h.count - i, (uint32_t)ARRAYLEN(m))
                                  * sizeof(m[0]));

        n = got > 0 ? (uint32_t)got / sizeof(m[0]) : 0;
        if (!n)
            break;
        for (j = 0; j < n && !found && !past; j++)
        {
            found = (key && m[j].key == key) || (alt && m[j].key == alt);
            past = m[j].key > last;
        }
    }
    close(fd);
    return found;
}
