/***************************************************************************
 * Original code from RockBox
 * was: apps/onplay.c
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * The long-press context menu. Builds the item list appropriate to what
 * was selected (file, directory, playlist entry) and runs the chosen
 * operation.
 *
 * Menus here are built from static MENUITEM_* macro declarations rather than
 * assembled at runtime. Each macro declares a struct at file scope, and which
 * items actually appear is decided by each item's callback returning
 * ACTION_EXIT_MENUITEM to hide itself -- so reading this file means reading
 * the callbacks, not looking for list-building code.
 *
 * The caller sets the target through selected_file_set() before showing the
 * menu; the operations below then act on that stored path rather than taking
 * it as a parameter.
 *
 * Parts, in order:
 *   - the selected-file target, and the cut/copy/paste clipboard
 *   - playlist operations: insert, queue, add to a named or new playlist
 *   - bookmark, playback and rating items with their visibility callbacks
 *   - file operations: cut, copy, paste, delete, rename, properties
 *   - the configurable tail of the WPS menu, whose rows are assigned by the
 *     user rather than declared here
 *   - the hotkey action table, the picker built from it, and the config.cfg
 *     handlers for the two packed settings that store the assignments
 *   - the menu declarations, and context_menu_show*() which run them
 ****************************************************************************/
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "debug.h"
#include "lcd.h"
#include "audio.h"
#include "widgets/menu.h"
#include "lang.h"
#include "playlist/playlist.h"
#include "button.h"
#include "kernel.h"
#include "widgets/keyboard.h"
#include "mp3data.h"
#include "metadata.h"
#include "screens/playback/track_info.h"
#include "widgets/text_box.h"
#include "screens/browse/browser.h"
#include "settings/settings.h"
#include "playlist/viewer.h"
#include "speech/talk.h"
#include "context_menu.h"
#include "files/filetypes.h"
#include "viewers/image_viewer/image_viewer_pub.h"
#include "viewers/lyric_viewer/lyric_viewer.h"
#include "viewers/properties.h"
#include "viewers/playing_time.h"
#include "files/file_ops.h"
#include "bookmark.h"
#include "input/action.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"
#include "screens/settings/exported_settings.h"
#include "draw/icon_bitmaps.h"
#include "playlist/save_screen.h"
#include "playlist/catalog.h"
#include "screens/browse/browser_db.h"
#include "metadata/cuesheet.h"
#include "skin/statusbar_skinned.h"
#include "draw/viewport.h"
#include "pathfuncs.h"
#include "shortcuts.h"
#include "root_menu.h"
#include "speech/language.h"
#include "system/activity.h"
#include "system/strutil.h"
#include "system/app_util.h"
#include "system/shutdown.h"
#include "storage.h"
#include "string-extra.h"
#include "dir.h"

static int context_menu_result = ONPLAY_OK;
static bool in_queue_submenu = false;

static bool (*ctx_current_playlist_insert)(int position, bool queue, bool create_new);
static int (*ctx_add_to_playlist)(const char* playlist, bool new_playlist);
extern struct menu_item_ex file_menu; /* settings_menu.c  */

