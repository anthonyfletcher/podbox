/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Settings menu for Spun (viewers/playback_viewer).
 ****************************************************************************/

#include <stdbool.h>
#include "config.h"
#include "lang.h"
#include "settings/settings.h"
#include "widgets/menu.h"
#include "draw/icon_bitmaps.h"

/* Artwork is the only thing here worth a switch, and it is worth one for two
 * reasons rather than the obvious one.
 *
 * A card with no sleeve draws a pattern instead, which is a deliberate part
 * of the card language rather than a placeholder -- so turning artwork off is
 * a look, not a degradation. And the slots it needs are the largest fixed
 * claim Spun makes on the working memory, so switching it off hands that back
 * to the statistics model and a large library loses fewer rows to it.
 *
 * Top Rows and Achievement Order are the two the content genuinely leaves
 * open: ten is a choice rather than a fact, and a wall of a hundred and
 * seventy badges has no single right order -- what you want from it depends
 * on whether you are admiring what you have or hunting what you have not.
 *
 * What is deliberately NOT here: the frame cap and the artwork slot count are
 * measurements, not preferences; the year is a gesture; and turning the
 * motion off saves a fifth of a second of boost per step, which is not worth
 * a row. A settings screen fills up on its own and does not need help. */
MENUITEM_SETTING(spun_artwork, &global_settings.spun_artwork, NULL);
MENUITEM_SETTING(spun_top_count, &global_settings.spun_top_count, NULL);
MENUITEM_SETTING(spun_badge_order, &global_settings.spun_badge_order, NULL);
MENUITEM_SETTING(spun_rank_by, &global_settings.spun_rank_by, NULL);

MAKE_MENU(spun_menu, ID2P(LANG_SPUN), NULL, Icon_NOICON,
          &spun_artwork,
          &spun_top_count,
          &spun_badge_order,
          &spun_rank_by);
