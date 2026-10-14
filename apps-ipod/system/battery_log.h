/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The battery log, at BATTERY_LOG_FILE: one line a minute of the battery's
 * voltage and level beside what the player spent the minute doing -- playing,
 * boosted, backlight, LCD, disk -- so a run that drains fast shows what it
 * was doing while it drained. Lines wait in RAM and reach the file only as
 * the disk is about to sleep, so the log never spins it up.
 ****************************************************************************/
#ifndef _BATTERY_LOG_H
#define _BATTERY_LOG_H

#include <stdbool.h>
#include "system/library_files.h"

#define BATTERY_LOG_FILE LIB_BATTERY_LOG

/* The setting's callback. */
void battery_log_enable(bool on);

void battery_log_init(void);

#endif /* _BATTERY_LOG_H */
