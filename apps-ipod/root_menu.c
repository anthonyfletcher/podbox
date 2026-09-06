/***************************************************************************
 * Original code from RockBox
 * was: apps/root_menu.c
 * Copyright (C) 2007 Jonathan Gordon
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * The top-level menu and the navigation loop behind it: dispatches to each
 * screen, remembers where the user came from, and owns the start-up screen
 * setting.
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include "string-extra.h"
#include "config.h"
#include "system/appevents.h"
#include "widgets/menu.h"
#include "root_menu.h"
#include "lang.h"
#include "settings/settings.h"
#include "kernel.h"
#include "debug.h"
#include "system/activity.h"
#include "system/strutil.h"
#include "rolo.h"
#include "powermgmt.h"
#include "power.h"
#include "speech/talk.h"
#include "audio.h"
#include "screens/shortcuts.h"

/* gui api */
#include "widgets/list.h"
#include "widgets/splash.h"
#include "input/action.h"
#include "widgets/yesno.h"
#include "draw/viewport.h"

#include "screens/browse/browser.h"
#include "screens/browse/browser_db.h"
#include "screens/playback/wps.h"
#include "screens/bookmark.h"
#include "playlist/playlist.h"
#include "database/sound_mix.h"
#include "playlist/viewer.h"
#include "playlist/catalog.h"
#include "screens/settings/exported_settings.h"
#include "database/tagcache.h"
#include "screens/covers/album_covers.h"
#include "screens/system/db_search.h"
#include "screens/system/file_search.h"
#include "dircache.h"                 /* whether the file search has a source */
#include "screens/browse/album_charts.h"
#include "screens/browse/featured_artists.h"
#include "screens/browse/browser_flat.h"
#include "speech/language.h"
#include "files/filetypes.h"
#include "viewers/text_viewer/text_viewer.h"
#include "viewers/playback_viewer/pv_row.h"
#include "disk.h"
#include "sound.h"

struct root_items {
    int (*function)(void* param);
    void* param;
    const struct menu_item_ex *context_menu;
};
static int next_screen = GO_TO_ROOT; /* holding info about the upcoming screen
                                        * which is the current screen for the
                                        * rest of the code after load_screen
                                        * is called */
static int last_screen = GO_TO_ROOT; /* unfortunatly needed so we can resume
                                        or goto current track based on previous
                                        screen */

static int previous_music = GO_TO_WPS; /* Toggles behavior of the return-to
                                        * playback-button. Upstream this also
                                        * tracked the FM screen; this fork has
                                        * no tuner, so it only ever holds
                                        * GO_TO_WPS. */

static char current_track_path[MAX_PATH];
static void rootmenu_track_changed_callback(unsigned short id, void* param)
{
    (void)id;
    struct mp3entry *id3 = ((struct track_event *)param)->id3;
    strmemccpy(current_track_path, id3->path, MAX_PATH);
}

/* One file the browser opens on next entry instead of resuming where it was,
 * armed by the context menu's "Show in Files". Consumed once: the browser
 * goes back to its own position afterwards. */
static char reveal_path[MAX_PATH];

void browser_reveal_on_next_load(const char *path)
{
    strmemccpy(reveal_path, path, MAX_PATH);
}
/* Can a database screen be opened right now?
 *
 * Deliberately stricter than tagcache_is_usable(), which only asks whether a
 * database exists. Committing one that already exists leaves it saying yes,
 * and every tagcache_search() made during a commit then blocks on the commit's
 * read lock -- on this, the UI thread -- so the screen stops redrawing and
 * stops taking buttons until the commit finishes, and the search fails at the
 * end of it anyway. Waiting below instead is the same wait with a progress
 * screen and a way out. */
static bool tagcache_reachable(void)
{
    return tagcache_is_usable() && tagcache_get_commit_step() == 0;
}

/* Waits for the tagcache to become usable, showing build/init progress as
 * needed. Returns false if the user aborted (caller should bail out). */
static bool wait_for_tagcache_ready(void)
{
    if (!tagcache_reachable())
    {
        bool reinit_attempted = false;
        int idle_passes = 0;

        /* The commit meter below is drawn with its own voice held, because
         * the block inside the loop says the same thing at more length and
         * two announcements of one commit talk over each other. */
        splash_progress_set_silent(true);

        /* Now display progress until it's ready or the user exits */
        while(!tagcache_reachable())
        {
            struct tagcache_stat *stat = tagcache_get_stat();

            /* Allow user to exit */
            if (action_userabort(HZ/2))
                break;

            /* Maybe just needs to reboot due to delayed commit */
            if (stat->commit_delayed)
            {
                splash(HZ*2, ID2P(LANG_PLEASE_REBOOT));
                break;
            }

            /* The database isn't usable yet (missing, or a previous build
             * never finished). The background tagcache thread builds a
             * missing database on its own now, but if we've landed here with
             * nothing in progress, kick a rebuild automatically.
             *
             * readyvalid gates this and nothing else: a rebuild must not be
             * decided from a ready status nobody has ascertained. It is set
             * only by tagcache_commit_finalize(), so a commit that began
             * before this boot's first finalize leaves it false for the whole
             * run -- which is why the progress below must not sit behind it. */
            if (stat->readyvalid && !reinit_attempted && !stat->ready &&
                stat->processed_entries == 0 && stat->commit_step == 0)
            {
                reinit_attempted = true;
                tagcache_rebuild();
                idle_passes = 0;  /* it gets a grace window of its own */
            }

            /* Nothing is building, and nothing here can start one: either a
             * rebuild has been asked for already and did not take, or
             * readyvalid withholds the one above. The progress splash below
             * has no timeout, so left to itself it reports a build that is
             * not happening, for as long as the screen is up.
             *
             * A few passes of grace first -- the tagcache thread finalizes
             * about a second after boot, and until it does this state is
             * indistinguishable from a database that is merely still being
             * checked. After that a restart really is the repair: the boot
             * check commits a leftover database_tmp.tcd, and rebuilds
             * outright when the headers do not pass. */
            if (stat->commit_step == 0 && stat->processed_entries == 0)
            {
                if (++idle_passes > 6)
                {
                    splash(HZ*2, ID2P(LANG_PLEASE_REBOOT));
                    break;
                }
            }
            else
                idle_passes = 0;

            /* Announce building progress by voice, ahead of the splash below
             * so the richer wording is the one that is spoken. */
            static long talked_tick = 0;
            if(global_settings.talk_menu &&
               (talked_tick == 0
                || TIME_AFTER(current_tick, talked_tick+7*HZ)))
            {
                talked_tick = current_tick;
                if (stat->commit_step > 0)
                {
                    talk_id(LANG_TAGCACHE_INIT, false);
                    talk_number(stat->commit_step, true);
                    talk_id(VOICE_OF, true);
                    talk_number(tagcache_get_max_commit_step(), true);
                } else if(stat->processed_entries)
                {
                    talk_number(stat->processed_entries, false);
                    talk_id(LANG_BUILDING_DATABASE, true);
                }
            }

            /* The status-bar activity indicator (%ld) reports a scan running
             * behind another screen, and cannot report this one: the wait is
             * blocking, so the screen underneath is frozen and reads as broken
             * without something drawn over it saying why and how to leave. */
            if (stat->commit_step > 0)
            {
                if (lang_is_rtl())
                    splash_progress(stat->commit_step,
                                    tagcache_get_max_commit_step(),
                                    "(%s) [%d/%d] %s", str(LANG_OFF_ABORT),
                                    stat->commit_step,
                                    tagcache_get_max_commit_step(),
                                    str(LANG_TAGCACHE_INIT));
                else
                    splash_progress(stat->commit_step,
                                    tagcache_get_max_commit_step(),
                                    "%s [%d/%d] (%s)", str(LANG_TAGCACHE_INIT),
                                    stat->commit_step,
                                    tagcache_get_max_commit_step(),
                                    str(LANG_OFF_ABORT));
            }
            else
                splashf(0, str(LANG_BUILDING_DATABASE),
                        stat->processed_entries); /* (voiced above) */
        }

        splash_progress_set_silent(false);
    }
    return tagcache_reachable();
}

