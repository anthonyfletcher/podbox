/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Reading the chapter marks an Ogg Vorbis or Opus book carries in its
 * comments.
 *
 * The marks are pairs of Vorbis comments, CHAPTER001=00:01:02.500 and
 * CHAPTER001NAME=The name, numbered from 000 or 001. The comment header is
 * an Ogg packet that may run over several pages, so it is read through the
 * same page reader album art uses. Each mark is kept in the cuesheet's entry
 * of its number until all are read, since a time and its name are separate
 * comments and a tag editor may write them in any order; the ones with a
 * time are then moved down into the order of their times.
 *
 * Parts, in order:
 *   - one comment
 *   - ogg_chapters_possible() and read_ogg_chapters()
 ****************************************************************************/

#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "file.h"
#include "system.h"
#include "metadata.h"
#include "metadata_common.h"
#include "string-extra.h"
#include "cuesheet.h"
#include "chapters.h"
#include "ogg_chapters.h"

/* Enough of a comment for its key and a chapter's name */
#define COMMENT_HEAD 300

/* ---- one comment ----------------------------------------------------- */

static unsigned long digits(const char **p, int *count)
{
    unsigned long v = 0;

    for (*count = 0; **p >= '0' && **p <= '9'; (*p)++, (*count)++)
        v = v * 10 + (**p - '0');
    return v;
}

/* HH:MM:SS, with .mmm after it if it has one, into ms; false if it is not
 * a time */
static bool parse_time(const char *s, unsigned long *ms)
{
    unsigned long h, m, sec, frac = 0;
    int n;

    h = digits(&s, &n);
    if (!n || *s++ != ':')
        return false;
    m = digits(&s, &n);
    if (!n || *s++ != ':')
        return false;
    sec = digits(&s, &n);
    if (!n)
        return false;
    if (*s == '.')
    {
        s++;
        frac = digits(&s, &n);
        while (n > 3)
        {
            frac /= 10;
            n--;
        }
        while (n < 3)
        {
            frac *= 10;
            n++;
        }
    }
    *ms = ((h * 60 + m) * 60 + sec) * 1000 + frac;
    return true;
}

/* A CHAPTERnnn= or CHAPTERnnnNAME= comment onto entry nnn */
static void read_comment(const char *c, struct cuesheet *cue, bool *timed)
{
    unsigned long n;
    int count;

    if (strncasecmp(c, "CHAPTER", 7))
        return;
    c += 7;
    n = digits(&c, &count);
    if (!count || n >= MAX_TRACKS)
        return;
    if (*c == '=')
        timed[n] = parse_time(c + 1, &cue->tracks[n].offset);
    else if (!strncasecmp(c, "NAME=", 5))
        strmemccpy(cue->tracks[n].title, c + 5,
                   sizeof(cue->tracks[n].title));
}

/* ---- ogg_chapters_possible() and read_ogg_chapters() ------------------ */

/* Only a book is looked in: every music track in Ogg would otherwise cost a
 * read of its comments on each load */
bool ogg_chapters_possible(const char *path, bool spoken)
{
    const char *ext = strrchr(path, '.');

    return spoken && ext && (!strcasecmp(ext, ".ogg")
                             || !strcasecmp(ext, ".oga")
                             || !strcasecmp(ext, ".opus"));
}

int read_ogg_chapters(const char *path, struct cuesheet *cue)
{
    static struct cue_track_info held;
    bool timed[MAX_TRACKS];
    unsigned char head[92];
    char c[COMMENT_HEAD + 1];
    struct ogg_file ogg;
    uint32_t left;
    int fd, type, found = 0;

    fd = open(path, O_RDONLY, 0644);
    if (fd < 0)
        return 0;
    type = get_ogg_format_and_move_to_comments(fd, head);
    if ((type != AFMT_OGG_VORBIS && type != AFMT_OPUS)
        || !ogg_file_init(&ogg, fd, type, 0))
    {
        close(fd);
        return 0;
    }

    /* the vendor string, then the count of comments */
    memset(timed, 0, sizeof(timed));
    if (ogg_file_read(&ogg, head, 4) == 4
        && ogg_file_read(&ogg, NULL, get_long_le(head)) >= 0
        && ogg_file_read(&ogg, head, 4) == 4)
    {
        for (left = get_long_le(head); left > 0; left--)
        {
            uint32_t len, take;

            if (ogg_file_read(&ogg, head, 4) != 4)
                break;
            len = get_long_le(head);
            take = MIN(len, (uint32_t)COMMENT_HEAD);
            if (ogg_file_read(&ogg, c, take) != (ssize_t)take
                || (len > take && ogg_file_read(&ogg, NULL, len - take) < 0))
                break;
            c[take] = '\0';
            read_comment(c, cue, timed);
        }
    }
    close(fd);

    /* Down into time order: entry n is held while the ones from its place
     * up to it move up one, and nothing past n is touched, since found <= n */
    for (int n = 0; n < MAX_TRACKS; n++)
    {
        int p = found;

        if (!timed[n])
            continue;
        held = cue->tracks[n];
        while (p > 0 && cue->tracks[p - 1].offset > held.offset)
            p--;
        memmove(&cue->tracks[p + 1], &cue->tracks[p],
                (found - p) * sizeof(struct cue_track_info));
        cue->tracks[p] = held;
        found++;
    }
    return found;
}
