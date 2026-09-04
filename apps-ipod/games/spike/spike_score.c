/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * What the best run has been, and what it was played over.
 *
 * Small text files rather than the tagcache's runtime data, and the reason
 * is not simplicity: a player can be playing a track the database has never
 * seen, and a record that only exists for indexed music is one that
 * disappears when you play something out of a folder.
 *
 * Two files and one format. `spike.run` is the run in progress -- one line
 * a track, appended as it starts -- and `spike.scores` is the record: the
 * same lines under a header carrying the numbers. A run that beats the
 * record is copied over it, and that copy is the only time either file is
 * rewritten.
 *
 * Nothing is held in RAM between calls but one cached page of the list.
 * Track names are the size of a run and a run is an evening; the numbers
 * are what the game carries, and they are five of them.
 *
 * Parts, in order:
 *   - the log
 *   - reading
 *   - writing
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "string-extra.h"   /* strlcpy */
#include "config.h"
#include "file.h"
#include "system/strutil.h"     /* read_line */
#include "games/spike/spike_score.h"

#define SPK_SCORE_FILE  ROCKBOX_DIR "/spike.scores"
#define SPK_SCORE_TMP   ROCKBOX_DIR "/spike.scores.tmp"
#define SPK_RUN_FILE    ROCKBOX_DIR "/spike.run"

/* A name, a genre, the tab between them and the tag. */
#define SPK_LINE_MAX    (SPK_NAME_MAX + SPK_GENRE_MAX + 8)

/* The record's header, and the whole of the file's version check. A header
 * the reader does not recognise is a file with nothing in it, and the first
 * run to finish writes it out again.
 *
 * Trap: it earns its place because a foreign file here is not gibberish but
 * plausible -- a per-track score table writes lines that read as tracks with
 * the score still on the front of the name. Bump the letter with the layout
 * of a line. */
#define SPK_HEAD        "R1"
#define SPK_HEAD_LEN    2

/* One screenful and a little, so an ordinary scroll never turns a page. */
#define SPK_PAGE        16

static int logged;              /* lines this run has written */


/** The log **/

static const char *spk_file(enum spk_log which)
{
    return which == SPK_LOG_BEST ? SPK_SCORE_FILE : SPK_RUN_FILE;
}

/* A field on its way into the file. Tabs separate the two fields and
 * newlines separate the lines, so neither may survive in either -- and a
 * name is truncated here rather than by the screen, since a line nobody can
 * read the end of costs nothing to shorten. */
static void spk_clean(char *dst, int size, const char *src)
{
    int i;

    if (src == NULL)
        src = "";

    strlcpy(dst, src, size);

    for (i = 0; dst[i]; i++)
    {
        if (dst[i] == '\t' || dst[i] == '\n' || dst[i] == '\r')
            dst[i] = ' ';
    }
}

void spk_score_begin(void)
{
    remove(SPK_RUN_FILE);
    logged = 0;
}

/* Trap: this is a disk write on the game's own thread, and a track change
 * is not a promise that the disk is awake -- a playlist buffered ahead can
 * leave it asleep for minutes. A spin-up blocks the frame it lands on. The
 * clock survives it (a forward jump is measured against wall time, so a long
 * block does not read as a seek) and it happens once a track, which is the
 * price of not holding an evening of track names in RAM. */
