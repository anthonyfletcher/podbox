/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The grid engine: the course, and the rules that move the player through
 * it. Nothing here knows about pixels, beats in milliseconds or input --
 * one call crosses one beat boundary, and that is the whole interface.
 ****************************************************************************/

#ifndef SPIKE_WORLD_H
#define SPIKE_WORLD_H

#include <stdbool.h>

/* Surfaces sit at four discrete levels; 0 is the ground. */
#define SPK_LEVELS       4

/* What a press buys, and the two halves of it are not the same number.
 *
 * The arc *rises* SPK_ARC_UP levels: that is what carries the body into a
 * raised block at SPK_BLOCK_LEVEL, and what puts a diamond two levels up
 * within reach of a press. But it *lands* no more than SPK_CLIMB_UP above
 * where it left.
 *
 * They differ on purpose. A jump that could land wherever its arc reached
 * made the middle of the field pointless -- from the ground it always took
 * the highest surface within two, so a platform at level 1 was never the
 * thing anybody stood on. Climbing one at a time is what gives each level a
 * turn, and it is why a step needs none: the press is the whole ladder. */
#define SPK_ARC_UP       2
#define SPK_CLIMB_UP     1

/* What the player is doing over the beat that has just begun. Each of them
 * covers exactly one cell per beat, which is the invariant the whole design
 * rests on: the scroll rate is the tempo, whatever the player does. */
enum spk_motion
{
    SPK_WALK,        /* one cell, from -> to */
    SPK_ARC_RISE,    /* first beat of a jump, airborne over the next cell */
    SPK_ARC_FALL,    /* second beat, ending on the landing */
    SPK_ARC_SPRING   /* a rise off a spring: the same beat, three levels */
};

/* How a beat boundary ended. Four ways to die and they are all different,
 * because what killed the player is the only thing the death has to say. */
enum spk_outcome
{
    SPK_OK = 0,
    SPK_FELL,        /* nothing within reach to land on */
    SPK_HIT,         /* walked into a creature from the side */
    SPK_CRUSHED,     /* a block came down on the cell being stepped into */
    SPK_BONKED       /* jumped into the underside of a raised block */
};

struct spk_state
{
    int  beat;      /* beats run, which is also the cell occupied */
    int  level;     /* the surface under it; -1 while airborne */

    enum spk_motion motion;  /* what covers beat -> beat+1 */
    int  from;      /* level the motion starts at */
    int  to;        /* level it ends at, or -1 for nothing to land on */

    /* The height an airborne move reaches, settled when it is committed and
     * read for the whole two beats of it -- a press reaches two above the
     * take-off and a spring reaches the top surface, and the falling half
     * has no other way to know which of the two it is falling out of. */
    int  apex;

    bool landed;    /* the boundary just crossed ended a jump */
    bool got;       /* ...collected a diamond */
    bool threw;     /* ...came down on a switch */
    int  got_level; /* ...and from what level, for the drawing */
    bool stomped;   /* ...came down on a creature */

    /* Set by the arrival that found a spring and cleared by the move that
     * begins off it, both inside one call. It is in the state rather than a
     * local because the two are a beat's arrival and a beat's departure, and
     * only the state crosses between them. */
    bool sprung;
};

/* Surface bits of a cell, one per level. The course is assembled ahead of
 * the player as it is asked for, so every index from the run's first cell
 * onwards is valid and there is no end to run off.
 *
 * 'mask' is what the cell holds and 'live' is what is there this moment,
 * which differ only where a switch has not been thrown. 'promised' is what
 * throwing it would add, and is what the drawing dashes in -- not everything
 * the mask has over the live surfaces, because a swapping cell keeps its
 * platforms in the mask after the switch has taken them away and a dashed
 * line there would offer something that is never coming. */
unsigned int spk_world_mask(int cell);
unsigned int spk_world_live(int cell);
unsigned int spk_world_promised(int cell);

/* A block that rides up and down on a four-beat pattern.
 *
 * It fills exactly one level of one cell: the ground when it is down, and
 * SPK_BLOCK_LEVEL when it is raised -- which is the height a jump from the
 * ground passes through. So one block kills a step on some beats and a jump
 * on the others, and the verb that is safe is whichever one the block is not
 * using -- which makes it the one obstacle here that punishes jumping.
 * Everything else in the course teaches that a press is the way out of
 * trouble, and this is where that habit is turned around.
 *
 * Anything at another level goes past untouched. A platform over a block is
 * a way round it and not a trap, and a jump that starts a level up sails
 * over a raised one.
 *
 * The pattern is four bits because it has to be *read*, not memorised: the
 * player watches a block for a bar and then knows it. */
#define SPK_BLOCK_LEVEL  2

bool spk_world_blocks(int cell);
bool spk_world_block_up(int cell, int beat);