/* redefine MAKE_MENU so the MENU_EXITAFTERTHISMENU flag can be added easily */
#define MAKE_ONPLAYMENU( name, str, callback, icon, ... )               \
    static const struct menu_item_ex *name##_[]  = {__VA_ARGS__};       \
    static const struct menu_callback_with_desc name##__ = {callback,str,icon};\
    static const struct menu_item_ex name =                             \
        {MT_MENU|MENU_HAS_DESC|MENU_EXITAFTERTHISMENU|                  \
         MENU_ITEM_COUNT(sizeof( name##_)/sizeof(*name##_)),            \
            { (void*)name##_},{.callback_and_desc = & name##__}};

static struct selected_file
{
    char buf[MAX_PATH];
    const char *path;
    int attr;
    int context;
} selected_file;

static struct clipboard
{
    char path[MAX_PATH];    /* Clipped file's path */
    unsigned int attr;      /* Clipped file's attributes */
    unsigned int flags;     /* Operation type flags */
} clipboard;

/* set selected file (doesn't touch buffer) */
static void selected_file_set(int context, const char *path, int attr)
{
    selected_file.path     = path;
    selected_file.attr     = attr;
    selected_file.context  = context;
}

/* Empty the clipboard */
static void clipboard_clear_selection(struct clipboard *clip)
{
    clip->path[0] = '\0';
    clip->attr    = 0;
    clip->flags   = 0;
}

/* Store the selection in the clipboard */
static bool clipboard_clip(struct clipboard *clip, const char *path,
                           unsigned int attr, unsigned int flags)
{
    /* if it fits it clips */
    if (strmemccpy(clip->path, path, sizeof (clip->path)) != NULL)
    {
        clip->attr = attr;
        clip->flags = flags;
        return true;
    }
    else {
        clipboard_clear_selection(clip);
        return false;
    }
}

/* ----------------------------------------------------------------------- */
/* Displays the bookmark menu options for the user to decide.  This is an  */
/* interface function.                                                     */
/* ----------------------------------------------------------------------- */


static int bookmark_load_menu_wrapper(void)
{
    if (get_current_activity() == ACTIVITY_CONTEXTMENU)  /* get rid of parent activity */
        pop_current_activity_without_refresh();          /* when called from ctxt menu */

    return bookmark_load_menu();
}

static int bookmark_menu_callback(int action,
                                  const struct menu_item_ex *this_item,
                                  struct gui_synclist *this_list);
MENUITEM_FUNCTION(bookmark_create_menu_item, 0,
                  ID2P(LANG_BOOKMARK_MENU_CREATE),
                  bookmark_create_menu,
                  bookmark_menu_callback, Icon_Bookmark);
MENUITEM_FUNCTION(bookmark_load_menu_item, 0,
                  ID2P(LANG_BOOKMARK_MENU_LIST),
                  bookmark_load_menu_wrapper,
                  bookmark_menu_callback, Icon_Bookmark);
MAKE_ONPLAYMENU(bookmark_menu, ID2P(LANG_BOOKMARK_MENU),
                bookmark_menu_callback, Icon_Bookmark,
                &bookmark_create_menu_item, &bookmark_load_menu_item);
static int bookmark_menu_callback(int action,
                                  const struct menu_item_ex *this_item,
                                  struct gui_synclist *this_list)
{
    (void) this_list;
    if (action == ACTION_REQUEST_MENUITEM)
    {
        /* hide loading bookmarks menu if no bookmarks exist */
        if (this_item == &bookmark_load_menu_item)
        {
            if (!bookmark_exists())
                return ACTION_EXIT_MENUITEM;
        }
    }
    else if (action == ACTION_EXIT_MENUITEM)
        settings_save();

    return action;
}

/* CONTEXT_WPS playlist options */
static bool shuffle_playlist(void)
{
    if (!yesno_pop_confirm(ID2P(LANG_SHUFFLE)))
        return false;
    playlist_sort(NULL, true);
    playlist_randomise(NULL, current_tick, true);
    playlist_set_modified(NULL, true);

    return false;
}

static bool save_playlist(void)
{
    /* save_playlist_screen should load the newly saved playlist and resume */
    save_playlist_screen(NULL);
    return false;
}

static int wps_view_cur_playlist(void)
{
    if (get_current_activity() == ACTIVITY_CONTEXTMENU)  /* get rid of parent activity */
        pop_current_activity_without_refresh(); /* when called from ctxt menu */

    playlist_viewer_ex(NULL, NULL);

    return 0;
}

static void playing_time(void)
{
    playing_time_screen();
}

static void view_album_art(void)
{
    image_viewer(NULL);
}

/* Returns its own result rather than a fixed one, so that leaving the lyrics
 * because USB was plugged in lands at the root and not back in the WPS. */
static int view_lyrics(void)
{
    if (lyric_viewer() == GO_TO_ROOT)
        return ONPLAY_MAINMENU;
    return ONPLAY_OK;
}

MENUITEM_FUNCTION(wps_view_cur_playlist_item, 0, ID2P(LANG_VIEW_DYNAMIC_PLAYLIST),
                  wps_view_cur_playlist, NULL, Icon_NOICON);
MENUITEM_FUNCTION(search_playlist_item, 0, ID2P(LANG_SEARCH_IN_PLAYLIST),
                  search_playlist, NULL, Icon_Playlist);
MENUITEM_FUNCTION(playlist_save_item, 0, ID2P(LANG_SAVE_DYNAMIC_PLAYLIST),
                  save_playlist, NULL, Icon_Playlist);
MENUITEM_FUNCTION(reshuffle_item, 0, ID2P(LANG_SHUFFLE_PLAYLIST),
                  shuffle_playlist, NULL, Icon_Playlist);
MENUITEM_FUNCTION(playing_time_item, 0, ID2P(LANG_PLAYING_TIME),
                  playing_time, NULL, Icon_Playlist);
MAKE_ONPLAYMENU( wps_playlist_menu, ID2P(LANG_CURRENT_PLAYLIST),
                 NULL, Icon_Playlist,
                 &wps_view_cur_playlist_item, &playlist_save_item,
                 &search_playlist_item, &reshuffle_item, &playing_time_item
               );

/* argument for add_to_playlist (for use by menu callbacks) */
#define PL_NONE    0x00
#define PL_QUEUE   0x01
#define PL_REPLACE 0x02
struct add_to_pl_param
{
    int8_t position;
    uint8_t flags;
};

static struct add_to_pl_param addtopl_insert           = {PLAYLIST_INSERT, PL_NONE};
static struct add_to_pl_param addtopl_insert_first     = {PLAYLIST_INSERT_FIRST, PL_NONE};
static struct add_to_pl_param addtopl_insert_last      = {PLAYLIST_INSERT_LAST, PL_NONE};
static struct add_to_pl_param addtopl_insert_shuf      = {PLAYLIST_INSERT_SHUFFLED, PL_NONE};
static struct add_to_pl_param addtopl_insert_last_shuf = {PLAYLIST_INSERT_LAST_SHUFFLED, PL_NONE};

static struct add_to_pl_param addtopl_queue            = {PLAYLIST_INSERT, PL_QUEUE};
static struct add_to_pl_param addtopl_queue_first      = {PLAYLIST_INSERT_FIRST, PL_QUEUE};
static struct add_to_pl_param addtopl_queue_last       = {PLAYLIST_INSERT_LAST, PL_QUEUE};
static struct add_to_pl_param addtopl_queue_shuf       = {PLAYLIST_INSERT_SHUFFLED, PL_QUEUE};
static struct add_to_pl_param addtopl_queue_last_shuf  = {PLAYLIST_INSERT_LAST_SHUFFLED, PL_QUEUE};

static struct add_to_pl_param addtopl_replace          = {PLAYLIST_INSERT, PL_REPLACE};
static struct add_to_pl_param addtopl_replace_shuffled = {PLAYLIST_INSERT_LAST_SHUFFLED, PL_REPLACE};

static void op_playlist_insert_selected(int position, bool queue)
{
    if (selected_file.context == CONTEXT_STD && ctx_current_playlist_insert != NULL)
    {
        ctx_current_playlist_insert(position, queue, false);
        return;
    }
    else if (selected_file.context == CONTEXT_ID3DB)
    {
        browser_db_current_playlist_insert(position, queue);
        return;
    }
    if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO)
        playlist_insert_track(NULL, selected_file.path, position, queue, true);
    else if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_M3U)
        playlist_insert_playlist(NULL, selected_file.path, position, queue);
    else if (selected_file.attr & ATTR_DIRECTORY)
    {
        bool recurse = (global_settings.recursive_dir_insert == RECURSE_ON);
        if (global_settings.recursive_dir_insert == RECURSE_ASK)
        {

            const char *lines[] = {
                ID2P(LANG_RECURSE_DIRECTORY_QUESTION),
                selected_file.path
            };
            const struct text_message message={lines, 2};
            /* Ask if user wants to recurse directory */
            recurse = (gui_syncyesno_run(&message, NULL, NULL)==YESNO_YES);
        }

        playlist_insert_directory(NULL, selected_file.path, position, queue,
                                  recurse == RECURSE_ON, NULL);
    }
}

/* CONTEXT_[TREE|ID3DB|STD] playlist options */
static int add_to_playlist(void* arg)
{
    struct add_to_pl_param* param = arg;
    int position = param->position;
    bool new_playlist = (param->flags & PL_REPLACE) == PL_REPLACE;
    bool queue = (param->flags & PL_QUEUE) == PL_QUEUE;

    /* warn if replacing the playlist */
    if (new_playlist && !warn_on_pl_erase())
        return 1;

    splash(0, ID2P(LANG_WAIT));

    if (new_playlist && global_settings.keep_current_track_on_replace_playlist)
    {
        if (audio_status() & AUDIO_STATUS_PLAY)
        {
            playlist_remove_all_tracks(NULL);
            new_playlist = false;
        }
    }

    if (new_playlist)
        playlist_create(NULL, NULL);

    /* always set seed before inserting shuffled */
    if (position == PLAYLIST_INSERT_SHUFFLED ||
        position == PLAYLIST_INSERT_LAST_SHUFFLED)
    {
        srand(current_tick);
        if (position == PLAYLIST_INSERT_LAST_SHUFFLED)
            playlist_set_last_shuffled_start();
    }

    op_playlist_insert_selected(position, queue);

    /* Replacing the playlist re-decides the artwork under the "auto" WPS art
     * source; queueing onto the one already playing deliberately does not.
     * Set unconditionally on a replace rather than only for a database browse:
     * the keep-current-track path above skips playlist_create(), so nothing
     * else clears what a previous artist browse left behind. */
    if ((param->flags & PL_REPLACE) == PL_REPLACE)
        playlist_set_from_artist(selected_file.context == CONTEXT_ID3DB &&
                                 browser_db_current_under_artist_level());

    if (new_playlist && (playlist_amount() > 0))
    {
        /* nothing is currently playing so begin playing what we just
           inserted */
        if (global_settings.playlist_shuffle)
            playlist_shuffle(current_tick, -1);
        playlist_start(0, 0, 0);
        context_menu_result = ONPLAY_START_PLAY;
    }

    playlist_set_modified(NULL, true);
    return 0;
}

static bool view_playlist(void)
{
    bool result;

    result = playlist_viewer_ex(selected_file.path, NULL);

    if (result == PLAYLIST_VIEWER_OK &&
        context_menu_result == ONPLAY_OK)
        /* playlist was started from viewer */
        context_menu_result = ONPLAY_START_PLAY;

    return result;
}

static int treeplaylist_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list);

/* insert items */
MENUITEM_FUNCTION_W_PARAM(i_pl_item, 0, ID2P(LANG_ADD),
                  add_to_playlist, &addtopl_insert,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(i_first_pl_item, 0, ID2P(LANG_PLAY_NEXT),
                  add_to_playlist, &addtopl_insert_first,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(i_last_pl_item, 0, ID2P(LANG_PLAY_LAST),
                  add_to_playlist, &addtopl_insert_last,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(i_shuf_pl_item, 0, ID2P(LANG_ADD_SHUFFLED),
                  add_to_playlist, &addtopl_insert_shuf,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(i_last_shuf_pl_item, 0, ID2P(LANG_PLAY_LAST_SHUFFLED),
                  add_to_playlist, &addtopl_insert_last_shuf,
                  treeplaylist_callback, Icon_Playlist);
/* queue items */
MENUITEM_FUNCTION_W_PARAM(q_pl_item, 0, ID2P(LANG_QUEUE),
                  add_to_playlist, &addtopl_queue,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(q_first_pl_item, 0, ID2P(LANG_QUEUE_FIRST),
                  add_to_playlist, &addtopl_queue_first,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(q_last_pl_item, 0, ID2P(LANG_QUEUE_LAST),
                  add_to_playlist, &addtopl_queue_last,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(q_shuf_pl_item, 0, ID2P(LANG_QUEUE_SHUFFLED),
                  add_to_playlist, &addtopl_queue_shuf,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(q_last_shuf_pl_item, 0, ID2P(LANG_QUEUE_LAST_SHUFFLED),
                  add_to_playlist, &addtopl_queue_last_shuf,
                  treeplaylist_callback, Icon_Playlist);

/* queue submenu */
MAKE_ONPLAYMENU(queue_menu, ID2P(LANG_QUEUE_MENU),
                treeplaylist_callback, Icon_Playlist,
                &q_first_pl_item,
                &q_pl_item,
                &q_shuf_pl_item,
                &q_last_pl_item,
                &q_last_shuf_pl_item);

/* replace playlist */
MENUITEM_FUNCTION_W_PARAM(replace_pl_item, 0, ID2P(LANG_PLAY),
                  add_to_playlist, &addtopl_replace,
                  treeplaylist_callback, Icon_Playlist);

MENUITEM_FUNCTION_W_PARAM(replace_shuf_pl_item, 0, ID2P(LANG_PLAY_SHUFFLED),
                  add_to_playlist, &addtopl_replace_shuffled,
                  treeplaylist_callback, Icon_Playlist);

MAKE_ONPLAYMENU(browser_playlist_menu, ID2P(LANG_PLAYING_NEXT),
                treeplaylist_callback, Icon_Playlist,

                /* insert */
                &i_first_pl_item,
                &i_pl_item,
                &i_last_pl_item,
                &i_shuf_pl_item,
                &i_last_shuf_pl_item,

                /* queue */
                &q_first_pl_item,
                &q_pl_item,
                &q_last_pl_item,
                &q_shuf_pl_item,
                &q_last_shuf_pl_item,

                /* Queue submenu */
                &queue_menu,

                /* replace */
                &replace_pl_item,
                &replace_shuf_pl_item
               );

static int treeplaylist_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
    (void)this_list;
    int sel_file_attr = (selected_file.attr & FILE_ATTR_MASK);

    switch (action)
    {
    case ACTION_REQUEST_MENUITEM:
        if (this_item == &browser_playlist_menu)
        {
            if (sel_file_attr != FILE_ATTR_AUDIO &&
                sel_file_attr != FILE_ATTR_M3U &&
                (selected_file.attr & ATTR_DIRECTORY) == 0)
                return ACTION_EXIT_MENUITEM;
        }
        else if (this_item == &queue_menu)
        {
            if (global_settings.show_queue_options != QUEUE_SHOW_IN_SUBMENU)
                return ACTION_EXIT_MENUITEM;

            /* queueing options only work during playback */
            if (!(audio_status() & AUDIO_STATUS_PLAY))
                return ACTION_EXIT_MENUITEM;
        }
        else if ((this_item->flags & MENU_TYPE_MASK) == MT_FUNCTION_CALL_W_PARAM &&
                 this_item->function_param->function_w_param == add_to_playlist)
        {
            struct add_to_pl_param *param = this_item->function_param->param;

            if ((param->flags & PL_QUEUE) == PL_QUEUE)
            {
                if (global_settings.show_queue_options != QUEUE_SHOW_AT_TOPLEVEL &&
                    !in_queue_submenu)
                    return ACTION_EXIT_MENUITEM;
            }

            if (param->position == PLAYLIST_INSERT_SHUFFLED ||
                param->position == PLAYLIST_INSERT_LAST_SHUFFLED)
            {
                if (!global_settings.show_shuffled_adding_options)
                    return ACTION_EXIT_MENUITEM;

                if (sel_file_attr != FILE_ATTR_M3U &&
                    (selected_file.attr & ATTR_DIRECTORY) == 0)
                    return ACTION_EXIT_MENUITEM;
            }

            if ((param->flags & PL_REPLACE) != PL_REPLACE)
            {
                if (!(audio_status() & AUDIO_STATUS_PLAY))
                    return ACTION_EXIT_MENUITEM;
            }
        }

        break;

    case ACTION_ENTER_MENUITEM:
        in_queue_submenu = this_item == &queue_menu;
        break;
    }

    return action;
}

void context_menu_show_playlist(const char* path, int attr, void (*playlist_insert_cb))
{
    ctx_current_playlist_insert = playlist_insert_cb;
    selected_file_set(CONTEXT_STD, path, attr);
    in_queue_submenu = false;
    do_menu(&browser_playlist_menu, NULL, NULL, false);
}

/* playlist catalog options */
static bool cat_add_to_a_playlist(void)
{
    return catalog_add_to_a_playlist(selected_file.path, selected_file.attr,
                                     false, NULL, ctx_add_to_playlist);
}

static bool cat_add_to_a_new_playlist(void)
{
    return catalog_add_to_a_playlist(selected_file.path, selected_file.attr,
                                     true, NULL, ctx_add_to_playlist);
}

static int cat_playlist_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list);

MENUITEM_FUNCTION(cat_add_to_list, 0, ID2P(LANG_ADD_TO_EXISTING_PL),
                  cat_add_to_a_playlist, NULL, Icon_Playlist);
MENUITEM_FUNCTION(cat_add_to_new, 0, ID2P(LANG_CATALOG_ADD_TO_NEW),
                  cat_add_to_a_new_playlist, NULL, Icon_Playlist);
MAKE_ONPLAYMENU(cat_playlist_menu, ID2P(LANG_ADD_TO_PL),
                cat_playlist_callback, Icon_Playlist,
                &cat_add_to_list, &cat_add_to_new);

void context_menu_show_playlist_cat(const char* track_name, int attr, void (*add_to_pl_cb))
{
    ctx_add_to_playlist = add_to_pl_cb;
    selected_file_set(CONTEXT_STD, track_name, attr);
    do_menu(&cat_playlist_menu, NULL, NULL, false);
}

static int cat_playlist_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (!selected_file.path ||
        (((selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO) &&
         ((selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_M3U) &&
         ((selected_file.attr & ATTR_DIRECTORY) == 0)))
    {
        return ACTION_EXIT_MENUITEM;
    }

    if (action == ACTION_REQUEST_MENUITEM)
    {
        if ((audio_status() & AUDIO_STATUS_PLAY)
           || selected_file.context != CONTEXT_WPS)
        {
            return action;
        }
        return ACTION_EXIT_MENUITEM;
    }
    return action;
}

static void splash_cancelled(void)
{
    clear_screen_buffer(true);
    splash(HZ, ID2P(LANG_CANCEL));
}

static void splash_failed(int lang_what, int err)
{
    cond_talk_ids_fq(lang_what, LANG_FAILED);
    clear_screen_buffer(true);
    splashf(HZ*2, "%s %s (%d)", str(lang_what), str(LANG_FAILED), err);
}

static bool clipboard_cut(void)
{
    return clipboard_clip(&clipboard, selected_file.path, selected_file.attr,
                          PASTE_CUT);
}

static bool clipboard_copy(void)
{
    return clipboard_clip(&clipboard, selected_file.path, selected_file.attr,
                          PASTE_COPY);
}

/* Paste the clipboard to the current directory */
static int clipboard_paste(void)
{
    if (!clipboard.path[0])
        return 1;

    int rc = copy_move_fileobject(clipboard.path, getcwd(NULL, 0), clipboard.flags);

    switch (rc)
    {
    case FORC_CANCELLED:
        splash_cancelled();
        /* Fallthrough */
    case FORC_SUCCESS:
        context_menu_result = ONPLAY_RELOAD_DIR;
        /* Fallthrough */
    case FORC_NOOP:
        clipboard_clear_selection(&clipboard);
        /* Fallthrough */
    case FORC_NOOVERWRT:
        break;
    default:
        if (rc < FORC_SUCCESS) {
            splash_failed(LANG_PASTE, rc);
            context_menu_result = ONPLAY_RELOAD_DIR;
        }
    }

    return 1;
}

static int set_rating_inline(void)
{
    struct mp3entry* id3 = audio_current_track();
    if (id3 && id3->tagcache_idx && global_settings.runtimedb)
    {
        set_int_ex(str(LANG_MENU_SET_RATING), "", UNIT_INT, (void*)(&id3->rating),
                   NULL, 1, 0, 10, NULL, NULL);
        tagcache_update_numeric(id3->tagcache_idx-1, tag_rating, id3->rating);
    }
    else
        splash(HZ*2, ID2P(LANG_ID3_NO_INFO));
    return 0;
}
static int ratingitem_callback(int action,
                               const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM)
    {
        if (!selected_file.path || !global_settings.runtimedb || !tagcache_is_usable())
            return ACTION_EXIT_MENUITEM;
    }
    return action;
}
MENUITEM_FUNCTION(rating_item, 0, ID2P(LANG_MENU_SET_RATING),
                  set_rating_inline,
                  ratingitem_callback, Icon_Questionmark);
static bool view_cue(void)
{
    struct mp3entry* id3 = audio_current_track();
    if (id3 && id3->cuesheet)
    {
        browse_cuesheet(id3->cuesheet);
    }
    return false;
}
static int view_cue_item_callback(int action,
                                  const struct menu_item_ex *this_item,
                                  struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    struct mp3entry* id3 = audio_current_track();
    if (action == ACTION_REQUEST_MENUITEM)
    {
        if (!selected_file.path || !id3 || !id3->cuesheet)
            return ACTION_EXIT_MENUITEM;
    }
    return action;
}
MENUITEM_FUNCTION(view_cue_item, 0, ID2P(LANG_BROWSE_CUESHEET),
                  view_cue, view_cue_item_callback, Icon_NOICON);


static int browse_id3_wrapper(void)
{
    if (get_current_activity() == ACTIVITY_CONTEXTMENU)  /* get rid of parent activity */
        pop_current_activity_without_refresh();          /* when called from ctxt menu */

    if (browse_id3(audio_current_track(),
            playlist_get_display_index(),
            playlist_amount(), NULL, 1, view_text))
        return GO_TO_ROOT;
    return GO_TO_PREVIOUS;
}

static int clipboard_delete_selected_fileobject(void)
{
    int rc = delete_fileobject(selected_file.path);
    if (rc < FORC_SUCCESS) {
        splash_failed(LANG_DELETE, rc);
    } else if (rc == FORC_CANCELLED) {
        splash_cancelled();
    }
    if (rc != FORC_NOOP) {
        /* Could have failed after some but not all needed changes; reload */
        context_menu_result = ONPLAY_RELOAD_DIR;
    }
    return 1;
}

static void show_result(int rc, int lang_what)
{
    if (rc < FORC_SUCCESS) {
        splash_failed(lang_what, rc);
    } else if (rc == FORC_CANCELLED) {
        /* splash_cancelled(); kbd_input() splashes it */
    } else if (rc == FORC_SUCCESS) {
        context_menu_result = ONPLAY_RELOAD_DIR;
    }
}

static int clipboard_create_dir(void)
{
    int rc = create_dir();

    show_result(rc, LANG_CREATE_DIR);

    return 1;
}

static int clipboard_rename_selected_file(void)
{
    int rc = rename_file(selected_file.path);

    show_result(rc, LANG_RENAME);

    return 1;
}

/* CONTEXT_[TREE|ID3DB] items */
static int clipboard_callback(int action,
                              const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list);

MENUITEM_FUNCTION(rename_file_item, 0, ID2P(LANG_RENAME),
                  clipboard_rename_selected_file, clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION(clipboard_cut_item, 0, ID2P(LANG_CUT),
                  clipboard_cut, clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION(clipboard_copy_item, 0, ID2P(LANG_COPY),
                  clipboard_copy, clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION(clipboard_paste_item, 0, ID2P(LANG_PASTE),
                  clipboard_paste, clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION(delete_file_item, 0, ID2P(LANG_DELETE),
                  clipboard_delete_selected_fileobject, clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION(delete_dir_item, 0, ID2P(LANG_DELETE_DIR),
                 clipboard_delete_selected_fileobject, clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION(create_dir_item, 0, ID2P(LANG_CREATE_DIR),
                  clipboard_create_dir, clipboard_callback, Icon_NOICON);

static bool prepare_database_sel(void *param);

/* Leave for the file browser, opened on the selected file. From the database
   the selection is resolved to a real path first; a table resolves to its
   first entry. */
static int reveal(void)
{
    if (!prepare_database_sel(NULL))
        return 0;

    if (!file_exists(selected_file.path))
    {
        splash(HZ*2, ID2P(LANG_FILE_NOT_FOUND));
        return 0;
    }

    browser_reveal_on_next_load(selected_file.path);
    context_menu_result = ONPLAY_REVEAL_FILE;
    return 0;
}

MENUITEM_FUNCTION(reveal_item, 0, ID2P(LANG_SHOW_IN_FILES),
                  reveal, clipboard_callback, Icon_file_view_menu);

static bool prepare_database_sel(void *param)
{
    if (selected_file.context == CONTEXT_ID3DB)
    {
        if (param && !strcmp(param, "properties")
            && (selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO)
        {
            strmemccpy(selected_file.buf, MAKE_ACT_STR(ACTIVITY_DATABASEBROWSER),
                       sizeof(selected_file.buf));
        }
        else
        {
            /* If database is not loaded into RAM, or tagcache_ram is
               set to "quick", filename needs to be retrieved from disk! */
            if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO
                && !storage_disk_is_active()
                && (global_settings.tagcache_ram != TAGCACHE_RAM_ON
                    || !tagcache_is_in_ram())
            )
                splash(0, ID2P(LANG_WAIT));
             if (!browser_db_get_subentry_filename(selected_file.buf, MAX_PATH))
            {
                context_menu_result = ONPLAY_RELOAD_DIR;
                return false;
            }
        }

        selected_file.path = selected_file.buf;
    }
    return true;
}

/* Two indirections, both forced by the menu machinery. prepare_database_sel()
 * resolves selected_file.path, which is not otherwise set in a database browse
 * context; and properties()'s GO_TO_* comes back through the
 * context_menu_result side-channel, because do_menu() discards this function's
 * own return value unless the item carries MENU_FUNC_CHECK_RETVAL. */
static bool context_menu_properties(void *param)
{
    (void)param;
    if (!prepare_database_sel((void *)"properties"))
        return false;

    if (get_current_activity() == ACTIVITY_CONTEXTMENU)  /* get rid of parent activity */
        pop_current_activity_without_refresh();          /* when called from ctxt menu */

    int ret = properties(selected_file.path);
    if (ret == GO_TO_ROOT)
        context_menu_result = ONPLAY_MAINMENU;
    return false;
}

MENUITEM_FUNCTION_W_PARAM(properties_item, 0, ID2P(LANG_PROPERTIES),
                  context_menu_properties, NULL,
                  clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(track_info_item, 0, ID2P(LANG_MENU_SHOW_ID3_INFO),
                  context_menu_properties, NULL,
                  clipboard_callback, Icon_NOICON);
static bool context_menu_add_to_shortcuts(void)
{
    /* A directory shortcut browses into it; a file shortcut runs the file
     * (SHORTCUT_FILE sets BROWSE_RUNFILE), rather than just opening its folder. */
    shortcuts_add((selected_file.attr & ATTR_DIRECTORY) ? SHORTCUT_BROWSER
                                                        : SHORTCUT_FILE,
                  selected_file.path);
    return false;
}
MENUITEM_FUNCTION(add_to_faves_item, 0, ID2P(LANG_ADD_TO_FAVES),
                  context_menu_add_to_shortcuts,
                  clipboard_callback, Icon_NOICON);

static void set_dir_helper(char* dirnamebuf, size_t bufsz)
{
    path_append(dirnamebuf, selected_file.path, PA_SEP_HARD, bufsz);
    settings_save();
}


static void show_updated_backdrop(void)
{
    skin_backdrop_load_setting();
    viewportmanager_theme_changed(THEME_STATUSBAR);
    skin_backdrop_show(sb_get_backdrop(SCREEN_MAIN));
}

static bool set_backdrop(void)
{
    char previous_backdrop[sizeof global_settings.backdrop_file];
    strcpy(previous_backdrop, global_settings.backdrop_file);

    path_append(global_settings.backdrop_file, selected_file.path,
                PA_SEP_HARD, sizeof(global_settings.backdrop_file));

    show_updated_backdrop();

    if (!yesno_pop(ID2P(LANG_SET_AS_BACKDROP))) {
        strcpy(global_settings.backdrop_file, previous_backdrop);
        show_updated_backdrop();
    }
    else
        settings_save();

    return true;
}
MENUITEM_FUNCTION(set_backdrop_item, 0, ID2P(LANG_SET_AS_BACKDROP),
                  set_backdrop, clipboard_callback, Icon_NOICON);
static bool set_startdir(void)
{
    set_dir_helper(global_settings.start_directory,
                   sizeof(global_settings.start_directory));
    return false;
}
MENUITEM_FUNCTION(set_startdir_item, 0, ID2P(LANG_START_DIR),
                  set_startdir, clipboard_callback, Icon_file_view_menu);

static bool set_catalogdir(void)
{
    catalog_set_directory(selected_file.path);
    settings_save();
    return false;
}
MENUITEM_FUNCTION(set_catalogdir_item, 0, ID2P(LANG_PLAYLIST_DIR),
                  set_catalogdir, clipboard_callback, Icon_Playlist);

static bool set_databasedir(void)
{
    struct tagcache_stat *tc_stat = tagcache_get_stat();
    if (strcasecmp(selected_file.path, tc_stat->db_path))
    {
        splash(HZ, ID2P(LANG_PLEASE_REBOOT));
    }

    set_dir_helper(global_settings.tagcache_db_path,
                   sizeof(global_settings.tagcache_db_path));
    return false;
}
MENUITEM_FUNCTION(set_databasedir_item, 0, ID2P(LANG_DATABASE_DIR),
                  set_databasedir, clipboard_callback, Icon_Audio);

MAKE_ONPLAYMENU(set_as_dir_menu, ID2P(LANG_SET_AS),
                clipboard_callback, Icon_NOICON,
                &set_catalogdir_item,
                &set_databasedir_item,
                &set_startdir_item);

static int clipboard_callback(int action,
                              const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list)
{
    (void)this_list;
    switch (action)
    {
        case ACTION_REQUEST_MENUITEM:
            /* no rename+delete for volumes */
            if ((selected_file.attr & ATTR_VOLUME) &&
                (this_item == &rename_file_item ||
                 this_item == &delete_dir_item ||
                 this_item == &clipboard_cut_item))
                return ACTION_EXIT_MENUITEM;
            if (selected_file.context == CONTEXT_ID3DB)
            {
                if (this_item == &track_info_item ||
                    this_item == &reveal_item)
                    return action;
                return ACTION_EXIT_MENUITEM;
            }
            if (this_item == &clipboard_paste_item)
            {  /* visible if there is something to paste */
                return (clipboard.path[0] != 0) ?
                                    action : ACTION_EXIT_MENUITEM;
            }
            else if (this_item == &create_dir_item &&
                     *browser_get_context()->dirfilter <= NUM_FILTER_MODES)
            {
                return action;
            }
            else if (selected_file.path)
            {
                /* requires an actual file */
                if (this_item == &clipboard_cut_item ||
                    this_item == &clipboard_copy_item)
                {
                    if (*browser_get_context()->dirfilter != SHOW_M3U)
                        return action;
                }
                else if (this_item == &rename_file_item ||
                    (this_item == &track_info_item &&
                        (selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO) ||
                    (this_item == &properties_item &&
                        (selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO) ||
                    this_item == &add_to_faves_item)
                {
                    return action;
                }
                else if ((selected_file.attr & ATTR_DIRECTORY))
                {
                    /* only for directories */
                    if (this_item == &delete_dir_item ||
                        this_item == &set_startdir_item ||
                        this_item == &set_catalogdir_item ||
                        this_item == &set_databasedir_item ||
                        this_item == &set_as_dir_menu
                        )
                        return action;
                }
                else if (this_item == &delete_file_item)
                    return action;
                else if (this_item == &set_backdrop_item)
                {
                    char *suffix = strrchr(selected_file.path, '.');
                    if (suffix)
                    {
                        if (strcasecmp(suffix, ".bmp") == 0)
                        {
                            return action;
                        }
                    }
                }
            }
            return ACTION_EXIT_MENUITEM;
            break;
    }
    return action;
}

static int context_menu_callback(int action,
                               const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list);

/* The configurable tail of the WPS context menu.

   global_settings.context_wps packs HK_CTX_ITEMS hotkey actions into one int.
   Item 0 is the hotkey button itself and is not drawn as a row; items 1..4 are
   the rows below, each showing whichever action the user assigned to it. A row
   set to HOTKEY_OFF hides itself rather than showing as a dead entry. */

static int execute_hotkey(int action);
static int wps_context_item_cb(int, const struct menu_item_ex *,
                               struct gui_synclist *);

static char *wps_context_get_item_name(int selected_item, void *data,
                                       char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)buffer; (void)buffer_len;
    int item = (intptr_t)data;
    const struct hotkey_assignment *hkey =
        get_hotkey(HK_CTX_GET(item, global_settings.context_wps));

    return ID2P(hkey->lang_id);
}

static int wps_context_item_speak_item(int selected_item, void *data)
{
    (void)selected_item;
    int item = (intptr_t)data;
    const struct hotkey_assignment *hkey =
        get_hotkey(HK_CTX_GET(item, global_settings.context_wps));

    talk_id(hkey->lang_id, false);
    return 0;
}

/* Like MENUITEM_RETURNVALUE_DYNTEXT, but the name-and-icon struct is writable
   so each row can take the icon of whatever action is assigned to it. */
#define WPSCTX_RETURNVALUE_DYNTEXT(name, val, cb, text_callback,            \
                                     voice_callback, text_cb_data, icon)    \
     struct menu_get_name_and_icon name##_                                  \
         = {cb,text_callback,voice_callback,text_cb_data,icon};             \
     static const struct menu_item_ex name   =                              \
        { MT_RETURN_VALUE|MENU_DYNAMIC_DESC, { .value = val},               \
        {.menu_get_name_and_icon = & name##_}};

WPSCTX_RETURNVALUE_DYNTEXT(context_item_0, GO_TO_PREVIOUS, wps_context_item_cb,
  wps_context_get_item_name, wps_context_item_speak_item, (void*)0, Icon_NOICON);

WPSCTX_RETURNVALUE_DYNTEXT(context_item_1, GO_TO_PREVIOUS, wps_context_item_cb,
  wps_context_get_item_name, wps_context_item_speak_item, (void*)1, Icon_NOICON);

WPSCTX_RETURNVALUE_DYNTEXT(context_item_2, GO_TO_PREVIOUS, wps_context_item_cb,
  wps_context_get_item_name, wps_context_item_speak_item, (void*)2, Icon_NOICON);

WPSCTX_RETURNVALUE_DYNTEXT(context_item_3, GO_TO_PREVIOUS, wps_context_item_cb,
  wps_context_get_item_name, wps_context_item_speak_item, (void*)3, Icon_NOICON);

WPSCTX_RETURNVALUE_DYNTEXT(context_item_4, GO_TO_PREVIOUS, wps_context_item_cb,
  wps_context_get_item_name, wps_context_item_speak_item, (void*)4, Icon_NOICON);

/* map item number to menu_get_name_and_icon structs so we can change icons */
static struct menu_get_name_and_icon * const ctx_item_map[HK_CTX_ITEMS]=
  {&context_item_0_, &context_item_1_, &context_item_2_, &context_item_3_,
   &context_item_4_};

static int wps_context_item_cb(int action,
                               const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list)
{
    (void)this_list;
    int item = (intptr_t) this_item->menu_get_name_and_icon->list_get_name_data;
    int act = HK_CTX_GET(item, global_settings.context_wps);

    if (action == ACTION_ENTER_MENUITEM || action == ACTION_REQUEST_MENUITEM)
    {
        if (act == HOTKEY_OFF)
            return ACTION_EXIT_MENUITEM;

        ctx_item_map[item]->icon_id = get_hotkey(act)->icon;
    }
    else if (action == ACTION_EXIT_MENUITEM) /* selected */
    {
        if (act == HOTKEY_CONTEXT_MENU)
        {
            context_menu_result = hotkey_run_menu(HOTKEY_FLAG_WPS, true, 0);
        }
        else
        {
            context_menu_result = execute_hotkey(act);
        }
        return ACTION_EXIT_AFTER_THIS_MENUITEM;
    }
    return action;
}

/* used when context_menu_show() is called in the CONTEXT_WPS context */
MAKE_ONPLAYMENU( wps_context_menu, ID2P(LANG_ONPLAY_MENU_TITLE),
           context_menu_callback, Icon_Audio,
           &wps_playlist_menu, &cat_playlist_menu,
           &sound_settings, &playback_settings,
           &rating_item,
           &bookmark_menu,
           &view_cue_item,
           &context_item_1,
           &context_item_2,
           &context_item_3,
           &context_item_4,
         );

int sort_playlists_callback(int action,
                            const struct menu_item_ex *this_item,
                            struct gui_synclist *this_list)
{
    (void) this_list;
    (void) this_item;

    if (action == ACTION_REQUEST_MENUITEM &&
        *browser_get_context()->dirfilter != SHOW_M3U)
    {
        return ACTION_EXIT_MENUITEM;
    }
    return action;
}

MENUITEM_SETTING(sort_playlists, &global_settings.sort_playlists, sort_playlists_callback);

MENUITEM_FUNCTION(view_playlist_item, 0, ID2P(LANG_VIEW),
                  view_playlist,
                  context_menu_callback, Icon_Playlist);

/* used when context_menu_show() is not called in the CONTEXT_WPS context */
MAKE_ONPLAYMENU( browser_context_menu, ID2P(LANG_ONPLAY_MENU_TITLE),
           context_menu_callback, Icon_file_view_menu,
           &view_playlist_item, &browser_playlist_menu, &cat_playlist_menu,
           &rename_file_item, &clipboard_cut_item, &clipboard_copy_item,
           &clipboard_paste_item, &delete_file_item, &delete_dir_item,
           &create_dir_item, &properties_item, &track_info_item,
           &reveal_item,
           &set_backdrop_item,
           &add_to_faves_item, &set_as_dir_menu, &file_menu, &sort_playlists,
         );
static int context_menu_callback(int action,
                               const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list)
{
    (void)this_list;
    switch (action)
    {
        case ACTION_STD_MENU:
            if (this_item == &wps_context_menu)
                return ACTION_STD_CANCEL;
            break;
        case ACTION_TREE_STOP:
            if (this_item == &wps_context_menu)
            {
                list_stop_handler();
                return ACTION_STD_CANCEL;
            }
            break;
        case ACTION_REQUEST_MENUITEM:
            if (this_item == &view_playlist_item)
            {
                if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_M3U &&
                        selected_file.context == CONTEXT_TREE)
                    return action;
            }
            return ACTION_EXIT_MENUITEM;
            break;
        case ACTION_EXIT_MENUITEM:
            return ACTION_EXIT_AFTER_THIS_MENUITEM;
            break;
        default:
            break;
    }
    return action;
}

/* direct function calls, no need for menu callbacks */
static bool hotkey_delete_item(void)
{
    /* no delete for volumes */
    if (selected_file.attr & ATTR_VOLUME)
        return false;

    if (selected_file.context == CONTEXT_ID3DB &&
        (selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO)
        return false;

     if (!prepare_database_sel(NULL))
        return false;

    return clipboard_delete_selected_fileobject();
}


static int hotkey_tree_pl_insert_shuffled(void)
{
    if ((audio_status() & AUDIO_STATUS_PLAY) ||
        (selected_file.attr & ATTR_DIRECTORY) ||
        ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_M3U))
    {
        add_to_playlist(&addtopl_insert_shuf);
    }
    return ONPLAY_RELOAD_DIR;
}

/* See context_menu_properties()'s comment for why this can't share a generic
 * screen-launching helper. */
static int hotkey_properties(void *param)
{
    (void)param;
    if (!prepare_database_sel((void *)"properties"))
        return ONPLAY_RELOAD_DIR;
    if (properties(selected_file.path) == GO_TO_ROOT)
        return ONPLAY_MAINMENU;

    return ONPLAY_RELOAD_DIR;
}

#define HOTKEY_FUNC(func, param) {{(void *)func}, param}

/* HOTKEY_CONTEXT_MENU: rather than run one action, offer all of the ones
   valid where we are. */
static int hotkey_execute_menu(void)
{
    intptr_t flag = HOTKEY_FLAG_WPS;
    if (selected_file.context != CONTEXT_WPS)
        flag = HOTKEY_FLAG_TREE;
    return hotkey_run_menu(flag, true, 0);
}

/* Any desired hotkey functions go here, in the enum in context_menu.h,
   and in the settings menu in settings_list.c.  The order here
   is not important. */
static const struct hotkey_assignment hotkey_items[] = {
 [0]{ .action = HOTKEY_OFF,
      .lang_id = LANG_OFF,
      .func = HOTKEY_FUNC(NULL,NULL),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_TREE,
      .icon = Icon_NOICON },
    { .action = HOTKEY_VIEW_PLAYLIST,
      .lang_id = LANG_VIEW_DYNAMIC_PLAYLIST,
      .func = HOTKEY_FUNC(NULL, NULL),
      .return_code = ONPLAY_PLAYLIST,
      .flags = HOTKEY_FLAG_WPS,
      .icon = Icon_Playlist },
    { .action = HOTKEY_SHOW_TRACK_INFO,
      .lang_id = LANG_MENU_SHOW_ID3_INFO,
      .func = HOTKEY_FUNC(browse_id3_wrapper, NULL),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_WPS,
      .icon = Icon_NOICON },
    { .action = HOTKEY_DELETE,
      .lang_id = LANG_DELETE,
      .func = HOTKEY_FUNC(hotkey_delete_item, NULL),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_TREE,
      .icon = Icon_NOICON },
    { .action = HOTKEY_INSERT,
      .lang_id = LANG_ADD,
      .func = HOTKEY_FUNC(add_to_playlist, (intptr_t*)&addtopl_insert),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_TREE,
      .icon = Icon_Queued },
    { .action = HOTKEY_INSERT_SHUFFLED,
      .lang_id = LANG_ADD_SHUFFLED,
      .func = HOTKEY_FUNC(hotkey_tree_pl_insert_shuffled, NULL),
      .return_code = ONPLAY_FUNC_RETURN,
      .flags = HOTKEY_FLAG_TREE,
      .icon = Icon_Queued },
    { .action = HOTKEY_BOOKMARK,
      .lang_id = LANG_BOOKMARK_MENU_CREATE,
      .func = HOTKEY_FUNC(bookmark_create_menu, NULL),
      .return_code = ONPLAY_OK,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_NOSBS,
      .icon = Icon_Bookmark },
    { .action = HOTKEY_BOOKMARK_LIST,
      .lang_id = LANG_BOOKMARK_MENU_LIST,
      .func = HOTKEY_FUNC(bookmark_load_menu, NULL),
      .return_code = ONPLAY_START_PLAY,
      .flags = HOTKEY_FLAG_WPS,
      .icon = Icon_Bookmark },
    { .action = HOTKEY_PROPERTIES,
      .lang_id = LANG_PROPERTIES,
      .func = HOTKEY_FUNC(hotkey_properties, NULL),
      .return_code = ONPLAY_FUNC_RETURN,
      .flags = HOTKEY_FLAG_TREE,
      .icon = Icon_NOICON },
    { .action = HOTKEY_ALBUMART,
      .lang_id = LANG_VIEW_ALBUMART,
      .func = HOTKEY_FUNC(view_album_art, NULL),
      .return_code = ONPLAY_OK,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_NOSBS,
      .icon = Icon_NOICON },
    { .action = HOTKEY_SHOW_IN_FILES,
      .lang_id = LANG_SHOW_IN_FILES,
      .func = HOTKEY_FUNC(reveal, NULL),
      .return_code = ONPLAY_REVEAL_FILE,
      .flags = HOTKEY_FLAG_WPS,
      .icon = Icon_file_view_menu },
    { .action = HOTKEY_LYRICS,
      .lang_id = LANG_LYRICS,
      .func = HOTKEY_FUNC(view_lyrics, NULL),
      .return_code = ONPLAY_FUNC_RETURN,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_NOSBS,
      .icon = Icon_NOICON },
    { .action = HOTKEY_CONTEXT_MENU,
      .lang_id = LANG_ONPLAY_MENU_TITLE,
      .func = HOTKEY_FUNC(hotkey_execute_menu, NULL),
      .return_code = ONPLAY_FUNC_RETURN,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_TREE,
      .icon = Icon_Submenu },
};

const struct hotkey_assignment *get_hotkey(int action)
{
    for (size_t i = ARRAYLEN(hotkey_items) - 1; i < ARRAYLEN(hotkey_items); i--)
    {
        if (hotkey_items[i].action == action)
            return &hotkey_items[i];
    }
    return &hotkey_items[0]; /* no valid hotkey set, return HOTKEY_OFF*/
}

/* Execute the hotkey function, if listed */
static int execute_hotkey(int action)
{
    /* search assignment struct for a match for the hotkey setting */
    const struct hotkey_assignment *this_item = get_hotkey(action);

    /* run the associated function (with optional param), if any */
    const struct menu_func_param func = this_item->func;

    int func_return = ONPLAY_RELOAD_DIR;
    if (func.function != NULL)
    {
        if (func.param != NULL)
            func_return = (*func.function_w_param)(func.param);
        else
            func_return = (*func.function)();
    }
    const int return_code = this_item->return_code;

    if (return_code == ONPLAY_FUNC_RETURN)
        return func_return;  /* Use value returned by function */
    return return_code;      /* or return the associated value */
}

static const char *hotkey_get_name(int selected_item, void *data,
                                   char *buffer, size_t buffer_len)
{
    (void)buffer; (void)buffer_len;
    const struct hotkey_assignment **hk_menu =
                (const struct hotkey_assignment**)data;
    return ID2P(hk_menu[selected_item]->lang_id);
}

static int hotkey_get_talk(int selected_item, void *data)
{
    const struct hotkey_assignment **hk_menu =
                (const struct hotkey_assignment**)data;
    talk_id(hk_menu[selected_item]->lang_id, false);
    return 0;
}

static enum themable_icons hotkey_get_icon(int selected_item, void *data)
{
    const struct hotkey_assignment **hk_menu =
                (const struct hotkey_assignment**)data;
    return hk_menu[selected_item]->icon;
}

int hotkey_run_menu(intptr_t flag, bool execute, int current_action)
{
    const struct hotkey_assignment *hk_menu[ARRAYLEN(hotkey_items)];

    char *title = str(LANG_ONPLAY_MENU_TITLE);
    if (flag & HOTKEY_FLAG_TREE)
        title = str(LANG_HOTKEY_FILE_BROWSER);

    struct simplelist_info info;
    int selected = 0;
    int count = 0;
    for (size_t i = 0; i < ARRAYLEN(hotkey_items); i++)
    {
        hk_menu[i] = NULL; /*clear all the hk_menu entries prior to setting them */
        if (hotkey_items[i].action == HOTKEY_OFF && execute)
            continue; /* Don't display HOTKEY_OFF item */
        if ((hotkey_items[i].flags & flag) == flag)
        {
            /* the menu cannot offer itself as one of its own entries */
            if (!execute || hotkey_items[i].action != HOTKEY_CONTEXT_MENU)
            {
                if (hotkey_items[i].action == current_action)
                    selected = count;
                hk_menu[count++] = &hotkey_items[i];
            }
        }
    }

    simplelist_info_init(&info, title, count, (void*)&hk_menu);
    info.get_name = hotkey_get_name;
    info.get_icon = hotkey_get_icon;
    info.get_talk = hotkey_get_talk;
    info.selection = selected;
    simplelist_show_list(&info);

    if (execute)
    {
        if (info.selection < 0) /* canceled */
            return ONPLAY_RELOAD_DIR;
        return execute_hotkey(hk_menu[info.selection]->action);
    }
    else
    {
        if (info.selection < 0) /* canceled */
            return -1;
        return hk_menu[info.selection]->action;
    }
}

/* Assign one packed item from the settings screens. Assigning an action that
   is already on another item clears the other one, so an action cannot appear
   twice in the same menu. The hotkey (item 0) is exempt -- it is a button, not
   a row, so it may duplicate one. */
static int hotkey_menu_do_setting(void *param, int *setting, int flag)
{
    int current = *setting;
    int item = (intptr_t)param;

    int temp = HK_CTX_GET(item, current);
    int sel = hotkey_run_menu(flag, false, temp);
    if (sel >= 0)
    {
        current &= ~HK_CTX_SET(item, HK_CTX_MASK);/*clear*/
        current |= HK_CTX_SET(item, sel);

        /* check for duplicates */
        if (item > 0)
        {
            for (int i = 1; i < HK_CTX_ITEMS; i++)
            {
                if (i != item && HK_CTX_GET(i, *setting) == sel)
                    current &= ~HK_CTX_SET(i, HK_CTX_MASK);/*clear*/
            }
        }
        *setting = current;
    }
    return sel;
}

int wps_context_menu_do_setting(void *param)
{
    return hotkey_menu_do_setting(param, &global_settings.context_wps,
                                  HOTKEY_FLAG_WPS);
}

int tree_context_menu_do_setting(void *param)
{
    return hotkey_menu_do_setting(param, &global_settings.hotkey_tree,
                                  HOTKEY_FLAG_TREE);
}

/* config.cfg stores each item by its english name -- see lang_id_to_english().
   Storing the numeric action instead would break as soon as the enum moved,
   and storing the translated name breaks on a language change. */
void wps_context_menu_load_from_cfg(void *setting, char *value)
{
    int item = 0;
    int var = 0;
    char *st = value;
    char *end = value;
    while (*end != '\0' && item < HK_CTX_ITEMS)
    {
        end++;
        if (*end == ',' || *end == '\0')
        {
            st = skip_whitespace(st);
            if (end - st > 1)
            {
                for (size_t i = ARRAYLEN(hotkey_items) - 1;
                     i < ARRAYLEN(hotkey_items); i--)
                {
                    const char *this = lang_id_to_english(hotkey_items[i].lang_id);
                    if (strncasecmp(st, this, end - st) == 0)
                    {
                        var |= HK_CTX_SET(item, hotkey_items[i].action);
                    }
                }
            }
            st = end + 1;
            item++;
        }
    }
    *(int*)setting = var;
}

char *wps_context_menu_write_to_cfg(void *setting, char *buf, int buf_len)
{
    int var = *(int*)setting;
    /* the file browser hotkey is a button, not a menu -- only item 0 is used */
    int items = (setting == &global_settings.hotkey_tree) ? 1 : HK_CTX_ITEMS;

    unsigned int written;
    char *buffer = buf;
    for (int i = 0; i < items && buf_len > 0; i++)
    {
        written = snprintf(buffer, buf_len, "%s, ",
                    lang_id_to_english(get_hotkey(HK_CTX_GET(i, var))->lang_id));
        buf_len -= written;
        buffer += written;
    }
    return buf;
}

void wps_context_menu_set_default(void *setting, void *defaultval)
{
    *(int*)setting = *(int*)defaultval;
}

bool wps_context_menu_is_changed(void *setting, void *defaultval)
{
    return *(int*)setting != *(int*)defaultval;
}

int context_menu_show(char* file, int attr, int from_context, bool hotkey, int customaction)
{
    const struct menu_item_ex *menu;
    context_menu_result = ONPLAY_OK;
    ctx_current_playlist_insert = NULL;
    selected_file_set(from_context, NULL, attr);

    if (from_context == CONTEXT_ID3DB)
    {
        ctx_add_to_playlist = browser_db_add_to_playlist;
        if (file != NULL)
        {
            /* add a leading slash so that catalog_add_to_a_playlist
               later prefills the name when creating a new playlist */
            snprintf(selected_file.buf, MAX_PATH, "/%s", file);
            selected_file.path = selected_file.buf;
        }
    }
   else
    {
        ctx_add_to_playlist = NULL;
        if (file != NULL)
        {
            strmemccpy(selected_file.buf, file, MAX_PATH);
            selected_file.path = selected_file.buf;
        }

    }
    int menu_selection;

    if (hotkey)
    {
        if (from_context == CONTEXT_WPS)
        {
            /* item 0 of the packed setting is the WPS hotkey; run it through
               the same callback the menu rows use so HOTKEY_CONTEXT_MENU
               behaves identically either way */
            wps_context_item_cb(ACTION_EXIT_MENUITEM, &context_item_0, NULL);
            return context_menu_result;
        }
        return execute_hotkey(global_settings.hotkey_tree & HK_CTX_MASK);
    }
    if (customaction == ONPLAY_CUSTOMACTION_SHUFFLE_SONGS)
    {
        int returnCode = add_to_playlist(&addtopl_replace_shuffled);
        if (returnCode == 1)
            /* User did not want to erase his current playlist, so let's show again the database main menu */
            return ONPLAY_RELOAD_DIR;
        return ONPLAY_START_PLAY;
    }

    push_current_activity(ACTIVITY_CONTEXTMENU);
    if (from_context == CONTEXT_WPS)
        menu = &wps_context_menu;
    else
        menu = &browser_context_menu;
    menu_selection = do_menu(menu, NULL, NULL, false);

    if (get_current_activity() == ACTIVITY_CONTEXTMENU) /* Activity may have been      */
        pop_current_activity();                         /* popped already by menu item */


    if (menu_selection == GO_TO_WPS)
        return ONPLAY_START_PLAY;
    if (menu_selection == GO_TO_ROOT)
        return ONPLAY_MAINMENU;
    if (menu_selection == GO_TO_MAINMENU)
        return ONPLAY_MAINMENU;
    if (menu_selection == GO_TO_PLAYLIST_VIEWER)
        return ONPLAY_PLAYLIST;
    if (menu_selection == GO_TO_PLUGIN)
        return ONPLAY_PLUGIN;

    return context_menu_result;
}

int context_menu_get_source(void)
{
    return selected_file.context;
}
