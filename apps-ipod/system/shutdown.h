/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.h (shutdown, events, car adapter)
 * Copyright (C) 2002 by Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * Interface to shutdown.c: the default event handlers and car adapter
 * init.
 ****************************************************************************/

#ifndef _SHUTDOWN_H_
#define _SHUTDOWN_H_

#include <stdbool.h>
#include "config.h"

long default_event_handler_ex(long event, void (*callback)(void *), void *parameter);
long default_event_handler(long event);
bool list_stop_handler(void);
void car_adapter_mode_init(void) INIT_ATTR;

#ifdef BOOTFILE
void check_bootfile(bool do_rolo);
#endif


#endif /* _SHUTDOWN_H_ */