/* A switch, and the platforms that wait on one.
 *
 * It has to be *landed* on, from above, exactly as a creature is stomped --
 * walking over it does nothing. That is the whole of the obstacle: the
 * player has to spend a press on something that is not in their way, for a
 * reward that is not on their path.
 *
 * A switch holds up platforms in its own pattern and no further: one is live
 * at a time, and leaving the pattern that carried it turns it off.
 *
 * A switched cell usually keeps its floor -- only the platforms over it
 * wait -- so missing the switch costs the high route and not the run. A cell
 * marked SPK_C_SFF as well waits with its floor, and that is still not the
 * run: one cell of missing floor is one cell to jump, so the price of
 * missing that switch is the press it would have saved. */
bool spk_world_has_switch(int cell, int *level);
bool spk_world_switch(int cell, int *level);
bool spk_world_switched(int cell);
bool spk_world_switch_on(int cell);

/* What else is in a cell, and at which level. Both answer false once taken;
 * what has been taken is remembered for the last few dozen cells, which is
 * further back than the field is drawn. */
bool spk_world_diamond(int cell, int *level);
bool spk_world_creature(int cell, int *level);

/* A creature with a spike on its head, which is fatal from above as well as
 * from the side. Everything else in the course teaches that a press is the
 * way out of trouble; the rising block turns that around once, and this
 * turns it around again -- the only way past is the jump that clears the
 * cell entirely, which is the stomp's own verb aimed one cell further.
 *
 * Asked only where there is a creature: it says which kind, not whether. */
bool spk_world_spiked(int cell);

/* A spring, and it is only ever on the ground.
 *
 * Landed on from above it launches, and walked over it does nothing -- the
 * switch's rule, for the same reason: a thing that must be gone to. The
 * launch reaches the top surface, which is a height no press from level 0
 * can reach, so a spring is how the top of the field is used at all.
 *
 * Missing one costs nothing. It is a reward and not a hazard, which is why
 * it is absent from spk_state_avoided() -- nothing was avoided. */
bool spk_world_spring(int cell);

/* The highest surface in a cell at or below max_level, or -1 for none.
 *
 * A step passes its own level as the ceiling and a jump passes SPK_CLIMB_UP
 * above it, and that one difference is the whole climbing rule: only a jump
 * gains height, and only one level of it at a time. Note that this is not
 * the height the arc reaches -- see SPK_ARC_UP.
 *
 * Both drop any distance, which is why sliding off an edge is not a
 * separate verb -- and why a surface out of reach is not a wall to be
 * stopped by but simply nothing to land on. The player walks off the edge
 * and falls, exactly as over a hole. */
int spk_world_surface(int cell, int max_level);

/* First cell at or after 'from' standing on ground with two clear beats
 * after it and nothing in them, which is what a respawn needs. */
int spk_world_respawn(int from);

/* Forget what the line that has just ended did: diamonds taken, creatures
 * stomped, and the switch it threw. spk_state_start() does this as well; this
 * is for the caller that has to ask the world questions in between, and the
 * switch is why -- a floor that is only there while it is thrown is not a
 * floor to respawn on. */
void spk_world_forget(void);

void spk_state_start(struct spk_state *st, int cell);

/* ...and the same, standing somewhere other than the ground.
 *
 * Nothing in the game needs it: a run opens at level 0 and a respawn returns
 * there. It is for the validator, which cannot otherwise prove a phrase that
 * is entered above the floor -- and once phrases do that, "every pattern is
 * crossable" stops covering the most intricate ones in the library. */
void spk_state_start_at(struct spk_state *st, int cell, int level);

/* Cross a beat boundary: apply the motion that has finished and choose the
 * next. Anything but SPK_OK leaves the state standing where it was, with the
 * camera stopped -- the death plays where it happened, and the world freezes
 * there for a beat. */
enum spk_outcome spk_state_advance(struct spk_state *st);

/* What the move under way will end in, without ending it. A contact has to
 * be shown at the moment the bodies meet rather than at the boundary the
 * beat happens to end on, and that moment is part-way through the beat --
 * so the screen has to be able to ask ahead. */
enum spk_outcome spk_state_peek(const struct spk_state *st);

/* Whether the move the player did not make would have ended the line, which
 * is what an obstacle is: a hole, a surface out of reach, a creature at
 * walking level, a block that is down -- one rule and no list. False on flat
 * ground, and false through the second half of a jump, where there was no
 * choice to make. Asked before the boundary is crossed; nothing mutates. */
bool spk_state_avoided(const struct spk_state *st);

/* Spend the marks the boundary just crossed left behind: a diamond taken, a
 * creature stomped, a switch thrown.
 *
 * They are one-beat marks. The drawing plays each of them off the phase, so
 * they last exactly as long as the beat they belong to and the next advance
 * clears them -- but an advance that ends in a death returns before it gets
 * that far, and no further boundary is coming to try again. Left set, they
 * go on describing a beat that ended, and the phase cycling underneath them
 * replays the animation on every beat of the death and of the skip after
 * it. This is what the death calls instead. */
void spk_state_end_beat(struct spk_state *st);

/* Commit a jump to the boundary just crossed. False where one is already
 * under way, which is what collapses repeat presses inside one window. */
bool spk_state_jump(struct spk_state *st);

#endif /* SPIKE_WORLD_H */
