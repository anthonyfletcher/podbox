/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The per-track sound measurements on disk.
 *
 * A scan appends to a working file as it goes and renames it into place at
 * the end. Append-only is what makes a run resumable for free: everything
 * finished is already on disk, so stopping costs nothing and needs no
 * confirmation, and an interruption -- the user, an unplugged charger, a flat
 * battery -- is the same path as finishing.
 *
 * The finished file is sorted by key so a reader can binary search it. The
 * working file is not, because sorting as it grows would mean rewriting it
 * per track; the sort happens once, at the end, from the key table the scan
 * was keeping anyway.
 *
 * Parts, in order:
 *   - keys
 *   - the file header
 *   - the working file: begin, done, add, finish
 *   - reading a finished index
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "file.h"
#include "core_alloc.h"
#include "audio/track_decode.h"
#include "database/sound_index.h"

#define SOUND_FILE   ROCKBOX_DIR "/db_sound.dat"
#define SOUND_PART   ROCKBOX_DIR "/db_sound.part"

/* Bump the version with the layout of struct sound_record or of the header.
 * A reader also checks the record size, which catches a struct that grew
 * without this moving -- but only by rejecting the file, so move this too and
 * say what changed.
 *   1: first.
 *   2: pitch[12], tonic, mode, mode_margin, tonal_clarity, harmonic_change.
 *      40 bytes to 56.
 *   3: size, so a record written on a desktop can be checked for staleness
 *      without an mtime the desktop cannot reproduce. 56 bytes to 64. */
#define SOUND_MAGIC    0x534E4431  /* "SND1" */
#define SOUND_VERSION  3

/* The record goes to disk verbatim, so its size is part of the format rather
 * than a detail of this build. A field added without the version moving would
 * otherwise change the stride and be caught only by the reader rejecting
 * every file. */
_Static_assert(sizeof (struct sound_record) == 64,
               "struct sound_record is the on-disk stride");

struct sound_header
{
    uint32_t magic;
    uint16_t version;
    uint16_t record_bytes;
    uint32_t count;
    uint32_t reserved;
};

/* What the scan keeps in memory: enough to answer "is this one done" and to
 * sort the working file at the end without holding every record. */
struct key_entry
{
    uint64_t key;
    uint32_t mtime;
    uint32_t size;
    uint32_t ordinal;   /* Position in the working file */
};

static int          part_fd = -1;
static int          table_handle;
static int          table_used;
static int          table_max;
static bool         table_sorted;   /* Of the first table_used entries */


/** Keys **/

#define FNV64_BASIS  0xcbf29ce484222325ULL
#define FNV64_PRIME  0x100000001b3ULL

static uint64_t fnv64_lower(const char *s)
{
    uint64_t h = FNV64_BASIS;

    for (; s && *s; s++)
    {
        unsigned char c = (unsigned char)*s;

        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';

        h = (h ^ c) * FNV64_PRIME;
    }

    return h;
}

/* The path as the index names it: without a volume specifier.
 *
 * The same track arrives under two names. tagcache hands a walk the path as
 * it was scanned, while retrieving an entry dircache holds rebuilds it from
 * the dircache tree, which puts the volume root on the front -- so one file
 * is "/Music/x.flac" through tagcache_get_next() and "/<HDD0>/Music/x.flac"
 * through tagcache_retrieve(). A key has to name the file rather than the
 * route the caller took to it, so every key goes through here.
 *
 * Parsed here rather than by path_strip_volume(), which lives behind
 * HAVE_MULTIVOLUME: the offline tool computes keys that must match the
 * player's byte for byte, without the firmware's path layer. */
bool sound_record_usable(const struct sound_record *r)
{
    return !(r->flags & SOUND_F_FAILED) && r->analysed_s > 0;
}

const char *sound_index_path(const char *path)
{
    const char *p = path;

    if (path == NULL || p[0] != '/' || p[1] != '<')
        return path;

    for (p += 2; *p != '\0' && *p != '>' && *p != '/'; p++)
        ;

    return (p[0] == '>' && p[1] == '/') ? p + 1 : path;
}

uint64_t sound_index_key(const char *path)
{
    /* Zero is the "no key" value a record is never written with, so a path
     * that happened to hash to it is nudged. */
    uint64_t h = fnv64_lower(sound_index_path(path));

    return h ? h : 1;
}

uint32_t sound_index_genre_key(const char *genre)
{
    if (genre == NULL || *genre == '\0')
        return 0;

    /* Folded to 32 bits from the same 64-bit hash rather than a separate
     * 32-bit one, so there is a single definition of "the same string" here.
     * A few hundred genres over 32 bits collide about once in fifty thousand
     * libraries, which is a mis-grouped genre and not a lost measurement. */
    return (uint32_t)(fnv64_lower(genre) >> 32);
}