static int browser(void* param)
{
    int ret_val;
    struct browser_context* tc = browser_get_context();
    int filter = SHOW_SUPPORTED;
    char folder[MAX_PATH] = "/";
    /* stuff needed to remember position in file browser */
    static char last_folder[MAX_PATH] = "/";
    /* and stuff for the database browser */
    static int last_db_dirlevel = 0, last_db_selection = 0, last_ft_dirlevel = 0;

    switch ((intptr_t)param)
    {
        case GO_TO_FILEBROWSER:
            filter = global_settings.dirfilter;
            if (reveal_path[0])
            {
                strcpy(folder, reveal_path);
                reveal_path[0] = '\0';
            }
            else if (global_settings.browse_current &&
                    last_screen == GO_TO_WPS &&
                    current_track_path[0])
            {
                strcpy(folder, current_track_path);
            }
            else if (!strcmp(last_folder, "/"))
            {
                strcpy(folder, global_settings.start_directory);
            }
            else
            {
                    strcpy(folder, last_folder);
            }
            push_current_activity(ACTIVITY_FILEBROWSER);
        break;
        case GO_TO_DBBROWSER:
            if (!wait_for_tagcache_ready())
                return GO_TO_PREVIOUS;
            filter = SHOW_ID3DB;
            last_ft_dirlevel = tc->dirlevel;
            tc->dirlevel = last_db_dirlevel;
            tc->selected_item = last_db_selection;
            /* dirlevel and currtable have to agree, and this entry restores
             * only the first of them -- the table is whatever the last
             * database session left behind, which is not always a session
             * this one is resuming. A shortcut's listing or an album entered
             * from Album covers both end at level 0 with the table pointing
             * into the tree, and Music then opened inside that listing with
             * nothing above it: BACK left the browser instead of climbing to
             * the Music menu, and no press reached the menu at all.
             *
             * Level 0 is the Music menu, so that is the only table it can
             * mean. Forcing it also takes browser_db_load()'s fresh-root
             * path, which is where the shortcut base below is cleared. */
            if (tc->dirlevel == 0)
                tc->currtable = 0;
            /* A resume of its own is never a shortcut's jump, at any level. */
            browser_db_clear_shortcut_base();
            push_current_activity(ACTIVITY_DATABASEBROWSER);
        break;

#define TAGNAVI_CASE(n) case GO_TO_TAGNAVI_FIRST + (n):
        TAGNAVI_CASE(0)  TAGNAVI_CASE(1)  TAGNAVI_CASE(2)  TAGNAVI_CASE(3)
        TAGNAVI_CASE(4)  TAGNAVI_CASE(5)  TAGNAVI_CASE(6)  TAGNAVI_CASE(7)
        TAGNAVI_CASE(8)  TAGNAVI_CASE(9)  TAGNAVI_CASE(10) TAGNAVI_CASE(11)
        TAGNAVI_CASE(12) TAGNAVI_CASE(13) TAGNAVI_CASE(14) TAGNAVI_CASE(15)
        TAGNAVI_CASE(16) TAGNAVI_CASE(17) TAGNAVI_CASE(18) TAGNAVI_CASE(19)
#undef TAGNAVI_CASE
        {
            int slot = (intptr_t)param - GO_TO_TAGNAVI_FIRST;
            int target_tag;
            const char *target_menu;
            const unsigned char *target_name;

            if (!wait_for_tagcache_ready())
                return GO_TO_PREVIOUS;
            if (!browser_db_get_main_menu_row(slot, &target_tag, &target_menu,
                                              &target_name))
                return GO_TO_PREVIOUS; /* slot not backed by a real row */

            filter = SHOW_ID3DB;
            last_ft_dirlevel = tc->dirlevel;
            /* Jump straight into this row's branch of the Database's root
             * menu, independent of the plain Database entry's own
             * last_db_dirlevel/selection resume memory. Looked up by tag
             * identity (not position) so it survives tagnavi.config
             * reordering, and armed for browser_db_load() to apply on its next
             * fresh root load -- rockbox_browse() (called below)
             * unconditionally resets dirlevel/selected_item to 0 for any
             * ID3-DB entry, but NOT currtable/currextra, so those must be
             * forced back to the root here or browser_db_load() will just keep
             * showing whatever table was last displayed and the armed
             * shortcut below will never see a fresh root load to apply on.
             *
             * A row is one of two kinds: a tag browse, or a submenu such as
             * Playback History. A submenu has a unique id to arm with; a tag
             * browse is armed by its label, because the tag alone does not
             * identify it -- "Album" and "Recently Added" both browse
             * tag_album first, and arming by tag sent both to the former. */
            tc->currtable = 0;
            if (target_menu)
                browser_db_enter_menu_on_next_load(target_menu);
            else if (target_name)
                browser_db_enter_by_label_on_next_load(target_name);
            else
                browser_db_enter_by_tag_on_next_load(target_tag);
            push_current_activity(ACTIVITY_DATABASEBROWSER);
        }
        break;

        case GO_TO_ALBUM_COVERS_TRACKS:
            if (!wait_for_tagcache_ready())
                return GO_TO_PREVIOUS;
            filter = SHOW_ID3DB;
            last_ft_dirlevel = tc->dirlevel;
            /* browser_db_enter_album_tracks_on_next_load() was already armed by
             * album_covers.c's SELECT handler before it returned this code --
             * just need the standard ID3DB browse boilerplate here, same as
             * the TAGNAVI_CASE block above. */
            tc->currtable = 0;
            push_current_activity(ACTIVITY_DATABASEBROWSER);
        break;
    }

    struct browse_context browse = {
        .dirfilter = filter,
        .icon = Icon_NOICON,
        .root = folder,
    };

    /* The Search row at the top of the file browser's root. Offered only when
     * the directory cache can answer it: the names it searches are the cache's
     * and there is no second source, so without one the row would open a box
     * that finds nothing. Decided per browse rather than per directory load --
     * a cache that finishes building while the browser is open is picked up
     * the next time it is entered. */
    if ((intptr_t)param == GO_TO_FILEBROWSER && dircache_is_ready())
        browse.flags |= BROWSE_SEARCH_ROW;

    ret_val = rockbox_browse(&browse);

    if (ret_val == GO_TO_WPS
        || ret_val == GO_TO_PREVIOUS_MUSIC
        || ret_val == GO_TO_PLUGIN)
        pop_current_activity_without_refresh();
    else
        pop_current_activity();

    switch ((intptr_t)param)
    {
        case GO_TO_FILEBROWSER:
            if (!get_current_file(last_folder, MAX_PATH) ||
                (!strchr(&last_folder[1], '/') &&
                 global_settings.start_directory[1] != '\0'))
            {
                last_folder[0] = '/';
                last_folder[1] = '\0';
            }
        break;
        case GO_TO_DBBROWSER:
            last_db_dirlevel = tc->dirlevel;
            last_db_selection = tc->selected_item;
            tc->dirlevel = last_ft_dirlevel;
        break;

#define TAGNAVI_CASE(n) case GO_TO_TAGNAVI_FIRST + (n):
        TAGNAVI_CASE(0)  TAGNAVI_CASE(1)  TAGNAVI_CASE(2)  TAGNAVI_CASE(3)
        TAGNAVI_CASE(4)  TAGNAVI_CASE(5)  TAGNAVI_CASE(6)  TAGNAVI_CASE(7)
        TAGNAVI_CASE(8)  TAGNAVI_CASE(9)  TAGNAVI_CASE(10) TAGNAVI_CASE(11)
        TAGNAVI_CASE(12) TAGNAVI_CASE(13) TAGNAVI_CASE(14) TAGNAVI_CASE(15)
        TAGNAVI_CASE(16) TAGNAVI_CASE(17) TAGNAVI_CASE(18) TAGNAVI_CASE(19)
#undef TAGNAVI_CASE
            /* Leave the database browser as a fresh Music entry would find it.
             *
             * A shortcut enters at dirlevel 0 so one BACK leaves, which means
             * it never passes back through the Music menu -- and it was that
             * pass that used to put currtable right, browser_db_exit()
             * restoring it from table_history on the way up. Without it,
             * currtable was left pointing at the shortcut's own listing, and
             * the plain Music entry inherits whatever is there: choosing Music
             * opened the album list, and every later database screen was
             * confused in turn.
             *
             * Same repair, and the same reason, as GO_TO_ALBUM_COVERS_TRACKS
             * below. Clearing the resume position with it is deliberate:
             * dirlevel and currtable have to agree, so keeping a stored depth
             * against a reset table is what caused this in the first place.
             * These shortcuts used to leave the Music position alone; entering
             * at level 0 is what costs that, and a shortcut that quietly
             * breaks Music is the worse trade. */
            tc->dirlevel = last_ft_dirlevel;
            tc->currtable = 0;
            last_db_dirlevel = 0;
            last_db_selection = 0;
        break;

        case GO_TO_ALBUM_COVERS_TRACKS:
            tc->dirlevel = last_ft_dirlevel;
            /* enter_album_tracks_directly() left tc->currtable/currextra
             * pointing at the album's flat track listing (TABLE_NAVIBROWSE,
             * extra=1) -- unlike dirlevel, nothing here ever restored those.
             * A later, unrelated GO_TO_DBBROWSER ("Music") entry doesn't
             * reset currtable itself (it assumes whatever's left over from
             * the last real Music session), so it silently inherited this
             * album's stale view instead of the actual Music resume point --
             * observed as: start playing a track from here, open the plain
             * Music menu later, and it opens back into this exact album
             * even though dirlevel claims to be at the top, so BACK skips
             * straight past Artist/base-Music-menu to the main menu. Reset
             * currtable to 0 (forces browser_db_load()'s fresh-root path) and
             * clear the Music resume position so the next Music entry
             * always starts at the base Music menu rather than in an
             * inconsistent dirlevel/currtable state. */
            tc->currtable = 0;
            last_db_dirlevel = 0;
            last_db_selection = 0;
            /* Unlike the general tag-tree browse cases above, redirecting
             * GO_TO_ROOT here is safe: this entry point is only ever reached
             * from Album covers (never from the main Music menu), and
             * browser_db.c's enter_album_tracks_directly() never advances
             * dirlevel past 0 -- so there's no deeper history that a
             * redirect could hijack or trap the user in, unlike an earlier
             * version of this that tried the same translation for the
             * general two-hop Album browse (GO_TO_ROOT there is ambiguous
             * between "backed out" and "pressed MENU" *at any depth*, which
             * did trap users). Requested explicitly: BACK from an album's
             * track list should return straight to Album covers, not the
             * main menu.
             *
             * GO_TO_PREVIOUS, not GO_TO_ALBUM_COVERS directly: this case's
             * items[] entry is invoked through load_screen(), which already
             * tracks "the screen active before this one" itself (its local
             * old_previous, restored into the global last_screen exactly
             * when a callee returns GO_TO_PREVIOUS). Returning
             * GO_TO_ALBUM_COVERS directly bypassed that and left last_screen
             * pointing at GO_TO_ALBUM_COVERS_TRACKS itself (the screen that
             * "ran" from load_screen()'s point of view) -- a value that
             * only makes sense as a one-shot dispatch armed by Album
             * covers' own select handler, never as something to resume.
             * The next time Album covers' own quick-MENU (PF_QUIT) returned
             * GO_TO_PREVIOUS, root_menu()'s loop resolved *that* against
             * the poisoned last_screen and re-entered the plain database
             * browser instead of Album covers -- an endless bounce between
             * Album covers and the bare Music menu that never reached the
             * real main menu. GO_TO_PREVIOUS here instead lets
             * load_screen()'s own bookkeeping do this correctly: it was
             * called with old_previous already equal to GO_TO_ALBUM_COVERS
             * (Album covers is the only caller of this entry point), so
             * returning GO_TO_PREVIOUS resolves back to exactly that. */
            if (ret_val == GO_TO_ROOT)
                ret_val = GO_TO_PREVIOUS;

            /* Both the ways back to Album covers are GO_TO_PREVIOUS by now,
             * so anything else is the user leaving this track list for good --
             * playing a track, which goes to the WPS. Album covers' resume
             * signal describes the track list, and outranks the playing track
             * on the next open whenever that comes, so it goes with it. */
            if (ret_val != GO_TO_PREVIOUS)
                album_covers_cancel_resume();
        break;
    }
    return ret_val;
}

