/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The diagnostic logs in .rockbox/logs, and what they share: one header
 * format, rotation at DEBUG_LOG_CAP, a "lines lost" marker, a switch per
 * log that a setting holds, and Switch Off After, which turns every log off
 * once they have been on for that long with the player on.
 *
 * Two ways to write. debug_log() is for a worker's progress lines: each is
 * stamped with the tick and held in RAM until the disk is about to sleep or
 * the buffer is three-quarters full, so a scan that logs every file does not
 * open the file for every line. A log that must be on disk before it goes on
 * -- the USB log's last line before a hang -- or that keeps its own ring
 * writes through debug_log_open() and debug_log_close() instead.
 ****************************************************************************/
#ifndef _DEBUG_LOG_H
#define _DEBUG_LOG_H

#include <stdbool.h>

enum debug_log_id {
    DEBUG_LOG_TAGCACHE = 0,
    DEBUG_LOG_ARTCACHE,
    DEBUG_LOG_USB,
    DEBUG_LOG_BATTERY,
    DEBUG_LOG_MIKEY,
    DEBUG_LOG_METADATA,
    DEBUG_LOG_COUNT,
};

/* A file at this size is renamed to <name>.1.<ext>, replacing the last one,
 * and a new one starts with a header. */
#define DEBUG_LOG_CAP (256*1024)

/* Append one tick-stamped line. Newline is added; do not pass one. When the
 * log is off a call costs a test and a return, so call sites can sit in hot
 * loops. */
void debug_log(enum debug_log_id id, const char *fmt, ...)
    ATTRIBUTE_PRINTF(2, 3);

/* True if this log is currently enabled -- for skipping work that exists
 * only to produce a log line. */
bool debug_log_enabled(enum debug_log_id id);

/* Append a session header, `what` saying what began it ("started" at boot,
 * "switched on" from the setting). Written whether or not the log is on, so
 * a debug screen can head the lines it writes. */
void debug_log_header(enum debug_log_id id, const char *what);

/* The file for appending, rotated first if it has reached the cap; -1 if it
 * cannot be opened. Holds the log's lock until debug_log_close(). */
int debug_log_open(enum debug_log_id id);
void debug_log_close(enum debug_log_id id, int fd);

/* The marker for lines a ring dropped before they reached the file. */
void debug_log_lost(enum debug_log_id id, int fd, unsigned long n,
                    const char *why);

/* Bring every log into line with its setting: a header for one switched on,
 * its held lines and a last line for one switched off. The settings'
 * callback, and run after Reset Settings, which calls no callback. Switching
 * one on starts the count towards Switch Off After again. */
void debug_log_apply(void);

/* Every log off, as Turn All Off does */
void debug_log_all_off(void);

bool debug_log_any_on(void);

/* Minutes before the logs switch themselves off; -1 if none is on or they
 * never do */
int debug_log_minutes_left(void);

/* Bytes in every log file and its rotated copy */
unsigned long debug_log_files_size(void);

/* From the root menu: the splash for logs that have just switched themselves
 * off, or at `boot`, the reminder that some are on. */
void debug_log_notice(bool boot);

/* After settings_load(), before anything logs. */
void debug_log_init(void);

#endif /* _DEBUG_LOG_H */
