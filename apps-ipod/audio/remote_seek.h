/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A seek an accessory drives; see remote_seek.c. One at a time, from the
 * USB thread or the iAP thread.
 ****************************************************************************/
#ifndef _REMOTE_SEEK_H_
#define _REMOTE_SEEK_H_

#include <stdbool.h>

/* 1 forward, -1 back. False if nothing is playing to seek in. */
bool remote_seek_start(int direction);
/* Ten times a second or more while a seek may run. A track change or a
 * stop ends it without seeking; a minute with no end ends it as
 * remote_seek_stop() does. */
void remote_seek_tick(void);
/* Seeks to where the preview has got to. False if none was running. */
bool remote_seek_stop(void);
/* 0 when none is running */
int remote_seek_direction(void);
/* where the preview is, in ms */
unsigned long remote_seek_position(void);

#endif /* _REMOTE_SEEK_H_ */
