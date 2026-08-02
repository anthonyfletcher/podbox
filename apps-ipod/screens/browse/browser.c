/***************************************************************************
 * Original code from RockBox
 * was: apps/tree.c
 * Copyright (C) 2002 Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * The file browser screen. Owns the browser_context (current directory,
 * selection, filter), the browse loop, and dispatch into the database
 * browser or a viewer.
 *
 * The list widget does not hold the directory. It asks for one item at a
 * time through the callbacks near the top -- name, icon, colour, album art,
 * voice -- each taking a selected_item index that this file resolves against
 * the cached directory entries. That cache is a buflib allocation shared with
 * the database browser, which is why it is taken and released around use
 * (browser_lock_cache) and why it has a move_callback.
 *
 * dirbrowse() is the screen's event loop and the file's centre of gravity.
 *
 * Parts, in order:
 *   - browser_context accessors and directory-entry lookup
 *   - the list callbacks: filename, colour, icon, album art, voice
 *   - album art slots for the browsing screen
 *   - init, cache locking, and the buflib move callback
 *   - update_dir(): reading a directory into the cache and configuring the list
 *   - current-file and current-directory tracking, including getcwd wrapping
 *   - dirbrowse(): the browse loop and its actions
 *   - rockbox_browse(): the entry point other screens call
 *   - resuming a bookmark, playing a file or directory, and flush/restore
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "string-extra.h"
#include "panic.h"
#include "rbpaths.h"

#include "system/applimits.h"
#include "dir.h"
#include "file.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "kernel.h"
#include "usb.h"
#include "usb_core.h"    /* usb_core_host_wrote_storage */
#include "browser.h"
#include "audio.h"
#include "playlist/playlist.h"
#include "widgets/menu.h"
#include "skin/skin_engine.h"
#include "settings/settings.h"
#include "debug.h"
#include "storage.h"
#include "rolo.h"
#include "draw/icon_bitmaps.h"
#include "lang.h"
#include "widgets/keyboard.h"
#include "screens/bookmark.h"
#include "screens/context_menu.h"
#include "core_alloc.h"
#include "metadata/art_cache.h"
#include "power.h"
#include "input/action.h"
#include "speech/talk.h"
#include "files/filetypes.h"
#include "system/app_util.h"
#include "system/activity.h"   /* ui_set_working -- the status-bar busy indicator */
#include "audio/sound_feedback.h"
#include "system/shutdown.h"
#include "system/strutil.h"
#include "pathfuncs.h"
#include "browser_disk.h"
#include "browser_db.h"
#include "rtc.h"
#include "dircache.h"
#include "database/tagcache.h"
#include "widgets/yesno.h"
#include "eeprom_settings.h"
#include "playlist/catalog.h"

/* gui api */
#include "widgets/list.h"
#include "widgets/splash.h"
#include "screens/playback/quick_screen.h"
#include "screens/shortcuts.h"
#include "system/appevents.h"

#include "root_menu.h"

static struct gui_synclist browser_lists;

/* I put it here because other files doesn't use it yet,
 * but should be elsewhere since it will be used mostly everywhere */
static struct browser_context tc;

char lastfile[MAX_PATH];
static char lastdir[MAX_PATH];
static int lasttable, lastextra;

static bool reload_dir = false;

static bool start_wps = false;
static int curr_context = false;/* id3db or tree*/

static int dirbrowse(void);
static int browser_play_dirname(char* name);
static int browser_play_filename(char *dir, char *file, int attr);
static void say_filetype(int attr);

struct entry* browser_get_entries(struct browser_context *t)
{
    return core_get_data(t->cache.entries_handle);
}

struct entry* browser_get_entry_at(struct browser_context *t, int index)
{
    if(index < 0 || index >= t->cache.max_entries)
        return NULL; /* no entry */
    struct entry* entries = browser_get_entries(t);
    return &entries[index];
}

static struct entry *get_valid_entry(const char* funcname,
                                     struct browser_context *t, int index)
{
    struct entry *entry = browser_get_entry_at(t, index);
    if (!entry)
        panicf("Invalid tree entry %s", funcname);
    /*DEBUGF("%s tc: %x idx: %d\n", funcname, t, index);*/
    return entry;
}

static bool ext_stripit(bool isdir, int attr, int dirfilter)
{
    if((dirfilter != SHOW_ID3DB) && !isdir)
    {
        switch(global_settings.show_filename_ext)
        {
            case 0:
                /* show file extension: off */
                return true;
                break;
            case 1:
                /* show file extension: on */
                break;
            case 2:
                /* show file extension: only unknown types */
                return filetype_supported(attr);
            case 3:
            default:
                /* show file extension: only when viewing all */
                return (dirfilter != SHOW_ALL);
        }
    }
    return false;
}

static const char* browser_get_filename(int selected_item, void *data,
                                     char *buffer, size_t buffer_len)
{
    struct browser_context * local_tc=(struct browser_context *)data;
    char *name;
    int attr=0;
    bool id3db = *(local_tc->dirfilter) == SHOW_ID3DB;

    if (id3db)
    {
        return browser_db_get_entry_name(&tc, selected_item, buffer, buffer_len);
    }
    else
    {
        struct entry *entry = get_valid_entry(__func__, local_tc, selected_item);
        name = entry->name;
        attr = entry->attr;
    }

    if(ext_stripit((attr & ATTR_DIRECTORY), attr, *(local_tc->dirfilter)))
    {
        return(strip_extension(buffer, buffer_len, name));
    }
    return(name);
}

static int browser_get_filecolor(int selected_item, void * data)
{
    if (*tc.dirfilter == SHOW_ID3DB)
        return -1;
    struct browser_context * local_tc=(struct browser_context *)data;
    struct entry *entry = get_valid_entry(__func__, local_tc, selected_item);

    return filetype_get_color(entry->name, entry->attr);
}

static enum themable_icons browser_get_fileicon(int selected_item, void * data)
{
    struct browser_context * local_tc=(struct browser_context *)data;
    bool id3db = *(local_tc->dirfilter) == SHOW_ID3DB;
    if (id3db) {
        return browser_db_get_icon(&tc);
    }
    else
    {
        struct entry *entry = get_valid_entry(__func__, local_tc, selected_item);

        return filetype_get_icon(entry->attr);
    }
}

