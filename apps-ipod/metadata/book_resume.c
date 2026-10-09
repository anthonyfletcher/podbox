/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Where each audiobook was left.
 *
 * A book played from the Audiobooks shelf is a dynamic playlist built out of
 * the database, so it has neither a directory nor a playlist file -- and a
 * .bmark is keyed by one or the other, which is why bookmark.c turns a
 * playlist like this one away (bookmark_is_bookmarkable_state()). The key
 * here is the book itself: its album and album artist, which is how the
 * database tells albums apart, or the file's path for a single-file book with
 * no album tag to name it. A position saved under the album alone, as an
 * older firmware keyed them, is rekeyed once the database is in RAM.
 *
 * One entry per book, most recently played first, in a libfile of entries
 * that each say their length. Both the book and its track are 64-bit keys
 * (database/path_key.h), so an entry is 32 bytes; only a book keyed by its
 * path carries the path as well, since playing it needs the file and not just
 * its key. Nothing is held in RAM between calls and every save rewrites the
 * file, which happens when a book stops being listened to rather than while it
 * plays.
 *
 * The two text formats an older firmware wrote are read only to convert them,
 * at the first boot of a new layout (book_resume_convert()).
 *
 * A book played through to its end is the one save the UI cannot make: the
 * playlist ends, playback stops, and there is nothing playing left to ask. So
 * the audio thread notes the track that ended in RAM, and the next save writes
 * it down marked as ended.
 *
 * Parts, in order:
 *   - the entry, and reading books out of the file
 *   - the older text formats, and converting them
 *   - positions keyed by the album alone, rekeyed
 *   - the track that ended, noted on the audio thread
 *   - writing: what is worth saving, marks, and the swap
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system/library_files.h"
#include "database/libfile.h"
#include "system.h"             /* ARRAYLEN */
#include "file.h"
#include "rbpaths.h"
#include "string-extra.h"
#include "audio.h"
#include "events.h"
#include "system/appevents.h"
#include "database/db_spoken.h"
#include "database/path_key.h"
#include "database/tagcache.h"
#include "metadata/book_resume.h"
#include "playlist/playlist.h"
#include "settings/settings.h"
#include "system/strutil.h"     /* read_line */

#define BOOK_RESUME_FILE  LIB_AUDIOBOOKS_FILE

/* The longest line either text format wrote: the earlier one's path, book
 * name and numbers. */
#define BOOK_LINE_MAX   (MAX_PATH + 160)

/* A track this close to its end when it finishes was played out rather than
 * stopped. Generous, because the position is sampled rather than exact. */
#define BOOK_END_SLACK_MS 10000

/* See book_resume_writes(). Moved by every change to the file. */
static unsigned writes;

unsigned book_resume_writes(void)
{
    return writes;
}

/* ------------------------------------------------------------------ *
 * the entry                                                          *
 * ------------------------------------------------------------------ */

/* One book in the file; the path, path_len bytes, follows it */
struct book_rec
{
    uint64_t book;
    uint64_t track;
    uint32_t elapsed;
    uint32_t offset;
    int32_t  index;
    uint8_t  left;
    uint8_t  named;             /* keyed by the album alone */
    uint16_t path_len;
};

/* The second text format opens with this line. Lines are
 * "<book>\t<track>\t<elapsed>\t<offset>\t<index>\t<left>[\t<path>]": the two
 * keys as sixteen hex digits, 'left' one of the words below, and 'path' only
 * for a book keyed by its file. Tabs, because FAT forbids one in a filename. */
#define BOOK_HEADER "# book positions 2"

static const char *const left_words[] = {
    [BOOK_LEFT_PARTWAY]   = "partway",
    [BOOK_LEFT_ENDED]     = "ended",
    [BOOK_LEFT_FINISHED]  = "finished",
    [BOOK_LEFT_UNSTARTED] = "unstarted",
};

/* The earlier format, which has no header:
 * "<elapsed>\t<offset>\t<index>\t<book>\t<track>[\tended]", the book spelled
 * out and the track a full path. */
