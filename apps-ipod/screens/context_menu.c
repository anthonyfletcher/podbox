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
 *   - Now Playing's own rows, and the sound-engine rows
 *   - playlist operations: play, play next, later and last, play once, add to
 *     a named or new playlist
 *   - bookmark, playback and rating items with their visibility callbacks
 *   - file operations: cut, copy, paste, delete, rename, properties
 *   - Show in Files, and Go to Album and Go to Artist
 *   - Now Playing's rows for the playing file
 *   - the menu declarations, More and Manage among them
 *   - the hotkey action table, the picker built from it, and the config.cfg
 *     handlers for the two hotkey settings
 *   - the menu a screen shows for one of its own items, with that screen's
 *     rows in slots of its own
 *   - context_menu_show(), which runs the browser's and Now Playing's
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
#include "games/spike/spike.h"
#include "database/sound_mix.h"
#include "database/sound_index.h"
#include "screens/playback/sound_props.h"
#include "screens/playback/track_info.h"
#include "widgets/text_box.h"
#include "screens/browse/browser.h"
#include "powermgmt.h"
#include "settings/settings.h"
#include "settings/settings_list.h"
#include "widgets/rating_dialog.h"
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
#include "screens/browse/book_shelf.h"
#include "metadata/book_resume.h"
#include "screens/browse/listen_progress.h"
#include "database/tagcache.h"
#include "database/db_spoken.h"
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
#include "system/format_time.h"
#include "system/shutdown.h"
#include "storage.h"
#include "string-extra.h"
#include "dir.h"

static int context_menu_result = ONPLAY_OK;

/* Whether whoever opened the menu can leave for the database browser, which
 * Go to Album and Go to Artist leave for */
static bool ctx_go_to;

/* Rows hidden from each kind of menu: see hiding_begin() */
static bool row_hidden(const struct menu_item_ex *row);
static void hiding_begin(void);
static void hiding_end(void);

static bool (*ctx_current_playlist_insert)(int position, bool queue, bool create_new);
static int (*ctx_add_to_playlist)(const char* playlist, bool new_playlist);

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

    if (playlist_viewer() == PLAYLIST_VIEWER_GO_TO_LIBRARY)
        context_menu_result = ONPLAY_GO_TO_LIBRARY;

    return 0;
}

static void playing_time(void)
{
    if (playing_time_screen())
        context_menu_result = ONPLAY_MAINMENU;
}

static void view_album_art(void)
{
    if (image_viewer(NULL) == GO_TO_ROOT)
        context_menu_result = ONPLAY_MAINMENU;
}

/* Returns its own result rather than a fixed one, so that leaving the lyrics
 * because USB was plugged in lands at the root and not back in the WPS. */
static int view_lyrics(void)
{
    if (lyric_viewer() == GO_TO_ROOT)
        return ONPLAY_MAINMENU;
    return ONPLAY_OK;
}

static int lyrics_run(void)
{
    if (lyric_viewer() == GO_TO_ROOT)
        context_menu_result = ONPLAY_MAINMENU;
    return 0;
}

MENUITEM_FUNCTION(lyrics_item, 0, ID2P(LANG_LYRICS),
                  lyrics_run, NULL, Icon_NOICON);

MENUITEM_FUNCTION(wps_view_cur_playlist_item, 0, ID2P(LANG_VIEW_DYNAMIC_PLAYLIST),
                  wps_view_cur_playlist, NULL, Icon_NOICON);
MENUITEM_FUNCTION(search_playlist_item, 0, ID2P(LANG_SEARCH_IN_PLAYLIST),
                  search_playlist, NULL, Icon_NOICON);
MENUITEM_FUNCTION(playlist_save_item, 0, ID2P(LANG_SAVE_DYNAMIC_PLAYLIST),
                  save_playlist, NULL, Icon_NOICON);
MENUITEM_FUNCTION(reshuffle_item, 0, ID2P(LANG_SHUFFLE_PLAYLIST),
                  shuffle_playlist, NULL, Icon_NOICON);
MENUITEM_FUNCTION(playing_time_item, 0, ID2P(LANG_PLAYING_TIME),
                  playing_time, NULL, Icon_NOICON);

/* The game takes the playlist as it finds it, so it belongs on the WPS and
 * nowhere else: what it is played against is what is already playing. */
static int spike_run(void)
{
    if (spike_screen())
        context_menu_result = ONPLAY_MAINMENU;
    return ONPLAY_OK;
}

MENUITEM_FUNCTION(spike_run_item, 0, ID2P(LANG_SPIKE_PLAY),
                  spike_run, NULL, Icon_NOICON);

/* Shuffle and Repeat as the Playback settings have them, at the top of
 * Now Playing's menu where they are wanted most */
MENUITEM_SETTING(wps_shuffle_item, &global_settings.playlist_shuffle, NULL);
MENUITEM_SETTING(wps_repeat_item, &global_settings.repeat_mode, NULL);

/* Whether a mix could be built from what is selected. Deliberately stops
 * short of looking the track up: a database row carries no path, and reading
 * one spins the disk -- which this must not do, because it runs every time the
 * menu is drawn. */
static bool sound_album_selectable(void);

