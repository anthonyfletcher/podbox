/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The internal filesystem layer, which a simulator replaces wholesale.
 *
 * firmware/SOURCES keeps dir.c, file.c, file_internal.c, disk.c, fat.c,
 * disk_cache.c and fileobj_mgr.c behind PLATFORM_NATIVE; a sim uses
 * uisimulator/common/filesystem-sim.c and the host filesystem instead. Two
 * entry points main.c calls are lost with them.
 ****************************************************************************/
#include "config.h"

#ifdef SIMULATOR

/* Neither file_internal.h nor rb_namespace.h can be included here: both reach
 * dircache_redirect.h, which is the native filesystem's own plumbing and does
 * not compile without it. filesystem_init() and ns_volume_is_visible() are
 * declared in sim/include/system-sim.h instead. */
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "disk.h"
#include "storage.h"
#include "mv.h"

/* The sim's filesystem is already up by the time init() runs. */
void filesystem_init(void)
{
}

/* Must be positive. A zero or negative count sends init() into the "No
 * partition found" screen, which then blocks forever on
 * button_get(true) != SYS_USB_CONNECTED -- a hang, not an error. */
int disk_mount_all(void)
{
    return 1;
}

/* Only ever read on the mount-failure screen above, which cannot be reached
 * here. Named so it is obvious in a screenshot if it ever is. */
void storage_get_info(int drive, struct storage_info *info)
{
    (void)drive;

    memset(info, 0, sizeof (*info));
    info->sector_size = 512;
    info->vendor   = "PodBox";
    info->product  = "simdisk";
    info->revision = "1.0";
}

/* Partition table reads, also only on the mount-failure screen. false means
 * "no such partition", which stops the loop that prints them. */
bool disk_partinfo(int partition, struct partinfo *info)
{
    (void)partition;
    (void)info;
    return false;
}

void disk_set_sector_multiplier(IF_MD(int drive,) int mult)
{
    IF_MD((void)drive;)
    (void)mult;
}

/* Free space for the main menu's disk line. The host filesystem underneath
 * simdisk/ has its own idea of free space and the sim does not ask it. */
void volume_recalc_free(IF_MV_NONVOID(int volume))
{
    IF_MV((void)volume;)
}

/* Volume visibility, from rb_namespace.c -- multivolume, hence native. */
bool ns_volume_is_visible(IF_MV_NONVOID(int volume))
{
    IF_MV((void)volume;)
    return true;
}

/* The storage driver's event hook, used by firmware/backlight.c to spin the
 * disk down with the backlight. Nothing here spins. */
void storage_post_event(long event, intptr_t data)
{
    (void)event;
    (void)data;
}

#endif /* SIMULATOR */
