/***************************************************************************
 * Original code from RockBox
 * was: apps/appevents.h
 * Copyright (C) 2008 by Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * The event ids other subsystems subscribe to -- track change, playback
 * state, settings changed.
 ****************************************************************************/

#ifndef _APPEVENTS_H
#define _APPEVENTS_H

#include <stdbool.h>
#include "events.h"

/** Only app/ level events should be defined here.
 *  firmware/ level events and CLASS's are defined in firmware/export/events.h
 */

/** Playback events **/
enum {
    /* Playback is starting from a stopped state
       data = NULL */
    PLAYBACK_EVENT_START_PLAYBACK = (EVENT_CLASS_PLAYBACK|1),
    /* Audio has begun buffering for decoding track (or is already completed)
       data = &(struct track_event){} */
    PLAYBACK_EVENT_TRACK_BUFFER,
    /* Handles for current user track are ready (other than audio or codec)
       data = &(struct track_event){}  */
    PLAYBACK_EVENT_CUR_TRACK_READY,
    /* Current user track finished
       data = &(struct track_event){}  */
    PLAYBACK_EVENT_TRACK_FINISH,
    /* A new current user track has begun
       data = &(struct track_event){} */
    PLAYBACK_EVENT_TRACK_CHANGE,
    /* A manual skip is about to be processed
       data = NULL */
    PLAYBACK_EVENT_TRACK_SKIP,
    /* Next track medadata was just loaded
       data = &(struct track_event){} */
    PLAYBACK_EVENT_NEXTTRACKID3_AVAILABLE,
};

/** VOICE events **/
enum {
    /* Voice is playing
       data = &(bool){true|false} */
    VOICE_EVENT_IS_PLAYING = (EVENT_CLASS_VOICE|1),
};

/** Buffering events **/
enum {
    BUFFER_EVENT_BUFFER_LOW = (EVENT_CLASS_BUFFERING|1),
    BUFFER_EVENT_REBUFFER,
    BUFFER_EVENT_CLOSED,
    BUFFER_EVENT_MOVED,
    BUFFER_EVENT_FINISHED,
    BUFFER_EVENT_BUFFER_RESET
};

/** Generic GUI class events **/
enum {
    /* Redraw the skin as needed (or by force), without touching the screen */
    GUI_EVENT_ACTIONREDRAW = (EVENT_CLASS_GUI|1),
    /* Redraw the skin, then update the screen if anything marked it dirty */
    GUI_EVENT_ACTIONUPDATE,
    GUI_EVENT_THEME_CHANGED,
    /* Called when the UI viewport is cleared in the skin engine to
     * notify the current screen that it needs to do an update */
    GUI_EVENT_NEED_UI_UPDATE,
};

/** Recording events **/
enum {
    RECORDING_EVENT_START = (EVENT_CLASS_RECORDING|1),
    RECORDING_EVENT_STOP,
};
#endif


