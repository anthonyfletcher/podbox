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
 * between calls and every save rewrites the file -- sixteen books is under
 * six kilobytes, and a save happens when a book stops being listened to
 * rather than while it plays.
 *
 * Parts, in order:
 *   - the line format, and reading one book out of the file
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
#include "database/db_spoken.h"
#include "metadata/book_resume.h"
#include "playlist/playlist.h"
#include "settings/settings.h"
#include "system/strutil.h"     /* read_line */

#define BOOK_RESUME_FILE  ROCKBOX_DIR "/audiobooks.resume"
#define BOOK_RESUME_TMP   ROCKBOX_DIR "/audiobooks.resume.tmp"

/* A path, a book name, a chapter name and the numbers. */
#define BOOK_LINE_MAX   (MAX_PATH + 160)

/* How many books are remembered. The seventeenth to be played drops the one
 * played longest ago. */
#define BOOK_RESUME_MAX 16

/* ------------------------------------------------------------------ *
 * the line format                                                    *
 * ------------------------------------------------------------------ */

/* "<elapsed>\t<offset>\t<index>\t<book>\t<track>\t<chapter>". Tabs, because
 * every other field is tag text or a path and FAT forbids a tab in a
 * filename; a tag carrying one is scrubbed on the way in. */

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

bool book_resume_get(const char *book, struct book_resume *pos)
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
        char *p = line;
        char *elapsed, *offset, *index, *track;

        if (!line_is_book(line, book))
            continue;

        elapsed = next_field(&p);
        offset  = next_field(&p);
        index   = next_field(&p);
        next_field(&p);                 /* the book, already matched */
        track   = next_field(&p);

        if (track == NULL || track[0] == '\0')
            break;

        pos->elapsed = strtoul(elapsed, NULL, 10);
        pos->offset  = strtoul(offset, NULL, 10);
        pos->index   = atoi(index);
        strmemccpy(pos->track, track, sizeof (pos->track));

        found = true;
        break;
    }

    close(fd);
    return found;
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
static bool rewrite(const char *book, const char *track,
                    int index, unsigned long elapsed, unsigned long offset)
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
    append_field(line, sizeof (line), at, track);
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
    struct mp3entry *id3;
    char book[BOOK_KEY_MAX];
    const char *name;

    if (!global_settings.segregate_audiobooks)
        return;

    if (!(audio_status() && (id3 = audio_current_track()) != NULL))
        return;

    if (!db_spoken_is_spoken_genre(id3->genre_string))
        return;

    /* The album is the book, the same as it is on the shelf. A book held in
     * one file with no album tag has only its path to be known by, and is
     * reached by playing the file rather than by opening a book, so that is
     * the key it is looked up under too. */
    name = id3->album;
    if (name == NULL || name[0] == '\0')
        name = id3->path;

    strmemccpy(book, name, sizeof (book));

    rewrite(book, id3->path, playlist_get_display_index() - 1,
            id3->elapsed, id3->offset);
}
