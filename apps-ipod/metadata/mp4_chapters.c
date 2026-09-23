/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Reading an audiobook's chapter marks out of the file itself.
 *
 * A chaptered .m4b is one long track carrying a list of the points where
 * its chapters begin. The list is filled into a struct cuesheet, which is
 * the shape the rest of the player already understands: the browser lists
 * it, the progress bar marks it, and skip moves between its entries. Only
 * the reader is new.
 *
 * Books carry that list one of two ways, and both are read here. Nero's
 * "chpl" is a flat table in the file's user data, and is what most
 * conversion tools write. Apple instead gives the book a second track
 * holding the names as timed text, which the audio track points at through
 * a "tref/chap"; reading that one means walking the sample tables to find
 * where each name lies. A file carrying both is read from chpl, the cheaper
 * of the two.
 *
 * A list of one entry is not a chapter list -- a lone chpl record is how
 * Nero marks encoder delay -- so two are needed before either is believed.
 * Chapters past MAX_TRACKS are dropped rather than refused, so a long book
 * still opens on the part of itself that fits.
 *
 * Parts, in order:
 *   - walking to a box
 *   - reading at an offset
 *   - the Nero chapter list
 *   - a track's ids, and the sample tables of the chapter track
 *   - one timed-text sample
 *   - the Apple chapter track
 *   - read_mp4_chapters()
 ****************************************************************************/

#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "file.h"
#include "system.h"
#include "metadata.h"
#include "metadata_common.h"
#include "string-extra.h"     /* strcasecmp */
#include "rbunicode.h"
#include "logf.h"
#include "cuesheet.h"
#include "chapters.h"
#include "mp4_chapters.h"

#define MP4_chap FOURCC('c', 'h', 'a', 'p')
#define MP4_chpl FOURCC('c', 'h', 'p', 'l')
#define MP4_co64 FOURCC('c', 'o', '6', '4')
#define MP4_mdhd FOURCC('m', 'd', 'h', 'd')
#define MP4_mdia FOURCC('m', 'd', 'i', 'a')
#define MP4_minf FOURCC('m', 'i', 'n', 'f')
#define MP4_moov FOURCC('m', 'o', 'o', 'v')
#define MP4_stbl FOURCC('s', 't', 'b', 'l')
#define MP4_stco FOURCC('s', 't', 'c', 'o')
#define MP4_stsc FOURCC('s', 't', 's', 'c')
#define MP4_stsz FOURCC('s', 't', 's', 'z')
#define MP4_stts FOURCC('s', 't', 't', 's')
#define MP4_tkhd FOURCC('t', 'k', 'h', 'd')
#define MP4_trak FOURCC('t', 'r', 'a', 'k')
#define MP4_tref FOURCC('t', 'r', 'e', 'f')
#define MP4_udta FOURCC('u', 'd', 't', 'a')

/* A chpl record is a 64-bit timestamp, a length byte and the title. */
#define CHPL_RECORD_MIN 9

/* Its count is preceded by a version, its flags and a reserved word. */
#define CHPL_HEADER     9

/* chpl timestamps are in units of 100ns. */
#define CHPL_TICKS_PER_MS 10000

/* Where a sample table's entries begin, and how many there are. */
struct table {
    off_t pos;
    uint32_t count;
};

/* What the chapter track's tables say about where its names lie. */
struct sample_tables {
    struct table stts;      /* how long each sample lasts, run-length coded */
    struct table stsc;      /* how many samples each run of chunks holds */
    struct table stco;      /* where each chunk begins */
    struct table stsz;      /* how long each sample is */
    uint32_t fixed_size;    /* the one size they share, when stsz gives one */
    uint32_t timescale;     /* ticks per second */
    bool wide_chunks;       /* chunk offsets are 64-bit */
};

/* Read the header of the box at the current position, reporting its type and
 * where it stops and leaving the file at its payload. A box is a 32-bit size
 * covering its own eight-byte header, then a fourcc; a size of 1 means the
 * real size follows the fourcc as a 64-bit field, and 0 means the box runs
 * to the end of its container. */
static bool next_box(int fd, off_t end, uint32_t *type, off_t *box_end)
{
    off_t start = lseek(fd, 0, SEEK_CUR);
    uint32_t size32;
    uint64_t size;

    if (start < 0 || start + 8 > end)
        return false;

    if (read_uint32be(fd, &size32) != 4 || read_uint32be(fd, type) != 4)
        return false;

    size = size32;

    if (size == 1)
    {
        if (read_uint64be(fd, &size) != 8 || size < 16)
            return false;
    }
    else if (size == 0)
        size = end - start;
    else if (size < 8)
        return false;

    if (size > (uint64_t)(end - start))
        return false;

    *box_end = start + size;

    return true;
}