/* The generic Plugins browser (apps/menus/plugin_menu.c's old Games/Apps/
 * Demos picker, and this screen's own later flat-PLUGIN_DIR replacement)
 * has been removed entirely: lastfm_scrobbler is the only plugin meant to
 * be user-reachable now (see apps/menus/main_menu.c's lastfm_scrobbler_item),
 * everything else is invoked directly by whatever core feature needs it.
 * GO_TO_BROWSEPLUGINS itself can't be removed from the enum/items[] table
 * without risk, though: a device with an existing saved config could still
 * have global_status.last_screen (or, with "start in: previous screen",
 * global_settings.start_in_screen) pointing at this slot from before this
 * change, and an unlisted items[] entry silently zero-initializes to a
 * NULL function pointer -- load_screen() would call through it and crash.
 * This stub exists purely so that stale reference redirects safely to the
 * root menu instead. */
static int plugins_browser_removed(void* param)
{
    (void)param;
    return GO_TO_ROOT;
}

static int wpsscrn(void* param)
{
    int ret_val = GO_TO_PREVIOUS;
    int audstatus = audio_status();
    (void)param;
    push_current_activity(ACTIVITY_WPS);

    if (audstatus)
    {
        talk_shutup();
        ret_val = gui_wps_show();
    }
    else if (global_status.resume_index != -1)
    {
        DEBUGF("Resume index %d crc32 %lX offset %lX\n",
               global_status.resume_index,
               (unsigned long)global_status.resume_crc32,
               (unsigned long)global_status.resume_offset);
        if (playlist_resume() != -1)
        {
            playlist_resume_track(global_status.resume_index,
                global_status.resume_crc32,
                global_status.resume_elapsed,
                global_status.resume_offset);
            ret_val = gui_wps_show();
        }
    }
    else if (!file_exists(PLAYLIST_CONTROL_FILE))
        splash(HZ*2, ID2P(LANG_NOTHING_TO_RESUME));
    else if (yesno_pop(ID2P(LANG_REPLAY_FINISHED_PLAYLIST)) &&
             playlist_resume() != -1)
    {
        playlist_start(0, 0, 0);
        ret_val = gui_wps_show();
    }

    if (ret_val == GO_TO_PLAYLIST_VIEWER
        || ret_val == GO_TO_PLUGIN
        || ret_val == GO_TO_WPS
        || ret_val == GO_TO_PREVIOUS_MUSIC
        || ret_val == GO_TO_PREVIOUS_BROWSER
        || (ret_val == GO_TO_PREVIOUS
               && (last_screen == GO_TO_MAINMENU /* Settings */
                || last_screen == GO_TO_BROWSEPLUGINS
                || last_screen == GO_TO_SYSTEM_SCREEN
                || last_screen == GO_TO_PLAYLISTS_SCREEN)))
    {
        pop_current_activity_without_refresh();
    }
    else
        pop_current_activity();

    return ret_val;
}
/* Which of do_menu()'s return codes the root menu passes on, and which it
 * treats as "we are done with that menu". */
static int menu_result(int result)
{
    switch (result)
    {
        case GO_TO_PLUGIN:
        case GO_TO_PLAYLIST_VIEWER:
        case GO_TO_WPS:
        case GO_TO_PREVIOUS_MUSIC:
            return result;
        default:
            return GO_TO_ROOT;
    }
}

static int miscscrn(void * param)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex*)param;
    return menu_result(do_menu(menu, NULL, NULL, false));
}


static int playlist_view_catalog(void * param)
{
    (void)param;
    push_current_activity(ACTIVITY_PLAYLISTBROWSER);
    bool item_was_selected = catalog_view_playlists();

    if (item_was_selected)
    {
        pop_current_activity_without_refresh();
        return GO_TO_WPS;
    }
    pop_current_activity();
    return GO_TO_ROOT;
}

static int playlist_view(void * param)
{
    (void)param;
    int val;

    val = playlist_viewer();
    switch (val)
    {
        case PLAYLIST_VIEWER_MAINMENU:
        case PLAYLIST_VIEWER_USB:
            return GO_TO_ROOT;
        case PLAYLIST_VIEWER_OK:
            return GO_TO_PREVIOUS;
    }
    return GO_TO_PREVIOUS;
}

static int load_bmarks(void* param)
{
    (void)param;
    if(bookmark_mrb_load())
        return GO_TO_WPS;
    return GO_TO_PREVIOUS;
}

static int album_covers_scrn(void* param)
{
    (void)param;
    return album_covers(NULL);
}

static int db_search_scrn(void* param)
{
    (void)param;
    /* Nothing armed here. Every Search reaches db_search_run() through this
     * function, the Music and Audiobooks rows included, so arming anything
     * would overwrite what browser_db_enter() had just asked for. Covering
     * the whole library is what an unarmed search already does, and that is
     * exactly what the main menu's own row wants. */
    return db_search_run();
}

static int file_search_scrn(void* param)
{
    (void)param;
    return file_search_run();
}

static int featured_artists_scrn(void* param)
{
    (void)param;
    if (!wait_for_tagcache_ready())
        return GO_TO_PREVIOUS;
    return featured_artists_show();
}

static int featured_tracks_scrn(void* param)
{
    (void)param;
    if (!wait_for_tagcache_ready())
        return GO_TO_PREVIOUS;
    return featured_artists_run();
}

static int artist_portraits_scrn(void* param)
{
    (void)param;
    return artist_portraits(NULL);
}

/* The text viewer restores the reading position itself, so this only has to
 * find the document. text_viewer_last_document() has normally already hidden
 * the row when there is nothing to open, but it is asked when the menu is
 * drawn and the file can go away between then and the keypress -- hence the
 * same question again here, with something to say when the answer has
 * changed. */
static int lastdoc_scrn(void* param)
{
    char path[MAX_PATH];
    (void)param;

    if (!text_viewer_last_document(path, sizeof(path)) || !file_exists(path))
    {
        splash(HZ*2, ID2P(LANG_FILE_NOT_FOUND));
        return GO_TO_PREVIOUS;
    }
    return text_viewer(path);
}

/* Both read the album index, which is derived from the database -- so they
 * need the same wait the database screens do, not merely a database that
 * exists. */
static int album_charts_scrn(void* param)
{
    (void)param;
    if (!wait_for_tagcache_ready())
        return GO_TO_PREVIOUS;
    return album_charts_run();
}

static int random_album_scrn(void* param)
{
    (void)param;
    if (!wait_for_tagcache_ready())
        return GO_TO_PREVIOUS;
    return album_random();
}

/* No database wait for these two: their list comes from the file index, which
 * walks the disk itself and knows nothing about the tag database. */
static int documents_scrn(void* param)
{
    (void)param;
    return browser_flat(false);
}

static int images_scrn(void* param)
{
    (void)param;
    return browser_flat(true);
}

/* Spun.
 *
 * It has to be a root-menu screen to work at all now that a card can be
 * played: GO_TO_WPS is zero, and a menu action signals "I am finished" by
 * returning non-zero, so nothing below the root menu can ask for the WPS. */
