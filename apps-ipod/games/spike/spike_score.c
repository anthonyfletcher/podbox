/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * What the best has been.
 *
 * A small text file rather than the tagcache's runtime data, and the reason
 * is not simplicity: a player can be playing a track the database has never
 * seen, and a best that only exists for indexed music is a best that
 * disappears when you play something from a folder.
 *
 * One line each, the run first and the tracks after it in the order they
 * were last beaten. Nothing is held in RAM between calls -- sixty-four
 * paths is seventeen kilobytes, which is most of what the whole game costs
 * -- so every read walks the file and every write rewrites it. Both happen
 * when a run ends, and never while one is running.
 *
 * Parts, in order:
 *   - reading
 *   - writing
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "string-extra.h"   /* strlcpy */
#include "config.h"
#include "file.h"
#include "pathfuncs.h"
#include "system/strutil.h"     /* read_line */
#include "games/spike/spike_score.h"

#define SPK_SCORE_FILE  ROCKBOX_DIR "/spike.scores"
#define SPK_SCORE_TMP   ROCKBOX_DIR "/spike.scores.tmp"

/* One path, a score, a count and the separators. */
#define SPK_LINE_MAX    (MAX_PATH + 40)


/** Reading **/

/* Split "<score> <beats> <rest>". 'rest' points into the line. */
static bool spk_parse(char *line, long *score, long *beats, char **rest)
{
    char *p = line;

    *score = strtol(p, &p, 10);
    if (p == line)
        return false;

    *beats = strtol(p, &p, 10);

    while (*p == ' ')
        p++;

    *rest = p;

    return true;
}

/* Every line in turn, with the leading tag consumed. Returns -1 at the end.
 * The file is small and is read three times at most in a run's life, so a
 * cursor is cheaper than any structure to avoid one. */
static int spk_next(int fd, char *line, int size, long *score, long *beats,
                    char **rest)
{
    while (read_line(fd, line, size) > 0)
    {
        char tag = line[0];

        if ((tag != 'R' && tag != 'T') || line[1] != ' ')
            continue;

        if (spk_parse(line + 2, score, beats, rest))
            return tag;
    }

    return -1;
}

void spk_score_run(long *score, long *beats)
{
    char line[SPK_LINE_MAX];
    long s, b;
    char *rest;
    int fd, tag;

    *score = 0;
    *beats = 0;

    fd = open(SPK_SCORE_FILE, O_RDONLY);
    if (fd < 0)
        return;

    while ((tag = spk_next(fd, line, sizeof (line), &s, &b, &rest)) >= 0)
    {
        if (tag == 'R')
        {
            *score = s;
            *beats = b;
            break;
        }
    }

    close(fd);
}

long spk_score_track(const char *path)
{
    char line[SPK_LINE_MAX];
    long s, b, best = 0;
    char *rest;
    int fd, tag;

    if (path == NULL || path[0] == '\0')
        return 0;

    fd = open(SPK_SCORE_FILE, O_RDONLY);
    if (fd < 0)
        return 0;

    while ((tag = spk_next(fd, line, sizeof (line), &s, &b, &rest)) >= 0)
    {
        if (tag == 'T' && strcmp(rest, path) == 0)
        {
            best = s;
            break;
        }
    }

    close(fd);

    return best;
}

/* The track's own name: a leaderboard is read as a list of songs, and the
 * path is how the entry is keyed rather than what it is called. */
static void spk_short_name(char *dst, int size, const char *path)
{
    const char *base = strrchr(path, '/');
    char *dot;

    base = base != NULL ? base + 1 : path;
    strlcpy(dst, base, size);

    dot = strrchr(dst, '.');
    if (dot != NULL && dot != dst)
        *dot = '\0';
}

/* One screenful, cached, so that walking the list is one file read a page
 * and not one a row. */
static struct
{
    bool valid;
    int  first;                 /* rank of the first row held, from zero */
    int  rows;
    int  total;                 /* track bests in the file */
    long run;
    long run_beats;
    long score[SPK_SCORE_PAGE];
    char name[SPK_SCORE_PAGE][SPK_SCORE_NAME];
} page;

