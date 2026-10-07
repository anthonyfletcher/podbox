/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Where each audiobook was left.
 *
 * A book played from the Audiobooks shelf is a dynamic playlist built out of
 * the database, so it has neither a directory nor a playlist file -- and a
 * .bmark is keyed by one or the other, which is why bookmark.c turns a
 * playlist like this one away (bookmark_is_bookmarkable_state()). The key
 * here is the book itself: its album tag, which is what the shelf browses
 * books by, or the file's path for a single-file book with no album tag to
 * name it.
 *
 * One line per book, most recently played first. Both the book and its track
 * are written as 64-bit keys (database/path_key.h), so a line is about sixty
 * bytes; only a book keyed by its path carries the path as well, since playing
 * it needs the file and not just its key. Nothing is held in RAM between calls
 * and every save rewrites the file, which happens when a book stops being
 * listened to rather than while it plays.
 *
 * A file with no header line is the earlier format, which spelled the book
 * and track out in full. It is read as it is, and the first save writes the
 * whole file in the current one.
 *
 * A book played through to its end is the one save the UI cannot make: the
 * playlist ends, playback stops, and there is nothing playing left to ask. So
 * the audio thread notes the track that ended in RAM, and the next save writes
 * it down marked as ended.
 *
 * Parts, in order:
 *   - the line formats, and reading books out of the file
 *   - the track that ended, noted on the audio thread
 *   - writing: what is worth saving, marks, and the temp-file swap
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system/library_files.h"
#include "system.h"             /* ARRAYLEN */
#include "file.h"
#include "rbpaths.h"
#include "string-extra.h"
#include "audio.h"
#include "events.h"
#include "system/appevents.h"
#include "database/db_spoken.h"
#include "database/path_key.h"
#include "metadata/book_resume.h"
#include "playlist/playlist.h"
#include "settings/settings.h"
#include "system/strutil.h"     /* read_line */

#define BOOK_RESUME_FILE  LIB_AUDIOBOOKS_FILE
#define BOOK_RESUME_TMP   LIB_AUDIOBOOKS_FILE ".new"

/* The longest line either format writes: the earlier one's path, book name
 * and numbers. */
#define BOOK_LINE_MAX   (MAX_PATH + 160)

/* A track this close to its end when it finishes was played out rather than
 * stopped. Generous, because the position is sampled rather than exact. */
#define BOOK_END_SLACK_MS 10000

/* ------------------------------------------------------------------ *
 * the line formats                                                   *
 * ------------------------------------------------------------------ */

/* The current format opens with this line. Lines are
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

/* One line of either format. 'path' points into the line it was read from. */
struct entry
{
    uint64_t book;
    const char *path;           /* the file, for a book keyed by it */
    struct book_resume pos;
};

uint64_t book_resume_key(const char *book)
{
    char name[BOOK_KEY_MAX];
    uint64_t h;

    if (book[0] == '/')
        return path_key(book);

    strmemccpy(name, book, sizeof (name));
    h = path_key_fold_hash(name);
    return h ? h : 1;
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
    e->pos.track   = path_key(track);
    e->pos.elapsed = strtoul(elapsed, NULL, 10);
    e->pos.offset  = strtoul(offset, NULL, 10);
    e->pos.index   = atoi(index);
    e->pos.left    = ended != NULL && strcmp(ended, OLD_ENDED_FIELD) == 0
                     ? BOOK_LEFT_ENDED : BOOK_LEFT_PARTWAY;
    return true;
}

/* Every entry in the file, in order, until 'fn' returns false. False when
 * the file is there and could not be opened. */
static bool scan(bool (*fn)(const struct entry *e, void *data), void *data)
{
    char line[BOOK_LINE_MAX];
    bool current = false;
    int fd;

    fd = open(BOOK_RESUME_FILE, O_RDONLY);
    if (fd < 0)
        return !file_exists(BOOK_RESUME_FILE);

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

bool book_resume_find(const char *book, struct book_resume *pos)
{
    struct find_ctx ctx = { .pos = pos, .found = false };

    if (book == NULL || book[0] == '\0')
        return false;

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

    scan(each_one, &ctx);
}

/* ------------------------------------------------------------------ *
 * the track that ended                                               *
 * ------------------------------------------------------------------ */

/* The name a book is saved under: the album, the same as it is on the shelf.
 * A book held in one file with no album tag has only its path to be known by,
 * and is reached by playing the file rather than by opening a book, so that is
 * the key it is looked up under too. */
static const char *book_name(const struct mp3entry *id3)
{
    if (id3->album == NULL || id3->album[0] == '\0')
        return id3->path;
    return id3->album;
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
    name = book_name(id3);
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

static bool write_entry(int fd, const struct entry *e)
{
    char line[BOOK_LINE_MAX];
    int len;

    len = snprintf(line, sizeof (line),
                   "%016llx\t%016llx\t%lu\t%lu\t%d\t%s%s%s\n",
                   (unsigned long long)e->book,
                   (unsigned long long)e->pos.track,
                   e->pos.elapsed, e->pos.offset, e->pos.index,
                   left_words[e->pos.left],
                   e->path != NULL ? "\t" : "",
                   e->path != NULL ? e->path : "");
    if (len >= (int)sizeof (line))
        len = sizeof (line) - 1;
    return write(fd, line, len) == len;
}

struct copy_ctx
{
    int out;
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
 * Every line goes out in the current format, whichever it was read in. A
 * failed write anywhere leaves the file as it was. */
static bool rewrite(const struct entry *first)
{
    struct copy_ctx ctx = { .skip = first->book, .kept = 1, .ok = true };
    int hlen = sizeof (BOOK_HEADER "\n") - 1;

    ctx.out = open(BOOK_RESUME_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (ctx.out < 0)
        return false;

    ctx.ok = write(ctx.out, BOOK_HEADER "\n", hlen) == hlen
             && write_entry(ctx.out, first)
             && scan(copy_one, &ctx) && ctx.ok;

    if (close(ctx.out) < 0)
        ctx.ok = false;
    if (!ctx.ok)
    {
        remove(BOOK_RESUME_TMP);
        return false;
    }

    remove(BOOK_RESUME_FILE);
    return rename(BOOK_RESUME_TMP, BOOK_RESUME_FILE) >= 0;
}

/* The book loaded for playback, playing or paused, or NULL. */
static struct mp3entry *loaded_book(const char **name, uint64_t *book)
{
    struct mp3entry *id3 = audio_status() ? audio_current_track() : NULL;

    if (id3 == NULL || !db_spoken_is_spoken_genre(id3->genre_string))
        return NULL;

    *name = book_name(id3);
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
