/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Interface to pv_log.c.
 *
 * One reader at a time. The stream position and its 16 KB buffer are file
 * statics, so a second read started while one is running takes the first
 * one's place in the file. In particular an entry callback must not call back
 * in here -- pv_log_size() and pv_log_peek() included.
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

/* Which log the figures came from. The setting decides which one the core is
 * writing (settings > playback > logging), and the two are never written at
 * once, so whichever is chosen holds a whole history rather than half of one.
 *
 * They do not say quite the same thing, and the difference is not a bug:
 *
 *   PLAYBACK  records how long each track was actually heard, so "minutes"
 *             means minutes of audio and a few seconds' tap is visible as
 *             such.
 *   SCROBBLER records only the verdict -- played or skipped -- and the
 *             track's full length. So "minutes" means the total length of
 *             the tracks that counted as played, which runs a little high,
 *             and a browsing tap is indistinguishable from a real skip.
 *
 * In exchange, the scrobbler log carries proper tag names and needs no
 * database lookup at all -- but it carries no file paths, so nothing keyed
 * off a path (artwork, most obviously) can work from it. */
enum pv_source
{
    PV_SRC_NONE = 0,
    PV_SRC_PLAYBACK,     /* ROCKBOX_DIR/playback.log and its rotations */
    PV_SRC_SCROBBLER     /* /.scrobbler.log, Audioscrobbler format     */
};

/* One data line, already decided.
 *
 * The reader owns every string here and reuses the memory for the next line,
 * so anything needed afterwards must be copied.
 *
 * 'path' is empty and 'artist'/'album'/'title' are NULL when the source does
 * not carry them -- which is the whole difference between the two formats,
 * and why both are present. Exactly one of the two sets is populated. */
struct pv_entry
{
    unsigned long ts;
    unsigned long elapsed_ms;   /* for a scrobbler play, the track's length */
    unsigned long length_ms;
    const char   *path;
    const char   *artist;
    const char   *album;
    const char   *title;
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

/* Which log to read: the one the current setting writes if it holds any
 * entries, otherwise the other if it does, otherwise PV_SRC_NONE.
 *
 * Preferring the setting matters. A user who logged normally for a year and
 * then switched to last.fm still has the old playback.log sitting there, and
 * reading it would show a year of statistics frozen at the moment they
 * switched -- plausible, wrong, and impossible to spot from the screen.
 * Falling back only when the preferred log is empty keeps the other history
 * reachable without letting it masquerade.
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

/* What 'src' holds, without reading any of it. Sizes only, so this is an open
 * and a seek per file. */
unsigned long pv_log_size(enum pv_source src);

/* "playback log" / "scrobbler log" / "none", for saying which was used. */
const char *pv_log_source_name(enum pv_source src);

/* The 'len' bytes ending at 'offset'. Returns how many were read.
 *
 * For confirming that a remembered position still describes the same history:
 * a byte count alone cannot tell a log that grew from one that was replaced
 * by a different log of a similar size. */
int pv_log_peek(enum pv_source src, unsigned long offset, void *buf, int len);

#endif /* _PV_LOG_H */
