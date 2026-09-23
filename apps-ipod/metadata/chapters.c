/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Choosing which reader a book's chapter marks need, and dressing what it
 * finds as a cuesheet.
 *
 * Where the marks live is the container's business, so each format has its
 * own reader and this is only the choice between them plus the part they
 * share. A reader fills the entries and says how many it found; the book's
 * own name and author, which come from the tags rather than from the marks,
 * are put on here.
 *
 * The choice is made on the extension alone, because it is asked before the
 * cuesheet buffer is allocated and so has to be answered without opening
 * anything.
 ****************************************************************************/

#include <stdbool.h>
#include <string.h>
#include "metadata.h"
#include "string-extra.h"
#include "cuesheet.h"
#include "chapters.h"
#include "mp4_chapters.h"
#include "id3_chapters.h"

bool chapters_possible(const char *path)
{
    return mp4_chapters_possible(path) || id3_chapters_possible(path);
}

bool parse_chapters_path(const char *path, const char *book,
                         const char *author, struct cuesheet *cue)
{
    int found = 0;

    memset(cue, 0, sizeof(struct cuesheet));

    if (mp4_chapters_possible(path))
        found = read_mp4_chapters(path, cue);
    else if (id3_chapters_possible(path))
        found = read_id3_chapters(path, cue);

    if (found < MIN_CHAPTERS)
        return false;

    cue->track_count = found;
    cue->chapters = true;
    cue->curr_track = cue->tracks;
    strmemccpy(cue->path, path, MAX_PATH);
    strmemccpy(cue->file, path, MAX_PATH);

    if (author)
        strmemccpy(cue->performer, author, sizeof(cue->performer));

    if (book)
        strmemccpy(cue->title, book, sizeof(cue->title));

    return true;
}

bool parse_chapters(struct mp3entry *id3, struct cuesheet *cue)
{
    const char *author = id3->albumartist ? id3->albumartist : id3->artist;

    return parse_chapters_path(id3->path, id3->album, author, cue);
}