static int playback_viewer_scrn(void* param)
{
    (void)param;
    return pv_row_screen();
}

/* These are all static const'd from apps/menus/ *.c
   so little hack so we can use them */
/* system_menu is not here: exported_settings.h declares it properly, and this
   list drops the const, which conflicts. */
extern struct menu_item_ex
        file_menu,
        tagcache_menu,
        music_menu,
        main_menu_,
        manage_settings,
        playlist_options,
        info_menu;

/* One tree. Settings Mode decides how much of it is shown rather than which of
 * two menus opens -- see the enum in settings.h.
 *
 * Opens on Sound Settings, with Search scrolled off the top rather than merely
 * unselected. Search is a way *into* the tree rather than a part of it, and a
 * settings screen whose first row is a search box reads as though that is
 * where the settings start. Same treatment the album lists give [Random] and
 * <All tracks>: one flick of the wheel brings it back. */
static int settings_scrn(void * param)
{
    (void)param;
    int selected = 1;
    menu_set_pending_top_item(1);
    return menu_result(do_menu(&main_menu_, &selected, NULL, false));
}

static const struct root_items items[] = {
    [GO_TO_FILEBROWSER] =   { browser, (void*)GO_TO_FILEBROWSER, &file_menu},
    [GO_TO_DBBROWSER] =     { browser, (void*)GO_TO_DBBROWSER, &tagcache_menu },
    [GO_TO_WPS] =           { wpsscrn, NULL, &playback_settings },
    [GO_TO_MAINMENU] =      { settings_scrn, NULL, &manage_settings },


    [GO_TO_RECENTBMARKS] =  { load_bmarks, NULL, &bookmark_settings_menu },
    [GO_TO_BROWSEPLUGINS] = { plugins_browser_removed, NULL, NULL },
    [GO_TO_PLAYLISTS_SCREEN] = { playlist_view_catalog, NULL,
                                                        &playlist_options },
    [GO_TO_PLAYLIST_VIEWER] = { playlist_view, NULL, &playlist_options },
    [GO_TO_SYSTEM_SCREEN] = { miscscrn, &info_menu, &system_menu },
    [GO_TO_SHORTCUTMENU] = { do_shortcut_menu, NULL, NULL },
    [GO_TO_ALBUM_COVERS] = { album_covers_scrn, NULL, NULL },
    [GO_TO_ARTIST_PORTRAITS] = { artist_portraits_scrn, NULL, NULL },
    [GO_TO_LASTDOC] = { lastdoc_scrn, NULL, &text_viewer_menu },
    [GO_TO_ALBUM_CHARTS] = { album_charts_scrn, NULL, &tagcache_menu },
    [GO_TO_RANDOM_ALBUM] = { random_album_scrn, NULL, &tagcache_menu },
    [GO_TO_DB_SEARCH] = { db_search_scrn, NULL, &tagcache_menu },
    [GO_TO_FILE_SEARCH] = { file_search_scrn, NULL, &file_menu },
    [GO_TO_FEATURED_ARTISTS] = { featured_artists_scrn, NULL, &music_menu },
    [GO_TO_FEATURED_TRACKS] = { featured_tracks_scrn, NULL, &music_menu },
    [GO_TO_DOCUMENTS] = { documents_scrn, NULL, &text_viewer_menu },
    [GO_TO_IMAGES] = { images_scrn, NULL, NULL },
    [GO_TO_SPUN] = { playback_viewer_scrn, NULL, NULL },
    [GO_TO_ALBUM_COVERS_TRACKS] = { browser, (void*)GO_TO_ALBUM_COVERS_TRACKS, &tagcache_menu },
/* One reserved slot per tagnavi.config root-menu tag-browse row (see
 * GO_TO_TAGNAVI_FIRST in root_menu.h); all share the same dispatch function
 * and only differ in which slot index they carry as param. */
#define TAGNAVI_ITEMS_ENTRY(n) \
    [GO_TO_TAGNAVI_FIRST + (n)] = \
        { browser, (void*)(GO_TO_TAGNAVI_FIRST + (n)), &tagcache_menu }
    TAGNAVI_ITEMS_ENTRY(0),  TAGNAVI_ITEMS_ENTRY(1),  TAGNAVI_ITEMS_ENTRY(2),
    TAGNAVI_ITEMS_ENTRY(3),  TAGNAVI_ITEMS_ENTRY(4),  TAGNAVI_ITEMS_ENTRY(5),
    TAGNAVI_ITEMS_ENTRY(6),  TAGNAVI_ITEMS_ENTRY(7),  TAGNAVI_ITEMS_ENTRY(8),
    TAGNAVI_ITEMS_ENTRY(9),  TAGNAVI_ITEMS_ENTRY(10), TAGNAVI_ITEMS_ENTRY(11),
    TAGNAVI_ITEMS_ENTRY(12), TAGNAVI_ITEMS_ENTRY(13), TAGNAVI_ITEMS_ENTRY(14),
    TAGNAVI_ITEMS_ENTRY(15), TAGNAVI_ITEMS_ENTRY(16), TAGNAVI_ITEMS_ENTRY(17),
    TAGNAVI_ITEMS_ENTRY(18), TAGNAVI_ITEMS_ENTRY(19),
#undef TAGNAVI_ITEMS_ENTRY

};
#define NUM_ITEMS (int)(sizeof(items)/sizeof(*items))

static int item_callback(int action,
                         const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list);

/* Hide the Shortcuts root entry entirely when no shortcuts are defined -- a
 * menu item whose callback returns ACTION_EXIT_MENUITEM for the count request is
 * left out of the list (see init_menu_lists() in menu.c). */
static int shortcut_menu_callback(int action,
                                  const struct menu_item_ex *this_item,
                                  struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && !shortcuts_available())
        return ACTION_EXIT_MENUITEM;
    return action;
}

/* Hidden entirely while the database is not held in RAM. Searching off the
 * ramcache means a seek and a read per tag entry, which on a disk is not slow
 * but unusable -- db_search_run() refuses in that case, so without this the
 * row exists only to say no. Same mechanism as the Shortcuts row above.
 *
 * The setting rather than tagcache_is_in_ram(): the row's presence should not
 * flicker with whether the cache happens to be loaded yet at this moment. */
static int db_search_callback(int action,
                              const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM
        && global_settings.tagcache_ram == TAGCACHE_RAM_OFF)
        return ACTION_EXIT_MENUITEM;
    return action;
}

MENUITEM_RETURNVALUE(shortcut_menu, ID2P(LANG_SHORTCUTS), GO_TO_SHORTCUTMENU,
                        shortcut_menu_callback, Icon_Bookmark);

MENUITEM_RETURNVALUE(file_browser, ID2P(LANG_DIR_BROWSER), GO_TO_FILEBROWSER,
                        NULL, Icon_file_view_menu);
MENUITEM_RETURNVALUE(db_browser, ID2P(LANG_MUSIC_BROWSER), GO_TO_DBBROWSER,
                        NULL, Icon_Audio);
MENUITEM_RETURNVALUE(album_covers_item, ID2P(LANG_ALBUM_COVERS),
                     GO_TO_ALBUM_COVERS,
                        NULL, Icon_Folder);
MENUITEM_RETURNVALUE(artist_portraits_item, ID2P(LANG_ARTIST_PORTRAITS), GO_TO_ARTIST_PORTRAITS,
                        NULL, Icon_Folder);
MENUITEM_RETURNVALUE(continue_reading, ID2P(LANG_CONTINUE_READING), GO_TO_LASTDOC,
                        item_callback, Icon_Font);
MENUITEM_RETURNVALUE(random_album_item, ID2P(LANG_RANDOM_ALBUM), GO_TO_RANDOM_ALBUM,
                        NULL, Icon_Playlist);
MENUITEM_RETURNVALUE(db_search_item, ID2P(LANG_DB_SEARCH), GO_TO_DB_SEARCH,
                        db_search_callback, Icon_Audio);
MENUITEM_RETURNVALUE(documents_item, ID2P(LANG_DOCUMENTS), GO_TO_DOCUMENTS,
                        NULL, Icon_Font);
MENUITEM_RETURNVALUE(images_item, ID2P(LANG_IMAGES), GO_TO_IMAGES,
                        NULL, Icon_Font);
MENUITEM_RETURNVALUE(spun_item, ID2P(LANG_SPUN), GO_TO_SPUN,
                        NULL, Icon_Audio);

/* Dynamic-text menu items for the reserved GO_TO_TAGNAVI_FIRST.. slots: the
 * displayed name/voice for slot N is fetched fresh from browser_db's parsed
 * "main" menu every time it's drawn (via list_get_name_data carrying the
 * slot index), rather than a compile-time string -- so these track
 * tagnavi.config's actual row names/order without needing a rebuild. A slot
 * with no backing row (index >= browser_db_get_main_menu_tag_row_count())
 * safely renders as blank rather than returning NULL. */
