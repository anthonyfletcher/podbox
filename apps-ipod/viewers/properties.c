/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/properties.c
 * Copyright (C) 2006 Peter D'Hoye
 *
 * Core File/Directory/Track Properties screen, ported from the properties
 * plugin. It is a plain core screen entered as properties(file) and returning
 * a GO_TO_* code, and renders as a normal themed core list (the theme stays
 * enabled, unlike the full-screen viewers) -- individual fields are still shown
 * full-screen via the core view_text().
 * GNU General Public License (version 2+)
 *
 * File and directory properties: size, dates and, for audio, the full tag
 * set. Aggregates whole directories via metadata/mul_id3.c.
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "string-extra.h"    /* strlcpy */
#include "system.h"          /* ARRAYLEN */
#include "kernel.h"          /* TIMEOUT_BLOCK */
#include "lang.h"            /* LANG_*, str */
#include "timefuncs.h"
#include "settings/settings.h"        /* global_settings, ID2P/P2ID/P2STR */
#include "input/action.h"          /* get_action, action_userabort */
#include "widgets/splash.h"          /* splash, splashf, splash_progress_set_delay */
#include "dir.h"             /* opendir/readdir/closedir, dir_get_info */
#include "file.h"            /* MAX_PATH, MAX_FILENAME */
#include "pathfuncs.h"       /* PATH_SEPCH */
#include "files/filetypes.h"       /* filetype_get_attr, FILE_ATTR_* */
#include "metadata.h"        /* struct mp3entry, get_metadata */
#include "playlist/playlist.h"        /* playlist_entries_iterate */
#include "screens/browse/browser_db.h"         /* browser_db_subentries_do_action */
#include "draw/viewport.h"        /* viewportmanager_theme_enable/undo */
#include "system/activity.h"
#include "system/shutdown.h"
#include "speech/talk.h"
#include "screens/playback/track_info.h"
#include "widgets/text_box.h"         /* browse_id3, view_text */
#include "root_menu.h"       /* GO_TO_* */
#include "widgets/list.h"        /* gui_synclist */
#include "metadata/mul_id3.h"
#include "database/tagcache.h"       /* struct tagcache_search */
#include "properties.h"

enum props_types {
    PROPS_FILE = 0,
    PROPS_PLAYLIST,
    PROPS_ID3,
    PROPS_MUL_ID3,
    PROPS_DIR
};

/* Static, not a local of browser_db_subentries_do_action: that call sits above
 * retrieve_entries(), which alone uses over 1.5 KB of stack. */
static struct tagcache_search tcs;
static struct mp3entry id3;
static struct tm tm;
static unsigned long display_size;
static int32_t lang_size_unit;
static int props_type, mul_id3_count, skipped_count;

static char str_filename[MAX_PATH], str_dirname[MAX_PATH],
            str_size[64], str_dircount[64], str_filecount[64],
            str_audio_filecount[64], str_date[64], str_time[64];


#define NUM_FILE_PROPERTIES 5
#define NUM_PLAYLIST_PROPERTIES (1 + NUM_FILE_PROPERTIES)
static const unsigned char* const props_file[] =
{
    ID2P(LANG_PROPERTIES_PATH),       str_dirname,
    ID2P(LANG_PROPERTIES_FILENAME),   str_filename,
    ID2P(LANG_PROPERTIES_SIZE),       str_size,
    ID2P(LANG_PROPERTIES_DATE),       str_date,
    ID2P(LANG_PROPERTIES_TIME),       str_time,

    ID2P(LANG_MENU_SHOW_ID3_INFO),    "...",
};

#define NUM_DIR_PROPERTIES 4
#define NUM_AUDIODIR_PROPERTIES (1 + NUM_DIR_PROPERTIES)
static const unsigned char* const props_dir[] =
{
    ID2P(LANG_PROPERTIES_PATH),       str_dirname,
    ID2P(LANG_PROPERTIES_SUBDIRS),    str_dircount,
    ID2P(LANG_PROPERTIES_FILES),      str_filecount,
    ID2P(LANG_PROPERTIES_SIZE),       str_size,

    ID2P(LANG_MENU_SHOW_ID3_INFO),    str_audio_filecount,
};