#define OLD_ENDED_FIELD "ended"

/* One book, from either form. 'path' points into what it was read from. */
struct entry
{
    uint64_t book;
    const char *path;           /* the file, for a book keyed by it */
    bool named;                 /* 'book' is the album's key alone */
    struct book_resume pos;
};

uint64_t book_resume_key(const char *book)
{
    char name[BOOK_ID_MAX];
    uint64_t h;

    if (book[0] == '/')
        return path_key(book);

    strmemccpy(name, book, sizeof (name));
    h = path_key_fold_hash(name);
    return h ? h : 1;
}

void book_resume_id(char *buf, size_t size, const char *album,
                    const char *author)
{
    snprintf(buf, size, "%.*s\t%.*s", BOOK_KEY_MAX - 1, album,
             BOOK_KEY_MAX - 1, author && author[0] ? author : UNTAGGED);
}

bool book_resume_id_of(long album_seek, long artist_seek, char *buf,
                       size_t size)
{
    char album[BOOK_KEY_MAX];
    char author[BOOK_KEY_MAX];
    struct tagcache_album al;
    int n;

    /* The book of that name if there is one, else whoever's album it is */
    for (n = tagcache_album_find_name(album_seek);
         artist_seek < 0 && n >= 0 && tagcache_album_get(n, &al)
         && al.album_seek == album_seek; n++)
    {
        if (al.spoken == al.tracks)
            artist_seek = al.artist_seek;
    }
    if (artist_seek < 0 && n > 0 && tagcache_album_get(n - 1, &al)
        && al.album_seek == album_seek)
        artist_seek = al.artist_seek;

    if (artist_seek < 0
        || !tagcache_seek_string(tag_album, album_seek, album, sizeof(album)))
        return false;
    if (!tagcache_seek_string(tag_albumartist, artist_seek, author,
                              sizeof(author)))
        author[0] = '\0';
    book_resume_id(buf, size, album, author);
    return true;
}

/* The next tab-separated field, consuming it. Splits 'line' in place. */
static char *next_field(char **p)
{
    char *field = *p;
    char *tab;

    if (field == NULL)
        return NULL;

    tab = strchr(field, '\t');
    if (tab != NULL)
    {
        *tab = '\0';
        *p = tab + 1;
    }
    else
        *p = NULL;

    return field;
}

static bool parse_key(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    int i;

    for (i = 0; i < 16; i++)
    {
        char c = s[i];
        int d;

        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else
            return false;
        v = (v << 4) | d;
    }

    *out = v;
    return s[16] == '\0';
}

static bool parse_current(char *line, struct entry *e)
{
    char *p = line;
    char *book, *track, *elapsed, *offset, *index, *left, *path;
    unsigned i;

    book    = next_field(&p);
    track   = next_field(&p);
    elapsed = next_field(&p);
    offset  = next_field(&p);
    index   = next_field(&p);
    left    = next_field(&p);
    path    = next_field(&p);

    if (left == NULL || !parse_key(book, &e->book) || e->book == 0
        || !parse_key(track, &e->pos.track))
        return false;

    for (i = 0; i < ARRAYLEN(left_words); i++)
    {
        if (strcmp(left, left_words[i]) == 0)
            break;
    }
    if (i == ARRAYLEN(left_words))
        return false;

    e->pos.left    = (enum book_left)i;
    e->pos.elapsed = strtoul(elapsed, NULL, 10);
    e->pos.offset  = strtoul(offset, NULL, 10);
    e->pos.index   = atoi(index);
    e->path = path != NULL && path[0] == '/' ? path : NULL;
    e->named = e->path == NULL;
    return true;
}

