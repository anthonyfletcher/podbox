/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The book index: the title, first author, spine count and cover location
 * of every EPUB and FB2 on the player, for the Books shelf and the art
 * cache.
 *
 * Opening a book to read its OPF is far too slow to do for every row of a
 * shelf, so the documents walk (file_index.c) does it once per book and the
 * answers are kept here. A book whose directory entry has the size and time
 * it had last time is copied across from the old index unopened. One entry
 * per book in a libfile of entries that each say their length; the walk
 * writes a new file beside the old and swaps them at its end.
 *
 * Parts, in order:
 *   - the record, and reading the file
 *   - the walk's side: begin, each book, end
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "core_alloc.h"
#include "string-extra.h"
#include "database/libfile.h"
#include "database/path_key.h"
#include "system/library_files.h"
#include "viewers/text_viewer/txt_source.h"
#include "viewers/text_viewer/ts_io_core.h"
#include "book_index.h"

#define BOOK_INDEX_FILE LIB_BOOK_INDEX_FILE
/* Books remembered from the old index while the walk runs. Past it, a book
 * is opened again whatever its stamp. */
#define BOOK_INDEX_MAX  2048

/* ------------------------------------------------------------------ *
 * the record, and reading the file                                   *
 * ------------------------------------------------------------------ */

/* One book; its path, title and author follow it, unterminated */
struct book_rec
{
    uint64_t key;               /* path_key() of the path */
    uint32_t size, mtime;       /* the directory entry it was read from */
    int32_t  cover_at;
    uint32_t cover_csize, cover_usize;
    uint16_t parts;
    uint8_t  cover_method;
    uint8_t  pad;
    uint16_t path_len, title_len, author_len, pad2;
};

bool book_index_is_book(const char *path)
{
    const char *dot = strrchr(path, '.');

    return dot != NULL && (strcasecmp(dot, ".epub") == 0
                           || strcasecmp(dot, ".fb2") == 0);
}

void book_index_each(book_index_fn fn, void *data)
{
    static char path[MAX_PATH];
    static char title[TS_BOOK_TITLE];
    static char author[TS_BOOK_AUTHOR];
    struct book_entry e = { .path = path, .title = title, .author = author };
    struct libfile_header h;
    struct book_rec r;
    int fd = libfile_open(BOOK_INDEX_FILE, LIB_BOOK_INDEX_MAGIC,
                          LIB_BOOK_INDEX_VERSION, 1, &h, NULL);

    if (fd < 0)
        return;
    while (read(fd, &r, sizeof(r)) == (ssize_t)sizeof(r))
    {
        if (r.path_len >= sizeof(path) || r.title_len >= sizeof(title)
            || r.author_len >= sizeof(author)
            || read(fd, path, r.path_len) != r.path_len
            || read(fd, title, r.title_len) != r.title_len
            || read(fd, author, r.author_len) != r.author_len)
            break;
        path[r.path_len] = '\0';
        title[r.title_len] = '\0';
        author[r.author_len] = '\0';
        e.parts = r.parts;
        e.cover_at = r.cover_at;
        e.cover_csize = r.cover_csize;
        e.cover_usize = r.cover_usize;
        e.cover_method = r.cover_method;
        if (!fn(&e, data))
            break;
    }
    close(fd);
}

/* ------------------------------------------------------------------ *
 * the walk's side                                                    *
 * ------------------------------------------------------------------ */

/* An old entry, found again by its key */
struct book_old
{
    uint64_t key;
    uint32_t size, mtime;
    uint32_t at;                /* where its record starts in the old file */
    uint16_t len;               /* its record's length, strings and all */
    bool copied;
};

/* The walk's working memory: the old entries, then the reader's arena */
struct book_work
{
    struct book_old old[BOOK_INDEX_MAX];
    uint8_t copy[sizeof(struct book_rec) + MAX_PATH + TS_BOOK_TITLE
                 + TS_BOOK_AUTHOR];
    uint8_t arena[TS_ARENA_ZIP];
};

static int work_handle;
static struct book_work *work;
static int old_count;
static int old_fd = -1;
static struct libfile_writer writer;
static bool writing, changed;

