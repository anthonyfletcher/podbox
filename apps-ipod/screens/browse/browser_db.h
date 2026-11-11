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
#include "widgets/list.h"               /* enum list_row_kind */

#define TAGNAVI_VERSION    "#! rockbox/tagbrowser/2.0"
#define TAGMENU_MAX_ITEMS  64
#define TAGMENU_MAX_MENUS  32
#define TAGMENU_MAX_FMTS   32

/* global_settings.database_sort_albums_by: how an album list is ordered. */
enum database_sort_albums {
    DB_SORT_ALBUMS_NAME = 0,
    DB_SORT_ALBUMS_YEAR,
    DB_SORT_ALBUMS_YEAR_DESC,
    DB_SORT_ALBUMS_ARTIST_NAME,
    DB_SORT_ALBUMS_ARTIST_YEAR,
    DB_SORT_ALBUMS_ARTIST_YEAR_DESC,
};

/* global_settings.database_sort_artists_by: how an artist list is ordered. */
enum database_sort_artists {
    DB_SORT_ARTISTS_NAME = 0,
    DB_SORT_ARTISTS_PLAYS,
};

/* global_settings.podcast_sort_shows_by: how the Podcasts list's shows are
 * ordered. */
enum podcast_sort_shows {
    PODCAST_SHOWS_NAME = 0,
    PODCAST_SHOWS_NEWEST,
};

/* The parent an album list hangs under, which is what decides its order.
 * The level above is the whole identity, so every menu grouping by the same
 * artist tag shares one slot: Genre, Year and the history lists all land in
 * DB_ALBUM_CTX_ALBUMARTIST alongside the Album Artist row. A parent that is
 * none of these -- anything a tagnavi_user.config invents -- has no slot and
 * takes database_sort_albums_by. */
enum db_album_sort_ctx {
    DB_ALBUM_CTX_ROOT = 0,      /* an album list with no level above it */
    DB_ALBUM_CTX_ARTIST,
    DB_ALBUM_CTX_ALBUMARTIST,
    DB_ALBUM_CTX_COMPOSER,
    DB_ALBUM_CTX_COUNT
};

/* That context's stored order: a DB_SORT_ALBUMS_* value, or -1 for "follow
 * database_sort_albums_by". Pass -1 to browser_db_album_sort_set() to put it
 * back. Neither call saves; the caller does. */
int browser_db_album_sort_get(int ctx);
void browser_db_album_sort_set(int ctx, int order);
/* Whether that context reads the artist-first orders as the plain ones. */
bool browser_db_album_sort_artist_first_moot(int ctx);

int browser_db_export(void);
int browser_db_import(void);
void browser_db_init(void) INIT_ATTR;
/* False when tagnavi.config is missing or did not parse: no menus to show */
bool browser_db_ready(void);
int browser_db_enter(struct browser_context* c, bool is_visible);
void browser_db_exit(struct browser_context* c, bool is_visible);
int browser_db_load(struct browser_context* c);
/* True when BACK should leave the browser rather than pop a level: the
 * current level is the one a main-menu shortcut entered at, and the menu it
 * would pop to is the one the shortcut existed to skip. */
bool browser_db_back_exits(const struct browser_context *c);
/* Forget any shortcut base, so BACK pops levels normally again. For a browse
 * that is starting on its own account rather than resuming a shortcut's jump. */
void browser_db_clear_shortcut_base(void);
/* The top row a freshly loaded level wants shown (it opens scrolled past its
 * <All tracks>/<Random> rows), or -1 for no preference. Clears the request, so
 * only the load that made it is affected -- call once, right after selecting. */
int browser_db_take_pending_top_item(void);
/* As browser_db_get_entry_name(), with what the row shows beside the name --
 * an album's year, under Show Year in Album Lists. For drawing only: the name
 * itself is what titles, keys and playlist names are made from. */
char* browser_db_get_display_name(struct browser_context *c, int id,
                                  char* buf, size_t bufsize);
char* browser_db_get_entry_name(struct browser_context *c, int id,
                                    char* buf, size_t bufsize);
