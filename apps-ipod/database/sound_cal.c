/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Where this library sits on each axis.
 *
 * The read-out bands music into words -- Dark, Busy, Very Dynamic -- and each
 * band edge is a percentile, so that a word names music that exists rather
 * than a corner of a scale nothing reaches. The edges shipped in
 * screens/playback/sound_props.c are percentiles of the library they were
 * derived on. This file makes them percentiles of the library they are being
 * shown on.
 *
 * One pass produces them and one file holds them. The pass reads the finished
 * index sequentially, histograms each axis and reads the ladder off the
 * cumulative counts -- no sort, no allocation, and nothing of the audio
 * buffer, which taking would stop playback (see sound_mix.c).
 *
 * The axis and not the raw field, which is worth stating because the reverse
 * looks more careful and is not. sound_mix_axes() clamps at fixed endpoints,
 * so a library whose music sits outside them lands every track on 0 or 1000.
 * Taking the percentile from the raw field would recover the edge, but not
 * the *track's* coordinate, which is clamped in the same way: an edge that
 * separates tracks the axis cannot tell apart separates nothing. The
 * information is gone at the axis, so it is the axis this measures -- and the
 * span test below is what notices that it has gone.
 *
 * Nothing here can make the read-out worse than not running. Every answer is
 * either a number this library supports or -1, and -1 is the shipped number.
 *
 * Parts, in order:
 *   - the axes, and the ladder
 *   - the pass
 *   - the file
 *   - what a reader asks
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "database/sound_cal.h"
#include "database/sound_index.h"
#include "database/sound_mix.h"

#define CAL_FILE  ROCKBOX_DIR "/db_sound.cal"
#define CAL_MAGIC 0x4c414353   /* "SCAL" */
#define CAL_VER   2


/** The axes, and the ladder **/

/* Read by offset rather than by a switch, so the table below is the only
 * place an axis is named and a row of the file cannot drift from the axis it
 * describes. sound_mood.c reads its own targets the same way. */
static const size_t cal_off[CAL_AXES] = {
    offsetof(struct sound_axes, energy),
    offsetof(struct sound_axes, bright),
    offsetof(struct sound_axes, dens),
    offsetof(struct sound_axes, peak),
    offsetof(struct sound_axes, clarity),
    offsetof(struct sound_axes, width),
    offsetof(struct sound_axes, crest),
    offsetof(struct sound_axes, change),
    offsetof(struct sound_axes, low),
    offsetof(struct sound_axes, mid),
    offsetof(struct sound_axes, loud),
    offsetof(struct sound_axes, tempo),
    offsetof(struct sound_axes, speed),
};

/* Below this a percentile is not a percentile. At three hundred records a p10
 * is decided by thirty of them, which one unusual box set can supply; below
 * that the ladder describes whatever was scanned first. The shipped numbers
 * are a better answer than a confident wrong one.
 *
 * Counted per axis, not per record. Tempo and speed are absent from every
 * track the tracker did not settle on -- 3,073 of 3,439 on the library
 * measured -- and a percentile of those two is a percentile of the tracks
 * that have them. Anything else would rank a library against music it cannot
 * offer. */
#define CAL_MIN_RECORDS  300

/* An axis whose p10 and p90 are closer together than this cannot be banded:
 * either the library really does sit in one place on it, or the axis endpoints
 * have clamped it there. Both want the shipped edges, and from here the two
 * are indistinguishable -- which is the whole reason this test exists rather
 * than a check on the endpoints. */
#define CAL_MIN_SPAN     40

/* Four units of the axis to a bucket. The shipped edges are given to single
 * units and then rounded to tens, so 0.4% of the scale is finer than anything
 * that reads the answer. */
#define CAL_SHIFT        2
#define CAL_BUCKETS      ((SOUND_AX >> CAL_SHIFT) + 1)


/** The pass **/

/* Static, and only ever live inside cal_pass(). Two and a half thousand
 * counters is 5K, which is small against the 38K sound_mix.c holds for the
 * candidate arrays and cannot come from anywhere else: core_alloc here would
 * take it from the audio buffer. */
static uint16_t cal_hist[CAL_AXES][CAL_BUCKETS];

/* Tracks counted on each axis, which is not the record count: an axis a
 * record has no reading for is not counted against it. */
static uint32_t cal_n[CAL_AXES];

/* The ladder as loaded or computed, and whether it may be used. */
static int16_t cal_val[CAL_AXES][CAL_PCOUNT];
static bool    cal_have;

