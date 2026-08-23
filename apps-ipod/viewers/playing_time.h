/***************************************************************************
 * Public entry point for the core-linked Playing Time screen
 * (viewers/playing_time.c), ported from the playing_time plugin.
 * GNU General Public License (version 2+)
 *
 * Interface to playing_time.c.
 ****************************************************************************/

#ifndef _PLAYING_TIME_H_
#define _PLAYING_TIME_H_

#include <stdbool.h>

/* Show the "Playing Time" stats screen for the current playlist. True if it
 * was left for the root menu -- MENU, or a USB attach. */
bool playing_time_screen(void);

#endif /* _PLAYING_TIME_H_ */
