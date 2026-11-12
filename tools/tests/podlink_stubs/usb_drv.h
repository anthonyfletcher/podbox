#include "system.h"
#define DIR_IN 0
#define DIR_OUT 1
int usb_drv_recv_nonblocking(int ep, void *data, int size);
int usb_drv_send_nonblocking(int ep, const void *data, int size);

int usb_drv_port_speed(void);
