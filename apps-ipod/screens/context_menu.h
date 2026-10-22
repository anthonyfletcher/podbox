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

enum {
    ONPLAY_MAINMENU = -1,
    ONPLAY_OK = 0,
    ONPLAY_RELOAD_DIR,
    ONPLAY_REVEAL_FILE,
    ONPLAY_START_PLAY,
    ONPLAY_PLAYLIST,
    ONPLAY_PLUGIN,
    ONPLAY_GO_TO_LIBRARY, /* the database browser, armed to open on an entry */
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

/* The two hotkey settings hold their action as item 0 of an int that has room
   for HK_CTX_ITEMS of them; the rest are unused. */
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

/* The kinds of context menu, each with its own rows hidden */
enum context_menu_kind {
    CONTEXT_KIND_NOW_PLAYING,
    CONTEXT_KIND_TRACK,
    CONTEXT_KIND_ALBUM,
    CONTEXT_KIND_ARTIST,
    CONTEXT_KIND_FOLDER,
    CONTEXT_KIND_BOOK,
    CONTEXT_KINDS
};

/* For the screen that turns rows back on: a kind's name, and the labels of
 * the rows it can hide, in order -- row i is bit i of
 * global_settings.context_hidden[kind]. */
int context_menu_kind_label(int kind);
int context_menu_kind_rows(int kind, const short **rows);

/* config.cfg handlers for global_settings.context_hidden[] */
void context_hidden_load_from_cfg(void *setting, char *value);
char *context_hidden_write_to_cfg(void *setting, char *buf, int buf_len);
bool context_hidden_is_changed(void *setting, void *defaultval);
void context_hidden_set_default(void *setting, void *defaultval);

/* Assign a hotkey's action, driven from the settings menu. */
int wps_context_menu_do_setting(void *param);
int tree_context_menu_do_setting(void *param);

/* config.cfg handlers -- both hotkey settings share these */
void wps_context_menu_set_default(void *setting, void *defaultval);
char *wps_context_menu_write_to_cfg(void *setting, char *buf, int buf_len);
void wps_context_menu_load_from_cfg(void *setting, char *value);
bool wps_context_menu_is_changed(void *setting, void *defaultval);

/* Returns ONPLAY_START_PLAY when a row started playback. 'in_order' leaves
 * out the shuffled rows, for something heard in order, such as a book. */
int context_menu_show_playlist(const char* path, int attr, void (*playlist_insert_cb),
                               bool in_order);

/* A row a screen adds to the menu it shows for one of its own items. run()
 * returns an ONPLAY_* value; anything only the screen needs to know it keeps
 * itself. */
#define CONTEXT_ROWS_MAX 8
struct context_menu_row {
    int lang_id;
    int (*run)(void);
    bool more;          /* under More rather than at the top */
};

/* The browser's menu for one item a screen of its own shows: the ways of
 * playing it, Add to Playlist, Rate, and the screen's own rows. 'in_order' is
 * for a book: no shuffled rows, and its own rows first with the ways of
 * playing it under More. 'go_to' offers Go to Album and Go to Artist,
 * for a caller that can follow ONPLAY_GO_TO_LIBRARY. 'playlist_insert_cb'
 * puts the item's tracks into the queue at the position given, and
 * 'add_to_pl_cb' writes them to a playlist file; either may be NULL to work
 * from the path. */
int context_menu_show_item(const char *path, int attr, bool in_order,
                           bool go_to,
                           bool (*playlist_insert_cb)(int position, bool queue,
                                                      bool create_new),
                           int (*add_to_pl_cb)(const char *playlist,
                                               bool new_playlist),
                           const struct context_menu_row *rows, int count);

#endif
