/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to spike_draw.c: the line art, and the geometry it is drawn
 * on. Every constant here is a literal because there is one target and the
 * numbers divide cleanly into it.
 ****************************************************************************/

#ifndef SPIKE_DRAW_H
#define SPIKE_DRAW_H

#include <stdbool.h>
#include "games/spike/spike_world.h"

/* Ten cells across the panel, exactly. Twenty pixels a level separates them
 * at a glance, which §15 is right to care about: a level the player cannot
 * count is a hole they cannot read. */
#define SPK_CELL_PX      32
#define SPK_LEVEL_PX     20
#define SPK_PLATFORM_H   10

/* How tall the field has to be, measured rather than guessed.
 *
 * The highest anything reaches is a jump taken from the topmost platform:
 * the arc peaks two levels above where it left, and the body stands a
 * further SPK_BODY_PX above that. Nothing in the game can be drawn higher, so
 * anything above it is empty sky -- and a band of empty sky above the action
 * makes the action look small.
 *
 * SPK_BODY_PX is the worst case over every pose at every phase, from
 * ~/bgrender/top: the hop reaches 36 and the ride down off a stomped
 * creature 43, which is measured from the surface the creature stands on. */
#define SPK_BODY_PX      44
#define SPK_FIELD_H      (SPK_LEVEL_PX * (SPK_LEVELS + 1) + SPK_BODY_PX)

/* ...and it sits in the middle of the panel: the rule under the score down to
 * the ground line is SPK_FIELD_H, and that block is what is centred, so the
 * band above the rule and the band below the floor come out equal.
 *
 * The score and the caption share the band above the rule rather than taking
 * rows of their own, so the field is where it is whatever the caption is
 * doing. */
#define SPK_BAND_H       ((LCD_HEIGHT - SPK_FIELD_H) / 2)

/* ...and then the whole picture is dropped by this much.
 *
 * Arithmetically the block is already centred; optically it is not, because
 * the band above the rule carries the score and the caption while the one
 * below the floor carries nothing, and a full band reads smaller than an
 * empty one of the same height. Ten pixels is what it takes to look level.
 * The drop moves everything together, so nothing inside the band moves
 * against anything else. */
#define SPK_HUD_DROP     10
#define SPK_HUD_H        (SPK_BAND_H + SPK_HUD_DROP)
#define SPK_RULE_INSET   4      /* the rule stops where the score does */

/* Where something that tall sits in the band above the rule. */
#define SPK_BAND_Y(h)    (SPK_HUD_DROP + (SPK_BAND_H - (h)) / 2)
#define SPK_GROUND_Y     (SPK_HUD_H + SPK_FIELD_H)
#define SPK_HUD_SCALE    2
#define SPK_HUD_Y        ((SPK_HUD_H - 7 * SPK_HUD_SCALE) / 2)
#define SPK_FIELD_TOP    SPK_HUD_H

/* The player's column, which leaves seven cells of preview ahead of it. It
 * never moves: every motion covers exactly one cell per beat, so the world
 * scrolls under a triangle that stays put. */
#define SPK_PLAYER_COL   2
#define SPK_PLAYER_X     (SPK_PLAYER_COL * SPK_CELL_PX + SPK_CELL_PX / 2)

/* Flushed every frame: from the drop down to the bottom of the hooks, and no
 * further either way. Anchoring the top at zero would send the rows the drop
 * left empty, which is the whole of what the drop would otherwise cost --
 * the picture moved, it did not grow.
 *
 * The
 * strip below that is background and stays background, so sending it thirty
 * times a second buys nothing -- the one time it has to go is when the
 * palette changes underneath it, which spk_draw_flush() handles. */
#define SPK_UPDATE_TOP   SPK_HUD_DROP
#define SPK_UPDATE_H     (SPK_GROUND_Y + 8)


/* Two deaths, and they are the only two things that can happen: the ground
 * was not there, or something solid was. Which obstacle it was does not need
 * saying -- the player can see it. */
enum spk_death
{
    SPK_DEATH_LEDGE,     /* stepped off an edge */
    SPK_DEATH_AIR,       /* jumped short: already falling when it ran out */
    SPK_DEATH_OUCH       /* touched something solid, whatever it was */
};