static bool parse_old(char *line, struct entry *e)
{
    char *p = line;
    char *elapsed, *offset, *index, *book, *track, *ended;

    elapsed = next_field(&p);
    offset  = next_field(&p);
    index   = next_field(&p);
    book    = next_field(&p);
    track   = next_field(&p);
    ended   = next_field(&p);

    if (track == NULL || track[0] == '\0' || book[0] == '\0')
        return false;

    /* A book keyed by its path wrote that path cut short; its track is the
     * same file in full. */
    if (book[0] == '/')
        book = track;
    e->book = book_resume_key(book);
    e->path = book[0] == '/' ? book : NULL;
    e->named = e->path == NULL;
    e->pos.track   = path_key(track);
    e->pos.elapsed = strtoul(elapsed, NULL, 10);
    e->pos.offset  = strtoul(offset, NULL, 10);
    e->pos.index   = atoi(index);
    e->pos.left    = ended != NULL && strcmp(ended, OLD_ENDED_FIELD) == 0
                     ? BOOK_LEFT_ENDED : BOOK_LEFT_PARTWAY;
    return true;
}

/* Every entry in the file, in order, until 'fn' returns false. False when
 * the file is there and could not be read. */
static bool scan(bool (*fn)(const struct entry *e, void *data), void *data)
{
    static char path[MAX_PATH];
    struct libfile_header h;
    struct book_rec r;
    int fd;
    bool old = false;

    fd = libfile_open(BOOK_RESUME_FILE, LIB_BOOKS_MAGIC, LIB_BOOKS_VERSION, 1,
                      &h, NULL);
    if (fd == LIBFILE_BAD)
    {
        int fd1 = libfile_open(BOOK_RESUME_FILE, LIB_BOOKS_MAGIC, 1, 1, &h,
                               NULL);
        if (fd1 >= 0)
        {
            fd = fd1;
            old = true;
        }
    }
    if (fd < 0)
    {
        /* A damaged file is set aside, or every save, mark and rekey that
         * copies it would fail until it was deleted by hand. One that could
         * not be read, or is from a later version, is kept and refused:
         * those do not mean the positions in it are lost. */
        if (fd == LIBFILE_BAD
            && rename(BOOK_RESUME_FILE, BOOK_RESUME_FILE ".bad") == 0)
            writes++;
        return !file_exists(BOOK_RESUME_FILE);
    }

    while (read(fd, &r, sizeof(r)) == (ssize_t)sizeof(r))
    {
        struct entry e;

        if (r.path_len >= sizeof(path) || r.left >= ARRAYLEN(left_words)
            || read(fd, path, r.path_len) != r.path_len)
            break;
        path[r.path_len] = '\0';
        e.book = r.book;
        e.path = r.path_len ? path : NULL;
        e.named = e.path == NULL && (old || r.named);
        e.pos.track = r.track;
        e.pos.elapsed = r.elapsed;
        e.pos.offset = r.offset;
        e.pos.index = r.index;
        e.pos.left = (enum book_left)r.left;
        if (!fn(&e, data))
            break;
    }

    close(fd);
    return true;
}

/* ------------------------------------------------------------------ *
 * the older text formats                                             *
 * ------------------------------------------------------------------ */

static bool scan_text(const char *file,
                      bool (*fn)(const struct entry *e, void *data),
                      void *data)
{
    char line[BOOK_LINE_MAX];
    bool current = false;
    int fd;

    fd = open(file, O_RDONLY);
    if (fd < 0)
        return false;

    while (read_line(fd, line, sizeof (line)) > 0)
    {
        struct entry e;

        if (line[0] == '#')
        {
            current = current || strcmp(line, BOOK_HEADER) == 0;
            continue;
        }

        if (!(current ? parse_current(line, &e) : parse_old(line, &e)))
            continue;
        if (!fn(&e, data))
            break;
    }

    close(fd);
    return true;
}

struct find_ctx
{
    uint64_t book;
    struct book_resume *pos;
    bool found;
};

static bool find_one(const struct entry *e, void *data)
{
    struct find_ctx *ctx = data;

    if (e->book != ctx->book)
        return true;

    *ctx->pos = e->pos;
    ctx->found = true;
    return false;
}