static char *tagnavi_item_get_name(int selected_item, void *data,
                                    char *buffer, size_t buffer_len)
{
    (void)selected_item;
    int index = (int)(intptr_t)data;
    const unsigned char *name;

    if (!browser_db_get_main_menu_row(index, NULL, NULL, &name))
    {
        buffer[0] = '\0';
        return buffer;
    }
    strlcpy(buffer, P2STR((unsigned char *)name), buffer_len);
    return buffer;
}

static int tagnavi_item_speak(int selected_item, void *data)
{
    (void)selected_item;
    int index = (int)(intptr_t)data;
    const unsigned char *name;

    if (browser_db_get_main_menu_row(index, NULL, NULL, &name))
    {
        int id = P2ID(name);
        if (id != -1)
            talk_id(id, false);
    }
    return 0;
}

#define TAGNAVI_DECL(n) \
    MENUITEM_RETURNVALUE_DYNTEXT(tagnavi_item_##n, GO_TO_TAGNAVI_FIRST + (n), \
        NULL, tagnavi_item_get_name, tagnavi_item_speak, \
        (void*)(intptr_t)(n), Icon_Audio)
TAGNAVI_DECL(0)
TAGNAVI_DECL(1)
TAGNAVI_DECL(2)
TAGNAVI_DECL(3)
TAGNAVI_DECL(4)
TAGNAVI_DECL(5)
TAGNAVI_DECL(6)
TAGNAVI_DECL(7)
TAGNAVI_DECL(8)
TAGNAVI_DECL(9)
TAGNAVI_DECL(10)
TAGNAVI_DECL(11)
TAGNAVI_DECL(12)
TAGNAVI_DECL(13)
TAGNAVI_DECL(14)
TAGNAVI_DECL(15)
TAGNAVI_DECL(16)
TAGNAVI_DECL(17)
TAGNAVI_DECL(18)
TAGNAVI_DECL(19)
#undef TAGNAVI_DECL
static char *get_wps_item_name(int selected_item, void * data,
                               char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    if (audio_status())
        return ID2P(LANG_NOW_PLAYING);
    return ID2P(LANG_RESUME_PLAYBACK);
}
MENUITEM_RETURNVALUE_DYNTEXT(wps_item, GO_TO_WPS, item_callback,
                                get_wps_item_name,
                                NULL, NULL, Icon_Playback_menu);
MENUITEM_RETURNVALUE(menu_, ID2P(LANG_SETTINGS), GO_TO_MAINMENU,
                        NULL, Icon_Submenu_Entered);
MENUITEM_RETURNVALUE(bookmarks, ID2P(LANG_BOOKMARK_MENU_RECENT_BOOKMARKS),
                        GO_TO_RECENTBMARKS,  item_callback,
                        Icon_Bookmark);
MENUITEM_RETURNVALUE(playlists, ID2P(LANG_PLAYLISTS), GO_TO_PLAYLISTS_SCREEN,
                     NULL, Icon_Playlist);
/* The dynamic playlist -- what playing an album built, not a saved .m3u. Named
 * for it: "queue" means a single track carrying PLAYLIST_QUEUED, which the
 * viewer marks with a "+" in this very list. */
MENUITEM_RETURNVALUE(current_playlist, ID2P(LANG_CURRENT_PLAYLIST),
                     GO_TO_PLAYLIST_VIEWER, NULL, Icon_Playlist);
MENUITEM_RETURNVALUE(system_menu_, ID2P(LANG_SYSTEM), GO_TO_SYSTEM_SCREEN,
                     NULL, Icon_System_menu);

struct menu_item_ex root_menu_;
static struct menu_callback_with_desc root_menu_desc = {
        item_callback, ID2P(LANG_ROCKBOX_TITLE), Icon_Rockbox };

/* These strings are the tokens written into the saved main-menu order, so they
 * are an on-disk format and not free to rename with the code. "pictureflow" is
 * the screen now called Album covers; changing it would drop the row from
 * every config.cfg already out there. */
static struct menu_table menu_table[] = {
    { "pictureflow", &album_covers_item },
    { "artistportraits", &artist_portraits_item },
    { "database", &db_browser },
    { "files", &file_browser },
    { "wps", &wps_item },
    { "reading", &continue_reading },
    { "random", &random_album_item },
    { "search", &db_search_item },
    { "docs", &documents_item },
    { "images", &images_item },
    { "spun", &spun_item },
    { "playlists", &playlists },
    { "currentplaylist", &current_playlist },
    { "shortcuts", &shortcut_menu },
    { "settings", &menu_ },
    { "system_menu", &system_menu_ },
    /* Kept last: root_menu_get_options()/root_menu_set_default() trim the
     * *tail* of this array down to however many of these are actually backed
     * by a tagnavi.config row (browser_db_get_main_menu_tag_row_count()), so any
     * unbacked slots must be the last entries here, not mixed in earlier. */
#define TAGNAVI_TABLE_ENTRY(n) { "tagnavi" #n, &tagnavi_item_##n }
    TAGNAVI_TABLE_ENTRY(0),  TAGNAVI_TABLE_ENTRY(1),  TAGNAVI_TABLE_ENTRY(2),
    TAGNAVI_TABLE_ENTRY(3),  TAGNAVI_TABLE_ENTRY(4),  TAGNAVI_TABLE_ENTRY(5),
    TAGNAVI_TABLE_ENTRY(6),  TAGNAVI_TABLE_ENTRY(7),  TAGNAVI_TABLE_ENTRY(8),
    TAGNAVI_TABLE_ENTRY(9),  TAGNAVI_TABLE_ENTRY(10), TAGNAVI_TABLE_ENTRY(11),
    TAGNAVI_TABLE_ENTRY(12), TAGNAVI_TABLE_ENTRY(13), TAGNAVI_TABLE_ENTRY(14),
    TAGNAVI_TABLE_ENTRY(15), TAGNAVI_TABLE_ENTRY(16), TAGNAVI_TABLE_ENTRY(17),
    TAGNAVI_TABLE_ENTRY(18), TAGNAVI_TABLE_ENTRY(19),
#undef TAGNAVI_TABLE_ENTRY
};
#define MAX_MENU_ITEMS (sizeof(menu_table) / sizeof(struct menu_table))
static struct menu_item_ex *root_menu__[MAX_MENU_ITEMS];

/* Enforces a fixed canonical order on the main menu, requested explicitly:
 * Resume/Now Playing, Music, Album covers, [tagnavi rows, in whatever order
 * they're already in], Playlists, Files, Plugins, Shortcuts, Settings,
 * System. Called as the final step of anything that (re)builds
 * root_menu__[] (root_menu_set_default(), root_menu_load_from_cfg(), and
 * root_menu_fixup_tagnavi_slots(), which appends newly-available tagnavi
 * slots to the *tail* and would otherwise land them after Playlists/Files/
 * etc) rather than baking the order into menu_table[] itself: tagnavi
 * slots must stay the *last* entries in menu_table[] for
 * root_menu_active_count() to correctly trim unbacked ones from the end
 * (see the comment on menu_table[] above), which rules out just declaring
 * menu_table[] in the desired final order.
 *
 * Every item not named in before_tagnavi/after_tagnavi falls into the
 * middle "tagnavi" group by construction (there's nothing else it could
 * be), preserving whatever relative order it already had -- this doesn't
 * hardcode "tagnavi0..19" by name, so it can't drift out of sync with
 * however many rows actually exist. Missing items (e.g. HAVE_TAGCACHE off,
 * or the user disabled something) are simply skipped, not left as gaps. */
static void root_menu_apply_canonical_order(void)
{
    static const struct menu_item_ex * const before_tagnavi[] = {
        &wps_item, &continue_reading,
        &db_browser, &album_covers_item, &artist_portraits_item,
        &random_album_item,
    };
    static const struct menu_item_ex * const after_tagnavi[] = {
        &current_playlist, &playlists,
        &documents_item, &images_item, &spun_item,
        &file_browser, &shortcut_menu,
        &menu_, &system_menu_,
    };
    struct menu_item_ex *reordered[MAX_MENU_ITEMS];
    unsigned count = MENU_GET_COUNT(root_menu_.flags);
    unsigned out = 0;
    unsigned i, k;

    for (k = 0; k < ARRAYLEN(before_tagnavi); k++)
        for (i = 0; i < count; i++)
            if (root_menu__[i] == before_tagnavi[k])
            {
                reordered[out++] = root_menu__[i];
                break;
            }

    for (i = 0; i < count; i++)
    {
        bool matched = false;
        for (k = 0; !matched && k < ARRAYLEN(before_tagnavi); k++)
            matched = (root_menu__[i] == before_tagnavi[k]);
        for (k = 0; !matched && k < ARRAYLEN(after_tagnavi); k++)
            matched = (root_menu__[i] == after_tagnavi[k]);
        if (!matched)
            reordered[out++] = root_menu__[i];
    }

    for (k = 0; k < ARRAYLEN(after_tagnavi); k++)
        for (i = 0; i < count; i++)
            if (root_menu__[i] == after_tagnavi[k])
            {
                reordered[out++] = root_menu__[i];
                break;
            }

    memcpy(root_menu__, reordered, out * sizeof(root_menu__[0]));

    /* The count follows what was actually written. It cannot normally change
     * -- every item lands in exactly one of the three groups -- but if it ever
     * did, leaving the old count standing would draw whatever stale pointer
     * was left in the tail as if it were a real row. */
    if (out != count)
        root_menu_.flags = (root_menu_.flags & ~(MENU_COUNT_MASK << MENU_COUNT_SHIFT))
                            | MENU_ITEM_COUNT(out);
}

