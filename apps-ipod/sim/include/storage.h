/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Simulator shadow of firmware/export/storage.h.
 *
 * storage_get_info() is declared -- and, for the ATA and SD drivers, defined
 * as a macro -- only under STORAGE_GET_INFO, which config.h withholds from
 * simulator builds. main.c calls it while printing the "No partition found"
 * screen. That screen is unreachable here (sim/shim-fs.c mounts successfully),
 * but it still has to compile.
 *
 * Defining STORAGE_GET_INFO instead is not an option: storage.h's own hostfs
 * branch answers that with `#error storage_get_info not implemented`.
 *
 * See ../README.md.
 ****************************************************************************/
#ifndef PODBOX_SIM_STORAGE_H
#define PODBOX_SIM_STORAGE_H

#include_next "storage.h"

#ifndef STORAGE_GET_INFO
void storage_get_info(int drive, struct storage_info *info);
#endif

#endif /* PODBOX_SIM_STORAGE_H */
