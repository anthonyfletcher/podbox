/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Bookmarks inside audiobooks: places in a book to come back to, beside the
 * one place book_resume.c keeps for where it was left. A mark names its book
 * by book_resume_key() and its chapter by the file's path, which is what
 * playing it again needs.
 *
 * One libfile of marks that each say their length, newest first. Marks are
 * made by hand, so every change simply rewrites the file.
 *
 * Parts, in order:
 *   - the record, and reading marks out of the file
 *   - rewriting the file
 *   - adding, listing and deleting
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "kernel.h"
#include "file.h"
#include "string-extra.h"
#include "metadata.h"
#include "database/libfile.h"
#include "metadata/book_marks.h"
#include "metadata/book_resume.h"
#include "system/format_time.h"
#include "system/library_files.h"

#define MARKS_FILE  LIB_BOOKMARKS_FILE

/* The UI adds and deletes; the background task carries old bookmarks over */
static struct mutex marks_mutex;

void book_marks_init(void)
{
    mutex_init(&marks_mutex);
}

/* ------------------------------------------------------------------ *
 * the record                                                         *
 * ------------------------------------------------------------------ */

/* One mark; its chapter's path, then its label, follow it */
struct mark_rec
{
    uint64_t book;
    uint32_t elapsed;
    uint32_t offset;
    uint16_t path_len;
    uint16_t label_len;
};

typedef bool (*scan_fn)(uint64_t book, const struct book_mark *m, void *data);

/* Every mark in the file, newest first, until 'fn' returns false. False when
 * the file is there and could not be read, so a rewrite does not lose it. */
static bool scan(scan_fn fn, void *data)
{
    static char path[MAX_PATH];
    static char label[BOOK_MARK_LABEL_MAX];
    struct libfile_header h;
    struct mark_rec r;
    struct book_mark m = { .path = path, .label = label };
    int fd;

    fd = libfile_open(MARKS_FILE, LIB_MARKS_MAGIC, LIB_MARKS_VERSION, 1, &h,
                      NULL);
    if (fd < 0)
    {
        /* A damaged file is set aside, or nothing could be added until it
         * was deleted by hand */
        if (fd == LIBFILE_BAD)
            rename(MARKS_FILE, MARKS_FILE ".bad");
        return !file_exists(MARKS_FILE);
    }

    while (read(fd, &r, sizeof(r)) == (ssize_t)sizeof(r))
    {
        if (r.path_len >= sizeof(path) || r.label_len >= sizeof(label)
            || read(fd, path, r.path_len) != r.path_len
            || read(fd, label, r.label_len) != r.label_len)
            break;
        path[r.path_len] = '\0';
        label[r.label_len] = '\0';
        m.elapsed = r.elapsed;
        m.offset = r.offset;
        if (!fn(r.book, &m, data))
            break;
    }

    close(fd);
    return true;
}

static bool write_mark(struct libfile_writer *w, uint64_t book,
                       const struct book_mark *m)
{
    struct mark_rec r;

    memset(&r, 0, sizeof(r));
    r.book = book;
    r.elapsed = m->elapsed;
    r.offset = m->offset;
    r.path_len = strlen(m->path);
    r.label_len = strlen(m->label);
    return libfile_write(w, &r, sizeof(r), sizeof(r))
           && libfile_write(w, m->path, r.path_len, r.path_len)
           && libfile_write(w, m->label, r.label_len, r.label_len);
}

/* ------------------------------------------------------------------ *
 * rewriting the file                                                 *
 * ------------------------------------------------------------------ */

struct copy_ctx
{
    struct libfile_writer *w;
    uint64_t book;              /* the mark to leave out, or the one added */
    const char *path;
    unsigned long elapsed;
    int kept;
    bool dropped;
    bool ok;
};

static bool same_mark(const struct copy_ctx *ctx, uint64_t book,
                      const struct book_mark *m)
{
    return book == ctx->book && m->elapsed / 1000 == ctx->elapsed / 1000
           && strcmp(m->path, ctx->path) == 0;
}

