/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to album_covers.c and the album-name display setting values.
 ****************************************************************************/
#ifndef _ALBUM_COVERS_H_
#define _ALBUM_COVERS_H_

#include "system/bg_task.h"

/* Values for global_settings.album_covers_show_album_name */
enum show_album_name_values {
    ALBUM_NAME_HIDE = 0,
    ALBUM_NAME_BOTTOM,
    ALBUM_NAME_TOP,
    ALBUM_AND_ARTIST_TOP,
    ALBUM_AND_ARTIST_BOTTOM
};

/* Values for global_settings.album_covers_sort_albums_by */
enum sort_albums_by_values {
    SORT_BY_ARTIST_AND_NAME = 0,
    SORT_BY_ARTIST_AND_YEAR,
    SORT_BY_YEAR,
    SORT_BY_NAME,

    SORT_VALUES_SIZE
};

/* Values for global_settings.album_covers_sort_artists_by. Ordering by plays
 * needs figures the artist list does not otherwise carry, so it costs a pass
 * over the database when the carousel opens -- which is why it is opt-in
 * rather than simply offered as another order. */
enum sort_artists_by_values {
    SORT_ARTISTS_BY_NAME = 0,
    SORT_ARTISTS_BY_PLAYS,
};

/* Values for global_settings.album_covers_year_sort_order */
enum year_sort_order_values {
    ASCENDING = 0,
    DESCENDING
};

/* Values for global_settings.album_covers_background: which of the theme's two
 * colours fills the carousel, the other drawing the captions. Both resolve
 * through dynamic colours. */
enum carousel_background_values {
    CAROUSEL_BG_FOREGROUND = 0,  /* the status bar's colour */
    CAROUSEL_BG_BACKGROUND       /* the same background as every other screen */
};

/* selected_file: jump to this file's album on open (e.g. context_menu_show.c's "Album
 * covers" context-menu item on a specific track); NULL for the normal
 * entry paths (main menu, WPS shortcuts) -- falls back to the currently
 * playing track's album, or wherever was last viewed. */
int album_covers(const char *selected_file);

/* The same carousel over the album-artist list, showing artist photos
 * (<artist>/folder.jpg). Selecting an artist opens that album-artist's album
 * listing in the database browser. selected_file is currently unused (always
 * starts at the first artist). */
int artist_portraits(const char *selected_file);

/* Opens the shared Carousel settings menu over a running carousel (theme and
 * activity switched so it is styled like the same menu reached from Settings),
 * and returns do_menu()'s result -- MENU_ATTACHED_USB included, which the
 * caller must handle. Applying whatever changed is the caller's job, since the
 * album and artist models care about different settings. */
int carousel_settings_menu(void);

/* The carousel's own cached state, for the standard triggers.
 *
 * bg_task_rebuild() forces it to be built again the next time Album covers
 * opens; bg_task_update() fills in what is missing instead. Both also tell the
 * album index to rebuild, which is the part that is a real background task --
 * see album_covers_request(). */
extern struct bg_task album_covers_task;

#endif
