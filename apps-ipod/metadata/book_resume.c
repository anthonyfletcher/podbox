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
 * One line per book, most recently played first. Nothing is held in RAM
 * between calls and every save rewrites the file -- sixty-four books is under
 * twenty-six kilobytes, and a save happens when a book stops being listened
 * to rather than while it plays.
 *
 * A book played through to its end is the one save the UI cannot make: the
 * playlist ends, playback stops, and there is nothing playing left to ask. So
 * the audio thread notes the track that ended in RAM, and the next save writes
 * it down marked as ended.
 *
 * Parts, in order:
 *   - the line format, and reading books out of the file
 *   - the track that ended, noted on the audio thread
 *   - writing: what is worth saving, and the temp-file swap
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "file.h"
#include "rbpaths.h"
#include "string-extra.h"
#include "audio.h"
#include "events.h"
#include "system/appevents.h"
#include "database/db_spoken.h"
#include "metadata/book_resume.h"
#include "playlist/playlist.h"
#include "settings/settings.h"
#include "system/strutil.h"     /* read_line */

#define BOOK_RESUME_FILE  ROCKBOX_DIR "/audiobooks.resume"
#define BOOK_RESUME_TMP   ROCKBOX_DIR "/audiobooks.resume.tmp"

/* A path, a book name, a chapter name and the numbers. */
#define BOOK_LINE_MAX   (MAX_PATH + 160)

/* How many books are remembered. The sixty-fifth to be played drops the one
 * played longest ago. The shelf's In progress row lists only these, so this
 * is also how long that list can get. */
#define BOOK_RESUME_MAX 64

/* A track this close to its end when it finishes was played out rather than
 * stopped. Generous, because the position is sampled rather than exact. */
#define BOOK_END_SLACK_MS 10000

/* ------------------------------------------------------------------ *
 * the line format                                                    *
 * ------------------------------------------------------------------ */

/* "<elapsed>\t<offset>\t<index>\t<book>\t<track>[\tended]". Tabs, because
 * every other field is tag text or a path and FAT forbids a tab in a
 * filename; a tag carrying one is scrubbed on the way in. The last field is
 * the literal word or absent, never a number, so nothing else can read as
 * it. */
#define BOOK_ENDED_FIELD "ended"

/* The book field of 'line', without writing into it -- the rewrite below
 * copies lines it keeps through verbatim. */
static bool line_book(const char *line, const char **book, size_t *len)
{
    const char *p = line;
    const char *end;
    int i;

    for (i = 0; i < 3; i++)
    {
        p = strchr(p, '\t');
        if (p == NULL)
            return false;
        p++;
    }

    end = strchr(p, '\t');
    if (end == NULL)
        return false;

    *book = p;
    *len = end - p;
    return true;
}