/* Display-only counterpart to root_menu__[] -- see
 * root_menu_build_display_list()'s comment for why this has to be a
 * separate array rather than a temporary edit of root_menu__[] itself. */
static struct menu_item_ex *root_menu_display__[MAX_MENU_ITEMS];

/* Resume Playback/Now Playing must stay reachable from the main menu
 * while something is genuinely playing, even if the user toggled it off
 * in Customize Main Menu at a moment nothing was playing (see
 * main_menu_config.c's locking of that item while audio_status() is
 * true, which stops it being toggled off *while* playing, but can't do
 * anything about a user who disabled it earlier and then started
 * playback some other way, e.g. resuming via a bookmark).
 *
 * Builds the list into root_menu_display__[], never root_menu__[] itself:
 * whether something happens to be playing at the exact moment
 * settings_save() fires must not affect what actually gets persisted (see
 * root_menu_write_to_cfg(), which reads root_menu__[] directly), so this
 * has to be entirely display-only, rebuilt fresh every time the root menu
 * is about to be shown rather than baked into the persisted array even
 * temporarily.
 *
 * *inserted_at_front is set if this actually added the item (it wasn't
 * already present) -- the caller needs that to keep root_menu()'s own
 * "selected" index (which indexes into the *persisted* root_menu__[]
 * layout) correct against the now-possibly-shifted display list. Returns
 * the resulting item count. */
static unsigned root_menu_build_display_list(bool *inserted_at_front)
{
    unsigned count = MENU_GET_COUNT(root_menu_.flags);
    unsigned i;
    bool wps_present = false;

    *inserted_at_front = false;
    if (count > MAX_MENU_ITEMS)
        count = MAX_MENU_ITEMS;

    /* Must finish copying every entry before returning. Trap: an early return
     * from inside this loop -- say as soon as wps_item is spotted, which is
     * first in canonical order -- leaves every later slot stale in
     * root_menu_display__[] while the count still reports them valid, and
     * do_menu() then dereferences them. */
    for (i = 0; i < count; i++)
    {
        root_menu_display__[i] = root_menu__[i];
        if (root_menu__[i] == &wps_item)
            wps_present = true;
    }

    if (wps_present || !audio_status() || count >= MAX_MENU_ITEMS)
        return count;

    /* Insert at the front, matching Resume/Now Playing's canonical
     * position (see root_menu_apply_canonical_order()). */
    memmove(&root_menu_display__[1], &root_menu_display__[0],
            count * sizeof(root_menu_display__[0]));
    root_menu_display__[0] = (struct menu_item_ex *)&wps_item;
    *inserted_at_front = true;
    return count + 1;
}

/* Of MAX_MENU_ITEMS, how many are actually usable right now -- hides any
 * trailing GO_TO_TAGNAVI_FIRST.. slots beyond browser_db's real row count (see
 * the comment on menu_table[] above) from both the Customize Main Menu
 * screen and the default-enabled root menu. Safe to call at any time: with
 * no tagcache/browser_db, or before browser_db_init() has parsed tagnavi.config,
 * browser_db_get_main_menu_tag_row_count() simply returns 0 and every reserved
 * slot is hidden until real data is available. */
static int root_menu_active_count(void)
{
    int real = browser_db_get_main_menu_tag_row_count();
    if (real > TAGNAVI_MAIN_MENU_SLOTS)
        real = TAGNAVI_MAIN_MENU_SLOTS;
    return MAX_MENU_ITEMS - (TAGNAVI_MAIN_MENU_SLOTS - real);
}

/* settings_load() (via root_menu_set_default()/root_menu_load_from_cfg(),
 * both driven off the root_menu_customized CUSTOM_SETTING) runs before
 * browser_db_init() has parsed tagnavi.config, so root_menu_active_count()
 * would have seen zero real tagnavi rows at that point and any tagnavi
 * item the user's *saved* configuration explicitly wanted got silently
 * dropped by root_menu_load_from_cfg()'s own matching loop (it can only
 * match against menu_table[] entries root_menu_active_count() already
 * knows about). Called once from root_menu()'s first entry, well after
 * the database browser is guaranteed ready, this re-adds any now-available tagnavi slot
 * the saved config wanted but couldn't find yet. No-op if it was already
 * there (i.e. the database browser happened to be ready by settings-load time after
 * all).
 *
 * Skipped entirely on a still-default configuration: tagnavi rows start
 * disabled by design now (see root_menu_set_default()), so there's nothing
 * to "restore" for a user who never customized anything -- this isn't an
 * init-order casualty to correct, it's the actual desired state. Only
 * matters once root_menu_customized is true, i.e. there's a real saved
 * preference this init-order race could have clipped. */
/* Tagnavi slots the saved configuration named but that could not be matched
 * when it was read. One bit per slot; TAGNAVI_MAIN_MENU_SLOTS is 20, so a
 * uint32_t covers it with room to spare. */
static uint32_t wanted_tagnavi_slots;

static void root_menu_fixup_tagnavi_slots(void)
{
    unsigned count = MENU_GET_COUNT(root_menu_.flags);
    int real = browser_db_get_main_menu_tag_row_count();
    int tagnavi_start = MAX_MENU_ITEMS - TAGNAVI_MAIN_MENU_SLOTS;
    int n;

    if (!global_settings.root_menu_customized)
        return;

    if (real > TAGNAVI_MAIN_MENU_SLOTS)
        real = TAGNAVI_MAIN_MENU_SLOTS;

    for (n = 0; n < real; n++)
    {
        struct menu_item_ex *item =
            (struct menu_item_ex *)menu_table[tagnavi_start + n].item;
        unsigned i;
        bool present = false;

        /* Only slots the saved configuration actually named -- which is not
         * the same as every slot backed by a real row. Adding those instead
         * reads to the user as "turning one row on turned them all on": the
         * first customization sets root_menu_customized, and the next boot
         * adds the lot. wanted_tagnavi_slots is what records the difference. */
        if (!(wanted_tagnavi_slots & (1u << n)))
            continue;

        for (i = 0; i < count; i++)
        {
            if (root_menu__[i] == item)
            {
                present = true;
                break;
            }
        }
        if (!present && count < MAX_MENU_ITEMS)
            root_menu__[count++] = item;
    }

    /* Consumed: this runs once, and a later save writes the real list. */
    wanted_tagnavi_slots = 0;

    if (count != MENU_GET_COUNT(root_menu_.flags))
        root_menu_.flags = (root_menu_.flags & ~(MENU_COUNT_MASK << MENU_COUNT_SHIFT))
                            | MENU_ITEM_COUNT(count);

    root_menu_apply_canonical_order();
}

/* Segregate Audiobooks owns the Audiobooks row: one setting, one feature.
 * Turning the setting on puts the row in the main menu and turning it off
 * takes it away, so nobody has to visit Customize Main Menu to finish
 * switching the thing on.
 *
 * Customize Main Menu still lists it, which is deliberate -- that screen is
 * how a row is *positioned*, and taking it away would mean the one row nobody
 * could move. Toggling the setting re-asserts whichever state it wants.
 *
 * Safe before tagnavi.config is parsed: the slot comes back -1 and this does
 * nothing, which is why root_menu() calls it again on first entry. */
void root_menu_set_audiobooks_row(bool on)
{
    int slot = browser_db_spoken_main_menu_slot();
    struct menu_item_ex *item;
    unsigned count = MENU_GET_COUNT(root_menu_.flags);
    unsigned i;

    if (slot < 0 || slot >= TAGNAVI_MAIN_MENU_SLOTS)
        return;

    item = (struct menu_item_ex *)
        menu_table[MAX_MENU_ITEMS - TAGNAVI_MAIN_MENU_SLOTS + slot].item;

    for (i = 0; i < count; i++)
        if (root_menu__[i] == item)
            break;

    if (on)
    {
        if (i < count || count >= MAX_MENU_ITEMS)
            return;                     /* already there, or no room */
        root_menu__[count++] = item;
    }
    else
    {
        if (i >= count)
            return;                     /* already absent */
        memmove(&root_menu__[i], &root_menu__[i + 1],
                (count - i - 1) * sizeof(root_menu__[0]));
        count--;
    }

    root_menu_.flags = (root_menu_.flags & ~(MENU_COUNT_MASK << MENU_COUNT_SHIFT))
                        | MENU_ITEM_COUNT(count);

    root_menu_apply_canonical_order();
}

struct menu_table *root_menu_get_options(int *nb_options)
{
    *nb_options = root_menu_active_count();

    return menu_table;
}

/* Record "tagnaviN" from a config token that matched nothing. Anything else
 * unmatched is a key from an older build, or a typo, and is simply dropped as
 * it always was. */
