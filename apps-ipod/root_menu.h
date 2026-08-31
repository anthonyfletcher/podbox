/***************************************************************************
 * Original code from RockBox
 * was: apps/root_menu.h
 * Copyright (C) 2007 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * Interface to root_menu.c and the GO_TO_* screen identifiers every screen
 * returns.
 ****************************************************************************/
#ifndef __ROOT_MENU_H__
#define __ROOT_MENU_H__

#include "config.h"
#include "gcc_extensions.h"

void root_menu(void) NORETURN_ATTR;
struct menu_table {
    char *string;
    const struct menu_item_ex *item;
};

struct menu_table *root_menu_get_options(int *nb_options);
/* Add or remove the Audiobooks row, which Segregate Audiobooks owns -- see
 * root_menu_set_audiobooks_row() for why the setting rather than Customize
 * Main Menu decides whether it is there. */
void root_menu_set_audiobooks_row(bool on);

/* Number of reserved root-menu shortcut slots for tagnavi.config's root
 * ("main") menu's direct tag-browse rows -- see the comment at
 * GO_TO_TAGNAVI_FIRST below. */
#define TAGNAVI_MAIN_MENU_SLOTS 20

enum {
    /* from old menu api, but still required*/
    MENU_ATTACHED_USB = -10,
    MENU_SELECTED_EXIT = -9,

    GO_TO_ROOTITEM_CONTEXT = -5,
    GO_TO_PREVIOUS_MUSIC = -4,
    GO_TO_PREVIOUS_BROWSER = -3,
    GO_TO_PREVIOUS = -2,
    GO_TO_ROOT = -1,
    GO_TO_FILEBROWSER = 0,
    GO_TO_DBBROWSER,
    GO_TO_WPS,
    GO_TO_MAINMENU,
    GO_TO_RECENTBMARKS,
    GO_TO_PLUGIN,
    /* Do Not add any items above here unless you want it to be able to 
       be the "start screen" after a boot up. The setting in settings_list.c
       will need editing if this is the case. */
    GO_TO_BROWSEPLUGINS,
    GO_TO_TIMESCREEN,
    GO_TO_PLAYLISTS_SCREEN,
    GO_TO_PLAYLIST_VIEWER,
    GO_TO_SYSTEM_SCREEN,
    GO_TO_SHORTCUTMENU,
    GO_TO_ALBUM_COVERS,
    /* Reserved block of main-menu shortcuts, one per direct tag-browse row of
     * tagnavi.config's root ("main") menu (Album/Artist/Genre/etc), looked up
     * by browser_db_get_main_menu_row() rather than hardcoded per-tag cases
     * so they survive tagnavi.config edits/reordering. TAGNAVI_MAIN_MENU_SLOTS
     * covers today's 8 such rows with headroom for a customized
     * tagnavi_user.config; browser_db_get_main_menu_tag_row_count() is used to
     * hide slots beyond the real row count from the Customize Main Menu
     * screen and the default-enabled root menu, not just cap this block. */
    GO_TO_TAGNAVI_FIRST,
    GO_TO_TAGNAVI_LAST = GO_TO_TAGNAVI_FIRST + TAGNAVI_MAIN_MENU_SLOTS - 1,
    /* Returned by album_covers.c's SELECT handler: enter the database
     * browser directly at the tapped album's track list (see
     * browser_db_enter_album_tracks_on_next_load()). browser() special-cases
     * this so that backing all the way out of that browse session lands on
     * Album covers instead of the generic root menu. */
    GO_TO_ALBUM_COVERS_TRACKS,
    /* Artist portraits: the coverflow carousel over the album-artist list
     * (screens/covers/artist_portraits.c). Selecting an artist reuses
     * GO_TO_ALBUM_COVERS_TRACKS to open its album listing, and BACK from
     * there returns here via the normal previous-screen tracking. */
    GO_TO_ARTIST_PORTRAITS,
    /* Reopen the most recently read document in the text viewer, which
     * restores its own reading position (see text_viewer_last_document()).
     * Append below here, never insert: these values are persisted in
     * global_status.last_screen and global_settings.start_in_screen, so
     * renumbering them changes what a saved config means. */
    GO_TO_LASTDOC,
    /* One album chart, chosen by album_charts_arm(). Reached only from the
     * database browser's Playback History rows, so it has no root-menu entry
     * of its own -- but it still needs a code, because that is how a browse
     * level hands control back. */
    GO_TO_ALBUM_CHARTS,
    /* Play a random album (screens/browse/album_charts.c). */
    GO_TO_RANDOM_ALBUM,
    GO_TO_DB_SEARCH,
    /* Documents and Images: the flat lists (screens/browse/browser_flat.c). */
    GO_TO_DOCUMENTS,
    GO_TO_IMAGES,
    /* Spun: the playback-log statistics deck (viewers/playback_viewer/). */
    GO_TO_SPUN,
    /* The guests the library credits (screens/browse/featured_artists.c).
     * Reached only from the Music menu's built-in row, so it has no root-menu
     * entry -- but it still needs a code, because that is how a browse level
     * hands control back. */
    GO_TO_FEATURED_ARTISTS,
    /* One artist's guest appearances, armed by featured_artists_arm(). The
     * browser's [Featured In] row, and the same track list as above. */
    GO_TO_FEATURED_TRACKS,
    /* The two browser search boxes, reached from the Search row at the top of
     * the file browser's root and of the playlist catalogue. Neither has a row
     * in the root menu; the codes exist because that is how a browse level
     * hands control back. Only the first is dispatched from items[] --
     * catalog.c consumes the second itself, so that leaving the box returns to
     * the catalogue rather than to the root. */
    GO_TO_FILE_SEARCH,
    GO_TO_PLAYLIST_SEARCH,
};
extern struct menu_item_ex root_menu_;

/* Open the file browser at this file the next time it is entered, rather
   than where it was left. Cleared as soon as it is used. */
void browser_reveal_on_next_load(const char *path);

void root_menu_load_from_cfg(void* setting, char *value);
char* root_menu_write_to_cfg(void* setting, char*buf, int buf_len);
void root_menu_set_default(void* setting, void* defaultval);
bool root_menu_is_changed(void* setting, void* defaultval);


#endif /* __ROOT_MENU_H__ */
