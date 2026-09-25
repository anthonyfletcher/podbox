/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Reading the chapter frames an MP3 carries in its ID3 tag.
 *
 * A chaptered MP3 holds one CHAP frame per chapter at the head of the file,
 * each carrying the millisecond the chapter starts at and, as a frame nested
 * inside it, the TIT2 holding its name. Both are filled into a struct
 * cuesheet, the same shape the container formats are read into.
 *
 * Frames are not read in the order they appear: a tag editor rewriting the
 * tag may put them in any order it likes, and everything downstream -- the
 * browser, the progress markers, the skip keys -- reads the entries as
 * ascending. Each chapter is placed by its start time as it is read. That is
 * also why the CTOC frame, which names an order of its own, is not read: the
 * times already say everything it would, and a CTOC on its own carries no
 * names to show.
 *
 * Parts, in order:
 *   - the two integer widths a tag uses
 *   - walking to a frame
 *   - one text frame
 *   - where a chapter belongs, and one CHAP frame
 *   - id3_chapters_possible() and read_id3_chapters()
 ****************************************************************************/

#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "file.h"
#include "system.h"
#include "metadata.h"
#include "metadata_common.h"
#include "string-extra.h"
#include "rbunicode.h"
#include "logf.h"
#include "cuesheet.h"
#include "chapters.h"
#include "id3_chapters.h"

#define ID3_CHAP FOURCC('C', 'H', 'A', 'P')
#define ID3_TIT2 FOURCC('T', 'I', 'T', '2')

/* "ID3", two version bytes, the flags and the size. */
#define ID3_HEADER 10

/* A frame is a four-character id, a size and two flag bytes. */
#define ID3_FRAME_HEADER 10

/* A CHAP frame's times: the start and end, then two byte offsets. */
#define CHAP_TIMES 16

/* A size that has to survive unsynchronisation keeps the top bit of every
 * byte clear, so it carries seven bits each rather than eight. */
static uint32_t syncsafe(const unsigned char *b)
{
    return ((uint32_t)(b[0] & 0x7f) << 21) | ((uint32_t)(b[1] & 0x7f) << 14)
         | ((uint32_t)(b[2] & 0x7f) << 7)  |  (uint32_t)(b[3] & 0x7f);
}