/* Seek to the payload of the first "type" box lying before "end". */
static bool find_box(int fd, uint32_t type, off_t end, off_t *box_end)
{
    uint32_t kind;

    while (next_box(fd, end, &kind, box_end))
    {
        if (kind == type)
            return true;

        if (lseek(fd, *box_end, SEEK_SET) != *box_end)
            break;
    }

    return false;
}

static bool read_u32_at(int fd, off_t pos, uint32_t *out)
{
    return lseek(fd, pos, SEEK_SET) == pos && read_uint32be(fd, out) == 4;
}

static bool read_u64_at(int fd, off_t pos, uint64_t *out)
{
    return lseek(fd, pos, SEEK_SET) == pos && read_uint64be(fd, out) == 8;
}

/* The flat chapter table in the file's user data. */
static int read_chpl(int fd, off_t udta_end, struct cuesheet *cue)
{
    off_t chpl_end;
    uint8_t count = 0;
    int i, found = 0;

    if (!find_box(fd, MP4_chpl, udta_end, &chpl_end)
        || lseek(fd, CHPL_HEADER - 1, SEEK_CUR) < 0
        || read_uint8(fd, &count) != 1)
        return 0;

    for (i = 0; i < count && found < MAX_TRACKS; i++)
    {
        struct cue_track_info *track = &cue->tracks[found];
        uint64_t timestamp;
        uint8_t len = 0;
        size_t want;

        if (lseek(fd, 0, SEEK_CUR) + CHPL_RECORD_MIN > chpl_end
            || read_uint64be(fd, &timestamp) != 8
            || read_uint8(fd, &len) != 1)
            break;

        track->offset = timestamp / CHPL_TICKS_PER_MS;

        want = MIN((size_t)len, sizeof(track->title) - 1);

        if (want > 0 && read(fd, track->title, want) != (ssize_t)want)
            break;

        track->title[want] = '\0';

        if (len > want && lseek(fd, len - want, SEEK_CUR) < 0)
            break;

        found++;
    }

    return found;
}

/* The id a trak gives itself. The two times before it are twice the width in
 * a version 1 header. */
static uint32_t track_id(int fd, off_t trak_end)
{
    off_t tkhd_end;
    uint8_t version;
    uint32_t id;

    if (!find_box(fd, MP4_tkhd, trak_end, &tkhd_end)
        || read_uint8(fd, &version) != 1
        || lseek(fd, version == 1 ? 3 + 16 : 3 + 8, SEEK_CUR) < 0
        || read_uint32be(fd, &id) != 4)
        return 0;

    return id;
}

/* The id a trak names as its chapter track. A tref/chap may name several;
 * the first is the one shown. */
static uint32_t chapter_track_id(int fd, off_t trak_end)
{
    off_t tref_end, chap_end;
    uint32_t id;

    if (!find_box(fd, MP4_tref, trak_end, &tref_end)
        || !find_box(fd, MP4_chap, tref_end, &chap_end)
        || read_uint32be(fd, &id) != 4)
        return 0;

    return id;
}

/* A sample table opens with a version, its flags and an entry count. */
static bool read_table(int fd, uint32_t type, off_t stbl_start,
                       off_t stbl_end, struct table *t)
{
    off_t box_end;

    if (lseek(fd, stbl_start, SEEK_SET) != stbl_start
        || !find_box(fd, type, stbl_end, &box_end)
        || lseek(fd, 4, SEEK_CUR) < 0
        || read_uint32be(fd, &t->count) != 4)
        return false;

    t->pos = lseek(fd, 0, SEEK_CUR);

    return t->pos >= 0;
}

/* Everything needed to place the chapter track's samples in the file. */
static bool read_sample_tables(int fd, off_t trak_end,
                               struct sample_tables *st)
{
    off_t mdia_start, mdia_end, minf_end, stbl_start, stbl_end, box_end;
    uint8_t version;

    memset(st, 0, sizeof (struct sample_tables));

    if (!find_box(fd, MP4_mdia, trak_end, &mdia_end))
        return false;

    mdia_start = lseek(fd, 0, SEEK_CUR);