/* Album art for database album rows, drawn by the skin's %La tag.
 *
 * Resolving one row costs a tagcache search (browser_db_get_album_dir) plus a file
 * read, and the skin asks for every visible row on every redraw -- so a few
 * decoded thumbnails are kept, keyed by list item, and thrown away whenever the
 * list reloads. A miss simply returns NULL: thumbnails are produced in the
 * background, so a row just has no cover until the cache catches up. */
#define TREE_AA_SLOTS 8

static int browser_aa_size_idx = -2;   /* -2 == not looked up, -1 == no such size */
static int browser_aa_handle = -1;     /* pixel store for the slots */
static int browser_aa_dim;
static int browser_aa_item[TREE_AA_SLOTS];  /* item cached in each slot, -1 empty */
static int browser_aa_victim;               /* round-robin replacement */
static struct bitmap browser_aa_bm;         /* handed back; points into the store */

static void browser_aa_reset(void)
{
    for (int i = 0; i < TREE_AA_SLOTS; i++)
        browser_aa_item[i] = -1;
    browser_aa_victim = 0;
}

static size_t browser_aa_slot_bytes(void)
{
    return (size_t)browser_aa_dim * browser_aa_dim * FB_DATA_SZ;
}

static bool browser_aa_ready(void)
{
    if (browser_aa_size_idx == -2)
    {
        browser_aa_size_idx = art_cache_size_index("list");
        if (browser_aa_size_idx >= 0)
            browser_aa_dim = art_cache_size_dim(browser_aa_size_idx);
    }
    if (browser_aa_size_idx < 0)
        return false;

    if (browser_aa_handle <= 0)
    {
        browser_aa_handle = core_alloc(TREE_AA_SLOTS * browser_aa_slot_bytes());
        if (browser_aa_handle <= 0)
            return false;   /* no memory right now; try again next redraw */
        browser_aa_reset();
    }
    return true;
}

/* Read a cached thumbnail into `slot`: a small header followed by native pixels
 * (see art_cache.h). The read yields, so the store is pinned across it rather
 * than trusting a pointer taken beforehand.
 *
 * Copied in whole, so it has to be stored the way it is drawn: rows. The list
 * size is (art_sizes.h), but that is checked rather than assumed -- a size
 * stored by columns would otherwise load without complaint and draw mirrored. */
static bool browser_aa_load(const char *path, int slot)
{
    struct art_cache_header hdr;
    size_t bytes = browser_aa_slot_bytes();
    bool ok = false;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return false;

    if (read(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr) &&
        hdr.magic == ART_CACHE_MAGIC &&
        hdr.version == ART_CACHE_FORMAT_VERSION &&
        hdr.layout == AA_ROWS &&
        hdr.width == browser_aa_dim && hdr.height == browser_aa_dim)
    {
        char *store = core_get_data_pinned(browser_aa_handle);
        ok = read(fd, store + (size_t)slot * bytes, bytes) == (ssize_t)bytes;
        core_put_data_pinned(store);
    }

    close(fd);
    return ok;
}

static const struct bitmap *browser_get_albumart(int selected_item, void * data,
                                              struct dim *size)
{
    struct browser_context *local_tc = (struct browser_context *)data;
    char dir[MAX_PATH];
    char aat[MAX_PATH];
    int slot;

    (void)size;   /* we always hand back dim x dim; the skin clips to its viewport */

    if (selected_item < local_tc->special_entry_count || !browser_aa_ready())
        return NULL;

    for (slot = 0; slot < TREE_AA_SLOTS; slot++)
        if (browser_aa_item[slot] == selected_item)
            goto hit;

    /* One list is either an album list or an artist list; resolve the row to the
     * matching folder (album folder, or its parent artist folder). */
    bool artist = !browser_db_is_album_list(local_tc);
    if (artist)
    {
        if (!browser_db_get_artist_dir(local_tc, selected_item, dir, sizeof(dir)))
            return NULL;
    }
    else if (!browser_db_get_album_dir(local_tc, selected_item, dir, sizeof(dir)))
        return NULL;

    /* Both kinds of row want the placeholder returned transparently, so a row
     * with no art still fills its viewport. Artist rows once took a different
     * path here, to avoid showing the album "?" on a person; the two
     * placeholders have since become one image, so the distinction no longer
     * draws anything different. */
    if (!art_cache_lookup(dir, browser_aa_size_idx, aat, sizeof(aat), NULL))
        return NULL;    /* no art and no placeholder generated yet */

    slot = browser_aa_victim;
    browser_aa_victim = (browser_aa_victim + 1) % TREE_AA_SLOTS;
    browser_aa_item[slot] = -1;    /* in case the load fails partway */

    if (!browser_aa_load(aat, slot))
        return NULL;
    browser_aa_item[slot] = selected_item;

hit:
    browser_aa_bm.width  = browser_aa_dim;
    browser_aa_bm.height = browser_aa_dim;
    browser_aa_bm.format = FORMAT_NATIVE;
    browser_aa_bm.data   = (unsigned char *)core_get_data(browser_aa_handle) +
                        (size_t)slot * browser_aa_slot_bytes();
    return &browser_aa_bm;
}


