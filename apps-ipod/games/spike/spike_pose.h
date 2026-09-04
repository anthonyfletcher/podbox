/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The character: the easing the world scrolls on, the pose tables the
 * triangle is drawn from, and the curves that carry it between levels.
 *
 * Authoring character here means editing numbers rather than drawing
 * frames, so the tables are meant to be tuned and will need many passes.
 ****************************************************************************/

#ifndef SPIKE_POSE_H
#define SPIKE_POSE_H

#include <stdbool.h>
#include <stdint.h>

/* Sub-beat phase is 0..SPK_PHASE-1 everywhere, so one shift converts it to a
 * table index and the same number indexes the scroll and the pose. */
#define SPK_PHASE        256

/* Six numbers, and every ounce of the triangle's character is in them.
 *
 * base_tilt carries the foot as well as the angle: the body pivots on the
 * leading base corner where it is positive and on the trailing one where it
 * is negative, which is how one number makes the triangle hop from foot to
 * foot rather than tipping the same way every beat.
 *
 * Trap: rotation is int16_t where the design has int8_t. A jump turns
 * through a full circle and 360 does not fit in a signed byte; storing it
 * in coarser units to keep the byte would cost more in the reader than the
 * two bytes are worth. */
struct spk_pose
{
    int8_t  apex_dx;      /* apex offset from base centre -- the lean */
    int8_t  half_width;   /* squash and stretch */
    int8_t  height;
    int16_t rotation;     /* degrees about the centroid; airborne only */
    int8_t  base_tilt;    /* degrees; which foot, and how far onto it */
    int8_t  y_offset;     /* off the surface, positive downward */
    int8_t  x_offset;     /* along it, positive forward */
};

/* Where in its cell the world sits, 0..SPK_PHASE across one beat. Front
 * loaded, so most of the movement is over by three fifths of the beat and
 * the rest is the settle -- the "move, and, move, and" of the design. */
int spk_ease(int phase);

/* The pose tables. Phase runs 0..SPK_PHASE-1 over one beat, except the jump,
 * whose phase covers both of its beats.
 *
 * 'strong' is the dominant foot, and alternates every beat. It picks which
 * corner the body lands on and how hard, so the two are never the same hop.
 *
 * A jump takes the foot it is going to land on rather than the one under the
 * beat it is in, because the end of the turn tilts onto it: the touchdown is
 * then continuous, and there are exactly two of them. */
/* One-beat gymnastic moves, 'move' choosing which. In order: forward
 * somersault, tuck spin, up into a handstand, out of it, back somersault, a
 * jack, up onto one corner, off it, a reach, a bow, a sway, two bounces.
 *
 * Four of the twelve never leave the floor. That is the point of them: a
 * routine of nothing but tricks reads as one long trick, and the moves that
 * only stretch or lean are what give the ones that turn something to be
 * different from.
 *
 * Pairs have to stay adjacent -- two and three are one move, and so are six
 * and seven. The first of each ends held in a position the second starts
 * from, and separating them leaves a body that snaps out of a handstand
 * without coming down.
 *
 * Every move otherwise starts and ends square and on the floor, so any two
 * can follow each other in any order. A move that turns finishes on a whole
 * circle for the same reason, whichever way round it went.
 *
 * The wait for a tempo does not use these: the triangle walks through it,
 * because breaking off a gymnastic move to start walking is a jolt at the
 * one moment the player is being asked to feel a tempo. This is the
 * high-score screen's, which is where a body with nothing to do and
 * something to celebrate belongs. */
#define SPK_IDLE_MOVES   12
void spk_pose_idle(struct spk_pose *out, int phase, int move);

/* Which move to do after 'prev'. Random, except that it will not repeat and
 * will not strand half of a pair -- the table knows which of its moves end
 * held, so the choosing belongs with it rather than with the screen. */
int spk_pose_idle_next(int prev);

void spk_pose_hop(struct spk_pose *out, int phase, bool strong);
/* 'ride' is how far above the surface the body arrives, in pixels: the
 * height of whatever it came down on and is about to push flat. Zero for an
 * ordinary landing. */
void spk_pose_land(struct spk_pose *out, int phase, bool strong, int ride);

/* Touching anything solid -- a creature or a block, walked into or jumped
 * into. There is only one of these because there is only one thing that
 * happened: the body met something that was not going to move. */
void spk_pose_ouch(struct spk_pose *out, int phase);
/* Coming down out of the sky onto the cell a run restarts from. Shape only:
 * how high it started is the caller's, since it depends on the level. */
void spk_pose_drop(struct spk_pose *out, int phase, bool strong);
void spk_pose_jump(struct spk_pose *out, int phase, bool land_strong);
void spk_pose_fall(struct spk_pose *out, int phase, bool from_air,
                  bool strong);

/* Level of the player during a step and during an airborne move, 8.8 fixed
 * so the caller can place it between two surfaces. The curve peaks on
 * 'apex' whatever the drop on the far side -- two levels above the take-off
 * for a press, the top surface off a spring. */
int spk_walk_level(int from, int to, int phase);
int spk_arc_level(int from, int to, int phase, int apex);

/* The three vertices of the triangle, base centred on (bx, by). */
void spk_pose_points(const struct spk_pose *p, int bx, int by, int pt[3][2]);

#endif /* SPIKE_POSE_H */
