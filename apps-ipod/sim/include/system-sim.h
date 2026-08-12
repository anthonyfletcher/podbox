/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Simulator shadow of firmware/target/hosted/sdl/system-sim.h.
 *
 * This is the general home for declarations a sim build loses. It shadows
 * system-sim.h rather than system.h because system.h pulls that header in for
 * SIMULATOR builds and nothing else, so everything below reaches every
 * translation unit without being visible to a hardware build at all.
 * apps-ipod/sim/shim-board.c implements what is declared here.
 *
 * See ../README.md.
 ****************************************************************************/
#ifndef PODBOX_SIM_SYSTEM_SIM_H
#define PODBOX_SIM_SYSTEM_SIM_H

#include_next "system-sim.h"

#include <stdbool.h>
#include "mv.h"   /* IF_MV_NONVOID; includes only stdbool, stdint and config.h */

/* On ARM these come from system-arm.h. The hosted headers have enable_irq()
 * but no FIQ pair, and main.c's bring-up calls enable_fiq(). */
#ifndef enable_fiq
#define enable_fiq()  do { } while (0)
#endif

#ifndef disable_fiq
#define disable_fiq() do { } while (0)
#endif

/* A free-running microsecond counter. Both targets have one in silicon
 * (pp5020.h, s5l87xx.h); the skin engine and the list renderer read it either
 * side of a render and a flush to attribute UI cost. Nothing depends on it
 * being a register rather than a call -- only on it counting microseconds and
 * not going backwards. */
#ifndef USEC_TIMER
unsigned long podbox_sim_usec_timer(void);
#define USEC_TIMER (podbox_sim_usec_timer())
#endif

/* power.h declares this only for PLATFORM_NATIVE, and system.h declares
 * dbg_ports() only when SIMULATOR is absent. Both are called from main.c. */
void power_init(void);
bool dbg_ports(void);

/* Declared in firmware/target/arm/pp/system-target.h, which a sim does not
 * reach, and defined in the target's battery driver. settings_list.c uses it
 * for the battery-capacity setting's default. */
int battery_default_capacity(void);

/* usb.h declares this under USB_ENABLE_HID; settings_list.c calls it from the
 * HID setting's callback regardless. */
void usb_set_hid(bool enable);

/* Declared in firmware/include/file_internal.h and rb_namespace.h, neither of
 * which can be included outside the native filesystem -- both reach
 * dircache_redirect.h, which is that filesystem's internal plumbing. */
void filesystem_init(void);
bool ns_volume_is_visible(IF_MV_NONVOID(int volume));

#endif /* PODBOX_SIM_SYSTEM_SIM_H */
