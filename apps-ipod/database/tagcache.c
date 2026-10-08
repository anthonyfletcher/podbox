/***************************************************************************
 * Original code from RockBox
 * was: apps/tagcache.c
 * Copyright (C) 2005 by Miika Pekkarinen
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * The tag database itself: builds and loads the on-disk index, and answers
 * searches over it. The largest file in apps/.
 *
 * The diagram below is the component view; this is where those components
 * live in the file, in order:
 *   - on-disk structs, byte-swapping, and the typed read/write helpers
 *   - opening the master and per-tag database files
 *   - the temporary buffer used during commit, and the yield helper
 *   - lookup: finding an entry by filename, through the path index in RAM
 *     or by scanning the filename file on disk
 *   - search: running a query's clauses and retrieving matching tags
 *   - modification: writing tags back, and deleting entries
 *   - the builder: scanning the tree into a temporary DB
 *   - commit: sorting, uniquing and writing the real index
 *   - the RAM DB loader, and the control thread that drives build/commit
 *
 * Things that shape the whole file:
 *   - There is one file per tag plus a master index. A "seek" in a search
 *     result is a byte offset into one of those files, not a record number.
 *   - Everything on disk is written in the target's native byte order and
 *     swapped on read if the header says otherwise, which is why the typed
 *     read and write helpers exist rather than plain read() and write().
 *   - Building the database is long and must not block playback, so it runs
 *     on its own thread and calls do_timed_yield() throughout. Anything added
 *     to the build or commit path needs to keep doing that.
 *   - The RAM DB is optional: when it is loaded, lookups go through memory
 *     (find_entry_ram) and otherwise through the files (find_entry_disk).
 *     Most search code has to work either way.
 ****************************************************************************/

/*
 *                    TagCache API
 *
 *       ----------x---------x------------------x-----
 *                 |         |                  |              External
 * +---------------x-------+ |       TagCache   |              Libraries
 * | Modification routines | |         Core     |
 * +-x---------x-----------+ |                  |
 *   | (R/W)   |             |                  |           |
 *   |  +------x-------------x-+  +-------------x-----+     |
 *   |  |                      x==x Filters & clauses |     |
 *   |  | Search routines      |  +-------------------+     |
 *   |  |                      x============================x DirCache
 *   |  +-x--------------------+                            | (optional)
 *   |    | (R)                                             |
 *   |    | +-------------------------------+  +---------+  |
 *   |    | | DB Commit (sort,unique,index) |  |         |  |
 *   |    | +-x--------------------------x--+  | Control |  |
 *   |    |   | (R/W)                    | (R) | Thread  |  |
 *   |    |   | +----------------------+ |     |         |  |
 *   |    |   | | TagCache DB Builder  | |     +---------+  |
 *   |    |   | +-x-------------x------+ |                  |
 *   |    |   |   | (R)         | (W)    |                  |
 *   |    |   |   |          +--x--------x---------+        |
 *   |    |   |   |          | Temporary Commit DB |        |
 *   |    |   |   |          +---------------------+        |
 * +-x----x-------x--+                                      |
 * | TagCache RAM DB x==\(W) +-----------------+            |
 * +-----------------+   \===x                 |            |
 *   |    |   |   |      (R) |  Ram DB Loader  x============x DirCache
 * +-x----x---x---x---+   /==x                 |            | (optional)
 * | Tagcache Disk DB x==/   +-----------------+            |
 * +------------------+                                     |
 *
 */


/*#define LOGF_ENABLE*/
/*#define LOGF_CLAUSES define to enable logf clause matching (LOGF_ENABLE req'd) */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include "config.h"
#include "system/library_files.h"
#include "database/libfile.h"
#include "ata_idle_notify.h"
#include "thread.h"
#include "kernel.h"
#include "system.h"
#include "logf.h"
#include "string-extra.h"
#include "usb.h"
#include "metadata.h"
#include "tagcache.h"
#include "database/path_key.h"
#include "database/sound_index.h"
#include "db_spoken.h"
#include "metadata/art_cache.h"   /* art_cache_dir_hash */
#include "core_alloc.h"
#include "audio.h"
#include "crc32.h"
#include "system/strutil.h"
#include "settings/settings.h"
#include "system/debug_log.h"
#include "dir.h"
#include "pathfuncs.h"
#include "debug.h"
#include "dircache.h"
#include "errno.h"

#include "lang.h"
#include "widgets/splash.h"
#include "eeprom_settings.h"
/* Whether the commit in progress should give up. It is honoured up to the
 * swap: until then the merge writes only .new files and the filename file's
 * tail, which commit() cuts back, so a cancel leaves the database as it was
 * and TAGCACHE_FILE_TEMP stays for the retry. Past the swap the commit
 * finishes, cancel or not. */
static bool usr_cancel(void);
#define USR_CANCEL usr_cancel()
/*
 * Define this to support non-native endian tagcache files.
 * Databases are always written in native endian so this is
 * basically only necessary to support databases generated
 * by the PC database tool.
 *
 * Adds around 0.5-1.0k of code.
 */
#define TAGCACHE_SUPPORT_FOREIGN_ENDIAN

/* Allow a little drift to the filename ordering (should not be too high/low). */
#define POS_HISTORY_COUNT 4

/* How much to pre-load entries while committing to prevent seeking. */
#define IDX_BUF_DEPTH 64

/* Tag Cache Header version 'TCHxx'. Increment when changing internal structures. */
#define TAGCACHE_MAGIC  0x54434810

/* How much to allocate extra space for ramcache. */
#define TAGCACHE_RESERVE 32768

/* Minimum memory to leave free for the audio buffer when allocating
 * tagcache tempbuf or ramcache. Prevents OOM panics in playback. */
#define TAGCACHE_MIN_AUDIO_RESERVE (1024 * 1024) /* 1 MB */

/*
 * Define how long one entry must be at least (longer -> less memory at commit).
 * Must be at least 4 bytes in length for correct alignment.
 */
#define TAGFILE_ENTRY_CHUNK_LENGTH   8

/* Used to guess the necessary buffer size at commit. */
#define TAGFILE_ENTRY_AVG_LENGTH   16

/* Max events in the internal tagcache command queue. */
#define TAGCACHE_COMMAND_QUEUE_LENGTH 32

/* Idle time before committing events in the command queue. */
#define TAGCACHE_COMMAND_QUEUE_COMMIT_DELAY  HZ*2

/* Dont commit database_tmp data. */
#define TAGCACHE_FILE_NOCOMMIT  "database_commit.ignore"

/* Temporary database containing new tags to be committed to the main db. */
#define TAGCACHE_FILE_TEMP       "database_tmp.tcd"

/* Leads the stamp a commit writes past the temp file's last entry: the
 * commit id its merge produces. */
#define TAGCACHE_TEMP_STAMP      0x54435354

/* The main database master index and numeric data. */
#define TAGCACHE_FILE_MASTER     "database_idx.tcd"

/* The main database string data. */
#define TAGCACHE_FILE_INDEX      "database_%d.tcd"

/* Present while a commit's .new files are being renamed into place */
#define TAGCACHE_FILE_SWAP       "database_swap.tcd"


/* Flags */
#define FLAG_DELETED     0x0001  /* Entry has been removed from db */
#define FLAG_DIRTYNUM    0x0004  /* Numeric data has been modified */
#define FLAG_TRKNUMGEN   0x0008  /* Track number has been generated  */
#define FLAG_RESURRECTED 0x0010  /* Statistics data has been resurrected */


#define DB_LOG(kind, msg) debug_log(DEBUG_LOG_TAGCACHE, kind ": %s", msg)

/* Tag Cache thread. */
static struct event_queue tagcache_queue SHAREDBSS_ATTR;
static long tagcache_stack[(DEFAULT_STACK_SIZE + 0x4000)/sizeof(long)];
static const char tagcache_thread_name[] = "tagcache";

/* Previous path when scanning directory tree recursively. */
static char curpath[TAGCACHE_BUFSZ];
/* Shared buffer for several build_index fns to reduce stack usage */
static char build_idx_buf[TAGCACHE_BUFSZ];
static const long build_idx_bufsz = sizeof(build_idx_buf);
/* Used when removing duplicates. */
static char *tempbuf;     /* Allocated when needed. */
static long tempbufidx;   /* Current location in buffer. */
static size_t tempbuf_size; /* Buffer size (TEMPBUF_SIZE). */
static long tempbuf_left; /* Buffer space left. */
static long tempbuf_pos;
static int tempbuf_handle;

#define SORTED_TAGS_COUNT 9
#define TAGCACHE_IS_UNIQUE(tag) (BIT_N(tag) & TAGCACHE_UNIQUE_TAGS)
#define TAGCACHE_IS_SORTED(tag) (BIT_N(tag) & TAGCACHE_SORTED_TAGS)
#define TAGCACHE_IS_NUMERIC_OR_NONUNIQUE(tag) \
    (BIT_N(tag) & (TAGCACHE_NUMERIC_TAGS | ~TAGCACHE_UNIQUE_TAGS))
/* Tags we want to get sorted (loaded to the tempbuf). */
#define TAGCACHE_SORTED_TAGS ((1LU << tag_artist) | (1LU << tag_album) | \
    (1LU << tag_genre) | (1LU << tag_composer) | (1LU << tag_comment) | \
    (1LU << tag_albumartist) | (1LU << tag_grouping) | (1LU << tag_title) | \
    (1LU << tag_virt_canonicalartist))

/* Uniqued tags (we can use these tags with filters and conditional clauses). */
#define TAGCACHE_UNIQUE_TAGS ((1LU << tag_artist) | (1LU << tag_album) | \
    (1LU << tag_genre) | (1LU << tag_composer) | (1LU << tag_comment) | \
    (1LU << tag_albumartist) | (1LU << tag_grouping) | \
    (1LU << tag_virt_canonicalartist))

/* String presentation of the tags defined in tagcache.h. Must be in correct order! */
static const char * const tags_str[] = { "artist", "album", "genre", "title",
    "filename", "composer", "comment", "albumartist", "grouping", "year",
    "discnumber", "tracknumber", "canonicalartist", "bitrate", "length",
    "playcount", "rating", "playtime", "lastplayed", "commitid", "mtime",
    "lastelapsed", "lastoffset"
#if !defined(LOGF_ENABLE) || !defined(LOGF_CLAUSES)
};
#define logf_clauses(...) do { } while(0)
#else /* strings for logf debugging */
    "tag_virt_basename", "tag_virt_length_min", "tag_virt_length_sec",
    "tag_virt_playtime_min", "tag_virt_playtime_sec",
    "tag_virt_entryage", "tag_virt_autoscore", "tag_virt_spoken"
};
/* more debug strings */
static const char * const tag_type_str[] = {
    [clause_none] = "clause_none", [clause_is] = "clause_is",
    [clause_is_not] = "clause_is_not", [clause_gt] = "clause_gt",
    [clause_gteq] = "clause_gteq", [clause_lt] = "clause_lt",
    [clause_lteq] = "clause_lteq", [clause_contains] = "clause_contains",
    [clause_not_contains] = "clause_not_contains",
    [clause_begins_with] = "clause_begins_with",
    [clause_not_begins_with] = "clause_not_begins_with",
    [clause_ends_with] = "clause_ends_with",
    [clause_not_ends_with] = "clause_not_ends_with",
    [clause_oneof] = "clause_oneof",
    [clause_contains_oneof] = "clause_contains_oneof",
    [clause_begins_oneof] = "clause_begins_oneof",
    [clause_ends_oneof] = "clause_ends_oneof",
    [clause_not_oneof] = "clause_not_oneof",
    [clause_not_contains_oneof] = "clause_not_contains_oneof",
    [clause_not_begins_oneof] = "clause_not_begins_oneof",
    [clause_not_ends_oneof] = "clause_not_ends_oneof",
    [clause_logical_or] = "clause_logical_or"
 };
#define logf_clauses logf
#endif /* !defined(LOGF_ENABLE) || !defined(LOGF_CLAUSES) */


/* Status information of the tagcache. */
static struct tagcache_stat tc_stat;

/* Queue commands. */
enum tagcache_queue {
    Q_STOP_SCAN = 0,
    Q_START_SCAN,
    Q_IMPORT_CHANGELOG,
    Q_UPDATE,
    Q_REBUILD,
    Q_RELOAD_RAMCACHE,

    /* Internal tagcache command queue. */
    CMD_UPDATE_MASTER_HEADER,
    CMD_UPDATE_NUMERIC,
};

struct tagcache_command_entry {
    int32_t command;
    int32_t idx_id;
    int32_t tag;
    int32_t data;
};

static struct tagcache_command_entry command_queue[TAGCACHE_COMMAND_QUEUE_LENGTH];
static volatile int command_queue_widx = 0;
static volatile int command_queue_ridx = 0;
static struct mutex command_queue_mutex SHAREDBSS_ATTR;

/* Moves whenever entry numbers stop meaning what they did: remove_files()
 * bumps it. */
static volatile uint32_t db_generation;
/* Set once boot has loaded the skins; the boot scan waits for it. */
static volatile bool boot_finished;

/* Tag database structures. */

/* Variable-length tag entry in tag files. */
struct tagfile_entry {
    int32_t tag_length;  /* Length of the data in bytes including '\0' */
    int32_t idx_id;      /* Corresponding entry location in index file of not unique tags */
    char tag_data[0];  /* Begin of the tag data */
};

/* Fixed-size tag entry in master db index. */
struct index_entry {
    int32_t tag_seek[TAG_COUNT]; /* Location of tag data or numeric tag data */
    int32_t flag;                /* Status flags */
};

/* Header is the same in every file. */
struct tagcache_header {
    int32_t magic;       /* Header version number */
    int32_t datasize;    /* Data size in bytes */
    int32_t entry_count; /* Number of entries in this file */
};

struct master_header {
    struct tagcache_header tch;
    int32_t serial; /* Increasing counting number */
    int32_t commitid; /* Number of commits so far */
    int32_t dirty;
};

static struct master_header current_tcmh;


#define TC_ALIGN_PTR(p, type, gap_out_p) \
    ({ typeof (p) __p = (p);                                  \
       typeof (p) __palgn = ALIGN_UP(__p, __alignof__(type)); \
       *(gap_out_p) = (char *)__palgn - (char *)__p;          \
       __palgn; })

/* One live entry of the path index: path_key() of its filename, split so the
 * slot is 12 bytes rather than padded to 16. */
struct path_slot {
    uint32_t key_lo;
    uint32_t key_hi;
    int32_t  idx_id;
};

/* Header is created when loading database to ram. */
struct ramcache_header {
    char *tags[TAG_COUNT];       /* Tag file content; tag_filename's header only */
    int entry_count[TAG_COUNT];  /* Number of entries in the indices. */
    long tag_size[TAG_COUNT];    /* Bytes in each tags[], to bound a seek */
    int path_count;              /* Slots in the path index */
    int path_first;              /* Its byte offset from this header */
    int album_count;             /* Rows in the album table, 0 for none */
    int album_first;             /* Its byte offset from this header */
    int artist_count;
    int artist_first;
    struct index_entry indices[0]; /* Master index file content */
};

/* The path index, sorted by key. An offset rather than a pointer so a move of
 * the allocation needs nothing fixing. */
#define tcrc_path_slots \
    ((struct path_slot *)((char *)tcramcache.hdr + tcramcache.hdr->path_first))


/* In-RAM ramcache structure (not persisted) */
static struct tcramcache
{
    struct ramcache_header *hdr;      /* allocated ramcache_header */
    int handle;                       /* buffer handle */
    bool current;   /* holds the files as they are; a merge clears it */
} tcramcache;

static inline void tcrc_buffer_lock(void)
{
    core_pin(tcramcache.handle);
}

static inline void tcrc_buffer_unlock(void)
{
    core_unpin(tcramcache.handle);
}


/**
 * Full tag entries stored in a temporary file waiting
 * for commit to the cache. */
struct temp_file_entry {
    int32_t tag_offset[TAG_COUNT];
    int16_t tag_length[TAG_COUNT];
    int32_t flag;
    int32_t data_length;
};

struct tempbuf_id_list {
    long id;
    struct tempbuf_id_list *next;
};

struct tempbuf_searchidx {
    long idx_id;
    char *str;
    int seek;
    struct tempbuf_id_list idlist;
};

/* Lookup buffer for fixing messed up index while after sorting. */
static long commit_entry_count;

/* What build_index() leaves for merge_master(), which writes the master once
 * for every tag instead of each tag rewriting it in turn. For each sorted tag,
 * old seek / TAGFILE_ENTRY_CHUNK_LENGTH -> the seek in the new file (-1 for a
 * string no longer there); for every string tag, new entry i -> its seek.
 * Carved from the front of tempbuf for the length of a commit. */
static struct
{
    int32_t *remap[TAG_COUNT];
    long remap_len[TAG_COUNT];
    int32_t *newseek[TAG_COUNT];
} merge;
static long lookup_buffer_depth;
static struct tempbuf_searchidx **lookup;

/* Used when building the temporary file. */
static int cachefd = -1, filenametag_fd;
static int total_entry_count = 0;
static int data_size = 0;
static int processed_dir_count;

/* Thread safe locking */
static volatile int write_lock;
static volatile int read_lock;

/* Sticky for the length of one commit. Trap: USR_CANCEL is asked again at the
 * guard around the master header update, long after the loops have fallen out.
 * Answered live it could read false there and write the header over an
 * incomplete index. */
static bool commit_cancelled;

static bool delete_entry(long idx_id);

static inline void str_setlen(char *buf, size_t len)
{
    buf[len] = '\0';
}

const char* tagcache_tag_to_str(int tag)
{
    return tags_str[tag];
}

static void swap_tagfile_entry(struct tagfile_entry *buf)
{
    if (tc_stat.econ)
    {
        buf->tag_length = swap32(buf->tag_length);
        buf->idx_id = swap32(buf->idx_id);
    }
}

static void swap_index_entry(struct index_entry *buf)
{
    if (tc_stat.econ)
    {
        for (int i = 0; i < TAG_COUNT; ++i)
            buf->tag_seek[i] = swap32(buf->tag_seek[i]);
        buf->flag = swap32(buf->flag);
    }
}

static void swap_tagcache_header(struct tagcache_header *buf)
{
    if (tc_stat.econ)
    {
        buf->magic = swap32(buf->magic);
        buf->datasize = swap32(buf->datasize);
        buf->entry_count = swap32(buf->entry_count);
    }
}

static void swap_master_header(struct master_header *buf)
{
    if (tc_stat.econ)
    {
        swap_tagcache_header(&buf->tch);
        buf->serial = swap32(buf->serial);
        buf->commitid = swap32(buf->commitid);
        buf->dirty = swap32(buf->dirty);
    }
}

static ssize_t read_tagfile_entry(int fd, struct tagfile_entry *buf)
{
    ssize_t ret = read(fd, buf, sizeof(*buf));
    if (ret == sizeof(*buf) && tc_stat.econ)
        swap_tagfile_entry(buf);

    return ret;
}

static ssize_t write_tagfile_entry(int fd, struct tagfile_entry *buf)
{
    struct tagfile_entry e = *buf;

    swap_tagfile_entry(&e);

    return write(fd, &e, sizeof(e));
}

enum e_read_errors {
    e_SUCCESS = 0,
    e_SUCCESS_LEN_ZERO = 1,
    e_ENTRY_SIZEMISMATCH,
    e_TAG_TOOLONG,
    e_TAG_SIZEMISMATCH
};

static enum e_read_errors
read_tagfile_entry_and_tag(int fd, struct tagfile_entry *tfe,
                           char* buf, int bufsz)
{
    if (read_tagfile_entry(fd, tfe) != sizeof(struct tagfile_entry))
        return e_ENTRY_SIZEMISMATCH;

    long tag_length = tfe->tag_length;
    if (tag_length < 0 || tag_length >= bufsz)
        return e_TAG_TOOLONG;

    if (tag_length > 0 && read(fd, buf, tag_length) != tag_length)
        return e_TAG_SIZEMISMATCH;

    str_setlen(buf, tag_length);
    return (tag_length > 0 && *buf) ? e_SUCCESS : e_SUCCESS_LEN_ZERO;
}

static ssize_t read_index_entries(int fd, struct index_entry *buf, size_t count)
{
    ssize_t ret = read(fd, buf, sizeof(*buf) * count);
    for (ssize_t i = 0; i < ret; i += sizeof(*buf))
        swap_index_entry(buf++);

    return ret;
}

static ssize_t write_index_entries(int fd, struct index_entry *buf, size_t count)
{
    /* One write for the batch. Through the one-sector file cache, a write
     * per 96-byte entry turns every sector into a read and a write of its
     * own; a batch goes out as whole sectors. */
    if (!tc_stat.econ)
        return write(fd, buf, sizeof(*buf) * count);

    ssize_t ret = 0;
    for (; count > 0; count--)
    {
        struct index_entry e = *buf++;
        swap_index_entry(&e);

        ssize_t rc = write(fd, &e, sizeof(e));
        if (rc < 0)
            return rc;
        ret += rc;
    }

    return ret;
}

static ssize_t read_tagcache_header(int fd, struct tagcache_header *buf)
{
    ssize_t ret = read(fd, buf, sizeof(*buf));
    if (ret == sizeof(*buf))
        swap_tagcache_header(buf);

    return ret;
}

static ssize_t write_tagcache_header(int fd, struct tagcache_header *buf)
{
    struct tagcache_header e = *buf;
    swap_tagcache_header(&e);
    return write(fd, &e, sizeof(e));
}

static ssize_t read_master_header(int fd, struct master_header *buf)
{
    ssize_t ret = read(fd, buf, sizeof(*buf));
    if (ret == sizeof(*buf))
        swap_master_header(buf);

    return ret;
}

static ssize_t write_master_header(int fd, struct master_header *buf)
{
    struct master_header e = *buf;
    swap_master_header(&e);
    return write(fd, &e, sizeof(e));
}

/*
 * open_db_fd and remove_db_file are noinline to minimize stack usage
 */
static int NO_INLINE open_db_fd(const char* filename, int mode)
{
    char buf[MAX_PATH];

    if(mode & O_CREAT)
    {
        if (mkdir(tc_stat.db_path) < 0 && errno != EEXIST)
            return -1;
    }

    return open_pathfmt(buf, sizeof(buf), mode, "%s/%s",
                        tc_stat.db_path, filename);
}

static int NO_INLINE remove_db_file(const char* filename)
{
    char buf[MAX_PATH];

    snprintf(buf, sizeof(buf), "%s/%s",
             tc_stat.db_path, filename);

    return remove(buf);
}

static bool NO_INLINE rename_db_file(const char *from, const char *to)
{
    char src[MAX_PATH];
    char dst[MAX_PATH];

    snprintf(src, sizeof(src), "%s/%s", tc_stat.db_path, from);
    snprintf(dst, sizeof(dst), "%s/%s", tc_stat.db_path, to);

    return rename(src, dst) == 0;
}

static int open_tag_fd(struct tagcache_header *hdr, int tag, bool write)
{
    int fd;
    char fname[MAX_PATH];

    if (TAGCACHE_IS_NUMERIC(tag) || tag < 0 || tag >= TAG_COUNT)
        return -1;

    fd = open_pathfmt(fname, sizeof(fname),
                      write ? O_RDWR : O_RDONLY, "%s/" TAGCACHE_FILE_INDEX,
                      tc_stat.db_path, tag);
    if (fd < 0)
    {
        logf("%s failed: tag=%d write=%d file= " TAGCACHE_FILE_INDEX,
             __func__, tag, write, tag);
        tc_stat.ready = false;
        return fd;
    }

    /* Check the header. */
    if (read_tagcache_header(fd, hdr) != sizeof(struct tagcache_header) ||
        hdr->magic != TAGCACHE_MAGIC)
    {
        logf("header error");
        tc_stat.ready = false;
        close(fd);
        return -2;
    }

    return fd;
}

static int open_master_file(const char *name, struct master_header *hdr,
                            bool write)
{
    int fd;
    int rc;

    fd = open_db_fd(name, write ? O_RDWR : O_RDONLY);
    if (fd < 0)
    {
        logf("master file open failed for R/W");
        tc_stat.ready = false;
        return fd;
    }

    rc = read(fd, hdr, sizeof(struct master_header));
    if (rc != sizeof(struct master_header))
    {
        logf("master file read failed");
        close(fd);
        return -1;
    }

    /* Tagcache files can have either endianness. A device will always
     * create files in its native endianness, but we accept non-native
     * endian files for compatibility reasons. */
    if (hdr->tch.magic == TAGCACHE_MAGIC)
        tc_stat.econ = false;
    else if (hdr->tch.magic == swap32(TAGCACHE_MAGIC))
    {
        tc_stat.econ = true;
        swap_master_header(hdr);
    }
    else
    {
        logf("master file bad magic: %08lx\n", (unsigned long)hdr->tch.magic);
        close(fd);
        return -2;
    }

    return fd;
}

static int open_master_fd(struct master_header *hdr, bool write)
{
    return open_master_file(TAGCACHE_FILE_MASTER, hdr, write);
}

static void remove_files(void)
{
    int i;
    char buf[MAX_PATH];
    const int bufsz = sizeof(buf);
    logf("%s", __func__);

    tc_stat.ready = false;
    tc_stat.ramcache = false;
    tc_stat.econ = false;
    db_generation++;
    remove_db_file(TAGCACHE_FILE_MASTER);
    for (i = 0; i < TAG_COUNT; i++)
    {
        if (TAGCACHE_IS_NUMERIC(i))
            continue;

        snprintf(buf, bufsz, "%s/" TAGCACHE_FILE_INDEX,
                 tc_stat.db_path, i);
        remove(buf);
    }
}

static bool check_all_headers(void)
{
    struct master_header myhdr;
    struct tagcache_header tch;
    int tag;
    int fd;

    if ( (fd = open_master_fd(&myhdr, false)) < 0)
        return false;

    close(fd);
    if (myhdr.dirty)
    {
        logf("tagcache is dirty!");
        return false;
    }

    memcpy(&current_tcmh, &myhdr, sizeof(struct master_header));

    for (tag = 0; tag < TAG_COUNT; tag++)
    {
        if (TAGCACHE_IS_NUMERIC(tag))
            continue;

        if ( (fd = open_tag_fd(&tch, tag, false)) < 0)
            return false;

        close(fd);
    }

    return true;
}

static bool update_master_header(void)
{
    struct master_header myhdr;
    int fd;

    if (!tc_stat.ready)
        return false;

    if ( (fd = open_master_fd(&myhdr, true)) < 0)
        return false;

    myhdr.serial = current_tcmh.serial;
    myhdr.commitid = current_tcmh.commitid;
    myhdr.dirty = current_tcmh.dirty;

    /* Write it back */
    lseek(fd, 0, SEEK_SET);
    write_master_header(fd, &myhdr);
    close(fd);

    return true;
}