static int browser_voice_cb(int selected_item, void * data)
{
    struct browser_context * local_tc=(struct browser_context *)data;
    unsigned char *name;
    int attr=0;
    int customaction = ONPLAY_NO_CUSTOMACTION;
    bool id3db = *(local_tc->dirfilter) == SHOW_ID3DB;
    char buf[AVERAGE_FILENAME_LENGTH*2];

    if (id3db)
    {
        attr = browser_db_get_attr(local_tc);
        name = browser_db_get_entry_name(local_tc, selected_item, buf, sizeof(buf));
        customaction = browser_db_get_custom_action(local_tc);

        /* See if name is an encoded ID, if it is, then speak it normally */
        int lang_id = P2ID(name);
        /*debugf("%s Found name %s id %d\n", __func__, P2STR(name), lang_id);*/
        if (lang_id >= 0) {
            if (global_settings.talk_menu)
                talk_id(lang_id, true);
            return 0;
        }

        /* Otherwise it is a custom "header" or a database entry, and there is
           no talk clip for it: doing so needs headers told apart from entries,
           and a subdirectory of clips per entry type (artist, album, ...).
           Not implemented, so such rows are silent. */
    }
    else
    {
        struct entry *entry = get_valid_entry(__func__, local_tc, selected_item);
        name = entry->name;
        attr = entry->attr;
    }
    bool is_dir = (attr & ATTR_DIRECTORY);
    bool did_clip = false;
    /* First the .talk clip case */
    if(is_dir)
    {
        if(global_settings.talk_dir_clip)
        {
            did_clip = true;
            if (browser_play_dirname(name) <= 0)
                /* failed, not existing */
                did_clip = false;
        }
    } else { /* it's a file */
        if (global_settings.talk_file_clip && (attr & FILE_ATTR_THUMBNAIL))
        {
            did_clip = true;
            if (browser_play_filename(local_tc->currdir, name, attr) <= 0)
                /* failed, not existing */
                did_clip = false;
        }
    }
    bool spell_name = (customaction == ONPLAY_CUSTOMACTION_FIRSTLETTER);
    if(!did_clip)
    {
        /* say the number or spell if required or as a fallback */
        switch (is_dir ? global_settings.talk_dir : global_settings.talk_file)
        {
        case 1: /* as numbers */
            talk_id(is_dir ? VOICE_DIR : VOICE_FILE, false);
            talk_number(selected_item+1        - (is_dir ? 0 : local_tc->dirsindir),
                        true);
            break;
        case 2: /* spelled */
            talk_shutup();
            if(global_settings.talk_filetype)
            {
                if(is_dir)
                    talk_id(VOICE_DIR, true);
            }
            spell_name = true;
            break;
        }
    }

    if(global_settings.talk_filetype && !is_dir
       && *local_tc->dirfilter < NUM_FILTER_MODES)
    {
        say_filetype(attr);
    }

    /* spell name AFTER voicing filetype */
    if (spell_name) {
            bool stripit = ext_stripit(is_dir, attr, *(local_tc->dirfilter));
            char *ext = NULL;

            /* Don't spell the extension if it's not displayed */

            if (stripit) {
                ext = strrchr(name, '.');
                if (ext)
                    *ext = 0;
            }

        talk_spell(name, true);

        if (stripit && ext)
            *ext = '.';
    }

    return 0;
}

bool check_rockboxdir(void)
{
    if(!dir_exists(ROCKBOX_DIR))
    {   /* No need to localise this message.
           If .rockbox is missing, it wouldn't work anyway */
        FOR_NB_SCREENS(i)
            screens[i].clear_display();
        splash(HZ*2, "No .rockbox directory");
        FOR_NB_SCREENS(i)
            screens[i].clear_display();
        splash(HZ*2, "Installation incomplete");
        return false;
    }
    return true;
}

/* do this really late in the init sequence */
void browser_init(void)
{
    check_rockboxdir();
    strcpy(tc.currdir, "/");
}

struct browser_context* browser_get_context(void)
{
    return &tc;
}

void browser_lock_cache(struct browser_context *t)
{
    core_pin(t->cache.name_buffer_handle);
    core_pin(t->cache.entries_handle);
}

void browser_unlock_cache(struct browser_context *t)
{
    core_unpin(t->cache.name_buffer_handle);
    core_unpin(t->cache.entries_handle);
}

/*
 * Returns the position of a given file in the current directory
 * returns -1 if not found
 */
static int browser_get_file_position(char * filename)
{
    int i, ret = -1;/* no file match, return undefined */

    browser_lock_cache(&tc);
    struct entry *entries = browser_get_entries(&tc);

    /* use lastfile to determine the selected item (default=0) */
    for (i=0; i < tc.filesindir; i++)
    {
        if (!strcasecmp(entries[i].name, filename))
        {
            ret = i;
            break;
        }
    }
    browser_unlock_cache(&tc);
    return(ret);
}

/* Whether the browse level currently on screen draws tall art rows (album or
 * artist art). Set by update_dir() before each redraw, so it is valid when the
 * themed status bar renders its chrome -- unlike the per-row %La, which needs a
 * drawn list row. The theme reads it (via %La outside a row) to drop the left
 * rounded corner that would otherwise clash with the square art. */
static bool db_showing_art;

bool browser_showing_art(void)
{
    return db_showing_art;
}

/*
 * Called when a new dir is loaded (for example when returning from other apps ...)
 * also completely redraws the tree
 */
