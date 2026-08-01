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
 *   - lookup: finding an entry by filename, in RAM or on disk
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
#include <ctype.h>
#include "config.h"
#include "ata_idle_notify.h"
#include "thread.h"
#include "kernel.h"
#include "system.h"
#include "logf.h"
#include "string-extra.h"
#include "usb.h"
#include "metadata.h"
#include "tagcache.h"
#include "widgets/yesno.h"
#include "core_alloc.h"
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
#include "eeprom_settings.h"
/* Whether the commit in progress should give up where it stands. commit() and
 * the two index builders it calls test it in seven loops and again before the
 * master header is written.
 *
 * Cancelling is safe: the loops fall out, the master header and
 * TAGCACHE_FILE_TEMP are left alone, commit_error releases the lock and the
 * borrowed buffers, and rc stays false. The commit has not happened, and the
 * next starts over from the temp file. */
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

/* Dump store/restore header version 'TCSxx'. */
#define TAGCACHE_STATEFILE_MAGIC 0x54435301

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

/* The main database master index and numeric data. */
#define TAGCACHE_FILE_MASTER     "database_idx.tcd"

/* The main database string data. */
#define TAGCACHE_FILE_INDEX      "database_%d.tcd"

/* ASCII dumpfile of the DB contents. */
#define TAGCACHE_FILE_CHANGELOG  "database_changelog.txt"

/* Serialized DB. */
#define TAGCACHE_STATEFILE       "database_state.tcd"

/* Flags */
#define FLAG_DELETED     0x0001  /* Entry has been removed from db */
#define FLAG_DIRCACHE    0x0002  /* Filename is a dircache pointer */
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
    "tag_virt_entryage", "tag_virt_autoscore"
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

#define IF_TCRCDC(...) IF_DIRCACHE(__VA_ARGS__)

#define tcrc_dcfrefs \
    ((struct dircache_fileref *)(tcramcache.hdr->tags[tag_filename] + \
                                  sizeof (struct tagcache_header)))

/* Header is created when loading database to ram. */
struct ramcache_header {
    char *tags[TAG_COUNT];       /* Tag file content (dcfrefs if tag_filename) */
    int entry_count[TAG_COUNT];  /* Number of entries in the indices. */
    struct index_entry indices[0]; /* Master index file content */
};


