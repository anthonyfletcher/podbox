/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to spike_menu.c: the game's menu, drawn by the player's own
 * menu code so that it is skinned like every other one.
 ****************************************************************************/

#ifndef SPIKE_MENU_H
#define SPIKE_MENU_H

#include <stdbool.h>

/* The calibration slider's range and step, shared with the game so the two
 * cannot disagree about what the offset may be. */
#define SPK_OFFSET_MAX       250
#define SPK_OFFSET_STEP      5

/* What the caller lends the menu. The offset and the octave shift are
 * written back through; the rest are read-outs, gathered by the caller so
 * they all describe the same instant. */
struct spk_menu
{
    int *offset_ms;
    int *shift;
    bool tempo_changed;         /* ...and the grid has to be re-anchored */

    int beat_ms;
    int bpm;                    /* the track's, 0 where it never locked */
    int bar;                    /* the downbeat's place in four, -1 unknown */
    int mean_ms;
    int presses;
    int fps;
    int draw_ms;
    int flush_ms;

    /* Only while the tempo is still being waited for: after the latch the
     * analyser is stopped and its window count stands still. */
    bool waiting;
    int  listen_beats;
    int  listen_conf;
    unsigned int listen_windows;
    unsigned long clock_ms;     /* track time the grid is cut from */
};

/* Returns true where the menu was left for the root -- MENU held, or USB --
 * which the game has to pass on rather than swallow. */
bool spike_menu_show(struct spk_menu *m);

#endif /* SPIKE_MENU_H */