bool book_resume_get(const char *book, struct book_resume *pos)
{
    return book_resume_find(book, pos)
        && pos->left == BOOK_LEFT_PARTWAY && pos->track != 0;
}

static void rekey(void);

bool book_resume_find(const char *book, struct book_resume *pos)
{
    struct find_ctx ctx = { .pos = pos, .found = false };

    if (book == NULL || book[0] == '\0')
        return false;

    rekey();
    ctx.book = book_resume_key(book);
    scan(find_one, &ctx);
    return ctx.found;
}

struct each_ctx
{
    book_resume_fn fn;
    void *data;
};

static bool each_one(const struct entry *e, void *data)
{
    struct each_ctx *ctx = data;

    return ctx->fn(e->book, e->path, &e->pos, ctx->data);
}

void book_resume_each(book_resume_fn fn, void *data)
{
    struct each_ctx ctx = { .fn = fn, .data = data };

    rekey();
    scan(each_one, &ctx);
}

/* ------------------------------------------------------------------ *
 * positions keyed by the album alone                                 *
 * ------------------------------------------------------------------ */

static bool rekey_checked;

/* The key of album-table row 'n' as a book, if it is a whole book whose
 * album has the key 'named'; else 0. */
static uint64_t book_of_row(int n, uint64_t named)
{
    struct tagcache_album al;
    char album[BOOK_KEY_MAX];
    char id[BOOK_ID_MAX];

    if (tagcache_album_get(n, &al)
        && al.spoken == al.tracks
        && tagcache_seek_string(tag_album, al.album_seek, album,
                                sizeof(album))
        && book_resume_key(album) == named
        && book_resume_id_of(al.album_seek, al.artist_seek, id, sizeof(id)))
        return book_resume_key(id);
    return 0;
}

/* The key of the book whose album had the key 'named', or 0. Two authors'
 * books can share a title, so the album holding the saved track decides; the
 * first of that name stands in when the track does not say. */
static uint64_t book_of_album(uint64_t named, uint64_t track)
{
    uint64_t book;
    int idx = track != 0 ? tagcache_find_key(track) : -1;

    if (idx >= 0 && (book = book_of_row(tagcache_album_of(idx), named)) != 0)
        return book;
    for (int n = 0; n < tagcache_album_count(); n++)
        if ((book = book_of_row(n, named)) != 0)
            return book;
    return 0;
}

static bool write_entry(struct libfile_writer *w, const struct entry *e);

/* Whether 'e' is a position keyed by its album alone whose book can now be
 * found. One whose book cannot be found stays as it is, so a file holding
 * only those is not rewritten at every boot to change nothing. */
static bool find_convertible(const struct entry *e, void *data)
{
    if (e->named && book_of_album(e->book, e->pos.track) != 0)
    {
        *(bool *)data = true;
        return false;
    }
    return true;
}

struct rekey_ctx
{
    struct libfile_writer *out;
    bool ok;
};

/* A position whose book is not found keeps its key and stays marked, so a
 * later boot tries again; it matches nothing meanwhile, and falls off the end
 * of the file in time. */
static bool rekey_one(const struct entry *e, void *data)
{
    struct rekey_ctx *ctx = data;
    struct entry n = *e;

    if (n.named)
    {
        uint64_t book = book_of_album(n.book, n.pos.track);

        if (book != 0)
        {
            n.book = book;
            n.named = false;
        }
    }
    ctx->ok = write_entry(ctx->out, &n);
    return ctx->ok;
}

/* Once per boot, as soon as the database is in RAM: the album tables are
 * what say which author an album's book is by. */
static void rekey(void)
{
    struct libfile_writer w;
    struct rekey_ctx ctx = { .out = &w, .ok = true };
    bool any = false;

    if (rekey_checked || tagcache_album_count() == 0)
        return;
    rekey_checked = true;

    scan(find_convertible, &any);
    if (!any || !libfile_begin(&w, BOOK_RESUME_FILE, LIB_BOOKS_MAGIC,
                               LIB_BOOKS_VERSION, 1, NULL))
        return;
    writes++;
    ctx.ok = scan(rekey_one, &ctx) && ctx.ok;
    libfile_finish(&w, ctx.ok);
}