/* In-RAM ramcache structure (not persisted) */
static struct tcramcache
{
    struct ramcache_header *hdr;      /* allocated ramcache_header */
    int handle;                       /* buffer handle */
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
    if (tag_length >= bufsz)
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

static int open_master_fd(struct master_header *hdr, bool write)
{
    int fd;
    int rc;

    fd = open_db_fd(TAGCACHE_FILE_MASTER, write ? O_RDWR : O_RDONLY);
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

static void remove_files(void)
{
    int i;
    char buf[MAX_PATH];
    const int bufsz = sizeof(buf);
    logf("%s", __func__);

    tc_stat.ready = false;
    tc_stat.ramcache = false;
    tc_stat.econ = false;
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

/* find the ramcache entry corresponding to the file indicated by
 * filename and dc (it's corresponding dircache id). */
static long find_entry_ram(const char *filename)
{
    static long last_pos = 0;
    struct dircache_fileref dcfref;

    /* Check if tagcache is loaded into ram. */
    if (!tc_stat.ramcache
        || global_settings.tagcache_ram != TAGCACHE_RAM_ON)
        return -1;

    if (dircache_search(DCS_CACHED_PATH | DCS_UPDATE_FILEREF, &dcfref,
                        filename) <= 0)
    {
        logf("tagcache: file not found.");
        return -1;
    }

    /* Search references */
    int end_pos = current_tcmh.tch.entry_count;
    while (1)
    {
        for (int i = last_pos; i < end_pos; i++)
        {
            do_timed_yield();

            if (!(tcramcache.hdr->indices[i].flag & FLAG_DIRCACHE))
                continue;

            int cmp = dircache_fileref_cmp(&tcrc_dcfrefs[i], &dcfref);
            if (cmp < 3)
                continue;

            last_pos = MAX(0, i - 3);
            return i;
        }

        if (last_pos == 0)
        {
            last_pos = MAX(0, end_pos - 3);
            break;
        }

        end_pos = last_pos;
        last_pos = 0;
    }

    return -1;
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

    const char *filename = filename_raw;


    if (!tc_stat.ready)
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
                    if (!strncmp(filename, buf, tag_length))
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

static int find_index(const char *filename)
{
    long idx_id = -1;

    idx_id = find_entry_ram(filename);

    if (idx_id < 0)
        idx_id = find_entry_disk(filename, true);

    return idx_id;
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
    /* We need to exclude all memory only flags & tags when writing to disk. */
    if (idx->flag & FLAG_DIRCACHE)
    {
        logf("memory only flags!");
        return false;
    }

    /* Only update numeric data. Writing the whole index to RAM by memcpy
     * destroys dircache pointers!
     */
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

        /* Don't touch the dircache flag or attributes. */
        idx_ram->flag = (idx->flag & 0x0000ffff)
            | (idx_ram->flag & (0xffff0000 | FLAG_DIRCACHE));
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

static bool retrieve(struct tagcache_search *tcs, IF_DIRCACHE(int idx_id,)
                     struct index_entry *idx, int tag, char *buf, long bufsz)
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

    if (tcs->ramsearch)
    {
        if (tag == tag_filename && (idx->flag & FLAG_DIRCACHE))
        {
            if (dircache_get_fileref_path(&tcrc_dcfrefs[idx_id], buf, bufsz) >= 0)
                success = true;
        }
        else
        if (tag != tag_filename)
        {
            struct tagfile_entry *ep =
                (struct tagfile_entry *)&tcramcache.hdr->tags[tag][seek];
            strmemccpy(buf, ep->tag_data, bufsz);
            success = true;
        }
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
        switch (clause->type)
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
                                             clause->type == clause_begins_oneof);
            case clause_not_ends_oneof:
                /* Fall-Through */
            case clause_not_begins_oneof:
                return !str_begins_ends_oneof(str, clause->str,
                                            clause->type == clause_not_begins_oneof);
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
            "Checking",  i, tag_type_str[clause->type],
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
                    retrieve(tcs, IF_DIRCACHE(tcs->idx_id,) idx, clause->tag,
                             buf, bufsz);
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
                        return false;
                    case e_TAG_TOOLONG:
                        logf("too long tag #6");
                        return false;
                    case e_TAG_SIZEMISMATCH:
                        logf("read error #16");
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
            "Found",  i, tag_type_str[clause->type],
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

    for (i = 0; i < tcs->unique_list_count; i++)
    {
        /* Return false if entry is found. */
        if (tcs->unique_list[i] == id)
        {
            /* logf("%d Exists @ %d", id, i); */
            return false;
        }
    }

    if (tcs->unique_list_count < tcs->unique_list_capacity)
    {
        tcs->unique_list[i] = id;
        tcs->unique_list_count++;
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

        for (i = tcs->seek_pos; i < current_tcmh.tch.entry_count; i++)
        {
            struct tagcache_seeklist_entry *seeklist;
            /* idx points to movable data, don't yield or reload */
            struct index_entry *idx = &tcramcache.hdr->indices[i];
            if (tcs->seek_list_count == SEEK_LIST_SIZE)
                break ;

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
                continue;
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

        tcs->seek_pos = i;

        return tcs->seek_list_count > 0;
    }

    if (tcs->masterfd < 0)
    {
        struct master_header tcmh;
        tcs->masterfd = open_master_fd(&tcmh, false);
    }

    lseek(tcs->masterfd, tcs->seek_pos * sizeof(struct index_entry) +
            sizeof(struct master_header), SEEK_SET);

    while (read_index_entries(tcs->masterfd, &entry, 1) == sizeof(struct index_entry))
    {
        struct tagcache_seeklist_entry *seeklist;

        if (tcs->seek_list_count == SEEK_LIST_SIZE)
            break ;

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
            continue;

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


bool tagcache_search(struct tagcache_search *tcs, int tag)
{
    /* NOTE: call tagcache_search_finish(&tcs) when finished or BAD things may happen (TM) */
    struct tagcache_header tag_hdr;
    struct master_header   master_hdr;
    int i;

    while (read_lock)
        sleep(1);

    memset(tcs, 0, sizeof(struct tagcache_search));
    if (tc_stat.commit_step > 0 || !tc_stat.ready)
        return false;

    tcs->position = sizeof(struct tagcache_header);
    tcs->type = tag;
    tcs->seek_pos = 0;
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

        if (!TAGCACHE_IS_NUMERIC(tcs->type))
        {
            tcs->idxfd[tcs->type] = open_tag_fd(&tag_hdr, tcs->type, false);
            if (tcs->idxfd[tcs->type] < 0)
                return false;

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
    memset(tcs->unique_list, 0, tcs->unique_list_capacity);
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
    long flag = 0;

    if (tcs->idxfd[tcs->type] < 0 && !is_numeric
        && !tcs->ramsearch
        )
        return false;

    /* Relative fetch. */
    if (tcs->filter_count > 0 || tcs->clause_count > 0 || is_numeric
        /* We need to retrieve flag status for dircache. */
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
        flag = seeklist->flag;
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
        if (tcs->type == tag_filename && (flag & FLAG_DIRCACHE))
        {
            ssize_t len = dircache_get_fileref_path(&tcrc_dcfrefs[tcs->idx_id],
                                                    buf, bufsz);
            if (len >= 0)
            {
                tcs->result_len = len + 1;
                tcs->result     = buf;
                tcs->ramresult  = false;
                return true;
            }
            /* else do it the hard way */
        }

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
            return false;
        case e_TAG_TOOLONG:
            tcs->valid = false;
            logf("too long tag #2");
            logf("P:%lX/%" PRIX32, (unsigned long) tcs->position, entry.tag_length);
            return false;
        case e_TAG_SIZEMISMATCH:
            tcs->valid = false;
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

    return retrieve(tcs, IF_DIRCACHE(idxid,) &idx, tag, buf, size);
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
    bool has_grouping;

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

    /* Check if the file is already cached. */
    idx_id = find_entry_ram(path);

    /* Be sure the entry doesn't exist. */
    if (filenametag_fd >= 0 && idx_id < 0)
        idx_id = find_entry_disk(path, false);

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
    entry.tag_offset[tag_discnumber] = id3.discnum;
    entry.tag_offset[tag_tracknumber] = id3.tracknum;
    entry.tag_offset[tag_length] = id3.length;
    entry.tag_offset[tag_bitrate] = id3.bitrate;
    entry.tag_offset[tag_mtime] = mtime;

    /* String tags. */
    has_artist = id3.artist != NULL
        && strlen(id3.artist) > 0;
    has_grouping = id3.grouping != NULL
        && strlen(id3.grouping) > 0;

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
    if (has_grouping)
    {
        ADD_TAG(entry, tag_grouping, &id3.grouping);
    }
    else
    {
        ADD_TAG(entry, tag_grouping, &id3.title);
    }
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
    if (has_grouping)
    {
        write_item(id3.grouping);
    }
    else
    {
        write_item(id3.title);
    }

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

static int tempbuf_sort(int fd)
{
    struct tempbuf_searchidx *index = (struct tempbuf_searchidx *)tempbuf;
    struct tagfile_entry fe;
    int i;
    int length;

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

        index[i].seek = lseek(fd, 0, SEEK_CUR);
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

        if (write_tagfile_entry(fd, &fe) != sizeof(struct tagfile_entry))
        {
            logf("tempbuf_sort: write error #1");
            return -1;
        }

        if (write(fd, index[i].str, length) != length)
        {
            logf("tempbuf_sort: write error #2");
            return -2;
        }

        /* Write some padding. */
        if (fe.tag_length - length > 0)
            write(fd, "XXXXXXXX", fe.tag_length - length);
    }

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
    struct index_entry idx;
    int masterfd;
    int masterfd_pos;
    struct temp_file_entry *entrybuf = (struct temp_file_entry *)tempbuf;
    int max_entries;
    int entries_processed = 0;
    int i, j;

    max_entries = tempbuf_size / sizeof(struct temp_file_entry) - 1;

    logf("Building numeric indices...");
    lseek(tmpfd, sizeof(struct tagcache_header), SEEK_SET);

    if ( (masterfd = open_master_fd(&tcmh, true)) < 0)
        return false;

    masterfd_pos = lseek(masterfd, tcmh.tch.entry_count * sizeof(struct index_entry),
                         SEEK_CUR);
    if (masterfd_pos < 0)
    {
        logf("we can't append!");
        close(masterfd);
        return false;
    }

    while (entries_processed < h->entry_count && !USR_CANCEL)
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
        for (i = 0; i < tcmh.tch.entry_count && !USR_CANCEL; i++)
        {
            /* Read the index entry. */
            if (read_index_entries(masterfd, &idx, 1) != sizeof(struct index_entry))
            {
                logf("read fail #3");
                close(masterfd);
                return false;
            }

            /**
             * Skip unless the entry is marked as being deleted
             * or the data has already been resurrected.
             */
            if (!(idx.flag & FLAG_DELETED) || (idx.flag & FLAG_RESURRECTED))
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
                if (tfe->tag_offset[tag_length] != idx.tag_seek[tag_length])
                    continue;

                /* Now it's time to do the hash matching. */
                if (tfe->tag_offset[tag_filename] != idx.tag_seek[tag_filename])
                {
                    int match_count = 0;

                    /* No filename match, check if we can match two other tags. */
#define tmpdb_match(tag) \
    if (tfe->tag_offset[tag] == idx.tag_seek[tag]) \
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

                /* A match found, now copy & resurrect the statistical data. */
#define tmpdb_copy_tag(tag) \
    tfe->tag_offset[tag] = idx.tag_seek[tag]

                tmpdb_copy_tag(tag_playcount);
                tmpdb_copy_tag(tag_rating);
                tmpdb_copy_tag(tag_playtime);
                tmpdb_copy_tag(tag_lastplayed);
                tmpdb_copy_tag(tag_commitid);
                tmpdb_copy_tag(tag_lastelapsed);
                tmpdb_copy_tag(tag_lastoffset);

                /* Avoid processing this entry again. */
                idx.flag |= FLAG_RESURRECTED;

                lseek(masterfd, -(off_t)sizeof(struct index_entry), SEEK_CUR);
                if (write_index_entries(masterfd, &idx, 1) != sizeof(struct index_entry))
                {
                    logf("masterfd writeback fail #1");
                    close(masterfd);
                    return false;
                }

                logf("Entry resurrected");
            }
        }


        /* Restore the master index position. */
        lseek(masterfd, masterfd_pos, SEEK_SET);

        /* Commit the data to the index. */
        for (i = 0; i < count && !USR_CANCEL; i++)
        {
            int loc = lseek(masterfd, 0, SEEK_CUR);

            if (read_index_entries(masterfd, &idx, 1) != sizeof(struct index_entry))
            {
                logf("read fail #3");
                close(masterfd);
                return false;
            }

            for (j = 0; j < TAG_COUNT; j++)
            {
                if (!TAGCACHE_IS_NUMERIC(j))
                    continue;

                idx.tag_seek[j] = entrybuf[i].tag_offset[j];
            }
            idx.flag = entrybuf[i].flag;

            if (idx.tag_seek[tag_commitid])
            {
                /* Data has been resurrected. */
                idx.flag |= FLAG_DIRTYNUM;
            }
            else if (tc_stat.ready && current_tcmh.commitid > 0)
            {
                idx.tag_seek[tag_commitid] = current_tcmh.commitid;
                idx.flag |= FLAG_DIRTYNUM;
            }

            /* Write back the updated index. */
            lseek(masterfd, loc, SEEK_SET);
            if (write_index_entries(masterfd, &idx, 1) != sizeof(struct index_entry))
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
static int build_index(int index_type, struct tagcache_header *h, int tmpfd)
{
    int i;
    struct tagcache_header tch;
    struct master_header   tcmh;
    struct index_entry idxbuf[IDX_BUF_DEPTH];
    int idxbuf_pos;
    int fd = -1, masterfd;
    bool error = false;
    int init;
    int masterfd_pos;

    logf("Building index: %d", index_type);

    /* Check the number of entries we need to allocate ram for. */
    commit_entry_count = h->entry_count + 1;

    masterfd = open_master_fd(&tcmh, false);
    if (masterfd >= 0)
    {
        commit_entry_count += tcmh.tch.entry_count;
        close(masterfd);
    }
    else
        remove_files(); /* Just to be sure we are clean. */

    /* Open the index file, which contains the tag names. */
    fd = open_tag_fd(&tch, index_type, true);
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
     * The buffer is sorted and written into tag file:
     *     tempbuf_sort(...);
     * leaving master index locations messed up.
     *
     * That is fixed using the lookup buffer for old tags:
     *     new_seek = tempbuf_find_location(old_seek, ...);
     * and for new tags:
     *     new_seek = tempbuf_find_location(idx);
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
        close(fd);
        return 0;
    }

    if (fd >= 0)
    {
        /**
         * If tag file contains unique tags (sorted index), we will load
         * it entirely into memory so we can resort it later for use with
         * chunked browsing.
         */
        if (TAGCACHE_IS_SORTED(index_type))
        {
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
        }
        else
            tempbufidx = tch.entry_count;
    }
    else
    {
        logf("Create New Index: %d", index_type);
        /**
         * Creating new index file to store the tags. No need to preload
         * anything whether the index type is sorted or not.
         *
         * Note: although we are creating a file under the db path, it must
         * already exist by this point so no mkdir is required.
         */
        fd = open_pathfmt(build_idx_buf, build_idx_bufsz,
                          O_WRONLY | O_CREAT | O_TRUNC,
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

    /* Loading the tag lookup file as "master file". */
    logf("Loading index file");
    masterfd = open_db_fd(TAGCACHE_FILE_MASTER, O_RDWR);

    if (masterfd < 0)
    {
        logf("Creating new DB");
        masterfd = open_db_fd(TAGCACHE_FILE_MASTER, O_WRONLY | O_CREAT | O_TRUNC);

        if (masterfd < 0)
        {
            logf("Failure to create index file (%s)", TAGCACHE_FILE_MASTER);
            close(fd);
            return -2;
        }

        /* Write the header (write real values later). */
        memset(&tcmh, 0, sizeof(struct master_header));
        tcmh.tch = *h;
        tcmh.tch.entry_count = 0;
        tcmh.tch.datasize = 0;
        tcmh.dirty = true;
        write_master_header(masterfd, &tcmh);
        init = true;
        masterfd_pos = lseek(masterfd, 0, SEEK_CUR);
    }
    else
    {
        /**
         * Master file already exists so we need to process the current
         * file first.
         */
        init = false;

        if (read_master_header(masterfd, &tcmh) != sizeof(struct master_header) ||
            tcmh.tch.magic != TAGCACHE_MAGIC)
        {
            logf("header error");
            close(fd);
            close(masterfd);
            return -2;
        }

        /**
         * If we reach end of the master file, we need to expand it to
         * hold new tags. If the current index is not sorted, we can
         * simply append new data to end of the file.
         * However, if the index is sorted, we need to update all tag
         * pointers in the master file for the current index.
         */
        masterfd_pos = lseek(masterfd, tcmh.tch.entry_count * sizeof(struct index_entry),
            SEEK_CUR);
        if (masterfd_pos == filesize(masterfd))
        {
            logf("appending...");
            init = true;
        }
    }

    /**
     * Load new unique tags in memory to be sorted later and added
     * to the master lookup file.
     */
    if (TAGCACHE_IS_SORTED(index_type))
    {
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
                error = true;
                goto error_exit;
            }

            /* Read data. */
            if (entry.tag_length[index_type] >= build_idx_bufsz)
            {
                logf("too long entry!");
                error = true;
                goto error_exit;
            }

            lseek(tmpfd, entry.tag_offset[index_type], SEEK_CUR);
            if (read(tmpfd, build_idx_buf, entry.tag_length[index_type]) !=
                entry.tag_length[index_type])
            {
                logf("read fail #4");
                error = true;
                goto error_exit;
            }
            str_setlen(build_idx_buf, entry.tag_length[index_type]);

            {
                if (TAGCACHE_IS_UNIQUE(index_type))
                    error = !tempbuf_insert(build_idx_buf, i, -1, true);
                else
                    error = !tempbuf_insert(build_idx_buf, i,
                                            tcmh.tch.entry_count + i, false);

                if (error)
                {
                    logf("insert error");
                    goto error_exit;
                }
            }
            /* Skip to next. */
            lseek(tmpfd, entry.data_length - entry.tag_offset[index_type] -
                    entry.tag_length[index_type], SEEK_CUR);
            do_timed_yield();
        }
        logf("done");

        /* Sort the buffer data and write it to the index file. */
        lseek(fd, sizeof(struct tagcache_header), SEEK_SET);
        /**
         * We need to truncate the index file now. There can be junk left
         * at the end of file (however, we _should_ always follow the
         * entry_count and don't crash with that).
         */
        ftruncate(fd, lseek(fd, 0, SEEK_CUR));

        i = tempbuf_sort(fd);
        if (i < 0)
            goto error_exit;
        logf("sorted %d tags", i);

        /**
         * Now update all indexes in the master lookup file.
         */
        logf("updating indices...");
        lseek(masterfd, sizeof(struct master_header), SEEK_SET);
        for (i = 0; i < tcmh.tch.entry_count && !USR_CANCEL; i += idxbuf_pos)
        {
            int j;
            int loc = lseek(masterfd, 0, SEEK_CUR);

            idxbuf_pos = MIN(tcmh.tch.entry_count - i, IDX_BUF_DEPTH);

            if (read_index_entries(masterfd, idxbuf, idxbuf_pos) !=
                (ssize_t)sizeof(struct index_entry) * idxbuf_pos)
            {
                logf("read fail #5");
                error = true;
                goto error_exit ;
            }
            lseek(masterfd, loc, SEEK_SET);

            for (j = 0; j < idxbuf_pos; j++)
            {
                if (idxbuf[j].flag & FLAG_DELETED)
                {
                    /* We can just ignore deleted entries. */
                    /* idxbuf[j].tag_seek[index_type] = 0; */
                    continue;
                }

                idxbuf[j].tag_seek[index_type] = tempbuf_find_location(
                    idxbuf[j].tag_seek[index_type]/TAGFILE_ENTRY_CHUNK_LENGTH
                    + commit_entry_count);

                if (idxbuf[j].tag_seek[index_type] < 0)
                {
                    logf("update error: %" PRId32 "/%d/%" PRId32,
                         idxbuf[j].flag, i+j, tcmh.tch.entry_count);
                    error = true;
                    goto error_exit;
                }

                do_timed_yield();
            }

            /* Write back the updated index. */
            if (write_index_entries(masterfd, idxbuf, idxbuf_pos) !=
                (ssize_t)sizeof(struct index_entry) * idxbuf_pos)
            {
                logf("write fail");
                error = true;
                goto error_exit;
            }
        }
        logf("done");
    }

    /**
     * Walk through the temporary file containing the new tags.
     */
    /* build_normal_index(h, tmpfd, masterfd, idx); */
    logf("updating new indices...");
    lseek(masterfd, masterfd_pos, SEEK_SET);
    lseek(tmpfd, sizeof(struct tagcache_header), SEEK_SET);
    lseek(fd, 0, SEEK_END);
    for (i = 0; i < h->entry_count && !USR_CANCEL; i += idxbuf_pos)
    {
        int j;

        idxbuf_pos = MIN(h->entry_count - i, IDX_BUF_DEPTH);
        if (init)
        {
            memset(idxbuf, 0, sizeof(struct index_entry)*IDX_BUF_DEPTH);
        }
        else
        {
            int loc = lseek(masterfd, 0, SEEK_CUR);

            if (read_index_entries(masterfd, idxbuf, idxbuf_pos) !=
                (ssize_t)sizeof(struct index_entry) * idxbuf_pos)
            {
                logf("read fail #6");
                error = true;
                break ;
            }
            lseek(masterfd, loc, SEEK_SET);
        }

        /* Read entry headers. */
        for (j = 0; j < idxbuf_pos; j++)
        {
            if (!TAGCACHE_IS_SORTED(index_type))
            {
                struct temp_file_entry entry;
                struct tagfile_entry fe;

                if (read(tmpfd, &entry, sizeof(struct temp_file_entry)) !=
                    sizeof(struct temp_file_entry))
                {
                    logf("read fail #7");
                    error = true;
                    break ;
                }

                /* Read data. */
                if (entry.tag_length[index_type] >= build_idx_bufsz)
                {
                    logf("too long entry!");
                    logf("length=%d", entry.tag_length[index_type]);
                    logf("pos=0x%02lx", (unsigned long) lseek(tmpfd, 0, SEEK_CUR));
                    error = true;
                    break ;
                }

                lseek(tmpfd, entry.tag_offset[index_type], SEEK_CUR);
                if (read(tmpfd, build_idx_buf, entry.tag_length[index_type]) !=
                    entry.tag_length[index_type])
                {
                    logf("read fail #8");
                    logf("offset=0x%02" PRIx32, entry.tag_offset[index_type]);
                    logf("length=0x%02x", entry.tag_length[index_type]);
                    error = true;
                    break ;
                }

                /* Write to index file. */
                idxbuf[j].tag_seek[index_type] = lseek(fd, 0, SEEK_CUR);
                fe.tag_length = entry.tag_length[index_type];
                fe.idx_id = tcmh.tch.entry_count + i + j;
                write_tagfile_entry(fd, &fe);
                write(fd, build_idx_buf, fe.tag_length);
                tempbufidx++;

                /* Skip to next. */
                lseek(tmpfd, entry.data_length - entry.tag_offset[index_type] -
                      entry.tag_length[index_type], SEEK_CUR);
            }
            else
            {
                /* Locate the correct entry from the sorted array. */
                idxbuf[j].tag_seek[index_type] = tempbuf_find_location(i + j);
                if (idxbuf[j].tag_seek[index_type] < 0)
                {
                    logf("entry not found (%d)", j);
                    error = true;
                    break ;
                }
            }
        }

        /* Write index. */
        if (write_index_entries(masterfd, idxbuf, idxbuf_pos) !=
            (ssize_t)sizeof(struct index_entry) * idxbuf_pos)
        {
            logf("tagcache: write fail #4");
            error = true;
            break ;
        }

        do_timed_yield();
    }
    logf("done");

    /* Finally write the header. */
    tch.magic = TAGCACHE_MAGIC;
    tch.entry_count = tempbufidx;
    tch.datasize = lseek(fd, 0, SEEK_END) - sizeof(struct tagcache_header);
    lseek(fd, 0, SEEK_SET);
    write_tagcache_header(fd, &tch);

    if (index_type != tag_filename)
        h->datasize += tch.datasize;
    logf("s:%d/%" PRId32 "/%" PRId32, index_type, tch.datasize, h->datasize);
    error_exit:

    close(fd);
    close(masterfd);

    if (error)
        return -2;

    return 1;
}

static bool commit(void)
{
    struct tagcache_header tch;
    struct master_header   tcmh;
    int i, len, rc;
    int tmpfd;
    int masterfd;
    bool dircache_buffer_stolen = false;
    bool ramcache_buffer_stolen = false;
    logf("committing tagcache");

    commit_cancelled = false;

    while (write_lock)
        sleep(1);

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

    /* Beyond here, jump to commit_error to undo locks and restore dircache */
    rc = false;
    read_lock++;

    /* Try to steal every buffer we can :) */
    if (tempbuf_size == 0)
    {
        /* Suspend dircache to free its allocation. */
        dircache_free_buffer();
        dircache_buffer_stolen = true;

        allocate_tempbuf();
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
        goto commit_error;
    }

    logf("commit %" PRId32 " entries...", tch.entry_count);

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
            goto commit_error;
        }
        do_timed_yield();
    }

    if (!build_numeric_indices(&tch, tmpfd))
    {
        logf("Failure to commit numeric indices");
        close(tmpfd);
        tc_stat.commit_step = 0;
        goto commit_error;
    }

    close(tmpfd);

    tc_stat.commit_step = 0;

    if (!USR_CANCEL)
    {
        /* Update the master index headers. */
        if ( (masterfd = open_master_fd(&tcmh, true)) < 0)
            goto commit_error;

        remove_db_file(TAGCACHE_FILE_TEMP);

        tcmh.tch.entry_count += tch.entry_count;
        tcmh.tch.datasize = sizeof(struct master_header)
            + sizeof(struct index_entry) * tcmh.tch.entry_count
            + tch.datasize;
        tcmh.dirty = false;
        tcmh.commitid++;

        lseek(masterfd, 0, SEEK_SET);
        write_master_header(masterfd, &tcmh);
        close(masterfd);

        logf("tagcache committed");
        tagcache_commit_finalize();

        if (ramcache_buffer_stolen)
        {
            tempbuf = NULL;
            tempbuf_size = 0;
            ramcache_buffer_stolen = false;
            tcrc_buffer_unlock();
        }

        /* Ask for the RAM copy back: this rewrote the index files under it,
         * and the scan is what reloads it -- see the tagcache thread's
         * SYS_TIMEOUT arm. A commit with nothing in it cannot reach here, it
         * returns above, which is what stops the cycle build -> commit(0) ->
         * start_scan -> build -> ... from repeating forever. */
        if (tc_stat.ramcache_allocated > 0)
            tagcache_start_scan();

        rc = true;
    } /*!USR_CANCEL*/

commit_error:
    if (ramcache_buffer_stolen)
    {
        tempbuf = NULL;
        tempbuf_size = 0;
        tcrc_buffer_unlock();
    }

    read_lock--;

    /* Resume the dircache, if we stole the buffer. */
    if (dircache_buffer_stolen)
    {
        free_tempbuf();
        dircache_resume();
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

    if ( (masterfd = open_master_fd(&myhdr, true)) < 0)
        return;

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
                    return;

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

        /* Queue is full, try again later... */
        mutex_unlock(&command_queue_mutex);
        sleep(1);
    }
}

long tagcache_increase_serial(void)
{
    long old;

    if (!tc_stat.ready)
        return -2;

    while (read_lock)
        sleep(1);

    old = current_tcmh.serial++;
    queue_command(CMD_UPDATE_MASTER_HEADER, 0, 0, 0);

    return old;
}

void tagcache_update_numeric(int idx_id, int tag, long data)
{
    queue_command(CMD_UPDATE_NUMERIC, idx_id, tag, data);
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
            continue;
        }

        buf[i] = *(datastr++);
    }

    str_setlen(buf, bufsz - 1);
    strmemccpy(&buf[i], "\" ", (bufsz - i - 1));

    write(fd, buf, i + 2);

    return true;
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

        /* Read the data. */
        for (pos = 0; pos < size; pos++)
        {
            if (*src == '\0')
                break;

            if (*src == '\\')
            {
                src++;
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

static int parse_changelog_line(int line_n, char *buf, void *parameters)
{
    struct index_entry idx;
    char tag_data[TAGCACHE_BUFSZ];
    int idx_id;
    long masterfd = (long)(intptr_t)parameters;
    const int import_tags[] = { tag_playcount, tag_rating, tag_playtime,
                                tag_lastplayed, tag_commitid, tag_lastelapsed,
                                tag_lastoffset };
    int i;
    (void)line_n;

    if (*buf == '#')
        return 0;

    /* logf("%d/%s", line_n, buf); */
    if (!read_tag(tag_data, sizeof tag_data, buf, "filename"))
    {
        logf("%d/filename missing", line_n);
        logf("-> %s", buf);
        return 0;
    }

    idx_id = find_index(tag_data);
    if (idx_id < 0)
    {
        logf("%d/entry not found", line_n);
        return 0;
    }

    if (!get_index(masterfd, idx_id, &idx, false))
    {
        logf("%d/failed to retrieve index entry", line_n);
        return 0;
    }

    /* Stop if tag has already been modified. */
    if (idx.flag & FLAG_DIRTYNUM)
        return 0;

    logf("%d/import: %s", line_n, tag_data);

    idx.flag |= FLAG_DIRTYNUM;
    for (i = 0; i < (long)(sizeof(import_tags)/sizeof(import_tags[0])); i++)
    {
        int data;

        if (!read_tag(tag_data, sizeof tag_data, buf,
                      tagcache_tag_to_str(import_tags[i])))
        {
            continue;
        }

        data = atoi(tag_data);
        if (data < 0)
            continue;

        idx.tag_seek[import_tags[i]] = data;

        if (import_tags[i] == tag_lastplayed && data >= current_tcmh.serial)
            current_tcmh.serial = data + 1;
        else if (import_tags[i] == tag_commitid && data >= current_tcmh.commitid)
            current_tcmh.commitid = data + 1;
    }

    return write_index(masterfd, idx_id, &idx) ? 0 : -5;
}

bool tagcache_import_changelog(void)
{
    struct master_header myhdr;
    struct tagcache_header tch;
    int clfd;
    long masterfd;
    char buf[2048];
    const int bufsz = sizeof(buf);

    if (!tc_stat.ready)
        return false;

    while (read_lock)
        sleep(1);

    clfd = open_db_fd(TAGCACHE_FILE_CHANGELOG, O_RDONLY);
    if (clfd < 0)
    {
        logf("failure to open changelog");
        return false;
    }

    if ( (masterfd = open_master_fd(&myhdr, true)) < 0)
    {
        close(clfd);
        return false;
    }

    write_lock++;

    filenametag_fd = open_tag_fd(&tch, tag_filename, false);

    fast_readline(clfd, buf, bufsz, (void *)(intptr_t)masterfd,
                  parse_changelog_line);

    close(clfd);
    close(masterfd);

    if (filenametag_fd >= 0)
    {
        close(filenametag_fd);
        filenametag_fd = -1;
    }

    write_lock--;

    update_master_header();

    return true;
}


bool tagcache_create_changelog(struct tagcache_search *tcs)
{
    struct master_header myhdr;
    struct index_entry idx;
    char buf[TAGCACHE_BUFSZ];
    const int bufsz = sizeof(buf);
    char temp[32];
    int clfd;
    int i, j;

    if (!tc_stat.ready)
        return false;

    if (!tagcache_search(tcs, tag_filename))
        return false;

    /* Initialize the changelog */
    clfd = open_db_fd(TAGCACHE_FILE_CHANGELOG, O_WRONLY | O_CREAT | O_TRUNC);
    if (clfd < 0)
    {
        logf("failure to open changelog");
        tagcache_search_finish(tcs);
        return false;
    }

    if (tcs->masterfd < 0)
    {
        if ( (tcs->masterfd = open_master_fd(&myhdr, false)) < 0)
        {
            close(clfd);
            tagcache_search_finish(tcs);
            return false;
        }
    }
    else
    {
        lseek(tcs->masterfd, 0, SEEK_SET);
        read_master_header(tcs->masterfd, &myhdr);
    }

    write(clfd, "## Changelog version 1\n", 23);

    for (i = 0; i < myhdr.tch.entry_count; i++)
    {
        if (read_index_entries(tcs->masterfd, &idx, 1) != sizeof(struct index_entry))
        {
            logf("read error #9");
            tagcache_search_finish(tcs);
            close(clfd);
            return false;
        }

        /* Skip until the entry found has been modified. */
        if (! (idx.flag & FLAG_DIRTYNUM) )
            continue;

        /* Skip deleted entries too. */
        if (idx.flag & FLAG_DELETED)
            continue;

        /* Now retrieve all tags. */
        for (j = 0; j < TAG_COUNT; j++)
        {
            if (TAGCACHE_IS_NUMERIC(j))
            {
                itoa_buf(temp, sizeof temp, (int)idx.tag_seek[j]);
                write_tag(clfd, tagcache_tag_to_str(j), temp);
                continue;
            }

            tcs->type = j;
            tagcache_retrieve(tcs, i, tcs->type, buf, bufsz);
            write_tag(clfd, tagcache_tag_to_str(j), buf);
        }

        write(clfd, "\n", 1);
        do_timed_yield();
    }

    close(clfd);

    tagcache_search_finish(tcs);

    return true;
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

    /* At first mark the entry removed from ram cache. */
    if (tc_stat.ramcache)
        tcramcache.hdr->indices[idx_id].flag |= FLAG_DELETED;

    if ( (masterfd = open_master_fd(&myhdr, true) ) < 0)
        return false;

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

    return true;

    cleanup:
    if (fd >= 0)
        close(fd);
    if (masterfd >= 0)
        close(masterfd);

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

static bool allocate_tagcache(void)
{
    tc_stat.ramcache_allocated = 0;
    tcramcache.handle = 0;
    tcramcache.hdr = NULL;

    /* Load the header. */
    struct master_header tcmh;
    int fd = open_master_fd(&tcmh, false);
    if (fd < 0)
        return false;

    close(fd);

    /**
     * Now calculate the required cache size plus
     * some extra space for alignment fixes.
     */
    size_t alloc_size = tcmh.tch.datasize + 256 + TAGCACHE_RESERVE +
        sizeof(struct ramcache_header) + TAG_COUNT*sizeof(void *);
    alloc_size += tcmh.tch.entry_count*sizeof(struct dircache_fileref);

    /* Ensure enough memory remains for the audio buffer after allocation.
     * Without this check, a large database can consume so much RAM that
     * audio_reset_buffer() panics on OOM when playback starts. */
    size_t available = core_available();
    if (alloc_size + TAGCACHE_MIN_AUDIO_RESERVE > available)
    {
        logf("tagcache: ramcache %luKB exceeds budget (%luKB avail)",
             (unsigned long)(alloc_size / 1024),
             (unsigned long)(available / 1024));
        return false;
    }

    int handle = core_alloc_ex(alloc_size, &ops);
    if (handle <= 0)
        return false;

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

    /* Load the tags right after the index entries */
    char *p = (char *)&tcramcache.hdr->indices[tcmh.tch.entry_count];

    for (int tag = 0; tag < TAG_COUNT; tag++)
    {
        ssize_t rc;

        if (TAGCACHE_IS_NUMERIC(tag))
            continue;

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
        if (rc < 0)
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

            int idx_id = fe->idx_id; /* dircache reference clobbers *fe */
            struct index_entry *idx = &tcramcache.hdr->indices[idx_id];

            if (idx_id != -1 || tag == tag_filename) /* filename NOT optional */
            {
                if (idx_id < 0 || idx_id >= tcmh.tch.entry_count)
                {
                    logf("corrupt tagfile entry:tag=%d:idxid=%d", tag, idx_id);
                    goto failure;
                }

                if (idx->tag_seek[tag] != pos)
                {
                    logf("corrupt data structures!:");
                    logf("  tag_seek[%d]=%" PRId32 ":pos=%ld", tag,
                         idx->tag_seek[tag], (long) pos);
                    goto failure;
                }
            }

            /* We have a special handling for the filename tags; neither the
               paths nor the entry headers are stored; only the tagcache header
               and dircache references are. */
            if (tag == tag_filename)
            {
                if (idx->flag & FLAG_DIRCACHE)
                {
                    /* This flag must not be used yet. */
                    logf("internal error!");
                    goto failure;
                }

                p += sizeof (struct dircache_fileref);
                bytesleft -= sizeof (struct dircache_fileref);

                char filename[TAGCACHE_BUFSZ];
                if (fe->tag_length >= (long)sizeof(filename)-1)
                {
                    read(fd, filename, 10);
                    str_setlen(filename, 10);
                    logf("TAG:%s", filename);
                    logf("too long filename");
                    goto failure;
                }

                if ((idx->flag & FLAG_DELETED)
                    IFN_DIRCACHE( || !global_settings.tagcache_scan_on_eject ))
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

        if (tag == tag_filename)
            p = (char *)&tcrc_dcfrefs[tcmh.tch.entry_count];

        close(fd);
    }

    tc_stat.ramcache_used = tc_stat.ramcache_allocated - bytesleft;
    logf("tagcache loaded into ram!");
    logf("utilization: %d%%", 100*tc_stat.ramcache_used / tc_stat.ramcache_allocated);

    ok = true;

failure:
    if (fd >= 0)
        close(fd);

    tcrc_buffer_unlock();
    return ok;
}

static bool check_file_refs(bool auto_update)
{
    int fd;
    bool ret = true;
    char buf[TAGCACHE_BUFSZ];
    const int bufsz = sizeof(buf);
    struct tagfile_entry tfe;
    struct tagcache_header hdr;

    logf("reverse scan...");

    if (tcramcache.handle > 0)
        tcrc_buffer_lock();
    else
        return false;
    /* Wait for any in-progress dircache build to complete */
    dircache_wait();

    fd = open_tag_fd(&hdr, tag_filename, false); /* open read only*/

    if (fd < 0)
    {
        logf(TAGCACHE_FILE_INDEX " open fail", tag_filename);
        return false;
    }

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

        int idx_id = tfe.idx_id; /* dircache reference clobbers *tfe */
        struct index_entry *idx = &tcramcache.hdr->indices[idx_id];
        unsigned int searchflag;
        if (!auto_update)
        {
            if(idx->flag & FLAG_DIRCACHE) /* already found */
            {
                continue;
            }
            searchflag = DCS_CACHED_PATH; /* attempt to load cache references */
        }
        else /* If auto updating, check storage too */
        {
            searchflag = DCS_STORAGE_PATH;
        }

        int rc_cache = dircache_search(searchflag | DCS_UPDATE_FILEREF,
                                   &tcrc_dcfrefs[idx_id], buf);

        if (rc_cache > 0)           /* in cache and we have fileref */
        {
            idx->flag |= FLAG_DIRCACHE;
        }
        else if (rc_cache == 0)     /* not in cache but okay */
        {;}
        else if (auto_update && rc_cache == ENOENT)
        {
            logf("Entry no longer valid.");
            logf("-> %s / %" PRId32, buf, tfe.tag_length);
            delete_entry(idx_id);
        }

        do_timed_yield();
    }

wend_finished:

    if (tcramcache.handle > 0)
        tcrc_buffer_unlock();
    close(fd);
    logf("done");

    return ret;
}

static bool check_deleted_files(void)
{
    return check_file_refs(true);
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

static bool check_dir(const char *dirname, int add_files)
{
    int success = false;

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
            else
                check_dir(curpath, add_files);
        }
        else if (add_files)
        {
            tc_stat.curentry = curpath;

            /* Add a new entry to the temporary db file. */
            add_tagcache(curpath, info.mtime);

            /* Wait until current path for debug screen is read and unset. */
            while (tc_stat.syncscreen && tc_stat.curentry != NULL)
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

    cachefd = open_db_fd(TAGCACHE_FILE_TEMP, O_RDONLY);
    if (cachefd >= 0)
    {
        logf("skipping, cache already waiting for commit");
        close(cachefd);
        return ;
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

    struct search_roots_ll * this;
    /* check_dir might add new roots */
    for(this = &roots_ll[0]; this; this = this->next)
    {
        logf("Search root %s", this->path);
        strmemccpy(curpath, this->path, sizeof(curpath));

        if (ret)
        {
            if (dir_exists(this->path))
                ret = check_dir(this->path, true);
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
        logf("Aborted.");
        cpu_boost(false);
        return ;
    }

    /* Commit changes to the database. */
    if (commit())
    {
        logf("tagcache built!");
    }

    if (tcramcache.hdr)
    {
        /* Import runtime statistics if we just initialized the db. */
        if (current_tcmh.serial == 0)
            queue_post(&tagcache_queue, Q_IMPORT_CHANGELOG, 0);
    }

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

static void load_ramcache(void)
{
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

    /* At first we should load the cache (if exists). */
    tc_stat.ramcache = load_tagcache();

    if (!tc_stat.ramcache)
    {
        /* RAM loading failed. Free the allocation and fall back to
         * disk-based access. Do not set tc_stat.ready = false here;
         * the on-disk database may still be valid and usable. */
        tcramcache.hdr = NULL;
        int handle = tcramcache.handle;
        tcramcache.handle = 0;
        core_free(handle);
        /* No buffer means nothing allocated. Leaving the size standing let
         * commit() take the branch that carves its tempbuf out of the RAM
         * copy, which with a freed one writes through (hdr + 1). */
        tc_stat.ramcache_allocated = 0;
    }

    cpu_boost(false);
}

void tagcache_unload_ramcache(void)
{
    tc_stat.ramcache = false;
    /* Just to make sure there is no statefile present. */
    /* remove_db_file(TAGCACHE_STATEFILE); */
}

/* Put the RAM copy back into use.
 *
 * The counterpart to tagcache_unload_ramcache(), which clears a flag and does
 * nothing else -- the buffer stays allocated and stays populated. So this is
 * only a flag again, and the caller owns the one condition that matters:
 * nothing may have written to the database files in between. Whoever knows
 * that (a USB session the host only read from, say) can hand the searches
 * back to RAM instead of leaving them on the disk for the rest of the run. */
void tagcache_reload_ramcache(void)
{
    if (tcramcache.hdr != NULL && tc_stat.ramcache_allocated > 0
        && tc_stat.ready)
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
    /* If the previous cache build/update was interrupted, commit
     * the changes first in foreground. */
    if (db_file_exists(TAGCACHE_FILE_TEMP))
    {
        static const char *lines[] = {ID2P(LANG_TAGCACHE_BUSY),
                                      ID2P(LANG_TAGCACHE_UPDATE)};
        static const struct text_message message = {lines, 2};
        bool go = global_settings.tagcache_autocommit;

        /* Asking runs on this thread, which owns no screen -- main.c's boot
         * screen does. They do not collide only because the boot screen draws
         * nothing until commit_step goes above zero, which cannot happen
         * before this is answered. Keep that true if either side moves. */
        if (!go)
            go = gui_syncyesno_run_w_tmo(HZ * 5, YESNO_YES, str(LANG_TAGCACHE),
                                         &message, NULL, NULL) == YESNO_YES;

        if (go)
        {
            allocate_tempbuf();
            commit();
            free_tempbuf();
        }
    }


    /* Allocate space for the tagcache if found on disk. */
    if (global_settings.tagcache_ram && !tc_stat.ramcache)
        allocate_tagcache();

    cpu_boost(false);
    tc_stat.initialized = true;

    /* Don't delay bootup with the header check but do it on background. */
    if (!tc_stat.ready)
    {
        sleep(HZ);
        tagcache_commit_finalize();
    }

    while (1)
    {
        /* Q_START_SCAN falls through into the SYS_TIMEOUT case below, so
         * without this the two cannot be told apart once inside it. */
        bool asked_to_scan = false;

        run_command_queue(false);

        queue_wait_w_tmo(&tagcache_queue, &ev, HZ);

        switch (ev.id)
        {
            case Q_IMPORT_CHANGELOG:
                tagcache_import_changelog();
                break;

            case Q_REBUILD:
                remove_files();
                remove_db_file(TAGCACHE_FILE_TEMP);
                tagcache_build();
                break;

            case Q_UPDATE:
                tagcache_build();
                load_ramcache();
                check_deleted_files();
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
                if (!tc_stat.ready)
                {
                    tagcache_build();
                    if (global_settings.tagcache_ram)
                        load_ramcache();
                    check_deleted_files();
                    check_done = true;
                    break ;
                }

                if (!tc_stat.ramcache && global_settings.tagcache_ram)
                {
                    load_ramcache();
                    if (global_settings.tagcache_ram == TAGCACHE_RAM_ON)
                        check_file_refs(do_update);
                    if (tc_stat.ramcache && do_update)
                        tagcache_build();
                }
                else
                if (do_update)
                {
                    tagcache_build();

                    /* This will be very slow unless dircache is enabled
                       or target is flash based, but do it anyway for
                       consistency. */
                    check_deleted_files();
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

void tagcache_remove_statefile(void)
{
    remove_db_file(TAGCACHE_STATEFILE);
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

void tagcache_start_scan(void)
{
    queue_post(&tagcache_queue, Q_START_SCAN, 0);
}

bool tagcache_update(void)
{
    if (!tc_stat.ready)
        return false;

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
    memset(&tc_stat, 0, sizeof(struct tagcache_stat));
    memset(&current_tcmh, 0, sizeof(struct master_header));
    filenametag_fd = -1;
    write_lock = read_lock = 0;

    strmemccpy(tc_stat.db_path, global_settings.tagcache_db_path,
               sizeof(tc_stat.db_path));
    mutex_init(&command_queue_mutex);
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