static void spk_score_fill(int first)
{
    char line[SPK_LINE_MAX];
    long s, b;
    char *rest;
    int fd, tag, i = 0;

    page.valid = true;
    page.first = first;
    page.rows = 0;
    page.total = 0;
    page.run = 0;
    page.run_beats = 0;

    fd = open(SPK_SCORE_FILE, O_RDONLY);
    if (fd < 0)
        return;

    while ((tag = spk_next(fd, line, sizeof (line), &s, &b, &rest)) >= 0)
    {
        if (tag == 'R')
        {
            page.run = s;
            page.run_beats = b;
            continue;
        }

        page.total++;

        if (i >= first && page.rows < SPK_SCORE_PAGE)
        {
            page.score[page.rows] = s;
            spk_short_name(page.name[page.rows], SPK_SCORE_NAME, rest);
            page.rows++;
        }

        i++;
    }

    close(fd);
}

int spk_score_rows(void)
{
    spk_score_fill(0);

    /* The run's own line, and then the tracks. It is not one of them: it is
     * a whole playlist, and ranking it among single songs would say they
     * were the same kind of thing. */
    return 1 + page.total;
}

void spk_score_row(int n, char *buf, int size)
{
    int track = n - 1;

    if (n == 0)
    {
        if (!page.valid)
            spk_score_fill(0);

        if (page.run > 0)
            snprintf(buf, size, "Run  %ld  (%ld beats)", page.run,
                     page.run_beats);
        else
            snprintf(buf, size, "Run  --");

        return;
    }

    if (!page.valid || track < page.first
        || track >= page.first + page.rows)
        spk_score_fill(track - track % SPK_SCORE_PAGE);

    if (track < page.first || track >= page.first + page.rows)
    {
        buf[0] = 0;
        return;
    }

    snprintf(buf, size, "%d.  %ld  %s", track + 1,
             page.score[track - page.first], page.name[track - page.first]);
}


/** Writing **/

/* The run line first and the track lines after it in score order, so the
 * file is the table a screen reads straight out: nothing has to sort it,
 * and the cap drops the lowest rather than the oldest.
 *
 * Two passes over the input, because the run line is written before any
 * track line and may be anywhere in the old file. Neither holds more than
 * one line at a time. */
static bool spk_rewrite(char tag, const char *key, long score, long beats)
{
    char line[SPK_LINE_MAX];
    long s, b;
    char *rest;
    int in, out, old, kept = 0;
    bool placed = false;

    out = open(SPK_SCORE_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0)
        return false;

    if (tag == 'R')
        fdprintf(out, "R %ld %ld\n", score, beats);
    else
    {
        in = open(SPK_SCORE_FILE, O_RDONLY);
        if (in >= 0)
        {
            while ((old = spk_next(in, line, sizeof (line), &s, &b,
                                   &rest)) >= 0)
            {
                if (old == 'R')
                {
                    fdprintf(out, "R %ld %ld\n", s, b);
                    break;
                }
            }

            close(in);
        }
    }

    in = open(SPK_SCORE_FILE, O_RDONLY);
    if (in >= 0)
    {
        while ((old = spk_next(in, line, sizeof (line), &s, &b, &rest)) >= 0)
        {
            if (old != 'T')
                continue;

            /* The entry being replaced goes; its new value is placed by
               score like any other. */
            if (tag == 'T' && strcmp(rest, key) == 0)
                continue;

            if (tag == 'T' && !placed && score > s)
            {
                placed = true;
                if (++kept <= SPK_SCORE_TRACKS)
                    fdprintf(out, "T %ld 0 %s\n", score, key);
            }

            if (++kept <= SPK_SCORE_TRACKS)
                fdprintf(out, "T %ld %ld %s\n", s, b, rest);
        }

        close(in);
    }

    /* Lower than everything already there, or the first of its kind. */
    if (tag == 'T' && !placed && ++kept <= SPK_SCORE_TRACKS)
        fdprintf(out, "T %ld 0 %s\n", score, key);

    close(out);

    remove(SPK_SCORE_FILE);
    page.valid = false;

    return rename(SPK_SCORE_TMP, SPK_SCORE_FILE) >= 0;
}

bool spk_score_put_run(long score, long beats)
{
    long best, was_beats;

    spk_score_run(&best, &was_beats);

    if (score <= best)
        return false;

    spk_rewrite('R', "", score, beats);

    return true;
}

bool spk_score_put_track(const char *path, long score)
{
    if (path == NULL || path[0] == '\0' || score <= spk_score_track(path))
        return false;

    spk_rewrite('T', path, score, 0);

    return true;
}
