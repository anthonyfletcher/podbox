/***************************************************************************
 * Original code from RockBox
 * was: apps/tagcache.h
 * Copyright (C) 2005 by Miika Pekkarinen
 * GNU General Public License (version 2+)
 *
 * Interface to tagcache.c: the search context, the tag enum and the query
 * API.
 ****************************************************************************/
#ifndef _TAGCACHE_H
#define _TAGCACHE_H

#include "config.h"
#include "system.h"
#include "metadata.h"
#include "settings/settings.h"

/**
 Note: When adding new tags, make sure to update index_entry_ec and tags_str in 
 tagcache.c and bump up the header version too.
 */
enum tag_type { tag_artist = 0, tag_album, tag_genre, tag_title,
    tag_filename, tag_composer, tag_comment, tag_albumartist, tag_grouping, tag_year,
    tag_discnumber, tag_tracknumber, tag_virt_canonicalartist, tag_bitrate, tag_length,
    tag_playcount, tag_rating, tag_playtime, tag_lastplayed, tag_commitid, tag_mtime,
    tag_lastelapsed, tag_lastoffset,
    /* Real tags end here, count them. */
    TAG_COUNT,
    /* Virtual tags */
    tag_virt_basename=TAG_COUNT,
    tag_virt_length_min, tag_virt_length_sec,
    tag_virt_playtime_min, tag_virt_playtime_sec,
    tag_virt_entryage, tag_virt_autoscore,
    /* Whether the track's genre names spoken word (db_spoken.c). */
    tag_virt_spoken,
    TAG_COUNT_ALL};

/* How many entries to fetch to the seek table at once while searching. */
#define SEEK_LIST_SIZE 32

#define TAGCACHE_MAX_FILTERS 4
#define TAGCACHE_MAX_CLAUSES 32

/* Tag to be used on untagged files. */
#define UNTAGGED "<Untagged>"
/* Maximum length of a single tag. */
#define TAG_MAXLEN (MAX_PATH*2)
/* buffer size for all the (stack allocated & static) buffers handling tc data */
#define TAGCACHE_BUFSZ (TAG_MAXLEN+32)

/* Numeric tags (we can use these tags with conditional clauses).
 *
 * A 32-bit mask over tag indices, so it holds tags 0..31 and no more.
 * tag_virt_spoken is 30; one index is left before a new numeric tag needs
 * this widened and every BIT_N() against it checked. */
#define TAGCACHE_NUMERIC_TAGS ((1LU << tag_year) | (1LU << tag_discnumber) | \
    (1LU << tag_tracknumber) | (1LU << tag_length) | (1LU << tag_bitrate) | \
    (1LU << tag_playcount) | (1LU << tag_rating) | (1LU << tag_playtime) | \
    (1LU << tag_lastplayed) | (1LU << tag_commitid) | (1LU << tag_mtime) | \
    (1LU << tag_lastelapsed) | (1LU << tag_lastoffset) | \
    (1LU << tag_virt_length_min) | (1LU << tag_virt_length_sec) | \
    (1LU << tag_virt_playtime_min) | (1LU << tag_virt_playtime_sec) | \
    (1LU << tag_virt_entryage) | (1LU << tag_virt_autoscore) | \
    (1LU << tag_virt_spoken))

#define TAGCACHE_IS_NUMERIC(tag) (BIT_N(tag) & TAGCACHE_NUMERIC_TAGS)

enum clause { clause_none, clause_is, clause_is_not, clause_gt, clause_gteq,
    clause_lt, clause_lteq, clause_contains, clause_not_contains, 
    clause_begins_with, clause_not_begins_with, clause_ends_with, clause_not_ends_with, 
	clause_oneof, clause_begins_oneof, clause_ends_oneof, clause_contains_oneof, 
	clause_not_oneof, clause_not_begins_oneof, clause_not_ends_oneof, clause_not_contains_oneof,
    clause_logical_or };

struct tagcache_stat {
    char db_path[MAX_PATHNAME+1];  /* Path to DB root directory */

    bool initialized;        /* Is tagcache currently busy? */
    bool readyvalid;         /* Has tagcache ready status been ascertained */
    bool ready;              /* Is tagcache ready to be used? */
    bool ramcache;           /* Is tagcache loaded in ram? */
    bool commit_delayed;     /* Has commit been delayed until next reboot? */
    bool econ;               /* Is endianess correction enabled? */
    volatile bool syncscreen;/* Synchronous operation with debug screen? */
    volatile bool scanning;  /* Is a build/update scan currently running? */
    volatile const char 
        *curentry;           /* Path of the current entry being scanned. */

    int  commit_step;        /* Commit progress */
    int  ramcache_allocated; /* Has ram been allocated for ramcache? */
    int  ramcache_used;      /* How much ram has been really used */
    int  progress;           /* Current progress of disk scan */
    int  processed_entries;  /* Scanned disk entries so far */
    int  total_entries;      /* Total entries in tagcache */
    int  queue_length;       /* Command queue length */

    /* const char *uimessage;   Pending error message. Implement soon. */
};

