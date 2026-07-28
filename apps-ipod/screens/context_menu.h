/***************************************************************************
 * Original code from RockBox
 * was: apps/onplay.h
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * Interface to context_menu.c (context_menu_show) and its custom-action values.
 ****************************************************************************/
#ifndef _CONTEXT_MENU_H_
#define _CONTEXT_MENU_H_

#include "widgets/menu.h"

enum {
    ONPLAY_NO_CUSTOMACTION,
    ONPLAY_CUSTOMACTION_SHUFFLE_SONGS,
    ONPLAY_CUSTOMACTION_FIRSTLETTER,
};

int context_menu_show(char* file, int attr, int from_context, bool hotkey, int customaction);
int context_menu_get_source(void);

enum {
    ONPLAY_MAINMENU = -1,
    ONPLAY_OK = 0,
    ONPLAY_RELOAD_DIR,
    ONPLAY_START_PLAY,
    ONPLAY_PLAYLIST,
    ONPLAY_PLUGIN,
    ONPLAY_FUNC_RETURN, /* for use in hotkey_assignment only */
};


enum hotkey_action {
    HOTKEY_OFF = 0,
    HOTKEY_VIEW_PLAYLIST,
    HOTKEY_PROPERTIES,
    HOTKEY_PICTUREFLOW,
    HOTKEY_SHOW_TRACK_INFO,
    HOTKEY_PITCHSCREEN,
    HOTKEY_OPEN_WITH,
    HOTKEY_DELETE,
    HOTKEY_BOOKMARK,
    HOTKEY_PLUGIN,
    HOTKEY_INSERT,
    HOTKEY_INSERT_SHUFFLED,
    HOTKEY_BOOKMARK_LIST,
};
enum hotkey_flags {
    HOTKEY_FLAG_NONE = 0x0,
    HOTKEY_FLAG_WPS = 0x1,
    HOTKEY_FLAG_TREE = 0x2,
    HOTKEY_FLAG_NOSBS = 0x4,
};

struct hotkey_assignment {
    int action;             /* hotkey_action */
    int lang_id;            /* Language ID */
    struct menu_func_param func;  /* Function to run if this entry is selected */
    int16_t return_code;    /* What to return after the function is run. */
    uint16_t flags;         /* Flags what context, display options */
};                          /* (Pick ONPLAY_FUNC_RETURN to use function's return value) */

const struct hotkey_assignment *get_hotkey(int action);

/* needed for the playlist viewer.. eventually clean this up */
void context_menu_show_playlist_cat(const char* track_name, int attr,
                                   void (*add_to_pl_cb));
void context_menu_show_playlist(const char* path, int attr, void (*playlist_insert_cb));

#endif