static bool do_timed_yield(void)
{
    /* Sorting can lock up for quite a while, so yield occasionally */
    static long wakeup_tick = 0;
    if (TIME_AFTER(current_tick, wakeup_tick))
    {
        yield();
        wakeup_tick = current_tick + (HZ/25);
        return true;
    }
    return false;
}

static void allocate_tempbuf(void)
{
    /* Yeah, malloc would be really nice now :) */
    size_t size;
    tempbuf_size = 0;

    /* Need to pass dummy ops to prevent the buffer being moved
     * out from under us, since we yield during the tagcache commit.
     *
     * Don't grab ALL memory -- leave a reserve so audio_reset_buffer()
     * can still allocate if playback starts during a commit. Without
     * this, core_alloc_maximum() + buflib_ops_locked starves the audio
     * buffer and causes an OOM panic. */
    size = core_allocatable();
    if (size > TAGCACHE_MIN_AUDIO_RESERVE)
    {
        size -= TAGCACHE_MIN_AUDIO_RESERVE;
        tempbuf_handle = core_alloc_ex(size, &buflib_ops_locked);
        if (tempbuf_handle > 0)
        {
            tempbuf = core_get_data(tempbuf_handle);
            tempbuf_size = size;
        }
    }

}

static void free_tempbuf(void)
{
    if (tempbuf_size == 0)
        return ;

    tempbuf_handle = core_free(tempbuf_handle);
    tempbuf = NULL;
    tempbuf_size = 0;
}

/* Path index lookups since boot, for the database info screen. */
static volatile int path_found, path_missed;

static int path_slot_cmp(const void *a, const void *b)
{
    const struct path_slot *x = a, *y = b;

    if (x->key_hi != y->key_hi)
        return x->key_hi < y->key_hi ? -1 : 1;
    if (x->key_lo != y->key_lo)
        return x->key_lo < y->key_lo ? -1 : 1;
    return x->idx_id - y->idx_id;
}

/* ------------------------------------------------------------------ *
 * the album and artist tables                                        *
 * ------------------------------------------------------------------ */

#define tcrc_albums \
    ((struct tagcache_album *)((char *)tcramcache.hdr \
                               + tcramcache.hdr->album_first))
#define tcrc_artists \
    ((struct tagcache_artist *)((char *)tcramcache.hdr \
                                + tcramcache.hdr->artist_first))

/* Rows the tables may need, for sizing the RAM copy: an album per album
 * name, and one more per album artist for names they share, never more than
 * the tracks */
static size_t album_tables_size(int entries)
{
    struct tagcache_header h;
    int albums = 0, artists = 0;
    int fd;

    if ((fd = open_tag_fd(&h, tag_album, false)) >= 0)
    {
        albums = h.entry_count;
        close(fd);
    }
    if ((fd = open_tag_fd(&h, tag_albumartist, false)) >= 0)
    {
        artists = h.entry_count;
        close(fd);
    }
    return MIN(entries, albums + artists) * sizeof(struct tagcache_album)
           + artists * sizeof(struct tagcache_artist) + 16;
}

static int album_entry_cmp(const void *a, const void *b)
{
    const struct index_entry *x = &tcramcache.hdr->indices[*(const int32_t *)a];
    const struct index_entry *y = &tcramcache.hdr->indices[*(const int32_t *)b];

    if (x->tag_seek[tag_album] != y->tag_seek[tag_album])
        return x->tag_seek[tag_album] < y->tag_seek[tag_album] ? -1 : 1;
    if (x->tag_seek[tag_albumartist] != y->tag_seek[tag_albumartist])
        return x->tag_seek[tag_albumartist] < y->tag_seek[tag_albumartist]
               ? -1 : 1;
    return *(const int32_t *)a - *(const int32_t *)b;
}

static int seek_cmp(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;

    return x < y ? -1 : x > y;
}