/* True when 'line' is the entry for 'book'. */
static bool line_is_book(const char *line, const char *book)
{
    const char *name;
    size_t len;

    if (!line_book(line, &name, &len))
        return false;

    return len == strlen(book) && memcmp(name, book, len) == 0;
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

/* Fill 'pos' from 'line', splitting it in place. The book field is skipped:
 * a caller has either matched it already or read it with line_book(). */
static bool parse_line(char *line, struct book_resume *pos)
{
    char *p = line;
    char *elapsed, *offset, *index, *track, *ended;

    elapsed = next_field(&p);
    offset  = next_field(&p);
    index   = next_field(&p);
    next_field(&p);
    track   = next_field(&p);
    ended   = next_field(&p);

    if (track == NULL || track[0] == '\0')
        return false;

    pos->elapsed = strtoul(elapsed, NULL, 10);
    pos->offset  = strtoul(offset, NULL, 10);
    pos->index   = atoi(index);
    pos->ended   = ended != NULL && strcmp(ended, BOOK_ENDED_FIELD) == 0;
    strmemccpy(pos->track, track, sizeof (pos->track));
    return true;
}

bool book_resume_get(const char *book, struct book_resume *pos)
{
    return book_resume_find(book, pos) && !pos->ended;
}

bool book_resume_find(const char *book, struct book_resume *pos)
{
    char line[BOOK_LINE_MAX];
    int fd;
    bool found = false;

    if (book == NULL || book[0] == '\0')
        return false;

    fd = open(BOOK_RESUME_FILE, O_RDONLY);
    if (fd < 0)
        return false;

    while (read_line(fd, line, sizeof (line)) > 0)
    {
        if (!line_is_book(line, book))
            continue;

        found = parse_line(line, pos);
        break;
    }

    close(fd);
    return found;
}

void book_resume_each(book_resume_fn fn, void *data)
{
    char line[BOOK_LINE_MAX];
    char book[BOOK_KEY_MAX];
    struct book_resume pos;
    int fd;

    fd = open(BOOK_RESUME_FILE, O_RDONLY);
    if (fd < 0)
        return;

    while (read_line(fd, line, sizeof (line)) > 0)
    {
        const char *name;
        size_t len;

        if (!line_book(line, &name, &len))
            continue;

        /* Copied out before parse_line() cuts the line up around it. */
        if (len >= sizeof (book))
            len = sizeof (book) - 1;
        memcpy(book, name, len);
        book[len] = '\0';

        if (!parse_line(line, &pos))
            continue;
        if (!fn(book, &pos, data))
            break;
    }

    close(fd);
}

/* ------------------------------------------------------------------ *
 * the track that ended                                               *
 * ------------------------------------------------------------------ */

/* The key a book is saved under: the album, the same as it is on the shelf.
 * A book held in one file with no album tag has only its path to be known by,
 * and is reached by playing the file rather than by opening a book, so that is
 * the key it is looked up under too. */
static void book_key(const struct mp3entry *id3, char *buf, size_t size)
{
    const char *name = id3->album;

    if (name == NULL || name[0] == '\0')
        name = id3->path;

    strmemccpy(buf, name, size);
}

/* The last book track to play to its end, waiting for a save to write it
 * down. Written on the audio thread and read on the UI thread; the flag is set
 * last and cleared first, and neither thread yields in between. */
static struct
{
    char book[BOOK_KEY_MAX];
    char track[MAX_PATH];
    unsigned long length;
} ended_track;
static volatile bool ended_pending;

/* PLAYBACK_EVENT_TRACK_FINISH, on the audio thread: no file I/O here. */
static void track_finish_event(unsigned short id, void *ev_data)
{
    const struct track_event *te = ev_data;
    const struct mp3entry *id3 = te->id3;
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
    book_key(id3, ended_track.book, sizeof (ended_track.book));
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

/* One field, preceded by its separator, with anything that would read back
 * as a field or line boundary turned into a space. Returns the new end. */
static size_t append_field(char *buf, size_t size, size_t at, const char *s)
{
    if (at + 1 < size)
        buf[at++] = '\t';

    for (; *s != '\0' && at + 1 < size; s++)
        buf[at++] = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;

    buf[at] = '\0';
    return at;
}

/* 'book' first and the other books after it in the order they were last
 * played, which is what makes the cap drop the one heard longest ago. */
static bool rewrite(const char *book, const char *track, int index,
                    unsigned long elapsed, unsigned long offset, bool ended)
{
    char line[BOOK_LINE_MAX];
    size_t at;
    int n, in, out, kept = 1;

    out = open(BOOK_RESUME_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0)
        return false;

    n = snprintf(line, sizeof (line), "%lu\t%lu\t%d", elapsed, offset, index);
    at = (n < 0 || (size_t)n >= sizeof (line)) ? sizeof (line) - 1 : (size_t)n;
    at = append_field(line, sizeof (line), at, book);
    at = append_field(line, sizeof (line), at, track);
    if (ended)
        append_field(line, sizeof (line), at, BOOK_ENDED_FIELD);
    fdprintf(out, "%s\n", line);

    in = open(BOOK_RESUME_FILE, O_RDONLY);
    if (in >= 0)
    {
        while (read_line(in, line, sizeof (line)) > 0)
        {
            const char *name;
            size_t len;

            if (!line_book(line, &name, &len))
                continue;
            if (line_is_book(line, book))   /* the entry just replaced */
                continue;
            if (++kept > BOOK_RESUME_MAX)
                break;

            fdprintf(out, "%s\n", line);
        }

        close(in);
    }

    close(out);

    remove(BOOK_RESUME_FILE);
    return rename(BOOK_RESUME_TMP, BOOK_RESUME_FILE) >= 0;
}

void book_resume_save(void)
{
    struct mp3entry *id3 = NULL;
    char book[BOOK_KEY_MAX];

    if (!global_settings.segregate_audiobooks)
        return;

    if (audio_status())
        id3 = audio_current_track();
    if (id3 != NULL && !db_spoken_is_spoken_genre(id3->genre_string))
        id3 = NULL;

    if (id3 != NULL)
        book_key(id3, book, sizeof (book));

    if (ended_pending)
    {
        /* Copied out before anything yields: rewrite() opens files, and the
         * audio thread may note the next ended track meanwhile. That one
         * sets the flag again for the next save. */
        static char ended_book[BOOK_KEY_MAX];
        static char ended_path[MAX_PATH];
        unsigned long ended_length = ended_track.length;

        strmemccpy(ended_book, ended_track.book, sizeof (ended_book));
        strmemccpy(ended_path, ended_track.track, sizeof (ended_path));
        ended_pending = false;

        /* A chapter that ended into the next one of the same book is not the
         * book ending: the save below puts the book where it is now. */
        if (id3 == NULL || strcmp(book, ended_book) != 0)
            rewrite(ended_book, ended_path, -1, ended_length, 0, true);
    }

    if (id3 != NULL)
        rewrite(book, id3->path, playlist_get_display_index() - 1,
                id3->elapsed, id3->offset, false);
}
