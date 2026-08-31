/***************************************************************************
 * Original code from RockBox
 * was: apps/filetree.c
 * Copyright (C) 2005 by Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * The browser's filesystem backend: reads a directory into the browser cache,
 * decides what entering a file does, and builds playlists from a
 * directory.
 *
 * The counterpart to browser_db.c: same interface (load, enter, exit) onto
 * the filesystem instead of the database, which is how browser.c can drive
 * either without caring which it has.
 *
 * "Entering" a file is dispatch by file type, not by extension parsing at the
 * call site: browser_disk_enter() switches on the attribute the filetype
 * table assigned during load, and hands off to a viewer, the playlist code,
 * or a settings loader.
 *
 * Parts, in order:
 *   - building a playlist from the current directory
 *   - reading a directory into the cache: filtering, sorting, thumbnails
 *   - path assembly helpers
 *   - browser_disk_enter(): the per-filetype dispatch
 ****************************************************************************/
#include <stdlib.h>
#include <file.h>
#include <dir.h>
#include <string.h>
#include <kernel.h>
#include <lcd.h>
#include <debug.h>
#include <font.h>
#include <limits.h>
#include "screens/bookmark.h"
#include "browser.h"
#include "core_alloc.h"
#include "settings/settings.h"
#include "files/filetypes.h"
#include "speech/talk.h"
#include "playlist/playlist.h"
#include "lang.h"
#include "speech/language.h"
#include "rolo.h"
#include "widgets/splash.h"
#include "metadata/cuesheet.h"
#include "browser_disk.h"
#include "system/app_util.h"
#include "system/activity.h"   /* ui_set_working -- the status-bar busy indicator */
#include "skin/skin_engine.h"
#include "skin/statusbar_skinned.h"
#include "strnatcmp.h"
#include "widgets/keyboard.h"

#include "mv.h"

#include "screens/playback/wps.h"
#include "draw/viewport.h"
#include "pathfuncs.h"
#include "root_menu.h"

/* The top row a freshly loaded directory wants shown, or -1 for no preference.
 * A list re-chooses its top row when the selection moves, not on a plain draw,
 * so opening scrolled past the Search row has to be asked for out here and
 * applied after the selection is set -- see browser.c. */
static int pending_top_item = -1;

static struct compare_data
{
    int sort_dir; /* qsort key for sorting directories */
    int sort_file; /*       ...for sorting files       */
    int(*_compar)(const char*, const char*, size_t);
} cmp_data;

/* dummmy functions to allow compatibility with strncmp & strncasecmp */
static int strnatcmp_n(const char *a, const char *b, size_t n)
{
    (void)n;
     return strnatcmp(a, b);
}
static int strnatcasecmp_n(const char *a, const char *b, size_t n)
{
    (void)n;
     return strnatcasecmp(a, b);
}

