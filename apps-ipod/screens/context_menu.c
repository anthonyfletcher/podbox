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
#include "screens/covers/album_covers.h"
#include "metadata/cuesheet.h"
#include "skin/statusbar_skinned.h"
#include "screens/playback/pitch_screen.h"
#include "draw/viewport.h"
#include "pathfuncs.h"
#include "shortcuts.h"
#include "system/activity.h"
#include "system/app_util.h"
#include "system/shutdown.h"
#include "storage.h"
#include "sound.h"
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

/* CONTEXT_WPS items */
MENUITEM_FUNCTION(browse_id3_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_MENU_SHOW_ID3_INFO),
                  browse_id3_wrapper, NULL, Icon_NOICON);

MENUITEM_FUNCTION(pitch_screen_item, 0, ID2P(LANG_PITCH),
                  gui_syncpitchscreen_run, NULL, Icon_Audio);
MENUITEM_FUNCTION(pitch_reset_item, 0, ID2P(LANG_RESET_SETTING),
                  reset_pitch, NULL, Icon_Submenu_Entered);

static int pitch_callback(int action,
                          const struct menu_item_ex *this_item,
                          struct gui_synclist *this_list);

/* need special handling so we can toggle the icon */
#define MAKE_PITCHMENU( name, str, callback, icon, ... )           \
    static const struct menu_item_ex *name##_[]  = {__VA_ARGS__};  \
    struct menu_callback_with_desc name##__ = {callback,str,icon}; \
    static const struct menu_item_ex name =                        \
        {MT_MENU|MENU_HAS_DESC|MENU_EXITAFTERTHISMENU|             \
         MENU_ITEM_COUNT(sizeof( name##_)/sizeof(*name##_)),       \
            { (void*)name##_},{.callback_and_desc = & name##__}};

MAKE_PITCHMENU(pitch_menu, ID2P(LANG_PITCH),
                pitch_callback, Icon_Audio,
                &pitch_screen_item,
                &pitch_reset_item);

static int pitch_callback(int action,
                          const struct menu_item_ex *this_item,
                          struct gui_synclist *this_list)
{
    if (action == ACTION_ENTER_MENUITEM || action == ACTION_REQUEST_MENUITEM)
    {
        pitch_menu__.icon_id = Icon_Submenu; /* if setting changed show + */
        int32_t ts = dsp_get_timestretch();
        if (sound_get_pitch() == PITCH_SPEED_100 && ts == PITCH_SPEED_100)
        {
            pitch_menu__.icon_id = Icon_Audio;
            if (action == ACTION_ENTER_MENUITEM)
            { /* if default then run pitch screen directly */
                gui_syncpitchscreen_run();
                action = ACTION_EXIT_MENUITEM;
            }
        }
    }
    return action;

    (void)this_item;
    (void)this_list;
}

MENUITEM_FUNCTION(view_album_art_item, 0, ID2P(LANG_VIEW_ALBUMART),
                  view_album_art, NULL, Icon_NOICON);

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

/* Properties is core-linked, not a loadable plugin, so it can't go through
 * filetype_load_plugin() like its siblings. Instead: prepare_database_sel() to
 * resolve selected_file.path in a database browse context, and the
 * context_menu_result side-channel to report properties()'s GO_TO_* result,
 * since do_menu() ignores this function's own return value without
 * MENU_FUNC_CHECK_RETVAL. */
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
                if (this_item == &track_info_item)
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

/* used when context_menu_show() is called in the CONTEXT_WPS context */
MAKE_ONPLAYMENU( wps_context_menu, ID2P(LANG_ONPLAY_MENU_TITLE),
           context_menu_callback, Icon_Audio,
           &wps_playlist_menu, &cat_playlist_menu,
           &sound_settings, &playback_settings,
           &rating_item,
           &bookmark_menu,
           &browse_id3_item,
           &delete_file_item, &view_cue_item,
           &pitch_menu,
           &view_album_art_item,
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

/* Properties equivalent of hotkey_album_covers() below -- see
 * context_menu_properties()'s comment for why this can't share a generic
 * plugin-loading helper. */
static int hotkey_properties(void *param)
{
    (void)param;
    if (!prepare_database_sel((void *)"properties"))
        return ONPLAY_RELOAD_DIR;
    if (properties(selected_file.path) == GO_TO_ROOT)
        return ONPLAY_MAINMENU;

    return ONPLAY_RELOAD_DIR;
}

/* Album covers equivalent of hotkey_properties() above -- see
 * context_menu_properties()'s comment for why this can't share a generic
 * plugin-loading helper. */
static int hotkey_album_covers(void *param)
{
    (void)param;
    if (!prepare_database_sel(NULL))
        return ONPLAY_RELOAD_DIR;
    if (album_covers(selected_file.path) == GO_TO_WPS)
        return ONPLAY_START_PLAY;

    return ONPLAY_RELOAD_DIR;
}

#define HOTKEY_FUNC(func, param) {{(void *)func}, param}

/* Any desired hotkey functions go here, in the enum in context_menu_show.h,
   and in the settings menu in settings_list.c.  The order here
   is not important. */
static const struct hotkey_assignment hotkey_items[] = {
 [0]{ .action = HOTKEY_OFF,
      .lang_id = LANG_OFF,
      .func = HOTKEY_FUNC(NULL,NULL),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_TREE },
    { .action = HOTKEY_VIEW_PLAYLIST,
      .lang_id = LANG_VIEW_DYNAMIC_PLAYLIST,
      .func = HOTKEY_FUNC(NULL, NULL),
      .return_code = ONPLAY_PLAYLIST,
      .flags = HOTKEY_FLAG_WPS },
    { .action = HOTKEY_SHOW_TRACK_INFO,
      .lang_id = LANG_MENU_SHOW_ID3_INFO,
      .func = HOTKEY_FUNC(browse_id3_wrapper, NULL),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_WPS },
    { .action = HOTKEY_PITCHSCREEN,
      .lang_id = LANG_PITCH,
      .func = HOTKEY_FUNC(gui_syncpitchscreen_run, NULL),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_NOSBS },
    { .action = HOTKEY_DELETE,
      .lang_id = LANG_DELETE,
      .func = HOTKEY_FUNC(hotkey_delete_item, NULL),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_TREE },
    { .action = HOTKEY_INSERT,
      .lang_id = LANG_ADD,
      .func = HOTKEY_FUNC(add_to_playlist, (intptr_t*)&addtopl_insert),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_TREE },
    { .action = HOTKEY_INSERT_SHUFFLED,
      .lang_id = LANG_ADD_SHUFFLED,
      .func = HOTKEY_FUNC(hotkey_tree_pl_insert_shuffled, NULL),
      .return_code = ONPLAY_FUNC_RETURN,
      .flags = HOTKEY_FLAG_TREE },
    { .action = HOTKEY_BOOKMARK,
      .lang_id = LANG_BOOKMARK_MENU_CREATE,
      .func = HOTKEY_FUNC(bookmark_create_menu, NULL),
      .return_code = ONPLAY_OK,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_NOSBS },
    { .action = HOTKEY_BOOKMARK_LIST,
      .lang_id = LANG_BOOKMARK_MENU_LIST,
      .func = HOTKEY_FUNC(bookmark_load_menu, NULL),
      .return_code = ONPLAY_START_PLAY,
      .flags = HOTKEY_FLAG_WPS },
    { .action = HOTKEY_PROPERTIES,
      .lang_id = LANG_PROPERTIES,
      .func = HOTKEY_FUNC(hotkey_properties, NULL),
      .return_code = ONPLAY_FUNC_RETURN,
      .flags = HOTKEY_FLAG_TREE },
    { .action = HOTKEY_PICTUREFLOW,
      .lang_id = LANG_ONPLAY_PICTUREFLOW,
      .func = HOTKEY_FUNC(hotkey_album_covers, NULL),
      .return_code = ONPLAY_FUNC_RETURN,
      .flags = HOTKEY_FLAG_TREE },
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
static int execute_hotkey(bool is_wps)
{
    const int action = (is_wps ? global_settings.hotkey_wps :
                                 global_settings.hotkey_tree);

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
        return execute_hotkey(from_context == CONTEXT_WPS);
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