/* Resolved once. A calibration is produced where an index is written, so the
 * two only come apart when an index arrives from somewhere else -- the
 * desktop tool, over USB -- and that one is picked up at the next boot rather
 * than costing every reader a second file open to notice. */
static bool    cal_tried;

static int axis_of(const struct sound_axes *a, int ax)
{
    return *(const int *)((const char *)a + cal_off[ax]);
}

/* The ladder for one axis, off the cumulative counts. An axis the span test
 * refuses is left at -1 and the rest are left alone: each is calibrated or
 * not on its own evidence. */
static void cal_ladder(int ax)
{
    uint32_t total = cal_n[ax];
    uint32_t cum = 0;
    int b = 0;
    int p, v;

    if (total >= CAL_MIN_RECORDS)
    {
        /* One sweep for the whole curve: the percentiles rise, so the bucket
         * cursor only ever moves forward and the hundred and one points cost
         * one pass over the histogram between them. */
        for (p = 0; p < CAL_PCOUNT; p++)
        {
            /* Rounded, so p50 of an even count lands between its two middle
             * records rather than always below them. */
            uint32_t want = (total * (uint32_t)p + 50) / 100;

            if (want == 0)
                want = 1;

            while (b < CAL_BUCKETS - 1 && cum + cal_hist[ax][b] < want)
                cum += cal_hist[ax][b++];

            /* Into the bucket, not just to it. A bucket is four units wide
             * and taking its middle every time is a systematic error the
             * round trip shows up directly: a target measured to a percentile
             * and resolved back came out a mean of eight units away with
             * that, and one or two with this. */
            v = b << CAL_SHIFT;

            if (cal_hist[ax][b] > 0)
                v += (int)((want - cum) * (1u << CAL_SHIFT) /
                           cal_hist[ax][b]);

            cal_val[ax][p] = (int16_t)(v > SOUND_AX ? SOUND_AX : v);
        }

        if (cal_val[ax][90] - cal_val[ax][10] >= CAL_MIN_SPAN)
            return;
    }

    for (p = 0; p < CAL_PCOUNT; p++)
        cal_val[ax][p] = -1;
}

/* Read the index and fill cal_val[]. The number of records counted, or 0
 * where there were too few to say anything. */
static uint32_t cal_pass(void)
{
    struct sound_index_reader rd;
    struct sound_record r;
    struct sound_axes a;
    uint32_t total = 0;
    int i, ax;

    memset(cal_hist, 0, sizeof (cal_hist));
    memset(cal_n, 0, sizeof (cal_n));

    if (sound_index_reader_open(&rd) != SOUND_OK)
        return 0;

    for (i = 0; i < rd.count; i++)
    {
        if (!sound_index_read(&rd, i, &r))
            break;

        /* A failed decode is written zeroed and zero is not neutral here --
         * a loudness of zero is full scale -- so such a record would drag
         * every percentile toward a track that was never measured. */
        if (!sound_record_usable(&r))
            continue;

        sound_mix_axes(&r, &a);

        for (ax = 0; ax < CAL_AXES; ax++)
        {
            int v = axis_of(&a, ax);

            /* Skipped, not clamped to zero. Tempo and speed are -1 on a
             * track the tracker never settled on, and counting those as the
             * slowest music in the library would put every percentile on
             * both axes at the bottom of it. */
            if (v < 0)
                continue;

            if (v > SOUND_AX)
                v = SOUND_AX;

            cal_hist[ax][v >> CAL_SHIFT]++;
            cal_n[ax]++;
        }

        total++;
    }

    sound_index_reader_close(&rd);

    /* Each axis stands or falls on its own count -- see CAL_MIN_RECORDS --
     * so the pass carries on even where one of them is too thin to rank. */
    for (ax = 0; ax < CAL_AXES; ax++)
        cal_ladder(ax);

    return total;
}


/** The file **/

/* What the index looked like when this was taken.
 *
 * Three fields because the count alone passes an index whose tracks were
 * replaced one for one, and because there is no file mtime to ask for: the
 * firmware's file layer does not carry one, and the mtime in a record is the
 * music file's rather than the index's. The two boundary keys are two seeks
 * and they move whenever the library's extent does. */
struct cal_header
{
    uint32_t magic;
    uint16_t version;
    uint16_t axes;       /* CAL_AXES this was written with */
    uint16_t pcts;       /* CAL_PCOUNT ditto */
    uint16_t reserved;
    uint32_t count;      /* records the index held */
    uint64_t first_key;
    uint64_t last_key;
};

