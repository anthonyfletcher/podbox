/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Interface to pv_log.c.
 *
 * One reader at a time: the stream position and its 16 KB buffer are file
 * statics, and a read started on another thread waits for the one running.
 * An entry callback must not call back in here -- pv_log_size() and
 * pv_log_peek() included.
 ****************************************************************************/
#ifndef _PV_LOG_H
#define _PV_LOG_H

#include <stdbool.h>

/* Timestamps below this came from a clock that was never set -- they land in
 * the year 2000. The entry still counts as a play; every date-derived figure
 * has to leave it out. 2005-01-01 00:00 UTC. */
#define PV_MIN_VALID_TS 1104537600UL

/* Heard for less than this is a browsing tap, not an opinion about the track.
 * It counts as a data line and as nothing else -- not a play, not a skip. */
#define PV_TAP_MS 5000

/* Whether there is a log to read. playback.log is the only one: both On and
 * On + Last.fm write it, every play. */
enum pv_source
{
    PV_SRC_NONE = 0,
    PV_SRC_PLAYBACK      /* ROCKBOX_DIR/playback.log and its rotations */
};

/* One data line, already decided.
 *
 * The reader owns 'path' and reuses its memory for the next line, so it must
 * be copied if it is needed afterwards. */
struct pv_entry
{
    unsigned long ts;
    unsigned long elapsed_ms;
    unsigned long length_ms;
    const char   *path;
    /* Where this line starts, counted through the family as one stream.
     * Rotation renames files but never changes those bytes, so an offset
     * stays meaningful across one -- and anything that recorded an offset
     * while reading can come back to it later without reading what came
     * before. */
    unsigned long offset;
    bool valid_ts;      /* ts is usable as a date */
    bool listened;      /* counts as a play */
    bool skipped;       /* not a play, and not merely a browsing tap */
};

typedef void (*pv_entry_fn)(const struct pv_entry *e, void *ctx);

/* PV_SRC_PLAYBACK if playback.log holds any entries, otherwise PV_SRC_NONE.
 *
 * Decide this once and pass it to everything below, so a later read cannot
 * disagree with an earlier one. */
enum pv_source pv_log_pick_source(void);

/* Stream every data line of 'src' through 'cb', oldest first. Returns how
 * many there were, or -1 if nothing would open. */
long pv_log_read(enum pv_source src, pv_entry_fn cb, void *ctx);

/* The same over a byte range of the family: start at 'from', stop once 'to'
 * is reached ('to' of 0 meaning read to the end).
 *
 * For anything wanting a slice of the history rather than all of it -- one
 * week's entries, say -- which would otherwise mean reading a megabyte to
 * find fifteen kilobytes of it.
 *
 * 'from' must be an offset a previous read reported for an entry, not an
 * arbitrary position: seeking into the middle of a line would parse its tail
 * as a line of its own. */
long pv_log_read_range(enum pv_source src, unsigned long from,
                       unsigned long to, pv_entry_fn cb, void *ctx);

/* The same, ending early once *stop turns true; stop may be NULL */
long pv_log_read_range_stop(enum pv_source src, unsigned long from,
                            unsigned long to, pv_entry_fn cb, void *ctx,
                            const volatile bool *stop);

/* An offset to read from with pv_log_read_range() that misses no entry dated
 * 'ts' or later: the start of the last file of the family whose first dated
 * entry is earlier. Costs an open and a 512-byte read per file. */
unsigned long pv_log_offset_since(enum pv_source src, unsigned long ts);

/* What 'src' holds, without reading any of it. Sizes only, so this is an open
 * and a seek per file. */
unsigned long pv_log_size(enum pv_source src);

/* "playback log" / "none", for saying which was used. */
const char *pv_log_source_name(enum pv_source src);

/* The 'len' bytes ending at 'offset'. Returns how many were read.
 *
 * For confirming that a remembered position still describes the same history:
 * a byte count alone cannot tell a log that grew from one that was replaced
 * by a different log of a similar size. */
int pv_log_peek(enum pv_source src, unsigned long offset, void *buf, int len);

#endif /* _PV_LOG_H */