void spk_score_played(const char *name, const char *genre)
{
    char n[SPK_NAME_MAX], g[SPK_GENRE_MAX];
    int fd;

    if (logged >= SPK_LOG_MAX)
        return;

    spk_clean(n, sizeof (n), name);
    spk_clean(g, sizeof (g), genre);

    if (n[0] == '\0')
        return;

    fd = open(SPK_RUN_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;

    fdprintf(fd, "T %s\t%s\n", n, g);
    close(fd);

    logged++;
}


/** Reading **/

/* The header, which only the record carries. */
bool spk_score_best(struct spk_run *out)
{
    char line[SPK_LINE_MAX];
    int fd;
    bool found = false;

    out->score = 0;
    out->beats = 0;
    out->secs = 0;
    out->tracks = 0;
    out->bpm10 = 0;

    fd = open(SPK_SCORE_FILE, O_RDONLY);
    if (fd < 0)
        return false;

    while (read_line(fd, line, sizeof (line)) > 0)
    {
        char *p = line;

        if (strncmp(line, SPK_HEAD " ", SPK_HEAD_LEN + 1) != 0)
            continue;

        p += SPK_HEAD_LEN + 1;
        out->score = strtol(p, &p, 10);
        out->beats = strtol(p, &p, 10);
        out->secs = strtol(p, &p, 10);
        out->tracks = (int)strtol(p, &p, 10);
        out->bpm10 = (int)strtol(p, &p, 10);
        found = true;
        break;
    }

    close(fd);

    return found;
}

/* One page of the list, so that scrolling costs one file read a screenful.
 * Keyed by the file it came from as well as by the row, because the results
 * screen and the record's screen are the same screen over two files. */
static struct
{
    bool valid;
    enum spk_log which;
    int  first;                 /* row the page begins at, from zero */
    int  rows;
    int  total;
    char name[SPK_PAGE][SPK_NAME_MAX];
    char genre[SPK_PAGE][SPK_GENRE_MAX];
} page;

static void spk_score_fill(enum spk_log which, int first)
{
    char line[SPK_LINE_MAX];
    int fd, i = 0;

    /* The log is track lines and nothing else; the record's are under a
     * header, and one it does not recognise means a file written by a
     * version that kept something else. */
    bool ready = which == SPK_LOG_RUN;

    page.valid = true;
    page.which = which;
    page.first = first;
    page.rows = 0;
    page.total = 0;

    fd = open(spk_file(which), O_RDONLY);
    if (fd < 0)
        return;

    while (read_line(fd, line, sizeof (line)) > 0)
    {
        char *tab;

        if (!ready)
        {
            ready = strncmp(line, SPK_HEAD " ", SPK_HEAD_LEN + 1) == 0;
            continue;
        }

        if (line[0] != 'T' || line[1] != ' ')
            continue;

        page.total++;

        if (i >= first && page.rows < SPK_PAGE)
        {
            tab = strchr(line + 2, '\t');
            if (tab != NULL)
                *tab = '\0';

            strlcpy(page.name[page.rows], line + 2, SPK_NAME_MAX);
            strlcpy(page.genre[page.rows], tab != NULL ? tab + 1 : "",
                    SPK_GENRE_MAX);
            page.rows++;
        }

        i++;
    }

    close(fd);
}

/* Every fill counts the whole file whichever page it kept, so the total is
 * good for as long as the page is and asking for it never turns one. */
int spk_score_tracks(enum spk_log which)
{
    if (!page.valid || page.which != which)
        spk_score_fill(which, 0);

    return page.total;
}

void spk_score_track(enum spk_log which, int n, char *name, int nsize,
                     char *genre, int gsize)
{
    if (!page.valid || page.which != which || n < page.first
        || n >= page.first + page.rows)
        spk_score_fill(which, n - n % SPK_PAGE);

    if (n < page.first || n >= page.first + page.rows)
    {
        name[0] = '\0';
        genre[0] = '\0';
        return;
    }

    strlcpy(name, page.name[n - page.first], nsize);
    strlcpy(genre, page.genre[n - page.first], gsize);
}


/** Writing **/

/* The record is the run's own log under a header, so beating it is a copy
 * and nothing more: the numbers were gathered as the run went and the lines
 * were written as it went. */
bool spk_score_end(const struct spk_run *r)
{
    char line[SPK_LINE_MAX];
    struct spk_run best;
    int in, out;

    page.valid = false;

    if (spk_score_best(&best) && r->score <= best.score)
        return false;

    if (r->score <= 0)
        return false;

    out = open(SPK_SCORE_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0)
        return false;

    fdprintf(out, SPK_HEAD " %ld %ld %ld %d %d\n", r->score, r->beats,
             r->secs, r->tracks, r->bpm10);

    in = open(SPK_RUN_FILE, O_RDONLY);
    if (in >= 0)
    {
        while (read_line(in, line, sizeof (line)) > 0)
        {
            if (line[0] == 'T' && line[1] == ' ')
                fdprintf(out, "%s\n", line);
        }

        close(in);
    }

    close(out);

    remove(SPK_SCORE_FILE);

    return rename(SPK_SCORE_TMP, SPK_SCORE_FILE) >= 0;
}
