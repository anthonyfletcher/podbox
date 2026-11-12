#include "config.h"
#include "system.h"
#define USB_DRIVER_SERIAL 1
void usb_signal_class_notify(int driver, uint32_t data);
bool usb_podlink_open(void);
void usb_podlink_close(void);
bool usb_podlink_active(void);