/* ------------------------------------------------------------------ *
 * the track that ended                                               *
 * ------------------------------------------------------------------ */

/* The id a book is saved under: its album and author, built in 'buf'. A book
 * held in one file with no album tag has only its path to be known by, and is
 * reached by playing the file rather than by opening a book, so that is the
 * key it is looked up under too. */
static const char *book_name(const struct mp3entry *id3, char *buf,
                             size_t size)
{
    if (id3->album == NULL || id3->album[0] == '\0')
        return id3->path;
    book_resume_id(buf, size, id3->album,
                   id3->albumartist && id3->albumartist[0] ? id3->albumartist
                                                           : id3->artist);
    return buf;
}

/* The last book track to play to its end, waiting for a save to write it
 * down. Written on the audio thread and read on the UI thread; the flag is set
 * last and cleared first, and neither thread yields in between. */
static struct
{
    uint64_t book;
    bool by_path;               /* the book is keyed by 'track' */
    char track[MAX_PATH];
    unsigned long length;
} ended_track;
static volatile bool ended_pending;

/* PLAYBACK_EVENT_TRACK_FINISH, on the audio thread: no file I/O here. */
static void track_finish_event(unsigned short id, void *ev_data)
{
    static char id_buf[BOOK_ID_MAX];
    const struct track_event *te = ev_data;
    const struct mp3entry *id3 = te->id3;
    const char *name;
    (void)id;

    if (!global_settings.segregate_audiobooks)
        return;
    if (!(te->flags & TEF_AUTO_SKIP) || id3->length == 0)
        return;
    /* The flag outlives the transition it was set for, so a stop partway
     * through the next track can still carry it. The position cannot. */
    if (id3->elapsed + BOOK_END_SLACK_MS < id3->length)
        return;
    if (!db_spoken_is_spoken_genre(id3->genre_string))
        return;

    ended_pending = false;
    name = book_name(id3, id_buf, sizeof(id_buf));
    ended_track.book = book_resume_key(name);
    ended_track.by_path = name == id3->path;
    strmemccpy(ended_track.track, id3->path, sizeof (ended_track.track));
    ended_track.length = id3->length;
    ended_pending = true;
}

void book_resume_init(void)
{
    add_event(PLAYBACK_EVENT_TRACK_FINISH, track_finish_event);
}

/* ------------------------------------------------------------------ *
 * writing                                                            *
 * ------------------------------------------------------------------ */

static bool write_entry(struct libfile_writer *w, const struct entry *e)
{
    struct book_rec r;

    memset(&r, 0, sizeof(r));
    r.book = e->book;
    r.track = e->pos.track;
    r.elapsed = e->pos.elapsed;
    r.offset = e->pos.offset;
    r.index = e->pos.index;
    r.left = e->pos.left;
    r.named = e->named;
    r.path_len = e->path != NULL ? strlen(e->path) : 0;
    return libfile_write(w, &r, sizeof(r), sizeof(r))
           && libfile_write(w, e->path, r.path_len, r.path_len);
}

struct copy_ctx
{
    struct libfile_writer *out;
    uint64_t skip;              /* the entry just replaced */
    int kept;
    bool ok;
};

static bool copy_one(const struct entry *e, void *data)
{
    struct copy_ctx *ctx = data;

    if (e->book == ctx->skip)
        return true;
    if (++ctx->kept > BOOK_RESUME_MAX)
        return false;

    ctx->ok = write_entry(ctx->out, e);
    return ctx->ok;
}

/* 'first' at the top and the other books after it in the order they were
 * last played, which is what makes the cap drop the one heard longest ago.
 * A failed write anywhere leaves the file as it was. */
