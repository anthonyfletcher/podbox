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
#include "screens/system/art_health.h"

/* The settings each view mode owns are named for it and grouped together under
 * View Mode, and each is hidden while the other mode is selected -- a row that
 * does nothing is worse than a missing one, and the prefix says which mode a
 * row belongs to without having to switch back to find out.
 *
 * Transition speed is in the 3D group and not the shared one: the flat view
 * replaces the whole speed expression it feeds (see update_scroll_animation()),
 * timing itself off the wheel instead. Scroll speed is genuinely shared -- flat
 * scales its own durations by it in flat_ticks(). */
static int tilt_only_callback(int action,
                              const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM
        && global_settings.album_covers_view_mode == CAROUSEL_VIEW_FLAT)
        return ACTION_EXIT_MENUITEM;
    return action;
}

static int flat_only_callback(int action,
                              const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM
        && global_settings.album_covers_view_mode != CAROUSEL_VIEW_FLAT)
        return ACTION_EXIT_MENUITEM;
    return action;
}

MENUITEM_SETTING(album_covers_view_mode, &global_settings.album_covers_view_mode, NULL);
MENUITEM_SETTING(album_covers_pile_fade, &global_settings.album_covers_pile_fade, flat_only_callback);
MENUITEM_SETTING(album_covers_pile_offset, &global_settings.album_covers_pile_offset, flat_only_callback);
MENUITEM_SETTING(album_covers_center_margin, &global_settings.album_covers_center_margin, tilt_only_callback);
MENUITEM_SETTING(album_covers_slide_tuck, &global_settings.album_covers_slide_tuck, tilt_only_callback);
MENUITEM_SETTING(album_covers_parallel_slides, &global_settings.album_covers_parallel_slides, tilt_only_callback);
MENUITEM_SETTING(album_covers_scroll_speed, &global_settings.album_covers_scroll_speed, NULL);
MENUITEM_SETTING(album_covers_transition_speed, &global_settings.album_covers_transition_speed, tilt_only_callback);
MENUITEM_SETTING(album_covers_show_album_name, &global_settings.album_covers_show_album_name, NULL);
MENUITEM_SETTING(album_covers_on_select, &global_settings.album_covers_on_select, NULL);
MENUITEM_SETTING(album_covers_sort_albums_by, &global_settings.album_covers_sort_albums_by, NULL);
MENUITEM_SETTING(album_covers_sort_artists_by, &global_settings.album_covers_sort_artists_by, NULL);
MENUITEM_SETTING(album_covers_year_sort_order, &global_settings.album_covers_year_sort_order, NULL);
MENUITEM_SETTING(album_covers_show_year, &global_settings.album_covers_show_year, NULL);
MENUITEM_SETTING(album_covers_background, &global_settings.album_covers_background, NULL);
MENUITEM_SETTING(album_covers_statusbar, &global_settings.album_covers_statusbar, NULL);

/* Rebuilding and updating the cache are not here any more: they sit with every
 * other rebuild and update under Library > Maintenance, where the difference
 * between them can be read off the screen rather than remembered. This screen
 * keeps what is genuinely about the artwork cache -- how it builds, and what it
 * could not find. */

/* Off decodes the source image once per thumbnail size; on decodes it once for
 * the largest and derives the rest from that. Only affects thumbnails generated
 * from now on -- rebuild the cache to apply it to what is already there. */
MENUITEM_SETTING(art_cache_fast_build, &global_settings.art_cache_fast_build,
                 NULL);

MENUITEM_SETTING(debug_log_artcache, &global_settings.debug_log_artcache, NULL);

/* What the last completed pass could find no artwork for. The counts have
 * always been in Background Tasks; these are the folders behind them. */
static int art_health_albums(void)
{
    art_health_screen(false);
    return 0;
}
MENUITEM_FUNCTION(art_health_albums_item, 0, ID2P(LANG_ART_HEALTH_ALBUMS),
                  art_health_albums, NULL, Icon_NOICON);

static int art_health_artists(void)
{
    art_health_screen(true);
    return 0;
}
MENUITEM_FUNCTION(art_health_artists_item, 0, ID2P(LANG_ART_HEALTH_ARTISTS),
                  art_health_artists, NULL, Icon_NOICON);

MAKE_MENU(art_cache_menu, ID2P(LANG_ART_CACHE_MENU), NULL, Icon_NOICON,
            &art_cache_fast_build,
            &art_health_albums_item,
            &art_health_artists_item,
            &debug_log_artcache);

/* The chain the slides are drawn through, one slot at a time in the order
 * listed. Cover Flow's own rather than shared with the browser rows: it is not
 * a skinned screen, so a setting is the only way it can be told. */
MENUITEM_SETTING(album_covers_filter_1, &global_settings.album_covers_filter[0], NULL);
MENUITEM_SETTING(album_covers_filter_2, &global_settings.album_covers_filter[1], NULL);
MENUITEM_SETTING(album_covers_filter_3, &global_settings.album_covers_filter[2], NULL);
MAKE_MENU(album_covers_filter_menu, ID2P(LANG_ARTWORK_FILTER), NULL, Icon_NOICON,
            &album_covers_filter_1,
            &album_covers_filter_2,
            &album_covers_filter_3);

MAKE_MENU(album_covers_menu, ID2P(LANG_CAROUSEL_SETTINGS), NULL, Icon_NOICON,
            &album_covers_on_select,
            &album_covers_show_album_name,
            &album_covers_show_year,
            &album_covers_background,
            &album_covers_statusbar,
            &album_covers_year_sort_order,
            &album_covers_sort_albums_by,
            &album_covers_sort_artists_by,
            &album_covers_view_mode,
            /* Whichever mode is selected, its own settings follow it. */
            &album_covers_center_margin,
            &album_covers_slide_tuck,
            &album_covers_parallel_slides,
            &album_covers_transition_speed,
            &album_covers_pile_fade,
            &album_covers_pile_offset,
            &album_covers_filter_menu,
            /* Shared, so last. */
            &album_covers_scroll_speed);