static uint8_t cap8(unsigned int v)
{
    return v > 255 ? 255 : (uint8_t)v;
}

void sound_index_fill(struct sound_record *out, uint64_t key, uint32_t mtime,
                      uint32_t size, uint32_t genre_key, int year,
                      const struct track_sound *s, int decode_rc)
{
    int g;

    memset(out, 0, sizeof (*out));

    out->key       = key;
    out->mtime     = mtime;
    out->size      = size;
    out->genre_key = genre_key;
    out->year      = (uint16_t)(year > 0 ? year : 0);

    out->period_ms     = (uint16_t)s->period_ms;
    out->loudness_db10 = (int16_t)s->loudness_db10;
    out->confidence    = cap8(s->confidence);
    out->tempo_spread  = cap8(s->tempo_spread);
    out->crest_db      = cap8(s->crest_db10 / 10);
    out->width         = cap8(s->width);
    out->level_spread  = s->level_spread;
    out->strength      = s->strength;
    out->peakiness     = s->peakiness;
    out->analysed_s    = cap8((unsigned int)(s->analysed_ms / 1000));

    for (g = 0; g < BEAT_GROUPS; g++)
    {
        out->level[g]  = s->level[g];
        out->rate10[g] = cap8(s->rate10[g]);
    }

    for (g = 0; g < 12; g++)
        out->pitch[g] = s->chroma.pitch[g];

    out->tonic           = s->chroma.key;
    out->mode            = s->chroma.minor ? 1 : 0;
    out->mode_margin     = s->chroma.margin;
    out->tonal_clarity   = s->chroma.clarity;
    out->harmonic_change = s->chroma.change;

    if (s->chroma.margin < CHROMA_MARGIN_MIN)
        out->flags |= SOUND_F_NO_MODE;

    if (s->settled)
        out->flags |= SOUND_F_SETTLED;
    if (s->period_ms == 0)
        out->flags |= SOUND_F_NO_LOCK;
    if (decode_rc == TRACK_DECODE_FAILED ||
        decode_rc == TRACK_DECODE_NO_CODEC ||
        decode_rc == TRACK_DECODE_NO_FILE)
    {
        out->flags |= SOUND_F_FAILED;
    }
}


/** The file header **/

static void header_init(struct sound_header *h, uint32_t count)
{
    h->magic        = SOUND_MAGIC;
    h->version      = SOUND_VERSION;
    h->record_bytes = (uint16_t)sizeof (struct sound_record);
    h->count        = count;
    h->reserved     = 0;
}

static bool header_ok(const struct sound_header *h)
{
    return h->magic == SOUND_MAGIC &&
           h->version == SOUND_VERSION &&
           h->record_bytes == sizeof (struct sound_record);
}


/** The working file **/

static struct key_entry *table(void)
{
    return table_handle > 0 ? core_get_data(table_handle) : NULL;
}

static int key_cmp(const void *a, const void *b)
{
    const struct key_entry *x = a;
    const struct key_entry *y = b;

    if (x->key < y->key)
        return -1;
    if (x->key > y->key)
        return 1;

    return 0;
}

/* Copy the finished index into the working file.
 *
 * An update has to start from what is already known or it is a rebuild with a
 * gentler name. The records are copied rather than referenced because finish()
 * reads them back by their position in the working file, so everything it will
 * write has to be in that one file. At 64 bytes a track it is a megabyte for a
 * large library, read and written once. */
static bool seed_from_finished(void)
{
    struct sound_header h;
    struct sound_record r;
    struct key_entry *t = table();
    int fd = open(SOUND_FILE, O_RDONLY);
    unsigned int i;

    if (fd < 0)
        return true;    /* Nothing finished yet is not a failure */

    if (read(fd, &h, sizeof (h)) != (ssize_t)sizeof (h) || !header_ok(&h))
    {
        close(fd);
        return true;
    }

    lseek(part_fd, 0, SEEK_END);

    for (i = 0; i < h.count && table_used < table_max; i++)
    {
        if (read(fd, &r, sizeof (r)) != (ssize_t)sizeof (r))
            break;

        /* A failure is retried rather than inherited. What could not be read
         * last time is a property of the reader as much as the file -- a
         * decoder fix, or another machine's codecs -- and an update is how
         * that reaches an existing index without measuring the library
         * again. Dropping the record from the working file as well as the
         * table is what makes the scan measure the track; the cost is bounded
         * by the number of failures, which is small or the index is not worth
         * keeping. */
        if (r.flags & SOUND_F_FAILED)
            continue;

        if (write(part_fd, &r, sizeof (r)) != (ssize_t)sizeof (r))
            break;

        t[table_used].key = r.key;
        t[table_used].mtime = r.mtime;
        t[table_used].size = r.size;
        t[table_used].ordinal = (uint32_t)table_used;
        table_used++;
    }

    close(fd);

    header_init(&h, (uint32_t)table_used);
    lseek(part_fd, 0, SEEK_SET);
    write(part_fd, &h, sizeof (h));

    table_sorted = false;

    return true;
}