static bool copy_one(uint64_t book, const struct book_mark *m, void *data)
{
    struct copy_ctx *ctx = data;

    if (same_mark(ctx, book, m))
    {
        ctx->dropped = true;
        return true;
    }
    if (++ctx->kept > BOOK_MARKS_MAX)
    {
        ctx->ok = false;
        return false;
    }
    ctx->ok = write_mark(ctx->w, book, m);
    return ctx->ok;
}

/* The file again, with 'first' at the top if given, and without any mark of
 * ctx's book at ctx's place. A failure anywhere leaves the file as it was. */
static bool rewrite(struct copy_ctx *ctx, const struct book_mark *first)
{
    struct libfile_writer w;

    if (!libfile_begin(&w, MARKS_FILE, LIB_MARKS_MAGIC, LIB_MARKS_VERSION, 1,
                       NULL))
        return false;

    ctx->w = &w;
    ctx->ok = true;
    if (first != NULL)
    {
        ctx->kept = 1;
        ctx->ok = write_mark(&w, ctx->book, first);
    }
    ctx->ok = ctx->ok && scan(copy_one, ctx) && ctx->ok;
    return libfile_finish(&w, ctx->ok);
}

static void lock(void)
{
    mutex_lock(&marks_mutex);
}

/* ------------------------------------------------------------------ *
 * adding, listing and deleting                                       *
 * ------------------------------------------------------------------ */

bool book_marks_add(uint64_t book, const char *path, unsigned long elapsed,
                    unsigned long offset, const char *title)
{
    char label[BOOK_MARK_LABEL_MAX];
    char time[16];
    struct book_mark m = { path, label, elapsed, offset };
    struct copy_ctx ctx = { .book = book, .path = path, .elapsed = elapsed };
    const char *base;
    bool ok;

    /* A chapter with no title is called by its file's name */
    if (title == NULL || title[0] == '\0')
    {
        base = strrchr(path, '/');
        title = base ? base + 1 : path;
    }
    format_time(time, sizeof(time), elapsed);
    snprintf(label, sizeof(label), "%s \xc2\xb7 %s", title, time);

    lock();
    /* The same place marked twice is one mark, moved to the top */
    ok = rewrite(&ctx, &m);
    mutex_unlock(&marks_mutex);
    return ok;
}

bool book_marks_add_playing(void)
{
    char id[BOOK_ID_MAX];
    char path[MAX_PATH];
    char title[BOOK_MARK_LABEL_MAX];
    unsigned long elapsed, offset;
    struct mp3entry *id3 = book_resume_loaded(id, sizeof(id));

    if (id3 == NULL)
        return false;
    strmemccpy(path, id3->path, sizeof(path));
    strmemccpy(title, id3->title ? id3->title : "", sizeof(title));
    elapsed = id3->elapsed;
    offset = id3->offset;

    return book_marks_add(book_resume_key(id), path, elapsed, offset, title);
}

struct each_ctx
{
    uint64_t book;
    book_marks_fn fn;
    void *data;
    bool any;
};

static bool each_one(uint64_t book, const struct book_mark *m, void *data)
{
    struct each_ctx *ctx = data;

    if (book != ctx->book)
        return true;
    ctx->any = true;
    return ctx->fn == NULL ? false : ctx->fn(m, ctx->data);
}

void book_marks_each(const char *book, book_marks_fn fn, void *data)
{
    struct each_ctx ctx = { book_resume_key(book), fn, data, false };

    lock();
    scan(each_one, &ctx);
    mutex_unlock(&marks_mutex);
}

bool book_marks_any(const char *book)
{
    struct each_ctx ctx = { book_resume_key(book), NULL, NULL, false };

    lock();
    scan(each_one, &ctx);
    mutex_unlock(&marks_mutex);
    return ctx.any;
}

bool book_marks_delete(const char *book, const char *path,
                       unsigned long elapsed)
{
    struct copy_ctx ctx = { .book = book_resume_key(book), .path = path,
                            .elapsed = elapsed };
    bool ok;

    lock();
    ok = rewrite(&ctx, NULL) && ctx.dropped;
    mutex_unlock(&marks_mutex);
    return ok;
}
