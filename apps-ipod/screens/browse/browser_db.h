/***************************************************************************
 * Original code from RockBox
 * was: apps/tagtree.h
 * Copyright (C) 2005 by Miika Pekkarinen
 * GNU General Public License (version 2+)
 *
 * Interface to browser_db.c.
 ****************************************************************************/
#ifndef _BROWSER_DB_H
#define _BROWSER_DB_H

#include "config.h"
#include "database/tagcache.h"
#include "browser.h"

#define TAGNAVI_VERSION    "#! rockbox/tagbrowser/2.0"
#define TAGMENU_MAX_ITEMS  64
#define TAGMENU_MAX_MENUS  32
#define TAGMENU_MAX_FMTS   32

/* global_settings.database_sort_albums_by: how an album list is ordered. */
enum database_sort_albums {
    DB_SORT_ALBUMS_NAME = 0,
    DB_SORT_ALBUMS_YEAR,
    DB_SORT_ALBUMS_YEAR_DESC,
};

int browser_db_export(void);
int browser_db_import(void);
void browser_db_init(void) INIT_ATTR;
int browser_db_enter(struct browser_context* c, bool is_visible);
void browser_db_exit(struct browser_context* c, bool is_visible);
int browser_db_load(struct browser_context* c);
/* True when BACK should leave the browser rather than pop a level: the
 * current level is the one a main-menu shortcut entered at, and the menu it
 * would pop to is the one the shortcut existed to skip. */
bool browser_db_back_exits(const struct browser_context *c);
/* The top row a freshly loaded level wants shown (it opens scrolled past its
 * <All tracks>/<Random> rows), or -1 for no preference. Clears the request, so
 * only the load that made it is affected -- call once, right after selecting. */
int browser_db_take_pending_top_item(void);
char* browser_db_get_entry_name(struct browser_context *c, int id,
                                    char* buf, size_t bufsize);
bool browser_db_current_playlist_insert(int position, bool queue);
int browser_db_add_to_playlist(const char* playlist, bool new_playlist);
char *browser_db_get_title(struct browser_context* c);
int browser_db_get_attr(struct browser_context* c);
bool browser_db_is_album_list(struct browser_context* c);
bool browser_db_is_artist_list(struct browser_context* c);
/* True when the path down to the browser's current level came through an artist
 * level. Recorded on a playlist built from here, for the "auto" WPS art
 * source. */
bool browser_db_current_under_artist_level(void);
bool browser_db_get_album_dir(struct browser_context* c, int item,
                           char *buf, int buflen);
bool browser_db_get_artist_dir(struct browser_context* c, int item,
                            char *buf, int buflen);
int browser_db_get_icon(struct browser_context* c);
/* Arms a one-shot jump: the next time browser_db_load() sees a fresh root load
 * (dirlevel 0, TABLE_ROOT), it enters the root menu's row whose first tag
 * matches 'tag' (e.g. tag_album), looked up by tag identity rather than
 * position so it's robust to tagnavi.config reordering. Used by
 * root_menu.c's tagnavi-derived main-menu shortcuts. */
void browser_db_enter_by_tag_on_next_load(int tag);
/* The same for a caller that means one specific row rather than one kind of
 * browse. A first tag does not identify a row -- the shipped menu's "Album"
 * and "Recently Added" both start on tag_album -- so anything picking a row
 * off a list arms with this. The label lives in RAM only, so nothing about how
 * the choice is stored changes. */
void browser_db_enter_by_label_on_next_load(const unsigned char *label);
/* The same for a root row that opens a nested menu rather than browsing a tag
 * -- "Playback History ==> runtime" and its like. Armed by the submenu's
 * config id, again identity rather than position. */
void browser_db_enter_menu_on_next_load(const char *menu_id);
/* Arms a direct jump: root -> straight into that specific album's own track
 * list, identified by its tagcache seek (not name/position), skipping the
 * intermediate "Album" grouping listing entirely. Used by
 * apps/gui/album_covers.c so selecting a cover lands directly on that
 * album's tracks in the core database browser, with a single BACK press
 * exiting straight back out (no intermediate level to unwind through). */
void browser_db_enter_album_tracks_on_next_load(long album_seek,
                                             const char *album_title);
/* As above, but jumps straight to a specific album-artist's album listing
 * (identified by seek), for Artist portraits (apps/gui/album_covers.c). A single
 * BACK returns to the carousel; selecting an album descends into its tracks. */
void browser_db_enter_artist_albums_on_next_load(long albumartist_seek,
                                              const char *artist_title);
/* Number of direct tag-browse ("->") rows in the root ("main") menu -- rows
 * that load a nested sub-menu ("==>") or trigger an action (e.g. "~>"
 * shuffle) don't count. Used by root_menu.c to know how many of its reserved
 * GO_TO_TAGNAVI_FIRST..LAST slots are backed by a real row. */
int browser_db_get_main_menu_tag_row_count(void);
/* The Nth (0-based) such row: its raw (P2STR-resolvable) display name, and
 * whichever identity it has -- *out_tag for a tag-browse row, *out_menu_id for
 * a submenu row, the other left as -1/NULL. Feed whichever came back to the
 * matching enter_..._on_next_load() above. False if index is out of range. */
bool browser_db_get_main_menu_row(int index, int *out_tag,
                                  const char **out_menu_id,
                                  const unsigned char **out_name);
/* The Music menu's rows, for the screen that turns them on and off
 * (screens/music_menu_config.c). Only rows the menu would actually draw are
 * enumerated, so one already hidden for another reason -- an empty submenu, or
 * Search while the database is not in RAM -- is not offered as a choice.
 *
 * 'index' is the row's position in tagnavi.config's root menu, which is what
 * global_settings.music_menu_hidden is keyed on. That mask only means anything
 * for the row set it was chosen against, so it is stored alongside
 * browser_db_root_row_signature() and discarded when the two disagree. */
int browser_db_root_row_count(void);
bool browser_db_get_root_row(int n, int *out_index,
                             const unsigned char **out_name);
int browser_db_root_row_signature(void);
bool browser_db_root_row_is_hidden(int index);

int browser_db_get_filename(struct browser_context* c, char *buf, int buflen);
int browser_db_get_custom_action(struct browser_context* c);
bool browser_db_get_subentry_filename(char *buf, size_t bufsize);
bool browser_db_subentries_do_action(struct tagcache_search *tcs,
                                     bool (*action_cb)(const char *file_name),
                                     char *buf, size_t buf_sz);

#endif