/* Read whatever the working file already holds into the table. A file whose
 * header is not ours is a file with nothing in it -- the same rule as the
 * rest of the fork's own files, and it earns its place here because a
 * foreign file of the wrong stride reads as plausible records rather than as
 * gibberish.
 *
 * An empty one is seeded from the finished index, which is what makes an
 * update an update. An unfinished one is left alone: it is a run in progress
 * and already holds everything the finished index did. */
/* 'seed' is whether a finished index may be carried into this run. False on
 * a rebuild, which is the whole of what makes it one: removing the working
 * file alone leaves the finished index to be read straight back in, and
 * nothing is measured again. */
static bool load_part(bool seed)
{
    struct sound_header h;
    struct sound_record r;
    struct key_entry *t = table();
    unsigned int i;

    if (read(part_fd, &h, sizeof (h)) != (ssize_t)sizeof (h) ||
        !header_ok(&h) || h.count == 0)
    {
        lseek(part_fd, 0, SEEK_SET);
        header_init(&h, 0);

        if (write(part_fd, &h, sizeof (h)) != (ssize_t)sizeof (h))
            return false;

        ftruncate(part_fd, sizeof (h));
        table_used = 0;
        table_sorted = true;

        return seed ? seed_from_finished() : true;
    }

    for (i = 0; i < h.count && table_used < table_max; i++)
    {
        if (read(part_fd, &r, sizeof (r)) != (ssize_t)sizeof (r))
            break;

        t[table_used].key = r.key;
        t[table_used].mtime = r.mtime;
        t[table_used].size = r.size;
        t[table_used].ordinal = (uint32_t)table_used;
        table_used++;
    }

    /* A count that ran past what the file holds is a run that was cut off
     * mid-write. What was read is sound; the header is corrected on the next
     * add, and until then only the table is consulted. */
    table_sorted = false;

    return true;
}

int sound_index_begin(int capacity, bool fresh)
{
    if (capacity < 1)
        capacity = 1;

    if (fresh)
        remove(SOUND_PART);

    table_handle = core_alloc((size_t)capacity * sizeof (struct key_entry));
    if (table_handle <= 0)
        return SOUND_ERR_MEM;

    table_max = capacity;
    table_used = 0;
    table_sorted = true;

    part_fd = open(SOUND_PART, O_RDWR | O_CREAT, 0666);
    if (part_fd < 0)
    {
        core_free(table_handle);
        table_handle = 0;
        return SOUND_ERR_IO;
    }

    if (!load_part(!fresh))
    {
        sound_index_close();
        return SOUND_ERR_IO;
    }

    return SOUND_OK;
}

bool sound_index_done(uint64_t key, uint32_t mtime, uint32_t size)
{
    struct key_entry *t = table();
    int i;

    if (t == NULL)
        return false;

    /* Linear, and that is not an oversight: this is asked once per track
     * against a table of at most a few tens of thousands, beside a decode
     * that takes seconds. Keeping it sorted across appends would cost more
     * than it saves. */
    for (i = 0; i < table_used; i++)
    {
        if (t[i].key != key)
            continue;

        if (t[i].size != size)
            return false;

        /* Zero on either side is "not known", which is what a desktop tool
         * writes. Two known values that disagree are a changed file; anything
         * else falls back to the size having matched. */
        if (t[i].mtime != 0 && mtime != 0 && t[i].mtime != mtime)
            return false;

        return true;
    }

    return false;
}

bool sound_index_add(const struct sound_record *r)
{
    struct sound_header h;
    struct key_entry *t = table();
    off_t before;

    if (part_fd < 0 || t == NULL || table_used >= table_max)
        return false;

    before = lseek(part_fd, 0, SEEK_END);
    if (before < (off_t)sizeof (h))
        return false;

    if (write(part_fd, r, sizeof (*r)) != (ssize_t)sizeof (*r))
    {
        ftruncate(part_fd, before);
        return false;
    }

    t[table_used].key = r->key;
    t[table_used].mtime = r->mtime;
    t[table_used].size = r->size;
    t[table_used].ordinal = (uint32_t)table_used;
    table_used++;
    table_sorted = false;

    /* The header counts what is there and is rewritten per record, so a run
     * that ends without warning leaves a file that reads back correctly.
     *
     * The flush is every sixteenth, not every one. A hard power cut is the
     * only thing that outruns the file system's own cache, and it costs at
     * most sixteen tracks, which are simply measured again. Flushing per
     * track would wake the disk twenty thousand times for that. */
    header_init(&h, (uint32_t)table_used);
    lseek(part_fd, 0, SEEK_SET);
    write(part_fd, &h, sizeof (h));

    if ((table_used & 15) == 0)
        fsync(part_fd);

    return true;
}

