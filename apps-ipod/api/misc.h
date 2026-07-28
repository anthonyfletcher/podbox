/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Boundary UMBRELLA over the six files misc.h was split into, deliberately
 * wide rather than a narrow forward. Only firmware/powermgmt.c uses
 * symbols from here (read_line, settings_parseline, both now in
 * system/strutil.h), but firmware/scroll_engine.c and firmware/usb.c also
 * include "misc.h" while using no symbol from it, so they may depend on
 * what it pulled in transitively; including all six keeps their view
 * identical to before the split. If firmware/ ever becomes editable: point
 * powermgmt.c at system/strutil.h, delete the misc.h includes in
 * scroll_engine.c and usb.c, then delete this file.
 ****************************************************************************/
#include "../system/strutil.h"
#include "../system/format_time.h"
#include "../system/shutdown.h"
#include "../system/volume.h"
#include "../system/activity.h"
#include "../system/app_util.h"