static bool dir_properties(const char* selected_file, struct dir_stats *stats)
{
    strlcpy(stats->dirname, selected_file, sizeof(stats->dirname));

        cpu_boost(true);
    bool success = collect_dir_stats(stats, NULL);
        cpu_boost(false);
    if (!success)
        return false;

    strlcpy(str_dirname, selected_file, sizeof(str_dirname));
    snprintf(str_dircount, sizeof str_dircount, "%d", stats->dir_count);
    snprintf(str_filecount, sizeof str_filecount, "%d", stats->file_count);
    snprintf(str_audio_filecount, sizeof str_filecount, "%d",
             stats->audio_file_count);
    display_size = human_size(stats->byte_count, &lang_size_unit);
    snprintf(str_size, sizeof str_size, "%lu %s", display_size,
             str(lang_size_unit));
    return true;
}

static const unsigned char* p2str(const unsigned char* p)
{
    int id = P2ID(p);
    return (id != -1) ? str(id) : p;
}

/* props_file[]/props_dir[] hold header/value pairs; a single-line list item i
 * maps to the pair at index i*2 (label) and i*2+1 (value). */
static const unsigned char* const *props_table(int *nb_entries)
{
    if (props_type == PROPS_DIR)
    {
        if (nb_entries)
            *nb_entries = (int)ARRAYLEN(props_dir);
        return props_dir;
    }
    if (nb_entries)
        *nb_entries = (int)ARRAYLEN(props_file);
    return props_file;
}

static const char * get_props(int selected_item, void* data,
                              char *buffer, size_t buffer_len)
{
    (void)data;
    int nb_entries;
    const unsigned char* const *props = props_table(&nb_entries);
    int i = selected_item * 2;

    if (i + 1 >= nb_entries)
        strlcpy(buffer, "ERROR", buffer_len);
    else
        snprintf(buffer, buffer_len, "%s: %s",
                 (char *) p2str(props[i]), (char *) props[i + 1]);
    return buffer;
}

static int speak_property_selection(int selected_item, void *data)
{
    struct dir_stats *stats = data;
    const unsigned char* const *props = props_table(NULL);
    int i = selected_item * 2;
    int32_t id = P2ID(props[i]);
    talk_id(id, false);
    switch (id)
    {
    case LANG_PROPERTIES_PATH:
        talk_fullpath(str_dirname, true);
        break;
    case LANG_PROPERTIES_FILENAME:
        talk_file_or_spell(str_dirname, str_filename, NULL, true);
        break;
    case LANG_PROPERTIES_SIZE:
        talk_number(display_size, true);
        talk_id(lang_size_unit, true);
        break;
    case LANG_PROPERTIES_DATE:
        talk_date(&tm, true);
        break;
    case LANG_PROPERTIES_TIME:
        talk_time(&tm, true);
        break;
    case LANG_PROPERTIES_SUBDIRS:
        talk_number(stats->dir_count, true);
        break;
    case LANG_PROPERTIES_FILES:
        talk_number(stats->file_count, true);
        break;
    default:
        talk_spell(props[i + 1], true);
        break;
    }
    return 0;
}

/* Index of the "Show Track Info..." row, or -1 if this list doesn't have one
 * (only playlists and directories with audio do). */
static int track_info_row(struct dir_stats *stats)
{
    if (props_type == PROPS_PLAYLIST)
        return NUM_PLAYLIST_PROPERTIES - 1;
    if (props_type == PROPS_DIR && stats->audio_file_count)
        return NUM_AUDIODIR_PROPERTIES - 1;
    return -1;
}

