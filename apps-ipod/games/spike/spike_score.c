/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * What the best run has been, and what it was played over.
 *
 * Files of their own rather than the tagcache's runtime data, and the reason
 * is not simplicity: a player can be playing a track the database has never
 * seen, and a record that only exists for indexed music is one that
 * disappears when you play something out of a folder.
 *
 * Two libfiles of the same records, one a track. spike_run.dat is the run in
 * progress, a record appended as each track starts; spike_scores.dat is the
 * record, the same tracks with the run's numbers after them. A run that beats
 * the record is copied over it, and that copy is the only time either file is
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
 *   - converting the text files an older firmware kept
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "string-extra.h"   /* strlcpy */
#include "system/library_files.h"
#include "database/libfile.h"
#include "config.h"
#include "file.h"
#include "system/strutil.h"     /* read_line */
#include "games/spike/spike_score.h"

#define SPK_SCORE_FILE  LIB_SPIKE_FILE
#define SPK_RUN_FILE    LIB_SPIKE_RUN_FILE

/* One track of a run */
struct spk_rec
{
    char name[SPK_NAME_MAX];
    char genre[SPK_GENRE_MAX];
};

/* The record's numbers, after its tracks */
struct spk_best
{
    int32_t score;
    int32_t beats;
    int32_t secs;
    int32_t tracks;
    int32_t bpm10;
};

/* One screenful and a little, so an ordinary scroll never turns a page. */
#define SPK_PAGE        16

static int logged;              /* tracks this run has written */


/** The log **/

/* The file a list is in, opened at its first track; -1 if there is none */
static int spk_open(enum spk_log which, struct libfile_header *h,
                    uint32_t *tail)
{
    if (which == SPK_LOG_BEST)
        return libfile_open(SPK_SCORE_FILE, LIB_SPIKE_MAGIC,
                            LIB_SPIKE_VERSION, sizeof(struct spk_rec), h,
                            tail);
    return libfile_open(SPK_RUN_FILE, LIB_SPIKE_RUN_MAGIC, LIB_SPIKE_VERSION,
                        sizeof(struct spk_rec), h, tail);
}

/* A field on its way into the file. Control characters go, and a name is
 * truncated here rather than by the screen, since a name nobody can read the
 * end of costs nothing to shorten. */
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
    struct spk_rec r;

    if (logged >= SPK_LOG_MAX)
        return;

    memset(&r, 0, sizeof(r));
    spk_clean(r.name, sizeof (r.name), name);
    spk_clean(r.genre, sizeof (r.genre), genre);

    if (r.name[0] == '\0')
        return;

    if (libfile_append(SPK_RUN_FILE, LIB_SPIKE_RUN_MAGIC, LIB_SPIKE_VERSION,
                       sizeof(r), &r, 1))
        logged++;
}


/** Reading **/

/* The numbers, which only the record carries, after its tracks */
bool spk_score_best(struct spk_run *out)
{
    struct libfile_header h;
    struct spk_best b;
    uint32_t tail;
    bool found;
    int fd;

    memset(out, 0, sizeof(*out));

    fd = spk_open(SPK_LOG_BEST, &h, &tail);
    if (fd < 0)
        return false;
    found = tail == sizeof(b)
            && lseek(fd, h.count * sizeof(struct spk_rec), SEEK_CUR) >= 0
            && read(fd, &b, sizeof(b)) == (ssize_t)sizeof(b);
    close(fd);

    if (found)
    {
        out->score = b.score;
        out->beats = b.beats;
        out->secs = b.secs;
        out->tracks = b.tracks;
        out->bpm10 = b.bpm10;
    }
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
    struct spk_rec rec[SPK_PAGE];
} page;

static void spk_score_fill(enum spk_log which, int first)
{
    struct libfile_header h;
    int fd, n;

    page.valid = true;
    page.which = which;
    page.first = first;
    page.rows = 0;
    page.total = 0;

    fd = spk_open(which, &h, NULL);
    if (fd < 0)
        return;

    page.total = h.count;
    n = page.total - first < SPK_PAGE ? page.total - first : SPK_PAGE;
    if (n > 0 && lseek(fd, first * sizeof(struct spk_rec), SEEK_CUR) >= 0
        && read(fd, page.rec, n * sizeof(struct spk_rec))
           == (ssize_t)(n * sizeof(struct spk_rec)))
        page.rows = n;

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

    strlcpy(name, page.rec[n - page.first].name, nsize);
    strlcpy(genre, page.rec[n - page.first].genre, gsize);
}


