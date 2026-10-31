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
 * Rockbox's own bookmarks are carried over once, the first time the store is
 * used with the database in RAM and no file of its own yet: the database is
 * what says which book a bookmarked track belongs to.
 *
 * Parts, in order:
 *   - the record, and reading marks out of the file
 *   - rewriting the file
 *   - carrying over Rockbox's bookmarks
 *   - adding, listing and deleting
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "rbpaths.h"
#include "string-extra.h"
#include "metadata.h"
#include "database/libfile.h"
#include "database/path_key.h"
#include "database/tagcache.h"
#include "metadata/book_marks.h"
#include "metadata/book_resume.h"
#include "settings/settings.h"
#include "system/format_time.h"
#include "system/library_files.h"
#include "system/strutil.h"     /* read_line */

#define MARKS_FILE  LIB_BOOKMARKS_FILE

/* Held around each read or rewrite of the file. Every caller is on the UI
 * thread, the carry-over of Rockbox's bookmarks included: that runs outside
 * it, taking it once per mark it adds. */
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
 * carrying over Rockbox's bookmarks                                  *
 * ------------------------------------------------------------------ */

/* Rockbox kept a bookmark file beside each folder or playlist it bookmarked,
 * named after it with ".bmark" added, and the newest few in one more. A line
 * is ">flags;index;offset;seed;elapsed;repeat;shuffle;", then the pitch and
 * the speed where the flags say so, then the playlist and the track. A line
 * with no '>' is the older form, with one more number after the seed. The
 * track is a name in the playlist's folder when the playlist is a folder,
 * ending in '/', and a full path otherwise. */
#define BMARK_RECENT    ROCKBOX_DIR "/most-recent.bmark"
#define BMARK_ROOT      "/root_dir.bmark"
#define BMARK_PITCH     0x01
#define BMARK_SPEED     0x02
#define BMARK_NUMBERS   7

/* The files found, read once the database search that finds them is over */
#define BMARK_FILES_MAX 32
static char bmark_files[BMARK_FILES_MAX][MAX_PATH];
static int bmark_file_ct;

static struct tagcache_search_clause spoken_clause = {
    .tag = tag_virt_spoken,
    .type = clause_gt,
    .numeric = true,
    .source = source_constant,
    .numeric_data = 0,
};

static void note_bmark(const char *path)
{
    if (bmark_file_ct < BMARK_FILES_MAX && file_exists(path))
        strmemccpy(bmark_files[bmark_file_ct++], path, MAX_PATH);
}

static void find_bmarks(void)
{
    struct tagcache_search tcs;
    char path[MAX_PATH];
    char last[MAX_PATH] = "";
    char file[MAX_PATH];
    const char *dirname = global_settings.playlist_catalog_dir[0]
                          ? (const char *)global_settings.playlist_catalog_dir
                          : PLAYLIST_CATALOG_DEFAULT_DIR;
    struct dirent *de;
    DIR *dir;

    bmark_file_ct = 0;
    note_bmark(BMARK_RECENT);
    note_bmark(BMARK_ROOT);

    /* Beside each book's folder: one check a folder, no walk of the disk */
    if (tagcache_search(&tcs, tag_filename))
    {
        tagcache_search_add_clause(&tcs, &spoken_clause);
        while (tagcache_get_next(&tcs, path, sizeof(path)))
        {
            char *slash = strrchr(path, '/');

            if (slash == NULL || slash == path)
                continue;
            *slash = '\0';
            if (strcmp(path, last) == 0)
                continue;
            strmemccpy(last, path, sizeof(last));
            snprintf(file, sizeof(file), "%s.bmark", path);
            note_bmark(file);
        }
        tagcache_search_finish(&tcs);
    }

    /* Beside the saved playlists */
    dir = opendir(dirname);
    if (dir == NULL)
        return;
    while ((de = readdir(dir)) != NULL)
    {
        size_t len = strlen(de->d_name);

        if (len > 6 && strcasecmp(de->d_name + len - 6, ".bmark") == 0)
        {
            snprintf(file, sizeof(file), "%s/%s", dirname, de->d_name);
            note_bmark(file);
        }
    }
    closedir(dir);
}

/* The next ';'-separated field, consuming it; NULL when there is none */
static char *bmark_field(char **p)
{
    char *field = *p;
    char *sep;

    if (field == NULL)
        return NULL;
    sep = strchr(field, ';');
    if (sep != NULL)
    {
        *sep = '\0';
        *p = sep + 1;
    }
    else
        *p = NULL;
    return field;
}