bool browser_db_current_playlist_insert(int position, bool queue);
int browser_db_add_to_playlist(const char* playlist, bool new_playlist);
char *browser_db_get_title(struct browser_context* c);
int browser_db_get_attr(struct browser_context* c);
bool browser_db_is_album_list(struct browser_context* c);
bool browser_db_is_artist_list(struct browser_context* c);
/* True when this browse was reached through a menu row asking about the
 * `spoken` tag -- an audiobook list, at any of its levels. */
bool browser_db_is_spoken_list(struct browser_context* c);
/* The book row 'item' is, or is a track of: its book_resume_id(). In the
 * Audiobooks browse, and in any other while Segregate Audiobooks is on. False
 * for any other row. */
bool browser_db_get_book(struct browser_context* c, int item,
                         char *buf, size_t buflen);
/* And that book's album and album artist */
bool browser_db_get_book_album(struct browser_context* c, int item,
                               long *album_seek, long *artist_seek);
/* True when the path down to the browser's current level came through an artist
 * level. Recorded on a playlist built from here, for the "auto" WPS art
 * source. */
bool browser_db_current_under_artist_level(void);
/* The art-cache key of album or artist row 'item': its album folder, or the
 * artist folder above it. A track-artist row takes the album artist of the
 * same name, from the RAM copy only. 0 for any other row, or one with no
 * folder. */
unsigned int browser_db_get_art_hash(struct browser_context* c, int item);
/* The art key of the latest album of an artist row's album artist, 0 for
 * any other row or an artist who is no album artist */
unsigned int browser_db_get_artist_cover_hash(struct browser_context* c,
                                              int item);
int browser_db_get_icon(struct browser_context* c);
/* %Lk and %LP for row 'id' of the level on screen. */
enum list_row_kind browser_db_get_entry_kind(struct browser_context *c, int id);
bool browser_db_entry_is_playing(struct browser_context *c, int id);
/* The database entry 'id3' was read from, or -1 when it has none or was read
 * before the last Rebuild. */
struct mp3entry;
long browser_db_track_idx(const struct mp3entry *id3);

/* What the selected row of the current browse names, for a caller that wants
 * to ask the database about it rather than browse into it. NONE for a track
 * row, a special row, or a level grouped by anything else. */
enum browser_db_scope {
    BROWSER_DB_SCOPE_NONE = 0,
    BROWSER_DB_SCOPE_ALBUM,
    BROWSER_DB_SCOPE_ARTIST
};
enum browser_db_scope browser_db_current_scope(void);

/* The path of track row 'row', through a search the caller opened on
 * tag_filename; false for a row that is not a track */
struct tagcache_search;
bool browser_db_row_path(struct browser_context *c,
                         struct tagcache_search *tcs, int row,
                         char *buf, size_t size);

/* What an album's track list shows above itself, for the skin's %LH. Strings
 * are empty where the database names nothing; year and discs are 0. */
struct browser_db_header {
    char name[128];
    char artist[128];
    int year;
    int tracks;
    int discs;          /* the highest disc number, 0 for none */
    long length_ms;
    unsigned int art_hash;
    int album_row;      /* tagcache_album_get()'s, which the art is keyed by */
};
/* False unless the level on screen is an album's tracks in a music browse
 * and the album table is in RAM. A track list longer than the loaded page is
 * read from disk to count its tracks. */
bool browser_db_album_header(struct browser_context *c,
                             struct browser_db_header *h);

/* The selected row's name, as it reads on screen. */
char *browser_db_current_entry_name(char *buf, size_t bufsize);

/* Levels a browse can be deep, which is the ceiling on the filters below. */
#define BROWSER_DB_MAX_TAGS 5

/* The menus the firmware defines itself (add_builtin_menus()), by the ids
 * other screens open them by */
#define BROWSER_DB_MENU_AUDIOBOOKS "audiobooks"
#define BROWSER_DB_MENU_PODCASTS   "podcasts"

/* What scopes a tagcache search to the selected row: every grouping level
 * above it, then the row itself.
 *
 * Captured into the caller's own struct rather than applied straight to a
 * search, because reading the row can page a fresh chunk of the browse level
 * in -- and that borrows the app buffer, which a screen wanting this has
 * usually claimed. Capture before claiming, apply as often as needed after.
 *
 * The clauses live in here for the reason a search's do: tagcache holds them
 * by pointer until the search finishes.
 *
 * The ancestor levels are not decoration. A tag's seek names a string rather
 * than a thing, so two albums called "Greatest Hits" share one album seek and
 * it is the artist level above that tells them apart.
 *
 * The menu's own clauses are deliberately left out. They say which tracks the
 * browse is showing -- "Never played tracks" shows an album's unheard half --
 * and a caller here is asking about the album, not about the view of it. */