/* The counters that say what has changed in the database since a caller last
 * looked. Read together because anything watching one wants the others, and
 * because the deleted count wants the ramcache pinned once.
 *
 * commitid and serial live in the master header and cost nothing to read.
 * deleted_ct is counted over the master index, so it needs the database in
 * RAM; off the ramcache it is -1 and a caller watching it simply never sees a
 * deletion. */
struct tagcache_marks {
    int32_t commitid;   /* commits so far; tag_commitid stamps each entry */
    int32_t serial;     /* one per logged play, and the lastplayed value */
    int32_t deleted_ct; /* entries flagged deleted, or -1 if not countable */
};

enum source_type {source_constant, 
                  source_runtime, 
                  source_current_path /* dont add items after this.
                                       it is used as an index 
                                       into id3_to_search_mapping */
                 };

struct tagcache_search_clause
{
    int tag;
    int type;
    bool numeric;
    int source;
    long numeric_data;
    char *str;
};

struct tagcache_seeklist_entry {
    int32_t seek;
    int32_t flag;
    int32_t idx_id;
};

struct tagcache_search {
    /* For internal use only. */
    int fd, masterfd;
    int idxfd[TAG_COUNT];
    struct tagcache_seeklist_entry seeklist[SEEK_LIST_SIZE];
    int seek_list_count;
    int32_t filter_tag[TAGCACHE_MAX_FILTERS];
    int32_t filter_seek[TAGCACHE_MAX_FILTERS];
    int filter_count;
    struct tagcache_search_clause *clause[TAGCACHE_MAX_CLAUSES];
    int clause_count;
    int list_position;
    int seek_pos;
    long position;
    int entry_count;
    int master_entry_count; /* Index entries the master header claims */
    bool valid;
    bool initialized;
    bool failed;         /* A read failed: results so far are incomplete */
    uint32_t *unique_list;
    int unique_list_capacity;
    int unique_list_count;

    /* Exported variables. */
    bool ramsearch;      /* Is ram copy of the tagcache being used. */
    bool ramresult;      /* False if result is not static, and must be copied.
                            Currently always false since ramresult buffer is
                            movable */
    int type;            /* The tag type to be searched. Only nonvirtual tags */
    char *result;        /* The result data for all tags. */
    int result_len;      /* Length of the result including \0 */
    int32_t result_seek; /* Current position in the tag data. */
    int32_t idx_id;      /* Entry number in the master index. */
};


const char* tagcache_tag_to_str(int tag);

bool tagcache_find_index(struct tagcache_search *tcs, const char *filename);
bool tagcache_check_clauses(struct tagcache_search *tcs,
                            struct tagcache_search_clause **clause, int count);
bool tagcache_search(struct tagcache_search *tcs, int tag);
void tagcache_search_set_uniqbuf(struct tagcache_search *tcs,
                                 void *buffer, long length);
bool tagcache_search_add_filter(struct tagcache_search *tcs,
                                int tag, int seek);
bool tagcache_search_add_clause(struct tagcache_search *tcs,
                                struct tagcache_search_clause *clause);
bool tagcache_get_next(struct tagcache_search *tcs, char *buf, long size);
bool tagcache_retrieve(struct tagcache_search *tcs, int idxid, 
                       int tag, char *buf, long size);
void tagcache_search_finish(struct tagcache_search *tcs);
long tagcache_get_numeric(const struct tagcache_search *tcs, int tag);
long tagcache_increase_serial(void);
bool tagcache_import_changelog(void);
bool tagcache_create_changelog(struct tagcache_search *tcs);
void tagcache_update_numeric(int idx_id, int tag, long data);

struct tagcache_stat* tagcache_get_stat(void);
/* Fill in the change counters above. Cheap enough to ask on a timer; the
 * deleted count is the only part that walks anything, and it walks RAM. */
void tagcache_get_marks(struct tagcache_marks *m);
/* True while a database build/update scan or commit is in progress. Used to
 * drive the status-bar activity indicator (%ld) instead of a modal splash. */
bool tagcache_is_busy(void);
/* Whether a search would start now or wait. tagcache_search() opens by polling
 * read_lock, which a commit or a rebuild holds for as long as it runs -- so on
 * a thread that must not stall, ask this first and do without the answer.
 *
 * Reliable rather than advisory, because threads here are cooperative: nothing
 * else runs between this returning true and the search that follows it, so the
 * lock cannot be taken in the gap. That holds only if the caller does not
 * yield in between. */
bool tagcache_search_ready(void);
int tagcache_get_commit_step(void);
bool tagcache_prepare_shutdown(void);
void tagcache_shutdown(void);
void tagcache_remove_statefile(void);

void tagcache_screensync_event(void);
void tagcache_screensync_enable(bool state);

bool tagcache_is_in_ram(void);
bool tagcache_fill_tags(struct mp3entry *id3, const char *filename);
void tagcache_unload_ramcache(void);
void tagcache_reload_ramcache(void);
void tagcache_commit_finalize(void);
void tagcache_init(void) INIT_ATTR;
bool tagcache_is_initialized(void);
bool tagcache_is_fully_initialized(void);
bool tagcache_is_usable(void);
void tagcache_start_scan(void);
void tagcache_stop_scan(void);
bool tagcache_update(void);
bool tagcache_rebuild(void);
int tagcache_get_max_commit_step(void);
#endif
