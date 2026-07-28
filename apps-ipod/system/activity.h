/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.h (activity stack)
 * Copyright (C) 2002 by Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * The current_activity enum and the push/pop API.
 ****************************************************************************/

#ifndef _ACTIVITY_H_
#define _ACTIVITY_H_

#include <stdbool.h>

enum current_activity {
    ACTIVITY_UNKNOWN = 0,
    ACTIVITY_MAINMENU,
    ACTIVITY_WPS,
    ACTIVITY_RECORDING,
    ACTIVITY_FM,
    ACTIVITY_PLAYLISTVIEWER,
    ACTIVITY_SETTINGS,
    ACTIVITY_FILEBROWSER,
    ACTIVITY_DATABASEBROWSER,
    ACTIVITY_PLUGINBROWSER,
    ACTIVITY_QUICKSCREEN,
    ACTIVITY_PITCHSCREEN,
    ACTIVITY_OPTIONSELECT,
    ACTIVITY_PLAYLISTBROWSER,
    ACTIVITY_PLUGIN,
    ACTIVITY_CONTEXTMENU,
    ACTIVITY_SYSTEMSCREEN,
    ACTIVITY_TIMEDATESCREEN,
    ACTIVITY_BOOKMARKSLIST,
    ACTIVITY_SHORTCUTSMENU,
    ACTIVITY_ID3SCREEN,
    ACTIVITY_USBSCREEN,
    ACTIVITY_ALBUMCOVERS,
    ACTIVITY_TEXTVIEWER,
    ACTIVITY_IMAGEVIEWER,
    ACTIVITY_FOLDERSELECT
};

/* custom string representation of activity */
#define MAKE_ACT_STR(act) ((char[3]){'>', 'A'+ (act), 0x0})

void push_current_activity(enum current_activity screen);
void push_activity_without_refresh(enum current_activity screen);
void pop_current_activity(void);
void pop_current_activity_without_refresh(void);
enum current_activity get_current_activity(void);

/* Generic "working" indicator for the themed status bar (%lw / "Working..").
 * Bracket a long-running foreground or background operation with
 * ui_set_working(true)/(false) to show the skin's generic busy notification.
 * Distinct from the database/thumbnail-cache "building" indicator (%lb), which
 * is driven automatically. No built-in caller yet -- reserved for future use. */
bool ui_working(void);
void ui_set_working(bool working);


#endif /* _ACTIVITY_H_ */