struct browser_db_filters {
    enum browser_db_scope scope;
    int count;                        /* levels in the arrays below */
    int tag[BROWSER_DB_MAX_TAGS];
    int seek[BROWSER_DB_MAX_TAGS];
    struct tagcache_search_clause numeric[BROWSER_DB_MAX_TAGS];
};

/* The selected row's scope. False, leaving *out empty, when there is no such
 * row -- i.e. whenever browser_db_current_scope() is NONE. */
bool browser_db_current_filters(struct browser_db_filters *out);

/* Narrow 'tcs' by them. False when tagcache would not take them all, which
 * leaves the search wider than asked for: a browse deeper than tagcache allows
 * filters, and a caller that has to report rather than count. */
bool browser_db_add_filters(struct tagcache_search *tcs,
                            struct browser_db_filters *f);

/* Arms a direct jump: root -> straight into that specific album's own track
 * list, identified by its tagcache seek (not name/position), skipping the
 * intermediate "Album" grouping listing entirely. Used by
 * screens/covers/album_covers.c so selecting a cover lands directly on that
 * album's tracks in the core database browser, with a single BACK press
 * exiting straight back out (no intermediate level to unwind through). */
void browser_db_enter_album_tracks_on_next_load(long album_seek,
                                             const char *album_title);
/* As above, but only that album artist's tracks of the album, where the root
 * has an albumartist -> album -> title row to filter through. */
void browser_db_enter_artist_album_tracks_on_next_load(long album_seek,
                                                   long albumartist_seek,
                                                   const char *album_title);
/* With an album jump armed, its track list opens on the playing track. */
void browser_db_select_playing_on_next_album_load(void);
/* The album of track 'track_idx' opened as though browsed to through the
 * Music row whose first tag is 'root_tag', on that track, so BACK climbs the
 * row. The album on its own, as above, where the row cannot reach it. */
void browser_db_walk_to_album_on_next_load(int root_tag, int track_idx,
                                           long album_seek, long artist_seek,
                                           const char *album_title);
bool browser_db_walk_pending(void);
/* Whether the last walk opened the album on its own; reading it clears it */
bool browser_db_take_walk_fallback(void);
/* As above, but jumps straight to a specific album-artist's album listing
 * (identified by seek), for Artist portraits
 * (screens/covers/artist_portraits.c). A single BACK returns to the carousel;
 * selecting an album descends into its tracks. */
void browser_db_enter_artist_albums_on_next_load(long albumartist_seek,
                                              const char *artist_title);
/* A submenu's row ("podcasts", "Podcasts") opened at one value of its first
 * level -- a show, a book, an author -- by seek, titled 'title'. A single
 * BACK leaves. Where the row is gone, the submenu or its row opens instead. */
void browser_db_enter_menu_row_value_on_next_load(const char *menu_id,
                                                  const char *label,
                                                  long seek, const char *title);
/* Number of main-menu slots the root ("main") menu numbers: its tag-browse
 * ("->") and submenu ("==>") rows, then those of the custom menu; action
 * rows (e.g. "~>" shuffle) don't count. Used by root_menu.c to know how many
 * of its reserved GO_TO_TAGNAVI_FIRST..LAST slots are numbered. */
int browser_db_get_main_menu_tag_row_count(void);
/* The main-menu slot index of the row opening the audiobooks menu, or -1.
 * Recognised by its rows asking about the `spoken` tag rather than by its
 * name, so a renamed or user-written menu still counts. */
int browser_db_spoken_main_menu_slot(void);
/* %Lr of a podcast episode, kept apart by Segregate Podcasts: how far
 * through it is, 0 not started and 100 heard to its end; -1 for any other
 * row. The episode by its database index, or by its list row. */
int browser_db_episode_percent(int idx_id);
int browser_db_get_entry_range(struct browser_context *c, int id);
/* %Lv of row 'id': a podcast show's episodes not started, or NULL with
 * none, and for any other row */