/** Writing **/

/* The record is the run's own log with the numbers after it, so beating it
 * is a copy and nothing more: the numbers were gathered as the run went and
 * the tracks were written as it went. */
static bool write_record(const struct spk_run *r, int run_fd,
                         uint32_t run_count)
{
    struct libfile_writer w;
    struct spk_rec rec;
    struct spk_best b = {
        .score = r->score, .beats = r->beats, .secs = r->secs,
        .tracks = r->tracks, .bpm10 = r->bpm10,
    };
    bool ok;

    if (!libfile_begin(&w, SPK_SCORE_FILE, LIB_SPIKE_MAGIC, LIB_SPIKE_VERSION,
                       sizeof(rec), NULL))
        return false;
    ok = true;
    for (uint32_t i = 0; ok && run_fd >= 0 && i < run_count; i++)
        ok = read(run_fd, &rec, sizeof(rec)) == (ssize_t)sizeof(rec)
             && libfile_write(&w, &rec, sizeof(rec), 1);
    ok = ok && libfile_write(&w, &b, sizeof(b), 0);
    return libfile_finish(&w, ok);
}

bool spk_score_end(const struct spk_run *r)
{
    struct libfile_header h;
    struct spk_run best;
    bool ok;
    int in;

    page.valid = false;

    if (spk_score_best(&best) && r->score <= best.score)
        return false;

    if (r->score <= 0)
        return false;

    in = spk_open(SPK_LOG_RUN, &h, NULL);
    ok = write_record(r, in, in >= 0 ? h.count : 0);
    if (in >= 0)
        close(in);
    return ok;
}


/** Converting the text files an older firmware kept **/

/* "T <name>\t<genre>" track lines; the record's under an "R1 " line of its
 * numbers */
static bool convert_text(const char *text, const char *dat, uint32_t magic,
                         struct spk_run *best)
{
    struct libfile_writer w;
    struct spk_rec rec;
    char line[SPK_NAME_MAX + SPK_GENRE_MAX + 8];
    bool ok;
    int fd = open(text, O_RDONLY);

    if (fd < 0)
        return false;
    if (!libfile_begin(&w, dat, magic, LIB_SPIKE_VERSION, sizeof(rec), NULL))
    {
        close(fd);
        return false;
    }
    ok = true;
    while (ok && read_line(fd, line, sizeof(line)) > 0)
    {
        if (best && !strncmp(line, "R1 ", 3))
        {
            char *p = line + 3;

            best->score = strtol(p, &p, 10);
            best->beats = strtol(p, &p, 10);
            best->secs = strtol(p, &p, 10);
            best->tracks = (int)strtol(p, &p, 10);
            best->bpm10 = (int)strtol(p, &p, 10);
            continue;
        }
        if (line[0] != 'T' || line[1] != ' ')
            continue;

        char *tab = strchr(line + 2, '\t');
        if (tab != NULL)
            *tab = '\0';
        memset(&rec, 0, sizeof(rec));
        strlcpy(rec.name, line + 2, sizeof(rec.name));
        strlcpy(rec.genre, tab != NULL ? tab + 1 : "", sizeof(rec.genre));
        ok = libfile_write(&w, &rec, sizeof(rec), 1);
    }
    close(fd);

    if (ok && best)
    {
        struct spk_best b = {
            .score = best->score, .beats = best->beats, .secs = best->secs,
            .tracks = best->tracks, .bpm10 = best->bpm10,
        };
        ok = libfile_write(&w, &b, sizeof(b), 0);
    }
    return libfile_finish(&w, ok);
}

bool spk_score_convert(const char *scores_text, const char *run_text)
{
    struct spk_run best;
    bool ok = true;

    memset(&best, 0, sizeof(best));
    if (scores_text)
        ok = convert_text(scores_text, SPK_SCORE_FILE, LIB_SPIKE_MAGIC,
                          &best);
    if (run_text)
        ok &= convert_text(run_text, SPK_RUN_FILE, LIB_SPIKE_RUN_MAGIC, NULL);
    return ok;
}
