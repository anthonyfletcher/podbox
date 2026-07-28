/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/wps.h
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * Interface to wps.c, including the wps_do_action() verbs other code uses
 * to control playback.
 ****************************************************************************/
#ifndef _WPS_H_
#define _WPS_H_

#include <stdbool.h>

struct mp3entry;

/* Please don't add anything else to here... */
struct wps_state
{
    struct mp3entry *id3;
    struct mp3entry *nid3;
    int ff_rewind_count;
    bool paused;
};

long gui_wps_show(void);

enum wps_do_action_type
{
    WPS_PLAY,
    WPS_PAUSE,
    WPS_PLAYPAUSE, /* toggle */
};

void wps_do_action(enum wps_do_action_type, bool updatewps);
/* fade (if enabled) and pause the audio, optionally rewind a little */
#define pause_action(update) wps_do_action(WPS_PAUSE, update)
#define unpause_action(update) wps_do_action(WPS_PLAY, update)
#define wps_do_playpause(update) wps_do_action(WPS_PLAYPAUSE, update)

struct wps_state *get_wps_state(void);

/* in milliseconds */
#define DEFAULT_SKIP_THRESH          3000l

#endif /* _WPS_H_ */
