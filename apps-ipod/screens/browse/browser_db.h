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

int browser_db_export(void);
int browser_db_import(void);
void browser_db_init(void) INIT_ATTR;
int browser_db_enter(struct browser_context* c, bool is_visible);
void browser_db_exit(struct browser_context* c, bool is_visible);
int browser_db_load(struct browser_context* c);
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
/* Returns the tag and raw (P2STR-resolvable) display name of the Nth (0-based)
 * such row. Returns false if index is out of range. */
bool browser_db_get_main_menu_tag_row(int index, int *out_tag,
                                   const unsigned char **out_name);
int browser_db_get_filename(struct browser_context* c, char *buf, int buflen);
int browser_db_get_custom_action(struct browser_context* c);
bool browser_db_get_subentry_filename(char *buf, size_t bufsize);
bool browser_db_subentries_do_action(struct tagcache_search *tcs,
                                     bool (*action_cb)(const char *file_name),
                                     char *buf, size_t buf_sz);

#endif
