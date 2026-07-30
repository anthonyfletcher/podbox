/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Settings menu for the Album Covers screen.
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include "config.h"
#include "lang.h"
#include "input/action.h"
#include "settings/settings.h"
#include "widgets/menu.h"
#include "widgets/yesno.h"
#include "widgets/splash.h"
#include "draw/icon_bitmaps.h"
#include "screens/covers/album_covers.h"
#include "metadata/art_cache.h"

MENUITEM_SETTING(album_covers_center_margin, &global_settings.album_covers_center_margin, NULL);
MENUITEM_SETTING(album_covers_slide_tuck, &global_settings.album_covers_slide_tuck, NULL);
MENUITEM_SETTING(album_covers_parallel_slides, &global_settings.album_covers_parallel_slides, NULL);
MENUITEM_SETTING(album_covers_scroll_speed, &global_settings.album_covers_scroll_speed, NULL);
MENUITEM_SETTING(album_covers_transition_speed, &global_settings.album_covers_transition_speed, NULL);
MENUITEM_SETTING(album_covers_show_album_name, &global_settings.album_covers_show_album_name, NULL);
MENUITEM_SETTING(album_covers_sort_albums_by, &global_settings.album_covers_sort_albums_by, NULL);
MENUITEM_SETTING(album_covers_year_sort_order, &global_settings.album_covers_year_sort_order, NULL);
MENUITEM_SETTING(album_covers_show_year, &global_settings.album_covers_show_year, NULL);

/* Artwork only. Rebuild purges every cached thumbnail so they regenerate from
 * source; update fills in what is missing -- the cache thread idles once it has
 * covered the current track count, so artwork added to already-indexed folders
 * (artist photos especially, dropped in long after the music) is never picked
 * up on its own.
 *
 * These do not touch the carousel's album index. That is a list of albums and
 * artists read out of the database, holding no artwork at all, so the two go
 * stale for entirely different reasons: artwork when files change on disk, the
 * index when the database does. Rebuilding one to fix the other only costs
 * time. The index actions live in the carousel menu below. */
static int art_cache_menu_rebuild(void)
{
    if (yesno_pop_confirm(ID2P(LANG_REBUILD_CACHE)))
        bg_task_rebuild(&art_cache_task);
    return 0;
}
MENUITEM_FUNCTION(art_cache_rebuild_item, 0, ID2P(LANG_REBUILD_CACHE),
                  art_cache_menu_rebuild, NULL, Icon_NOICON);

static int art_cache_menu_update(void)
{
    if (yesno_pop_confirm(ID2P(LANG_UPDATE_CACHE)))
        bg_task_update(&art_cache_task);
    return 0;
}
MENUITEM_FUNCTION(art_cache_update_item, 0, ID2P(LANG_UPDATE_CACHE),
                  art_cache_menu_update, NULL, Icon_NOICON);

/* Index only -- the album/artist list the carousel scrolls through. Rebuild
 * discards it and reads the database again; update keeps the existing covers
 * where they still apply. Either takes effect on the next carousel open. */
static int carousel_menu_rebuild_index(void)
{
    if (yesno_pop_confirm(ID2P(LANG_REBUILD_INDEX)))
        bg_task_rebuild(&album_covers_task);
    return 0;
}
MENUITEM_FUNCTION(carousel_rebuild_index_item, 0, ID2P(LANG_REBUILD_INDEX),
                  carousel_menu_rebuild_index, NULL, Icon_NOICON);

static int carousel_menu_update_index(void)
{
    if (yesno_pop_confirm(ID2P(LANG_UPDATE_INDEX)))
        bg_task_update(&album_covers_task);
    return 0;
}
MENUITEM_FUNCTION(carousel_update_index_item, 0, ID2P(LANG_UPDATE_INDEX),
                  carousel_menu_update_index, NULL, Icon_NOICON);

/* Off decodes the source image once per thumbnail size; on decodes it once for
 * the largest and derives the rest from that. Only affects thumbnails generated
 * from now on -- rebuild the cache to apply it to what is already there. */
MENUITEM_SETTING(art_cache_fast_build, &global_settings.art_cache_fast_build,
                 NULL);

MENUITEM_SETTING(debug_log_artcache, &global_settings.debug_log_artcache, NULL);

MAKE_MENU(art_cache_menu, ID2P(LANG_ART_CACHE_MENU), NULL, Icon_Audio,
            &art_cache_fast_build,
            &debug_log_artcache,
            &art_cache_rebuild_item,
            &art_cache_update_item);

MAKE_MENU(album_covers_menu, ID2P(LANG_CAROUSEL_SETTINGS), NULL, Icon_Audio,
            &album_covers_show_album_name,
            &album_covers_show_year,
            &album_covers_year_sort_order,
            &album_covers_sort_albums_by,
            &album_covers_center_margin,
            &album_covers_slide_tuck,
            &album_covers_parallel_slides,
            &album_covers_scroll_speed,
            &album_covers_transition_speed,
            &carousel_rebuild_index_item,
            &carousel_update_index_item);