/* Action handler for the properties list. On select, either exit so the caller
 * can open Track Info, or show the field's full value full-screen. */
static struct dir_stats *cb_stats;
static int props_action_cb(int action, struct gui_synclist *lists)
{
    if (action != ACTION_STD_OK)
        return action;

    int sel = gui_synclist_get_sel_pos(lists);
    if (sel == track_info_row(cb_stats))
        return ACTION_STD_CANCEL;   /* exit; properties() opens Track Info */

    int nb_entries;
    const unsigned char* const *props = props_table(&nb_entries);
    int i = sel * 2;

    FOR_NB_SCREENS(j)
        viewportmanager_theme_enable(j, false, NULL);
    int leave = view_text((char *) p2str(props[i]), (char *) props[i + 1]);
    FOR_NB_SCREENS(j)
        viewportmanager_theme_undo(j, false);

    /* The field view was left for the root menu, so this list goes with it. */
    return leave ? ACTION_STD_MENU : ACTION_REDRAW;
}

static int browse_file_or_dir(struct dir_stats *stats)
{
    int nb_props;
    if (props_type == PROPS_PLAYLIST)
        nb_props = NUM_PLAYLIST_PROPERTIES;
    else if (props_type == PROPS_DIR)
        nb_props = stats->audio_file_count ? NUM_AUDIODIR_PROPERTIES
                                           : NUM_DIR_PROPERTIES;
    else
        nb_props = NUM_FILE_PROPERTIES;

    cb_stats = stats;

    struct simplelist_info info;
    simplelist_info_init(&info,
        str(props_type == PROPS_DIR ? LANG_PROPERTIES_DIRECTORY_PROPERTIES :
                                      LANG_PROPERTIES_FILE_PROPERTIES),
        nb_props, stats);
    info.get_name = get_props;
    if (global_settings.talk_menu)
        info.get_talk = speak_property_selection;
    info.action_callback = props_action_cb;

    if (simplelist_show_list(&info))
        return 1;                   /* USB */
    if (info.selection >= 0)        /* "Show Track Info..." chosen */
        return -1;
    return 0;
}