static int update_dir(void)
{
    struct gui_synclist * const list = &browser_lists;
    int show_path_in_browser = global_settings.show_path_in_browser;
    bool changed = false;

    const char* title = NULL;/* Must clear the title as the list is reused */
    int icon = NOICON;

    bool id3db = *tc.dirfilter == SHOW_ID3DB;

    /* Initialise before any of the early returns below. exit_to_new_screen()
     * calls gui_synclist_scroll_stop() whenever this returns <= 0, including
     * the -1 paths where the load failed -- e.g. "remember last folder"
     * pointing at a directory that has since been deleted. */
    gui_synclist_init(list, &browser_get_filename, &tc, false, 1, NULL);

    /* Checks for changes */
    if (id3db) {
        if (tc.currtable != lasttable ||
            tc.currextra != lastextra ||
            reload_dir)
        {
            if (browser_db_load(&tc) < 0)
                return -1;

            lasttable = tc.currtable;
            lastextra = tc.currextra;
            changed = true;
        }
    }
    else
    {
        tc.sort_dir = global_settings.sort_dir;
        /* if the tc.currdir has been changed, reload it ...*/
        if (reload_dir || strncmp(tc.currdir, lastdir, sizeof(lastdir)))
        {
            if (browser_disk_load(&tc, NULL) < 0)
                return -1;
            strmemccpy(lastdir, tc.currdir, MAX_PATH);
            changed = true;
        }
    }
    /* the list's contents moved under us, so cached thumbnails (keyed by item
     * index) no longer describe the rows they sit in */
    if (changed)
        browser_aa_reset();
    /* if selected item is undefined */
    if (tc.selected_item == -1)
    {
        if (!id3db)
            /* use lastfile to determine the selected item */
            tc.selected_item = browser_get_file_position(lastfile);

        /* If the file doesn't exists, select the first one (default) */
        if(tc.selected_item < 0)
            tc.selected_item = 0;
        changed = true;
    }
    if (changed)
    {
        if( !id3db && tc.dirfull )
        {
            splash(HZ, ID2P(LANG_SHOWDIR_BUFFER_FULL));
        }
    }

    if (id3db)
    {
        if (show_path_in_browser == SHOW_PATH_FULL
            || show_path_in_browser == SHOW_PATH_CURRENT)
        {
            title = browser_db_get_title(&tc);
            icon = filetype_get_icon(ATTR_DIRECTORY);
        }
    }
    else
    {
        if (tc.browse && tc.browse->title)
        {
            title = tc.browse->title;
            icon = tc.browse->icon;
            if (icon == NOICON)
                icon = filetype_get_icon(ATTR_DIRECTORY);
            /* display sub directories in the title of plugin browser */
            if (tc.dirlevel > 0 && *tc.dirfilter == SHOW_PLUGINS)
            {
                char *subdir = strrchr(tc.currdir, '/');
                if (subdir != NULL)
                    title = subdir + 1; /* step past the separator */
            }
        }
        else
        {
            if (show_path_in_browser == SHOW_PATH_FULL)
            {
                title = tc.currdir;
                icon = filetype_get_icon(ATTR_DIRECTORY);
            }
            else if (show_path_in_browser == SHOW_PATH_CURRENT)
            {
                title = strrchr(tc.currdir, '/');
                if (title != NULL)
                {
                    title++; /* step past the separator */
                    if (*title == '\0')
                    {
                        /* Display "Files" for the root dir */
                        title = ID2P(LANG_DIR_BROWSER);
                    }
                    icon = filetype_get_icon(ATTR_DIRECTORY);
                }
            }
        }
    }

    /* set title and icon, if nothing is set, clear the title
     * with NULL and icon as NOICON as the list is reused */
    gui_synclist_scroll_stop(list); /*ADDED*/
    gui_synclist_set_title(list, P2STR((unsigned char*)title), icon);

    gui_synclist_set_nb_items(list, tc.filesindir);
    gui_synclist_set_icon_callback(list,
                            global_settings.show_icons?browser_get_fileicon:NULL);
    /* Art (cover callback + tall uniform rows) on album lists (album art) and
     * artist lists (artist art), each behind its own toggle -- so ordinary lists
     * and the whole off path never touch the art-resolution code. */
    {
        bool tall_rows = false;
        if (*tc.dirfilter == SHOW_ID3DB)
        {
            if (global_settings.db_albumart && browser_db_is_album_list(&tc))
                tall_rows = true;
            else if (global_settings.db_artistart && browser_db_is_artist_list(&tc))
                tall_rows = true;
        }
        db_showing_art = tall_rows;
        gui_synclist_set_albumart_callback(list,
                                    tall_rows ? browser_get_albumart : NULL);
        /* Uniform tall rows so a cover fits (special rows included); 0 = the
         * skin's default height. */
        gui_synclist_set_row_height(list,
                                    tall_rows ? global_settings.db_art_row_height : 0);
    }
    /* In the database the letter menus voice themselves through this callback,
     * so with "Voice menus" off they would still speak. */
    if (!id3db || global_settings.talk_menu)
        gui_synclist_set_voice_callback(list, &browser_voice_cb);
    gui_synclist_set_color_callback(list, &browser_get_filecolor);
    if( tc.selected_item >= tc.filesindir)
        tc.selected_item=tc.filesindir-1;

    gui_synclist_select_item(list, tc.selected_item);
    /* A freshly entered database level asks to open scrolled past its special
     * rows (<All tracks>/<Random>), so the list reads as starting at the first
     * real entry -- they are still one press up. Must follow the select, which
     * would otherwise keep one row visible above the cursor. */
    if (*tc.dirfilter == SHOW_ID3DB)
    {
        int top_item = browser_db_take_pending_top_item();
        /* Only if the selection still fits on screen below it. The list
         * re-chooses its top row when the selection moves, not on a plain
         * draw, so a top row that pushed the cursor out of sight would simply
         * stay that way until the user scrolled. */
        if (top_item >= 0
            && gui_synclist_get_sel_pos(list)
                 < top_item + gui_synclist_get_nb_lines(list, SCREEN_MAIN))
            gui_synclist_set_top_item(list, top_item);
    }
    /* Draw twice to settle the top row: after a change between lists of
     * different row heights (tall album-art rows <-> ordinary rows) the shared
     * %?La conditional flips on the first row, leaving its album-layout
     * viewports in a transient state; a second pass renders it clean, the same
     * way any user interaction already does. The first pass is flush-inhibited
     * so that transient frame never reaches the screen (no flicker). */
    gui_synclist_inhibit_flush(true);
    gui_synclist_draw(list);
    gui_synclist_inhibit_flush(false);
    gui_synclist_draw(list);
    gui_synclist_speak_item(list);
    return tc.filesindir;
}

/* load tracks from specified directory to resume play */
void resume_directory(const char *dir)
{
    int dirfilter = *tc.dirfilter;
    int ret;
    bool id3db = *tc.dirfilter == SHOW_ID3DB;
    /* make sure the dirfilter is sane. The only time it should be possible
     * thats its not is when resume playlist is called from a plugin
     */
    if (!id3db)
        *tc.dirfilter = global_settings.dirfilter;
    ret = browser_disk_load(&tc, dir);
    *tc.dirfilter = dirfilter;
    if (ret < 0)
        return;
    lastdir[0] = 0;

    browser_disk_build_playlist(&tc, 0);

    if (id3db)
        browser_db_load(&tc);
}

/* This is the whole firmware's getcwd(): there is no per-process working
 * directory to ask about, so "the current directory" is defined to be the one
 * the browser is showing. Passing buf == NULL returns the internal string
 * directly rather than copying, which is why callers must not hold it across
 * a directory change.
 *
 * Returns the current working directory and also writes cwd to buf if
 * non-NULL.  In case of error, returns NULL. */
char *getcwd(char *buf, getcwd_size_t size)
{
    if (!buf)
        return tc.currdir;
    else if (size)
    {
        if (strmemccpy(buf, tc.currdir, size) != NULL)
            return buf;
    }
    /* size == 0, or truncation in strmemccpy */
    return NULL;
}

/* Force a reload of the directory next time directory browser is called */
void reload_directory(void)
{
    reload_dir = true;
}

char* get_current_file(char* buffer, size_t buffer_len)
{
    /* in ID3DB mode it is a bad idea to call this function */
    /* (only happens with `follow playlist') */
    if( *tc.dirfilter == SHOW_ID3DB )
        return NULL;

    struct entry *entry = browser_get_entry_at(&tc, tc.selected_item);
    if (entry && getcwd(buffer, buffer_len))
    {
        if (!tc.dirlength)
            return buffer;

        size_t usedlen = strlen(buffer);

        if (usedlen + 2 < buffer_len) /* ensure enough room for '/' + '\0' */
        {
            if (buffer[usedlen-1] != '/')
            {
                buffer[usedlen] = '/';
                /* strmemccpy will zero terminate if we run out of space after */
                usedlen++;
            }
            buffer_len -= usedlen;
            if (strmemccpy(buffer + usedlen, entry->name, buffer_len) != NULL)
                return buffer;
        }
    }
    return NULL;
}