static int album_row(long album_seek, long artist_seek)
{
    int lo = 0, hi = tcramcache.hdr->album_count - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        const struct tagcache_album *r = &tcrc_albums[mid];

        if (r->album_seek == album_seek && r->artist_seek == artist_seek)
            return mid;
        if (r->album_seek < album_seek
            || (r->album_seek == album_seek && r->artist_seek < artist_seek))
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static int artist_row(long seek)
{
    int lo = 0, hi = tcramcache.hdr->artist_count - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;

        if (tcrc_artists[mid].seek == seek)
            return mid;
        if (tcrc_artists[mid].seek < seek)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static int entry_album_row(int idx_id)
{
    const struct index_entry *e = &tcramcache.hdr->indices[idx_id];

    return album_row(e->tag_seek[tag_album], e->tag_seek[tag_albumartist]);
}

/* During a load, once the master index is in: the album rows and the artist
 * rows, from the master alone, placed at *pp. scratch has room for a word per
 * entry and is free until the filename pass. False, with no tables, when
 * they do not fit; the RAM copy loads without them. */
static bool albums_group(int32_t *scratch, int entries, char **pp,
                         ssize_t *bytesleft)
{
    struct ramcache_header *hdr = tcramcache.hdr;
    struct tagcache_album *rows;
    struct tagcache_artist *arts;
    ssize_t gap;
    char *p;
    int n = 0, nrows = 0, nart = 0;

    hdr->album_count = 0;
    hdr->artist_count = 0;

    for (int i = 0; i < entries; i++)
    {
        if (!(hdr->indices[i].flag & FLAG_DELETED))
            scratch[n++] = i;
    }
    qsort(scratch, n, sizeof(*scratch), album_entry_cmp);
    for (int i = 0; i < n; i++)
    {
        const struct index_entry *a = i ? &hdr->indices[scratch[i - 1]]
                                        : NULL;
        const struct index_entry *b = &hdr->indices[scratch[i]];

        if (!a || a->tag_seek[tag_album] != b->tag_seek[tag_album]
            || a->tag_seek[tag_albumartist] != b->tag_seek[tag_albumartist])
            nrows++;
    }

    p = TC_ALIGN_PTR(*pp, struct tagcache_album, &gap);
    if (*bytesleft < gap + (ssize_t)(nrows * sizeof(*rows)))
        return false;
    rows = (struct tagcache_album *)p;
    memset(rows, 0, nrows * sizeof(*rows));

    for (int i = 0, r = -1; i < n; i++)
    {
        const struct index_entry *e = &hdr->indices[scratch[i]];
        long plays = e->tag_seek[tag_playcount];

        if (r < 0 || rows[r].album_seek != e->tag_seek[tag_album]
            || rows[r].artist_seek != e->tag_seek[tag_albumartist])
        {
            r++;
            rows[r].album_seek = e->tag_seek[tag_album];
            rows[r].artist_seek = e->tag_seek[tag_albumartist];
            rows[r].first = scratch[i];
        }
        rows[r].last = scratch[i];
        rows[r].tracks++;
        if (e->tag_seek[tag_year] > rows[r].year)
            rows[r].year = e->tag_seek[tag_year];
        if (plays > 0)
        {
            rows[r].playcount += plays;
            if (e->tag_seek[tag_lastplayed] > rows[r].lastplayed)
                rows[r].lastplayed = e->tag_seek[tag_lastplayed];
        }
    }

    /* The artists: the distinct album artists of the albums */
    for (int r = 0; r < nrows; r++)
        scratch[r] = rows[r].artist_seek;
    qsort(scratch, nrows, sizeof(*scratch), seek_cmp);
    for (int r = 0; r < nrows; r++)
    {
        if (r == 0 || scratch[r] != scratch[nart - 1])
            scratch[nart++] = scratch[r];
    }

    p += nrows * sizeof(*rows);
    p = TC_ALIGN_PTR(p, struct tagcache_artist, &gap);
    if (*bytesleft < (ssize_t)((char *)p - *pp)
                     + (ssize_t)(nart * sizeof(*arts)))
        return false;
    arts = (struct tagcache_artist *)p;
    memset(arts, 0, nart * sizeof(*arts));
    for (int a = 0; a < nart; a++)
    {
        arts[a].seek = scratch[a];
    }
    p += nart * sizeof(*arts);

    hdr->album_first = (char *)rows - (char *)hdr;
    hdr->album_count = nrows;
    hdr->artist_first = (char *)arts - (char *)hdr;
    hdr->artist_count = nart;
    *bytesleft -= p - *pp;
    *pp = p;
    return true;
}

/* During the filename pass: an album's art keys, from its first track */
static void albums_note_path(int idx_id, const char *filename)
{
    static char dir[TAGCACHE_BUFSZ];
    struct tagcache_album *r;
    char *sep;
    int n;

    if (tcramcache.hdr->album_count == 0
        || (n = entry_album_row(idx_id)) < 0)
        return;
    r = &tcrc_albums[n];
    if (r->first != idx_id)
        return;

    strmemccpy(dir, filename, sizeof(dir));
    sep = strrchr(dir, '/');
    if (!sep || sep == dir)
        return;
    *sep = '\0';                     /* track file -> album folder */
    r->art_hash = art_cache_dir_hash(dir);
    sep = strrchr(dir, '/');
    if (!sep || sep == dir)
        return;
    *sep = '\0';                     /* album -> artist folder */
    r->artist_art_hash = art_cache_dir_hash(dir);
}

/* Once every tag is in: what needs the genres, and the artists' figures */
static void albums_finish(int entries)
{
    struct ramcache_header *hdr = tcramcache.hdr;

    if (hdr->album_count == 0)
        return;

    for (int i = 0; i < entries; i++)
    {
        const struct index_entry *e = &hdr->indices[i];
        const struct tagfile_entry *g;
        int n;

        if ((e->flag & FLAG_DELETED) || e->tag_seek[tag_genre] < 0
            || e->tag_seek[tag_genre] >= hdr->tag_size[tag_genre])
            continue;
        g = (const struct tagfile_entry *)&hdr->tags[tag_genre]
                                                 [e->tag_seek[tag_genre]];
        if (db_spoken_is_spoken_genre(g->tag_data)
            && (n = entry_album_row(i)) >= 0)
            tcrc_albums[n].spoken++;
    }

    for (int r = 0; r < hdr->album_count; r++)
    {
        const struct tagcache_album *al = &tcrc_albums[r];
        int a = artist_row(al->artist_seek);
        struct tagcache_artist *ar;

        if (a < 0)
            continue;
        ar = &tcrc_artists[a];
        ar->albums++;
        if (al->spoken == al->tracks)
            ar->spoken_albums++;
        ar->playcount += al->playcount;
        if (al->lastplayed > ar->lastplayed)
            ar->lastplayed = al->lastplayed;
        if (ar->art_hash == 0)
            ar->art_hash = al->artist_art_hash;
    }
}

int tagcache_album_count(void)
{
    return tc_stat.ramcache ? tcramcache.hdr->album_count : 0;
}

bool tagcache_album_get(int n, struct tagcache_album *out)
{
    if (n < 0 || n >= tagcache_album_count())
        return false;
    *out = tcrc_albums[n];
    return true;
}

int tagcache_album_find(long album_seek, long artist_seek)
{
    return tagcache_album_count() ? album_row(album_seek, artist_seek) : -1;
}

int tagcache_album_find_name(long album_seek)
{
    int lo = 0, hi = tagcache_album_count() - 1, found = -1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;

        if (tcrc_albums[mid].album_seek < album_seek)
            lo = mid + 1;
        else
        {
            if (tcrc_albums[mid].album_seek == album_seek)
                found = mid;
            hi = mid - 1;
        }
    }
    return found;
}

int tagcache_album_of(int idx_id)
{
    if (!tagcache_album_count() || idx_id < 0
        || idx_id >= current_tcmh.tch.entry_count
        || (tcramcache.hdr->indices[idx_id].flag & FLAG_DELETED))
        return -1;
    return entry_album_row(idx_id);
}

int tagcache_artist_count(void)
{
    return tc_stat.ramcache ? tcramcache.hdr->artist_count : 0;
}

bool tagcache_artist_get(int n, struct tagcache_artist *out)
{
    if (n < 0 || n >= tagcache_artist_count())
        return false;
    *out = tcrc_artists[n];
    return true;
}

int tagcache_artist_find(long seek)
{
    return tagcache_artist_count() ? artist_row(seek) : -1;
}

void tagcache_album_played(int idx_id, long serial)
{
    int n = tagcache_album_of(idx_id);
    int a;

    if (n < 0)
        return;
    tcrc_albums[n].playcount++;
    if (serial > tcrc_albums[n].lastplayed)
        tcrc_albums[n].lastplayed = serial;
    a = artist_row(tcrc_albums[n].artist_seek);
    if (a >= 0)
    {
        tcrc_artists[a].playcount++;
        if (serial > tcrc_artists[a].lastplayed)
            tcrc_artists[a].lastplayed = serial;
    }
}

/* The live entry whose filename has this path_key(), or -1. Refuses unless
 * the RAM copy is in use, which is exactly when the index describes the
 * database: load_tagcache() builds it, and a commit turns the RAM copy off
 * before it writes.
 *
 * Nothing here yields, so the allocation cannot move under it. Where two
 * slots share a key -- the same file entered twice -- the first one not
 * deleted since the load is the answer. */
static int path_index_find(uint64_t key)
{
    const uint32_t hi = (uint32_t)(key >> 32), lo = (uint32_t)key;
    const struct path_slot *s;
    int first = 0, last;

    if (!tc_stat.ramcache)
        return -1;

    s = tcrc_path_slots;
    last = tcramcache.hdr->path_count;
    while (first < last)
    {
        int mid = first + (last - first) / 2;

        if (s[mid].key_hi < hi || (s[mid].key_hi == hi && s[mid].key_lo < lo))
            first = mid + 1;
        else
            last = mid;
    }

    for (; first < tcramcache.hdr->path_count
           && s[first].key_hi == hi && s[first].key_lo == lo; first++)
    {
        int idx_id = s[first].idx_id;

        /* A buffer commit() borrowed and USB then switched back on holds
         * scratch here, not entries */
        if (idx_id < 0 || idx_id >= current_tcmh.tch.entry_count)
            continue;
        if (!(tcramcache.hdr->indices[idx_id].flag & FLAG_DELETED))
        {
            path_found++;
            return idx_id;
        }
    }

    path_missed++;
    return -1;
}

/* The RAM copy's entry for a file, through the path index. Works under On and
 * Quick alike: the index needs no dircache. -1 when the file is not in the
 * database or the RAM copy is not in use; find_index() tells the two apart. */
static long find_entry_ram(const char *filename)
{
    return path_index_find(path_key(filename));
}

static long find_entry_disk(const char *filename_raw, bool localfd)
{
    struct tagfile_entry tfe;
    struct tagcache_header tch;
    static long last_pos = -1;
    long pos_history[POS_HISTORY_COUNT];
    unsigned int pos_history_idx = 0;
    unsigned int i;

    char buf[TAGCACHE_BUFSZ];
    const long bufsz = sizeof(buf);

    int fd;
    int pos = -1;
    long idx = -1;

    bool found = false;

    /* Matched as path_key() matches: without a volume specifier, and
     * ignoring case, as FAT does. */
    const char *filename = path_key_strip(filename_raw);

    /* The scan (!localfd) still looks, or a dirty database would have every
     * file added again. */
    if (!tc_stat.ready && localfd)
        return -2;

    fd = filenametag_fd;
    if (fd < 0 || localfd)
    {
        last_pos = -1;
        if ( (fd = open_tag_fd(&tch, tag_filename, false)) < 0)
            return -1;
    }

    check_again:

    if (last_pos > 0) /* pos gets cached to prevent reading from beginning */
        pos = lseek(fd, last_pos, SEEK_SET);
    else /* start back at beginning */
        pos = lseek(fd, sizeof(struct tagcache_header), SEEK_SET);

    long tag_length = strlen(filename) + 1; /* include NULL */

    if (tag_length < bufsz)
    {
        while (true)
        {
            for (i = pos_history_idx-1; i < pos_history_idx; i--)
                pos_history[i+1] = pos_history[i];
            pos_history[0] = pos;

            if (read_tagfile_entry(fd, &tfe) != sizeof(struct tagfile_entry))
            {
                logf("size mismatch find entry");
                break;
            }
            else
            {
                pos += sizeof(struct tagfile_entry) + tfe.tag_length;
                /* don't read the entry unless the length matches */
                if (tfe.tag_length == tag_length)
                {
                    if(read(fd, buf, tfe.tag_length) != tag_length)
                    {
                        logf("read error #2");
                        close(fd);
                        if (!localfd)
                            filenametag_fd = -1;
                        last_pos = -1;
                        return -3;
                    }
                    if (!strncasecmp(filename, buf, tag_length))
                    {
                        last_pos = pos_history[pos_history_idx];
                        found = true;
                        idx = tfe.idx_id;
                        break ;
                    }
                }
                else
                    lseek(fd, pos, SEEK_SET);

            }
        }
        if (pos_history_idx < POS_HISTORY_COUNT - 1)
            pos_history_idx++;
    }

    /* Not found? */
    if (!found)
    {
        if (last_pos > 0) /* start back at the beginning */
        {
            last_pos = -1;
            logf("seek again");
            goto check_again;
        }

        idx = -4;
    }

    if (fd != filenametag_fd || localfd)
        close(fd);

    return idx;
}

/* A miss in a loaded path index is final. Falling through to the disk would
 * read the whole filename file only to agree with it. */
static int find_index(const char *filename)
{
    if (tc_stat.ramcache)
        return find_entry_ram(filename);

    return find_entry_disk(filename, true);
}

bool tagcache_find_index(struct tagcache_search *tcs, const char *filename)
{
    /* NOTE: on ret==true you need to call tagcache_search_finish(tcs) yourself */
    int idx_id;

    if (!tc_stat.ready)
        return false;

    idx_id = find_index(filename);
    if (idx_id < 0)
        return false;

    if (!tagcache_search(tcs, tag_filename))
        return false;

    tcs->entry_count = 0;
    tcs->idx_id = idx_id;

    return true;
}

static bool get_index(int masterfd, int idxid,
                      struct index_entry *idx, bool use_ram)
{
    bool localfd = false;

    if (idxid < 0)
    {
        logf("Incorrect idxid: %d", idxid);
        return false;
    }

    if (tc_stat.ramcache && use_ram)
    {
        if (tcramcache.hdr->indices[idxid].flag & FLAG_DELETED)
            return false;

        *idx = tcramcache.hdr->indices[idxid];
        return true;
    }

    if (masterfd < 0)
    {
        struct master_header tcmh;

        localfd = true;
        masterfd = open_master_fd(&tcmh, false);
        if (masterfd < 0)
            return false;
    }

    lseek(masterfd, idxid * sizeof(struct index_entry)
          + sizeof(struct master_header), SEEK_SET);
    if (read_index_entries(masterfd, idx, 1) != sizeof(struct index_entry))
    {
        logf("read error #3");
        if (localfd)
            close(masterfd);

        return false;
    }

    if (localfd)
        close(masterfd);

    if (idx->flag & FLAG_DELETED)
        return false;

    return true;

    (void)use_ram;
}


static bool write_index(int masterfd, int idxid, struct index_entry *idx)
{
    /* Only the numeric data: the RAM copy's string seeks already match */
    if (tc_stat.ramcache)
    {
        struct index_entry *idx_ram = &tcramcache.hdr->indices[idxid];

        for (int tag = 0; tag < TAG_COUNT; tag++)
        {
            if (TAGCACHE_IS_NUMERIC(tag))
            {
                idx_ram->tag_seek[tag] = idx->tag_seek[tag];
            }
        }

        /* Don't touch the attributes. */
        idx_ram->flag = (idx->flag & 0x0000ffff)
            | (idx_ram->flag & 0xffff0000);
    }

    lseek(masterfd, idxid * sizeof(struct index_entry)
          + sizeof(struct master_header), SEEK_SET);
    if (write_index_entries(masterfd, idx, 1) != sizeof(struct index_entry))
    {
        logf("write error #3");
        logf("idxid: %d", idxid);
        return false;
    }

    return true;
}


static bool open_files(struct tagcache_search *tcs, int tag)
{
    if (tcs->idxfd[tag] < 0)
    {
        char fname[MAX_PATH];
        tcs->idxfd[tag] = open_pathfmt(fname, sizeof(fname),
                                       O_RDONLY, "%s/" TAGCACHE_FILE_INDEX,
                                       tc_stat.db_path, tag);
        if (tcs->idxfd[tag] < 0)
        {
            logf("File not open!");
            return false;
        }
    }

    return true;
}

static bool retrieve(struct tagcache_search *tcs, struct index_entry *idx,
                     int tag, char *buf, long bufsz)
{
    bool success = false;
    bool is_basename = false;
    struct tagfile_entry tfe;
    long seek;

    if (tag == tag_virt_basename)
    {
        tag = tag_filename;
        is_basename = true;
    }

    if (TAGCACHE_IS_NUMERIC(tag))
        goto failure;

    seek = idx->tag_seek[tag];
    if (seek < 0)
    {
        logf("Retrieve failed");
        goto failure;
    }

    /* Filenames are never held in RAM */
    if (tcs->ramsearch && tag != tag_filename)
    {
        struct tagfile_entry *ep =
            (struct tagfile_entry *)&tcramcache.hdr->tags[tag][seek];
        strmemccpy(buf, ep->tag_data, bufsz);
        success = true;
    }

    if (!success && open_files(tcs, tag))
    {
        lseek(tcs->idxfd[tag], seek, SEEK_SET);
        switch (read_tagfile_entry_and_tag(tcs->idxfd[tag], &tfe, buf, bufsz))
        {
            case e_ENTRY_SIZEMISMATCH:
                logf("read error #5");
                break;
            case e_TAG_TOOLONG:
                logf("too long tag #5");
                break;
            case e_TAG_SIZEMISMATCH:
                logf("read error #6");
                break;
            default:
                logf("unknown_error");
                break;
            case e_SUCCESS_LEN_ZERO:
            case e_SUCCESS:
                success = true;
                break;
        }
    }

    if (success)
    {
        if (is_basename)
        {
            char* basename = strrchr(buf, '/');
            if (basename != NULL)
                memmove(buf, basename + 1, strlen(basename)); /* includes NULL */
        }
        return true;
    }

failure:
    str_setlen(buf, 0);
    return false;
}

#define COMMAND_QUEUE_IS_EMPTY (command_queue_ridx == command_queue_widx)

static long tc_find_tag(int tag, int idx_id, const struct index_entry *idx)
{
    if (! COMMAND_QUEUE_IS_EMPTY && TAGCACHE_IS_NUMERIC(tag))
    {
        /* Attempt to find tag data through store-to-load forwarding in
           command queue */
        long result = -1;

        mutex_lock(&command_queue_mutex);

        int ridx = command_queue_widx;

        while (ridx != command_queue_ridx)
        {
            if (--ridx < 0)
                ridx = TAGCACHE_COMMAND_QUEUE_LENGTH - 1;

            if (command_queue[ridx].command == CMD_UPDATE_NUMERIC
                && command_queue[ridx].idx_id == idx_id
                && command_queue[ridx].tag == tag)
            {
                result = command_queue[ridx].data;
                break;
            }
        }

        mutex_unlock(&command_queue_mutex);

        if (result >= 0)
        {
            logf("tc_find_tag: Recovered tag %d value %lX from write queue",
                 tag, (unsigned long) result);
            return result;
        }
    }

    return idx->tag_seek[tag];
}

static inline long sec_in_ms(long ms)
{
    return (ms/1000) % 60;
}

static inline long min_in_ms(long ms)
{
    return (ms/1000) / 60;
}

static long check_virtual_tags(int tag, int idx_id,
                               const struct index_entry *idx)
{
    long data = 0;

    switch (tag)
    {
        case tag_virt_length_sec:
            data = sec_in_ms(tc_find_tag(tag_length, idx_id, idx));
            break;

        case tag_virt_length_min:
            data = min_in_ms(tc_find_tag(tag_length, idx_id, idx));
            break;

        case tag_virt_playtime_sec:
            data = sec_in_ms(tc_find_tag(tag_playtime, idx_id, idx));
            break;

        case tag_virt_playtime_min:
            data = min_in_ms(tc_find_tag(tag_playtime, idx_id, idx));
            break;

        case tag_virt_autoscore:
            if (tc_find_tag(tag_length, idx_id, idx) == 0
                || tc_find_tag(tag_playcount, idx_id, idx) == 0)
            {
                data = 0;
            }
            else
            {
                /* A straight calculus gives:
                     autoscore = 100 * playtime / length / playcout (1)
                   Now, consider the euclidian division of playtime by length:
                     playtime = alpha * length + beta
                   With:
                     0 <= beta < length
                   Now, (1) becomes:
                     autoscore = 100 * (alpha / playcout + beta / length / playcount)
                   Both terms should be small enough to avoid any overflow
                */
                long playtime = tc_find_tag(tag_playtime, idx_id, idx);
                long length = tc_find_tag(tag_length, idx_id, idx);
                long playcount = tc_find_tag(tag_playcount, idx_id, idx);
                data = 100 * (playtime / length) + (100 * (playtime % length)) / length;
                data /= playcount;
            }
            break;

        case tag_virt_spoken:
            data = db_spoken_is_spoken_seek(tc_find_tag(tag_genre, idx_id, idx));
            break;

        /* How many commits before the file has been added to the DB. */
        case tag_virt_entryage:
            data = current_tcmh.commitid
                   - tc_find_tag(tag_commitid, idx_id, idx) - 1;
            break;

        case tag_virt_basename:
            tag = tag_filename; /* return filename; caller handles basename */
            /* FALLTHRU */

        default:
            data = tc_find_tag(tag, idx_id, idx);
    }

    return data;
}

long tagcache_get_numeric(const struct tagcache_search *tcs, int tag)
{
    struct index_entry idx;

    if (!tc_stat.ready)
        return false;

    if (!TAGCACHE_IS_NUMERIC(tag))
        return -1;

    if (!get_index(tcs->masterfd, tcs->idx_id, &idx, true))
        return -2;

    return check_virtual_tags(tag, tcs->idx_id, &idx);
}

/* Several tags of the track under the cursor from one read of its index
 * entry, where tagcache_get_numeric() reads the entry again for each. A string
 * tag gives its seek, the value tagcache_search_add_filter() compares. */
bool tagcache_get_values(const struct tagcache_search *tcs,
                         const int *tags, long *out, int count)
{
    struct index_entry idx;
    int i;

    if (!tc_stat.ready || !get_index(tcs->masterfd, tcs->idx_id, &idx, true))
        return false;

    for (i = 0; i < count; i++)
        out[i] = check_virtual_tags(tags[i], tcs->idx_id, &idx);

    return true;
}

inline static bool str_ends_with(const char *str1, const char *str2)
{
    logf_clauses("%s %s %s", str1, __func__, str2);
    int str_len = strlen(str1);
    int clause_len = strlen(str2);

    if (clause_len > str_len)
        return false;

    return !strcasecmp(&str1[str_len - clause_len], str2);
}

inline static bool str_oneof(const char *str, const char *list)
{
    logf_clauses("%s %s %s", str, __func__, list);
    const char *sep;
    int l, len = strlen(str);

    while (*list)
    {
        sep = strchr(list, '|');
        l = sep ? (intptr_t)sep - (intptr_t)list : (int)strlen(list);
        if ((l==len) && !strncasecmp(str, list, len))
            return true;
        list += sep ? l + 1 : l;
    }

    return false;
}

inline static bool str_begins_ends_oneof(const char *str, const char *list, bool begins)
{
    logf_clauses("%s %s (%s) %s", str, __func__, begins ? "begins" : "ends", list);
    const char *sep;
    int l, p, len = strlen(str);

    while (*list)
    {
        sep = strchr(list, '|');
        l = sep ? (intptr_t)sep - (intptr_t)list : (int)strlen(list);
        p = begins ? 0 : len - l;
        if (l <= len && !strncasecmp(&str[p], list, l))
            return true;
        list += sep ? l + 1 : l;
    }

    return false;
}

inline static bool str_contains_oneof(const char *str, char *list)
{
    logf_clauses("%s %s %s", str, __func__, list);
	const char *sep;
	int l, len = strlen(str);
	
	while (*list)
	{
		sep = strchr(list, '|');
		l = sep ? (intptr_t)sep - (intptr_t)list : (int)strlen(list);
		list[l] = '\0';
		if (l <= len && strcasestr(str, list) != NULL) {
			if (sep) list[l] = '|';
			return true;
		}
		if (sep) list[l] = '|';
		list += sep ? l + 1 : l;
	}
	
	return false;
}

/* An article is skipped only with something after it: "The The" sorts as
 * "The", and a band called "A" stays under A. */
const char *tagcache_skip_article(const char *name)
{
    static const char * const articles[] = { "the ", "a ", "an " };

    for (size_t i = 0; i < ARRAYLEN(articles); i++)
    {
        size_t len = strlen(articles[i]);

        if (!strncasecmp(name, articles[i], len) && name[len] != '\0')
            return name + len;
    }

    return name;
}

const char *tagcache_sort_name(const char *name)
{
    return global_settings.sort_ignore_articles
        ? tagcache_skip_article(name) : name;
}

bool tagcache_tag_skips_articles(int tag)
{
    return tag == tag_artist || tag == tag_album || tag == tag_albumartist
        || tag == tag_virt_canonicalartist;
}

static bool check_against_clause(long numeric, const char *str,
                                 const struct tagcache_search_clause *clause)
{
    if (clause->numeric)
    {
        switch (clause->type)
        {
            case clause_is:
                return numeric == clause->numeric_data;
            case clause_is_not:
                return numeric != clause->numeric_data;
            case clause_gt:
                return numeric > clause->numeric_data;
            case clause_gteq:
                return numeric >= clause->numeric_data;
            case clause_lt:
                return numeric < clause->numeric_data;
            case clause_lteq:
                return numeric <= clause->numeric_data;
            default:
                logf("Incorrect numeric tag: %d", clause->type);
        }
    }
    else
    {
        int type = clause->type & ~CLAUSE_SORT_NAME;

        if (clause->type & CLAUSE_SORT_NAME)
            str = tagcache_sort_name(str);

        switch (type)
        {
            case clause_is:
                return !strcasecmp(clause->str, str);
            case clause_is_not:
                return strcasecmp(clause->str, str);
            case clause_gt:
                return 0>strcasecmp(clause->str, str);
            case clause_gteq:
                return 0>=strcasecmp(clause->str, str);
            case clause_lt:
                return 0<strcasecmp(clause->str, str);
            case clause_lteq:
                return 0<=strcasecmp(clause->str, str);
            case clause_contains:
                return (strcasestr(str, clause->str) != NULL);
            case clause_not_contains:
                return (strcasestr(str, clause->str) == NULL);
            case clause_begins_with:
                return (strcasestr(str, clause->str) == str);
            case clause_not_begins_with:
                return (strcasestr(str, clause->str) != str);
            case clause_ends_with:
                return str_ends_with(str, clause->str);
            case clause_not_ends_with:
                return !str_ends_with(str, clause->str);
            case clause_oneof:
                return str_oneof(str, clause->str);
            case clause_not_oneof:
                return !str_oneof(str, clause->str);
            case clause_ends_oneof:
                /* Fall-Through */
            case clause_begins_oneof:
                return str_begins_ends_oneof(str, clause->str,
                                             type == clause_begins_oneof);
            case clause_not_ends_oneof:
                /* Fall-Through */
            case clause_not_begins_oneof:
                return !str_begins_ends_oneof(str, clause->str,
                                            type == clause_not_begins_oneof);
			case clause_contains_oneof:
				return str_contains_oneof(str, clause->str);
			case clause_not_contains_oneof:
				return !str_contains_oneof(str, clause->str);
            default:
                logf("Incorrect tag: %d", clause->type);
        }
    }

    return false;
}

static bool check_clauses(struct tagcache_search *tcs,
                          struct index_entry *idx,
                          struct tagcache_search_clause **clauses, int count)
{
    int i;

    /* Go through all conditional clauses. */
    for (i = 0; i < count; i++)
    {
        int seek;
        char buf[256];
        const int bufsz = sizeof(buf);
        char *str = buf;
        struct tagcache_search_clause *clause = clauses[i];

        logf_clauses("%s clause %d %s %s [%ld] %s",
            "Checking",  i, tag_type_str[clause->type & ~CLAUSE_SORT_NAME],
            tags_str[clause->tag],  clause->numeric_data,
            (clause->numeric || clause->str == NULL) ? "[NUMERIC?]" : clause->str);

        if (clause->type == clause_logical_or)
        {
            logf_clauses("Bailing");
            break; /* all conditions before logical-or satisfied --
                      stop processing clauses */
        }
        seek = check_virtual_tags(clause->tag, tcs->idx_id, idx);

        if (tcs->ramsearch)
        {
            struct tagfile_entry *tfe;

            if (!TAGCACHE_IS_NUMERIC(clause->tag))
            {
                if (clause->tag == tag_filename
                    || clause->tag == tag_virt_basename)
                {
                    /* The RAM copy keeps no filenames, so this is a disk
                     * read per entry, the RAM copy pinned throughout: a
                     * filename or basename clause over a large library
                     * takes seconds. */
                    if (!retrieve(tcs, idx,
                                  clause->tag, buf, bufsz))
                    {
                        tcs->failed = true;
                        return false;
                    }
                }
                else
                {
                    tfe = (struct tagfile_entry *)
                                        &tcramcache.hdr->tags[clause->tag][seek];
                    /* str points to movable data, but no locking required here,
                     * as no yield() is following */
                    str = tfe->tag_data;
                }
            }
        }
        else
        {
            struct tagfile_entry tfe;

            if (!TAGCACHE_IS_NUMERIC(clause->tag))
            {
                int tag = clause->tag;
                if (tag == tag_virt_basename)
                    tag = tag_filename;

                int fd = tcs->idxfd[tag];
                lseek(fd, seek, SEEK_SET);

                switch (read_tagfile_entry_and_tag(fd, &tfe, str, bufsz))
                {
                    case e_SUCCESS_LEN_ZERO: /* Check if entry has been deleted. */
                        return false;
                    case e_SUCCESS:
                        if (clause->tag == tag_virt_basename)
                        {
                            char *basename = strrchr(str, '/');
                            if (basename)
                                str = basename + 1;
                        }
                        break;
                    case e_ENTRY_SIZEMISMATCH:
                        logf("read error #15");
                        tcs->failed = true;
                        return false;
                    case e_TAG_TOOLONG:
                        logf("too long tag #6");
                        return false;
                    case e_TAG_SIZEMISMATCH:
                        logf("read error #16");
                        tcs->failed = true;
                        return false;
                    default:
                        logf("unknown_error");
                        break;;
                }
            }
        }

        if (!check_against_clause(seek, str, clause))
        {
            /* Clause failed -- try finding a logical-or clause */
            while (++i < count)
            {
                if (clauses[i]->type == clause_logical_or)
                    break;
            }

            if (i < count)        /* Found logical-or? */
                continue;         /* Check clauses after logical-or */

            return false;
        }

        logf_clauses("%s clause %d %s %s [%ld] %s",
            "Found",  i, tag_type_str[clause->type & ~CLAUSE_SORT_NAME],
            tags_str[clause->tag],  clause->numeric_data,
            (clause->numeric || clause->str == NULL) ? "[NUMERIC?]" : clause->str);
    }

    return true;
}

bool tagcache_check_clauses(struct tagcache_search *tcs,
                            struct tagcache_search_clause **clause, int count)
{
    struct index_entry idx;

    if (count == 0)
        return true;

    if (!get_index(tcs->masterfd, tcs->idx_id, &idx, true))
        return false;

    return check_clauses(tcs, &idx, clause, count);
}

static bool add_uniqbuf(struct tagcache_search *tcs, uint32_t id)
{
    int i;

    /* If uniq buffer is not defined we must return true for search to work. */
    if (tcs->unique_list == NULL || (!TAGCACHE_IS_UNIQUE(tcs->type)
                                     && !TAGCACHE_IS_NUMERIC(tcs->type)))
    {
        return true;
    }

    /* An open-addressed hash set in the caller's buffer, so a level costs
     * its rows rather than rows x distinct values. A slot of 0 is empty, so
     * the key is id + 1; once full, duplicates are let through. */
    uint32_t cap = tcs->unique_list_capacity;
    uint32_t key = id + 1;

    if (cap == 0)
        return true;

    i = (key * 2654435761u) % cap;
    for (uint32_t n = 0; n < cap; n++)
    {
        if (tcs->unique_list[i] == key)
            return false;
        if (tcs->unique_list[i] == 0)
        {
            tcs->unique_list[i] = key;
            tcs->unique_list_count++;
            return true;
        }
        if (++i == (int)cap)
            i = 0;
    }

    return true;
}

static bool build_lookup_list(struct tagcache_search *tcs)
{
    struct index_entry entry;
    int i, j;

    tcs->seek_list_count = 0;

    if (tcs->ramsearch)
    {
        tcrc_buffer_lock(); /* lock because below makes a pointer to movable data */

        int end = MIN(tcs->seek_end, current_tcmh.tch.entry_count);
        int pos;

        /* With an id list, seek_pos counts through the list rather than the
         * index. */
        if (tcs->id_list)
            end = tcs->id_count;

        for (pos = tcs->seek_pos; pos < end; pos++)
        {
            struct tagcache_seeklist_entry *seeklist;
            struct index_entry *idx;

            if (tcs->seek_list_count == SEEK_LIST_SIZE)
                break ;

            i = tcs->id_list ? tcs->id_list[pos] : pos;
            if (i < 0 || i >= current_tcmh.tch.entry_count)
                continue;

            /* idx points to movable data, don't yield or reload */
            idx = &tcramcache.hdr->indices[i];

            /* Skip deleted files. */
            if (idx->flag & FLAG_DELETED)
                continue;

            /* Go through all filters.. */
            for (j = 0; j < tcs->filter_count; j++)
            {
                if (idx->tag_seek[tcs->filter_tag[j]] != tcs->filter_seek[j])
                {
                    break ;
                }
            }

            if (j < tcs->filter_count)
                continue ;

            /* Check for conditions. */
            if (!check_clauses(tcs, idx, tcs->clause, tcs->clause_count))
            {
                if (tcs->failed)
                {
                    tcrc_buffer_unlock();
                    return false;
                }
                continue;
            }
            /* Add to the seek list if not already in uniq buffer (doesn't yield)*/
            if (!add_uniqbuf(tcs, idx->tag_seek[tcs->type]))
                continue;

            /* Lets add it. */
            seeklist = &tcs->seeklist[tcs->seek_list_count];
            seeklist->seek = idx->tag_seek[tcs->type];
            seeklist->flag = idx->flag;
            seeklist->idx_id = i;
            tcs->seek_list_count++;
        }

        tcrc_buffer_unlock();

        tcs->seek_pos = pos;

        return tcs->seek_list_count > 0;
    }

    if (tcs->masterfd < 0)
    {
        struct master_header tcmh;
        tcs->masterfd = open_master_fd(&tcmh, false);
        if (tcs->masterfd < 0)
        {
            tcs->failed = true;
            return false;
        }
        tcs->master_entry_count = tcmh.tch.entry_count;
    }

    if (lseek(tcs->masterfd, tcs->seek_pos * sizeof(struct index_entry) +
              sizeof(struct master_header), SEEK_SET) < 0)
    {
        tcs->failed = true;
        return false;
    }

    /* Bounded by the count the master header declares, so that a read
     * returning short is a truncated file rather than the end of one. Ending
     * the loop on the read instead cannot tell those apart, and answers a
     * partial list as though it were the whole answer. */
    while (tcs->seek_pos < MIN(tcs->seek_end, tcs->master_entry_count))
    {
        struct tagcache_seeklist_entry *seeklist;

        if (tcs->seek_list_count == SEEK_LIST_SIZE)
            break ;

        if (read_index_entries(tcs->masterfd, &entry, 1)
                != sizeof(struct index_entry))
        {
            tcs->failed = true;
            return false;
        }

        i = tcs->seek_pos;
        tcs->seek_pos++;

        /* Check if entry has been deleted. */
        if (entry.flag & FLAG_DELETED)
            continue;

        /* Go through all filters.. */
        for (j = 0; j < tcs->filter_count; j++)
        {
            if (entry.tag_seek[tcs->filter_tag[j]] != tcs->filter_seek[j])
                break ;
        }

        if (j < tcs->filter_count)
            continue ;

        /* Check for conditions. */
        if (!check_clauses(tcs, &entry, tcs->clause, tcs->clause_count))
        {
            if (tcs->failed)
                return false;
            continue;
        }

        /* Add to the seek list if not already in uniq buffer. */
        if (!add_uniqbuf(tcs, entry.tag_seek[tcs->type]))
            continue;

        /* Lets add it. */
        seeklist = &tcs->seeklist[tcs->seek_list_count];
        seeklist->seek = entry.tag_seek[tcs->type];
        seeklist->flag = entry.flag;
        seeklist->idx_id = i;
        tcs->seek_list_count++;

        yield();
    }

    return tcs->seek_list_count > 0;
}


/* The genre seeks tag_virt_spoken answers from hold good only for the commit
 * they were read at -- a commit re-sorts the tag files and moves every seek in
 * them. Refreshed as a search starts because that is the one point every
 * clause check is downstream of, and there is no other moment both late enough
 * to have a database and early enough to be before the question.
 *
 * Three things the flag and the captured commitid are each load-bearing for:
 *
 * db_spoken_build() runs a search of its own and so arrives back here. Without
 * the flag that is unbounded recursion rather than a wasted rebuild.
 *
 * The build's searches are refused while a commit holds the read lock, and a
 * commit can land between them. Recording the commitid read *before* it means
 * such a build is rebuilt next time rather than standing as current.
 *
 * A build that could not read the database records nothing, so it is retried
 * rather than leaving an empty table looking authoritative.
 *
 * A search starting while a build is in progress skips the update and reads
 * however much of the table is filled, so for the length of one rebuild it
 * may call an audiobook music. A lock would trade that for blocking a UI
 * search behind a background one, which is the worse of the two. */
static int32_t spoken_commitid = -1;
static uint32_t spoken_generation;
static bool spoken_building;

static void spoken_table_update(void)
{
    int32_t built_at;
    uint32_t built_gen;

    /* The generation too: a rebuild can leave commitid where it was. */
    if (spoken_building || (spoken_commitid == current_tcmh.commitid
                            && spoken_generation == db_generation))
        return;

    built_at = current_tcmh.commitid;
    built_gen = db_generation;

    spoken_building = true;
    if (db_spoken_build())
    {
        spoken_commitid = built_at;
        spoken_generation = built_gen;
    }
    spoken_building = false;
}

bool tagcache_search(struct tagcache_search *tcs, int tag)
{
    /* NOTE: call tagcache_search_finish(&tcs) when finished or BAD things may happen (TM) */
    struct tagcache_header tag_hdr;
    struct master_header   master_hdr;
    int i;

    /* Refused, not waited for, while a commit holds read_lock: that is the
     * commit's whole length, and the caller -- the UI, the audio thread, a
     * car's request -- would stop with it. Every caller treats false as the
     * database being busy. */
    memset(tcs, 0, sizeof(struct tagcache_search));
    if (read_lock || tc_stat.commit_step > 0 || !tc_stat.ready)
        return false;

    /* Before write_lock++ below, so the rebuild's own search is an ordinary
     * one rather than one nested inside a half-built search. */
    spoken_table_update();

    tcs->position = sizeof(struct tagcache_header);
    tcs->type = tag;
    tcs->seek_pos = 0;
    tcs->seek_end = INT_MAX;
    tcs->list_position = 0;
    tcs->seek_list_count = 0;
    tcs->filter_count = 0;
    tcs->masterfd = -1;

    for (i = 0; i < TAG_COUNT; i++)
        tcs->idxfd[i] = -1;

    tcs->ramsearch = tc_stat.ramcache;
    if (tcs->ramsearch)
    {
        tcs->entry_count = tcramcache.hdr->entry_count[tcs->type];
    }
    else
    {
        /* Always open as R/W so we can pass tcs to functions that modify data also
         * without failing. */
        tcs->masterfd = open_master_fd(&master_hdr, true);
        if (tcs->masterfd < 0)
            return false;

        tcs->master_entry_count = master_hdr.tch.entry_count;

        if (!TAGCACHE_IS_NUMERIC(tcs->type))
        {
            tcs->idxfd[tcs->type] = open_tag_fd(&tag_hdr, tcs->type, false);
            if (tcs->idxfd[tcs->type] < 0)
            {
                close(tcs->masterfd);
                tcs->masterfd = -1;
                return false;
            }

            tcs->entry_count = tag_hdr.entry_count;
        }
        else
        {
            tcs->entry_count = master_hdr.tch.entry_count;
        }
    }

    tcs->valid = true;
    tcs->initialized = true;
    write_lock++;

    return true;
}

void tagcache_search_set_uniqbuf(struct tagcache_search *tcs,
                                 void *buffer, long length)
{
    tcs->unique_list = (uint32_t *)buffer;
    tcs->unique_list_capacity = length / sizeof(*tcs->unique_list);
    tcs->unique_list_count = 0;
    memset(tcs->unique_list, 0,
           tcs->unique_list_capacity * sizeof(*tcs->unique_list));
}

void tagcache_search_set_range(struct tagcache_search *tcs, int first, int last)
{
    tcs->seek_pos = first;
    tcs->seek_end = last + 1;
}

bool tagcache_search_set_ids(struct tagcache_search *tcs,
                             const int *ids, int count)
{
    if (!tcs->ramsearch)
        return false;

    tcs->id_list = ids;
    tcs->id_count = count;
    tcs->seek_pos = 0;
    return true;
}

bool tagcache_search_add_filter(struct tagcache_search *tcs,
                                int tag, int seek)
{
    if (tcs->filter_count == TAGCACHE_MAX_FILTERS)
        return false;

    if (TAGCACHE_IS_NUMERIC_OR_NONUNIQUE(tag))
        return false;

    tcs->filter_tag[tcs->filter_count] = tag;
    tcs->filter_seek[tcs->filter_count] = seek;
    tcs->filter_count++;

    return true;
}

bool tagcache_search_add_clause(struct tagcache_search *tcs,
                                struct tagcache_search_clause *clause)
{
    int i;
    int clause_count = tcs->clause_count;

    if (clause_count >= TAGCACHE_MAX_CLAUSES)
    {
        logf("Too many clauses");
        return false;
    }

    if (clause->type != clause_logical_or)
    {
        /* BUGFIX OR'd clauses seem to be mishandled once made into a filter */
        if (clause_count <= 1 || tcs->clause[clause_count - 1]->type != clause_logical_or)
        {
            /* Check if there is already a similar filter in present (filters are
             * much faster than clauses).
             */
            for (i = 0; i < tcs->filter_count; i++)
            {
                if (tcs->filter_tag[i] == clause->tag)
                {
                    return true;
                }
            }
        }

        if (!TAGCACHE_IS_NUMERIC(clause->tag) && tcs->idxfd[clause->tag] < 0)
        {
            char fname[MAX_PATH];
            tcs->idxfd[clause->tag] = open_pathfmt(fname, sizeof(fname), O_RDONLY,
                                                   "%s/" TAGCACHE_FILE_INDEX,
                                                   tc_stat.db_path, clause->tag);
        }
    }

    tcs->clause[tcs->clause_count] = clause;
    tcs->clause_count++;

    return true;
}

static bool get_next(struct tagcache_search *tcs, bool is_numeric, char *buf, long bufsz)
{
    struct tagfile_entry entry;

    if (tcs->idxfd[tcs->type] < 0 && !is_numeric
        && !tcs->ramsearch
        )
        return false;

    /* Relative fetch. */
    if (tcs->filter_count > 0 || tcs->clause_count > 0 || is_numeric
        || tcs->id_list
        /* RAM holds no filenames: the index walk reads them from disk and
         * skips deleted entries */
        || (tcs->ramsearch && tcs->type == tag_filename)
        )
    {
        struct tagcache_seeklist_entry *seeklist;

        /* Check for end of list. */
        if (tcs->list_position == tcs->seek_list_count)
        {
            tcs->list_position = 0;

            /* Try to fetch more. */
            if (!build_lookup_list(tcs))
            {
                tcs->valid = false;
                return false;
            }
        }

        seeklist = &tcs->seeklist[tcs->list_position];
        tcs->position = seeklist->seek;
        tcs->idx_id = seeklist->idx_id;
        tcs->list_position++;
    }
    else
    {
        if (tcs->entry_count == 0)
        {
            tcs->valid = false;
            return false;
        }

        tcs->entry_count--;
    }

    tcs->result_seek = tcs->position;

    if (is_numeric)
    {
        itoa_buf(buf, bufsz, tcs->position);
        tcs->result = buf;
        tcs->result_len = strlen(buf) + 1;
        return true;
    }

    /* Direct fetch. */
    if (tcs->ramsearch)
    {
        if (tcs->type != tag_filename)
        {
            struct tagfile_entry *ep;

            ep = (struct tagfile_entry *)&tcramcache.hdr->tags[tcs->type][tcs->position];
            /* don't return ep->tag_data directly as it may move */
            tcs->result_len = strlcpy(buf, ep->tag_data, bufsz) + 1;
            tcs->result = buf;
            tcs->idx_id = ep->idx_id;
            tcs->ramresult = false; /* was true before we copied to buf too */

            /* Increase position for the next run. This may get overwritten. */
            tcs->position += sizeof(struct tagfile_entry) + ep->tag_length;

            return true;
        }
    }

    if (!open_files(tcs, tcs->type))
    {
        tcs->valid = false;
        tcs->failed = true;
        return false;
    }

    /* Seek stream to the correct position and continue to direct fetch. */
    lseek(tcs->idxfd[tcs->type], tcs->position, SEEK_SET);

    switch (read_tagfile_entry_and_tag(tcs->idxfd[tcs->type], &entry, buf, bufsz))
    {
        case e_SUCCESS_LEN_ZERO:
        case e_SUCCESS:
             break;
        case e_ENTRY_SIZEMISMATCH:
            logf("read error #5");
            tcs->valid = false;
            tcs->failed = true;
            return false;
        case e_TAG_TOOLONG:
            tcs->valid = false;
            tcs->failed = true;
            logf("too long tag #2");
            logf("P:%lX/%" PRIX32, (unsigned long) tcs->position, entry.tag_length);
            return false;
        case e_TAG_SIZEMISMATCH:
            tcs->valid = false;
            tcs->failed = true;
            logf("read error #4");
            return false;
    }

    /**
     Update the position for the next read (this may be overridden
     if filters or clauses are being used).
     */
    tcs->position += sizeof(struct tagfile_entry) + entry.tag_length;
    str_setlen(buf, entry.tag_length);

    tcs->result = buf;
    tcs->result_len = entry.tag_length + 1;
    tcs->idx_id = entry.idx_id;
    tcs->ramresult = false;

    return true;
}

bool tagcache_get_next(struct tagcache_search *tcs, char *buf, long size)
{
    if (tcs->valid && tagcache_is_usable())
    {
        bool is_numeric = TAGCACHE_IS_NUMERIC(tcs->type);
        while (get_next(tcs, is_numeric, buf, size))
        {
            if (tcs->result_len > 1)
                return true;
        }
    }
    else if (tcs->valid)
        tcs->failed = true;     /* the database went away mid-search */
#ifdef LOGF_ENABLE
    if (tcs->unique_list_count > 0)
        logf(" uniqbuf: %d used / %d avail", tcs->unique_list_count, tcs->unique_list_capacity);
#endif

    return false;
}

bool tagcache_retrieve(struct tagcache_search *tcs, int idxid,
                       int tag, char *buf, long size)
{
    struct index_entry idx;

    *buf = '\0';
    if (!get_index(tcs->masterfd, idxid, &idx, true))
        return false;

    return retrieve(tcs, &idx, tag, buf, size);
}

void tagcache_search_finish(struct tagcache_search *tcs)
{
    int i;

    if (!tcs->initialized)
        return;

    if (tcs->masterfd >= 0)
    {
        close(tcs->masterfd);
        tcs->masterfd = -1;
    }

    for (i = 0; i < TAG_COUNT; i++)
    {
        if (tcs->idxfd[i] >= 0)
        {
            close(tcs->idxfd[i]);
            tcs->idxfd[i] = -1;
        }
    }

    tcs->ramsearch = false;
    tcs->valid = false;
    tcs->initialized = 0;
    if (write_lock > 0)
        write_lock--;
}

static struct tagfile_entry *get_tag(const struct index_entry *entry, int tag)
{
    return (struct tagfile_entry *)&tcramcache.hdr->tags[tag][entry->tag_seek[tag]];
}

static long get_tag_numeric(const struct index_entry *entry, int tag, int idx_id)
{
    return check_virtual_tags(tag, idx_id, entry);
}

static char* get_tag_string(const struct index_entry *entry, int tag)
{
    char* s = get_tag(entry, tag)->tag_data;
    return strcmp(s, UNTAGGED) ? s : NULL;
}

bool tagcache_fill_tags(struct mp3entry *id3, const char *filename)
{
    struct index_entry *entry;
    int idx_id;

    if (!tc_stat.ready || !tc_stat.ramcache)
        return false;

    /* Find the corresponding entry in tagcache. */

    if (filename != NULL)
        memset(id3, 0, sizeof(struct mp3entry));
    else /* Note: caller clears id3 prior to call */
        filename = id3->path;

    idx_id = find_entry_ram(filename);
    if (idx_id < 0)
        return false;

    /* The path, and nothing that makes the entry valid_mp3entry(): it is
     * tags without a file behind them, so no codec type and no size. Callers
     * cache a filled entry by its path, and a play is counted only for a
     * valid one. */
    if (filename != id3->path)
        strmemccpy(id3->path, filename, sizeof(id3->path));

    entry = &tcramcache.hdr->indices[idx_id];

    char* buf = id3->id3v2buf;
    ssize_t remaining = sizeof(id3->id3v2buf);

    /* this macro sets id3 strings by copying to the id3v2buf */
#define SET(x, y) do                                                           \
    {                                                                          \
        if (remaining > 0)                                                     \
        {                                                                      \
            x          = NULL; /* initialize with null if tag doesn't exist */ \
            char* src = get_tag_string(entry, y);                              \
            if (src)                                                           \
            {                                                                  \
                x = buf;                                                       \
                size_t len = strlcpy(buf, src, remaining) +1;                  \
                buf += len; remaining -= len;                                  \
            }                                                                  \
        }                                                                      \
    } while(0)


    SET(id3->title,         tag_title);
    SET(id3->artist,        tag_artist);
    SET(id3->album,         tag_album);
    SET(id3->genre_string,  tag_genre);
    SET(id3->composer,      tag_composer);
    SET(id3->comment,       tag_comment);
    SET(id3->albumartist,   tag_albumartist);
    SET(id3->grouping,      tag_grouping);

    id3->length     = get_tag_numeric(entry, tag_length, idx_id);
    id3->playcount  = get_tag_numeric(entry, tag_playcount, idx_id);
    id3->rating     = get_tag_numeric(entry, tag_rating, idx_id);
    id3->lastplayed = get_tag_numeric(entry, tag_lastplayed, idx_id);
    id3->score      = get_tag_numeric(entry, tag_virt_autoscore, idx_id) / 10;
    id3->year       = get_tag_numeric(entry, tag_year, idx_id);

    id3->discnum = get_tag_numeric(entry, tag_discnumber, idx_id);
    id3->tracknum = get_tag_numeric(entry, tag_tracknumber, idx_id);
    id3->bitrate = get_tag_numeric(entry, tag_bitrate, idx_id);
    if (id3->bitrate == 0)
        id3->bitrate = 1;

    if (global_settings.autoresume_enable)
    {
        id3->elapsed = get_tag_numeric(entry, tag_lastelapsed, idx_id);
        logf("tagcache_fill_tags: Set elapsed for %s to %lX\n",
             id3->title, id3->elapsed);

        id3->offset = get_tag_numeric(entry, tag_lastoffset, idx_id);
        logf("tagcache_fill_tags: Set offset for %s to %lX\n",
             id3->title, id3->offset);
    }

    return true;
}

int tagcache_find_path(const char *path)
{
    return path_index_find(path_key(path));
}

int tagcache_find_key(uint64_t key)
{
    return path_index_find(key);
}

bool tagcache_path_slot(int n, uint64_t *key, int *idx_id)
{
    if (!tc_stat.ramcache || n < 0 || n >= tcramcache.hdr->path_count)
        return false;

    const struct path_slot *s = &tcrc_path_slots[n];
    *key = (uint64_t)s->key_hi << 32 | s->key_lo;
    *idx_id = tcramcache.hdr->indices[s->idx_id].flag & FLAG_DELETED
              ? -1 : s->idx_id;
    return true;
}

/* Linear: the index is sorted by key, not by idx_id. Nothing here yields. */
uint64_t tagcache_entry_key(int idx_id)
{
    if (!tc_stat.ramcache || idx_id < 0
        || idx_id >= current_tcmh.tch.entry_count)
        return 0;

    const struct path_slot *s = tcrc_path_slots;
    for (int n = tcramcache.hdr->path_count; n > 0; n--, s++)
        if (s->idx_id == idx_id)
            return (uint64_t)s->key_hi << 32 | s->key_lo;
    return 0;
}

int tagcache_path_slots(void)
{
    return tc_stat.ramcache ? tcramcache.hdr->path_count : 0;
}

int32_t tagcache_commit_id(void)
{
    return current_tcmh.commitid;
}

/* The RAM copy's entry, if it is live. A deleted entry's string seeks hold a
 * CRC of the text rather than a position (see delete_entry()), so they must
 * not be followed. */
static const struct index_entry *ram_entry(int idx_id)
{
    const struct index_entry *entry;

    if (!tc_stat.ramcache || idx_id < 0
        || idx_id >= current_tcmh.tch.entry_count)
        return NULL;

    entry = &tcramcache.hdr->indices[idx_id];
    return (entry->flag & FLAG_DELETED) ? NULL : entry;
}

bool tagcache_entry_string(int idx_id, int tag, char *buf, size_t size)
{
    const struct index_entry *entry = ram_entry(idx_id);
    const char *s;

    /* The RAM copy keeps no filenames, only their keys. */
    if (!entry || tag < 0 || tag >= TAG_COUNT || TAGCACHE_IS_NUMERIC(tag)
        || tag == tag_filename)
        return false;

    s = get_tag_string(entry, tag);
    if (!s)
        return false;

    strmemccpy(buf, s, size);
    return true;
}

/* sound_index_genre_of(), for the sound index's update to refold its
 * records' genre keys. */
static bool genre_by_key(uint64_t key, char *buf, size_t size)
{
    return tagcache_entry_string(path_index_find(key), tag_genre, buf, size);
}

bool tagcache_seek_string(int tag, long seek, char *buf, size_t size)
{
    const struct tagfile_entry *ep;

    if (!tc_stat.ramcache || tag < 0 || tag >= TAG_COUNT
        || TAGCACHE_IS_NUMERIC(tag) || tag == tag_filename || seek < 0
        || seek > tcramcache.hdr->tag_size[tag]
                  - (long)sizeof(struct tagfile_entry))
        return false;

    ep = (const struct tagfile_entry *)&tcramcache.hdr->tags[tag][seek];
    if (!strcmp(ep->tag_data, UNTAGGED))
        return false;

    strmemccpy(buf, ep->tag_data, size);
    return true;
}

bool tagcache_entry_numeric(int idx_id, int tag, long *value)
{
    const struct index_entry *entry = ram_entry(idx_id);

    if (!entry || (unsigned)tag >= 32 || !TAGCACHE_IS_NUMERIC(tag))
        return false;

    *value = get_tag_numeric(entry, tag, idx_id);
    return true;
}

bool tagcache_path_index_info(int *slots, int *found, int *missed)
{
    *found = path_found;
    *missed = path_missed;
    *slots = tc_stat.ramcache ? tcramcache.hdr->path_count : 0;
    return tc_stat.ramcache;
}

static inline void write_item(const char *item)
{
    int len = strlen(item) + 1;

    data_size += len;
    write(cachefd, item, len);
}

static int check_if_empty(char **tag)
{
    int length;

    if (*tag == NULL || **tag == '\0')
    {
        *tag = UNTAGGED;
        return sizeof(UNTAGGED); /* Tag length */
    }

    length = strlen(*tag);
    if (length > TAG_MAXLEN)
    {
        logf("over length tag: %s", *tag);
        length = TAG_MAXLEN;
        str_setlen((*tag), length);
    }

    return length + 1;
}


/* GCC 3.4.6 for Coldfire can choose to inline this function. Not a good
 * idea, as it uses lots of stack and is called from a recursive function
 * (check_dir).
 */
/* The year a folder name starts with, as in "1998 - Album", or 0: four digits
 * from 1900 to 2099, then anything but a fifth digit. 'end' is the '/' after
 * the name. */
static int name_year(const char *start, const char *end)
{
    int year = 0;

    if (end - start < 4)
        return 0;
    for (int i = 0; i < 4; i++)
    {
        if (!isdigit((unsigned char)start[i]))
            return 0;
        year = year * 10 + (start[i] - '0');
    }
    if (isdigit((unsigned char)start[4]))
        return 0;

    return (year >= 1900 && year <= 2099) ? year : 0;
}

/* The year of the album a track's folder belongs to, or 0: the folder's own
 * name, or for a disc folder, the name of the folder above it. Only a disc
 * folder looks up -- above an album is usually the artist, and a band named
 * "1990s" is not a year. */
static int folder_name_year(const char *path)
{
    const char *end = strrchr(path, '/');
    const char *start = end;
    int year;

    if (end == NULL)
        return 0;
    while (start > path && start[-1] != '/')
        start--;

    year = name_year(start, end);
    if (year == 0 && start > path && is_disc_folder(start, end))
    {
        end = start - 1;
        start = end;
        while (start > path && start[-1] != '/')
            start--;
        year = name_year(start, end);
    }

    return year;
}

/* The entries the walk found, one bit each. After a walk that completed,
 * every live entry left unmarked is deleted before the commit -- a file that
 * has gone, or one outside the scan paths or under a database.ignore -- which
 * is also what lets a moved file keep its figures. Static, so it costs no
 * allocation; a larger library, or a database that is not ready, falls back
 * to check_deleted_files(). */
#define WALK_SEEN_MAX 65536
static uint32_t walk_seen[WALK_SEEN_MAX / 32];
static long walk_seen_count;
/* Set when the walk skipped a folder or could not look a file up, so an
 * unmarked entry may still exist: the deletion pass is skipped. */
static bool walk_incomplete;

static bool walk_checks_deletions(void)
{
    return tc_stat.ready && current_tcmh.tch.entry_count <= WALK_SEEN_MAX;
}

static void NO_INLINE add_tagcache(char *path, unsigned long mtime)
{
    #define ADD_TAG(entry, tag, data) \
        /* Adding tag */                              \
        entry.tag_length[tag] = check_if_empty(data); \
        entry.tag_offset[tag] = offset;               \
        offset += entry.tag_length[tag]

    struct mp3entry id3;
    struct temp_file_entry entry;
    bool ret;
    int idx_id = -1;
    char tracknumfix[3];
    int offset = 0;
    int path_length = strlen(path);
    bool has_artist;

    DB_LOG("file", path);

    if (cachefd < 0)
        return ;

    /* Check for overlength file path. */
    if (path_length > MAX_PATH || path_length > TAG_MAXLEN)
    {
        /* Path can't be shortened. */
        logf("Too long path: %s", path);
        DB_LOG("error", "path too long");
        return ;
    }

    /* Check if the file is supported. */
    if (probe_file_format(path) == AFMT_UNKNOWN)
        return ;

    /* Check if the file is already cached. A miss in the RAM copy's path
     * index is final; only without one does the disk have to be asked, at
     * the cost of a scan of the filename file for every file not found. */
    if (tc_stat.ramcache)
        idx_id = find_entry_ram(path);
    else if (filenametag_fd >= 0)
        idx_id = find_entry_disk(path, false);

    /* find_entry_disk() drops the descriptor on a read error. */
    if (!tc_stat.ramcache && filenametag_fd < 0)
        walk_incomplete = true;

    if (idx_id >= 0 && idx_id < walk_seen_count)
        walk_seen[idx_id / 32] |= 1u << (idx_id % 32);

    /* Check if file has been modified. */
    if (idx_id >= 0)
    {
        struct index_entry idx;

        /* TODO: Mark that the index exists (for fast reverse scan) */
        /* found_idx[idx_id/8] |= idx_id%8; */

        if (!get_index(-1, idx_id, &idx, true))
        {
            logf("failed to retrieve index entry");
            DB_LOG("error", "failed to retrieve index entry");
            return ;
        }

        if ((unsigned long)idx.tag_seek[tag_mtime] == mtime)
        {
            /* No changes to file. */
            return ;
        }

        /* Metadata might have been changed. Delete the entry. */
        logf("Re-adding: %s", path);
        DB_LOG("info", "re-adding");
        if (!delete_entry(idx_id))
        {
            logf("delete_entry failed: %d", idx_id);
            DB_LOG("error", "delete entry failed");
            return ;
        }
    }

    /*memset(&id3, 0, sizeof(struct mp3entry)); -- get_metadata does this for us */
    memset(&entry, 0, sizeof(struct temp_file_entry));
    memset(&tracknumfix, 0, sizeof(tracknumfix));
    ret = get_metadata_ex(&id3, -1, path, METADATA_EXCLUDE_ID3_PATH);

    if (!ret)
    {
        logf("get_metadata failed: %s", path);
        DB_LOG("error", "get_metadata failed");
        return ;
    }

    /* Skip files with video tracks (e.g. music videos in MP4 containers) */
    if (id3.has_video)
        return ;

    logf("-> %s", path);

    if (id3.tracknum < 0)              /* Track number missing? */
    {
        id3.tracknum = -1;
    }

    /* Numeric tags */
    entry.tag_offset[tag_year] = id3.year;
    if (global_settings.year_from_folder)
    {
        int year = folder_name_year(path);

        if (year > 0)
            entry.tag_offset[tag_year] = year;
    }
    entry.tag_offset[tag_discnumber] = id3.discnum;
    entry.tag_offset[tag_tracknumber] = id3.tracknum;
    entry.tag_offset[tag_length] = id3.length;
    entry.tag_offset[tag_bitrate] = id3.bitrate;
    entry.tag_offset[tag_mtime] = mtime;

    /* String tags. */
    has_artist = id3.artist != NULL
        && strlen(id3.artist) > 0;

    /* No album artist is the artist's own album, so every view grouped by
     * album artist finds the track under its artist, not under <Untagged>. */
    if (has_artist && (id3.albumartist == NULL || id3.albumartist[0] == '\0'))
        id3.albumartist = id3.artist;

    ADD_TAG(entry, tag_filename, &path);
    ADD_TAG(entry, tag_title, &id3.title);
    ADD_TAG(entry, tag_artist, &id3.artist);
    ADD_TAG(entry, tag_album, &id3.album);
    ADD_TAG(entry, tag_genre, &id3.genre_string);
    ADD_TAG(entry, tag_composer, &id3.composer);
    ADD_TAG(entry, tag_comment, &id3.comment);
    ADD_TAG(entry, tag_albumartist, &id3.albumartist);
    if (has_artist)
    {
        ADD_TAG(entry, tag_virt_canonicalartist, &id3.artist);
    }
    else
    {
        ADD_TAG(entry, tag_virt_canonicalartist, &id3.albumartist);
    }
    /* Grouping as tagged, <Untagged> when it is not -- never a copy of the
     * title, which made it a second sorted file of every title for each
     * commit to rewrite, read by nothing. */
    ADD_TAG(entry, tag_grouping, &id3.grouping);
    entry.data_length = offset;

    /* Write the header */
    write(cachefd, &entry, sizeof(struct temp_file_entry));

    /* And tags also... Correct order is critical */
    write_item(path);
    write_item(id3.title);
    write_item(id3.artist);
    write_item(id3.album);
    write_item(id3.genre_string);
    write_item(id3.composer);
    write_item(id3.comment);
    write_item(id3.albumartist);
    if (has_artist)
    {
        write_item(id3.artist);
    }
    else
    {
        write_item(id3.albumartist);
    }
    write_item(id3.grouping);

    total_entry_count++;

    #undef ADD_TAG
}


static bool tempbuf_insert(char *str, int id, int idx_id, bool unique)
{
    struct tempbuf_searchidx *index = (struct tempbuf_searchidx *)tempbuf;
    int len = strlen(str)+1;
    int i;
    unsigned *crcbuf = (unsigned *)&tempbuf[tempbuf_size-4];
    unsigned crc32 = 0xffffffff;
    char chr_lower;
    for (i = 0; str[i] != '\0' && i < len -1; i++)
    {
        chr_lower = tolower(str[i]);
        crc32 = crc_32(&chr_lower, 1, crc32);
    }

    if (unique)
    {
        /* Check if the crc does not exist -> entry does not exist for sure. */
        for (i = 0; i < tempbufidx; i++)
        {
            if (crcbuf[-i] != crc32)
                continue;

            if (!strcasecmp(str, index[i].str))
            {
                if (id < 0 || id >= lookup_buffer_depth)
                {
                    logf("lookup buf overf.: %d", id);
                    return false;
                }

                lookup[id] = &index[i];
                return true;
            }
        }
    }

    /* Insert to CRC buffer. */
    crcbuf[-tempbufidx] = crc32;
    tempbuf_left -= 4;

    /* Insert it to the buffer. */
    tempbuf_left -= len;
    if (tempbuf_left - 4 < 0 || tempbufidx >= commit_entry_count)
    {
        logf("temp buf error rem: %ld idx: %ld / %ld",
             tempbuf_left, tempbufidx, commit_entry_count-1);
        return false;
    }
    if (id >= lookup_buffer_depth)
    {
        logf("lookup buf overf. #2: %d", id);
        return false;
    }

    if (id >= 0)
    {
        lookup[id] = &index[tempbufidx];
        index[tempbufidx].idlist.id = id;
    }
    else
        index[tempbufidx].idlist.id = -1;

    index[tempbufidx].idlist.next = NULL;
    index[tempbufidx].idx_id = idx_id;
    index[tempbufidx].seek = -1;
    index[tempbufidx].str = &tempbuf[tempbuf_pos];
    memcpy(index[tempbufidx].str, str, len);
    tempbuf_pos += len;
    tempbufidx++;

    return true;
}

static int compare(const void *p1, const void *p2)
{
    do_timed_yield();

    struct tempbuf_searchidx *e1 = (struct tempbuf_searchidx *)p1;
    struct tempbuf_searchidx *e2 = (struct tempbuf_searchidx *)p2;

    if (strcmp(e1->str, UNTAGGED) == 0)
    {
        if (strcmp(e2->str, UNTAGGED) == 0)
            return 0;
        return -1;
    }
    else if (strcmp(e2->str, UNTAGGED) == 0)
        return 1;

    return strncasecmp(e1->str, e2->str, TAG_MAXLEN);
}

/* Writes go through this, a few sectors at a time: tempbuf_sort() produces a
 * tag file as a header, a string and padding per entry, and three writes an
 * entry through the one-sector file cache cost a disk operation each. */
static char sort_wbuf[2048];
static int sort_wlen;

static bool sort_flush(int fd)
{
    bool ok = sort_wlen == 0 || write(fd, sort_wbuf, sort_wlen) == sort_wlen;
    sort_wlen = 0;
    return ok;
}

static bool sort_write(int fd, const void *data, int len)
{
    const char *p = data;

    while (len > 0)
    {
        int n = MIN(len, (int)sizeof(sort_wbuf) - sort_wlen);
        memcpy(&sort_wbuf[sort_wlen], p, n);
        sort_wlen += n;
        p += n;
        len -= n;
        if (sort_wlen == (int)sizeof(sort_wbuf) && !sort_flush(fd))
            return false;
    }
    return true;
}

static int tempbuf_sort(int fd)
{
    struct tempbuf_searchidx *index = (struct tempbuf_searchidx *)tempbuf;
    struct tagfile_entry fe;
    int i;
    int length;
    off_t pos = lseek(fd, 0, SEEK_CUR);

    /* Generate reverse lookup entries. */
    for (i = 0; i < lookup_buffer_depth; i++)
    {
        struct tempbuf_id_list *idlist;

        if (!lookup[i])
            continue;

        if (lookup[i]->idlist.id == i)
            continue;

        idlist = &lookup[i]->idlist;
        while (idlist->next != NULL)
            idlist = idlist->next;

        ALIGN_BUFFER(tempbuf_pos, tempbuf_left, alignof(struct tempbuf_id_list));
        tempbuf_left -= sizeof(struct tempbuf_id_list);
        if (tempbuf_left < 0)
            return -1;

        idlist->next = (struct tempbuf_id_list *)&tempbuf[tempbuf_pos];
        tempbuf_pos += sizeof(struct tempbuf_id_list);

        idlist = idlist->next;
        idlist->id = i;
        idlist->next = NULL;

        do_timed_yield();
    }

    qsort(index, tempbufidx, sizeof(struct tempbuf_searchidx), compare);
    memset(lookup, 0, lookup_buffer_depth * sizeof(struct tempbuf_searchidx **));

    sort_wlen = 0;
    for (i = 0; i < tempbufidx; i++)
    {
        struct tempbuf_id_list *idlist = &index[i].idlist;

        /* Fix the lookup list. */
        while (idlist != NULL)
        {
            if (idlist->id >= 0)
                lookup[idlist->id] = &index[i];
            idlist = idlist->next;
        }

        index[i].seek = pos;
        length = strlen(index[i].str) + 1;
        fe.tag_length = length;
        fe.idx_id = index[i].idx_id;

        /* Check the chunk alignment. */
        if ((fe.tag_length + sizeof(struct tagfile_entry))
            % TAGFILE_ENTRY_CHUNK_LENGTH)
        {
            fe.tag_length += TAGFILE_ENTRY_CHUNK_LENGTH -
                ((fe.tag_length + sizeof(struct tagfile_entry))
                 % TAGFILE_ENTRY_CHUNK_LENGTH);
        }
        pos += sizeof(struct tagfile_entry) + fe.tag_length;

        int padding = fe.tag_length - length;
        swap_tagfile_entry(&fe);
        if (!sort_write(fd, &fe, sizeof(fe))
            || !sort_write(fd, index[i].str, length)
            || (padding > 0 && !sort_write(fd, "XXXXXXXX", padding)))
        {
            logf("tempbuf_sort: write error");
            return -1;
        }
    }

    if (!sort_flush(fd))
        return -1;

    return i;
}

inline static struct tempbuf_searchidx* tempbuf_locate(int id)
{
    if (id < 0 || id >= lookup_buffer_depth)
        return NULL;

    return lookup[id];
}


inline static int tempbuf_find_location(int id)
{
    struct tempbuf_searchidx *entry;

    entry = tempbuf_locate(id);
    if (entry == NULL)
        return -1;

    return entry->seek;
}

static bool build_numeric_indices(struct tagcache_header *h, int tmpfd)
{
    struct master_header tcmh;
    struct index_entry idxbuf[IDX_BUF_DEPTH];
    int masterfd;
    int masterfd_pos;
    struct temp_file_entry *entrybuf = (struct temp_file_entry *)tempbuf;
    int max_entries;
    int entries_processed = 0;
    int i, j, k, n;

    max_entries = tempbuf_size / sizeof(struct temp_file_entry) - 1;

    logf("Building numeric indices...");
    lseek(tmpfd, sizeof(struct tagcache_header), SEEK_SET);

    masterfd = open_master_file(TAGCACHE_FILE_MASTER ".new", &tcmh, true);
    if (masterfd < 0)
        return false;

    masterfd_pos = lseek(masterfd, tcmh.tch.entry_count * sizeof(struct index_entry),
                         SEEK_CUR);
    if (masterfd_pos < 0)
    {
        logf("we can't append!");
        close(masterfd);
        return false;
    }

    /* Into the master's .new file, before the swap: the resurrected flags
     * and the figures they gave reach the database together or not at all.
     * Marked in the live master, a cut part way would leave entries marked
     * whose figures a retry then copies to nothing. */
    while (entries_processed < h->entry_count)
    {
        int count = MIN(h->entry_count - entries_processed, max_entries);

        /* Read in as many entries as possible. */
        for (i = 0; i < count; i++)
        {
            struct temp_file_entry *tfe = &entrybuf[i];
            int datastart;

            /* Read in numeric data. */
            if (read(tmpfd, tfe, sizeof(struct temp_file_entry)) !=
                sizeof(struct temp_file_entry))
            {
                logf("read fail #1");
                close(masterfd);
                return false;
            }

            datastart = lseek(tmpfd, 0, SEEK_CUR);

            /**
             * Read string data from the following tags:
             * - tag_filename
             * - tag_artist
             * - tag_album
             * - tag_title
             *
             * A crc32 hash is calculated from the read data
             * and stored back to the data offset field kept in memory.
             */
#define tmpdb_read_string_tag(tag) \
    lseek(tmpfd, tfe->tag_offset[tag], SEEK_CUR); \
    if ((unsigned long)tfe->tag_length[tag] > (unsigned long)build_idx_bufsz) \
    { \
        logf("read fail: buffer overflow"); \
        close(masterfd); \
        return false; \
    } \
    \
    if (read(tmpfd, build_idx_buf, tfe->tag_length[tag]) != \
        tfe->tag_length[tag]) \
    { \
        logf("read fail #2"); \
        close(masterfd); \
        return false; \
    } \
    str_setlen(build_idx_buf, tfe->tag_length[tag]); \
    \
    tfe->tag_offset[tag] = crc_32(build_idx_buf, strlen(build_idx_buf), 0xffffffff); \
    lseek(tmpfd, datastart, SEEK_SET)

            tmpdb_read_string_tag(tag_filename);
            tmpdb_read_string_tag(tag_artist);
            tmpdb_read_string_tag(tag_album);
            tmpdb_read_string_tag(tag_title);

            /* Seek to the end of the string data. */
            lseek(tmpfd, tfe->data_length, SEEK_CUR);
        }

        /* Backup the master index position. */
        masterfd_pos = lseek(masterfd, 0, SEEK_CUR);
        lseek(masterfd, sizeof(struct master_header), SEEK_SET);

        /* Check if we can resurrect some deleted runtime statistics data. */
        for (i = 0; i < tcmh.tch.entry_count; i += n)
        {
            off_t loc = lseek(masterfd, 0, SEEK_CUR);
            bool changed = false;

            n = MIN(tcmh.tch.entry_count - i, IDX_BUF_DEPTH);
            if (read_index_entries(masterfd, idxbuf, n)
                != (ssize_t)sizeof(struct index_entry) * n)
            {
                logf("read fail #3");
                close(masterfd);
                return false;
            }

            for (k = 0; k < n; k++)
            {
                struct index_entry *idx = &idxbuf[k];

                /**
                 * Skip unless the entry is marked as being deleted
                 * or the data has already been resurrected.
                 */
                if (!(idx->flag & FLAG_DELETED)
                    || (idx->flag & FLAG_RESURRECTED))
                    continue;

                /* Now try to match the entry. */
                /**
                 * To succesfully match a song, the following conditions
                 * must apply:
                 *
                 * For numeric fields: tag_length
                 * - Full identical match is required
                 *
                 * If tag_filename matches, no further checking necessary.
                 *
                 * For string hashes: tag_artist, tag_album, tag_title
                 * - All three of these must match
                 */
                for (j = 0; j < count; j++)
                {
                    struct temp_file_entry *tfe = &entrybuf[j];

                    /* Try to match numeric fields first. */
                    if (tfe->tag_offset[tag_length]
                        != idx->tag_seek[tag_length])
                        continue;

                    /* Now it's time to do the hash matching. */
                    if (tfe->tag_offset[tag_filename]
                        != idx->tag_seek[tag_filename])
                    {
                        int match_count = 0;

                        /* No filename match: the other three tags must. */
#define tmpdb_match(tag) \
    if (tfe->tag_offset[tag] == idx->tag_seek[tag]) \
        match_count++

                        tmpdb_match(tag_artist);
                        tmpdb_match(tag_album);
                        tmpdb_match(tag_title);

                        if (match_count < 3)
                        {
                            /* Still no match found, give up. */
                            continue;
                        }
                    }

                    /* A match: copy and resurrect the statistical data. */
#define tmpdb_copy_tag(tag) \
    tfe->tag_offset[tag] = idx->tag_seek[tag]

                    tmpdb_copy_tag(tag_playcount);
                    tmpdb_copy_tag(tag_rating);
                    tmpdb_copy_tag(tag_playtime);
                    tmpdb_copy_tag(tag_lastplayed);
                    tmpdb_copy_tag(tag_commitid);
                    tmpdb_copy_tag(tag_lastelapsed);
                    tmpdb_copy_tag(tag_lastoffset);

                    /* Avoid processing this entry again: one deleted entry
                     * gives its figures to one new entry. */
                    idx->flag |= FLAG_RESURRECTED;
                    changed = true;

                    logf("Entry resurrected");
                    break;
                }
            }

            if (changed)
            {
                lseek(masterfd, loc, SEEK_SET);
                if (write_index_entries(masterfd, idxbuf, n)
                    != (ssize_t)sizeof(struct index_entry) * n)
                {
                    logf("masterfd writeback fail #1");
                    close(masterfd);
                    return false;
                }
            }
        }


        /* Restore the master index position. */
        lseek(masterfd, masterfd_pos, SEEK_SET);

        /* Commit the data to the index. */
        for (i = 0; i < count; i += n)
        {
            off_t loc = lseek(masterfd, 0, SEEK_CUR);

            n = MIN(count - i, IDX_BUF_DEPTH);
            if (read_index_entries(masterfd, idxbuf, n)
                != (ssize_t)sizeof(struct index_entry) * n)
            {
                logf("read fail #3");
                close(masterfd);
                return false;
            }

            for (k = 0; k < n; k++)
            {
                struct index_entry *idx = &idxbuf[k];

                for (j = 0; j < TAG_COUNT; j++)
                {
                    if (!TAGCACHE_IS_NUMERIC(j))
                        continue;

                    idx->tag_seek[j] = entrybuf[i + k].tag_offset[j];
                }
                idx->flag = entrybuf[i + k].flag;

                if (idx->tag_seek[tag_commitid])
                {
                    /* Data has been resurrected. */
                    idx->flag |= FLAG_DIRTYNUM;
                }
                else if (tc_stat.ready && current_tcmh.commitid > 0)
                {
                    idx->tag_seek[tag_commitid] = current_tcmh.commitid;
                    idx->flag |= FLAG_DIRTYNUM;
                }
            }

            /* Write back the updated index. */
            lseek(masterfd, loc, SEEK_SET);
            if (write_index_entries(masterfd, idxbuf, n)
                != (ssize_t)sizeof(struct index_entry) * n)
            {
                logf("write fail");
                close(masterfd);
                return false;
            }
        }

        entries_processed += count;
        logf("%d/%" PRId32 " entries processed", entries_processed, h->entry_count);
    }

    close(masterfd);

    return true;
}

/**
 * Return values:
 *     > 0   success
 *    == 0   temporary failure
 *     < 0   fatal error
 */
/* One string tag of a commit. A sorted tag is merged with the new strings,
 * sorted and written whole to <file>.new, leaving the original untouched;
 * the filename tag is appended to in place, its old entries unchanged. Either
 * way, the seeks the master needs are left in `merge` for merge_master().
 *
 * Return values: 1 done (or a cancel noticed before anything was written),
 * 0 the buffer is too small, below 0 an error. */
static int build_index(int index_type, struct tagcache_header *h, int tmpfd)
{
    int i;
    struct tagcache_header tch;
    struct master_header   tcmh;
    int fd = -1, outfd = -1;
    bool error = false;
    long master_count = 0;
    bool sorted = TAGCACHE_IS_SORTED(index_type);

    logf("Building index: %d", index_type);

    /* Check the number of entries we need to allocate ram for. */
    commit_entry_count = h->entry_count + 1;

    fd = open_master_fd(&tcmh, false);
    if (fd >= 0)
    {
        master_count = tcmh.tch.entry_count;
        commit_entry_count += master_count;
        close(fd);

        /* Open the index file, which contains the tag names. Without a
         * master there are no old entries, and any tag file is a leftover. */
        fd = open_tag_fd(&tch, index_type, !sorted);
    }
    else
        fd = -1;

    if (fd >= 0)
    {
        logf("tch.datasize=%" PRId32, tch.datasize);
        lookup_buffer_depth = 1 +
        /* First part */ commit_entry_count +
        /* Second part */ (tch.datasize / TAGFILE_ENTRY_CHUNK_LENGTH);
    }
    else
    {
        lookup_buffer_depth = 1 +
        /* First part */ commit_entry_count +
        /* Second part */ 0;
    }

    logf("lookup_buffer_depth=%ld", lookup_buffer_depth);
    logf("commit_entry_count=%ld", commit_entry_count);

    /* Allocate buffer for all index entries from both old and new
     * tag files. */
    tempbufidx = 0;
    tempbuf_pos = commit_entry_count * sizeof(struct tempbuf_searchidx);

    /* Allocate lookup buffer. The first portion of commit_entry_count
     * contains the new tags in the temporary file and the second
     * part for locating entries already in the db.
     *
     *  New tags  Old tags
     * +---------+---------------------------+
     * |  index  | position/ENTRY_CHUNK_SIZE |  lookup buffer
     * +---------+---------------------------+
     *
     * Old tags are inserted to a temporary buffer with position:
     *     tempbuf_insert(position/ENTRY_CHUNK_SIZE, ...);
     * And new tags with index:
     *     tempbuf_insert(idx, ...);
     *
     * The buffer is sorted and written into the new tag file, after which
     *     new_seek = tempbuf_find_location(old_seek, ...);
     * for old tags and
     *     new_seek = tempbuf_find_location(idx);
     * for new ones fill `merge` for the master.
     */
    lookup = (struct tempbuf_searchidx **)&tempbuf[tempbuf_pos];
    tempbuf_pos += lookup_buffer_depth * sizeof(void **);
    memset(lookup, 0, lookup_buffer_depth * sizeof(void **));

    /* And calculate the remaining data space used mainly for storing
     * tag data (strings). */
    tempbuf_left = tempbuf_size - tempbuf_pos - 8;
    if (tempbuf_left - TAGFILE_ENTRY_AVG_LENGTH * commit_entry_count < 0)
    {
        logf("Buffer way too small!");
        if (fd >= 0)
            close(fd);
        return 0;
    }

    if (fd >= 0 && sorted)
    {
        /**
         * A sorted tag file is loaded entirely into memory so it can be
         * resorted with the new strings.
         */
        logf("loading tags...");
        for (i = 0; i < tch.entry_count && !USR_CANCEL; i++)
        {
            struct tagfile_entry entry;
            int loc = lseek(fd, 0, SEEK_CUR);
            bool ret;
            switch (read_tagfile_entry_and_tag(fd, &entry, build_idx_buf, build_idx_bufsz))
            {
                case e_SUCCESS_LEN_ZERO: /* Skip deleted entries. */
                    continue;
                case e_SUCCESS:
                     break;
                case e_ENTRY_SIZEMISMATCH:
                    logf("read error #7");
                    close(fd);
                    return -2;
                case e_TAG_TOOLONG:
                    logf("too long tag #3");
                    close(fd);
                    return -2;
                case e_TAG_SIZEMISMATCH:
                    logf("read error #8");
                    close(fd);
                    return -2;
            }

            /* An interrupted commit already merged this entry for a new
             * track; it is inserted again from the temp file below. */
            if (!TAGCACHE_IS_UNIQUE(index_type)
                && entry.idx_id >= master_count)
                continue;

            /**
             * Save the tag and tag id in the memory buffer. Tag id
             * is saved so we can later reindex the master lookup
             * table when the index gets resorted.
             */
            ret = tempbuf_insert(build_idx_buf, loc/TAGFILE_ENTRY_CHUNK_LENGTH
                                 + commit_entry_count, entry.idx_id,
                                 TAGCACHE_IS_UNIQUE(index_type));
            if (!ret)
            {
                close(fd);
                return -3;
            }
            do_timed_yield();
        }
        logf("done");
        close(fd);
        fd = -1;
    }
    else if (fd >= 0)
    {
        tempbufidx = tch.entry_count;
        /* Entries past the master's count were appended by an interrupted
         * commit, and are appended again below. The file holds one entry
         * per master entry, in order. */
        if (tch.entry_count > master_count)
        {
            off_t pos = sizeof(struct tagcache_header);
            for (i = 0; i < master_count; i++)
            {
                struct tagfile_entry entry;
                lseek(fd, pos, SEEK_SET);
                if (read_tagfile_entry(fd, &entry)
                    != (ssize_t)sizeof(entry))
                {
                    logf("read error #9");
                    close(fd);
                    return -2;
                }
                pos += sizeof(entry) + entry.tag_length;
            }
            ftruncate(fd, pos);
            tempbufidx = master_count;
        }
    }
    else if (!sorted)
    {
        logf("Create New Index: %d", index_type);
        /* The filename file starts afresh with the master. The database path
         * already exists by this point, so no mkdir is required. */
        fd = open_pathfmt(build_idx_buf, build_idx_bufsz,
                          O_RDWR | O_CREAT | O_TRUNC,
                          "%s/" TAGCACHE_FILE_INDEX,
                          tc_stat.db_path, index_type);
        if (fd < 0)
        {
            logf(TAGCACHE_FILE_INDEX " open fail", index_type);
            return -2;
        }

        tch.magic = TAGCACHE_MAGIC;
        tch.entry_count = 0;
        tch.datasize = 0;

        if (write_tagcache_header(fd, &tch) != sizeof(struct tagcache_header))
        {
            logf("header write failed");
            close(fd);
            return -2;
        }
    }

    if (sorted)
    {
        /**
         * Load new unique tags in memory to be sorted later and added
         * to the master lookup file.
         */
        lseek(tmpfd, sizeof(struct tagcache_header), SEEK_SET);
        /* h is the header of the temporary file containing new tags. */
        logf("inserting new tags...");
        for (i = 0; i < h->entry_count && !USR_CANCEL; i++)
        {
            struct temp_file_entry entry;

            if (read(tmpfd, &entry, sizeof(struct temp_file_entry)) !=
                sizeof(struct temp_file_entry))
            {
                logf("read fail #3");
                return -2;
            }

            /* Read data. */
            if (entry.tag_length[index_type] >= build_idx_bufsz)
            {
                logf("too long entry!");
                return -2;
            }

            lseek(tmpfd, entry.tag_offset[index_type], SEEK_CUR);
            if (read(tmpfd, build_idx_buf, entry.tag_length[index_type]) !=
                entry.tag_length[index_type])
            {
                logf("read fail #4");
                return -2;
            }
            str_setlen(build_idx_buf, entry.tag_length[index_type]);

            if (TAGCACHE_IS_UNIQUE(index_type))
                error = !tempbuf_insert(build_idx_buf, i, -1, true);
            else
                error = !tempbuf_insert(build_idx_buf, i,
                                        master_count + i, false);

            if (error)
            {
                logf("insert error");
                return -2;
            }
            /* Skip to next. */
            lseek(tmpfd, entry.data_length - entry.tag_offset[index_type] -
                    entry.tag_length[index_type], SEEK_CUR);
            do_timed_yield();
        }
        logf("done");

        if (commit_cancelled)
            return 1;

        /* Sort the buffer data and write it to the new index file. */
        outfd = open_pathfmt(build_idx_buf, build_idx_bufsz,
                             O_WRONLY | O_CREAT | O_TRUNC,
                             "%s/" TAGCACHE_FILE_INDEX ".new",
                             tc_stat.db_path, index_type);
        if (outfd < 0)
            return -2;

        tch.magic = TAGCACHE_MAGIC;
        tch.entry_count = 0;
        tch.datasize = 0;
        if (write_tagcache_header(outfd, &tch) != sizeof(struct tagcache_header)
            || (i = tempbuf_sort(outfd)) < 0)
        {
            close(outfd);
            return -2;
        }
        logf("sorted %d tags", i);

        /* Where every old and new string now is, for the master. */
        for (i = 0; i < merge.remap_len[index_type]; i++)
            merge.remap[index_type][i] =
                tempbuf_find_location(i + commit_entry_count);

        for (i = 0; i < h->entry_count; i++)
        {
            merge.newseek[index_type][i] = tempbuf_find_location(i);
            if (merge.newseek[index_type][i] < 0)
            {
                logf("entry not found (%d)", i);
                close(outfd);
                return -2;
            }
        }

        tch.entry_count = tempbufidx;
        tch.datasize = lseek(outfd, 0, SEEK_END)
                       - sizeof(struct tagcache_header);
        lseek(outfd, 0, SEEK_SET);
        write_tagcache_header(outfd, &tch);
        close(outfd);

        h->datasize += tch.datasize;
        logf("s:%d/%" PRId32 "/%" PRId32, index_type, tch.datasize, h->datasize);
        return 1;
    }

    /* The filename tag: appended in place, which old entries do not see. */
    if (commit_cancelled)
    {
        close(fd);
        return 1;
    }

    logf("appending new entries...");
    lseek(tmpfd, sizeof(struct tagcache_header), SEEK_SET);
    lseek(fd, 0, SEEK_END);
    for (i = 0; i < h->entry_count && !error; i++)
    {
        struct temp_file_entry entry;
        struct tagfile_entry fe;

        if (read(tmpfd, &entry, sizeof(struct temp_file_entry)) !=
            sizeof(struct temp_file_entry))
        {
            logf("read fail #7");
            error = true;
            break;
        }

        if (entry.tag_length[index_type] >= build_idx_bufsz)
        {
            logf("too long entry!");
            error = true;
            break;
        }

        lseek(tmpfd, entry.tag_offset[index_type], SEEK_CUR);
        if (read(tmpfd, build_idx_buf, entry.tag_length[index_type]) !=
            entry.tag_length[index_type])
        {
            logf("read fail #8");
            error = true;
            break;
        }

        merge.newseek[index_type][i] = lseek(fd, 0, SEEK_CUR);
        fe.tag_length = entry.tag_length[index_type];
        fe.idx_id = master_count + i;
        if (write_tagfile_entry(fd, &fe) != sizeof(fe)
            || write(fd, build_idx_buf, fe.tag_length) != fe.tag_length)
        {
            logf("tagcache: write fail #4");
            error = true;
            break;
        }
        tempbufidx++;

        /* Skip to next. */
        lseek(tmpfd, entry.data_length - entry.tag_offset[index_type] -
              entry.tag_length[index_type], SEEK_CUR);
        do_timed_yield();
    }

    if (!error)
    {
        tch.magic = TAGCACHE_MAGIC;
        tch.entry_count = tempbufidx;
        tch.datasize = lseek(fd, 0, SEEK_END) - sizeof(struct tagcache_header);
        lseek(fd, 0, SEEK_SET);
        write_tagcache_header(fd, &tch);
    }
    close(fd);

    return error ? -2 : 1;
}

/* After every tag is built: write the master once, with every old entry's
 * sorted seeks moved to where build_index() put their strings and every new
 * entry's seeks filled in, to TAGCACHE_FILE_MASTER ".new". The numeric pass
 * fills the new entries' figures after the swap. */
static bool merge_master(const struct tagcache_header *h)
{
    struct master_header tcmh;
    struct index_entry idxbuf[IDX_BUF_DEPTH];
    int oldfd, outfd;
    long i, n, k;
    int t;
    bool ok = true;

    oldfd = open_master_fd(&tcmh, false);
    if (oldfd < 0)
    {
        memset(&tcmh, 0, sizeof(struct master_header));
        tcmh.tch = *h;
        tcmh.tch.entry_count = 0;
        tcmh.tch.datasize = 0;
    }
    tcmh.dirty = true;

    outfd = open_db_fd(TAGCACHE_FILE_MASTER ".new",
                       O_WRONLY | O_CREAT | O_TRUNC);
    if (outfd < 0)
    {
        if (oldfd >= 0)
            close(oldfd);
        return false;
    }

    if (write_master_header(outfd, &tcmh) != sizeof(struct master_header))
        ok = false;

    for (i = 0; ok && i < tcmh.tch.entry_count; i += n)
    {
        n = MIN(tcmh.tch.entry_count - i, IDX_BUF_DEPTH);
        if (read_index_entries(oldfd, idxbuf, n)
            != (ssize_t)sizeof(struct index_entry) * n)
        {
            logf("read fail #5");
            ok = false;
            break;
        }

        for (k = 0; ok && k < n; k++)
        {
            /* A deleted entry's string seeks hold CRCs; it keeps them. */
            if (idxbuf[k].flag & FLAG_DELETED)
                continue;

            for (t = 0; t < TAG_COUNT; t++)
            {
                if (!TAGCACHE_IS_SORTED(t))
                    continue;

                long chunk = idxbuf[k].tag_seek[t] / TAGFILE_ENTRY_CHUNK_LENGTH;
                int32_t seek = chunk >= 0 && chunk < merge.remap_len[t]
                               ? merge.remap[t][chunk] : -1;
                if (seek < 0)
                {
                    logf("update error: %" PRId32 "/%ld/%d",
                         idxbuf[k].flag, i + k, t);
                    ok = false;
                    break;
                }
                idxbuf[k].tag_seek[t] = seek;
            }
            do_timed_yield();
        }

        if (ok && write_index_entries(outfd, idxbuf, n)
                  != (ssize_t)sizeof(struct index_entry) * n)
            ok = false;
    }

    for (i = 0; ok && i < h->entry_count; i += n)
    {
        n = MIN(h->entry_count - i, IDX_BUF_DEPTH);
        memset(idxbuf, 0, sizeof(struct index_entry) * n);
        for (k = 0; k < n; k++)
            for (t = 0; t < TAG_COUNT; t++)
                if (!TAGCACHE_IS_NUMERIC(t))
                    idxbuf[k].tag_seek[t] = merge.newseek[t][i + k];

        if (write_index_entries(outfd, idxbuf, n)
            != (ssize_t)sizeof(struct index_entry) * n)
            ok = false;
        do_timed_yield();
    }

    if (oldfd >= 0)
        close(oldfd);
    close(outfd);
    return ok;
}

/* Drop the .new files of a commit that did not finish. */
static void merge_discard(void)
{
    char name[32];

    for (int t = 0; t < TAG_COUNT; t++)
    {
        if (!TAGCACHE_IS_SORTED(t))
            continue;
        snprintf(name, sizeof(name), TAGCACHE_FILE_INDEX ".new", t);
        remove_db_file(name);
    }
    remove_db_file(TAGCACHE_FILE_MASTER ".new");
}

static bool db_file_exists(const char *filename);

/* The renames of a swap: every .new file still there over its real one */
static bool swap_renames(void)
{
    char name[32];
    char real[32];
    bool ok = true;

    for (int t = 0; t < TAG_COUNT; t++)
    {
        if (!TAGCACHE_IS_SORTED(t))
            continue;
        snprintf(name, sizeof(name), TAGCACHE_FILE_INDEX ".new", t);
        snprintf(real, sizeof(real), TAGCACHE_FILE_INDEX, t);
        if (db_file_exists(name))
            ok &= rename_db_file(name, real);
    }
    if (db_file_exists(TAGCACHE_FILE_MASTER ".new"))
        ok &= rename_db_file(TAGCACHE_FILE_MASTER ".new", TAGCACHE_FILE_MASTER);
    return ok;
}

/* Put the new files in place: the tag files, then the master. Each rename
 * replaces its target whole, but a cut between two would pair new tag files
 * with the old master. So the marker goes down first, once every .new file
 * is complete, and finish_interrupted_swap() completes a swap it finds at
 * the next boot. Returns 1 when done, 0 when a rename failed part way, and
 * -1 when the marker could not be made and nothing was renamed. */
static int merge_swap(void)
{
    int fd = open_db_fd(TAGCACHE_FILE_SWAP, O_WRONLY | O_CREAT | O_TRUNC);

    if (fd < 0)
        return -1;
    close(fd);
    if (!swap_renames())
        return 0;
    remove_db_file(TAGCACHE_FILE_SWAP);
    return 1;
}

/* At boot, before anything opens the database: a swap that was cut short is
 * carried through, its .new files having been complete when it began. */
static void finish_interrupted_swap(void)
{
    if (!db_file_exists(TAGCACHE_FILE_SWAP))
        return;
    debug_log(DEBUG_LOG_TAGCACHE, "swap: finishing one cut short");
    if (swap_renames())
        remove_db_file(TAGCACHE_FILE_SWAP);
}

/* Lay out `merge` at the front of tempbuf: a remap table per sorted tag,
 * sized from its file, and a seek per new entry for every string tag.
 * Returns the bytes taken, or 0 when tempbuf will not hold them. */
static size_t merge_layout(const struct tagcache_header *h)
{
    struct master_header mhdr;
    struct tagcache_header thdr;
    size_t need = 0;
    int32_t *p;
    bool master = false;
    int fd;

    fd = open_master_fd(&mhdr, false);
    if (fd >= 0)
    {
        master = true;
        close(fd);
    }

    for (int t = 0; t < TAG_COUNT; t++)
    {
        merge.remap_len[t] = 0;
        if (TAGCACHE_IS_NUMERIC(t))
            continue;
        if (master && TAGCACHE_IS_SORTED(t)
            && (fd = open_tag_fd(&thdr, t, false)) >= 0)
        {
            merge.remap_len[t] = (sizeof(struct tagcache_header)
                                  + thdr.datasize)
                                 / TAGFILE_ENTRY_CHUNK_LENGTH + 1;
            close(fd);
        }
        need += (merge.remap_len[t] + h->entry_count) * sizeof(int32_t);
    }

    need = ALIGN_UP(need, sizeof(void *));
    if (need >= tempbuf_size)
        return 0;

    p = (int32_t *)tempbuf;
    for (int t = 0; t < TAG_COUNT; t++)
    {
        merge.remap[t] = NULL;
        merge.newseek[t] = NULL;
        if (TAGCACHE_IS_NUMERIC(t))
            continue;
        merge.remap[t] = p;
        p += merge.remap_len[t];
        merge.newseek[t] = p;
        p += h->entry_count;
    }

    tempbuf += need;
    tempbuf_size -= need;
    return need;
}

/* A generous estimate of the buffer a merge needs: build_index()'s index and
 * lookup tables for every entry, old and new, and the largest sorted tag file
 * twice over, plus the new tag data. */
static size_t commit_need(const struct tagcache_header *tmp)
{
    struct master_header mhdr;
    struct tagcache_header thdr;
    size_t biggest = 0;
    long count = tmp->entry_count + 1;
    int fd;

    fd = open_master_fd(&mhdr, false);
    if (fd >= 0)
    {
        count += mhdr.tch.entry_count;
        close(fd);
    }

    for (int tag = 0; tag < TAG_COUNT; tag++)
    {
        if (!TAGCACHE_IS_SORTED(tag))
            continue;
        fd = open_tag_fd(&thdr, tag, false);
        if (fd >= 0)
        {
            biggest = MAX(biggest, (size_t)thdr.datasize);
            close(fd);
        }
    }

    return count * (sizeof(struct tempbuf_searchidx) + sizeof(void *))
           + 2 * biggest + tmp->datasize + 65536
           + (biggest / 2 + tmp->entry_count * 4) * TAG_COUNT;
}

/* The commit id stamped past the temp file's last entry, at `end`, or -1
 * for an unstamped file. Every reader of the file stops at its last entry,
 * so the stamp is invisible to them. */
static int32_t temp_stamp_read(int tmpfd, off_t end)
{
    int32_t stamp[2];

    if (lseek(tmpfd, end, SEEK_SET) != end
        || read(tmpfd, stamp, sizeof(stamp)) != (ssize_t)sizeof(stamp)
        || stamp[0] != TAGCACHE_TEMP_STAMP)
        return -1;
    return stamp[1];
}

/* Unstamped on failure, which costs only the guard: a cut between the swap
 * and removing the file then merges it twice. */
static void temp_stamp_write(off_t end, int32_t commitid)
{
    int32_t stamp[2] = { TAGCACHE_TEMP_STAMP, commitid };
    int fd = open_db_fd(TAGCACHE_FILE_TEMP, O_WRONLY);

    if (fd < 0)
        return;
    if (lseek(fd, end, SEEK_SET) == end)
        write(fd, stamp, sizeof(stamp));
    close(fd);
}

static bool commit(void)
{
    struct tagcache_header tch;
    struct master_header   tcmh;
    int i, len, rc;
    int tmpfd;
    off_t tmp_end;
    int masterfd;
    bool dircache_buffer_stolen = false;
    bool ramcache_buffer_stolen = false;
    size_t merge_bytes = 0;
    bool tempbuf_ours = false;
    struct tagcache_header filename_hdr;
    off_t filename_size = 0;
    bool filename_hdr_ok = false;
    const bool ram_was_on = tc_stat.ramcache;
    const bool ram_was_current = tcramcache.current;
    const bool dirty_before = current_tcmh.dirty;
    bool db_untouched = false;
    int swapped;
    logf("committing tagcache");

    commit_cancelled = false;

    while (write_lock)
        sleep(1);

    /* A swap the marker says is unfinished leaves old and new files mixed;
     * a merge over them would build on the mix. */
    finish_interrupted_swap();
    if (db_file_exists(TAGCACHE_FILE_SWAP))
    {
        logf("swap unfinished, delaying commit");
        tc_stat.commit_delayed = true;
        return false;
    }

    int fd = open_db_fd(TAGCACHE_FILE_NOCOMMIT, O_RDONLY);
    if (fd >= 0)
    {
        logf("canceling commit");
        tc_stat.commit_delayed = true;
        close(fd);
        tmpfd = -1;
    }
    else
    {
        tmpfd = open_db_fd(TAGCACHE_FILE_TEMP, O_RDONLY);
    }
    if (tmpfd < 0)
    {
        logf("nothing to commit");
        return true;
    }


    /* Load the header. */
    len = sizeof(struct tagcache_header);
    rc = read(tmpfd, &tch, len);

    if (tch.magic != TAGCACHE_MAGIC || rc != len)
    {
        logf("incorrect tmpheader");
        close(tmpfd);
        remove_db_file(TAGCACHE_FILE_TEMP);
        return false;
    }

    /* Fully initialize existing headers (if any) before going further. */
    tc_stat.ready = check_all_headers();

    /* A temp file stamped with the master's own commit id was merged by a
     * commit cut short between the swap and removing the file. */
    tmp_end = sizeof(struct tagcache_header)
              + (off_t)tch.entry_count * sizeof(struct temp_file_entry)
              + tch.datasize;
    if (tc_stat.ready
        && temp_stamp_read(tmpfd, tmp_end) == current_tcmh.commitid)
    {
        logf("tmpfile already merged");
        close(tmpfd);
        remove_db_file(TAGCACHE_FILE_TEMP);
        return true;
    }

    if (tch.entry_count == 0 && tc_stat.ready)
    {
        /* Nothing was added, so there is nothing to merge -- and this is the
         * ordinary outcome of a scan rather than an error: add_tagcache()
         * skips every file whose mtime is unchanged, so a library that has
         * not moved produces an empty temp file.
         *
         * Going on would rewrite all ten index files with the contents they
         * already have, and would drop the RAM copy below without asking for
         * it back -- only a commit that added entries starts the scan that
         * reloads it. That is what left every search reading the disk, at a
         * whole master-index scan each, for the rest of a session in which a
         * host had written anything.
         *
         * tc_stat.ready is in the test because rewriting those files is also
         * how a dirty database is repaired: check_all_headers() above reports
         * a half-finished commit as not ready, and that one has to go the
         * long way round even with nothing of its own to add. */
        logf("nothing to commit");
        close(tmpfd);
        remove_db_file(TAGCACHE_FILE_TEMP);
        return true;
    }

    /* At first be sure to unload the ramcache! */
    tc_stat.ramcache = false;
    tcramcache.current = false;

    /* Beyond here, jump to commit_error to undo locks and restore dircache */
    rc = false;
    /* Under the queue's mutex, so a flush already writing the master
     * finishes before the merge reads it, and none starts after. */
    mutex_lock(&command_queue_mutex);
    read_lock++;
    mutex_unlock(&command_queue_mutex);

    /* Again under read_lock: a search can start between the wait at the top
     * and here, and may be walking the RAM copy lent out below. */
    while (write_lock)
        sleep(1);

    /* Try to steal every buffer we can :) */
    if (tempbuf_size == 0)
    {
        /* Free memory first. Freeing the dircache means rebuilding it after
         * the merge -- seconds on a disk, which the reload then waits out --
         * so it is taken only for a merge free memory will not hold. */
        allocate_tempbuf();
        tempbuf_ours = true;
        if ((size_t)tempbuf_size < commit_need(&tch))
        {
            free_tempbuf();
            dircache_free_buffer();
            dircache_buffer_stolen = true;

            allocate_tempbuf();
        }
    }

    if (tempbuf_size == 0 && tc_stat.ramcache_allocated > 0)
    {
        tcrc_buffer_lock();
        tempbuf = (char *)(tcramcache.hdr + 1);
        tempbuf_size = tc_stat.ramcache_allocated - sizeof(struct ramcache_header) - 128;
        tempbuf_size &= ~0x03;
        ramcache_buffer_stolen = true;
    }


    /* And finally fail if there are no buffers available. */
    if (tempbuf_size == 0)
    {
        logf("delaying commit until next boot");
        tc_stat.commit_delayed = true;
        close(tmpfd);
        db_untouched = true;
        goto commit_error;
    }

    logf("commit %" PRId32 " entries...", tch.entry_count);

    merge_bytes = merge_layout(&tch);
    if (merge_bytes == 0)
    {
        logf("no room for the merge maps");
        tc_stat.commit_delayed = true;
        close(tmpfd);
        db_untouched = true;
        goto commit_error;
    }

    /* The filename file is appended to in place; this is what to cut it back
     * to if the commit does not finish. */
    {
        struct tagcache_header fh;
        int ffd = open_tag_fd(&fh, tag_filename, false);
        filename_hdr_ok = ffd >= 0;
        if (filename_hdr_ok)
        {
            filename_hdr = fh;
            filename_size = ffilesize(ffd);
            close(ffd);
        }
    }

    /* Mark DB dirty so it will stay disabled if commit fails. */
    current_tcmh.dirty = true;
    update_master_header();

    /* Now create the index files. */
    tc_stat.commit_step = 0;
    tch.datasize = 0;
    tc_stat.commit_delayed = false;

    for (i = 0; i < TAG_COUNT && !USR_CANCEL; i++)
    {
        int ret;

        if (TAGCACHE_IS_NUMERIC(i))
            continue;

        tc_stat.commit_step++;
        ret = build_index(i, &tch, tmpfd);
        if (ret <= 0)
        {
            close(tmpfd);
            logf("tagcache failed init");
            if (ret == 0)
                tc_stat.commit_delayed = true;

            tc_stat.commit_step = 0;
            goto merge_error;
        }
        do_timed_yield();
    }

    /* Until the swap nothing old has been touched but the filename file's
     * tail, so a cancel or a failure here leaves the database as it was. */
    if (USR_CANCEL || !merge_master(&tch) || USR_CANCEL)
    {
        close(tmpfd);
        tc_stat.commit_step = 0;
        goto merge_error;
    }

    if (!build_numeric_indices(&tch, tmpfd))
    {
        logf("Failure to commit numeric indices");
        close(tmpfd);
        tc_stat.commit_step = 0;
        goto merge_error;
    }

    close(tmpfd);

    /* The master's .new file is finished whole, header included, so the
     * swap alone commits: a cut before it leaves the old database, one
     * after it the new, which the temp file's stamp then identifies. */
    masterfd = open_master_file(TAGCACHE_FILE_MASTER ".new", &tcmh, true);
    if (masterfd < 0)
    {
        tc_stat.commit_step = 0;
        goto merge_error;
    }

    tcmh.tch.entry_count += tch.entry_count;
    tcmh.tch.datasize = sizeof(struct master_header)
        + sizeof(struct index_entry) * tcmh.tch.entry_count
        + tch.datasize;
    tcmh.dirty = false;
    tcmh.commitid++;

    lseek(masterfd, 0, SEEK_SET);
    if (write_master_header(masterfd, &tcmh) != sizeof(struct master_header))
    {
        close(masterfd);
        tc_stat.commit_step = 0;
        goto merge_error;
    }
    close(masterfd);

    temp_stamp_write(tmp_end, tcmh.commitid);

    swapped = merge_swap();
    if (swapped < 0)
    {
        logf("swap marker failed");
        tc_stat.commit_step = 0;
        goto merge_error;
    }
    if (swapped == 0)
    {
        /* Old and new files are mixed until the next boot finishes the
         * swap; nothing may read or write them meanwhile. */
        logf("merge swap failed");
        tc_stat.commit_step = 0;
        tc_stat.ready = false;
        tc_stat.ramcache = false;
        goto commit_error;
    }

    tc_stat.commit_step = 0;

    /* Past the swap the commit finishes, cancel or not. */
    {
        /* Only now: the next boot needs it until the swap is done. */
        remove_db_file(TAGCACHE_FILE_TEMP);

        logf("tagcache committed");
        tagcache_commit_finalize();

        if (ramcache_buffer_stolen)
        {
            tempbuf = NULL;
            tempbuf_size = 0;
            ramcache_buffer_stolen = false;
            tcrc_buffer_unlock();
        }

        /* Ask for the RAM copy back: this rewrote the index files under it.
         * A reload only -- the scan that led here has already walked the disk
         * and checked for deletions, and a second scan would do both again. */
        if (tc_stat.ramcache_allocated > 0)
            queue_post(&tagcache_queue, Q_RELOAD_RAMCACHE, 0);

        rc = true;
    }
    goto commit_error;  /* success shares the cleanup below */

merge_error:
    merge_discard();
    if (filename_hdr_ok)
    {
        char name[32];
        snprintf(name, sizeof(name), TAGCACHE_FILE_INDEX, tag_filename);
        int ffd = open_db_fd(name, O_RDWR);
        if (ffd >= 0)
        {
            ftruncate(ffd, filename_size);
            write_tagcache_header(ffd, &filename_hdr);
            close(ffd);
            db_untouched = true;
        }
    }
    else
        db_untouched = true;

    /* The database is as it was before the commit, so it is clean again. */
    if (db_untouched)
    {
        current_tcmh.dirty = dirty_before;
        update_master_header();
    }

commit_error:
    if (merge_bytes)
    {
        tempbuf -= merge_bytes;
        tempbuf_size += merge_bytes;
    }

    if (ramcache_buffer_stolen)
    {
        tempbuf = NULL;
        tempbuf_size = 0;
        tcrc_buffer_unlock();
    }

    read_lock--;

    /* A buffer allocated here is freed here, whether or not the dircache
     * was given up for it. */
    if (tempbuf_ours)
        free_tempbuf();

    /* Resume the dircache, if we stole the buffer. */
    if (dircache_buffer_stolen)
        dircache_resume();

    /* A commit that changed nothing hands back the RAM copy it switched off,
     * or reloads it when its buffer served as scratch. */
    if (db_untouched && ram_was_current)
    {
        if (!ramcache_buffer_stolen)
        {
            tcramcache.current = true;
            tc_stat.ramcache = ram_was_on;
        }
        else if (ram_was_on && tc_stat.ramcache_allocated > 0)
            queue_post(&tagcache_queue, Q_RELOAD_RAMCACHE, 0);
    }

    return rc;
}

void tagcache_commit_finalize(void)
{
    tc_stat.ready = check_all_headers();
    tc_stat.readyvalid = true;
}


static bool modify_numeric_entry(int masterfd, int idx_id, int tag, long data)
{
    struct index_entry idx;

    if (!tc_stat.ready)
        return false;

    if (!TAGCACHE_IS_NUMERIC(tag))
        return false;

    if (!get_index(masterfd, idx_id, &idx, false))
        return false;

    idx.tag_seek[tag] = data;
    idx.flag |= FLAG_DIRTYNUM;

    return write_index(masterfd, idx_id, &idx);
}


static bool command_queue_is_full(void)
{
    int next;

    next = command_queue_widx + 1;
    if (next >= TAGCACHE_COMMAND_QUEUE_LENGTH)
        next = 0;

    return (next == command_queue_ridx);
}

static void command_queue_sync_callback(void)
{
    struct master_header myhdr;
    int masterfd;

    mutex_lock(&command_queue_mutex);

    /* A commit rewriting the master would write over these, so they wait for
     * the next flush; so does a missing master, which a rebuild deletes until
     * its merge. The mutex is left free for whoever queues meanwhile. */
    if (read_lock || (masterfd = open_master_fd(&myhdr, true)) < 0)
    {
        mutex_unlock(&command_queue_mutex);
        return;
    }

    while (command_queue_ridx != command_queue_widx)
    {
        struct tagcache_command_entry *ce = &command_queue[command_queue_ridx];

        switch (ce->command)
        {
            case CMD_UPDATE_MASTER_HEADER:
            {
                close(masterfd);
                update_master_header();

                /* Re-open the masterfd. */
                if ( (masterfd = open_master_fd(&myhdr, true)) < 0)
                {
                    mutex_unlock(&command_queue_mutex);
                    return;
                }

                break;
            }
            case CMD_UPDATE_NUMERIC:
            {
                modify_numeric_entry(masterfd, ce->idx_id, ce->tag, ce->data);
                break;
            }
        }

        if (++command_queue_ridx >= TAGCACHE_COMMAND_QUEUE_LENGTH)
            command_queue_ridx = 0;
    }

    close(masterfd);

    tc_stat.queue_length = 0;
    mutex_unlock(&command_queue_mutex);
}

static void run_command_queue(bool force)
{
    if (COMMAND_QUEUE_IS_EMPTY)
        return;

    if (force || command_queue_is_full())
        command_queue_sync_callback();
    else
        register_storage_idle_func(command_queue_sync_callback);
}

static void queue_command(int cmd, long idx_id, int tag, long data)
{
    while (1)
    {
        int next;

        mutex_lock(&command_queue_mutex);

        /* A header write stores the header as it is when flushed, so one
         * queued is enough. */
        if (cmd == CMD_UPDATE_MASTER_HEADER)
        {
            int ridx;
            for (ridx = command_queue_ridx; ridx != command_queue_widx;
                 ridx = (ridx + 1) % TAGCACHE_COMMAND_QUEUE_LENGTH)
            {
                if (command_queue[ridx].command == CMD_UPDATE_MASTER_HEADER)
                {
                    mutex_unlock(&command_queue_mutex);
                    return;
                }
            }
        }

        next = command_queue_widx + 1;
        if (next >= TAGCACHE_COMMAND_QUEUE_LENGTH)
            next = 0;

        /* Make sure queue is not full. */
        if (next != command_queue_ridx)
        {
            struct tagcache_command_entry *ce = &command_queue[command_queue_widx];

            ce->command = cmd;
            ce->idx_id = idx_id;
            ce->tag = tag;
            ce->data = data;

            command_queue_widx = next;

            tc_stat.queue_length++;

            mutex_unlock(&command_queue_mutex);
            break;
        }

        /* Full during a commit: nothing flushes until the merge ends, minutes
         * on a large library, and the caller is the audio thread at a track
         * change. The change is dropped rather than stopping playback. */
        if (read_lock)
        {
            mutex_unlock(&command_queue_mutex);
            logf("command queue full during commit, dropped %d", cmd);
            return;
        }

        /* Full outside a commit: flush it here rather than wait for a thread
         * that may not flush until a scan ends, and wait only if that could
         * not drain it. */
        mutex_unlock(&command_queue_mutex);
        command_queue_sync_callback();
        if (command_queue_is_full())
            sleep(1);
    }
}

long tagcache_increase_serial(void)
{
    long old;

    if (!tc_stat.ready)
        return -2;

    /* No wait for a commit: the audio thread calls this at every track end.
     * A commit rereads the header as it finishes, so an increment made during
     * one can be lost and two plays share a serial -- a tie in "recently
     * played", nothing worse. */
    old = current_tcmh.serial++;
    queue_command(CMD_UPDATE_MASTER_HEADER, 0, 0, 0);

    return old;
}

/* Dropped while the database is not ready: an index taken before a rebuild
 * would land on whichever track holds that number afterwards. */
void tagcache_update_numeric(int idx_id, int tag, long data)
{
    if (!tc_stat.ready)
        return;
    queue_command(CMD_UPDATE_NUMERIC, idx_id, tag, data);
}

uint32_t tagcache_generation(void)
{
    return db_generation;
}

static bool write_tag(int fd, const char *tagstr, const char *datastr)
{
    char buf[512];
    const int bufsz = sizeof(buf);
    int i;

    snprintf(buf, bufsz, "%s=\"", tagstr);

    for (i = strlen(buf); i < (long)sizeof(buf)-4; i++)
    {
        if (*datastr == '\0')
            break;

        if (*datastr == '"' || *datastr == '\\')
            buf[i++] = '\\';

        else if (*datastr == '\n')
        {
            buf[i++] = '\\';
            buf[i] = 'n';
            datastr++;
            continue;
        }

        buf[i] = *(datastr++);
    }

    str_setlen(buf, bufsz - 1);
    strmemccpy(&buf[i], "\" ", (bufsz - i - 1));

    return write(fd, buf, i + 2) == i + 2;
}


static bool read_tag(char *dest, long size,
                     const char *src, const char *tagstr)
{
    int pos;
    char current_tag[32];

    while (*src != '\0')
    {
        /* Skip all whitespace */
        while (*src == ' ')
            src++;

        if (*src == '\0')
            break;

        pos = 0;
        /* Read in tag name */
        while (*src != '=' && *src != ' ')
        {
            current_tag[pos] = *src;
            src++;
            pos++;

            if (*src == '\0' || pos >= (int) sizeof(current_tag))
                return false;
        }

        str_setlen(current_tag, pos);

        /* Read in tag data */

        /* Find the start. */
        while (*src != '"' && *src != '\0')
            src++;

        if (*src == '\0' || *(++src) == '\0')
            return false;

        /* Read the data, leaving room for the terminator. */
        for (pos = 0; pos < size - 1; pos++)
        {
            if (*src == '\0')
                break;

            if (*src == '\\')
            {
                src++;
                if (*src == '\0')
                    break;
                if (*src == 'n')
                    dest[pos] = '\n';
                else
                    dest[pos] = *src;

                src++;
                continue;
            }

            if (*src == '\0')
                break;

            if (*src == '"')
            {
                src++;
                break;
            }

            dest[pos] = *(src++);
        }

        str_setlen(dest, pos);

        if (!strcasecmp(tagstr, current_tag))
            return true;
    }

    return false;
}

/* The runtime figures, in the order both saved forms carry them */
static const int runtime_tags[] = {
    tag_playcount, tag_rating, tag_playtime, tag_lastplayed, tag_commitid,
    tag_lastelapsed, tag_lastoffset
};
#define RUNTIME_TAGS ((int)ARRAYLEN(runtime_tags))

/* One entry of plays.dat: the figures, then the filename's length; the
 * filename follows it, unterminated. */
struct runtime_rec {
    int32_t  data[RUNTIME_TAGS];
    uint16_t name_len;
} __attribute__((packed));

/* Folds one file's figures into the database, a negative one meaning none.
 * An entry already changed since is left as it is. False only when the
 * master could not be written. */
static bool import_one(int masterfd, const char *filename, const long *data)
{
    struct index_entry idx;
    int idx_id = find_index(filename);

    if (idx_id < 0 || !get_index(masterfd, idx_id, &idx, false)
        || (idx.flag & FLAG_DIRTYNUM))
        return true;

    idx.flag |= FLAG_DIRTYNUM;
    for (int i = 0; i < RUNTIME_TAGS; i++)
    {
        int tag = runtime_tags[i];

        if (data[i] < 0)
            continue;
        idx.tag_seek[tag] = data[i];
        if (tag == tag_lastplayed && data[i] >= current_tcmh.serial)
            current_tcmh.serial = data[i] + 1;
        else if (tag == tag_commitid && data[i] >= current_tcmh.commitid)
            current_tcmh.commitid = data[i] + 1;
    }

    return write_index(masterfd, idx_id, &idx);
}

/* One line of an export: tag="value" pairs, the filename among them */
static int parse_changelog_line(int line_n, char *buf, void *parameters)
{
    char tag_data[TAGCACHE_BUFSZ];
    char filename[TAGCACHE_BUFSZ];
    long data[RUNTIME_TAGS];
    long masterfd = (long)(intptr_t)parameters;
    (void)line_n;

    if (*buf == '#')
        return 0;
    if (!read_tag(filename, sizeof filename, buf, "filename"))
        return 0;

    for (int i = 0; i < RUNTIME_TAGS; i++)
    {
        data[i] = -1;
        if (read_tag(tag_data, sizeof tag_data, buf,
                     tagcache_tag_to_str(runtime_tags[i])))
            data[i] = atoi(tag_data);
    }

    return import_one(masterfd, filename, data) ? 0 : -5;
}

/* Every entry of plays.dat */
static bool import_runtime_records(int fd, int masterfd)
{
    struct runtime_rec rec;
    char filename[TAGCACHE_BUFSZ];
    long data[RUNTIME_TAGS];

    while (read(fd, &rec, sizeof(rec)) == (ssize_t)sizeof(rec))
    {
        if (rec.name_len >= sizeof(filename)
            || read(fd, filename, rec.name_len) != rec.name_len)
            return false;
        filename[rec.name_len] = '\0';
        for (int i = 0; i < RUNTIME_TAGS; i++)
            data[i] = rec.data[i];
        if (!import_one(masterfd, filename, data))
            return false;
        do_timed_yield();
    }
    return true;
}

/* Folds saved figures back in: from plays.dat after a rebuild, or from an
 * export the owner asked for. */
static bool import_runtime_data(bool from_export)
{
    struct master_header myhdr;
    struct libfile_header lh;
    int fd;
    long masterfd;
    char buf[2048];

    if (!tc_stat.ready)
        return false;

    while (read_lock)
        sleep(1);

    if (from_export)
        fd = open(LIB_EXPORT_FILE, O_RDONLY);
    else
        fd = libfile_open(LIB_PLAYS_FILE, LIB_PLAYS_MAGIC, LIB_PLAYS_VERSION,
                          1, &lh, NULL);
    if (fd < 0)
    {
        logf("no runtime data to import");
        return false;
    }

    if ( (masterfd = open_master_fd(&myhdr, true)) < 0)
    {
        close(fd);
        return false;
    }

    write_lock++;

    /* Trap: filenametag_fd belongs to a scan that may be running on the
     * tagcache thread. find_index() opens its own descriptor. */
    if (from_export)
        fast_readline(fd, buf, sizeof(buf), (void *)(intptr_t)masterfd,
                      parse_changelog_line);
    else
        import_runtime_records(fd, masterfd);

    close(fd);
    close(masterfd);

    write_lock--;

    update_master_header();

    return true;
}

bool tagcache_import_changelog(void)
{
    return import_runtime_data(true);
}

/* The runtime figures live only in the master index, which a rebuild deletes:
 * play count, rating, play time, last played, the commit an entry arrived in,
 * and its resume point. A rebuild saves them to plays.dat, which the import
 * queued after its commit reads back; Export Modifications writes the same
 * figures as Rockbox's text changelog instead.
 *
 * Read from the files directly rather than through a search, so it works on a
 * database that is not ready -- the state an interrupted merge leaves, and the
 * one the automatic rebuild starts from. Returns 1 when saved. A master that
 * cannot be read returns -1 and leaves an earlier file as it was. A file that
 * cannot be written returns 0, and a Rebuild stops there rather than import
 * the older copy over a newer library. */
static int save_runtime_data(bool as_export)
{
    struct master_header hdr;
    struct tagcache_header tch;
    struct index_entry idx;
    struct tagfile_entry tfe;
    struct libfile_writer w;
    char buf[TAGCACHE_BUFSZ];
    char num[16];
    int masterfd, fnfd, clfd = -1;
    bool ok = true;
    bool unreadable = false;
    long i;
    int t;

    /* Plays still queued are not in the master yet. */
    run_command_queue(true);

    masterfd = open_master_fd(&hdr, false);
    if (masterfd < 0)
        return -1;

    fnfd = open_tag_fd(&tch, tag_filename, false);
    if (fnfd < 0)
    {
        close(masterfd);
        return -1;
    }

    if (as_export)
    {
        clfd = open(LIB_EXPORT_FILE ".new", O_WRONLY | O_CREAT | O_TRUNC,
                    0666);
        ok = clfd >= 0 && write(clfd, "## Changelog version 1\n", 23) == 23;
    }
    else
        ok = libfile_begin(&w, LIB_PLAYS_FILE, LIB_PLAYS_MAGIC,
                           LIB_PLAYS_VERSION, 1, NULL);
    if (!ok)
    {
        if (clfd >= 0)
            close(clfd);
        close(fnfd);
        close(masterfd);
        return 0;
    }

    for (i = 0; ok && i < hdr.tch.entry_count; i++)
    {
        if (read_index_entries(masterfd, &idx, 1) != (ssize_t)sizeof(idx))
        {
            ok = false;
            unreadable = true;
            break;
        }

        if (!(idx.flag & FLAG_DIRTYNUM) || (idx.flag & FLAG_DELETED))
            continue;

        if (lseek(fnfd, idx.tag_seek[tag_filename], SEEK_SET) < 0
            || read_tagfile_entry(fnfd, &tfe) != (ssize_t)sizeof(tfe)
            || tfe.tag_length <= 0 || tfe.tag_length > (int)sizeof(buf)
            || read(fnfd, buf, tfe.tag_length) != tfe.tag_length)
            continue;
        buf[tfe.tag_length - 1] = '\0';

        if (as_export)
        {
            ok = write_tag(clfd, "filename", buf);
            for (t = 0; ok && t < RUNTIME_TAGS; t++)
            {
                itoa_buf(num, sizeof num, (int)idx.tag_seek[runtime_tags[t]]);
                ok = write_tag(clfd, tagcache_tag_to_str(runtime_tags[t]),
                               num);
            }
            if (ok)
                ok = write(clfd, "\n", 1) == 1;
        }
        else
        {
            struct runtime_rec rec;

            for (t = 0; t < RUNTIME_TAGS; t++)
                rec.data[t] = idx.tag_seek[runtime_tags[t]];
            rec.name_len = strlen(buf);
            ok = libfile_write(&w, &rec, sizeof(rec), sizeof(rec))
                 && libfile_write(&w, buf, rec.name_len, rec.name_len);
        }

        do_timed_yield();
    }

    close(fnfd);
    close(masterfd);

    if (as_export)
    {
        close(clfd);
        if (ok)
            ok = rename(LIB_EXPORT_FILE ".new", LIB_EXPORT_FILE) == 0;
        if (!ok)
            remove(LIB_EXPORT_FILE ".new");
    }
    else
        ok = libfile_finish(&w, ok);
    debug_log(DEBUG_LOG_TAGCACHE, "runtime data: %s", ok ? "saved" : "failed");
    return ok ? 1 : (unreadable ? -1 : 0);
}

/* Export Modifications */
bool tagcache_create_changelog(struct tagcache_search *tcs)
{
    (void)tcs;
    return save_runtime_data(true) > 0;
}

static bool delete_entry(long idx_id)
{
    int fd = -1;
    int masterfd = -1;
    int tag, i;
    struct index_entry idx, myidx;
    struct master_header myhdr;
    int in_use[TAG_COUNT];

    logf("delete_entry(): %ld", idx_id);

    debug_log(DEBUG_LOG_TAGCACHE, "del %ld: enter", idx_id);

    /* At first mark the entry removed from ram cache.
     *
     * Note what this costs if anything below fails: RAM says deleted, the
     * master index on disk does not, and the two disagree until something
     * reloads the RAM copy -- at which point the entry is back. */
    if (tc_stat.ramcache)
        tcramcache.hdr->indices[idx_id].flag |= FLAG_DELETED;

    if ( (masterfd = open_master_fd(&myhdr, true) ) < 0)
    {
        debug_log(DEBUG_LOG_TAGCACHE,
                  "del %ld: master open failed, RAM only", idx_id);
        return false;
    }

    lseek(masterfd, idx_id * sizeof(struct index_entry), SEEK_CUR);
    if (read_index_entries(masterfd, &myidx, 1) != sizeof(struct index_entry))
    {
        logf("delete_entry(): read error");
        goto cleanup;
    }

    if (myidx.flag & FLAG_DELETED)
    {
        logf("delete_entry(): already deleted!");
        goto cleanup;
    }

    myidx.flag |= FLAG_DELETED;
    lseek(masterfd, -(off_t)sizeof(struct index_entry), SEEK_CUR);
    if (write_index_entries(masterfd, &myidx, 1) != sizeof(struct index_entry))
    {
        logf("delete_entry(): write_error #1");
        goto cleanup;
    }

    /* Now check which tags are no longer in use (if any) */
    for (tag = 0; tag < TAG_COUNT; tag++)
        in_use[tag] = 0;

    lseek(masterfd, sizeof(struct master_header), SEEK_SET);
    for (i = 0; i < myhdr.tch.entry_count; i++)
    {
        struct index_entry *idxp;

        /* Use RAM DB if available for greater speed */
        if (tc_stat.ramcache)
            idxp = &tcramcache.hdr->indices[i];
        else
        {
            if (read_index_entries(masterfd, &idx, 1) != sizeof(struct index_entry))
            {
                logf("delete_entry(): read error #2");
                goto cleanup;
            }
            idxp = &idx;
        }

        if (idxp->flag & FLAG_DELETED)
            continue;

        for (tag = 0; tag < TAG_COUNT; tag++)
        {
            if (TAGCACHE_IS_NUMERIC(tag))
                continue;

            if (idxp->tag_seek[tag] == myidx.tag_seek[tag])
                in_use[tag]++;
        }
    }

    /* Now delete all tags no longer in use. */
    for (tag = 0; tag < TAG_COUNT; tag++)
    {
        struct tagcache_header tch;
        int oldseek = myidx.tag_seek[tag];

        if (TAGCACHE_IS_NUMERIC(tag))
            continue;

        /**
         * Replace tag seek with a hash value of the field string data.
         * That way runtime statistics of moved or altered files can be
         * resurrected.
         */
        if (tc_stat.ramcache && tag != tag_filename)
        {
            struct tagfile_entry *tfe;
            int32_t *seek = &tcramcache.hdr->indices[idx_id].tag_seek[tag];

            /* crc_32 is assumed not to yield (why would it...?) */
            tfe = (struct tagfile_entry *)&tcramcache.hdr->tags[tag][*seek];
            *seek = crc_32(tfe->tag_data, strlen(tfe->tag_data), 0xffffffff);
            myidx.tag_seek[tag] = *seek;
        }
        else
        {
            struct tagfile_entry tfe;

            /* Open the index file, which contains the tag names. */
            if ((fd = open_tag_fd(&tch, tag, true)) < 0)
                goto cleanup;

            /* Skip the header block */
            lseek(fd, myidx.tag_seek[tag], SEEK_SET);

            switch (read_tagfile_entry_and_tag(fd, &tfe,
                                               build_idx_buf, build_idx_bufsz))
            {
                case e_SUCCESS_LEN_ZERO:
                    logf("deleted_entry(): SUCCESS");
                    /* FALL THROUGH */
                case e_SUCCESS:
                     break;
                case e_ENTRY_SIZEMISMATCH:
                    logf("delete_entry(): read error #3");
                    goto cleanup;
                case e_TAG_TOOLONG:
                    logf("too long tag #4");
                    goto cleanup;
                case e_TAG_SIZEMISMATCH:
                    logf("delete_entry(): read error #3");
                    goto cleanup;
            }

            myidx.tag_seek[tag] = crc_32(build_idx_buf,
                                         strlen(build_idx_buf), 0xffffffff);
        }

        if (in_use[tag])
        {
            logf("in use: %d/%d", tag, in_use[tag]);
            if (fd >= 0)
            {
                close(fd);
                fd = -1;
            }
            continue;
        }

        /* Delete from ram. */
        if (tc_stat.ramcache && tag != tag_filename)
        {
            struct tagfile_entry *tagentry =
                    (struct tagfile_entry *)&tcramcache.hdr->tags[tag][oldseek];
            str_setlen(tagentry->tag_data, 0);
        }

        /* Open the index file, which contains the tag names. */
        if (fd < 0)
        {
            if ((fd = open_tag_fd(&tch, tag, true)) < 0)
                goto cleanup;
        }

        /* Skip the header block */
        lseek(fd, oldseek + sizeof(struct tagfile_entry), SEEK_SET);

        /* Debug, print 10 first characters of the tag
        read(fd, buf, 10);
        buf[10]='\0';
        logf("TAG:%s", buf);
        lseek(fd, -10, SEEK_CUR);
        */

        /* Write first data byte in tag as \0 */
        write(fd, "", 1);

        /* Now tag data has been removed */
        close(fd);
        fd = -1;
    }

    /* Write index entry back into master index. */
    lseek(masterfd, sizeof(struct master_header) +
          (idx_id * sizeof(struct index_entry)), SEEK_SET);
    if (write_index_entries(masterfd, &myidx, 1) != sizeof(struct index_entry))
    {
        logf("delete_entry(): write_error #2");
        goto cleanup;
    }

    close(masterfd);

    debug_log(DEBUG_LOG_TAGCACHE, "del %ld: written to disk", idx_id);
    return true;

    cleanup:
    if (fd >= 0)
        close(fd);
    if (masterfd >= 0)
        close(masterfd);

    /* After the closes, not before: debug_log() opens the log to append and
     * gives up silently if it cannot, so a line written while this function
     * still holds handles is a line that may never appear. */
    debug_log(DEBUG_LOG_TAGCACHE, "del %ld: bailed, RAM only", idx_id);
    return false;
}

/**
 * Returns true if there is an event waiting in the queue
 * that requires the current operation to be aborted.
 */
static bool check_event_queue(void)
{
    struct queue_event ev;

    /* An enumerating host counts. Trap: the queue alone is too late for one --
     * nothing arrives on it until SET_CONFIGURATION, by which point a scan
     * holding the CPU has already cost the host SET_ADDRESS. */
    if (usb_host_is_present())
        return true;

    if(!queue_peek(&tagcache_queue, &ev))
        return false;

    switch (ev.id)
    {
        case Q_STOP_SCAN:
        case SYS_POWEROFF:
        case SYS_REBOOT:
        case SYS_USB_CONNECTED:
            return true;
    }

    return false;
}


/* The queue alone, for load_tagcache(). It runs at startup, where a player
 * booted on a charger is plugged in throughout; standing down would leave the
 * database on the disk for the whole session. */
static bool check_event_queue_no_usb(void)
{
    struct queue_event ev;

    if(!queue_peek(&tagcache_queue, &ev))
        return false;

    switch (ev.id)
    {
        case Q_STOP_SCAN:
        case SYS_POWEROFF:
        case SYS_REBOOT:
        case SYS_USB_CONNECTED:
            return true;
    }

    return false;
}

/* USR_CANCEL. The host test is a flag read and runs every time round; the
 * queue peek takes a corelock and these loops run once per database entry, so
 * it rides do_timed_yield()'s HZ/25 limit. */
static bool usr_cancel(void)
{
    if (commit_cancelled)
        return true;

    if (usb_host_is_present() || (do_timed_yield() && check_event_queue()))
        commit_cancelled = true;

    return commit_cancelled;
}

static void fix_ramcache(void* old_addr, void* new_addr)
{
    ptrdiff_t offpos = new_addr - old_addr;
    for (int i = 0; i < TAG_COUNT; i++)
        tcramcache.hdr->tags[i] += offpos;
}

static int move_cb(int handle, void* current, void* new)
{
    (void)handle;
    fix_ramcache(current, new);
    tcramcache.hdr = new;
    return BUFLIB_CB_OK;
}

static struct buflib_callbacks ops = {
    .move_callback = move_cb,
    .shrink_callback = NULL,
};

/* Bytes the RAM copy of the database on disk needs, alignment slack
 * included, or 0 if there is no database to read. */
static size_t ramcache_size(struct master_header *tcmh)
{
    int fd = open_master_fd(tcmh, false);
    if (fd < 0)
        return 0;

    close(fd);

    size_t size = tcmh->tch.datasize + 256 + TAGCACHE_RESERVE +
        sizeof(struct ramcache_header) + TAG_COUNT*sizeof(void *);
    size += tcmh->tch.entry_count*sizeof(struct path_slot);
    size += album_tables_size(tcmh->tch.entry_count);
    return size;
}

/* Set by the last allocate_tagcache() that found no room for the RAM copy;
 * cleared by one that did. */
static bool ram_refused;

bool tagcache_ram_refused(void)
{
    return ram_refused;
}

static bool allocate_tagcache(void)
{
    tc_stat.ramcache_allocated = 0;
    tcramcache.handle = 0;
    tcramcache.hdr = NULL;

    struct master_header tcmh;
    size_t alloc_size = ramcache_size(&tcmh);
    if (alloc_size == 0)
        return false;

    /* Ensure enough memory remains for the audio buffer after allocation.
     * Without this check, a large database can consume so much RAM that
     * audio_reset_buffer() panics on OOM when playback starts.
     *
     * Only when the allocation would come out of free space, though. Once the
     * audio buffer holds the pool -- which it does the moment playback has run
     * -- core_available() is near zero however much headroom there really is,
     * because the memory this needs comes from shrinking that buffer. Applying
     * the test then refused the RAM copy to every path that runs with the UI
     * up, so a rebuild or update from the menu left the database on the disk
     * for the rest of the session: slow browsing, and the deleted-file check
     * giving up on its first line for want of a buffer.
     *
     * Nothing is lost by standing aside: shrinking is negotiated, and
     * playback's own shrink_callback() refuses below AUDIO_BUFFER_RESERVE.
     *
     * The reserve is for an audio buffer still to be laid out. Once one is,
     * free space is the slack it left, and taking from it costs playback
     * nothing: holding the reserve back would refuse even a small RAM copy
     * after every update made while music plays. */
    size_t available = core_available();
    if (audio_buffer_size() == 0
        && alloc_size <= available
        && alloc_size + TAGCACHE_MIN_AUDIO_RESERVE > available)
    {
        logf("tagcache: ramcache %luKB exceeds budget (%luKB avail)",
             (unsigned long)(alloc_size / 1024),
             (unsigned long)(available / 1024));
        debug_log(DEBUG_LOG_TAGCACHE,
                  "ramcache: %luKB wanted, %luKB free, reserving %luKB",
                  (unsigned long)(alloc_size / 1024),
                  (unsigned long)(available / 1024),
                  (unsigned long)(TAGCACHE_MIN_AUDIO_RESERVE / 1024));
        ram_refused = true;
        return false;
    }

    int handle = core_alloc_ex(alloc_size, &ops);
    if (handle <= 0)
    {
        debug_log(DEBUG_LOG_TAGCACHE, "ramcache: alloc of %luKB refused",
                  (unsigned long)(alloc_size / 1024));
        ram_refused = true;
        return false;
    }
    ram_refused = false;

    tcramcache.handle = handle;
    tcramcache.hdr = core_get_data(handle);
    tc_stat.ramcache_allocated = alloc_size;

    memset(tcramcache.hdr, 0, sizeof(struct ramcache_header));
    memcpy(&current_tcmh, &tcmh, sizeof current_tcmh);
    logf("tagcache: %d bytes allocated.", tc_stat.ramcache_allocated);

    return true;
}


static bool load_tagcache(void)
{
    /* DEBUG: After tagcache commit and dircache rebuild, hdr-sturcture
     * may become corrupt. */

    bool ok = false;
    ssize_t bytesleft = tc_stat.ramcache_allocated - sizeof(struct ramcache_header);
    int fd;
    long load_start = current_tick;
    /* Which tag this got to before giving up, for the log at `failure:`. Every
     * exit below reports the same way through logf(), which is compiled out on
     * a release build -- so a load that fails on a device says nothing at all,
     * and the database is silently left on the disk for the session. */
    int failtag = -1;

    /* Wait for any in-progress dircache build to complete */
    dircache_wait();

    logf("loading tagcache to ram...");

    tcrc_buffer_lock(); /* lock for the rest of the scan, simpler to handle */

    fd = open_db_fd(TAGCACHE_FILE_MASTER, O_RDONLY);
    if (fd < 0)
    {
        logf("tagcache open failed");
        goto failure;
    }

    struct master_header tcmh;
    if (read_master_header(fd, &tcmh) != sizeof(struct master_header) ||
        tcmh.tch.magic != TAGCACHE_MAGIC)
    {
        logf("incorrect header");
        goto failure;
    }

    /* Master header copy should already match, this can be redundant to do. */
    current_tcmh = tcmh;

    /* Load the master index table. */
    for (int i = 0; i < tcmh.tch.entry_count; i++)
    {
        bytesleft -= sizeof(struct index_entry);
        if (bytesleft < 0)
        {
            logf("too big tagcache.");
            goto failure;
        }

        int rc = read_index_entries(fd, &tcramcache.hdr->indices[i], 1);
        if (rc != sizeof (struct index_entry))
        {
            logf("read error #10");
            goto failure;
        }
    }

    close(fd);
    fd = -1;

    /* The path index next, ahead of the tag blocks: the filename pass that
     * fills it runs in the middle of the tag loop, before the blocks after it
     * have a position. */
    char *p = (char *)&tcramcache.hdr->indices[tcmh.tch.entry_count];
    ssize_t gap;
    struct path_slot *slots;
    int path_n = 0;

    p = TC_ALIGN_PTR(p, struct path_slot, &gap);
    bytesleft -= gap + tcmh.tch.entry_count * (ssize_t)sizeof(struct path_slot);
    if (bytesleft < 0)
    {
        logf("too big tagcache (path index)");
        goto failure;
    }
    slots = (struct path_slot *)p;
    tcramcache.hdr->path_first = p - (char *)tcramcache.hdr;
    tcramcache.hdr->path_count = 0;
    p += tcmh.tch.entry_count * sizeof(struct path_slot);

    /* The album tables next, grouped while the path index is still free to
     * sort in. Without room for them the RAM copy loads all the same. */
    if (!albums_group((int32_t *)slots, tcmh.tch.entry_count, &p, &bytesleft))
        debug_log(DEBUG_LOG_TAGCACHE, "load: no room for the album tables");

    /* Then the tags */
    for (int tag = 0; tag < TAG_COUNT; tag++)
    {
        ssize_t rc;

        if (TAGCACHE_IS_NUMERIC(tag))
            continue;

        failtag = tag;

        p = TC_ALIGN_PTR(p, struct tagcache_header, &rc);
        bytesleft -= rc;
        if (bytesleft < (ssize_t)sizeof(struct tagcache_header))
        {
            logf("Too big tagcache #10.5");
            goto failure;
        }

        tcramcache.hdr->tags[tag] = p;

        /* Load the header */
        struct tagcache_header *tch = (struct tagcache_header *)p;
        p += sizeof(struct tagcache_header);
        bytesleft -= sizeof (struct tagcache_header);

        fd = open_tag_fd(tch, tag, false);
        /* fd, not rc: rc last held an alignment gap and is never
         * negative, so a tag file that would not open was not noticed
         * here. *tch is then whatever the buffer held, and an
         * entry_count of zero or less walks past the loop below
         * without a read ever failing -- leaving the load reporting
         * success with one tag block never filled in. */
        if (fd < 0)
            goto failure;

        /* Load the entries for this tag */
        for (tcramcache.hdr->entry_count[tag] = 0;
             tcramcache.hdr->entry_count[tag] < tch->entry_count;
             tcramcache.hdr->entry_count[tag]++)
        {
            /* Abort if we got a critical event in queue */
            if (do_timed_yield() && check_event_queue_no_usb())
                goto failure;

            p = TC_ALIGN_PTR(p, struct tagfile_entry, &rc);
            bytesleft -= rc;
            if (bytesleft < (ssize_t)sizeof(struct tagfile_entry))
            {
                logf("Too big tagcache #10.75");
                goto failure;
            }

            struct tagfile_entry *fe = (struct tagfile_entry *)p;
            off_t pos = lseek(fd, 0, SEEK_CUR);

            /* Load the header for the tag itself */
            if (read_tagfile_entry(fd, fe) != sizeof(struct tagfile_entry))
            {
                /* End of lookup table. */
                logf("read error #11");
                goto failure;
            }

            int idx_id = fe->idx_id; /* fe is scratch for tag_filename */
            struct index_entry *idx = &tcramcache.hdr->indices[idx_id];

            if (idx_id != -1 || tag == tag_filename) /* filename NOT optional */
            {
                if (idx_id < 0 || idx_id >= tcmh.tch.entry_count)
                {
                    logf("corrupt tagfile entry:tag=%d:idxid=%d", tag, idx_id);
                    goto failure;
                }

                /* Not for a deleted entry: delete_entry() deliberately
                 * replaces tag_seek with a CRC of the tag text, so that
                 * runtime statistics survive a file coming back. Comparing
                 * that against a file position is meaningless, and treating
                 * the mismatch as corruption abandoned the whole load -- so
                 * one deleted track left the database unable to enter RAM on
                 * every boot thereafter, until something rebuilt it. Searches
                 * skip deleted entries anyway, so the hash is never read as a
                 * seek by anything downstream. */
                if (!(idx->flag & FLAG_DELETED) && idx->tag_seek[tag] != pos)
                {
                    logf("corrupt data structures!:");
                    logf("  tag_seek[%d]=%" PRId32 ":pos=%ld", tag,
                         idx->tag_seek[tag], (long) pos);
                    goto failure;
                }
            }

            /* Filenames are not kept: only the tag file's header, and each
               path's key in the path index. fe is scratch, reused for every
               entry. */
            if (tag == tag_filename)
            {
                char filename[TAGCACHE_BUFSZ];
                if (fe->tag_length >= (long)sizeof(filename)-1)
                {
                    read(fd, filename, 10);
                    str_setlen(filename, 10);
                    logf("TAG:%s", filename);
                    logf("too long filename");
                    goto failure;
                }

                if (idx->flag & FLAG_DELETED)
                {
                    /* seek over tag data instead of reading */
                    if (lseek(fd, fe->tag_length, SEEK_CUR) < 0)
                    {
                        logf("read error #11.5");
                        goto failure;
                    }

                    continue;
                }

                if (read(fd, filename, fe->tag_length) != fe->tag_length)
                {
                    logf("read error #12");
                    goto failure;
                }
                filename[fe->tag_length] = '\0';

                if (path_n < tcmh.tch.entry_count)
                {
                    uint64_t key = path_key(filename);

                    slots[path_n].key_lo = (uint32_t)key;
                    slots[path_n].key_hi = (uint32_t)(key >> 32);
                    slots[path_n].idx_id = idx_id;
                    path_n++;
                }
                albums_note_path(idx_id, filename);
                continue;
            }

            bytesleft -= sizeof(struct tagfile_entry) + fe->tag_length;
            if (bytesleft < 0)
            {
                logf("too big tagcache #2");
                logf("tl: %" PRId32, fe->tag_length);
                logf("bl: %ld", (long) bytesleft);
                goto failure;
            }

            p = fe->tag_data;
            rc = read(fd, p, fe->tag_length);
            p += rc;

            if (rc != fe->tag_length)
            {
                logf("read error #13");
                logf("rc=0x%04x", (unsigned int)rc); /* 0x431 */
                logf("len=0x%04" PRIx32, fe->tag_length); /* 0x4000 */
                logf("pos=0x%04lx", (unsigned long) lseek(fd, 0, SEEK_CUR)); /* 0x433 */
                logf("tag=0x%02x", tag); /* 0x00 */
                goto failure;
            }
        }

        tcramcache.hdr->tag_size[tag] = p - tcramcache.hdr->tags[tag];

        close(fd);
    }
    fd = -1;

    qsort(slots, path_n, sizeof(struct path_slot), path_slot_cmp);
    tcramcache.hdr->path_count = path_n;
    albums_finish(tcmh.tch.entry_count);

    tc_stat.ramcache_used = tc_stat.ramcache_allocated - bytesleft;
    debug_log(DEBUG_LOG_TAGCACHE,
              "load: %d entries, %d paths, %d albums, %d artists, %ld ms",
              (int)tcmh.tch.entry_count, path_n,
              tcramcache.hdr->album_count, tcramcache.hdr->artist_count,
              (current_tick - load_start) * 1000 / HZ);
    logf("tagcache loaded into ram!");
    logf("utilization: %d%%", 100*tc_stat.ramcache_used / tc_stat.ramcache_allocated);

    ok = true;

failure:
    if (!ok)
    {
        /* bytesleft says which kind of failure this was without needing a
         * message per exit: near zero means the buffer ran out, healthy means
         * the file did not read back as expected. failtag says where. */
        debug_log(DEBUG_LOG_TAGCACHE,
                  "load: failed on tag %d (%s), %ld bytes left of %d",
                  failtag,
                  failtag >= 0 && failtag < (int)ARRAYLEN(tags_str)
                      ? tags_str[failtag] : "master index",
                  (long)bytesleft, tc_stat.ramcache_allocated);
    }

    if (fd >= 0)
        close(fd);

    tcrc_buffer_unlock();
    return ok;
}

/* Deletes the entries whose files have gone, asking the directory cache and
 * then the disk for each path. */
static bool check_deleted_files(void)
{
    int fd;
    bool ret = true;
    char buf[TAGCACHE_BUFSZ];
    const int bufsz = sizeof(buf);
    struct tagfile_entry tfe;
    struct tagcache_header hdr;

    int deleted_ct = 0;

    logf("reverse scan...");

    if (tcramcache.handle > 0)
        tcrc_buffer_lock();
    else
    {
        debug_log(DEBUG_LOG_TAGCACHE, "refs: no ramcache buffer, giving up");
        return false;
    }
    /* Wait for any in-progress dircache build to complete */
    dircache_wait();

    fd = open_tag_fd(&hdr, tag_filename, false); /* open read only*/

    if (fd < 0)
    {
        logf(TAGCACHE_FILE_INDEX " open fail", tag_filename);
        debug_log(DEBUG_LOG_TAGCACHE, "refs: filename tagfile open failed");
        /* The pin above is held from here to wend_finished, and this is
         * the one exit between the two. Left taken it is never dropped:
         * the RAM database is the largest thing in the pool and buflib
         * cannot move a pinned block, so from here on nothing that needs
         * to compact can have it -- the audio buffer included. */
        tcrc_buffer_unlock();
        return false;
    }

    debug_log(DEBUG_LOG_TAGCACHE, "refs: start, entries=%d",
              (int)hdr.entry_count);

    processed_dir_count = 0;

    while (!check_event_queue())
    {
        int res = read_tagfile_entry_and_tag(fd, &tfe, buf, bufsz);
        processed_dir_count++;

        switch (res)
        {
            case e_ENTRY_SIZEMISMATCH:
                logf("size mismatch entry EOF?"); /* likely EOF */
                ret = false;
                goto wend_finished;
            case e_TAG_TOOLONG:
                logf("too long tag");
                ret = false;
                goto wend_finished;
            case e_TAG_SIZEMISMATCH:
                logf("size mismatch tag - read error #14");
                ret = false;
                goto wend_finished;
            case e_SUCCESS:
                break;
            case e_SUCCESS_LEN_ZERO:
                continue;
        }

        int idx_id = tfe.idx_id;

        /* The loader checks this too, but only for the entries the
         * tag file's own header admits to; this walk runs to the end
         * of the file. delete_entry() writes at idx_id, so an entry past
         * that count writes outside the RAM database. */
        if (idx_id < 0 || idx_id >= current_tcmh.tch.entry_count)
        {
            logf("corrupt filename tagfile entry: idxid=%d", idx_id);
            debug_log(DEBUG_LOG_TAGCACHE, "refs: bad idx_id %d", idx_id);
            ret = false;
            goto wend_finished;
        }

        int rc_cache = dircache_search(DCS_STORAGE_PATH, NULL, buf);

        if (rc_cache >= 0)          /* there */
        {;}
        /* A negative return means the lookup did not succeed; it does not say
         * why in any form worth trusting. Upstream read absence out of the
         * return value (`rc_cache == ENOENT`), which compares it against 2 --
         * a *success* code the branch above has already taken -- so nothing
         * deleted from the player was ever dropped from the database.
         *
         * errno is no better. dircache_search()'s "absent for sure" branch
         * reports ENOENT only when its inner call returns exactly 0; a missing
         * file here comes back rc=-15 with errno untouched at 0, so an errno
         * test misses it just as completely.
         *
         * So ask the filesystem instead of decoding the failure. It costs a
         * stat, but only for entries that already failed to resolve -- 43 on
         * the library this was found on, against 3502 scanned -- and deleting
         * an entry is destructive enough to be worth confirming directly. */
        else if (!file_exists(buf))
        {
            logf("Entry no longer valid.");
            logf("-> %s / %" PRId32, buf, tfe.tag_length);
            debug_log(DEBUG_LOG_TAGCACHE, "refs: gone %s", buf);
            delete_entry(idx_id);
            deleted_ct++;
        }
        else if (rc_cache < 0)
        {
            /* Did not resolve, but the file is there. Left alone. */
            debug_log(DEBUG_LOG_TAGCACHE, "refs: rc=%d errno=%d %s",
                      rc_cache, errno, buf);
        }

        do_timed_yield();
    }

wend_finished:
    debug_log(DEBUG_LOG_TAGCACHE, "refs: done, scanned=%d deleted=%d",
              processed_dir_count, deleted_ct);


    if (tcramcache.handle > 0)
        tcrc_buffer_unlock();
    close(fd);
    logf("done");

    return ret;
}

/* Note that this function must not be inlined, otherwise the whole point
 * of having the code in a separate function is lost.
 */
static void NO_INLINE check_ignore(const char *dirname,
    int *ignore, int *unignore)
{
    char newpath[MAX_PATH];
    const int bufsz = sizeof(newpath);

    /* check for a database.ignore file */
    snprintf(newpath, bufsz, "%s/database.ignore", dirname);
    *ignore = file_exists(newpath);
    /* check for a database.unignore file */
    snprintf(newpath, bufsz, "%s/database.unignore", dirname);
    *unignore = file_exists(newpath);
}

/* max roots on native. on application more can be added via malloc() */
#define MAX_STATIC_ROOTS 12

static struct search_roots_ll {
    const char *path;
    struct search_roots_ll * next;
} roots_ll[MAX_STATIC_ROOTS];

/* check if the path is already included in the search roots, by the
 * means that the path itself or one of its parents folders is in the list */
static bool search_root_exists(const char *path)
{
    struct search_roots_ll *this;
    for(this = &roots_ll[0]; this; this = this->next)
    {
        size_t root_len = strlen(this->path);
        /* check if the link target is inside of an existing search root
         * don't add if target is inside, we'll scan it later */
        if (!strncmp(this->path, path, root_len))
            return true;
    }
    return false;
}

#define add_search_root(a) do {} while(0)
#define free_search_roots(a) do {} while(0)

/* Deepest directory nesting the scan will follow. A music library is four or
 * five levels; this is well clear of any real one.
 *
 * Trap: the recursion below has no other bound. Upstream guards recursive
 * SYMLINKS by turning them into search roots, but a FAT directory whose
 * cluster chain has been corrupted into pointing at an ancestor is not a
 * symlink and loops forever -- walking entries without limit, and holding one
 * more DIR handle at every level until none are left. Once they are gone
 * every open() in the firmware fails, including the one debug_log() makes,
 * so the player stops with nothing written anywhere that says why. */
#define TAGCACHE_MAX_DEPTH 16

static bool check_dir(const char *dirname, int add_files, int depth)
{
    int success = false;

    if (depth > TAGCACHE_MAX_DEPTH)
    {
        debug_log(DEBUG_LOG_TAGCACHE, "depth limit at %s", dirname);
        logf("tagcache: depth limit at %s", dirname);
        return false;
    }

    DIR *dir = opendir(dirname);
    if (!dir)
    {
        logf("tagcache: opendir(%s) failed", dirname);
        return false;
    }

    /* check for a database.ignore and database.unignore */
    int ignore, unignore;
    check_ignore(dirname, &ignore, &unignore);

    /* don't do anything if both ignore and unignore are there */
    if (ignore != unignore)
        add_files = unignore;

    /* Recursively scan the dir. */
    while (!check_event_queue())
    {
        struct dirent *entry = readdir(dir);
        if (entry == NULL)
        {
            success = true;
            break;
        }

        if (is_dotdir_name(entry->d_name))
            continue;

        struct dirinfo info = dir_get_info(dir, entry);
        size_t len = strlen(curpath);
        path_append(&curpath[len-1], PA_SEP_HARD, entry->d_name,
                    sizeof (curpath) - len);

        processed_dir_count++;
        if (info.attribute & ATTR_DIRECTORY)
        {
            /* don't follow symlinks to dirs, but try to add it as a search root
             * this makes able to avoid looping in recursive symlinks */
            if (info.attribute & ATTR_LINK)
                add_search_root(curpath);
            else if (!check_dir(curpath, add_files, depth + 1))
                walk_incomplete = true;
        }
        else if (add_files)
        {
            tc_stat.curentry = curpath;

            /* Add a new entry to the temporary db file. */
            add_tagcache(curpath, info.mtime);

            /* Wait until current path for debug screen is read and unset. */
            while (tc_stat.syncscreen && tc_stat.curentry != NULL
                   && !check_event_queue())
                yield();

            tc_stat.curentry = NULL;
        }

        str_setlen(curpath, len);
    }

    closedir(dir);

    return success;
}

void tagcache_screensync_event(void)
{
    tc_stat.curentry = NULL;
}

void tagcache_screensync_enable(bool state)
{
    tc_stat.syncscreen = state;
}

/* After a complete walk: delete every live entry it did not find. */
static void delete_unseen_entries(void)
{
    struct master_header hdr;
    struct index_entry idxbuf[IDX_BUF_DEPTH];
    long i, n, k, count;
    int deleted = 0;
    int fd = open_master_fd(&hdr, false);

    if (fd < 0)
        return;

    count = MIN((long)hdr.tch.entry_count, walk_seen_count);
    for (i = 0; i < count; i += n)
    {
        n = MIN(count - i, IDX_BUF_DEPTH);
        if (read_index_entries(fd, idxbuf, n)
            != (ssize_t)sizeof(struct index_entry) * n)
            break;

        for (k = 0; k < n; k++)
        {
            long id = i + k;
            if ((idxbuf[k].flag & FLAG_DELETED)
                || (walk_seen[id / 32] & (1u << (id % 32))))
                continue;
            if (delete_entry(id))
                deleted++;
        }
        do_timed_yield();
    }

    close(fd);
    debug_log(DEBUG_LOG_TAGCACHE, "walk: %d deleted", deleted);
}

/* this is called by the database tool to not pull in global_settings */
static
void do_tagcache_build(const char *path[])
{
    struct tagcache_header header;
    bool ret;

    str_setlen(curpath, 0);
    data_size = 0;
    total_entry_count = 0;
    processed_dir_count = 0;

    dircache_wait();

    logf("updating tagcache");

    /* A temp file left by a cancelled commit is committed first. Left
     * waiting, it would stop every scan until the next boot. */
    if (db_file_exists(TAGCACHE_FILE_TEMP))
    {
        commit();
        if (db_file_exists(TAGCACHE_FILE_TEMP))
        {
            logf("skipping, cache already waiting for commit");
            return ;
        }
    }

    cachefd = open_db_fd(TAGCACHE_FILE_TEMP, O_RDWR | O_CREAT | O_TRUNC);
    if (cachefd < 0)
    {
        logf("master file open failed: %s", TAGCACHE_FILE_TEMP);
        return ;
    }

    filenametag_fd = open_tag_fd(&header, tag_filename, false);

    cpu_boost(true);

    logf("Scanning files...");
    /* Scan for new files. */
    memset(&header, 0, sizeof(struct tagcache_header));
    write(cachefd, &header, sizeof(struct tagcache_header));

    ret = true;

    roots_ll[0].path = path[0];
    roots_ll[0].next = NULL;

    extern bool ns_volume_is_visible(int volume); /*rb_namespace.c*/
    /* i is for the path vector, j for the roots_ll array */
    int i = 1, j = 1;
    bool added = false;
    char volnamebuf[NUM_VOLUMES][VOL_MAX_LEN + 1];
    /* we can just parse the root directory ('/') and get to any mounted
    * volume but we can also enumerate a volume in the root directory
    * when this occurs it leads to multiple entries since the files can
    * be reached through multiple paths ex, /Foo could also be /SD1/Foo
    * we used to hide the volume that was mapped but then when you switch
    * from the sd to the internal the paths don't map to the right volume
    * instead we will attempt to rewrite the root with any non-hidden volumes
    * failing that just leave the paths alone */
    if (!strcmp(PATH_ROOTSTR, path[0]))
    {
        i = 0;
        j = 0;
    }
    /* path can be skipped , but root_ll entries can't */
    for(; path[i] && j < MAX_STATIC_ROOTS; i++)
    {
        /* check if the link target is inside of an existing search root
         * don't add if target is inside, we'll scan it later */
        if (!added && !strcmp(PATH_ROOTSTR, path[i]))
        {
            for (int v = 0; v < NUM_VOLUMES; v++)
            {
                if (ns_volume_is_visible(v))
                {
                    make_volume_root(v, volnamebuf[v]);
                    roots_ll[j].path = volnamebuf[v];
                    if (j > 0)
                        roots_ll[j-1].next = &roots_ll[j];
                    j++;
                    added = true;
                }
            }
            if(!added)
                j = 1;
            added = true;
            continue;
        }
        if (search_root_exists(path[i])) /* skip this path */
            continue;

        roots_ll[j].path = path[i];
        roots_ll[j-1].next = &roots_ll[j];
        j++;
    }

    walk_seen_count = walk_checks_deletions() ? current_tcmh.tch.entry_count
                                              : 0;
    memset(walk_seen, 0, (walk_seen_count + 31) / 32 * sizeof(walk_seen[0]));
    walk_incomplete = false;

    struct search_roots_ll * this;
    /* check_dir might add new roots */
    for(this = &roots_ll[0]; this; this = this->next)
    {
        logf("Search root %s", this->path);
        strmemccpy(curpath, this->path, sizeof(curpath));

        if (ret)
        {
            if (dir_exists(this->path))
                ret = check_dir(this->path, true, 0);
            else
                logf("Dir not found %s", this->path);
        }
    }
    free_search_roots(&roots_ll[0]);

    /* Write the header. */
    header.magic = TAGCACHE_MAGIC;
    header.datasize = data_size;
    header.entry_count = total_entry_count;
    lseek(cachefd, 0, SEEK_SET);
    write(cachefd, &header, sizeof(struct tagcache_header));
    close(cachefd);

    if (filenametag_fd >= 0)
    {
        close(filenametag_fd);
        filenametag_fd = -1;
    }

    if (!ret)
    {
        /* A partial temp file would stop every later scan this session and
         * be committed, incomplete, at the next boot. */
        logf("Aborted.");
        walk_seen_count = 0;
        remove_db_file(TAGCACHE_FILE_TEMP);
        cpu_boost(false);
        return ;
    }

    if (walk_seen_count && !walk_incomplete)
        delete_unseen_entries();
    else if (walk_seen_count)
        debug_log(DEBUG_LOG_TAGCACHE, "walk: incomplete, no deletions");
    walk_seen_count = 0;

    /* Commit changes to the database. */
    if (commit())
    {
        logf("tagcache built!");
    }

    /* Import runtime statistics if we just initialized the db. */
    if (current_tcmh.serial == 0)
        queue_post(&tagcache_queue, Q_IMPORT_CHANGELOG, 0);

    cpu_boost(false);
}

void tagcache_build(void)
{
    char *vect[MAX_STATIC_ROOTS + 1]; /* +1 to ensure NULL sentinel */
    char str[sizeof(global_settings.tagcache_scan_paths)];
    strmemccpy(str, global_settings.tagcache_scan_paths, sizeof(str));

    int res = split_string(str, ':', vect, MAX_STATIC_ROOTS);
    vect[res] = NULL;

    tc_stat.scanning = true;
    do_tagcache_build((const char**)vect);
    tc_stat.scanning = false;
}

static void free_ramcache(void)
{
    tc_stat.ramcache = false;
    tcramcache.current = false;
    tcramcache.hdr = NULL;
    int handle = tcramcache.handle;
    tcramcache.handle = 0;
    core_free(handle);
    /* No buffer means nothing allocated. Leaving the size standing let
     * commit() take the branch that carves its tempbuf out of the RAM
     * copy, which with a freed one writes through (hdr + 1). */
    tc_stat.ramcache_allocated = 0;
}

/* The size of each file the RAM copy was loaded from: the master first, then
 * the tag files. Only a commit rewrites them, and a commit reloads; so after a
 * USB session the same sizes and master header mean the host left the
 * database alone, and the copy in RAM is still correct. */
static off_t loaded_size[1 + TAG_COUNT];

static off_t db_file_size(int tag)
{
    char buf[MAX_PATH];
    off_t size = -1;
    int fd;

    if (tag < 0)
        snprintf(buf, sizeof(buf), "%s/" TAGCACHE_FILE_MASTER,
                 tc_stat.db_path);
    else
        snprintf(buf, sizeof(buf), "%s/" TAGCACHE_FILE_INDEX,
                 tc_stat.db_path, tag);

    fd = open(buf, O_RDONLY);
    if (fd >= 0)
    {
        size = ffilesize(fd);
        close(fd);
    }
    return size;
}

static void record_loaded_sizes(void)
{
    loaded_size[0] = db_file_size(-1);
    for (int tag = 0; tag < TAG_COUNT; tag++)
        loaded_size[1 + tag] = TAGCACHE_IS_NUMERIC(tag) ? 0
                                                        : db_file_size(tag);
}

/* After a USB session that wrote to the disk: switch the RAM copy back on if
 * the database files are the ones it was loaded from. A reload would take
 * seconds, and browsing during the scan that follows would be served from
 * disk meanwhile. */
bool tagcache_reinstate_ramcache(void)
{
    struct master_header hdr;
    int fd;

    if (tcramcache.hdr == NULL || tc_stat.ramcache_allocated <= 0
        || !tcramcache.current || !tc_stat.ready)
        return false;

    fd = open_master_fd(&hdr, false);
    if (fd < 0)
        return false;
    close(fd);

    if (hdr.tch.entry_count != current_tcmh.tch.entry_count
        || hdr.commitid != current_tcmh.commitid || hdr.dirty)
        return false;

    if (db_file_size(-1) != loaded_size[0])
        return false;
    for (int tag = 0; tag < TAG_COUNT; tag++)
        if (!TAGCACHE_IS_NUMERIC(tag)
            && db_file_size(tag) != loaded_size[1 + tag])
            return false;

    tc_stat.ramcache = true;
    return true;
}

static void load_ramcache(void)
{
    /* A buffer is sized for the database it was allocated for, with
     * TAGCACHE_RESERVE to spare. An update or rebuild that grows the database
     * past that cannot load into it, so swap it for one that fits. */
    if (tcramcache.hdr)
    {
        struct master_header tcmh;
        size_t need = ramcache_size(&tcmh);

        if (need > (size_t)tc_stat.ramcache_allocated)
        {
            debug_log(DEBUG_LOG_TAGCACHE, "ramcache: %dKB held, %luKB needed",
                      tc_stat.ramcache_allocated / 1024,
                      (unsigned long)(need / 1024));
            free_ramcache();
            if (!allocate_tagcache())
                return ;
        }
    }

    /* Ask for the buffer again if an earlier attempt gave it back. Loading is
     * all-or-nothing -- a load stopped part way is a failure like any other
     * below, because a half-populated buffer must never be left where
     * tagcache_reload_ramcache() would switch it back on -- and a USB connect
     * arriving mid-load stops one. Without this, that single interruption cost
     * the RAM copy for the rest of the session, whatever happened afterwards:
     * the free below is permanent and every later call returned here. */
    if (!tcramcache.hdr && !allocate_tagcache())
        return ;

    cpu_boost(true);

    /* Off while the buffer is rewritten, which can be in place with the RAM
     * copy in use: readers fall back to the disk instead of finding an empty
     * path index and taking every miss as final */
    tc_stat.ramcache = false;
    tc_stat.ramcache = load_tagcache();
    tcramcache.current = tc_stat.ramcache;
    if (tc_stat.ramcache)
        record_loaded_sizes();

    if (!tc_stat.ramcache)
        debug_log(DEBUG_LOG_TAGCACHE, "ramcache: buffer taken but load failed");

    if (!tc_stat.ramcache)
    {
        /* RAM loading failed. Free the allocation and fall back to
         * disk-based access. Do not set tc_stat.ready = false here;
         * the on-disk database may still be valid and usable. */
        free_ramcache();
    }

    cpu_boost(false);
}

void tagcache_unload_ramcache(void)
{
    tc_stat.ramcache = false;
}

/* Put the RAM copy back into use.
 *
 * The counterpart to tagcache_unload_ramcache(), which clears a flag and does
 * nothing else -- the buffer stays allocated and stays populated. A commit
 * since the load, which can also have used the buffer as scratch, is refused
 * here; the caller owns the rest: nothing else may have written to the
 * database files in between. Whoever knows
 * that (a USB session the host only read from, say) can hand the searches
 * back to RAM instead of leaving them on the disk for the rest of the run. */
void tagcache_reload_ramcache(void)
{
    if (tcramcache.hdr != NULL && tc_stat.ramcache_allocated > 0
        && tcramcache.current && tc_stat.ready)
    {
        tc_stat.ramcache = true;
    }
}


/*
 * db_file_exists is noinline to minimize stack usage
 */
static bool NO_INLINE db_file_exists(const char* filename)
{
    char buf[MAX_PATH];

    snprintf(buf, sizeof(buf), "%s/%s", tc_stat.db_path, filename);

    return file_exists(buf);
}

static void tagcache_thread(void)
{
    struct queue_event ev;
    bool check_done = false;
    cpu_boost(true);
    finish_interrupted_swap();

    /* If the previous cache build/update was interrupted, commit
     * the changes first in foreground. */
    if (db_file_exists(TAGCACHE_FILE_TEMP))
    {
        allocate_tempbuf();
        if (commit() && current_tcmh.serial == 0)
            queue_post(&tagcache_queue, Q_IMPORT_CHANGELOG, 0);
        free_tempbuf();
    }

    /* Allocate space for the tagcache if found on disk. */
    if (!tc_stat.ramcache)
        allocate_tagcache();

    cpu_boost(false);
    tc_stat.initialized = true;

    /* The header check, on this thread rather than holding up boot. */
    if (!tc_stat.ready)
        tagcache_commit_finalize();

    /* The first wait returns at once, so the boot scan and the RAM load start
     * now rather than a second later. */
    bool first_wait = true;

    while (1)
    {
        /* Q_START_SCAN falls through into the SYS_TIMEOUT case below, so
         * without this the two cannot be told apart once inside it. */
        bool asked_to_scan = false;

        run_command_queue(false);

        queue_wait_w_tmo(&tagcache_queue, &ev, first_wait ? 0 : HZ);
        first_wait = false;

        switch (ev.id)
        {
            case Q_RELOAD_RAMCACHE:
                /* Q_UPDATE and Q_REBUILD reload for themselves, and leave
                 * nothing to do here. */
                if (!tc_stat.ramcache)
                    load_ramcache();
                break;

            case Q_IMPORT_CHANGELOG:
                /* A lookup per line: the path index makes it a binary
                 * search, the disk a walk of the filename file. */
                if (!tc_stat.ramcache)
                    load_ramcache();
                import_runtime_data(false);
                break;

            case Q_REBUILD:
                /* The master is the only copy of the figures; a save that
                 * could not write keeps it. Splashed from this thread, as
                 * playlist.c reports its control-file errors. */
                if (save_runtime_data(false) == 0)
                {
                    splashf(HZ*2, "%s %s", str(LANG_TAGCACHE_FORCE_UPDATE),
                            str(LANG_FAILED));
                    break;
                }
                remove_files();
                /* What the save could not flush names entries that no
                 * longer exist. */
                mutex_lock(&command_queue_mutex);
                command_queue_ridx = command_queue_widx;
                tc_stat.queue_length = 0;
                mutex_unlock(&command_queue_mutex);
                remove_db_file(TAGCACHE_FILE_TEMP);
                tagcache_build();
                /* Load it back, as Q_UPDATE below does. commit() unloads the
                 * RAM copy before it starts, so without this a rebuild from
                 * the menu leaves the database on the disk for the rest of the
                 * session: every search a seek, no album tables, the browser
                 * slow, and get_progress() with no entry count to divide by. It returns only on the next USB session
                 * or reboot, which is what makes it look like a USB fault.
                 *
                 * No check_deleted_files() to go with it: a rebuild has just
                 * regenerated the whole database from what is on the disk, so
                 * there is nothing stale left for it to find. */
                load_ramcache();
                break;

            case Q_UPDATE:
                /* Deletions first: a moved file keeps its play counts only
                 * if its old entry is already deleted when the commit adds
                 * it again. The walk does that itself where it can. */
                if (!walk_checks_deletions())
                    check_deleted_files();
                tagcache_build();
                load_ramcache();
                break ;

            case Q_START_SCAN:
                check_done = false;
                asked_to_scan = true;
                /* fallthrough */
            case SYS_TIMEOUT:
                if (check_done)
                    break ;

                /* Deferred, not abandoned, while a host has hold of us. This
                 * arm reads the whole database into RAM and can rescan the
                 * disk, which on a player booted with the cable in lands
                 * squarely on top of enumeration. check_done stays clear, so
                 * the next tick a second later picks it up.
                 *
                 * Deferring rather than aborting matters: load_ramcache()
                 * treats a stopped load as failure and frees the buffer for
                 * the rest of the session, which is why load_tagcache() uses
                 * check_event_queue_no_usb() once it is under way. Not
                 * starting costs nothing; stopping costs the RAM copy. */
                if (usb_host_is_present())
                    break ;

                /* Two different questions, so two different settings.
                 * Something asked for a scan (a USB session wrote to us, or a
                 * commit added entries) -> Auto Update decides. Nobody asked
                 * and this is the once-per-boot check -> Scan on Startup
                 * decides. The library on this player only changes over USB,
                 * and that path has its own rescan, so the boot check is
                 * usually looking for changes that cannot have happened. */
                const bool do_update = asked_to_scan
                        ? global_settings.tagcache_scan_on_eject
                        : global_settings.tagcache_scan_on_startup;

                /* A fresh player has no database yet. Rather than make the
                 * user open the database browser and confirm a prompt to
                 * create one, build it here in the background automatically
                 * -- there's no situation where you wouldn't want one. */
                debug_log(DEBUG_LOG_TAGCACHE,
                          "scan: asked=%d update=%d ready=%d ram=%d",
                          asked_to_scan, do_update, tc_stat.ready,
                          tc_stat.ramcache);

                /* Not before boot has loaded the skins. A commit takes all
                 * but TAGCACHE_MIN_AUDIO_RESERVE as one locked block, and a
                 * small library reaches it within seconds -- the theme's
                 * fonts then fail to load on a fresh player's first boot.
                 * check_done stays clear, so the next tick tries again. */
                if ((!tc_stat.ready || do_update) && !boot_finished)
                    break ;

                if (!tc_stat.ready)
                {
                    tagcache_build();
                    load_ramcache();
                    check_deleted_files();
                    check_done = true;
                    break ;
                }

                if (!tc_stat.ramcache)
                    load_ramcache();

                if (do_update)
                {
                    /* Before the build, as in Q_UPDATE: the walk deletes what
                     * it does not find, where it can, and the storage check
                     * is only the fallback. */
                    if (!walk_checks_deletions())
                        check_deleted_files();
                    tagcache_build();
                }

                logf("tagcache check done");

                check_done = true;
                break ;

            case Q_STOP_SCAN:
                break ;

            case SYS_POWEROFF:
            case SYS_REBOOT:
                break ;

            case SYS_USB_CONNECTED:
                logf("USB: TagCache");
                usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
                usb_wait_for_disconnect(&tagcache_queue);
                break ;
        }
    }
}

bool tagcache_prepare_shutdown(void)
{
    if (tagcache_get_commit_step() > 0)
        return false;

    tagcache_stop_scan();
    while (read_lock || write_lock)
        sleep(1);

    return true;
}

void tagcache_shutdown(void)
{
    /* Flush the command queue. */
    run_command_queue(true);

}

static int get_progress(void)
{
    int total_count = -1;

    struct dircache_info dcinfo;
    dircache_get_info(&dcinfo);
    if (dcinfo.status != DIRCACHE_IDLE &&
        ((!tc_stat.ramcache) || current_tcmh.tch.entry_count == 0))
    {
        total_count = dcinfo.entry_count;
    }
    else
    {
        if (tcramcache.hdr && tc_stat.ramcache)
            total_count = current_tcmh.tch.entry_count;
    }

    if (total_count < 0)
        return -1;
    else if (total_count == 0)
        return 0;
    else if (processed_dir_count > total_count)
        return 100;
    else
        return processed_dir_count * 100 / total_count;
}

void tagcache_get_marks(struct tagcache_marks *m)
{
    int i, n = 0;

    m->commitid = current_tcmh.commitid;
    m->serial = current_tcmh.serial;
    m->generation = db_generation;
    m->deleted_ct = -1;

    /* Deletions are only ever marked in the index -- delete_entry() sets the
     * flag and leaves entry_count alone -- so counting the flags is the only
     * way to notice one at all. */
    /* The handle, not just the header: a failed load frees the buffer and
     * clears both, and pinning a handle of 0 below is not something to find
     * out about later. check_deleted_files() guards the same way. */
    if (!tc_stat.ready || !tc_stat.ramcache || tcramcache.handle <= 0)
        return;

    /* Pinned and run straight through, as build_lookup_list() does: the
     * indices are movable, so yielding mid-walk would invalidate the pointer.
     * One flag read per entry is cheap enough not to want the yield. A race
     * with delete_entry() only undercounts, and the caller looks again. */
    tcrc_buffer_lock();

    for (i = 0; i < current_tcmh.tch.entry_count; i++)
    {
        if (tcramcache.hdr->indices[i].flag & FLAG_DELETED)
            n++;
    }

    tcrc_buffer_unlock();
    m->deleted_ct = n;
}

struct tagcache_stat* tagcache_get_stat(void)
{
    tc_stat.total_entries = current_tcmh.tch.entry_count;
    tc_stat.progress = get_progress();
    tc_stat.processed_entries = processed_dir_count;

    return &tc_stat;
}

bool tagcache_is_busy(void)
{
    /* A disk scan (scanning) or a database commit (commit_step) is running.
     * Drives the status-bar %ld indicator so background work needs no splash. */
    return tc_stat.scanning || tc_stat.commit_step > 0;
}

bool tagcache_search_ready(void)
{
    /* read_lock, not tagcache_is_busy(): the two do not agree. A commit holds
     * read_lock from well before commit_step is set to well after it is
     * cleared, and that whole span is what tagcache_search() waits out. */
    return !read_lock;
}

void tagcache_start_scan(void)
{
    queue_post(&tagcache_queue, Q_START_SCAN, 0);
}

bool tagcache_update(void)
{
    queue_post(&tagcache_queue, Q_UPDATE, 0);
    return false;
}

bool tagcache_rebuild(void)
{
    queue_post(&tagcache_queue, Q_REBUILD, 0);
    return false;
}

void tagcache_stop_scan(void)
{
    queue_post(&tagcache_queue, Q_STOP_SCAN, 0);
}



void tagcache_init(void)
{
    /* Per boot, for the same reason art_cache_init() does it. */
    debug_log_restart(DEBUG_LOG_TAGCACHE);

    memset(&tc_stat, 0, sizeof(struct tagcache_stat));
    memset(&current_tcmh, 0, sizeof(struct master_header));
    filenametag_fd = -1;
    write_lock = read_lock = 0;

    strmemccpy(tc_stat.db_path, LIB_DB_DIR, sizeof(tc_stat.db_path));
    mutex_init(&command_queue_mutex);
    sound_index_genre_of = genre_by_key;
    queue_init(&tagcache_queue, true);
    create_thread(tagcache_thread, tagcache_stack,
                  sizeof(tagcache_stack), 0, tagcache_thread_name
                  IF_PRIO(, PRIORITY_BACKGROUND)
                  IF_COP(, CPU));
}


bool tagcache_is_initialized(void)
{
    return tc_stat.initialized;
}

void tagcache_boot_finished(void)
{
    boot_finished = true;
}
bool tagcache_is_fully_initialized(void)
{
    return tc_stat.readyvalid;
}
bool tagcache_is_usable(void)
{
    return tc_stat.initialized && tc_stat.ready;
}
bool tagcache_is_in_ram(void)
{
    return tc_stat.ramcache;
}
int tagcache_get_commit_step(void)
{
    return tc_stat.commit_step;
}
int tagcache_get_max_commit_step(void)
{
    return (int)(SORTED_TAGS_COUNT)+1;
}