static bool rewrite(const struct entry *first)
{
    struct libfile_writer w;
    struct copy_ctx ctx = { .out = &w, .skip = first->book, .kept = 1,
                            .ok = true };

    if (!libfile_begin(&w, BOOK_RESUME_FILE, LIB_BOOKS_MAGIC,
                       LIB_BOOKS_VERSION, 1, NULL))
        return false;
    writes++;

    ctx.ok = write_entry(&w, first) && scan(copy_one, &ctx) && ctx.ok;
    return libfile_finish(&w, ctx.ok);
}

static bool convert_one(const struct entry *e, void *data)
{
    struct copy_ctx *ctx = data;

    ctx->ok = write_entry(ctx->out, e);
    return ctx->ok;
}

bool book_resume_convert(const char *text_file)
{
    struct libfile_writer w;
    struct copy_ctx ctx = { .out = &w, .ok = true };

    if (!libfile_begin(&w, BOOK_RESUME_FILE, LIB_BOOKS_MAGIC,
                       LIB_BOOKS_VERSION, 1, NULL))
        return false;
    writes++;
    ctx.ok = scan_text(text_file, convert_one, &ctx) && ctx.ok;
    return libfile_finish(&w, ctx.ok);
}

/* The book loaded for playback, playing or paused, or NULL. */
static struct mp3entry *loaded_book(const char **name, uint64_t *book)
{
    static char id_buf[BOOK_ID_MAX];
    struct mp3entry *id3 = audio_status() ? audio_current_track() : NULL;

    if (id3 == NULL || !db_spoken_is_spoken_genre(id3->genre_string))
        return NULL;

    *name = book_name(id3, id_buf, sizeof(id_buf));
    *book = book_resume_key(*name);
    return id3;
}

/* The loaded book cannot be marked from under itself: the next save would
 * write its position straight over the mark. In progress takes that position
 * as it is; the other two stop the book, the way finishing it would. */
bool book_resume_mark(const char *book, enum book_left left)
{
    struct entry e = {
        .path = book[0] == '/' ? book : NULL,
        .pos = { .track = 0, .index = -1, .left = left },
    };
    const char *name;
    uint64_t loaded;

    rekey();
    e.book = book_resume_key(book);
    if (global_settings.segregate_audiobooks
        && loaded_book(&name, &loaded) != NULL && loaded == e.book)
    {
        if (left == BOOK_LEFT_PARTWAY)
        {
            book_resume_save();
            return true;
        }
        audio_stop();
    }
    return rewrite(&e);
}

void book_resume_save(void)
{
    struct mp3entry *id3;
    const char *name = NULL;
    uint64_t book = 0;

    if (!global_settings.segregate_audiobooks)
        return;

    rekey();
    id3 = loaded_book(&name, &book);

    if (ended_pending)
    {
        /* Copied out before anything yields: rewrite() opens files, and the
         * audio thread may note the next ended track meanwhile. That one
         * sets the flag again for the next save. */
        static char ended_path[MAX_PATH];
        struct entry e = {
            .book = ended_track.book,
            .path = ended_track.by_path ? ended_path : NULL,
            .pos = {
                .index   = -1,
                .elapsed = ended_track.length,
                .left    = BOOK_LEFT_ENDED,
            },
        };

        strmemccpy(ended_path, ended_track.track, sizeof (ended_path));
        ended_pending = false;
        e.pos.track = path_key(ended_path);

        /* A chapter that ended into the next one of the same book is not the
         * book ending: the save below puts the book where it is now. */
        if (id3 == NULL || book != e.book)
            rewrite(&e);
    }

    if (id3 != NULL)
    {
        struct entry e = {
            .book = book,
            .path = name == id3->path ? id3->path : NULL,
            .pos = {
                .track   = path_key(id3->path),
                .index   = playlist_get_display_index() - 1,
                .elapsed = id3->elapsed,
                .offset  = id3->offset,
                .left    = BOOK_LEFT_PARTWAY,
            },
        };

        rewrite(&e);
    }
}