/* Allow apps to change our dirfilter directly (required for sub browsers)
   if they're suddenly going to become a file browser for example */
void set_dirfilter(int l_dirfilter)
{
    *tc.dirfilter = l_dirfilter;
}

/* Selects a path + file and update tree context properly */
static void set_current_file_ex(const char *path, const char *filename)
{
    int i;

    /* in ID3DB mode it is a bad idea to call this function */
    /* (only happens with `follow playlist') */
    if( *tc.dirfilter == SHOW_ID3DB )
        return;

    if (!filename) /* path and filename supplied combined */
    {
        /* separate directory from filename */
        /* gets the directory's name and put it into tc.currdir */
        filename = strrchr(path+1,'/');
        size_t endpos = filename - path;
        if (filename && endpos < MAX_PATH - 1)
        {
            strmemccpy(tc.currdir, path, endpos + 1);
            filename++;
        }
        else
        {
            strcpy(tc.currdir, "/");
            filename = path+1;
        }
    }
    else /* path and filename came in separate ensure an ending '/' */
    {
        char *end_p = strmemccpy(tc.currdir, path, MAX_PATH);
        size_t endpos = end_p - tc.currdir;
        if (endpos < MAX_PATH)
        {
            if (tc.currdir[endpos - 2] != '/')
            {
                tc.currdir[endpos - 1] = '/';
                tc.currdir[endpos] = '\0';
            }
        }
    }
    strmemccpy(lastfile, filename, MAX_PATH);


    /* If we changed dir we must recalculate the dirlevel
       and adjust the selected history properly */
    if (strncmp(tc.currdir,lastdir,sizeof(lastdir)))
    {
        tc.dirlevel =  0;
        tc.selected_item_history[tc.dirlevel] = -1;

        /* use '/' to calculate dirlevel */
        for (i = 1; path[i] != '\0'; i++)
        {
            if (path[i] == '/')
            {
                tc.dirlevel++;
                tc.selected_item_history[tc.dirlevel] = -1;
            }
        }
    }
    if (browser_disk_load(&tc, NULL) >= 0)
    {
        tc.selected_item = browser_get_file_position(lastfile);
        if (!tc.is_browsing && tc.out_of_tree == 0)
        {
            /* the browser is closed */
            /* don't allow the previous items to overwrite what we just loaded */
            tc.out_of_tree = tc.selected_item + 1;
        }
    }
}

/* Selects a file and update tree context properly */
void set_current_file(const char *path)
{
    set_current_file_ex(path, NULL);
}


static int exit_to_new_screen(int screen)
{
    gui_synclist_scroll_stop(&browser_lists);
    return screen;
}