static void note_wanted_tagnavi_slot(const char *key)
{
    int slot;

    if (strncmp(key, "tagnavi", 7) != 0 || !isdigit((unsigned char)key[7]))
        return;

    slot = atoi(key + 7);
    if (slot >= 0 && slot < TAGNAVI_MAIN_MENU_SLOTS)
        wanted_tagnavi_slots |= 1u << slot;
}

void root_menu_load_from_cfg(void* setting, char *value)
{
    char *next = value, *start, *end;
    unsigned int menu_item_count = 0, i;
    bool main_menu_added = false;

    if (*value == '-')
    {
        root_menu_set_default(setting, NULL);
        return;
    }
    root_menu_.flags = MENU_HAS_DESC | MT_MENU;
    root_menu_.submenus = (const struct menu_item_ex **)&root_menu__;
    root_menu_.callback_and_desc = &root_menu_desc;

    while (next && menu_item_count < MAX_MENU_ITEMS)
    {
        start = next;
        next = strchr(next, ',');
        if (next)
        {
            *next = '\0';
            next++;
        }
        start = skip_whitespace(start);
        if ((end = strchr(start, ' ')))
            *end = '\0';
        bool matched = false;
        for (i=0; i<(unsigned)root_menu_active_count(); i++)
        {
            if (*start && !strcmp(start, menu_table[i].string))
            {
                unsigned k;
                matched = true;

                /* Once only, however many times the key is listed. Nothing
                 * downstream removes a repeat: it goes into root_menu__ twice,
                 * root_menu_write_to_cfg() writes it out twice from there, and
                 * the next load reads it back twice -- so a configuration that
                 * ever gained a duplicate key kept it, showing the row twice in
                 * Customize Main Menu and on the main menu itself. Skipping it
                 * here also repairs such a configuration on its next save. */
                for (k = 0; k < menu_item_count; k++)
                {
                    if (root_menu__[k] == menu_table[i].item)
                        break;
                }
                if (k < menu_item_count)
                    break;

                root_menu__[menu_item_count++] = (struct menu_item_ex *)menu_table[i].item;
                if (menu_table[i].item == &menu_)
                    main_menu_added = true;
                break;
            }
        }

        /* A tagnavi slot this configuration asked for that could not be
         * matched, because the database browser has not parsed
         * tagnavi.config yet and root_menu_active_count() therefore still
         * hides every slot. Remember exactly which ones, so
         * root_menu_fixup_tagnavi_slots() can restore those and only those
         * once the real row count is known. */
        if (!matched && *start)
            note_wanted_tagnavi_slot(start);
    }
    if (!main_menu_added)
        root_menu__[menu_item_count++] = (struct menu_item_ex *)&menu_;
    root_menu_.flags |= MENU_ITEM_COUNT(menu_item_count);
    root_menu_apply_canonical_order();
    *(bool*)setting = true;
}

char* root_menu_write_to_cfg(void* setting, char*buf, int buf_len)
{
    (void)setting;
    unsigned i, written, j;
    for (i = 0; i < MENU_GET_COUNT(root_menu_.flags); i++)
    {
        /* Stop rather than let buf_len go negative: snprintf()'s return
         * value is how much it *would* have written, uncapped by the
         * buffer size, so on truncation this can exceed buf_len. Letting
         * that make buf_len negative and feeding it back into the next
         * snprintf() call (int -> size_t, wrapping around to a huge
         * value) was a real out-of-bounds write once enough items were
         * enabled to overrun the caller's buffer, not just a truncated
         * string -- was masked before now only by callers happening to
         * pass a buffer big enough that this never triggered. */
        if (buf_len <= 0)
            break;
        for (j=0; j<MAX_MENU_ITEMS; j++)
        {
            if (menu_table[j].item == root_menu__[i])
            {
                written = snprintf(buf, buf_len, "%s, ", menu_table[j].string);
                if ((int)written >= buf_len)
                    written = buf_len - 1;
                buf_len -= written;
                buf += written;
                break;
            }
        }
    }
    return buf;
}

void root_menu_set_default(void* setting, void* defaultval)
{
    unsigned i;
    int active_count = root_menu_active_count();
    int tagnavi_start = MAX_MENU_ITEMS - TAGNAVI_MAIN_MENU_SLOTS;
    unsigned out = 0;
    (void)defaultval;

    root_menu_.flags = MENU_HAS_DESC | MT_MENU;
    root_menu_.submenus = (const struct menu_item_ex **)&root_menu__;
    root_menu_.callback_and_desc = &root_menu_desc;

    for (i=0; i<(unsigned)active_count; i++)
    {
        /* Tagnavi rows start disabled on a fresh/default configuration --
         * opt in via Customize Main Menu, rather than every "main"
         * tagnavi.config row automatically cluttering the menu on first
         * boot. Requested explicitly. They're still counted by
         * root_menu_active_count() (so main_menu_config.c's "not yet
         * enabled" section can list them as available to turn on), just
         * not included here. */
        if ((int)i >= tagnavi_start)
            continue;
        /* Likewise off to begin with. Random album and Search have homes in
         * the Music menu, so leaving them out of the root menu costs nothing
         * but a level of nesting -- and a root entry that plays something the
         * moment it is chosen is not one to hand someone unasked. Documents
         * and Images are off because most players hold neither; on a device
         * with no documents on it they would be two rows that only ever say
         * there is nothing to show.
         *
         * That home is a setting now: Library > Music > Edit Music Menu
         * turns any Music row off, and nothing here knows. Turn off
         * Random album there without adding it here and the feature has
         * nowhere left to be reached from -- the user's to make, but not one
         * either screen warns about.
         *
         * Current playlist is off for a different reason: it is already the
         * first entry of the Playlists context menu ("View Current Playlist"),
         * and the WPS reaches it too, so nothing is stranded by leaving the
         * root row out -- unlike the four above, this one cannot be made
         * unreachable from here. */
        if (menu_table[i].item == &random_album_item
            || menu_table[i].item == &db_search_item
            || menu_table[i].item == &documents_item
            || menu_table[i].item == &images_item
            || menu_table[i].item == &current_playlist)
            continue;
        root_menu__[out++] = (struct menu_item_ex *)menu_table[i].item;
    }
    root_menu_.flags |= MENU_ITEM_COUNT(out);
    root_menu_apply_canonical_order();
    *(bool*)setting = false;
}

bool root_menu_is_changed(void* setting, void* defaultval)
{
    (void)defaultval;
    return *(bool*)setting;
}

static int item_callback(int action,
                         const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list)
{
    (void)this_list;
    switch (action)
    {
        case ACTION_TREE_STOP:
            return ACTION_REDRAW;
        case ACTION_REQUEST_MENUITEM:
            if (this_item == &bookmarks)
            {
                if (global_settings.usemrb == 0)
                    return ACTION_EXIT_MENUITEM;
            }
            else if (this_item == &wps_item)
            {
                /* Hide the row when choosing it could only report "nothing to
                 * resume". These are wpsscrn()'s own three ways of producing a
                 * screen -- playing already, a stored resume point, or a
                 * finished playlist it can offer to replay -- so the two must
                 * be kept in step: if they drift, the row either lies or hides
                 * something that would have worked. */
                if (!audio_status() &&
                    global_status.resume_index == -1 &&
                    !file_exists(PLAYLIST_CONTROL_FILE))
                    return ACTION_EXIT_MENUITEM;
            }
            else if (this_item == &continue_reading)
            {
                /* Hide the row unless there is a document to reopen. Costs
                 * the same two file lookups the row above already spends,
                 * and on the same terms: this runs while the menu is being
                 * drawn for someone who just pressed a button, not on a
                 * timer, so it is not disk traffic that can hold off
                 * spindown or idle poweroff. */
                char path[MAX_PATH];
                if (!text_viewer_last_document(path, sizeof(path)) ||
                    !file_exists(path))
                    return ACTION_EXIT_MENUITEM;
            }
        break;
    }
    return action;
}

static int get_selection(int last_screen)
{
    int i;
    /* Only the populated prefix of root_menu__[] is valid -- MAX_MENU_ITEMS
     * (ARRAYLEN(root_menu__)) now includes the reserved-but-often-unused
     * tagnavi slots (see menu_table[]'s comment), and entries beyond the
     * current count are stale/uninitialized, not NULL-safe zeroed slots to
     * skip past silently. Scanning past the real count risks dereferencing
     * garbage on a target where address 0 is mapped, ordinary DRAM rather
     * than a faulting NULL page. */
    int len = MENU_GET_COUNT(root_menu_.flags);
    for(i=0; i < len; i++)
    {
        if (((root_menu__[i]->flags&MENU_TYPE_MASK) == MT_RETURN_VALUE) &&
            (root_menu__[i]->value == last_screen))
        {
            return i;
        }
    }
    return 0;
}

