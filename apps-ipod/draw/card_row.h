/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to card_row.c -- the geometry and motion of a scrolling row of
 * cards. See .specifications/spun-card-engine.md.
 ****************************************************************************/
#ifndef _CARD_ROW_H
#define _CARD_ROW_H

#include <stdbool.h>

/* This file draws nothing and knows nothing about pixels. It answers one
 * question per frame -- which cards are on screen, where, and how much of
 * each -- and the caller blits that.
 *
 * The separation is what makes the row testable on a host with no LCD, and
 * it is why every hard part below is integer arithmetic rather than a
 * drawing routine. */

/* A card owning the run of PV_SUB cards that immediately follows it. Only
 * these respond to card_row_toggle(), and only these carry the arrow. */
#define CARD_ROW_PARENT  0x01
/* A card belonging to the parent above it. Folded -- width zero -- unless
 * that parent is the open one. */
#define CARD_ROW_SUB     0x02

/* One card's share of a frame, already clipped to the view.
 *
 * 'src_x' is how far into the card's own bitmap the visible part starts, so
 * a card hanging off the left edge needs no special case in the caller. */
struct card_row_item
{
    short index;
    short src_x;
    short dst_x;
    short w;
};

struct card_row
{
    /* The row, supplied by the caller and not owned here. 'cur_w' and
     * 'from_w' are the two written to: a card's width animates from where it
     * was when the last fold began toward its target, and a folded card's
     * target is zero. */
    const short         *full_w;
    const unsigned char *flags;
    short               *cur_w;
    short               *from_w;
    int n;

    int focus;          /* card the row settles on */
    int open;           /* the expanded parent, or -1 */

    /* The scroll chases its target on a fixed timestep; 'accum' is the
     * millisecond remainder a frame did not spend. A fold runs on a clock of
     * its own, because it has a definite start and no moving target. */
    int scroll;         /* px */
    int scroll_to;
    int accum;
    int fold_t;

    int view_w;
    int focus_x;        /* where the focused card's left edge settles */
    int gap;
    int fold_w;         /* what a folded card shows of itself */
};

/* 'cur_w' and 'from_w' must each be an array of n shorts the caller keeps
 * for the row's life. Everything else is read-only. */
void card_row_init(struct card_row *r, const short *full_w,
                   const unsigned char *flags, short *cur_w, short *from_w,
                   int n, int view_w, int focus_x, int gap);

/* What a folded card shows of itself, in pixels. Zero -- the default -- means
 * it disappears entirely.
 *
 * A few pixels instead leaves the run visible as a set of spines, the way
 * books stand on a shelf: a card that owns something says so by what is
 * stacked behind it rather than only by its arrow. A folded card is still not
 * a scroll stop, whatever it shows. */
void card_row_set_fold_w(struct card_row *r, int w);

/* Move the focus one card, skipping folded ones. False if it was already at
 * that end. */
bool card_row_step(struct card_row *r, int dir);

/* Put the focus on a card outright and snap the row to it, for a caller
 * restoring a position rather than moving one. Clamped, and a folded card is
 * not a valid resting place, so the nearest one that is takes it. */
void card_row_set_focus(struct card_row *r, int idx);

/* Unfold the focused card's sub-cards, or fold them again.
 *
 * Opening moves the focus into the run: what was asked for is the run, and
 * leaving it on the parent makes the next step one past a card already read.
 * Pressed on a sub-card it folds the run and takes the focus back to the
 * parent, so a user who has scrolled into one is not stranded there.
 *
 * False when the focused card has nothing to fold. */
bool card_row_toggle(struct card_row *r);

/* Advance every animation by 'dt' milliseconds. True while anything is still
 * moving, which is the caller's reason to draw another frame. */
bool card_row_tick(struct card_row *r, int dt);

/* The visible cards, left to right, into out[]. Returns how many. */
int card_row_plan(const struct card_row *r, struct card_row_item *out,
                  int max);

/* Where a card sits in row coordinates, and the row's full width. Exposed
 * for tests and for anything that needs to reason about the row as a whole;
 * the drawing path needs neither. */
int card_row_card_x(const struct card_row *r, int idx);
int card_row_total_w(const struct card_row *r);

#endif /* _CARD_ROW_H */
