/***************************************************************************
 * Original code from RockBox
 * was: apps/status.h
 * Copyright (C) 2002 Linus Nielsen Feltzing
 * GNU General Public License (version 2+)
 *
 * The playmode enum and its accessors. The ordering is fixed -- the status
 * bar icons and the skin %mp tag depend on it.
 ****************************************************************************/
#ifndef _PLAY_STATUS_H
#define _PLAY_STATUS_H

/* Do not reorder these, inbuilt statusbar icons and the
 * skin engine %mp tag depend on this ordering. */
enum playmode
{
    STATUS_PLAY,
    STATUS_STOP,
    STATUS_PAUSE,
    STATUS_FASTFORWARD,
    STATUS_FASTBACKWARD,
    STATUS_RECORD,
    STATUS_RECORD_PAUSE,
    STATUS_RADIO,
    STATUS_RADIO_PAUSE
};

void play_status_set_ffmode(enum playmode mode);
enum playmode play_status_get_ffmode(void);
int current_playmode(void);


#endif /* _PLAY_STATUS_H */