/* main loop, handles key events */
static int dirbrowse(void)
{
    int numentries=0;
    char buf[MAX_PATH];
    int button;
    int oldbutton;
    bool reload_root = false;
    int lastfilter = *tc.dirfilter;
    bool lastsortcase = global_settings.sort_case;
    bool exit_func = false;

    char* currdir = tc.currdir; /* just a shortcut */
    bool id3db = *tc.dirfilter == SHOW_ID3DB;

    if (id3db)
        curr_context=CONTEXT_ID3DB;
    else
        curr_context=CONTEXT_TREE;
    if (tc.selected_item < 0)
        tc.selected_item = 0;
    lasttable = -1;
    lastextra = -1;

    start_wps = false;
    numentries = update_dir();
    reload_dir = false;
    if (numentries == -1)
        return exit_to_new_screen(GO_TO_PREVIOUS);  /* currdir is not a directory */

    if (*tc.dirfilter > NUM_FILTER_MODES && numentries==0)
    {
        splash(HZ*2, *tc.dirfilter == SHOW_M3U ?
                     ID2P(LANG_CATALOG_NO_PLAYLISTS) : ID2P(LANG_NO_FILES));
        return exit_to_new_screen(GO_TO_PREVIOUS);  /* No files found for rockbox_browse() */
    }

    while(tc.browse && tc.is_browsing) {
        bool restore = false;
        if (tc.dirlevel < 0)
            tc.dirlevel = 0; /* shouldnt be needed.. this code needs work! */

        keyclick_set_callback(gui_synclist_keyclick_callback, &browser_lists);
        button = get_action(CONTEXT_TREE|ALLOW_SOFTLOCK,
                            list_do_action_timeout(&browser_lists, HZ/2));
        oldbutton = button;
        gui_synclist_do_button(&browser_lists, &button);
        tc.selected_item = gui_synclist_get_sel_pos(&browser_lists);
        int customaction = ONPLAY_NO_CUSTOMACTION;
        bool do_restore_display = true;
            if (id3db && (button == ACTION_STD_OK || button == ACTION_STD_CONTEXT))
            {
                customaction = browser_db_get_custom_action(&tc);
                if (customaction == ONPLAY_CUSTOMACTION_SHUFFLE_SONGS)
                {
                    /* The code to insert shuffled is on the context branch of the switch so we always go here */
                    button = ACTION_STD_CONTEXT;
                    do_restore_display = false;
                }
            }
        switch ( button ) {
            case ACTION_STD_OK:
                /* nothing to do if no files to display */
                if ( numentries == 0 )
                    break;
                if (tc.browse->flags & BROWSE_SELECTONLY)
                {
                    struct entry *entry =
                                get_valid_entry(__func__, &tc, tc.selected_item);
                    short attr = entry->attr;
                    if(!(attr & ATTR_DIRECTORY))
                    {
                        tc.browse->flags |= BROWSE_SELECTED;
                        get_current_file(tc.browse->buf, tc.browse->bufsize);
                        return exit_to_new_screen(GO_TO_PREVIOUS);
                    }
                }
                switch (id3db ? browser_db_enter(&tc, true) : browser_disk_enter(&tc))
                {
                    case GO_TO_FILEBROWSER: reload_dir = true; break;
                    case GO_TO_PLUGIN:
                        return exit_to_new_screen(GO_TO_PLUGIN);
                    case GO_TO_WPS:
                        return exit_to_new_screen(GO_TO_WPS);
                    /* The database browser's synthetic album-charts row (see
                     * load_root() in browser_db.c). Codes not named here fall
                     * to the default and are silently dropped, so a new one
                     * has to be added in both places. */
                    case GO_TO_ALBUM_CHARTS:
                        return exit_to_new_screen(GO_TO_ALBUM_CHARTS);
                    case GO_TO_RANDOM_ALBUM:
                        return exit_to_new_screen(GO_TO_RANDOM_ALBUM);
                    case GO_TO_DB_SEARCH:
                        return exit_to_new_screen(GO_TO_DB_SEARCH);
                    case GO_TO_ROOT: exit_func = true; break;
                    default:
                        break;
                }
                restore = do_restore_display;
                break;

            case ACTION_STD_CANCEL:
                exit_to_new_screen(0);
                if (*tc.dirfilter > NUM_FILTER_MODES && tc.dirlevel < 1) {
                    exit_func = true;
                    break;
                }
                /* browser_db_back_exits(): a main-menu shortcut jumped
                 * straight to this level, so popping would land on the menu it
                 * was meant to skip. One press undoes one jump. */
                if ((*tc.dirfilter == SHOW_ID3DB
                     && (tc.dirlevel == 0 || browser_db_back_exits(&tc))) ||
                    ((*tc.dirfilter != SHOW_ID3DB && !strcmp(currdir,"/"))))
                {
                    if (oldbutton == ACTION_TREE_PGLEFT)
                        break;
                    else
                        return exit_to_new_screen(GO_TO_ROOT);
                }

                if (id3db)
                    browser_db_exit(&tc, true);
                else
                    if (browser_disk_exit(&tc) == 3)
                        exit_func = true;

                restore = do_restore_display;
                break;

            case ACTION_TREE_STOP:
                if (list_stop_handler())
                    restore = do_restore_display;
                break;

            case ACTION_STD_MENU:
                return exit_to_new_screen(GO_TO_ROOT);
                break;


            case ACTION_TREE_WPS:
                return exit_to_new_screen(GO_TO_PREVIOUS_MUSIC);
                break;
            case ACTION_STD_QUICKSCREEN:
            {
                bool enter_shortcuts_menu = global_settings.shortcuts_replaces_qs;
                if (enter_shortcuts_menu && *tc.dirfilter >= NUM_FILTER_MODES)
                    break;
                else if (!enter_shortcuts_menu)
                {
                    int ret = quick_screen_quick(button);
                    if (ret == QUICKSCREEN_IN_USB)
                        reload_dir = true;
                    else if (ret == QUICKSCREEN_GOTO_SHORTCUTS_MENU)
                        enter_shortcuts_menu = true;
                }

                if (enter_shortcuts_menu && *tc.dirfilter < NUM_FILTER_MODES)
                {
                    int last_screen = global_status.last_screen;
                    global_status.last_screen = GO_TO_SHORTCUTMENU;
                    int shortcut_ret = do_shortcut_menu(NULL);
                    if (shortcut_ret == GO_TO_PREVIOUS)
                        global_status.last_screen = last_screen;
                    else
                        return exit_to_new_screen(shortcut_ret);
                }
                else if (enter_shortcuts_menu) /* currently disabled */
                {
                    /* QuickScreen defers skin updates, popping its activity, when
                       switching to Shortcuts Menu, so make up for that here:   */
                    FOR_NB_SCREENS(i)
                        skin_update(CUSTOM_STATUSBAR, i, SKIN_REFRESH_ALL);
                }

                restore = do_restore_display;
                break;
            }

            case ACTION_TREE_HOTKEY:
                if (HK_CTX_GET(0, global_settings.hotkey_tree) == HOTKEY_OFF)
                    break;
                /* fall through */
            case ACTION_STD_CONTEXT:
            {
                bool hotkey = button == ACTION_TREE_HOTKEY;
                int context_menu_result;
                int attr = 0;

                if (tc.browse->flags & BROWSE_NO_CONTEXT_MENU)
                    break;

                if(!numentries)
                    context_menu_result = context_menu_show(NULL, 0, curr_context, hotkey, customaction);
                else {
                    if (id3db)
                    {
                        if (browser_db_get_attr(&tc) == FILE_ATTR_AUDIO)
                        {
                            attr = FILE_ATTR_AUDIO;

                            /* Look up the filename only once it is needed, so we
                               don't have to wait for the disk to wake up here. */
                            buf[0] = '\0';
                        }
                        else
                        {
                            attr = ATTR_DIRECTORY;
                            int title_len = 0;

                            /* In case of "special entries", add table title as
                               prefix, e.g. "The Beatles [All Tracks]", instead
                               of just "[All Tracks]", to improve the suggested
                               playlist filename.
                            */
                            if (tc.selected_item < tc.special_entry_count)
                            {
                                title_len = snprintf(buf, sizeof(buf), "%s ",
                                                     browser_db_get_title(&tc));
                                if (title_len < 0)
                                    title_len = 0;
                            }

                            if (title_len < (int) sizeof(buf))
                                browser_db_get_entry_name(&tc, tc.selected_item,
                                                       buf + title_len,
                                                       sizeof(buf) - title_len);

                            fix_path_part(buf, 0, sizeof(buf) - 1);
                        }
                    }
                    else
                    {
                        struct entry *entry =
                               get_valid_entry(__func__, &tc, tc.selected_item);

                        attr = entry->attr;

                        browser_disk_assemble_path(buf, sizeof(buf), currdir, entry->name);

                    }
                    context_menu_result = context_menu_show(buf, attr, curr_context, hotkey, customaction);
                }
                switch (context_menu_result)
                {
                    case ONPLAY_MAINMENU:
                        return exit_to_new_screen(GO_TO_ROOT);
                        break;

                    case ONPLAY_REVEAL_FILE:
                        return exit_to_new_screen(GO_TO_FILEBROWSER);
                        break;

                    case ONPLAY_OK:
                        restore = do_restore_display;
                        break;

                    case ONPLAY_RELOAD_DIR:
                        reload_dir = true;
                        break;

                    case ONPLAY_START_PLAY:
                        return exit_to_new_screen(GO_TO_WPS);
                        break;

                    case ONPLAY_PLUGIN:
                        return exit_to_new_screen(GO_TO_PLUGIN);
                        break;
                }
                break;
            }


            default:
                if (default_event_handler(button) == SYS_USB_CONNECTED)
                {
                    if(*tc.dirfilter > NUM_FILTER_MODES)
                        /* leave sub-browsers after usb, doing otherwise
                           might be confusing to the user */
                        exit_func = true;
                    else
                        reload_dir = true;
                }
                break;
        }
        if (start_wps)
            return exit_to_new_screen(GO_TO_WPS);
        if (button && !IS_SYSEVENT(button))
        {
            storage_spin();
        }


    check_rescan:
        /* do we need to rescan dir? */
        if (reload_dir || reload_root ||
            lastfilter != *tc.dirfilter ||
            lastsortcase != global_settings.sort_case)
        {
            if (reload_root) {
                strcpy(currdir, "/");
                tc.dirlevel = 0;
                tc.currtable = 0;
                tc.currextra = 0;
                lasttable = -1;
                lastextra = -1;
                reload_root = false;
            }

            if (!reload_dir)
            {
                gui_synclist_select_item(&browser_lists, 0);
                gui_synclist_draw(&browser_lists);
                tc.selected_item = 0;
                lastdir[0] = 0;
            }

            lastfilter = *tc.dirfilter;
            lastsortcase = global_settings.sort_case;
            restore = do_restore_display;
        }

        if (exit_func)
            return exit_to_new_screen(GO_TO_PREVIOUS);

        if (restore || reload_dir) {
            FOR_NB_SCREENS(i)
                screens[i].scroll_stop();
            /* restore display */
            numentries = update_dir();
            reload_dir = false;
            if (currdir[1] && (numentries < 0))
            {   /* not in root and reload failed */
                reload_root = true; /* try root */
                goto check_rescan;
            }
        }
    }
    return exit_to_new_screen(GO_TO_ROOT);
}

