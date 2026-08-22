/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * CheckWPS shadow of firmware/export/powermgmt.h.
 *
 * battery_default_capacity() is declared in the target's system-target.h,
 * which system.h reaches only for a native platform; settings_list.c uses it
 * for the battery-capacity setting's default.
 *
 * See ../README.
 ****************************************************************************/
#ifndef PODBOX_CHECKWPS_POWERMGMT_H
#define PODBOX_CHECKWPS_POWERMGMT_H

#include_next "powermgmt.h"

int battery_default_capacity(void);

#endif /* PODBOX_CHECKWPS_POWERMGMT_H */
