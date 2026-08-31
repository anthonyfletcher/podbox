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
    /* Reserved. Skins select their layout by comparing %cs against these
       values numerically, so the positions are an interface a theme is
       written against, not an internal detail: dropping an entry silently
       renumbers every activity below it and repoints each %cs test at the
       wrong screen. This slot held the pitch screen, which no longer exists;
       it stays to keep the numbering that skins already encode. Add new
       activities at the end. */
    ACTIVITY_RESERVED_11,
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
    ACTIVITY_FOLDERSELECT,
    ACTIVITY_ALBUMCHARTS,
    ACTIVITY_DOCUMENTBROWSER,
    ACTIVITY_IMAGEBROWSER,
    ACTIVITY_DB_SEARCH,
    ACTIVITY_LYRICS,
    /* Appended, never inserted: this enum is a skin ABI -- the %cs token
     * compares against these numbers, so renumbering blanks conditionals in
     * every theme that uses it. */
    ACTIVITY_PLAYBACKVIEWER,
    ACTIVITY_SETTINGS_SEARCH,
    ACTIVITY_FEATUREDARTISTS,
    ACTIVITY_SPIKE,
    ACTIVITY_FILE_SEARCH,
    ACTIVITY_PLAYLIST_SEARCH
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