int create_playlist(void)
{
    bool ret;
    trigger_cpu_boost();
    ret = catalog_add_to_a_playlist(PATH_ROOTSTR, ATTR_DIRECTORY, true, NULL, NULL);
    cancel_cpu_boost();

    return (ret) ? 1 : 0;
}

#define NUM_TC_BACKUP   3
static struct browser_context backups[NUM_TC_BACKUP];
/* do not make backup if it is not recursive call */
static int backup_count = -1;
int rockbox_browse(struct browse_context *browse)
{
    tc.is_browsing = (browse != NULL);
    int ret_val = 0;
    int dirfilter = SHOW_ALL;
    if (tc.is_browsing)
        dirfilter = browse->dirfilter;
    else
    {
        DEBUGF("%s browse is [NULL] \n", __func__);
        browse = tc.browse;
    }
    if (backup_count >= NUM_TC_BACKUP)
        return GO_TO_PREVIOUS;
    if (backup_count >= 0)
        backups[backup_count] = tc;
    backup_count++;
    int *prev_dirfilter = tc.dirfilter;
    tc.dirfilter = &dirfilter;
    tc.sort_dir = global_settings.sort_dir;

    reload_dir = true;

    if (tc.out_of_tree > 0)
    {
        /* an item has already been loaded out_of_tree holds the selected index
         * what happens with the item is dependent on the browse context */
        tc.selected_item = tc.out_of_tree - 1;
        tc.out_of_tree = 0;
        ret_val = browser_disk_enter(&tc);
    }
    else
    {
        if (*tc.dirfilter >= NUM_FILTER_MODES)
        {
            int last_context;
            /* don't reset if its the same browse already loaded */
            if (tc.browse != browse ||
                !(tc.currdir[1] && strstr(tc.currdir, browse->root) != NULL))
            {
                tc.browse = browse;
                tc.selected_item = 0;
                tc.dirlevel = 0;

                strmemccpy(tc.currdir, browse->root, sizeof(tc.currdir));
            }

            start_wps = false;
            last_context = curr_context;

            if (browse->selected)
            {
                set_current_file_ex(browse->root, browse->selected);
                /* set_current_file changes dirlevel, change it back */
                tc.dirlevel = 0;
            }

            ret_val = dirbrowse();
            curr_context = last_context;
        }
        else
        {
            if (dirfilter != SHOW_ID3DB && (browse->flags & BROWSE_DIRFILTER) == 0)
                tc.dirfilter = &global_settings.dirfilter;
            tc.browse = browse;
            set_current_file(browse->root);
            if (browse->flags&BROWSE_RUNFILE)
                ret_val = browser_disk_enter(&tc);
            else
                ret_val = dirbrowse();
        }
    }

    tc.is_browsing = false;
    tc.dirfilter = prev_dirfilter; /* Bugfix restore dirfilter*/

    backup_count--;
    if (backup_count >= 0)
        tc = backups[backup_count];

    return ret_val;
}

static int move_callback(int handle, void* current, void* new)
{
    struct browser_cache* cache = &tc.cache;
    ptrdiff_t diff = new - current;
    /* FIX_PTR makes sure to not accidentally update static allocations */
#define FIX_PTR(x) \
    { if ((void*)x >= current && (void*)x < (current+cache->name_buffer_size)) x+= diff; }

    if (handle == cache->name_buffer_handle)
    {   /* update entry structs, *even if they are struct tagentry */
        struct entry *this = core_get_data(cache->entries_handle);
        struct entry *last = this + cache->max_entries;
        for(; this < last; this++)
            FIX_PTR(this->name);
    }
    /* nothing to do if entries moved */
    return BUFLIB_CB_OK;
}

static struct buflib_callbacks ops = {
    .move_callback = move_callback,
    .shrink_callback = NULL,
};

void browser_mem_init(void)
{
    /* initialize tree context struct */
    struct browser_cache* cache = &tc.cache;
    memset(&tc, 0, sizeof(tc));
    tc.dirfilter = &global_settings.dirfilter;
    tc.sort_dir = global_settings.sort_dir;

    cache->name_buffer_size = AVERAGE_FILENAME_LENGTH *
        global_settings.max_files_in_dir;
    cache->name_buffer_handle = core_alloc_ex(cache->name_buffer_size, &ops);

    cache->max_entries = global_settings.max_files_in_dir;
    cache->entries_handle =
            core_alloc_ex(cache->max_entries*(sizeof(struct entry)), &ops);
}