static inline int load_screen(int screen)
{
    /* set the global_status.last_screen before entering,
        if we dont we will always return to the wrong screen on boot */
    int old_previous = last_screen;
    int ret_val;
    enum current_activity activity = ACTIVITY_UNKNOWN;
    if (screen <= GO_TO_ROOT)
        return screen;
    if (screen == old_previous)
        old_previous = GO_TO_ROOT;
    global_status.last_screen = screen;
    status_save(false);

    if (screen == GO_TO_BROWSEPLUGINS)
        activity = ACTIVITY_PLUGINBROWSER;
    else if (screen == GO_TO_MAINMENU)
        activity = ACTIVITY_SETTINGS;
    else if (screen == GO_TO_SYSTEM_SCREEN)
        activity =  ACTIVITY_SYSTEMSCREEN;
    /* Deliberately NOT handling GO_TO_ALBUM_COVERS here: album_covers()
     * itself is also reachable directly from apps/gui/wps.c (the "coverflow"
     * WPS select-action and the custom STOP-opens-coverflow behavior),
     * bypassing this dispatcher entirely, so it pushes/pops
     * ACTIVITY_ALBUMCOVERS itself to cover both entry paths. See
     * screens/covers/album_covers.c's album_covers(). */

    if (activity != ACTIVITY_UNKNOWN)
        push_current_activity(activity);

    ret_val = items[screen].function(items[screen].param);

    if (activity != ACTIVITY_UNKNOWN)
    {
        if (ret_val == GO_TO_PLUGIN
            || ret_val == GO_TO_WPS
            || ret_val == GO_TO_PREVIOUS_MUSIC
            || ret_val == GO_TO_PREVIOUS_BROWSER
            || ret_val == GO_TO_FILEBROWSER)
        {
            pop_current_activity_without_refresh();
        }
        else
            pop_current_activity();
    }

    last_screen = screen;
    if (ret_val == GO_TO_PREVIOUS)
        last_screen = old_previous;
    return ret_val;
}

static int load_context_screen(int selection)
{
    const struct menu_item_ex *context_menu = NULL;
    int retval = GO_TO_PREVIOUS;
    push_current_activity(ACTIVITY_CONTEXTMENU);
    if ((root_menu__[selection]->flags&MENU_TYPE_MASK) == MT_RETURN_VALUE)
    {
        int item = root_menu__[selection]->value;
        context_menu = items[item].context_menu;
    }
    /* special cases */
    else if (root_menu__[selection] == &info_menu)
    {
        context_menu = &system_menu;
    }

    if (context_menu)
        retval = do_menu(context_menu, NULL, NULL, false);
    pop_current_activity();
    return retval;
}

static void ignore_back_button_stub(bool ignore)
{
    (void) ignore;
}

static int root_menu_setup_screens(void)
{
    int new_screen = next_screen;
    if (global_settings.start_in_screen == 0)
        new_screen = global_status.last_screen;
    else
        new_screen = global_settings.start_in_screen - 2;

    /* A saved index can outlive the screen it named -- this fork removes root
       entries -- and an unlisted items[] slot is a NULL function pointer that
       load_screen() would call through. */
    if (new_screen >= NUM_ITEMS)
        new_screen = GO_TO_ROOT;
    else if (new_screen == GO_TO_PLUGIN)
    {
        if (global_status.last_screen == GO_TO_SHORTCUTMENU)
        {
            /* Can make this any value other than GO_TO_SHORTCUTMENU
               otherwise it takes over on startup when the user wanted
               the plugin at key - LANG_START_SCREEN */
            global_status.last_screen = GO_TO_PLUGIN;
        }
        if(global_status.last_screen == GO_TO_SHORTCUTMENU ||
           global_status.last_screen == GO_TO_PLUGIN)
        {
            if (global_settings.start_in_screen == 0)
            {  /* Start in: Previous Screen */
                last_screen = GO_TO_PREVIOUS;
                global_status.last_screen = GO_TO_ROOT;
                /* since the plugin has GO_TO_PLUGIN as origin it
                   will just return GO_TO_PREVIOUS <=> GO_TO_PLUGIN in a loop
                   To allow exit after restart we check for GO_TO_ROOT
                   if so exit to ROOT after the plugin exits */
            }
        }
    }
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, rootmenu_track_changed_callback);

    if (new_screen == GO_TO_WPS && global_settings.unplug_autoresume)
    {
       new_screen = GO_TO_ROOT;
        if (headphones_inserted())
            new_screen = GO_TO_WPS;
    }

    return new_screen;
}

static int browser_default(void)
{
    switch (global_settings.browser_default)
    {
        case BROWSER_DEFAULT_DB:
            return GO_TO_DBBROWSER;
        case BROWSER_DEFAULT_PL_CAT:
            return GO_TO_PLAYLISTS_SCREEN;
        case BROWSER_DEFAULT_FILES:
        default:
            return GO_TO_FILEBROWSER;
    }
}

void root_menu(void)
{
    int previous_browser = browser_default();
    int selected = 0;

    root_menu_fixup_tagnavi_slots();
    /* Now that tagnavi.config has been parsed the slot can be found, which it
     * could not be when the setting was loaded. */
    root_menu_set_audiobooks_row(global_settings.segregate_audiobooks);
    push_current_activity(ACTIVITY_MAINMENU);
    next_screen = root_menu_setup_screens();

    while (true)
    {
        /* A playlist that ran out, extended here rather than where it ended:
         * this is the UI thread, and the work is seconds of disk. Silent
         * either way -- nobody is looking at the screen when the music
         * stops. */
        if (sound_mix_continue_due()
            && sound_mix_continue(global_settings.mix_length) > 0)
        {
            next_screen = GO_TO_WPS;
        }

        switch (next_screen)
        {
            case MENU_ATTACHED_USB:
            case MENU_SELECTED_EXIT:
                /* fall through */
            case GO_TO_ROOT:
                if (last_screen != GO_TO_ROOT)
                    selected = get_selection(last_screen);
                global_status.last_screen = GO_TO_ROOT; /* We've returned to ROOT */
                /* When we are in the main menu we want the hardware BACK
                 * button to be handled by HOST instead of rockbox */
                ignore_back_button_stub(true);

                {
                    /* See root_menu_build_display_list()'s comment: this
                     * is a display-only copy, built fresh every time,
                     * that may insert Resume Playback/Now Playing even
                     * though the user toggled it off -- root_menu__[]
                     * itself (what actually gets persisted) is never
                     * touched. 'selected' indexes into the persisted
                     * layout, so it needs shifting to match whenever the
                     * display list has the extra item inserted ahead of
                     * it, and shifting back afterward for whatever else
                     * consumes it (e.g. the next get_selection() call). */
                    struct menu_item_ex display_menu = root_menu_;
                    bool inserted;
                    unsigned display_count =
                        root_menu_build_display_list(&inserted);
                    int display_selected = selected + (inserted ? 1 : 0);

                    display_menu.submenus =
                        (const struct menu_item_ex **)&root_menu_display__;
                    display_menu.flags =
                        (root_menu_.flags & ~(MENU_COUNT_MASK << MENU_COUNT_SHIFT))
                        | MENU_ITEM_COUNT(display_count);

                    next_screen = do_menu(&display_menu, &display_selected,
                                          NULL, false);

                    selected = display_selected - (inserted ? 1 : 0);
                    if (selected < 0)
                        selected = 0;
                }

                ignore_back_button_stub(false);

                if (next_screen != GO_TO_PREVIOUS)
                    last_screen = GO_TO_ROOT;
                break;
            case GO_TO_DBBROWSER:
#define TAGNAVI_CASE(n) case GO_TO_TAGNAVI_FIRST + (n):
            TAGNAVI_CASE(0)  TAGNAVI_CASE(1)  TAGNAVI_CASE(2)  TAGNAVI_CASE(3)
            TAGNAVI_CASE(4)  TAGNAVI_CASE(5)  TAGNAVI_CASE(6)  TAGNAVI_CASE(7)
            TAGNAVI_CASE(8)  TAGNAVI_CASE(9)  TAGNAVI_CASE(10) TAGNAVI_CASE(11)
            TAGNAVI_CASE(12) TAGNAVI_CASE(13) TAGNAVI_CASE(14) TAGNAVI_CASE(15)
            TAGNAVI_CASE(16) TAGNAVI_CASE(17) TAGNAVI_CASE(18) TAGNAVI_CASE(19)
#undef TAGNAVI_CASE
            case GO_TO_FILEBROWSER:
            case GO_TO_PLAYLISTS_SCREEN:
                previous_browser = next_screen;
                goto load_next_screen;
                break;

            case GO_TO_PREVIOUS:
            {
                next_screen = last_screen;
                if (last_screen == GO_TO_PLUGIN)/* for WPS */
                    last_screen = GO_TO_PREVIOUS;
                else if (last_screen == GO_TO_PREVIOUS)
                    next_screen = GO_TO_ROOT;
                break;
            }

            case GO_TO_PREVIOUS_BROWSER:
                next_screen = previous_browser;
                break;

            case GO_TO_PREVIOUS_MUSIC:
                next_screen = previous_music;
                break;
            case GO_TO_ROOTITEM_CONTEXT:
                next_screen = load_context_screen(selected);
                break;
            case GO_TO_PLUGIN:
                /* Plugins are gone; nothing produces this screen anymore. A
                 * stale config (start screen / last screen set to a plugin)
                 * can still land here -- fall back to the root menu. */
                next_screen = GO_TO_ROOT;
                break;
            default:
                goto load_next_screen;
                break;
        } /* switch() */
        continue;
load_next_screen: /* load_screen is inlined */
        next_screen = load_screen(next_screen);
    }

}