    if (mdia_start < 0
        || !find_box(fd, MP4_mdhd, mdia_end, &box_end)
        || read_uint8(fd, &version) != 1
        || lseek(fd, version == 1 ? 3 + 16 : 3 + 8, SEEK_CUR) < 0
        || read_uint32be(fd, &st->timescale) != 4
        || st->timescale == 0)
        return false;

    if (lseek(fd, mdia_start, SEEK_SET) != mdia_start
        || !find_box(fd, MP4_minf, mdia_end, &minf_end)
        || !find_box(fd, MP4_stbl, minf_end, &stbl_end))
        return false;

    stbl_start = lseek(fd, 0, SEEK_CUR);

    if (stbl_start < 0
        || !read_table(fd, MP4_stts, stbl_start, stbl_end, &st->stts)
        || !read_table(fd, MP4_stsc, stbl_start, stbl_end, &st->stsc))
        return false;

    /* stsz counts its samples after a size they all share, which is zero
       when they differ and the entries that follow give each one. */
    if (lseek(fd, stbl_start, SEEK_SET) != stbl_start
        || !find_box(fd, MP4_stsz, stbl_end, &box_end)
        || lseek(fd, 4, SEEK_CUR) < 0
        || read_uint32be(fd, &st->fixed_size) != 4
        || read_uint32be(fd, &st->stsz.count) != 4)
        return false;

    st->stsz.pos = lseek(fd, 0, SEEK_CUR);

    if (st->stsz.pos < 0)
        return false;

    if (read_table(fd, MP4_stco, stbl_start, stbl_end, &st->stco))
        st->wide_chunks = false;
    else if (read_table(fd, MP4_co64, stbl_start, stbl_end, &st->stco))
        st->wide_chunks = true;
    else
        return false;

    return true;
}

/* How many samples a chunk holds. Entries name the first chunk of a run, and
 * the run covers every chunk up to the one the next entry names. Chunks are
 * asked for in order, so the search carries on from the last answer. */
static bool samples_per_chunk(int fd, const struct table *stsc,
                              uint32_t chunk, uint32_t *cursor, uint32_t *per)
{
    uint32_t e;

    for (e = *cursor; e < stsc->count; e++)
    {
        uint32_t first, n, next_first;

        if (!read_u32_at(fd, stsc->pos + 12*e, &first)
            || read_uint32be(fd, &n) != 4
            || first > chunk)
            return false;

        if (e + 1 < stsc->count)
        {
            if (!read_u32_at(fd, stsc->pos + 12*(e + 1), &next_first))
                return false;

            if (next_first <= chunk)
                continue;
        }

        *cursor = e;
        *per = n;

        return true;
    }

    return false;
}

static bool chunk_offset(int fd, const struct sample_tables *st,
                         uint32_t chunk, off_t *pos)
{
    if (chunk >= st->stco.count)
        return false;

    if (st->wide_chunks)
    {
        uint64_t wide;

        if (!read_u64_at(fd, st->stco.pos + 8*chunk, &wide))
            return false;

        *pos = (off_t)wide;
    }
    else
    {
        uint32_t narrow;

        if (!read_u32_at(fd, st->stco.pos + 4*chunk, &narrow))
            return false;

        *pos = narrow;
    }

    return true;
}

static bool sample_size(int fd, const struct sample_tables *st, int index,
                        uint32_t *size)
{
    if (st->fixed_size)
    {
        *size = st->fixed_size;
        return true;
    }

    if ((uint32_t)index >= st->stsz.count)
        return false;

    return read_u32_at(fd, st->stsz.pos + 4*index, size);
}

/* A timed-text sample is a 16-bit length and the name, which is UTF-8 unless
 * it opens with a byte order mark. Whatever follows the name is styling and
 * is ignored. */
static void read_sample_title(int fd, off_t pos, uint32_t size,
                              struct cue_track_info *track)
{
    unsigned char raw[MAX_NAME*3+1];
    uint16_t len;
    size_t want;

    track->title[0] = '\0';

    if (size < 2 || lseek(fd, pos, SEEK_SET) != pos
        || read_uint16be(fd, &len) != 2 || len == 0)
        return;

    want = MIN((size_t)len, MIN((size_t)size - 2, sizeof(raw) - 1));

    if (read(fd, raw, want) != (ssize_t)want)
        return;

    if (want > 2 && ((raw[0] == 0xff && raw[1] == 0xfe)
                     || (raw[0] == 0xfe && raw[1] == 0xff)))
    {
        unsigned char *end = utf16decode(raw + 2,
                                         (unsigned char *)track->title,
                                         (want - 2) / 2,
                                         sizeof(track->title) - 1,
                                         raw[0] == 0xff);
        *end = '\0';
        return;
    }