static uint32_t plain32(const unsigned char *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
         | ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

/* Read the header of the frame at the current position, reporting its id and
 * where its body stops and leaving the file at that body. Version 4 writes
 * the size seven bits to the byte where version 3 writes eight. */
static bool next_frame(int fd, off_t end, int major, uint32_t *id,
                       off_t *body_end)
{
    unsigned char h[ID3_FRAME_HEADER];
    off_t start = lseek(fd, 0, SEEK_CUR);
    uint32_t size;

    if (start < 0 || start + ID3_FRAME_HEADER > end)
        return false;

    if (read(fd, h, ID3_FRAME_HEADER) != ID3_FRAME_HEADER)
        return false;

    /* the tag is padded to its declared size with zeroes */
    if (h[0] == 0)
        return false;

    *id = FOURCC(h[0], h[1], h[2], h[3]);
    size = major >= 4 ? syncsafe(h + 4) : plain32(h + 4);

    if (size == 0 || size > (uint32_t)(end - start - ID3_FRAME_HEADER))
        return false;

    *body_end = start + ID3_FRAME_HEADER + size;

    return true;
}

/* A text frame is an encoding byte and the string. An encoding this does not
 * know is read as ISO 8859-1, which is what the tag would have held before
 * the others existed. */
static void read_frame_text(int fd, off_t end, char *out, size_t out_size)
{
    unsigned char raw[MAX_NAME*3+1];
    off_t start = lseek(fd, 0, SEEK_CUR);
    unsigned char encoding;
    size_t want;

    out[0] = '\0';

    if (start < 0 || start + 1 >= end || read(fd, &encoding, 1) != 1)
        return;

    want = MIN((size_t)(end - start - 1), sizeof(raw) - 1);

    if (read(fd, raw, want) != (ssize_t)want)
        return;

    switch (encoding)
    {
        case 0x01:  /* UTF-16, behind a byte order mark */
        {
            bool le = want >= 2 && raw[0] == 0xff && raw[1] == 0xfe;
            size_t skip = (le || (want >= 2 && raw[0] == 0xfe
                                  && raw[1] == 0xff)) ? 2 : 0;

            int units = chapter_utf16_units(raw + skip,
                                            (int)(want - skip) / 2, le);

            *utf16decode(raw + skip, (unsigned char *)out, units,
                         out_size - 1, le) = '\0';
            break;
        }

        case 0x02:  /* UTF-16, big endian, with no mark */
            *utf16decode(raw, (unsigned char *)out,
                         chapter_utf16_units(raw, (int)want / 2, false),
                         out_size - 1, false) = '\0';
            break;

        case 0x03:  /* UTF-8 */
            memcpy(out, raw, want);
            out[want] = '\0';
            break;

        default:
            *iso_decode_ex(raw, (unsigned char *)out, -1, want,
                           out_size - 1) = '\0';
            break;
    }
}

/* Open the entry a chapter starting at "offset" belongs in, shifting the
 * later ones up to make room. The entries below it are already in order, so
 * this walks back only as far as it has to. */
static struct cue_track_info *slot_for(struct cuesheet *cue, int found,
                                       unsigned long offset)
{
    int p = found;

    while (p > 0 && cue->tracks[p - 1].offset > offset)
        p--;

    if (p < found)
        memmove(&cue->tracks[p + 1], &cue->tracks[p],
                (found - p) * sizeof (struct cue_track_info));

    cue->tracks[p].offset = offset;

    return &cue->tracks[p];
}

/* A CHAP frame opens with an element id that nothing shows, then the times,
 * then the frames describing the chapter -- of which only the name is worth
 * anything here. */
static bool read_chap_frame(int fd, off_t frame_end, int major,
                            struct cuesheet *cue, int found)
{
    struct cue_track_info *track;
    unsigned char times[CHAP_TIMES];
    uint32_t sub_id;
    off_t sub_end;
    char c;

    do {
        if (lseek(fd, 0, SEEK_CUR) >= frame_end || read(fd, &c, 1) != 1)
            return false;
    } while (c != '\0');

    if (lseek(fd, 0, SEEK_CUR) + CHAP_TIMES > frame_end
        || read(fd, times, CHAP_TIMES) != CHAP_TIMES)
        return false;

    track = slot_for(cue, found, plain32(times));
    track->title[0] = '\0';

    while (next_frame(fd, frame_end, major, &sub_id, &sub_end))
    {
        if (sub_id == ID3_TIT2)
        {
            read_frame_text(fd, sub_end, track->title, sizeof(track->title));
            break;
        }

        if (lseek(fd, sub_end, SEEK_SET) != sub_end)
            break;
    }

    return true;
}

bool id3_chapters_possible(const char *path)
{
    const char *ext = strrchr(path, '.');

    return ext && !strcasecmp(ext, ".mp3");
}

int read_id3_chapters(const char *path, struct cuesheet *cue)
{
    unsigned char header[ID3_HEADER];
    off_t tag_end, frame_end;
    uint32_t frame_id;
    int major, found = 0;
    int fd;

    fd = open(path, O_RDONLY, 0644);
    if (fd < 0)
        return 0;

    /* An unsynchronised tag stands 0xFF 0x00 in for every 0xFF, which each
       size and offset below would have to be read through. Nothing writes a
       chaptered book that way, so such a tag is passed over rather than
       misread. */
    if (read(fd, header, ID3_HEADER) != ID3_HEADER
        || memcmp(header, "ID3", 3) != 0
        || header[3] < 3 || header[3] > 4
        || (header[5] & 0x80))
    {
        close(fd);
        return 0;
    }

    major = header[3];
    tag_end = ID3_HEADER + syncsafe(header + 6);

    /* An extended header sits before the first frame and gives its own
       length -- counted from after the length in version 3, and from the
       start of it in version 4. */
    if (header[5] & 0x40)
    {
        unsigned char ext[4];
        uint32_t skip;

        if (read(fd, ext, 4) != 4)
        {
            close(fd);
            return 0;
        }

        skip = major >= 4 ? syncsafe(ext) - 4 : plain32(ext);

        if (skip > (uint32_t)tag_end || lseek(fd, skip, SEEK_CUR) < 0)
        {
            close(fd);
            return 0;
        }
    }

    while (found < MAX_TRACKS
           && next_frame(fd, tag_end, major, &frame_id, &frame_end))
    {
        if (frame_id == ID3_CHAP
            && read_chap_frame(fd, frame_end, major, cue, found))
            found++;

        if (lseek(fd, frame_end, SEEK_SET) != frame_end)
            break;
    }

    close(fd);

    if (found == 0)
        logf("no chapter frames in %s", path);

    return found;
}