const char *browser_db_get_entry_value(struct browser_context *c, int id,
                                       char *buf, size_t size);
/* %LD of row 'id': a podcast episode's description, or for Play Unplayed
 * the episodes it plays; NULL for any other row */
char *browser_db_get_entry_description(struct browser_context *c, int id,
                                       char *buf, size_t size);
/* Whether the list on screen is of podcast episodes, and whether any of
 * them has notes */
bool browser_db_shows_episodes(struct browser_context *c);
bool browser_db_episodes_have_notes(struct browser_context *c);
/* Whether the list on screen is tracks newest first, numbered from the end,
 * or tracks whose order numbers nothing, not numbered at all */
bool browser_db_counts_down(struct browser_context *c);
bool browser_db_unnumbered(struct browser_context *c);
/* What spoken word the list on screen is of, as tag_virt_spoken answers:
 * 1 books, 2 podcasts, 0 either or neither */
int browser_db_list_spoken_kind(struct browser_context *c);

/* Row 'id' as a podcast episode, or -1 for any other row */
enum {
    BROWSER_DB_EPISODE_NOT_STARTED,
    BROWSER_DB_EPISODE_IN_PROGRESS,
    BROWSER_DB_EPISODE_FINISHED,
};
int browser_db_episode_state(struct browser_context *c, int id);

/* Mark row 'id', an episode, played or not; the episodes below it played;
 * or every episode of the show that row 'id' is. A place held goes. */
enum browser_db_mark {
    BROWSER_DB_MARK_PLAYED,
    BROWSER_DB_MARK_UNPLAYED,
    BROWSER_DB_MARK_OLDER,
    BROWSER_DB_MARK_SHOW,
};
bool browser_db_mark_episodes(struct browser_context *c, int id,
                              enum browser_db_mark how);
/* The finished episodes of the show row 'id' is, and deleting their files:
 * how many went. The database keeps them until its next Update. */
int browser_db_played_episodes(struct browser_context *c, int id);
int browser_db_delete_played(struct browser_context *c, int id);
/* The same over every show */
int browser_db_played_episodes_all(void);
int browser_db_delete_played_all(void);
/* The Nth (0-based) such row, and its raw (P2STR-resolvable) display name.
 * False if index is out of range, or names a submenu with nothing in it:
 * that slot is numbered but hidden, as is one PodBox has a row of its own
 * for. */
bool browser_db_get_main_menu_row(int index, const unsigned char **out_name);
/* Its %Lk: LIST_ROW_CUSTOM or LIST_ROW_CUSTOM_MENU for the user's own, as in
 * the Music menu, and LIST_ROW_ACTION for the rest. */
enum list_row_kind browser_db_get_main_menu_row_kind(int index);
/* Arm the jump into the Nth such row, for the next fresh root load: by its
 * label, or a submenu's id, since a first tag does not identify a row -- the
 * shipped menu's "Album" and "Recently Added" both start on tag_album. False
 * where browser_db_get_main_menu_row() is. */
bool browser_db_enter_main_menu_row_on_next_load(int index);
/* The same for the main menu's row opening the menu of id 'menu_id', and
 * then for the row of that menu labelled 'label', from which BACK leaves */
void browser_db_enter_menu_on_next_load(const char *menu_id);
void browser_db_enter_menu_row_on_next_load(const char *menu_id,
                                            const char *label);
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
/* The selected Music menu row's position in tagnavi.config's root menu, or
 * -1 when the browser is not on the Music menu itself */
int browser_db_selected_root_row(void);

int browser_db_get_filename(struct browser_context* c, char *buf, int buflen);
int browser_db_get_custom_action(struct browser_context* c);
bool browser_db_get_subentry_filename(char *buf, size_t bufsize);
bool browser_db_subentries_do_action(struct tagcache_search *tcs,
                                     bool (*action_cb)(const char *file_name),
                                     char *buf, size_t buf_sz);

/* Ratings, 0 to 10. The selected row's database entry, or -1 when it is not
 * a track; and the selected row's tracks all rated at once, returning how many
 * were or -1 when they could not be reached. */
long browser_db_selected_track_idx(void);
int browser_db_get_rating(long idx_id);
void browser_db_set_rating(long idx_id, int rating);
int browser_db_subentries_set_rating(int rating);

#endif