    memcpy(track->title, raw, want);
    track->title[want] = '\0';
}

/* When each sample starts, in milliseconds. The durations are run-length
 * coded, so one entry can cover many samples. */
static int read_start_times(int fd, const struct sample_tables *st,
                            struct cuesheet *cue)
{
    uint64_t ticks = 0;
    uint32_t e;
    int found = 0;

    for (e = 0; e < st->stts.count && found < MAX_TRACKS; e++)
    {
        uint32_t count, delta, i;

        if (!read_u32_at(fd, st->stts.pos + 8*e, &count)
            || read_uint32be(fd, &delta) != 4)
            break;

        for (i = 0; i < count && found < MAX_TRACKS; i++)
        {
            cue->tracks[found++].offset =
                (unsigned long)(ticks * 1000 / st->timescale);
            ticks += delta;
        }
    }

    return found;
}

/* The names, walking the chunks in order and stepping through the samples
 * each one holds. */
static int read_sample_titles(int fd, const struct sample_tables *st,
                              int wanted, struct cuesheet *cue)
{
    uint32_t chunk = 0;         /* chunks placed so far */
    uint32_t left = 0;          /* samples still to come from the last one */
    uint32_t cursor = 0;        /* where the stsc search reached */
    off_t pos = 0;              /* where the next sample begins */
    int found = 0;

    while (found < wanted)
    {
        uint32_t size;

        while (left == 0)
        {
            uint32_t per;

            if (!samples_per_chunk(fd, &st->stsc, chunk + 1, &cursor, &per)
                || !chunk_offset(fd, st, chunk, &pos))
                return found;

            left = per;
            chunk++;
        }

        if (!sample_size(fd, st, found, &size))
            return found;

        read_sample_title(fd, pos, size, &cue->tracks[found]);

        pos += size;
        left--;
        found++;
    }

    return found;
}

/* The chapter track Apple's books carry. */
static int read_chap(int fd, off_t moov_start, off_t moov_end,
                     struct cuesheet *cue)
{
    struct sample_tables st;
    uint32_t wanted = 0, kind;
    off_t trak_end, payload;
    int times;

    /* The audio track names its chapter track by id. */
    if (lseek(fd, moov_start, SEEK_SET) != moov_start)
        return 0;

    while (!wanted && next_box(fd, moov_end, &kind, &trak_end))
    {
        if (kind == MP4_trak)
            wanted = chapter_track_id(fd, trak_end);

        if (lseek(fd, trak_end, SEEK_SET) != trak_end)
            return 0;
    }

    if (!wanted || lseek(fd, moov_start, SEEK_SET) != moov_start)
        return 0;

    /* Then the track carrying that id holds the names. */
    while (next_box(fd, moov_end, &kind, &trak_end))
    {
        payload = lseek(fd, 0, SEEK_CUR);

        if (payload >= 0 && kind == MP4_trak
            && track_id(fd, trak_end) == wanted
            && lseek(fd, payload, SEEK_SET) == payload
            && read_sample_tables(fd, trak_end, &st))
        {
            times = read_start_times(fd, &st, cue);

            return MIN(times, read_sample_titles(fd, &st, times, cue));
        }

        if (lseek(fd, trak_end, SEEK_SET) != trak_end)
            break;
    }

    return 0;
}

bool mp4_chapters_possible(const char *path)
{
    const char *ext = strrchr(path, '.');

    return ext && !strcasecmp(ext, ".m4b");
}

int read_mp4_chapters(const char *path, struct cuesheet *cue)
{
    off_t file_end, moov_start, moov_end, udta_end;
    int found = 0;
    int fd;

    fd = open(path, O_RDONLY, 0644);
    if (fd < 0)
        return false;

    file_end = lseek(fd, 0, SEEK_END);

    if (file_end < 0 || lseek(fd, 0, SEEK_SET) != 0
        || !find_box(fd, MP4_moov, file_end, &moov_end))
    {
        close(fd);
        return 0;
    }

    moov_start = lseek(fd, 0, SEEK_CUR);

    if (moov_start >= 0)
    {
        if (find_box(fd, MP4_udta, moov_end, &udta_end))
            found = read_chpl(fd, udta_end, cue);

        if (found < MIN_CHAPTERS)
            found = read_chap(fd, moov_start, moov_end, cue);
    }

    close(fd);

    if (found < MIN_CHAPTERS)
        logf("no chapters in %s", path);

    return found;
}