/* The index's own fingerprint, for comparing against a file's. False where
 * there is no index to take one from. */
static bool cal_fingerprint(uint32_t *count, uint64_t *first, uint64_t *last)
{
    struct sound_index_reader rd;
    struct sound_record r;
    bool ok = false;

    if (sound_index_reader_open(&rd) != SOUND_OK)
        return false;

    if (rd.count > 0 &&
        sound_index_read(&rd, 0, &r))
    {
        *first = r.key;

        if (sound_index_read(&rd, rd.count - 1, &r))
        {
            *last = r.key;
            *count = (uint32_t)rd.count;
            ok = true;
        }
    }

    sound_index_reader_close(&rd);

    return ok;
}

static bool cal_read(uint32_t count, uint64_t first, uint64_t last)
{
    struct cal_header h;
    int fd = open(CAL_FILE, O_RDONLY);
    bool ok = false;

    if (fd < 0)
        return false;

    if (read(fd, &h, sizeof (h)) == (ssize_t)sizeof (h) &&
        h.magic == CAL_MAGIC && h.version == CAL_VER &&
        h.axes == CAL_AXES && h.pcts == CAL_PCOUNT &&
        h.count == count && h.first_key == first && h.last_key == last &&
        read(fd, cal_val, sizeof (cal_val)) == (ssize_t)sizeof (cal_val))
    {
        ok = true;
    }

    close(fd);

    return ok;
}

static bool cal_save(uint32_t count, uint64_t first, uint64_t last)
{
    struct cal_header h;
    int fd;
    bool ok;

    h.magic     = CAL_MAGIC;
    h.version   = CAL_VER;
    h.axes      = CAL_AXES;
    h.pcts      = CAL_PCOUNT;
    h.reserved  = 0;
    h.count     = count;
    h.first_key = first;
    h.last_key  = last;

    fd = open(CAL_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;

    /* Torn by a power cut it fails its own length check on the way back in,
     * which costs the shipped numbers and one pass to rebuild. That is what
     * the index's write-then-rename dance buys, for a file of a few hundred
     * bytes that can always be made again. */
    ok = write(fd, &h, sizeof (h)) == (ssize_t)sizeof (h) &&
         write(fd, cal_val, sizeof (cal_val)) == (ssize_t)sizeof (cal_val);

    close(fd);

    if (!ok)
        remove(CAL_FILE);

    return ok;
}

bool sound_cal_update(void)
{
    uint32_t count = 0, total;
    uint64_t first = 0, last = 0;

    cal_tried = true;
    cal_have  = false;

    if (!cal_fingerprint(&count, &first, &last))
        return false;

    total = cal_pass();
    if (total == 0)
    {
        /* Nothing to say about this library. The file goes, so that a stale
         * one from a larger index is not left describing it. */
        remove(CAL_FILE);
        return false;
    }

    cal_have = true;

    return cal_save(count, first, last);
}

void sound_cal_ensure(void)
{
    uint32_t count = 0;
    uint64_t first = 0, last = 0;

    if (cal_tried)
        return;

    cal_tried = true;

    if (!sound_index_exists() ||
        !cal_fingerprint(&count, &first, &last))
        return;

    if (cal_read(count, first, last))
        cal_have = true;
    else
        sound_cal_update();
}


/** What a reader asks **/

int sound_cal_at(int axis, int permille)
{
    int p, lo, hi;

    if (!cal_have || axis < 0 || axis >= CAL_AXES ||
        permille < 0 || permille > 1000)
        return -1;

    p = permille / 10;
    lo = cal_val[axis][p];

    if (lo < 0 || p >= 100 || permille % 10 == 0)
        return lo;

    /* Between two stored points, because whole percent is not fine enough to
     * ask in. One percent of a 3,439-track library is thirty-four tracks, and
     * where the axis is crowded those thirty-four span tens of units -- so a
     * target rounded to whole percent and resolved back came out a mean of 18
     * units below where it went in, every time in the same direction. The
     * curve between two points is monotonic; a straight line across it costs
     * nothing and removes the quantisation. */
    hi = cal_val[axis][p + 1];

    return lo + (hi - lo) * (permille % 10) / 10;
}

size_t sound_cal_offset(int axis)
{
    return axis >= 0 && axis < CAL_AXES ? cal_off[axis] : 0;
}
