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
    ONPLAY_REVEAL_FILE,
    ONPLAY_START_PLAY,
    ONPLAY_PLAYLIST,
    ONPLAY_PLUGIN,
    ONPLAY_FUNC_RETURN, /* for use in hotkey_assignment only */
};


enum hotkey_action {
    HOTKEY_OFF = 0,
    HOTKEY_VIEW_PLAYLIST,
    HOTKEY_PROPERTIES,
    HOTKEY_PICTUREFLOW, /* retired -- no longer offered; kept to hold the
                           number, see the note below. get_hotkey() answers
                           HOTKEY_OFF for it, which is what an old saved
                           setting now does. */
    HOTKEY_SHOW_TRACK_INFO,
    HOTKEY_DELETE,
    HOTKEY_BOOKMARK,
    HOTKEY_INSERT,
    HOTKEY_INSERT_SHUFFLED,
    HOTKEY_BOOKMARK_LIST,
    HOTKEY_ALBUMART,
    HOTKEY_SHOW_IN_FILES,
    HOTKEY_LYRICS,
    /* Append here, never insert: these values are packed HK_CTX_BITS at a
       time into the saved context_wps/hotkey_tree integers, so renumbering
       one repoints every hotkey the user has already configured. */
    HOTKEY_CONTEXT_MENU = 0x3E, /* shows the actions above as a menu */
    /* Note no more than 62 items -- see HK_CTX_BITS */
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
    uint8_t icon;           /* themable_icons value shown beside the entry */
};                          /* (Pick ONPLAY_FUNC_RETURN to use function's return value) */

/* The WPS context menu and the file browser hotkey each store several actions
   packed into one int: item 0 is the hotkey itself, items 1..4 are the
   configurable rows at the bottom of the context menu. */
#define HK_CTX_ITEMS (5) /* 6 x 5 = 30 bits */
#define HK_CTX_MASK  (0x3F)
#define HK_CTX_BITS  (6) /* 6 bits, enough for 62 hotkey actions */
#define HK_CTX_SET(item, hotkey) ((hotkey & HK_CTX_MASK) << (item * HK_CTX_BITS))
#define HK_CTX_GET(item, hotkey) ((hotkey >> (item * HK_CTX_BITS)) & HK_CTX_MASK)

const struct hotkey_assignment *get_hotkey(int action);

/* Show the actions valid for flag as a list. With execute set the chosen one
   is run and its return code returned; otherwise the chosen action is
   returned, for the settings screens. Returns -1 if cancelled. */
int hotkey_run_menu(intptr_t flag, bool execute, int current_action);

/* Assign one packed item, driven from the settings menu. param is the item. */
int wps_context_menu_do_setting(void *param);
int tree_context_menu_do_setting(void *param);

/* config.cfg handlers -- both packed settings share these */
void wps_context_menu_set_default(void *setting, void *defaultval);
char *wps_context_menu_write_to_cfg(void *setting, char *buf, int buf_len);
void wps_context_menu_load_from_cfg(void *setting, char *value);
bool wps_context_menu_is_changed(void *setting, void *defaultval);

/* needed for the playlist viewer.. eventually clean this up */
void context_menu_show_playlist_cat(const char* track_name, int attr,
                                   void (*add_to_pl_cb));
void context_menu_show_playlist(const char* path, int attr, void (*playlist_insert_cb));

#endif