/* The book the track at 'path' is in, and the track's title; 0 if it is in
 * none. A book's tracks are all spoken word, which an album of music with
 * one spoken track is not. */
static uint64_t book_of_track(const char *path, char *title, size_t size)
{
    struct tagcache_search tcs;
    struct tagcache_album al;
    char id[BOOK_ID_MAX];
    int idx = tagcache_find_path(path);

    if (idx < 0 || !tagcache_album_get(tagcache_album_of(idx), &al)
        || al.spoken != al.tracks
        || !book_resume_id_of(al.album_seek, al.artist_seek, id, sizeof(id)))
        return 0;

    title[0] = '\0';
    if (tagcache_search(&tcs, tag_title))
    {
        tagcache_retrieve(&tcs, idx, tag_title, title, size);
        tagcache_search_finish(&tcs);
    }
    if (strcmp(title, UNTAGGED) == 0)
        title[0] = '\0';

    /* A track with no album is a book of its own, keyed by its path */
    return strncmp(id, UNTAGGED "\t", sizeof(UNTAGGED)) == 0
           ? path_key(path) : book_resume_key(id);
}

/* The bookmarks in one .bmark file that are in a book, as marks. How many. */
static int import_file(const char *file)
{
    static char line[2 * MAX_PATH];
    char track[MAX_PATH];
    char title[BOOK_MARK_LABEL_MAX];
    int fd = open(file, O_RDONLY);
    int added = 0;

    if (fd < 0)
        return 0;

    while (read_line(fd, line, sizeof(line)) > 0)
    {
        bool newer = line[0] == '>';
        char *p = line + newer;
        char *num[BMARK_NUMBERS + 2];
        char *playlist;
        int want = BMARK_NUMBERS;
        int i;
        uint64_t book;

        for (i = 0; i < want; i++)
        {
            num[i] = bmark_field(&p);
            if (num[i] == NULL)
                break;
            if (i == 0 && newer)
            {
                int flags = atoi(num[0]);

                want += !!(flags & BMARK_PITCH) + !!(flags & BMARK_SPEED);
            }
        }
        playlist = bmark_field(&p);
        if (i < want || playlist == NULL || p == NULL || *p == '\0')
            continue;

        if (playlist[0] != '\0' && playlist[strlen(playlist) - 1] == '/')
            snprintf(track, sizeof(track), "%s%s", playlist, p);
        else
            strmemccpy(track, p, sizeof(track));

        book = book_of_track(track, title, sizeof(title));
        if (book != 0
            && book_marks_add(book, track, strtoul(num[4], NULL, 10),
                              strtoul(num[newer ? 2 : 1], NULL, 10), title))
            added++;
    }

    close(fd);
    return added;
}

/* Once: Rockbox's bookmarks in books become marks. The file is written even
 * with none, so that its being there says this has been done. A mark made
 * before the database is first in RAM writes the file too, and the carry-over
 * is then skipped; that is the first moments of the first boot only. */
static void import_once(void)
{
    static bool checked;
    struct copy_ctx none = { .path = "" };
    int added = 0;

    if (checked || tagcache_album_count() == 0)
        return;
    checked = true;
    if (file_exists(MARKS_FILE))
        return;

    find_bmarks();
    for (int i = 0; i < bmark_file_ct; i++)
    {
        int n = import_file(bmark_files[i]);

        library_files_log("bookmarks: %d from %s", n, bmark_files[i]);
        added += n;
    }
    if (!file_exists(MARKS_FILE))
    {
        lock();
        rewrite(&none, NULL);
        mutex_unlock(&marks_mutex);
    }
    if (bmark_file_ct > 0)
        library_files_log("bookmarks: %d carried over from %d files", added,
                          bmark_file_ct);
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
    struct mp3entry *id3;

    /* The carry-over first: it yields, and the track is copied out of the
     * live id3 before anything else does */
    import_once();
    id3 = book_resume_loaded(id, sizeof(id));
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

    import_once();
    lock();
    scan(each_one, &ctx);
    mutex_unlock(&marks_mutex);
}

bool book_marks_any(const char *book)
{
    struct each_ctx ctx = { book_resume_key(book), NULL, NULL, false };

    import_once();
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

    import_once();
    lock();
    ok = rewrite(&ctx, NULL) && ctx.dropped;
    mutex_unlock(&marks_mutex);
    return ok;
}
