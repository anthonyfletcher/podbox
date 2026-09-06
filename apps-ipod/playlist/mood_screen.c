/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Picking a mood or a journey, and playing what it names.
 *
 * Two menus over one handler, the way the maintenance rows are done: every
 * row here is the same call with a different number, so a table is fewer
 * lines than the rows it replaces and the rows cannot drift apart.
 *
 * Reached from the playlist catalogue rather than a menu of its own -- a
 * playlist built from how the music sounds is still a playlist, so it is
 * offered where the saved ones are.
 ****************************************************************************/

#include <stdbool.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lang.h"
#include "widgets/menu.h"
#include "widgets/splash.h"
#include "settings/settings.h"
#include "database/sound_mix.h"
#include "database/sound_mood.h"
#include "playlist/mood_screen.h"
#include "system/activity.h"

/* Whether a mood playlist started, so the caller knows to leave for the
 * playing screen rather than stay in the browser. */
static bool mood_started;

/* The journeys offered. Any two moods make one mechanically; these are the
 * pairs that describe a way an evening actually goes. */
static const struct journey_def {
    int lang;
    uint8_t from;
    uint8_t to;
} journeys[] = {
    { LANG_JOURNEY_CALM_ENERGETIC,       MOOD_CALM,       MOOD_ENERGETIC  },
    { LANG_JOURNEY_DARK_BRIGHT,          MOOD_DARK,       MOOD_BRIGHT     },
    { LANG_JOURNEY_SPARSE_DENSE,         MOOD_SPARSE,     MOOD_DENSE      },
    { LANG_JOURNEY_SLOW_FAST,            MOOD_SLOW,       MOOD_FAST       },
    { LANG_JOURNEY_MELANCHOLY_UPLIFTING, MOOD_MELANCHOLY, MOOD_UPLIFTING  },
};

/* One report for both menus: what came back is a count or one of the reasons
 * it could not be built, and neither depends on which row was chosen. */
static int mood_report(int added)
{
    /* 1 is what MENU_FUNC_CHECK_RETVAL reads as "leave the menu": the
     * playlist is already playing, so there is nothing left to choose. */
    if (added > 0)
    {
        mood_started = true;
        return 1;
    }

    switch (added)
    {
        case SOUND_MIX_NO_INDEX:
            splash(HZ * 2, ID2P(LANG_SOUND_MIX_NO_INDEX));
            break;
        case SOUND_MIX_NO_DB:
            splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
            break;
        case SOUND_MIX_NO_PLAYLIST:
            splash(HZ * 2, ID2P(LANG_SOUND_MIX_FAILED));
            break;
        case SOUND_MIX_CANCELLED:
            break;
        default:
            splash(HZ * 2, ID2P(LANG_SOUND_MIX_NONE));
            break;
    }

    return 0;
}

static int mood_run(void *param)
{
    splash(0, ID2P(LANG_WAIT));

    return mood_report(sound_mix_from_mood((intptr_t)param,
                                           global_settings.mix_length));
}

static int journey_run(void *param)
{
    const struct journey_def *j = &journeys[(intptr_t)param];

    splash(0, ID2P(LANG_WAIT));

    return mood_report(sound_mix_journey(j->from, j->to,
                                         global_settings.mix_length));
}

/* MENU_FUNC_CHECK_RETVAL, or the menu stays open over a playlist that has
 * already started. Icon_NOICON throughout: every row here is the same kind of
 * thing, so an icon column would be one glyph repeated. */
#define MOOD_ITEM(name, idx, lang)                                      \
    MENUITEM_FUNCTION_W_PARAM(name, MENU_FUNC_CHECK_RETVAL, ID2P(lang), \
                              mood_run, (void*)idx, NULL, Icon_NOICON)

MOOD_ITEM(mood_calm,       MOOD_CALM,       LANG_MOOD_CALM);
MOOD_ITEM(mood_energetic,  MOOD_ENERGETIC,  LANG_MOOD_ENERGETIC);
MOOD_ITEM(mood_dark,       MOOD_DARK,       LANG_MOOD_DARK);
MOOD_ITEM(mood_bright,     MOOD_BRIGHT,     LANG_MOOD_BRIGHT);
MOOD_ITEM(mood_warm,       MOOD_WARM,       LANG_MOOD_WARM);
MOOD_ITEM(mood_raw,        MOOD_RAW,        LANG_MOOD_RAW);
MOOD_ITEM(mood_lush,       MOOD_LUSH,       LANG_MOOD_LUSH);
MOOD_ITEM(mood_punchy,     MOOD_PUNCHY,     LANG_MOOD_PUNCHY);
MOOD_ITEM(mood_smooth,     MOOD_SMOOTH,     LANG_MOOD_SMOOTH);
MOOD_ITEM(mood_sparse,     MOOD_SPARSE,     LANG_MOOD_SPARSE);
MOOD_ITEM(mood_dense,      MOOD_DENSE,      LANG_MOOD_DENSE);
MOOD_ITEM(mood_hypnotic,   MOOD_HYPNOTIC,   LANG_MOOD_HYPNOTIC);
MOOD_ITEM(mood_slow,       MOOD_SLOW,       LANG_MOOD_SLOW);
MOOD_ITEM(mood_fast,       MOOD_FAST,       LANG_MOOD_FAST);
MOOD_ITEM(mood_melancholy, MOOD_MELANCHOLY, LANG_MOOD_MELANCHOLY);
MOOD_ITEM(mood_uplifting,  MOOD_UPLIFTING,  LANG_MOOD_UPLIFTING);

MAKE_MENU(moods_menu, ID2P(LANG_MOODS), NULL, Icon_NOICON,
          &mood_calm, &mood_energetic, &mood_dark, &mood_bright,
          &mood_warm, &mood_raw, &mood_lush, &mood_punchy,
          &mood_smooth, &mood_sparse, &mood_dense, &mood_hypnotic,
          &mood_slow, &mood_fast, &mood_melancholy, &mood_uplifting);

#define JOURNEY_ITEM(name, idx, lang)                                   \
    MENUITEM_FUNCTION_W_PARAM(name, MENU_FUNC_CHECK_RETVAL, ID2P(lang), \
                              journey_run, (void*)idx, NULL, Icon_NOICON)

JOURNEY_ITEM(journey_0, 0, LANG_JOURNEY_CALM_ENERGETIC);
JOURNEY_ITEM(journey_1, 1, LANG_JOURNEY_DARK_BRIGHT);
JOURNEY_ITEM(journey_2, 2, LANG_JOURNEY_SPARSE_DENSE);
JOURNEY_ITEM(journey_3, 3, LANG_JOURNEY_SLOW_FAST);
JOURNEY_ITEM(journey_4, 4, LANG_JOURNEY_MELANCHOLY_UPLIFTING);

MAKE_MENU(journeys_menu, ID2P(LANG_JOURNEYS), NULL, Icon_NOICON,
          &journey_0, &journey_1, &journey_2, &journey_3, &journey_4);

bool mood_screen_pick(bool journey)
{
    mood_started = false;

    /* Its own activity, and not the catalogue's. A theme decides whether a
     * list has an icon column from the activity it is drawn under, so
     * inheriting the browser's gives every row here the browser's column --
     * a column of one repeated glyph beside sixteen names. Choosing one of a
     * list of options is what this is. */
    push_current_activity(ACTIVITY_OPTIONSELECT);

    do_menu(journey ? &journeys_menu : &moods_menu, NULL, NULL, false);

    pop_current_activity();

    return mood_started;
}
