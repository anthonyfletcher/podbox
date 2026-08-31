/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The game's own menu, on held Menu.
 *
 * Drawn by the player's menu code and not by the game's, so it is skinned
 * like every other menu and looks like the machine it is running on. The
 * field is drawn by hand because it is a field; a list of options is a
 * list, and the theme already knows how to draw one.
 *
 * The music stops while it is open, and it has to: a run left playing under
 * a menu is a run whose clock has moved past anything the field could be
 * caught up to. The caller pauses.
 ****************************************************************************/

#include <stdio.h>
#include "config.h"
#include "lang.h"
#include "settings/settings.h"      /* set_int, set_option */
#include "speech/talk.h"            /* STR, UNIT_MS */
#include "widgets/menu.h"
#include "widgets/list.h"
#include "root_menu.h"              /* GO_TO_ROOT */
#include "system/activity.h"
#include "games/spike/spike_bar.h"
#include "games/spike/spike_menu.h"
#include "games/spike/spike_score.h"

/* What the caller lends the menu for as long as it is open. Two of them are
 * written back through; the rest are read-outs. */
static struct spk_menu *state;


/** High scores **/

static const char *spk_score_name(int selected, void *data, char *buffer,
                                  size_t buffer_len)
{
    (void)data;

    spk_score_row(selected, buffer, (int)buffer_len);

    return buffer;
}

static int spk_scores_screen(void)
{
    struct simplelist_info info;

    simplelist_info_init(&info, str(LANG_SPIKE_SCORES), spk_score_rows(),
                         NULL);
    info.get_name = spk_score_name;

    /* Zero whatever it says. simplelist_show_list() reports "left for the
     * root" for a plain MENU *and* for a USB attach, and Menu here means
     * one screen back and nothing more -- passing that on took the player
     * out of the game entirely. USB still leaves, through do_menu, which
     * can tell the two apart. */
    simplelist_show_list(&info);

    return 0;
}


/** What the game has to say about itself **/

static int spk_info_screen(void)
{
    struct simplelist_info info;
    int hits, margin10;

    simplelist_info_init(&info, str(LANG_SPIKE_INFO), 0, NULL);
    simplelist_reset_lines();

    /* The track's tempo and the game's, separately: they differ by a power
     * of two, and a grid that feels wrong is either a tempo the tracker got
     * wrong or an octave that suited the grid and not the music. One line
     * cannot tell those apart. */
    if (state->bpm > 0)
    {
        simplelist_addline("Track %d bpm", state->bpm);
        simplelist_addline("Grid %d ms", state->beat_ms);
    }
    else
        simplelist_addline("No tempo lock");

    /* The downbeat's evidence rather than its verdict. Found or not found
     * is one word for a decision taken on a margin, and a margin of 11 and
     * a margin of 40 are very different things to be told. */
    spk_bar_working(&hits, &margin10);
    if (state->bar >= 0)
        simplelist_addline("Bar %d of 4", state->bar + 1);
    else
        simplelist_addline("Bar unknown");
    simplelist_addline("  from %d onsets, %d.%d x", hits, margin10 / 10,
                       margin10 % 10);

    if (state->waiting)
    {
        simplelist_addline("Waiting: %d beats", state->listen_beats);
        simplelist_addline("  confidence %d, %u windows",
                           state->listen_conf, state->listen_windows);
    }

    simplelist_addline("Track time %lu.%lus", state->clock_ms / 1000,
                       (state->clock_ms % 1000) / 100);

    if (state->presses > 0)
        simplelist_addline("Your presses %+d ms (%d)", state->mean_ms,
                           state->presses);
    else
        simplelist_addline("Your presses --");

    simplelist_addline("%d fps", state->fps);
    simplelist_addline("Draw %d ms, flush %d ms", state->draw_ms,
                       state->flush_ms);

    simplelist_show_list(&info);

    return 0;
}


/** The menu **/

static int spk_offset_setting(void)
{
    set_int(str(LANG_SPIKE_OFFSET), " ms", UNIT_MS, state->offset_ms, NULL,
            SPK_OFFSET_STEP, -SPK_OFFSET_MAX, SPK_OFFSET_MAX, NULL);

    return 0;
}

static int spk_tempo_setting(void)
{
    static const struct opt_items names[] = {
        { STR(LANG_SPIKE_TEMPO_HALF) },
        { STR(LANG_SPIKE_TEMPO_HEARD) },
        { STR(LANG_SPIKE_TEMPO_DOUBLE) },
    };
    int shift = *state->shift + 1;

    set_option(str(LANG_SPIKE_TEMPO), &shift, RB_INT, names,
               ARRAYLEN(names), NULL);

    if (shift - 1 != *state->shift)
    {
        *state->shift = shift - 1;
        state->tempo_changed = true;
    }

    return 0;
}

MENUITEM_FUNCTION(spk_scores_item, 0, ID2P(LANG_SPIKE_SCORES),
                  spk_scores_screen, NULL, Icon_NOICON);
MENUITEM_FUNCTION(spk_offset_item, 0, ID2P(LANG_SPIKE_OFFSET),
                  spk_offset_setting, NULL, Icon_NOICON);
MENUITEM_FUNCTION(spk_tempo_item, 0, ID2P(LANG_SPIKE_TEMPO),
                  spk_tempo_setting, NULL, Icon_NOICON);
MENUITEM_FUNCTION(spk_info_item, 0, ID2P(LANG_SPIKE_INFO),
                  spk_info_screen, NULL, Icon_NOICON);

/* The one row here that is a real setting rather than a screen of its own:
 * it is a display preference and has to survive the session, so it lives in
 * settings_list.c and is reached from here like everything else. Taking
 * effect needs the field re-placed, which is why leaving the menu re-enters
 * the game rather than resuming it. */
MENUITEM_SETTING(spk_caption_item, &global_settings.spike_caption, NULL);

MAKE_MENU(spike_menu, ID2P(LANG_SPIKE), NULL, Icon_NOICON,
          &spk_scores_item, &spk_caption_item, &spk_offset_item,
          &spk_tempo_item, &spk_info_item);

bool spike_menu_show(struct spk_menu *m)
{
    int result;

    state = m;
    m->tempo_changed = false;

    push_current_activity(ACTIVITY_CONTEXTMENU);
    result = do_menu(&spike_menu, NULL, NULL, false);
    if (get_current_activity() == ACTIVITY_CONTEXTMENU)
        pop_current_activity();

    /* Menu is one screen back, all the way out to the game -- leaving the
     * game itself is asked for on the field, where it is asked *about*.
     * USB is the one thing that takes the screen without asking anyone. */
    return result == MENU_ATTACHED_USB;
}