int browser_disk_build_playlist(struct browser_context* c, int start_index)
{
    int i;
    int start=start_index;
    int res;
    struct playlist_info *playlist = playlist_get_current();

    browser_lock_cache(c);
    struct entry *entries = browser_get_entries(c);
    bool exceeds_pl = false;
    if (c->filesindir > playlist->max_playlist_size)
    {
        exceeds_pl = true;
        start_index = 0;
    }
    struct playlist_insert_context pl_context;

    res = playlist_insert_context_create(playlist, &pl_context,
                                        PLAYLIST_REPLACE, false, false);
    if (res >= 0)
    {
        cpu_boost(true);
        for(i = 0;i < c->filesindir;i++)
        {
            int item = i;
            if (exceeds_pl)
                item = (i + start) % c->filesindir;
            if((entries[item].attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO)
            {
                res = playlist_insert_context_add(&pl_context, entries[item].name);
                if (res < 0)
                    break;
            }
            else if (!exceeds_pl)
            {
                /* Adjust the start index when se skip non-MP3 entries */
                if(i < start)
                    start_index--;
            }
        }
        cpu_boost(false);
    }

    playlist_insert_context_release(&pl_context);

    browser_unlock_cache(c);
    return start_index;
}

/* Start playback of a playlist, checking for bookmark autoload, modified
 * playlists, etc., as required. Returns false if playback wasn't started,
 * or started via bookmark autoload, true otherwise.
 *
 * Pointers to both the full pathname and the separated parts needed to
 * avoid allocating yet another path buffer on the stack (and save some
 * code; the caller typically needs to create the full pathname anyway)...
 */
bool browser_disk_play_playlist(char* pathname, char* dirname, char* filename)
{
    if (global_settings.party_mode && audio_status())
    {
        splash(HZ, ID2P(LANG_PARTY_MODE));
        return false;
    }

    int res =  bookmark_autoload(pathname);
    if (res == BOOKMARK_CANCEL || res == BOOKMARK_DO_RESUME || !warn_on_pl_erase())
        return false;

    /* Single exit so the indicator is cleared on both outcomes. This one is
     * also reached from iap-lingo4.c, outside the file-type switch below that
     * clears it for the rest of this file. */
    bool started = false;

    ui_set_working(true);

    if (playlist_create(dirname, filename) != -1)
    {
        if (global_settings.playlist_shuffle)
            playlist_shuffle(current_tick, -1);

        playlist_start(0, 0, 0);
        started = true;
    }

    ui_set_working(false);
    return started;
}

/* walk a directory and check all entries if a .talk file exists */
static void check_file_thumbnails(struct browser_context* c)
{
    int i;
    struct dirent *entry;
    struct entry* entries;
    DIR *dir;

    dir = opendir(c->currdir);
    if(!dir)
        return;
    /* mark all files as non talking, except the .talk ones */
    browser_lock_cache(c);
    entries = browser_get_entries(c);

    /* Past the Search row: it has no file behind it to have a .talk clip, and
     * the test below indexes back from the end of a name long enough to hold
     * the extension. */
    for (i = c->special_entry_count; i < c->filesindir; i++)
    {
        if (entries[i].attr & ATTR_DIRECTORY)
            continue; /* we're not touching directories */

        if (strcasecmp(file_thumbnail_ext,
            &entries[i].name[strlen(entries[i].name)
                              - strlen(file_thumbnail_ext)]))
        {   /* no .talk file */
            entries[i].attr &= ~FILE_ATTR_THUMBNAIL; /* clear */
        }
        else
        {   /* .talk file, we later let them speak themselves */
            entries[i].attr |= FILE_ATTR_THUMBNAIL; /* set */
        }
    }

    while((entry = readdir(dir)) != 0) /* walk directory */
    {
        int ext_pos;
        struct dirinfo info = dir_get_info(dir, entry);
        ext_pos = strlen((char *)entry->d_name) - strlen(file_thumbnail_ext);
        if (ext_pos <= 0 /* too short to carry ".talk" */
            || (info.attribute & ATTR_DIRECTORY) /* no file */
            || strcasecmp((char *)&entry->d_name[ext_pos], file_thumbnail_ext))
        {   /* or doesn't end with ".talk", no candidate */
            continue;
        }

        /* terminate the (disposable) name in dir buffer,
           this truncates off the ".talk" without needing an extra buffer */
        entry->d_name[ext_pos] = '\0';

        /* search corresponding file in dir cache */
        for (i=0; i < c->filesindir; i++)
        {
            if (!strcasecmp(entries[i].name, (char *)entry->d_name))
            {   /* match */
                entries[i].attr |= FILE_ATTR_THUMBNAIL; /* set the flag */
                break; /* exit search loop, because we found it */
            }
        }
    }
    browser_unlock_cache(c);
    closedir(dir);
}

/* support function for qsort() */
static int compare(const void* p1, const void* p2)
{
    struct entry* e1 = (struct entry*)p1;
    struct entry* e2 = (struct entry*)p2;
    int criteria;

    if (cmp_data.sort_dir == SORT_AS_FILE)
    {   /* treat as two files */
        criteria = cmp_data.sort_file;
    }
    else if (e1->attr & ATTR_DIRECTORY && e2->attr & ATTR_DIRECTORY)
    {   /* two directories */
        criteria = cmp_data.sort_dir;

        if (e1->attr & ATTR_VOLUME || e2->attr & ATTR_VOLUME)
        {   /* a volume identifier is involved */
            if (e1->attr & ATTR_VOLUME && e2->attr & ATTR_VOLUME)
                criteria = SORT_ALPHA; /* two volumes: sort alphabetically */
            else /* only one is a volume: volume first */
                return (e2->attr & ATTR_VOLUME) - (e1->attr & ATTR_VOLUME);
        }

    }
    else if (!(e1->attr & ATTR_DIRECTORY) && !(e2->attr & ATTR_DIRECTORY))
    {   /* two files */
        criteria = cmp_data.sort_file;
    }
    else /* dir and file, dir goes first */
        return (e2->attr & ATTR_DIRECTORY) - (e1->attr & ATTR_DIRECTORY);

    switch(criteria)
    {
        case SORT_TYPE:
        case SORT_TYPE_REVERSED:
        {
            int t1 = e1->attr & FILE_ATTR_MASK;
            int t2 = e2->attr & FILE_ATTR_MASK;

            if (!t1) /* unknown type */
                t1 = INT_MAX; /* gets a high number, to sort after known */
            if (!t2) /* unknown type */
                t2 = INT_MAX; /* gets a high number, to sort after known */

            if (t1 != t2) /* if different */
                return (t1 - t2) * (criteria == SORT_TYPE_REVERSED ? -1 : 1);
            /* else alphabetical sorting */
            return cmp_data._compar(e1->name, e2->name, MAX_PATH);
        }

        case SORT_DATE:
        case SORT_DATE_REVERSED:
        {
            if (e1->time_write != e2->time_write)
                return (e1->time_write - e2->time_write)
                       * (criteria == SORT_DATE_REVERSED ? -1 : 1);
            /* else fall through to alphabetical sorting */
        }
        case SORT_ALPHA:
        case SORT_ALPHA_REVERSED:
        {
            return cmp_data._compar(e1->name, e2->name, MAX_PATH) *
                (criteria == SORT_ALPHA_REVERSED ? -1 : 1);
        }

    }
    return 0; /* never reached */
}

/* load and sort directory into the tree's cache. returns NULL on failure. */
int browser_disk_load(struct browser_context* c, const char* tempdir)
{
    if (c->out_of_tree > 0) /* something else is loaded */
        return 0;

    int files_in_dir = 0;
    int name_buffer_used = 0;
    struct dirent *entry;
    bool (*callback_show_item)(char *, int, struct browser_context *) = NULL;
    DIR *dir;

    if (!c->is_browsing)
        c->browse = NULL;

    if (tempdir)
        dir = opendir(tempdir);
    else
    {
        dir = opendir(c->currdir);
        callback_show_item = c->browse? c->browse->callback_show_item: NULL;
    }
    if(!dir)
        return -1; /* not a directory */

    c->dirsindir = 0;
    c->dirfull = false;

    browser_lock_cache(c);

    /* The Search row, when this browse asked for one and we are at the top of
     * it. Built before the directory is read so that it is entry 0, and
     * counted as a special entry so that everything keyed on that -- the album
     * art rows, and the opening position below -- steps over it.
     *
     * Written straight into the caches rather than through the readdir loop:
     * it has no directory entry behind it, and every filter in that loop is
     * about what a real one contains. */
    c->special_entry_count = 0;
    if (!tempdir && c->dirlevel == 0 && c->browse
        && (c->browse->flags & BROWSE_SEARCH_ROW))
    {
        struct entry* dptr = browser_get_entry_at(c, 0);
        const char *label = str(LANG_DB_SEARCH);
        int len = strlen(label);

        if (dptr && len < c->cache.name_buffer_size)
        {
            dptr->attr = FILE_ATTR_SEARCH;
            dptr->time_write = 0;
            dptr->name = core_get_data(c->cache.name_buffer_handle);
            strcpy(dptr->name, label);
            name_buffer_used = len + 1;
            files_in_dir = 1;
            c->special_entry_count = 1;
        }
    }

    while ((entry = readdir(dir))) {
        int len;
        struct dirinfo info;
        struct entry* dptr = browser_get_entry_at(c, files_in_dir);
        if (!dptr)
        {
            c->dirfull = true;
            break;
        }

        info = dir_get_info(dir, entry);
        len = strlen((char *)entry->d_name);

        /* Skip FAT volume ID */
        if (info.attribute & ATTR_VOLUME_ID) {
            continue;
        }

        dptr->attr = info.attribute;
        int dir_attr = (dptr->attr & ATTR_DIRECTORY);
        /* skip directories . and .. */
        if (dir_attr && is_dotdir_name(entry->d_name))
            continue;

        /* filter out dotfiles and hidden files */
        if (*c->dirfilter != SHOW_ALL &&
            ((entry->d_name[0]=='.') ||
            (info.attribute & ATTR_HIDDEN))) {
            continue;
        }

        if (*c->dirfilter == SHOW_PLUGINS && (dptr->attr & ATTR_DIRECTORY) &&
            (dptr->attr &
            (ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID | ATTR_VOLUME)) != 0) {
            continue; /* skip non plugin folders */
        }

        /* check for known file types */
        if ( !(dir_attr) )
            dptr->attr |= filetype_get_attr((char *)entry->d_name);

        int file_attr = (dptr->attr & FILE_ATTR_MASK);

#define CHK_FT(show,attr) (*c->dirfilter == (show) && file_attr != (attr))
        /* filter out non-visible files */
        if ((!(dir_attr) && (CHK_FT(SHOW_PLAYLIST, FILE_ATTR_M3U) ||
            (CHK_FT(SHOW_MUSIC, FILE_ATTR_AUDIO) && file_attr != FILE_ATTR_M3U) ||
            (*c->dirfilter == SHOW_SUPPORTED && !filetype_supported(dptr->attr)))) ||
            CHK_FT(SHOW_WPS,  FILE_ATTR_WPS)  ||
            CHK_FT(SHOW_FONT, FILE_ATTR_FONT) ||
            CHK_FT(SHOW_SBS,  FILE_ATTR_SBS)  ||
            CHK_FT(SHOW_M3U, FILE_ATTR_M3U) ||
            CHK_FT(SHOW_CFG, FILE_ATTR_CFG) ||
            CHK_FT(SHOW_LNG, FILE_ATTR_LNG) ||
            CHK_FT(SHOW_MOD, FILE_ATTR_MOD) ||
           /* show first level directories */
           ((!(dir_attr) || c->dirlevel > 0)       &&
            CHK_FT(SHOW_PLUGINS, FILE_ATTR_ROCK)   &&
                       file_attr != FILE_ATTR_LUA  &&
                       file_attr != FILE_ATTR_OPX) ||
            (callback_show_item && !callback_show_item(entry->d_name, dptr->attr, c)))
        {
            continue;
        }
#undef CHK_FT

        if (len > c->cache.name_buffer_size - name_buffer_used - 1) {
            /* Tell the world that we ran out of buffer space */
            c->dirfull = true;
            break;
        }

        ++files_in_dir;

        dptr->name = core_get_data(c->cache.name_buffer_handle)+name_buffer_used;
        dptr->time_write = info.mtime;
        strcpy(dptr->name, (char *)entry->d_name);
        name_buffer_used += len + 1;

        if (dir_attr) /* count the remaining dirs */
            c->dirsindir++;
    }
    c->filesindir = files_in_dir;
    c->dirlength = files_in_dir;
    closedir(dir);

    /* allow directories to be sorted into file list */
    cmp_data.sort_dir = (*c->dirfilter == SHOW_PLUGINS) ? SORT_AS_FILE : c->sort_dir;

    /* playlist catalog uses sorting independent from file browser */
    cmp_data.sort_file = (*c->dirfilter == SHOW_M3U) ?
                         global_settings.sort_playlists : global_settings.sort_file;

    if (global_settings.sort_case)
    {
        if (global_settings.interpret_numbers == SORT_INTERPRET_AS_NUMBER)
            cmp_data._compar = strnatcmp_n;
        else
            cmp_data._compar = strncmp;
    }
    else
    {
        if (global_settings.interpret_numbers == SORT_INTERPRET_AS_NUMBER)
            cmp_data._compar = strnatcasecmp_n;
        else
            cmp_data._compar = strncasecmp;
    }

    /* The Search row sorts nowhere: it is not a filename and it has to stay at
     * the top, so the sort starts past it. */
    qsort(browser_get_entries(c) + c->special_entry_count,
          files_in_dir - c->special_entry_count, sizeof(struct entry), compare);

    /* If thumbnail talking is enabled, make an extra run to mark files with
       associated thumbnails, so we don't do unsuccessful spinups later. */
    if (global_settings.talk_file_clip)
        check_file_thumbnails(c); /* map .talk to ours */

    /* Open past the Search row, the way a database level opens past its own
     * special rows. A search box as the first thing on screen reads as though
     * that is where the files start, and one flick of the wheel brings it
     * back. selected_item == 0 is what tells a list opening at the top from
     * one restoring a position, exactly as it does in browser_db.c. */
    if (c->special_entry_count > 0 && c->selected_item == 0
        && files_in_dir > c->special_entry_count)
    {
        c->selected_item = c->special_entry_count;
        pending_top_item = c->special_entry_count;
    }

    browser_unlock_cache(c);
    return 0;
}

/* Returns the top row a freshly loaded directory wants shown, or -1 if it has
 * no preference. Clears the request, so a later redraw (or a list the user has
 * since scrolled) is not forced back to it. */
int browser_disk_take_pending_top_item(void)
{
    int item = pending_top_item;
    pending_top_item = -1;
    return item;
}

static void browser_disk_load_font(char *file)
{
    int current_font_id;
    enum screen_type screen = SCREEN_MAIN;
    set_file(file, (char *)global_settings.font_file);
    ui_set_working(true);
    current_font_id = screens[screen].getuifont();
    if (current_font_id >= 0)
        font_unload(current_font_id);
    screens[screen].setuifont(
        font_load_ex(file,0,global_settings.glyphs_to_cache));
    viewportmanager_theme_changed(THEME_UI_VIEWPORT);
}

static void browser_disk_apply_skin_file(char *buf, char *file)
{
    ui_set_working(true);
    set_file(buf, file);
    settings_apply_skins();
}

/* Repaint everything after a "... loaded" splash that followed a theme change.
 * The load redraws the screen and consumes the skins' full-update flag, then
 * the splash covers the middle of the display.
 *
 * Clearing the framebuffer is the part that cannot be skipped. Asking the skins
 * to redraw in full only repaints the viewports they define, and skin_render()
 * clears the screen itself only for the WPS -- so on a list screen the box
 * survives in whatever the theme's viewports do not cover, which for a skin
 * that insets its UI viewport is most of the box. The list and the statusbar
 * skin then draw over the cleared buffer before the next flush. */
static void browser_disk_repaint_after_splash(void)
{
    FOR_NB_SCREENS(i)
        screens[i].clear_display();
    skin_request_full_update(CUSTOM_STATUSBAR);
    sb_skin_force_next_update();
}

static const char *strip_slash(const char *path, const char *def)
{
    if (path)
    {
        while (*path == PATH_SEPCH)
            path++; /* we don't want this treated as an absolute path */
        return path;
    }
    return def;
}

int browser_disk_assemble_path(char *buf, size_t bufsz, const char* currdir, const char* filename)
{
    size_t len;
    const char *cd = strip_slash(currdir, "");
    filename = strip_slash(filename, "");
    /* remove slashes and NULL strings to make logic below simpler */

    /* Multi-volume device drives might be enumerated in root so everything
       should be an absolute qualified path with <drive>/ prepended */
    if (*cd != '\0') /* Not in / */
    {
        if (*cd == VOL_START_TOK)
        {
          /* use currdir, here we want the slash as it already contains the <drive> */
            len = path_append(buf, currdir, filename, bufsz);
        } /* buf => /currdir/filename */
        else
        {
            len = path_append(buf, root_realpath(), cd, bufsz); /* /<drive>/currdir */
            if(len < bufsz)
                len += path_append(buf + len, PA_SEP_HARD, filename, bufsz - len);
        } /* buf => /<drive>/currdir/filename */
    }
    else /* In / */
    {
        if (*filename == VOL_START_TOK)
        {
            len = path_append(buf, PATH_SEPSTR, filename, bufsz);
        } /* buf => /filename */
        else
        {
            len = path_append(buf, root_realpath(), filename, bufsz);
        } /* buf => /<drive>/filename */
    }

    if (len > bufsz)
        splash(HZ, ID2P(LANG_PLAYLIST_DIRECTORY_ACCESS_ERROR));
    return (int)len;
}

int browser_disk_enter(struct browser_context* c)
{
    int rc = GO_TO_PREVIOUS;
    char buf[MAX_PATH];

    struct entry* file = browser_get_entry_at(c, c->selected_item);
    if (!file)
    {
        splashf(HZ, ID2P(LANG_READ_FAILED), str(LANG_UNKNOWN));
        return rc;
    }

    int file_attr = file->attr;
    browser_disk_assemble_path(buf, sizeof(buf), c->currdir, file->name);
    if (file_attr & ATTR_DIRECTORY) {
        memcpy(c->currdir, buf, sizeof(c->currdir));
        if ( c->dirlevel < MAX_DIR_LEVELS )
            c->selected_item_history[c->dirlevel] = c->selected_item;
        c->dirlevel++;
        c->selected_item=0;
    }
    else {
        int seed = current_tick;
        bool play = false;
        int start_index=0;

        switch ( file_attr & FILE_ATTR_MASK ) {
            /* The synthetic Search row. Which box it opens is the browse's,
             * not the row's: the playlist catalogue searches playlist names,
             * everything else searches filenames.
             *
             * Returned rather than broken out of. The tail of this function
             * sends any browse above NUM_FILTER_MODES to the root once its row
             * has acted, which is right for a picker -- choosing a theme ends
             * the browse -- but this row has not acted, it is asking for a
             * screen. Breaking here would hand back GO_TO_ROOT instead, and
             * the catalogue (SHOW_M3U) is exactly such a browse. */
            case FILE_ATTR_SEARCH:
                return (*c->dirfilter == SHOW_M3U) ? GO_TO_PLAYLIST_SEARCH
                                                   : GO_TO_FILE_SEARCH;

            case FILE_ATTR_M3U:
                play = browser_disk_play_playlist(buf, c->currdir, file->name);

                if (play)
                {
                    start_index = 0;
                }

                break;

            case FILE_ATTR_AUDIO:
            {
                int res = bookmark_autoload(c->currdir);
                if (res == BOOKMARK_CANCEL || res == BOOKMARK_DO_RESUME)
                    break;

                ui_set_working(true);

                /* about to create a new current playlist...
                   allow user to cancel the operation */
                if (!warn_on_pl_erase())
                    break;

                if (global_settings.party_mode && audio_status())
                {
                    playlist_insert_track(NULL, buf,
                                          PLAYLIST_INSERT_LAST, true, true);
                    splash(HZ, ID2P(LANG_QUEUE_LAST));
                }
                else
                {
                    /* use the assembled path sans filename */
                    char * fp = strrchr(buf, PATH_SEPCH);
                    if (fp)
                        *fp = '\0';
                    if (playlist_create(buf, NULL) != -1)
                    {
                        start_index = browser_disk_build_playlist(c, c->selected_item);
                        if (global_settings.playlist_shuffle)
                        {
                            start_index = playlist_shuffle(seed, start_index);
                            /* when shuffling dir.: play all files
                               even if the file selected by user is
                               not the first one */
                            if (!global_settings.play_selected)
                                start_index = 0;
                        }
                        playlist_start(start_index, 0, 0);
                        play = true;
                    }
                }
                break;
            }
            case FILE_ATTR_SBS:
                browser_disk_apply_skin_file(buf, global_settings.sbs_file);
                break;
                /* wps config file */
            case FILE_ATTR_WPS:
                browser_disk_apply_skin_file(buf, global_settings.wps_file);
                break;
            case FILE_ATTR_CFG:
                ui_set_working(true);
                if (!settings_load_config(buf,true))
                    break;
                splash(HZ, ID2P(LANG_SETTINGS_LOADED));
                browser_disk_repaint_after_splash();
                break;

            case FILE_ATTR_BMARK:
                ui_set_working(true);
                bookmark_load(buf, false);
                rc = GO_TO_FILEBROWSER;
                break;

            case FILE_ATTR_LNG:
                ui_set_working(true);
                if (lang_core_load(buf))
                {
                    splash(HZ, ID2P(LANG_FAILED));
                    break;
                }
                set_file(buf, (char *)global_settings.lang_file);
                talk_init(); /* use voice of same language */
                viewportmanager_theme_changed(THEME_LANGUAGE);
                settings_apply_skins();
                splash(HZ, ID2P(LANG_LANGUAGE_LOADED));
                browser_disk_repaint_after_splash();
                break;

            case FILE_ATTR_FONT:
                browser_disk_load_font(buf);
                break;

            case FILE_ATTR_KBD:
                /* loadable layouts are gone; just remember the selection */
                set_file(buf, (char *)global_settings.kbd_file);
                break;

                /* firmware file */
            case FILE_ATTR_MOD:
                ui_set_working(true);
                audio_hard_stop();
                rolo_load(buf);
                break;
            case FILE_ATTR_CUE:
                display_cuesheet_content(buf);
                break;

            default:
            {
                if (global_settings.party_mode && audio_status()) {
                    splash(HZ, ID2P(LANG_PARTY_MODE));
                    break;
                }

                file = browser_get_entry_at(c, c->selected_item);
                if (!file)
                {
                    splashf(HZ, ID2P(LANG_READ_FAILED), str(LANG_UNKNOWN));
                    return rc;
                }

                /* Core-linked viewers (text/image) run in-process and hand back
                 * a GO_TO_* code directly. Other file types have no handler. */
                filetype_open_core_viewer(file->attr, buf, &rc);
                break;
            }
        }

        /* One clear for every case above that lit the indicator, on all of
         * their break paths. A no-op when nothing set it, so cases that finish
         * instantly never flash it. */
        ui_set_working(false);

        if ( play ) {
            /* the resume_index must always be the index in the
               shuffled list in case shuffle is enabled */
            global_status.resume_index = start_index;
            global_status.resume_crc32 =
                playlist_get_filename_crc32(NULL, start_index);
            global_status.resume_elapsed = 0;
            global_status.resume_offset = 0;
            status_save(false);
            rc = GO_TO_WPS;
        }
        else {
            if (*c->dirfilter > NUM_FILTER_MODES &&
                *c->dirfilter != SHOW_CFG &&
                *c->dirfilter != SHOW_FONT &&
                *c->dirfilter != SHOW_PLUGINS)
            {
                rc = GO_TO_ROOT;
            }
        }
    }

    return rc;
}

int browser_disk_exit(struct browser_context* c)
{
    extern char lastfile[]; /* from tree.c */
    char buf[MAX_PATH];
    int rc = 0;
    bool exit_func = false;
    int i = strlen(c->currdir);

    /* strip trailing slashes */
    while (c->currdir[i-1] == PATH_SEPCH)
        i--;

    if (i>1) {
        while (c->currdir[i-1]!=PATH_SEPCH)
            i--;
        strcpy(buf,&c->currdir[i]);
        if (i==1)
            c->currdir[i]='\0';
        else
            c->currdir[i-1]='\0';

        if ((unsigned)c->dirlevel<=2) /* only expect redirect two levels max */
        {
            char *currdir = c->currdir;
            const char *root = root_realpath();
            int len = i-1;
            /* compare to the root path bail if they don't match except single '/' */
            for (; len > 0 && *root != '\0' && *root == *currdir; len--)
            {
                root++;
                currdir++;
            }
            if (*root == PATH_SEPCH) /* root may have trailing slash */
                root++;
            if (*root == '\0' &&
                (len == 0 || (len == 1 && *currdir == PATH_SEPCH)))
            {
                strcpy(c->currdir, PATH_ROOTSTR);
                c->dirlevel=1;
            }
        }

        if (*c->dirfilter > NUM_FILTER_MODES && c->dirlevel < 1)
            exit_func = true;

        c->dirlevel--;
        if ( c->dirlevel < MAX_DIR_LEVELS )
            c->selected_item=c->selected_item_history[c->dirlevel];
        else
            c->selected_item=0;

        /* if undefined position */
        if (c->selected_item == -1)
            strcpy(lastfile, buf);
    }
    else
    {
        if (*c->dirfilter > NUM_FILTER_MODES && c->dirlevel < 1)
            exit_func = true;
    }

    if (exit_func)
        rc = 3;

    c->out_of_tree = 0;

    return rc;
}