static bool mix_seed_available(void)
{
    struct mp3entry *id3;

    if (selected_file.context == CONTEXT_WPS)
    {
        id3 = audio_current_track();
        return id3 != NULL && id3->path[0] != '\0';
    }

    /* An album's mean is a goal like any other -- see sound_mix_from_album().
     * Read on the same terms the read-out reads it, artist rows included:
     * sound_album_selectable() says why they are not offered. */
    if (selected_file.attr & ATTR_DIRECTORY)
        return sound_album_selectable();

    if ((selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO)
        return false;

    return selected_file.context == CONTEXT_ID3DB ||
           selected_file.path != NULL;
}

/* The track a mix is built around, copied into the caller's buffer. Copied
 * rather than pointed at: the mix creates a playlist, and the playing track's
 * id3 is the live entry rather than a copy of one. */
static const char *mix_seed_path(char *buf, size_t bufsize)
{
    struct mp3entry *id3;

    if (selected_file.context == CONTEXT_WPS)
    {
        id3 = audio_current_track();
        if (id3 == NULL || id3->path[0] == '\0')
            return NULL;

        strmemccpy(buf, id3->path, bufsize);
        return buf;
    }

    if ((selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO)
        return NULL;

    /* A database row names an entry, not a file. The filename behind it is
     * read here rather than in the callback above, so the disk wakes only for
     * somebody who has actually chosen this. */
    if (selected_file.context == CONTEXT_ID3DB)
    {
        return browser_db_get_filename(browser_get_context(), buf,
                                       (int)bufsize) < 0 ? NULL : buf;
    }

    if (selected_file.path == NULL)
        return NULL;

    strmemccpy(buf, selected_file.path, bufsize);

    return buf;
}

static int sound_mix_report(int added);

/* Whether the selection is an album whose tracks can be read.
 *
 * In the database, album rows only. browser_db_subentries_do_action() reaches
 * a row's tracks by descending to them, and it enumerates the one level it
 * lands on -- which from an artist row is that artist's first album, reported
 * as though it were the artist. A genre row is wrong the same way. Both would
 * answer confidently about music they had not read. */
static bool sound_album_selectable(void)
{
    if (!(selected_file.attr & ATTR_DIRECTORY))
        return false;

    if (selected_file.context == CONTEXT_ID3DB)
        return browser_db_is_album_list(browser_get_context());

    return selected_file.path != NULL;
}

/* Every track of the selected album, handed to the read-out's accumulator.
 *
 * True carries the enumeration on. There is no failure this can have -- a
 * track the index holds nothing for is simply not counted, and one whose
 * name cannot be read arrives as NULL and is ignored -- and returning false
 * would abandon the rest of the album over one of them. */
static bool sound_album_add_cb(const char *file_name)
{
    sound_props_album_add(file_name);
    return true;
}

/* begin() and an add() per track, from whichever browser the selection came
 * from, leaving the run open for the finishing calls. */
static void sound_album_collect(void)
{
    if (selected_file.context == CONTEXT_ID3DB)
    {
        /* Static, as properties.c's are: this sits under Play Similar's own
         * frame, and browser_db_subentries_do_action() is deep enough below
         * it already. The buffer is the caller's because that function sits
         * underneath splash_progress() and the callback. */
        static struct tagcache_search tcs;
        static char buf[MAX_PATH];

        sound_props_album_begin();
        browser_db_subentries_do_action(&tcs, sound_album_add_cb,
                                        buf, sizeof (buf));
    }
    else
        sound_props_album_walk(selected_file.path);
}

/* Gather the album the selection names and hand back its mean. The walk is
 * the read-out's; this only takes the answer out rather than showing it. */
static bool sound_album_gather(struct sound_axes *mean, char *one, size_t len)
{
    sound_album_collect();

    return sound_props_album_result(mean, one, len);
}

static int sound_mix_run(void)
{
    char seed[MAX_PATH];
    int added;

    splash(0, ID2P(LANG_WAIT));

    if (selected_file.attr & ATTR_DIRECTORY)
    {
        struct sound_axes mean;

        if (!sound_album_gather(&mean, seed, sizeof (seed)))
        {
            splash(HZ * 2, ID2P(LANG_SOUND_ALBUM_NONE));
            return ONPLAY_OK;
        }

        added = sound_mix_from_album(&mean, seed,
                                     global_settings.mix_length);
        return sound_mix_report(added);
    }

    if (mix_seed_path(seed, sizeof (seed)) == NULL)
    {
        splash(HZ * 2, ID2P(LANG_SOUND_MIX_NO_TRACK));
        return ONPLAY_OK;
    }

    added = sound_mix_from_track(seed, global_settings.mix_length);

    return sound_mix_report(added);
}

/* What came of a build, said once for all three seeds.
 *
 * Each of these sends the reader somewhere different -- to the analysis
 * screen, to a wider library, or to nothing they can act on -- so they do not
 * collapse into one message. */
static int sound_mix_report(int added)
{
    if (added > 0)
    {
        context_menu_result = ONPLAY_START_PLAY;
        return ONPLAY_OK;
    }

    switch (added)
    {
        case SOUND_MIX_NO_INDEX:
            splash(HZ * 2, ID2P(LANG_SOUND_MIX_NO_INDEX));
            break;
        case SOUND_MIX_NO_RECORD:
            splash(HZ * 2, ID2P(LANG_SOUND_MIX_NO_RECORD));
            break;
        case SOUND_MIX_NO_DB:
            splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
            break;
        case SOUND_MIX_NO_PLAYLIST:
            splash(HZ * 2, ID2P(LANG_SOUND_MIX_FAILED));
            break;
        case SOUND_MIX_CANCELLED:
            break;
        default:
            splash(HZ, ID2P(LANG_SOUND_MIX_NONE));
            break;
    }

    return ONPLAY_OK;
}

/* A row of the Audiobooks browse, or a book in any other: the sound items,
 * which build music, are not offered for it, and Mark as is. */
static bool in_book_list(void)
{
    struct browser_context *c = browser_get_context();
    long album_seek, artist_seek;

    return selected_file.context == CONTEXT_ID3DB
           && (browser_db_is_spoken_list(c)
               || browser_db_get_book_album(c, c->selected_item, &album_seek,
                                            &artist_seek));
}

/* The menu is for a book, which is heard in order: the shuffled rows are left
 * out, and the rest sit under More behind the book's own rows */
static bool ctx_in_order;

/* Hidden rather than shown failing. The setting comes first: somebody who has
 * turned the engine off has said they do not want this, and an index left on
 * disk from before is not consent to keep offering it. */
static int sound_mix_callback(int action,
                              const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;

    if (action == ACTION_REQUEST_MENUITEM)
    {
        if (in_book_list() ||
            !global_settings.playlist_engine || !mix_seed_available() ||
            !tagcache_is_usable() || !sound_index_exists())
            return ACTION_EXIT_MENUITEM;
    }

    return action;
}

MENUITEM_FUNCTION(sound_mix_item, 0, ID2P(LANG_SOUND_MIX),
                  sound_mix_run, sound_mix_callback, Icon_Audio);

/* How long a wind-down runs, in minutes: 0 is Default, first. */
static const int winddown_minutes[] = { 0, 15, 30, 45, 60, 90, 120 };

static const char *winddown_choice_name(int selected_item, void *data,
                                        char *buffer, size_t buffer_len)
{
    (void)data;

    if (winddown_minutes[selected_item] == 0)
        return str(LANG_WINDDOWN_DEFAULT);
    return format_sleeptimer(buffer, buffer_len,
                             winddown_minutes[selected_item], NULL);
}

/* A wind-down is seeded, which is why it is here beside Play Similar and not
 * on the Playlists screen with the Moods and Journeys. Those pick both ends
 * for you and start from nothing; this one starts from the track chosen, so
 * it wants a track to have been chosen. */
static int winddown_run(void)
{
    char seed[MAX_PATH];
    struct simplelist_info info;
    long target_ms = 0;

    simplelist_info_init(&info, str(LANG_WINDDOWN),
                         ARRAYLEN(winddown_minutes), NULL);
    info.get_name = winddown_choice_name;
    if (simplelist_show_list(&info))
        return ONPLAY_MAINMENU;
    if (info.selection < 0)
        return ONPLAY_OK;

    /* Default fills to the time left on a running sleep timer, which
     * Playlist Length does not cap: a wind-down that stops before the player
     * does has not wound anything down. With no timer it is Playlist Length's
     * count, since there is nothing counting down to fit. */
    if (winddown_minutes[info.selection] > 0)
        target_ms = winddown_minutes[info.selection] * 60 * 1000L;
    else if (get_sleep_timer() > 0)
        target_ms = get_sleep_timer() * 1000L;

    splash(0, ID2P(LANG_WAIT));

    if (mix_seed_path(seed, sizeof (seed)) == NULL)
    {
        splash(HZ * 2, ID2P(LANG_SOUND_MIX_NO_TRACK));
        return ONPLAY_OK;
    }

    return sound_mix_report(sound_mix_winddown(seed, global_settings.mix_length,
                                               target_ms));
}

/* Tracks only. An album's mean is a point rather than a track, and the near
 * end of a journey has to be a mood -- see sound_mix_winddown(). */
static int winddown_callback(int action,
                             const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;

    if (action == ACTION_REQUEST_MENUITEM)
    {
        if (in_book_list() ||
            !global_settings.playlist_engine || !sound_index_exists() ||
            !tagcache_is_usable() ||
            (selected_file.attr & ATTR_DIRECTORY) || !mix_seed_available())
            return ACTION_EXIT_MENUITEM;
    }

    return action;
}

MENUITEM_FUNCTION(winddown_item, 0, ID2P(LANG_WINDDOWN),
                  winddown_run, winddown_callback, Icon_Audio);

/* Sleep Timer from the now-playing screen: a length to start it with, Sleep
 * Timer Duration's own preselected, and Cancel first while one is running.
 * Choosing a length starts the timer and leaves the setting as it was. */
#define SLEEP_CHOICES_MAX 8     /* Cancel, the lengths, and the setting's */

static int sleep_minutes[SLEEP_CHOICES_MAX];    /* 0 is Cancel */

static const char *sleep_choice_name(int selected_item, void *data,
                                     char *buffer, size_t buffer_len)
{
    (void)data;

    if (sleep_minutes[selected_item] == 0)
        return str(LANG_SLEEP_TIMER_CANCEL_CURRENT);
    return format_sleeptimer(buffer, buffer_len, sleep_minutes[selected_item],
                             NULL);
}

static int sleep_timer_run(void)
{
    static const int lengths[] = { 15, 30, 45, 60, 90, 120 };
    int preferred = global_settings.sleeptimer_duration;
    struct simplelist_info info;
    bool placed = false;
    int count = 0, choice = 0;

    if (get_sleep_timer() > 0)
        sleep_minutes[count++] = 0;

    for (size_t i = 0; i < ARRAYLEN(lengths); i++)
    {
        if (!placed && preferred <= lengths[i])
        {
            choice = count;
            if (preferred < lengths[i])
                sleep_minutes[count++] = preferred;
            placed = true;
        }
        sleep_minutes[count++] = lengths[i];
    }
    if (!placed)
    {
        choice = count;
        sleep_minutes[count++] = preferred;
    }

    simplelist_info_init(&info, str(LANG_SLEEP_TIMER), count, NULL);
    info.get_name = sleep_choice_name;
    info.selection = choice;
    if (simplelist_show_list(&info))
        return ONPLAY_MAINMENU;

    if (info.selection >= 0)
        set_sleeptimer_duration(sleep_minutes[info.selection]);
    return ONPLAY_OK;
}

/* "Sleep Timer", or with one running, the time it has left. */
static char *sleep_timer_name(int selected_item, void *data,
                              char *buffer, size_t buffer_len)
{
    int left = get_sleep_timer();
    char time[10];

    (void)selected_item;
    (void)data;

    if (left <= 0)
        return (char *)str(LANG_SLEEP_TIMER);
    snprintf(buffer, buffer_len, "%s (%s)", str(LANG_SLEEP_TIMER),
             format_sleeptimer(time, sizeof(time), (left + 59) / 60, NULL));
    return buffer;
}

/* MENU_FUNC_CHECK_RETVAL, or MENU in the list stops at this row */
MENUITEM_FUNCTION_DYNTEXT(sleep_timer_item, MENU_FUNC_CHECK_RETVAL,
                          sleep_timer_run, sleep_timer_name, NULL, NULL, NULL,
                          Icon_NOICON);

/* The read-out over a set of tracks: a folder in the file browser, a row's
 * subentries in the database. The gathering is sound_album_gather()'s, which
 * Play Similar uses too; this one shows the answer rather than aiming at it. */
static int sound_album_run(void)
{
    splash(0, ID2P(LANG_WAIT));

    sound_album_collect();

    if (!sound_props_album_ready())
    {
        splash(HZ * 2, ID2P(LANG_SOUND_ALBUM_NONE));
        return ONPLAY_OK;
    }

    /* The read-out can be left for the root menu, and this menu goes with it
     * rather than redrawing underneath. */
    return sound_props_album_screen() ? ONPLAY_MAINMENU : ONPLAY_OK;
}

/* Trap: ATTR_DIRECTORY does not mean "a folder on disk". The database browser
 * sets it on every row that is not a track -- artist, album and genre alike
 * (browser_db.c:4909) -- and under CONTEXT_ID3DB the path beside it is that
 * row's display name with a slash put in front, which opendir() cannot open.
 * So the two contexts are told apart and read their tracks differently,
 * rather than one path being made to serve both. */
static int sound_album_callback(int action,
                                const struct menu_item_ex *this_item,
                                struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;

    if (action == ACTION_REQUEST_MENUITEM)
    {
        if (in_book_list() ||
            !global_settings.playlist_engine || !sound_index_exists() ||
            !sound_album_selectable())
            return ACTION_EXIT_MENUITEM;
    }

    return action;
}

/* MENU_FUNC_CHECK_RETVAL, or do_menu() discards what the row returns and the
 * read-out being left for the root menu goes no further than this row. */
MENUITEM_FUNCTION(sound_album_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_SOUND_ALBUM),
                  sound_album_run, sound_album_callback, Icon_Audio);
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

    /* With the playing track kept, what was put in around it is the music
     * now, not a queue: the next Add to Queue goes straight after that track. */
    if ((param->flags & PL_REPLACE) == PL_REPLACE && !new_playlist)
        playlist_forget_inserts();

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
           inserted; a book plays in order whatever Shuffle says */
        if (global_settings.playlist_shuffle && !ctx_in_order)
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

/* These rows are actions, so they take the generic function-call icon rather
 * than naming one: Icon_Playlist on all of them filled whole menus with a
 * single glyph that distinguished nothing. The Play Once rows are the one
 * exception -- Icon_Queued is what the playlist viewer already draws on a
 * queued track, so it says something the row text does not have to. */
/* insert items */
MENUITEM_FUNCTION_W_PARAM(i_pl_item, 0, ID2P(LANG_ADD),
                  add_to_playlist, &addtopl_insert,
                  treeplaylist_callback, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(i_first_pl_item, 0, ID2P(LANG_PLAY_NEXT),
                  add_to_playlist, &addtopl_insert_first,
                  treeplaylist_callback, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(i_last_pl_item, 0, ID2P(LANG_PLAY_LAST),
                  add_to_playlist, &addtopl_insert_last,
                  treeplaylist_callback, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(i_shuf_pl_item, 0, ID2P(LANG_ADD_SHUFFLED),
                  add_to_playlist, &addtopl_insert_shuf,
                  treeplaylist_callback, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(i_last_shuf_pl_item, 0, ID2P(LANG_PLAY_LAST_SHUFFLED),
                  add_to_playlist, &addtopl_insert_last_shuf,
                  treeplaylist_callback, Icon_NOICON);
/* queue items */
MENUITEM_FUNCTION_W_PARAM(q_pl_item, 0, ID2P(LANG_QUEUE),
                  add_to_playlist, &addtopl_queue,
                  treeplaylist_callback, Icon_Queued);
MENUITEM_FUNCTION_W_PARAM(q_first_pl_item, 0, ID2P(LANG_QUEUE_FIRST),
                  add_to_playlist, &addtopl_queue_first,
                  treeplaylist_callback, Icon_Queued);
MENUITEM_FUNCTION_W_PARAM(q_last_pl_item, 0, ID2P(LANG_QUEUE_LAST),
                  add_to_playlist, &addtopl_queue_last,
                  treeplaylist_callback, Icon_Queued);

/* Play Once submenu */
MAKE_ONPLAYMENU(queue_menu, ID2P(LANG_QUEUE_MENU),
                treeplaylist_callback, Icon_Playlist,
                &q_first_pl_item,
                &q_pl_item,
                &q_last_pl_item);

/* replace playlist */
MENUITEM_FUNCTION_W_PARAM(replace_pl_item, 0, ID2P(LANG_PLAY),
                  add_to_playlist, &addtopl_replace,
                  treeplaylist_callback, Icon_NOICON);

MENUITEM_FUNCTION_W_PARAM(replace_shuf_pl_item, 0, ID2P(LANG_SHUFFLE),
                  add_to_playlist, &addtopl_replace_shuffled,
                  treeplaylist_callback, Icon_NOICON);

/* The same two while something is playing, where they are rarer: shown in
 * More rather than above the rows that add to the queue */
MENUITEM_FUNCTION_W_PARAM(replace_more_pl_item, 0, ID2P(LANG_PLAY),
                  add_to_playlist, &addtopl_replace,
                  treeplaylist_callback, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(replace_shuf_more_pl_item, 0, ID2P(LANG_SHUFFLE),
                  add_to_playlist, &addtopl_replace_shuffled,
                  treeplaylist_callback, Icon_NOICON);

/* A book's queue rows, which sit under More: a book is mostly resumed, and
 * rarely queued or added to anything */
MENUITEM_FUNCTION_W_PARAM(bk_play_item, 0, ID2P(LANG_PLAY),
                  add_to_playlist, &addtopl_replace,
                  treeplaylist_callback, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(bk_first_item, 0, ID2P(LANG_PLAY_NEXT),
                  add_to_playlist, &addtopl_insert_first,
                  treeplaylist_callback, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(bk_later_item, 0, ID2P(LANG_ADD),
                  add_to_playlist, &addtopl_insert,
                  treeplaylist_callback, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(bk_last_item, 0, ID2P(LANG_PLAY_LAST),
                  add_to_playlist, &addtopl_insert_last,
                  treeplaylist_callback, Icon_NOICON);

static bool is_book_row(const struct menu_item_ex *item)
{
    return item == &bk_play_item || item == &bk_first_item ||
           item == &bk_later_item || item == &bk_last_item;
}

/* Every way of playing the selection, for a screen that offers them as one
 * row of its own (the playlist viewer, the book shelf, a shortcut). The
 * browser's own menu has these rows at its top level instead. */
MAKE_ONPLAYMENU(browser_playlist_menu, ID2P(LANG_PLAYING_NEXT),
                treeplaylist_callback, Icon_Playlist,
                &replace_pl_item,
                &replace_shuf_pl_item,
                &i_first_pl_item,
                &i_pl_item,
                &i_last_pl_item,
                &i_shuf_pl_item,
                &i_last_shuf_pl_item,
                &queue_menu,
                &replace_more_pl_item,
                &replace_shuf_more_pl_item
               );

static bool sel_is_playable(void)
{
    int attr = selected_file.attr & FILE_ATTR_MASK;

    return attr == FILE_ATTR_AUDIO || attr == FILE_ATTR_M3U ||
           (selected_file.attr & ATTR_DIRECTORY);
}

/* A folder, an album or a playlist: something with tracks to shuffle */
static bool sel_is_container(void)
{
    return (selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_M3U ||
           (selected_file.attr & ATTR_DIRECTORY);
}

/* Stopped, the rows on offer start something; playing, they add to it, and
 * the ones that start something move to More. Paused counts as playing. */
static int treeplaylist_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
    (void)this_list;
    bool playing = audio_status() & AUDIO_STATUS_PLAY;

    if (action != ACTION_REQUEST_MENUITEM)
        return action;

    if (!sel_is_playable())
        return ACTION_EXIT_MENUITEM;

    if (this_item == &browser_playlist_menu)
        return action;

    if (this_item == &queue_menu)
        return playing ? action : ACTION_EXIT_MENUITEM;

    /* A book has its own copies of the top rows, under More */
    if (is_book_row(this_item) != ctx_in_order &&
        (is_book_row(this_item) || this_item == &replace_pl_item ||
         this_item == &replace_shuf_pl_item || this_item == &i_first_pl_item ||
         this_item == &i_pl_item || this_item == &i_last_pl_item))
        return ACTION_EXIT_MENUITEM;

    struct add_to_pl_param *param = this_item->function_param->param;

    if ((param->flags & PL_REPLACE) == PL_REPLACE)
    {
        if (playing != (this_item == &replace_more_pl_item ||
                        this_item == &replace_shuf_more_pl_item))
            return ACTION_EXIT_MENUITEM;
        if (param->position == PLAYLIST_INSERT_LAST_SHUFFLED)
            return (sel_is_container() && !ctx_in_order)
                   ? action : ACTION_EXIT_MENUITEM;
        /* OK already plays a track in the browser */
        if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO &&
            selected_file.context != CONTEXT_STD)
            return ACTION_EXIT_MENUITEM;
        return action;
    }

    if (!playing)
        return ACTION_EXIT_MENUITEM;

    if (param->position == PLAYLIST_INSERT_SHUFFLED ||
        param->position == PLAYLIST_INSERT_LAST_SHUFFLED)
    {
        if (ctx_in_order || !global_settings.show_shuffled_adding_options ||
            !sel_is_container())
            return ACTION_EXIT_MENUITEM;
    }

    return action;
}

/* These two open a context submenu of their own, so they report as one whatever
 * called them. context_menu_show() pushes for the menu it opens directly, but
 * these are reached from a row of that menu -- or straight from the playlist
 * viewer, which has already restored its own activity by then. Left unpushed,
 * the menu runs as whatever screen is underneath and a theme switching on %cs
 * dresses it as that screen's rows. Popped conditionally, like the other sites:
 * an item may have exited elsewhere and popped it already. */
int context_menu_show_playlist(const char* path, int attr, void (*playlist_insert_cb),
                               bool in_order)
{
    context_menu_result = ONPLAY_OK;
    ctx_current_playlist_insert = playlist_insert_cb;
    selected_file_set(CONTEXT_STD, path, attr);
    ctx_in_order = in_order;
    ctx_go_to = false;
    push_current_activity(ACTIVITY_CONTEXTMENU);
    hiding_begin();
    do_menu(&browser_playlist_menu, NULL, NULL, false);
    hiding_end();
    if (get_current_activity() == ACTIVITY_CONTEXTMENU)
        pop_current_activity();
    return context_menu_result;
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
                  cat_add_to_a_playlist, NULL, Icon_NOICON);
MENUITEM_FUNCTION(cat_add_to_new, 0, ID2P(LANG_CATALOG_ADD_TO_NEW),
                  cat_add_to_a_new_playlist, NULL, Icon_NOICON);
MAKE_ONPLAYMENU(cat_playlist_menu, ID2P(LANG_ADD_TO_PL),
                cat_playlist_callback, Icon_Playlist,
                &cat_add_to_list, &cat_add_to_new);
/* A book's, under More */
MAKE_ONPLAYMENU(book_cat_menu, ID2P(LANG_ADD_TO_PL),
                cat_playlist_callback, Icon_Playlist,
                &cat_add_to_list, &cat_add_to_new);

static int cat_playlist_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
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
        if ((this_item == &book_cat_menu) != ctx_in_order)
            return ACTION_EXIT_MENUITEM;
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

static long selected_track_idx(void);

/* An entry number is only good for the generation it was read in: a Rebuild
 * finishing while the picker is up renumbers the tracks, and the rating would
 * land on someone else's. The picker is false only on Cancel: an album's
 * rating overwrites every track, so Cancel has to be told apart from keeping
 * the rating it opened on. */
static int set_rating(bool album)
{
    long idx = album ? -1 : selected_track_idx();
    int rating = album ? 0 : browser_db_get_rating(idx);
    uint32_t generation = tagcache_generation();

    if (!rating_dialog(str(album ? LANG_MENU_SET_ALBUM_RATING
                                 : LANG_MENU_SET_RATING), &rating))
        return 0;

    if (!tagcache_is_usable() || tagcache_generation() != generation)
        splash(HZ*2, ID2P(LANG_TAGCACHE_BUSY));
    else if (!album)
        browser_db_set_rating(idx, rating);
    else if (browser_db_subentries_set_rating(rating) < 0)
        splash(HZ*2, ID2P(LANG_FAILED));

    /* A User Rating list shows the number it was sorted by. */
    if (selected_file.context == CONTEXT_ID3DB)
        context_menu_result = ONPLAY_RELOAD_DIR;
    return 0;
}

static int set_track_rating(void)
{
    return set_rating(false);
}

static int set_album_rating(void)
{
    return set_rating(true);
}

/* A track's rating from anywhere the database knows the track. An album's
 * from album rows only, as for Album Sound: from an artist or genre row the
 * tracks reached are that row's first album's -- see sound_album_selectable(). */
static int rating_callback(int action,
                           const struct menu_item_ex *this_item,
                           struct gui_synclist *this_list);
MENUITEM_FUNCTION(rating_item, 0, ID2P(LANG_MENU_SET_RATING),
                  set_track_rating, rating_callback, Icon_Questionmark);
MENUITEM_FUNCTION(album_rating_item, 0, ID2P(LANG_MENU_SET_ALBUM_RATING),
                  set_album_rating, rating_callback, Icon_Questionmark);

static int rating_callback(int action,
                           const struct menu_item_ex *this_item,
                           struct gui_synclist *this_list)
{
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM)
    {
        if (!tagcache_is_usable() || ctx_in_order)
            return ACTION_EXIT_MENUITEM;
        if (this_item == &rating_item)
            return selected_track_idx() >= 0 ? action : ACTION_EXIT_MENUITEM;
        return selected_file.context == CONTEXT_ID3DB &&
               browser_db_current_scope() == BROWSER_DB_SCOPE_ALBUM
               ? action : ACTION_EXIT_MENUITEM;
    }
    return action;
}

static bool view_cue(void)
{
    struct mp3entry* id3 = audio_current_track();
    if (id3 && id3->cuesheet)
    {
        /* True closes the menu, which only the chapter row asks for -- it
           carries MENU_FUNC_CHECK_RETVAL and the cuesheet row does not. */
        enum cue_browse_result res = browse_cuesheet(id3->cuesheet);

        if (res == CUE_BROWSE_USB)
            context_menu_result = ONPLAY_MAINMENU;
        return res != CUE_BROWSE_NONE;
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
        if (!selected_file.path || !id3 || !id3->cuesheet
            || id3->cuesheet->chapters)
            return ACTION_EXIT_MENUITEM;
    }
    return action;
}
MENUITEM_FUNCTION(view_cue_item, 0, ID2P(LANG_BROWSE_CUESHEET),
                  view_cue, view_cue_item_callback, Icon_NOICON);

/* The same screen under the name it earns when the entries came from the
   file's own chapter marks. The two rows are mutually exclusive. */
static int view_chapters_item_callback(int action,
                                       const struct menu_item_ex *this_item,
                                       struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    struct mp3entry* id3 = audio_current_track();
    if (action == ACTION_REQUEST_MENUITEM)
    {
        if (!selected_file.path || !id3 || !id3->cuesheet
            || !id3->cuesheet->chapters)
            return ACTION_EXIT_MENUITEM;
    }
    return action;
}
MENUITEM_FUNCTION(view_chapters_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_BROWSE_CHAPTERS),
                  view_cue, view_chapters_item_callback, Icon_NOICON);


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

/* The selected track's database entry, or -1. From RAM alone, since the
 * menu asks each time it is drawn and must not wake the disk for it. */
static long selected_track_idx(void)
{
    struct mp3entry *id3;
    long idx;

    if (selected_file.context == CONTEXT_WPS)
    {
        id3 = audio_current_track();
        idx = browser_db_track_idx(id3);
        if (idx < 0 && id3 != NULL)
            idx = tagcache_find_path(id3->path);
        return idx;
    }

    if ((selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO)
        return -1;
    if (selected_file.context == CONTEXT_ID3DB)
        return browser_db_selected_track_idx();
    return selected_file.path ? tagcache_find_path(selected_file.path) : -1;
}

/* The album row of the selected track, from the RAM copy's album table */
static bool selected_track_album(struct tagcache_album *album)
{
    int row = tagcache_album_of(selected_track_idx());

    return row >= 0 && tagcache_album_get(row, album);
}

static int go_to_callback(int action,
                          const struct menu_item_ex *this_item,
                          struct gui_synclist *this_list);

/* Both leave for the database browser, opened on the album or the album
 * artist, as a cover chosen in Album Covers does */
static int go_to_album(void)
{
    struct tagcache_album album;
    char name[MAX_PATH];

    if (!selected_track_album(&album))
        return 0;
    if (!tagcache_seek_string(tag_album, album.album_seek, name, sizeof(name)))
        name[0] = '\0';

    browser_db_enter_artist_album_tracks_on_next_load(album.album_seek,
                                                      album.artist_seek, name);
    context_menu_result = ONPLAY_GO_TO_LIBRARY;
    return 0;
}

static int go_to_artist(void)
{
    struct tagcache_album album;
    char name[MAX_PATH];

    if (!selected_track_album(&album))
        return 0;
    if (!tagcache_seek_string(tag_albumartist, album.artist_seek,
                              name, sizeof(name)))
        name[0] = '\0';

    browser_db_enter_artist_albums_on_next_load(album.artist_seek, name);
    context_menu_result = ONPLAY_GO_TO_LIBRARY;
    return 0;
}

MENUITEM_FUNCTION(go_to_album_item, 0, ID2P(LANG_GO_TO_ALBUM),
                  go_to_album, go_to_callback, Icon_NOICON);
MENUITEM_FUNCTION(go_to_artist_item, 0, ID2P(LANG_GO_TO_ARTIST),
                  go_to_artist, go_to_callback, Icon_NOICON);

/* An artist row in the album table has an untagged album artist as a seek
 * like any other; its name read back is what says it is untagged. */
static int go_to_callback(int action,
                          const struct menu_item_ex *this_item,
                          struct gui_synclist *this_list)
{
    struct tagcache_album album;
    char name[2];

    (void)this_list;
    if (action != ACTION_REQUEST_MENUITEM)
        return action;

    if (!ctx_go_to || ctx_in_order || !selected_track_album(&album))
        return ACTION_EXIT_MENUITEM;
    if (this_item == &go_to_artist_item &&
        !tagcache_seek_string(tag_albumartist, album.artist_seek,
                              name, sizeof(name)))
        return ACTION_EXIT_MENUITEM;
    return action;
}

static char mark_book[BOOK_ID_MAX];

static int mark_book_callback(int action,
                              const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list)
{
    struct browser_context *c = browser_get_context();
    long album_seek, artist_seek;

    (void)this_item;
    (void)this_list;
    /* Not a podcast, which the shelves leave out: a show is never finished */
    if (action == ACTION_REQUEST_MENUITEM
        && !(in_book_list()
             && browser_db_get_book_album(c, c->selected_item, &album_seek,
                                          &artist_seek)
             && !db_spoken_album_is_podcast(album_seek, artist_seek)
             && browser_db_get_book(c, c->selected_item,
                                    mark_book, sizeof(mark_book))))
        return ACTION_EXIT_MENUITEM;
    return action;
}

static int mark_book_run(void)
{
    if (book_shelf_mark_menu(mark_book, -1))
        context_menu_result = ONPLAY_RELOAD_DIR;
    return 0;
}

MENUITEM_FUNCTION(mark_book_item, 0, ID2P(LANG_BOOK_MARK_AS),
                  mark_book_run, mark_book_callback, Icon_NOICON);

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
            /* The filename is read from disk */
            if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO
                && !storage_disk_is_active())
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

/* Named for what it describes: from the database a row that is not a track
 * shows that row's own details, not a track's. */
static int info_lang_id(void)
{
    if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO)
        return LANG_TRACK_INFO;
    if (ctx_in_order)
        return LANG_BOOK_INFO;

    switch (browser_db_current_scope())
    {
        case BROWSER_DB_SCOPE_ALBUM:
            return LANG_ALBUM_INFO;
        case BROWSER_DB_SCOPE_ARTIST:
            return LANG_ARTIST_INFO;
        default:
            return LANG_INFO;
    }
}

static char *info_item_name(int selected_item, void *data,
                            char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return str(info_lang_id());
}

static int info_item_speak(int selected_item, void *data)
{
    (void)selected_item; (void)data;
    talk_id(info_lang_id(), false);
    return 0;
}

MENUITEM_FUNCTION_DYNTEXT_W_PARAM(track_info_item, 0,
                  context_menu_properties, NULL,
                  info_item_name, info_item_speak, NULL,
                  clipboard_callback, Icon_NOICON);

/* The activity dance and the side-channel are context_menu_properties()'s, and
 * for its reasons: a screen opened from here must not be drawn inside the
 * context menu's activity, and do_menu() discards this function's own return
 * value. There is no prepare_database_sel() because nothing here works from a
 * path -- the screen asks the browser which row is selected. */
static bool context_menu_listen_progress(void)
{
    struct browser_context *c = browser_get_context();
    char title[MAX_PATH];
    long album_seek, artist_seek;
    int ret;

    if (get_current_activity() == ACTIVITY_CONTEXTMENU)
        pop_current_activity_without_refresh();

    /* A book, track rows included, is that author's book alone */
    if (browser_db_get_book_album(c, c->selected_item, &album_seek,
                                  &artist_seek)
        && tagcache_seek_string(tag_album, album_seek, title, sizeof(title)))
        ret = listen_progress_show_book(album_seek, artist_seek, title);
    else
        ret = listen_progress_show();
    if (ret == GO_TO_ROOT)
        context_menu_result = ONPLAY_MAINMENU;
    return false;
}
static int listen_progress_callback(int action,
                                    const struct menu_item_ex *this_item,
                                    struct gui_synclist *this_list);
/* A book's own figures sit at the top of its menu; an album's or an
 * artist's, under More. */
MENUITEM_FUNCTION(listen_book_item, 0, ID2P(LANG_LISTEN_PROGRESS),
                  context_menu_listen_progress,
                  listen_progress_callback, Icon_NOICON);
MENUITEM_FUNCTION(listen_progress_item, 0, ID2P(LANG_LISTEN_PROGRESS),
                  context_menu_listen_progress,
                  listen_progress_callback, Icon_NOICON);

static int listen_progress_callback(int action,
                                    const struct menu_item_ex *this_item,
                                    struct gui_synclist *this_list)
{
    struct browser_context *c = browser_get_context();
    long album_seek, artist_seek;

    (void)this_list;
    if (action != ACTION_REQUEST_MENUITEM)
        return action;

    if (selected_file.context != CONTEXT_ID3DB ||
        (this_item == &listen_book_item) != in_book_list())
        return ACTION_EXIT_MENUITEM;

    return browser_db_current_scope() != BROWSER_DB_SCOPE_NONE
           || browser_db_get_book_album(c, c->selected_item,
                                        &album_seek, &artist_seek)
         ? action : ACTION_EXIT_MENUITEM;
}
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
                  set_catalogdir, clipboard_callback, Icon_NOICON);

MAKE_ONPLAYMENU(set_as_dir_menu, ID2P(LANG_SET_AS),
                clipboard_callback, Icon_NOICON,
                &set_catalogdir_item,
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

/* Now Playing's rows for the playing file itself */
static bool hotkey_delete_item(void);

static int wps_track_info(void)
{
    if (browse_id3_wrapper() == GO_TO_ROOT)
        context_menu_result = ONPLAY_MAINMENU;
    return 0;
}

static int wps_file_callback(int action,
                             const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM &&
        (selected_file.path == NULL || selected_file.path[0] == '\0'))
        return ACTION_EXIT_MENUITEM;
    return action;
}

MENUITEM_FUNCTION(wps_track_info_item, 0, ID2P(LANG_TRACK_INFO),
                  wps_track_info, wps_file_callback, Icon_NOICON);
MENUITEM_FUNCTION(wps_reveal_item, 0, ID2P(LANG_SHOW_IN_FILES),
                  reveal, wps_file_callback, Icon_file_view_menu);
MENUITEM_FUNCTION(wps_album_art_item, 0, ID2P(LANG_VIEW_ALBUMART),
                  view_album_art, wps_file_callback, Icon_NOICON);
MENUITEM_FUNCTION(wps_delete_item, 0, ID2P(LANG_DELETE),
                  hotkey_delete_item, wps_file_callback, Icon_NOICON);

/* The title is what the menu acts on: the playing track's title, or the
 * selected row's name. Copied, because the playing track's id3 is the live
 * entry and not a copy of one. */
static char *context_menu_title(int selected_item, void *data,
                                char *buffer, size_t buffer_len)
{
    struct mp3entry *id3;
    const char *name;

    (void)selected_item;
    (void)data;

    if (selected_file.context == CONTEXT_WPS
        && (id3 = audio_current_track()) != NULL
        && id3->title != NULL && id3->title[0] != '\0')
    {
        strmemccpy(buffer, id3->title, buffer_len);
        return buffer;
    }

    if (selected_file.context == CONTEXT_ID3DB)
    {
        browser_db_current_entry_name(buffer, buffer_len);
        if (buffer[0] != '\0')
            return buffer;
    }
    else if (selected_file.path != NULL)
    {
        name = strrchr(selected_file.path, PATH_SEPCH);
        name = name ? name + 1 : selected_file.path;
        if (*name != '\0')
        {
            strmemccpy(buffer, name, buffer_len);
            return buffer;
        }
    }

    return str(LANG_ONPLAY_MENU_TITLE);
}

/* As MAKE_ONPLAYMENU, titled by context_menu_title() */
#define MAKE_ONPLAYMENU_TITLED( name, callback, icon, ... )             \
    static const struct menu_item_ex *name##_[]  = {__VA_ARGS__};       \
    static const struct menu_get_name_and_icon name##__ =               \
        {callback, context_menu_title, NULL, NULL, icon};               \
    static const struct menu_item_ex name =                             \
        {MT_MENU|MENU_DYNAMIC_DESC|MENU_EXITAFTERTHISMENU|              \
         MENU_ITEM_COUNT(sizeof( name##_)/sizeof(*name##_)),            \
            { (void*)name##_},{.menu_get_name_and_icon = & name##__}};

MENUITEM_FUNCTION(view_playlist_item, 0, ID2P(LANG_VIEW),
                  view_playlist,
                  context_menu_callback, Icon_NOICON);

/* Whether any of a submenu's rows would show. A submenu with none is hidden
 * rather than opened onto an empty list. */
static int submenu_callback(int action,
                            const struct menu_item_ex *this_item,
                            struct gui_synclist *this_list)
{
    if (action != ACTION_REQUEST_MENUITEM)
        return action;

    for (unsigned i = 0; i < MENU_GET_COUNT(this_item->flags); i++)
    {
        const struct menu_item_ex *row = this_item->submenus[i];
        /* A setting row holds its callback directly, as get_menu_callback()
         * in menu.c reads it */
        menu_callback_type cb =
            (row->flags & (MENU_HAS_DESC|MENU_DYNAMIC_DESC))
                ? row->callback_and_desc->menu_callback : row->menu_callback;

        if (row_hidden(row))
            continue;
        if (cb == NULL || cb(action, row, this_list) != ACTION_EXIT_MENUITEM)
            return action;
    }
    return ACTION_EXIT_MENUITEM;
}

/* The rarer ways of playing the selection, and the places it can be taken */
MAKE_ONPLAYMENU( more_menu, ID2P(LANG_MORE),
           submenu_callback, Icon_NOICON,
           &bk_play_item, &bk_first_item, &bk_later_item, &bk_last_item,
           &book_cat_menu,
           &replace_more_pl_item, &replace_shuf_more_pl_item,
           &i_shuf_pl_item, &i_last_shuf_pl_item, &queue_menu,
           &sound_album_item,
           &listen_progress_item,
           &reveal_item,
           &add_to_faves_item,
         );

/* File management, for the file browser */
MAKE_ONPLAYMENU( manage_menu, ID2P(LANG_MANAGE),
           submenu_callback, Icon_NOICON,
           &rename_file_item, &clipboard_cut_item, &clipboard_copy_item,
           &delete_file_item, &delete_dir_item, &create_dir_item,
           &set_backdrop_item, &set_as_dir_menu,
         );

/* Now Playing's rarer rows */
MAKE_ONPLAYMENU( wps_more_menu, ID2P(LANG_MORE),
           submenu_callback, Icon_NOICON,
           &wps_reveal_item,
           &wps_album_art_item,
           &winddown_item,
           &bookmark_menu,
           &view_cue_item,
           &view_chapters_item,
           &equalizer_menu,
           &sound_settings, &playback_settings,
           &wps_delete_item,
         );

/* used when context_menu_show() is called in the CONTEXT_WPS context */
MAKE_ONPLAYMENU_TITLED( wps_context_menu,
           context_menu_callback, Icon_Audio,
           &wps_shuffle_item, &wps_repeat_item,
           &rating_item,
           &wps_track_info_item,
           &go_to_album_item, &go_to_artist_item,
           &lyrics_item,
           &sleep_timer_item,
           &sound_mix_item,
           &spike_run_item,
           &wps_playlist_menu, &cat_playlist_menu,
           &wps_more_menu,
         );

/* used when context_menu_show() is not called in the CONTEXT_WPS context */
MAKE_ONPLAYMENU_TITLED( browser_context_menu,
           context_menu_callback, Icon_file_view_menu,
           &mark_book_item,
           &listen_book_item,
           &view_playlist_item,
           &replace_pl_item, &replace_shuf_pl_item,
           &i_first_pl_item, &i_pl_item, &i_last_pl_item,
           &cat_playlist_menu,
           &sound_mix_item, &winddown_item,
           &rating_item, &album_rating_item,
           &go_to_album_item, &go_to_artist_item,
           &properties_item, &track_info_item,
           &clipboard_paste_item,
           &more_menu,
           &manage_menu,
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
        add_to_playlist(ctx_in_order ? &addtopl_insert : &addtopl_insert_shuf);
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
    bool to_root = simplelist_show_list(&info);

    if (execute)
    {
        if (to_root)
            return ONPLAY_MAINMENU;
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

/* Assign a hotkey's action from the settings screens */
static int hotkey_menu_do_setting(void *param, int *setting, int flag)
{
    int sel = hotkey_run_menu(flag, false, HK_CTX_GET(0, *setting));

    (void)param;
    if (sel >= 0)
        *setting = HK_CTX_SET(0, sel);
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

/* Names actions were saved under before they were renamed, so a config.cfg
   written then keeps its assignment */
static const struct
{
    const char *name;
    int action;
} hotkey_old_names[] = {
    { "Add to Queue",       HOTKEY_INSERT },
    { "Shuffle into Queue", HOTKEY_INSERT_SHUFFLED },
};

/* Whole names only: "Play Later" is also the start of "Play Later Shuffled" */
static bool cfg_name_is(const char *st, size_t len, const char *name)
{
    return strlen(name) == len && strncasecmp(st, name, len) == 0;
}

void wps_context_menu_load_from_cfg(void *setting, char *value)
{
    int item = 0;
    int var = 0;
    char *st = value;
    char *end = value;
    while (*end != '\0' && item < 1)
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
                    if (cfg_name_is(st, end - st, this))
                    {
                        var |= HK_CTX_SET(item, hotkey_items[i].action);
                    }
                }
                for (size_t i = 0; i < ARRAYLEN(hotkey_old_names); i++)
                {
                    if (cfg_name_is(st, end - st, hotkey_old_names[i].name))
                        var |= HK_CTX_SET(item, hotkey_old_names[i].action);
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

    if (buf_len > 0)
        snprintf(buf, buf_len, "%s, ",
                 lang_id_to_english(get_hotkey(HK_CTX_GET(0, var))->lang_id));
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

/* The menu a screen of its own shows for one of its items: the browser's
 * ways of playing it, with that screen's own rows among them. The rows are
 * the caller's, held only while the menu is up. */
static const struct context_menu_row *item_rows;
static int item_row_count;

/* data is the row's index into item_rows, plus CONTEXT_ROWS_MAX for a slot
 * under More */
static int item_row_index(const void *data)
{
    return (intptr_t)data % CONTEXT_ROWS_MAX;
}

static char *item_row_name(int selected_item, void *data,
                           char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)buffer; (void)buffer_len;
    return str(item_rows[item_row_index(data)].lang_id);
}

static int item_row_speak(int selected_item, void *data)
{
    (void)selected_item;
    talk_id(item_rows[item_row_index(data)].lang_id, false);
    return 0;
}

static int item_row_run(void *data)
{
    context_menu_result = item_rows[item_row_index(data)].run();
    return 0;
}

static int item_row_callback(int action,
                             const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    void *data = this_item->menu_get_name_and_icon->list_get_name_data;
    int i = item_row_index(data);

    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM &&
        (i >= item_row_count ||
         item_rows[i].more != ((intptr_t)data >= CONTEXT_ROWS_MAX)))
        return ACTION_EXIT_MENUITEM;
    return action;
}

#define ITEM_ROW(name, data)                                                \
    MENUITEM_FUNCTION_DYNTEXT_W_PARAM(name, 0, item_row_run, (void *)(data), \
                                      item_row_name, item_row_speak,        \
                                      (void *)(data), item_row_callback,    \
                                      Icon_NOICON)

ITEM_ROW(item_row_0, 0)
ITEM_ROW(item_row_1, 1)
ITEM_ROW(item_row_2, 2)
ITEM_ROW(item_row_3, 3)
ITEM_ROW(item_row_4, 4)
ITEM_ROW(item_row_5, 5)
ITEM_ROW(item_row_6, 6)
ITEM_ROW(item_row_7, 7)
ITEM_ROW(item_more_0, CONTEXT_ROWS_MAX + 0)
ITEM_ROW(item_more_1, CONTEXT_ROWS_MAX + 1)
ITEM_ROW(item_more_2, CONTEXT_ROWS_MAX + 2)
ITEM_ROW(item_more_3, CONTEXT_ROWS_MAX + 3)
ITEM_ROW(item_more_4, CONTEXT_ROWS_MAX + 4)
ITEM_ROW(item_more_5, CONTEXT_ROWS_MAX + 5)
ITEM_ROW(item_more_6, CONTEXT_ROWS_MAX + 6)
ITEM_ROW(item_more_7, CONTEXT_ROWS_MAX + 7)

MAKE_ONPLAYMENU( item_more_menu, ID2P(LANG_MORE),
           submenu_callback, Icon_NOICON,
           &bk_play_item, &bk_first_item, &bk_later_item, &bk_last_item,
           &book_cat_menu,
           &replace_more_pl_item, &replace_shuf_more_pl_item,
           &i_shuf_pl_item, &i_last_shuf_pl_item, &queue_menu,
           &item_more_0, &item_more_1, &item_more_2, &item_more_3,
           &item_more_4, &item_more_5, &item_more_6, &item_more_7,
         );

MAKE_ONPLAYMENU_TITLED( item_menu,
           context_menu_callback, Icon_NOICON,
           &replace_pl_item, &replace_shuf_pl_item,
           &i_first_pl_item, &i_pl_item, &i_last_pl_item,
           &item_row_0, &item_row_1, &item_row_2, &item_row_3,
           &item_row_4, &item_row_5, &item_row_6, &item_row_7,
           &cat_playlist_menu,
           &rating_item,
           &go_to_album_item, &go_to_artist_item,
           &item_more_menu,
         );

/* ---- rows hidden from each kind of menu ----------------------------------
 *
 * A row is named by its label, so the same row can be hidden from an album's
 * menu and left on a track's, and a screen's own rows can be hidden as the
 * browser's are. What cannot be hidden is what keeps a menu from emptying:
 * the Info row and the rows that play. Each kind's rows are listed in the
 * order its editor shows them; a row's place in its list is its bit in
 * global_settings.context_hidden[], which is saved by name. */

static const short now_playing_rows[] = {
    LANG_SHUFFLE, LANG_REPEAT, LANG_MENU_SET_RATING, LANG_GO_TO_ALBUM,
    LANG_GO_TO_ARTIST, LANG_LYRICS, LANG_SLEEP_TIMER, LANG_SOUND_MIX,
    LANG_SPIKE_PLAY, LANG_CURRENT_PLAYLIST, LANG_ADD_TO_PL, LANG_SHOW_IN_FILES,
    LANG_VIEW_ALBUMART, LANG_WINDDOWN, LANG_BOOKMARK_MENU, LANG_BROWSE_CUESHEET,
    LANG_BROWSE_CHAPTERS, LANG_EQUALIZER, LANG_SOUND_SETTINGS, LANG_PLAYBACK,
    LANG_DELETE,
};
static const short track_rows[] = {
    LANG_ADD_TO_PL, LANG_SOUND_MIX, LANG_WINDDOWN, LANG_MENU_SET_RATING,
    LANG_GO_TO_ALBUM, LANG_GO_TO_ARTIST, LANG_PASTE, LANG_QUEUE_MENU,
    LANG_SHOW_IN_FILES, LANG_ADD_TO_FAVES, LANG_RENAME, LANG_CUT, LANG_COPY,
    LANG_DELETE, LANG_CREATE_DIR,
};
static const short album_rows[] = {
    LANG_ADD_TO_PL, LANG_SOUND_MIX, LANG_MENU_SET_ALBUM_RATING,
    LANG_GO_TO_ARTIST, LANG_ADD_SHUFFLED, LANG_PLAY_LAST_SHUFFLED,
    LANG_QUEUE_MENU, LANG_SOUND_ALBUM, LANG_LISTEN_PROGRESS,
    LANG_SHOW_IN_FILES,
};
static const short artist_rows[] = {
    LANG_ADD_TO_PL, LANG_ADD_SHUFFLED, LANG_PLAY_LAST_SHUFFLED,
    LANG_QUEUE_MENU, LANG_LISTEN_PROGRESS, LANG_SHOW_IN_FILES,
};
static const short folder_rows[] = {
    LANG_VIEW, LANG_ADD_TO_PL, LANG_SOUND_MIX, LANG_SOUND_ALBUM, LANG_PASTE,
    LANG_ADD_SHUFFLED, LANG_PLAY_LAST_SHUFFLED, LANG_QUEUE_MENU,
    LANG_ADD_TO_FAVES, LANG_RENAME, LANG_CUT, LANG_COPY, LANG_DELETE,
    LANG_DELETE_DIR, LANG_CREATE_DIR, LANG_SET_AS_BACKDROP, LANG_SET_AS,
};
static const short book_rows[] = {
    LANG_BOOK_MARK_AS, LANG_LISTEN_PROGRESS, LANG_ADD_TO_PL, LANG_QUEUE_MENU,
    LANG_SHOW_IN_FILES,
};

static const struct {
    int lang_id;
    const short *rows;
    int count;
} kinds[CONTEXT_KINDS] = {
    [CONTEXT_KIND_NOW_PLAYING] = { LANG_NOW_PLAYING_MENU, now_playing_rows,
                                   ARRAYLEN(now_playing_rows) },
    [CONTEXT_KIND_TRACK]  = { LANG_TRACK_MENU,  track_rows,
                              ARRAYLEN(track_rows) },
    [CONTEXT_KIND_ALBUM]  = { LANG_ALBUM_MENU,  album_rows,
                              ARRAYLEN(album_rows) },
    [CONTEXT_KIND_ARTIST] = { LANG_ARTIST_MENU, artist_rows,
                              ARRAYLEN(artist_rows) },
    [CONTEXT_KIND_FOLDER] = { LANG_FOLDER_MENU, folder_rows,
                              ARRAYLEN(folder_rows) },
    [CONTEXT_KIND_BOOK]   = { LANG_BOOK_MENU,   book_rows,
                              ARRAYLEN(book_rows) },
};

_Static_assert(CONTEXT_KINDS == CONTEXT_MENU_KINDS,
               "one hidden-row mask per kind of context menu");

int context_menu_kind_label(int kind)
{
    return kinds[kind].lang_id;
}

int context_menu_kind_rows(int kind, const short **rows)
{
    *rows = kinds[kind].rows;
    return kinds[kind].count;
}

/* The kind of the menu that is up, or -1 */
static int ctx_kind = -1;

/* What the selection is, as far as hiding goes. A screen's own item that is
 * not a track or a book is taken for an album: covers, guests and the like
 * all play a set of tracks, as an album's menu does. */
static int selected_kind(void)
{
    if (selected_file.context == CONTEXT_WPS)
        return CONTEXT_KIND_NOW_PLAYING;
    if (ctx_in_order)
        return CONTEXT_KIND_BOOK;
    if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO)
        return CONTEXT_KIND_TRACK;
    if (selected_file.context == CONTEXT_ID3DB)
        return browser_db_current_scope() == BROWSER_DB_SCOPE_ALBUM
               ? CONTEXT_KIND_ALBUM : CONTEXT_KIND_ARTIST;
    if (selected_file.context == CONTEXT_STD)
        return CONTEXT_KIND_ALBUM;
    return CONTEXT_KIND_FOLDER;
}

/* The label a row is known by: a setting's name, a fixed label, or for the
 * two kinds of dynamic row that can be hidden, the label behind them */
static int row_lang_id(const struct menu_item_ex *row)
{
    int type = row->flags & MENU_TYPE_MASK;

    if (type == MT_SETTING || type == MT_SETTING_W_TEXT)
    {
        const struct settings_list *s = find_setting(row->variable);
        return s ? s->lang_id : -1;
    }
    if (row->flags & MENU_HAS_DESC)
        return P2ID(row->callback_and_desc->desc);
    if (row->flags & MENU_DYNAMIC_DESC)
    {
        const struct menu_get_name_and_icon *d = row->menu_get_name_and_icon;

        if (row == &sleep_timer_item)
            return LANG_SLEEP_TIMER;
        if (d->list_get_name == item_row_name)
        {
            int i = item_row_index(d->list_get_name_data);
            return i < item_row_count ? item_rows[i].lang_id : -1;
        }
    }
    return -1;
}

/* The row's bit in its kind's mask, or -1 for a row that cannot be hidden */
static int row_bit(int kind, const struct menu_item_ex *row)
{
    int lang_id;

    if (kind < 0)
        return -1;
    lang_id = row_lang_id(row);
    for (int i = 0; i < kinds[kind].count; i++)
    {
        if (kinds[kind].rows[i] == lang_id)
            return i;
    }
    return -1;
}

static bool row_hidden(const struct menu_item_ex *row)
{
    int bit = row_bit(ctx_kind, row);

    return bit >= 0 && (global_settings.context_hidden[ctx_kind] & (1u << bit));
}

static bool row_hideable(const struct menu_item_ex *row)
{
    return row_bit(ctx_kind, row) >= 0;
}

/* Only ever called for a row row_hideable() has passed */
static void row_hide(const struct menu_item_ex *row)
{
    int bit = row_bit(ctx_kind, row);

    global_settings.context_hidden[ctx_kind] |= 1u << bit;
    settings_save();
    splash(HZ, ID2P(LANG_ROW_HIDDEN));
}

static const struct menu_row_hiding row_hiding = {
    .hidden = row_hidden,
    .hideable = row_hideable,
    .hide = row_hide,
};

/* Around every do_menu() of a context menu, once the selection is known */
static void hiding_begin(void)
{
    ctx_kind = selected_kind();
    menu_set_row_hiding(&row_hiding);
}

static void hiding_end(void)
{
    menu_set_row_hiding(NULL);
    ctx_kind = -1;
}

/* config.cfg: one line per kind, its hidden rows by their English labels,
 * so that adding or moving a row does not move the others */
static int setting_kind(void *setting)
{
    return (uint32_t *)setting - global_settings.context_hidden;
}

void context_hidden_load_from_cfg(void *setting, char *value)
{
    int kind = setting_kind(setting);
    uint32_t mask = 0;
    char *save, *name = strtok_r(value, ",", &save);

    while (name)
    {
        name = skip_whitespace(name);
        for (int i = 0; i < kinds[kind].count; i++)
        {
            if (!strcasecmp(name, lang_id_to_english(kinds[kind].rows[i])))
                mask |= 1u << i;
        }
        name = strtok_r(NULL, ",", &save);
    }
    *(uint32_t *)setting = mask;
}

char *context_hidden_write_to_cfg(void *setting, char *buf, int buf_len)
{
    int kind = setting_kind(setting);
    uint32_t mask = *(uint32_t *)setting;
    int len = 0;

    buf[0] = '\0';
    for (int i = 0; i < kinds[kind].count && len < buf_len; i++)
    {
        if (mask & (1u << i))
            len += snprintf(buf + len, buf_len - len, "%s%s", len ? ", " : "",
                            lang_id_to_english(kinds[kind].rows[i]));
    }
    return buf;
}

bool context_hidden_is_changed(void *setting, void *defaultval)
{
    (void)defaultval;
    return *(uint32_t *)setting != 0;
}

void context_hidden_set_default(void *setting, void *defaultval)
{
    (void)defaultval;
    *(uint32_t *)setting = 0;
}


int context_menu_show_item(const char *path, int attr, bool in_order,
                           bool go_to,
                           bool (*playlist_insert_cb)(int position, bool queue,
                                                      bool create_new),
                           int (*add_to_pl_cb)(const char *playlist,
                                               bool new_playlist),
                           const struct context_menu_row *rows, int count)
{
    context_menu_result = ONPLAY_OK;
    selected_file_set(CONTEXT_STD, path, attr);
    ctx_in_order = in_order;
    ctx_go_to = go_to;
    ctx_current_playlist_insert = playlist_insert_cb;
    ctx_add_to_playlist = add_to_pl_cb;
    item_rows = rows;
    item_row_count = MIN(count, CONTEXT_ROWS_MAX);

    push_current_activity(ACTIVITY_CONTEXTMENU);
    hiding_begin();
    if (do_menu(&item_menu, NULL, NULL, false) == MENU_ATTACHED_USB)
        context_menu_result = ONPLAY_MAINMENU;
    hiding_end();
    if (get_current_activity() == ACTIVITY_CONTEXTMENU)
        pop_current_activity();

    item_row_count = 0;
    return context_menu_result;
}

int context_menu_show(char* file, int attr, int from_context, bool hotkey, int customaction)
{
    const struct menu_item_ex *menu;
    context_menu_result = ONPLAY_OK;
    ctx_current_playlist_insert = NULL;
    ctx_go_to = true;
    selected_file_set(from_context, NULL, attr);
    ctx_in_order = from_context == CONTEXT_ID3DB && in_book_list();

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
            int act = HK_CTX_GET(0, global_settings.context_wps);

            if (act == HOTKEY_CONTEXT_MENU)
                return hotkey_run_menu(HOTKEY_FLAG_WPS, true, 0);
            return execute_hotkey(act);
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
    hiding_begin();
    menu_selection = do_menu(menu, NULL, NULL, false);
    hiding_end();

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
