/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * CheckWPS shadow of firmware/export/storage.h.
 *
 * config.h zeroes CONFIG_STORAGE for a __PCTOOL__ build, so storage.h matches
 * none of its driver branches and leaves these three undefined. settings.c
 * calls two of them and settings_list.c passes the third as a callback.
 *
 * See ../README.
 ****************************************************************************/
#ifndef PODBOX_CHECKWPS_STORAGE_H
#define PODBOX_CHECKWPS_STORAGE_H

#include_next "storage.h"

#ifndef STORAGE_FUNCTION
static inline void checkwps_storage_spindown(int seconds) { (void)seconds; }
static inline void checkwps_storage_set_storage_mode(int mode) { (void)mode; }

#define STORAGE_FUNCTION(NAME)          (checkwps_storage_## NAME)
#define storage_spindown(sec)           checkwps_storage_spindown(sec)
#define storage_set_storage_mode(mode)  checkwps_storage_set_storage_mode(mode)
#endif

#endif /* PODBOX_CHECKWPS_STORAGE_H */