bool bookmark_play(char *resume_file, int index, unsigned long elapsed,
                   unsigned long offset, int seed, char *filename)
{
    int i;
    char* suffix = strrchr(resume_file, '.');
    bool started = false;

    if (suffix != NULL && !strncasecmp(suffix, ".m3u", sizeof(".m3u") - 1)) /* gets m3u8 too */
    {
        /* Playlist playback */
        char* slash;
        /* check that the file exists */
        if (!file_exists(resume_file))
            return false;

        slash = strrchr(resume_file,'/');
        if (slash)
        {
            char* cp;
            *slash=0;

            cp=resume_file;
            if (!cp[0])
                cp="/";

            if (playlist_create(cp, slash+1) != -1)
            {
                if (global_settings.playlist_shuffle)
                    playlist_shuffle(seed, -1);
                started = true;
            }
            *slash='/';
        }
    }
    else
    {
        /* Directory playback */
        lastdir[0]='\0';
        if (playlist_create(resume_file, NULL) != -1)
        {
            char filename_buf[MAX_PATH + 1];
            const char* peek_filename;
            resume_directory(resume_file);
            if (global_settings.playlist_shuffle)
                playlist_shuffle(seed, -1);

            /* Check if the file is at the same spot in the directory,
               else search for it */
            int amt = playlist_amount();
            for ( i=0; i < amt; i++ )
            {
                int modidx = (i + index) % amt;
                peek_filename = playlist_peek(modidx, filename_buf,
                    sizeof(filename_buf));

                if (peek_filename == NULL)
                {
                    if (index == 0) /* searched every entry didn't find a match */
                        return false;
                    /* playlist has shrunk, search from the top */
                    i = 0;
                    amt = index;
                    index = 0;
                }
                else if (!strcmp(strrchr(peek_filename, '/') + 1, filename))
                {
                    started = true;
                    index = modidx;
                    break;
                }
            }
        }
    }

    if (started)
    {
        playlist_start(index, elapsed, offset);
        start_wps = true;
    }
    return started;
}

static void say_filetype(int attr)
{
    talk_id(filetype_get_voiceclip(attr), true);
}

static int browser_play_dirname(char* name)
{
    int vol = path_get_volume_id(name);
    if (talk_volume_id(vol))
        return 1;

    return talk_file(tc.currdir, name, dir_thumbnail_name, NULL,
                     global_settings.talk_filetype ?
                     TALK_IDARRAY(VOICE_DIR) : NULL,
                     false);
}

static int browser_play_filename(char *dir, char *file, int attr)
{
    if (strlen(file) >= strlen(file_thumbnail_ext)
        && strcasecmp(&file[strlen(file) - strlen(file_thumbnail_ext)],
                      file_thumbnail_ext))
        /* file has no .talk extension */
        return talk_file(dir, NULL, file, file_thumbnail_ext,
                         TALK_IDARRAY(filetype_get_voiceclip(attr)), false);

    /* it already is a .talk file, play this directly, but prefix it. */
    return talk_file(dir, NULL, file, NULL,
                     TALK_IDARRAY(LANG_VOICE_DIR_HOVER), false);
}

/* True while this cable session's dircache suspend is outstanding.
 *
 * dircache_suspend() nests, and browser_flush()/browser_restore() are called as
 * a pair per USB screen -- which a mid-connect blip runs more than once for a
 * single cable session. Tracking it here makes the suspend one per session
 * instead: without that, either the count climbs by one per session and the
 * cache never returns, or the blip releases it and rebuilds the tree while the
 * host is about to take the disk back. */
static bool dircache_suspended_for_usb = false;

/* These two functions are called by the USB and shutdown handlers */
void browser_flush(void)
{
     tc.is_browsing = false;/* clear browse to prevent reentry to a possibly missing file */
    tagcache_shutdown();

    tagcache_unload_ramcache();

    int old_val = global_status.dircache_size;

    if (!global_settings.dircache)
    {
        global_status.dircache_size = 0;
    }
    else if (!dircache_suspended_for_usb)
    {
        dircache_suspend();
        dircache_suspended_for_usb = true;

        struct dircache_info info;
        dircache_get_info(&info);

        global_status.dircache_size = info.last_size;
    }

    if (old_val != global_status.dircache_size)
        status_save(true);

}

/* A rescan is owed: the host changed the disk, but the cable was still in the
 * last time we looked. Survives any number of mid-connect blips and collapses
 * them into one scan. */
static bool rescan_pending = false;

void browser_restore(void)
{

    tagcache_remove_statefile();

    /* Only a write can have changed anything; after a read-only session the disk
     * is byte-for-byte as we left it. */
    if (usb_core_host_wrote_storage())
        rescan_pending = true;

    /* Not yet, if the cable is still in. Windows unconfigures and reconfigures
     * us mid-connect (SET_CONFIG(0) ~1.7s in, SET_CONFIG(1) 300ms later), and
     * that arrives here as a disconnect although the cable never moved.
     * Scanning then is both wasted and actively harmful: the reconnect lands
     * 300ms later with the tagcache thread deep in a scan, and it then takes
     * 3.4s to acknowledge the storage handover -- measured, and the largest
     * single delay in mounting.
     *
     * usb_state stays USB_INSERTED for the whole of such a blip, so this is a
     * reliable test for it. A real extraction sets USB_EXTRACTED a moment after
     * the disconnect broadcast, so in principle we can look too early and defer
     * a rescan we should have run; the request simply stays pending and the next
     * disconnect picks it up. Erring that way is deliberate -- a briefly stale
     * database costs the user nothing they will notice, and a spurious rescan
     * costs three and a half seconds of every single connect. */
    if (usb_inserted())
        return;

    /* The cable really is out, so release this session's suspend. Always --
     * leaving it standing is what kept the directory cache empty and every
     * listing on the disk for the rest of the run. */
    int dircache_rc = 0;
    if (dircache_suspended_for_usb)
    {
        dircache_rc = dircache_resume();
        dircache_suspended_for_usb = false;
    }

    if (!rescan_pending)
    {
        /* Nothing was written, so the database files are byte-for-byte what
         * they were and the RAM copy of them is still correct. browser_flush()
         * only cleared the flag, so handing the searches back is free -- and
         * without it the rest of the session reads every album name off the
         * disk, which is the difference between a tag browser that opens
         * instantly and one that does not. */
        tagcache_reload_ramcache();
        return;
    }

    rescan_pending = false;

    if (dircache_rc > 0)
    {
        /* Scanning the disk is background work, so it shows in the status bar
         * rather than over the screen. */
        ui_set_working(true);
        dircache_wait();
        ui_set_working(false);
    }

    tagcache_start_scan();
}