/* Everything one frame needs, gathered so the drawing has no state of its
 * own and can be read against a single instant. */
struct spk_frame
{
    const struct spk_state *st;
    int  phase;          /* sub-beat, 0..SPK_PHASE-1 */
    unsigned long now_ms; /* grid time, which is what the motes fall on */
    bool strong;         /* the beat's dominant foot */
    bool frozen;         /* the field holds still: a death, or the count-in */

    int  death_phase;    /* 0..SPK_PHASE-1 while dying, -1 otherwise */
    int  death_scroll;   /* where the scroll stopped, in 256ths of a cell */
    enum spk_death death_kind;
    bool skipping;       /* the beats between a death and a respawn */

    long score;
    int  multiplier;

    bool waiting;        /* no tempo yet: the run has not begun */

    /* The face the whole band is set in -- the score, the count-in and the
     * caption. Not the block glyphs: those are digits and capitals, and a
     * track name is neither.
     *
     * The caption is NULL when the setting is off; the font is loaded either
     * way, because the score is in it whatever the caption is doing.
     *
     * Scrolled here rather than by the scroll engine: that thread repaints
     * on its own schedule into a viewport it keeps a pointer to, and this
     * band is cleared and redrawn thirty times a second over a viewport that
     * lives on a stack frame. */
    const char *caption;
    int  caption_w;      /* the title's width, measured when it changed */
    bool caption_scroll; /* ...and whether it travels or is simply cropped */

    /* Where every character of it begins, in pixels from the start. The face
     * is proportional -- the .fnt carries a width table, whatever the source
     * face was called -- so a character is not a fixed step and the only way
     * to cut between two of them is to know where they are.
     *
     * Measured once when the caption changes: font_get_width() is a cache
     * lookup and a miss is a read off the disk, which is not a thing to do
     * per glyph per frame. Runs one past the title, over the blank that
     * separates it from itself when it comes round. */
    const short *caption_at;
    int  caption_chars;
    int  font;

    /* Which step of the caption's scroll this is -- one a beat, counted on a
     * clock that only goes forward. Not now_ms: the grid's clock is steered
     * toward the middle of the chunk the position report names, so it is
     * allowed to end a frame behind the one before it. */
    int  caption_step;

    /* The volume, 0 to 100, for the moment after the wheel has moved -- and
     * -1 the rest of the time. It takes the caption's room rather than
     * finding room of its own: the wheel is easy to brush and the answer to
     * "what did I just do" is wanted for a second, where the track's name is
     * wanted for as long as nothing else is happening. */
    int  volume;

    /* The run is past the best there has been. Said on the field, while it
     * is happening, rather than at the end: a player leaving the game is on
     * their way somewhere and a screen in the way is not a reward. */
    bool crowned;
};

/* What the end of a run reports. Both modes answer with a score; only Run
 * answers with a distance, because "how long did you last" is the question
 * that mode asks and a score alone does not answer it. */
struct spk_result
{
    long score;
    long best;           /* what it had to beat, before this run */
    long beats;
    bool crowned;        /* ...and did */
    bool run;            /* Run rather than Song */
};

/* Drawing and flushing are separate calls so a caller can time them
 * apart. On this panel they are not the same order of cost and the fixes
 * for them have nothing in common: one is answered by drawing less, the
 * other only by sending fewer rows. */
void spk_draw_frame(const struct spk_frame *f);
void spk_draw_flush(void);

/* Forget what was shed into the world: a new run has nothing behind it. */
void spk_draw_reset(void);

/* Send the whole panel on the next flush. The ordinary flush covers the
 * field and the band above it and no more, so anything that has drawn
 * outside those -- the menu -- has to say so or its last rows stay. */
void spk_draw_full_flush(void);

/* The end of a run: the score, what it beat, and a body with nothing left
 * to do turning somersaults beside it. 'phase' and 'move' drive the
 * gymnastics and come off wall time -- the music may have stopped, and this
 * screen is not on the grid. */
void spk_draw_result(const struct spk_result *r, int phase, int move);

#endif /* SPIKE_DRAW_H */