int sound_index_count(void)
{
    return table_used;
}

int sound_index_finish(void)
{
    struct sound_header h;
    struct sound_record r;
    struct key_entry *t = table();
    int out_fd;
    int i;

    if (part_fd < 0 || t == NULL)
        return SOUND_ERR_IO;

    if (!table_sorted)
    {
        qsort(t, (size_t)table_used, sizeof (*t), key_cmp);
        table_sorted = true;
    }

    out_fd = open(SOUND_FILE ".new", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0)
        return SOUND_ERR_IO;

    header_init(&h, (uint32_t)table_used);
    if (write(out_fd, &h, sizeof (h)) != (ssize_t)sizeof (h))
    {
        close(out_fd);
        remove(SOUND_FILE ".new");
        return SOUND_ERR_IO;
    }

    /* One seek and one record at a time, from the working file. The whole
     * set would be 800K at twenty thousand tracks and taking that from core
     * stops playback; the seeks cost a few seconds once, at the end of a scan
     * that took hours. */
    for (i = 0; i < table_used; i++)
    {
        off_t at = (off_t)sizeof (h)
                   + (off_t)t[i].ordinal * (off_t)sizeof (r);

        if (lseek(part_fd, at, SEEK_SET) != at ||
            read(part_fd, &r, sizeof (r)) != (ssize_t)sizeof (r) ||
            write(out_fd, &r, sizeof (r)) != (ssize_t)sizeof (r))
        {
            close(out_fd);
            remove(SOUND_FILE ".new");
            return SOUND_ERR_IO;
        }
    }

    fsync(out_fd);
    close(out_fd);

    /* In place only once the replacement is whole: a scan of this length must
     * not be able to destroy the previous index by failing near the end. */
    remove(SOUND_FILE);
    if (rename(SOUND_FILE ".new", SOUND_FILE) < 0)
        return SOUND_ERR_IO;

    sound_index_close();
    remove(SOUND_PART);

    return SOUND_OK;
}

void sound_index_close(void)
{
    if (part_fd >= 0)
    {
        close(part_fd);
        part_fd = -1;
    }

    if (table_handle > 0)
    {
        core_free(table_handle);
        table_handle = 0;
    }

    table_used = 0;
    table_max = 0;
}

bool sound_index_partial(int *done)
{
    struct sound_header h;
    int fd = open(SOUND_PART, O_RDONLY);
    bool ok = false;

    if (fd < 0)
        return false;

    if (read(fd, &h, sizeof (h)) == (ssize_t)sizeof (h) && header_ok(&h) &&
        h.count > 0)
    {
        if (done != NULL)
            *done = (int)h.count;
        ok = true;
    }

    close(fd);

    return ok;
}

bool sound_index_exists(void)
{
    return file_exists(SOUND_FILE);
}


/** Reading **/

int sound_index_reader_open(struct sound_index_reader *r)
{
    struct sound_header h;

    r->fd = open(SOUND_FILE, O_RDONLY);
    if (r->fd < 0)
        return SOUND_ERR_NONE;

    if (read(r->fd, &h, sizeof (h)) != (ssize_t)sizeof (h) || !header_ok(&h))
    {
        close(r->fd);
        r->fd = -1;
        return SOUND_ERR_NONE;
    }

    r->count = (int)h.count;

    return SOUND_OK;
}

void sound_index_reader_close(struct sound_index_reader *r)
{
    if (r->fd >= 0)
        close(r->fd);

    r->fd = -1;
    r->count = 0;
}

bool sound_index_read(struct sound_index_reader *r, int n,
                      struct sound_record *out)
{
    off_t at;

    if (r->fd < 0 || n < 0 || n >= r->count)
        return false;

    at = (off_t)sizeof (struct sound_header)
         + (off_t)n * (off_t)sizeof (*out);

    if (lseek(r->fd, at, SEEK_SET) != at)
        return false;

    return read(r->fd, out, sizeof (*out)) == (ssize_t)sizeof (*out);
}

bool sound_index_find(struct sound_index_reader *r, uint64_t key,
                      struct sound_record *out)
{
    int lo = 0;
    int hi = r->count - 1;

    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;

        if (!sound_index_read(r, mid, out))
            return false;

        if (out->key == key)
            return true;

        if (out->key < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    return false;
}
