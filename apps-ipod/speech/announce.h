/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to announce.c: saying the track that is playing.
 ****************************************************************************/

#ifndef __ANNOUNCE_H
#define __ANNOUNCE_H

#include <stdbool.h>
#include "kernel.h"

/* talk_track_change */
enum
{
    ANNOUNCE_OFF = 0,
    ANNOUNCE_OVER_MUSIC,
    ANNOUNCE_PAUSE_MUSIC,
};

/* Posted to the UI thread, which says them through default_event_handler():
 * a track has changed, and an announcement that paused the music is over.
 * Classed with the firmware's own MISC events, numbered clear of them. */
#define SYS_ANNOUNCE_TRACK MAKE_SYS_EVENT(SYS_EVENT_CLS_MISC, 32)
#define SYS_ANNOUNCE_DONE  MAKE_SYS_EVENT(SYS_EVENT_CLS_MISC, 33)

/* Listen for track changes; once, after audio_init(). */
void announce_init(void);

/* On the UI thread, for the two events above and their data. */
void announce_event(long event, intptr_t data);

#endif /* __ANNOUNCE_H */
