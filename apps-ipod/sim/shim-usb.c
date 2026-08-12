/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The USB stack, which a simulator does not have.
 *
 * sim.h withdraws HAVE_USBSTACK and CONFIG_USBOTG, so firmware/usbstack/ is
 * not built. Most of apps-ipod/'s USB surface goes with it on its own --
 * USB_ENABLE_AUDIO and USB_ENABLE_HID are defined inside HAVE_USBSTACK in
 * config.h and apps-ipod/ guards on them. What is left is the handful below.
 *
 * The core usb_* API is not here: firmware/usb.c is built unconditionally and
 * carries a real !HAVE_USBSTACK path, so usb_init(), usb_start_monitoring(),
 * usb_acknowledge() and the rest are live. The sim can fake an insert from its
 * own menu and the USB screen runs for real.
 ****************************************************************************/
#include "config.h"

#ifdef SIMULATOR

#include <stdbool.h>
#include "action.h"
#include "usb.h"
#include "usb_core.h"
#include "usbstack/usb_hid.h"

/* browser.c and shutdown.c ask this to decide whether the disk changed under
 * them while the host had it. Nothing writes to a sim's storage over USB. */
bool usb_core_host_wrote_storage(void)
{
    return false;
}

bool usb_core_driver_enabled(int driver)
{
    (void)driver;
    return false;
}

/* Whether a host has been seen. firmware/usb.c tracks this inside the stack,
 * so the symbol goes with it. Background work (tagcache, art_cache, file_index,
 * bg_task) checks it to back off while the host owns the disk -- in a sim
 * nothing else ever does. */
bool usb_host_is_present(void)
{
    return false;
}

/* Charging-mode selection, declared under HAVE_USB_POWER. */
void usb_set_mode(int mode)
{
    (void)mode;
}

#ifdef HAVE_USB_CHARGING_ENABLE
/* Declared under HAVE_USB_CHARGING_ENABLE, which the target config sets and
 * sim.h leaves alone -- but implemented in the stack, which is gone. */
int usb_charging_maxcurrent(void)
{
    return 0;
}
#endif

/* The HID setting's callback in settings_list.c. */
void usb_set_hid(bool enable)
{
    (void)enable;
}

/* usb_keymaps.c sends HID reports for the keypad-emulation modes. */
void usb_hid_send(usage_page_t usage_page, int id)
{
    (void)usage_page;
    (void)id;
}

/* input/usb_keymaps.c itself is behind USB_ENABLE_HID in SOURCES, but the USB
 * screen calls into it without checking. -1 ends the caption loop after the
 * first entry; ACTION_NONE keeps the screen's event loop turning. */
int keypad_mode_name_get(unsigned int mode)
{
    (void)mode;
    return -1;
}

int get_hid_usb_action(void)
{
    return ACTION_NONE;
}

#endif /* SIMULATOR */