/* The old file's entries, by key and place */
static void load_old(void)
{
    struct libfile_header h;
    struct book_rec r;
    off_t at;

    old_count = 0;
    old_fd = libfile_open(BOOK_INDEX_FILE, LIB_BOOK_INDEX_MAGIC,
                          LIB_BOOK_INDEX_VERSION, 1, &h, NULL);
    if (old_fd < 0)
        return;
    at = lseek(old_fd, 0, SEEK_CUR);
    while (old_count < BOOK_INDEX_MAX
           && read(old_fd, &r, sizeof(r)) == (ssize_t)sizeof(r))
    {
        size_t len = sizeof(r) + r.path_len + r.title_len + r.author_len;
        struct book_old *o = &work->old[old_count++];

        if (len > sizeof(work->copy))
        {
            old_count--;
            break;
        }
        o->key = r.key;
        o->size = r.size;
        o->mtime = r.mtime;
        o->at = (uint32_t)at;
        o->len = (uint16_t)len;
        o->copied = false;
        at += len;
        lseek(old_fd, at, SEEK_SET);
    }
}

static bool copy_old(struct book_old *o)
{
    o->copied = true;
    return lseek(old_fd, o->at, SEEK_SET) == (off_t)o->at
           && read(old_fd, work->copy, o->len) == o->len
           && libfile_write(&writer, work->copy, o->len, o->len);
}

void book_index_begin(void)
{
    writing = changed = false;
    work_handle = core_alloc(sizeof(struct book_work));
    if (work_handle <= 0)
    {
        work_handle = 0;
        return;
    }
    core_pin(work_handle);
    work = core_get_data(work_handle);
    load_old();
    writing = libfile_begin(&writer, BOOK_INDEX_FILE, LIB_BOOK_INDEX_MAGIC,
                            LIB_BOOK_INDEX_VERSION, 1, NULL);
}

/* The book at `path`, read afresh */
static bool write_new(const char *path, uint64_t key, unsigned long size,
                      unsigned long mtime)
{
    struct ts_core_file f;
    struct book_rec r;
    ts_book b;
    bs_io io;

    memset(&b, 0, sizeof b);
    b.cover_at = -1;
    if (ts_io_core(&io, &f, path) == BS_OK)
    {
        const char *dot = strrchr(path, '.');
        int rc = strcasecmp(dot, ".fb2") == 0
                 ? ts_fb2_book(&io, &b)
                 : ts_book_read(&io, work->arena, sizeof(work->arena), &b);

        if (rc != BS_OK)
        {
            memset(&b, 0, sizeof b);    /* kept, so it is not read again */
            b.cover_at = -1;
        }
        io.close(io.ctx);
    }

    memset(&r, 0, sizeof r);
    r.key = key;
    r.size = size;
    r.mtime = mtime;
    r.cover_at = b.cover_at;
    r.cover_csize = b.cover_csize;
    r.cover_usize = b.cover_usize;
    r.cover_method = b.cover_method;
    r.parts = b.parts;
    r.path_len = strlen(path);
    r.title_len = strlen(b.title);
    r.author_len = strlen(b.author);
    return libfile_write(&writer, &r, sizeof(r), sizeof(r))
           && libfile_write(&writer, path, r.path_len, r.path_len)
           && libfile_write(&writer, b.title, r.title_len, r.title_len)
           && libfile_write(&writer, b.author, r.author_len, r.author_len);
}

void book_index_found(const char *path, unsigned long size,
                      unsigned long mtime)
{
    uint64_t key;
    int i;

    if (!writing || !book_index_is_book(path))
        return;
    key = path_key(path);
    for (i = 0; i < old_count; i++)
    {
        struct book_old *o = &work->old[i];

        if (o->key != key || o->copied)
            continue;
        if (o->size == size && o->mtime == mtime)
        {
            writing = copy_old(o);
            return;
        }
        o->copied = true;           /* changed: replaced below */
        break;
    }
    changed = true;
    writing = write_new(path, key, size, mtime);
}

bool book_index_end(bool completed)
{
    int i;

    if (work_handle <= 0)
        return false;
    for (i = 0; i < old_count && writing; i++)
    {
        if (work->old[i].copied)
            continue;
        if (completed)
            changed = true;         /* gone from the disk */
        else
            writing = copy_old(&work->old[i]);
    }
    if (old_fd >= 0)
        close(old_fd);
    old_fd = -1;
    libfile_finish(&writer, writing);
    core_unpin(work_handle);
    core_free(work_handle);
    work_handle = 0;
    work = NULL;
    return writing && changed;
}
