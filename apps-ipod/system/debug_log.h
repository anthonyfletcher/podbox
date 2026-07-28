/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Opt-in file logging for the two background workers whose progress is
 * otherwise invisible: the tag database and the album-art cache. Both drive
 * the same "Building" indicator, so when it stays up there is no way from the
 * UI to tell which one is busy, or why it decided it had work to do.
 *
 * Off unless the matching setting is on. When off a call costs one test of a
 * global and a return, so call sites can sit in hot loops.
 ****************************************************************************/
#ifndef _DEBUG_LOG_H
#define _DEBUG_LOG_H

#include <stdbool.h>

enum debug_log_id {
    DEBUG_LOG_TAGCACHE = 0,
    DEBUG_LOG_ARTCACHE,
    DEBUG_LOG_COUNT,
};

/* Append one timestamped line. Newline is added; do not pass one. */
void debug_log(enum debug_log_id id, const char *fmt, ...)
    ATTRIBUTE_PRINTF(2, 3);

/* True if this log is currently enabled -- for skipping work that exists
 * only to produce a log line. */
bool debug_log_enabled(enum debug_log_id id);

/* Truncate the file and write a session header. Called when the setting is
 * switched on, so each run starts from a readable point. */
void debug_log_restart(enum debug_log_id id);

#endif /* _DEBUG_LOG_H */