static bool determine_props_type(const char *file)
{
    if (file[0] == PATH_SEPCH)
    {
        const char* basename = strrchr(file, PATH_SEPCH) + 1;
        const int dir_len = (basename - file);
        if ((int) sizeof(str_dirname) <= dir_len)
            return false;
        strlcpy(str_dirname, file, dir_len + 1);
        strlcpy(str_filename, basename, sizeof str_filename);
        struct dirent* entry;
        DIR* dir = opendir(str_dirname);
        if (!dir)
            return false;
        while(0 != (entry = readdir(dir)))
        {
            if(strcmp(entry->d_name, str_filename))
                continue;

            struct dirinfo info = dir_get_info(dir, entry);
            if (info.attribute & ATTR_DIRECTORY)
                props_type = PROPS_DIR;
            else
            {
                display_size = human_size(info.size, &lang_size_unit);
                snprintf(str_size, sizeof str_size, "%lu %s",
                         display_size, str(lang_size_unit));
                gmtime_r(&info.mtime, &tm);
                snprintf(str_date, sizeof str_date, "%04d/%02d/%02d",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
                snprintf(str_time, sizeof str_time, "%02d:%02d:%02d",
                         tm.tm_hour, tm.tm_min, tm.tm_sec);

                if (filetype_get_attr(entry->d_name) == FILE_ATTR_M3U)
                    props_type = PROPS_PLAYLIST;
                else
                    props_type = get_metadata(&id3, -1, file) ?
                                 PROPS_ID3 : PROPS_FILE;
            }
            closedir(dir);
            return true;
        }
        closedir(dir);
    }
    else if (!strcmp(file, MAKE_ACT_STR(ACTIVITY_DATABASEBROWSER)))
    {
        props_type = PROPS_MUL_ID3;
        return true;
    }
    return false;
}

static bool mul_id3_add(const char *file_name)
{
    if (!file_name || !get_metadata(&id3, -1, file_name))
        skipped_count++;
    else
    {
        collect_id3(&id3, mul_id3_count == 0);
        mul_id3_count++;
    }
    return true;
}

/* Assemble track info from a dir, a playlist, or a database table */
static bool assemble_track_info(const char *filename, struct dir_stats *stats)
{
    if (props_type == PROPS_DIR)
    {
        cpu_boost(true);
        strlcpy(stats->dirname, filename, sizeof(stats->dirname));
        splash_progress_set_delay(HZ/2); /* hide progress bar for 0.5s */
        bool success = collect_dir_stats(stats, &mul_id3_add);
        cpu_boost(false);
        if (!success)
            return false;
    }
    else if(props_type == PROPS_PLAYLIST &&
            !playlist_entries_iterate(filename, NULL, &mul_id3_add))
        return false;
    else if (props_type == PROPS_MUL_ID3 &&
             !browser_db_subentries_do_action(&tcs, &mul_id3_add, str_filename,
                                              sizeof str_filename))
        return false;

    if (mul_id3_count == 0)
    {
        splash(HZ * 2, "No matching files");
        return false;
    }
    else if (mul_id3_count > 1) /* otherwise, the retrieved id3 can be used as-is */
        finalize_id3(&id3);

    if (skipped_count > 0)
        splashf(HZ * 2, "Skipped %d files", skipped_count);

    return true;
}

int properties(const char *file)
{
    static struct dir_stats stats;

    /* The core keeps these statics between invocations (the plugin got a fresh
     * BSS each launch), so start every run from a clean slate. */
    memset(&stats, 0, sizeof(stats));
    mul_id3_count = 0;
    skipped_count = 0;
    collect_dir_stats_reset();

    int ret = file && determine_props_type(file);
    if (!ret)
    {
        splashf(0, "Could not find: %s", file ?: "(NULL)");
        action_userabort(TIMEOUT_BLOCK);
        return GO_TO_PREVIOUS;
    }

    /* Erase the background behind the progress bar. A theme that gives the
     * context menu activity its own viewport leaves whatever it drew there
     * showing through while the scan runs. Directories are exempt: they show
     * no progress bar and draw their own screen, so clearing only flashes. */
    if (props_type != PROPS_DIR)
    {
        /* Static because the scroll engine keeps the pointer, not a copy --
         * see the list_text[] note in list_render.c. */
        static struct viewport ui_vp;
        struct screen *display = &screens[SCREEN_MAIN];
        viewport_set_defaults(&ui_vp, SCREEN_MAIN);
        struct viewport *last_vp = display->set_viewport(&ui_vp);
        display->clear_viewport();
        display->set_viewport(last_vp);
    }

    if (props_type == PROPS_MUL_ID3)
        ret = assemble_track_info(NULL, NULL);
    else if (props_type == PROPS_DIR)
        ret = dir_properties(file, &stats);
    if (!ret)
    {
        if (!stats.canceled)
        {
            splash(0, ID2P(LANG_PROPERTIES_FAIL));    /* TODO: describe error */
            action_userabort(TIMEOUT_BLOCK);
        }
    }
    else if (props_type == PROPS_ID3)
        /* Track Info for single file */
        ret = browse_id3(&id3, 0, 0, &tm, 1, &view_text);
    else if (props_type == PROPS_MUL_ID3)
        /* database tracks */
        ret = browse_id3(&id3, 0, 0, NULL, mul_id3_count, &view_text);
    else if ((ret = browse_file_or_dir(&stats)) < 0)
        ret = assemble_track_info(file, &stats) ?
              /* playlist or folder tracks */
              browse_id3(&id3, 0, 0, NULL, mul_id3_count, &view_text) :
              (stats.canceled ? 0 : -1);

    return ret == 1 ? GO_TO_ROOT : GO_TO_PREVIOUS;
}
