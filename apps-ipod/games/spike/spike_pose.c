/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The pose tables and the curves between them.
 *
 * One rule governs all of it: every animation resolves exactly on a beat
 * boundary, and nothing crosses one unresolved. That is "no half movements"
 * applied to the drawing, and it is why each table is indexed by sub-beat
 * phase rather than counted in frames -- the frame rate can then vary
 * without the character ever arriving late.
 *
 * Parts, in order:
 *   - the easing
 *   - the tables
 *   - reading a table
 *   - the curves between levels
 *   - the triangle
 ****************************************************************************/

#include "config.h"
#include "fixedpoint.h"
#include "games/spike/spike_pose.h"

/* Rows in every table, so one shift turns a phase into an index. */
#define SPK_ROWS         16
#define SPK_ROW_SHIFT    4



/** The easing **/

/* 1-(1-t)^3 sampled at sixteenths, 0..SPK_PHASE, with a seventeenth entry so
 * the lerp below always has a pair. Nine tenths of the cell is covered by
 * three fifths of the beat and the rest is the settle. */
static const short ease_table[SPK_ROWS + 1] =
{
      0,  45,  85, 119, 148, 173, 194, 210,
    224, 235, 243, 248, 252, 254, 256, 256, 256
};

int spk_ease(int phase)
{
    int i = phase >> SPK_ROW_SHIFT;
    int frac = phase & ((1 << SPK_ROW_SHIFT) - 1);

    return ease_table[i]
           + (((ease_table[i + 1] - ease_table[i]) * frac) >> SPK_ROW_SHIFT);
}


/** The tables **/

/* Columns: apex_dx, half_width, height, rotation, base_tilt, y_offset. */

/* The hop. A wedge does not walk, it hops -- off the ground, onto one base
 * corner, and balanced there until the next one. Hop, balance, hop, balance,
 * with the hop over by two fifths of the beat so the landing arrives with
 * the cell.
 *
 * The table is written for the leading corner; spk_pose_hop() flips the sign
 * for the other beat, which moves the pivot to the trailing corner and lands
 * the body on the other foot. It also lands lighter there, because a real
 * hop has a dominant foot and perfect symmetry reads as machinery.
 *
 * The lean never returns to zero: apex_dx settles at +2, and a body tipped
 * in the direction of travel reads as confidence at every moment rather than
 * only during the hop. The last row leans back instead, coiling for the next
 * one -- anticipation is the cheapest animation principle there is. */
static const struct spk_pose hop_table[SPK_ROWS] =
{
    { -2, 12, 18, 0,   0,   0 , 0 },  /* coil */
    {  1,  8, 26, 0,   5,  -3 , 0 },  /* uncoil, leaving the ground */
    {  3,  9, 23, 0,  10,  -9 , 0 },
    {  3, 10, 22, 0,  14, -12 , 0 },  /* apex */
    {  3, 10, 22, 0,  15, -12 , 0 },  /* hang */
    {  3, 10, 22, 0,  16,  -8 , 0 },
    {  3,  9, 24, 0,  17,  -3 , 0 },  /* reaching for the foot */
    {  2, 13, 17, 0,  18,   0 , 0 },  /* land: squash onto it */
    {  2, 12, 19, 0,  18,   0 , 0 },
    {  2, 10, 23, 0,  18,   0 , 0 },  /* rebound */
    {  2, 10, 22, 0,  18,   0 , 0 },  /* balance */
    {  2, 10, 22, 0,  18,   0 , 0 },
    {  2, 10, 22, 0,  18,   0 , 0 },
    {  2, 10, 22, 0,  18,   0 , 0 },
    {  2, 10, 22, 0,  17,   0 , 0 },
    { -2, 11, 20, 0,  12,   0 , 0 }   /* coil for the next hop */
};

/* The warm-up, four beats of it, one move a beat: a forward somersault, a
 * tuck spin of two turns, up into a handstand, and out of it again. The last
 * two are a pair and have to stay adjacent -- move 3 begins inverted on the
 * apex, where move 2 left it.
 *
 * Rotation runs past 360 where a move turns more than once. fp14_sin() takes
 * degrees modulo a turn, so nothing has to be normalised here and the table
 * can say what it means. */
static const struct spk_pose idle_table[SPK_ROWS * 4] =
{
    /* 0: forward somersault */
    {  0, 12, 18,   0, 0,   0 , 0 },
    {  0,  8, 26,   0, 0,  -4 , 0 },
    {  0, 10, 22,  40, 0, -10 , 0 },
    {  0, 10, 22,  80, 0, -14 , 0 },
    {  0, 10, 22, 120, 0, -16 , 0 },
    {  0, 10, 22, 160, 0, -16 , 0 },
    {  0, 10, 22, 200, 0, -14 , 0 },
    {  0, 10, 22, 240, 0, -10 , 0 },
    {  0, 10, 22, 280, 0,  -6 , 0 },
    {  0, 10, 22, 320, 0,  -4 , 0 },
    {  0, 13, 17, 360, 0,   0 , 0 },
    {  0, 11, 21, 360, 0,   0 , 0 },
    {  0, 10, 22, 360, 0,   0 , 0 },
    {  0, 10, 22, 360, 0,   0 , 0 },
    {  0, 10, 22, 360, 0,   0 , 0 },
    {  0, 11, 20, 360, 0,   0 , 0 },

    /* 1: tuck spin -- small and fast, two turns, opened out to land */
    {  0, 12, 18,   0, 0,   0 , 0 },
    {  0,  9, 24,   0, 0,  -5 , 0 },
    {  0,  7, 14,  90, 0, -12 , 0 },
    {  0,  6, 12, 180, 0, -16 , 0 },
    {  0,  6, 12, 270, 0, -18 , 0 },
    {  0,  6, 12, 360, 0, -18 , 0 },
    {  0,  6, 12, 450, 0, -16 , 0 },
    {  0,  7, 14, 540, 0, -12 , 0 },
    {  0,  8, 18, 630, 0,  -7 , 0 },
    {  0, 10, 22, 720, 0,  -2 , 0 },
    {  0, 13, 17, 720, 0,   0 , 0 },
    {  0, 11, 21, 720, 0,   0 , 0 },
    {  0, 10, 22, 720, 0,   0 , 0 },
    {  0, 10, 22, 720, 0,   0 , 0 },
    {  0, 10, 22, 720, 0,   0 , 0 },
    {  0, 11, 20, 720, 0,   0 , 0 },

    /* 2: up into a handstand, and held there. The -7 is a third of the
       body's height: rotating half a turn about the centroid puts the apex
       that far below the base line, and this is what stands it back up. */
    {  0, 12, 18,   0, 0,   0 , 0 },
    {  0,  8, 26,   0, 0,  -4 , 0 },
    {  0, 10, 22,  30, 0, -10 , 0 },
    {  0, 10, 22,  70, 0, -14 , 0 },
    {  0, 10, 22, 110, 0, -14 , 0 },
    {  0, 10, 22, 150, 0, -11 , 0 },
    {  0, 10, 22, 175, 0,  -8 , 0 },
    {  0, 10, 22, 180, 0,  -9 , 0 },
    {  0, 10, 22, 180, 0,  -9 , 0 },
    {  0, 10, 23, 180, 0,  -9 , 0 },
    {  0, 10, 21, 180, 0,  -9 , 0 },
    {  0, 10, 22, 180, 0,  -9 , 0 },
    {  0, 10, 23, 180, 0,  -9 , 0 },
    {  0, 10, 22, 180, 0,  -9 , 0 },
    {  0, 10, 21, 180, 0,  -9 , 0 },
    {  0, 10, 22, 180, 0,  -9 , 0 },

    /* 3: and out of it, landing flat */
    {  0, 10, 22, 180, 0,  -9 , 0 },
    {  0, 10, 22, 185, 0,  -8 , 0 },
    {  0,  9, 24, 200, 0, -11 , 0 },
    {  0, 10, 22, 230, 0, -14 , 0 },
    {  0, 10, 22, 265, 0, -15 , 0 },
    {  0, 10, 22, 300, 0, -13 , 0 },
    {  0, 10, 22, 330, 0,  -9 , 0 },
    {  0, 10, 22, 350, 0,  -4 , 0 },
    {  0, 13, 17, 360, 0,   0 , 0 },
    {  0, 11, 21, 360, 0,   0 , 0 },
    {  0, 10, 22, 360, 0,   0 , 0 },
    {  0, 10, 22, 360, 0,   0 , 0 },
    {  0, 10, 22, 360, 0,   0 , 0 },
    {  0, 10, 22, 360, 0,   0 , 0 },
    {  0, 10, 22, 360, 0,   0 , 0 },
    {  0, 11, 20, 360, 0,   0 , 0 }
};

/* The jump, over both of its beats: crouch, launch, one full turn with the
 * apex leading on the rise and pointing down and forward on the descent,
 * and a hang at the top that costs nothing and adds weight.
 *
 * The turn closes on 360 four rows early, and those four tilt onto the foot
 * the body is about to land on. That is what makes the touchdown one of two
 * known poses instead of wherever the spin happened to finish: a press off
 * the beat is paid for by the arc starting part-way through, so the flight
 * is clipped and the arrival is not. Under no circumstances does the landing
 * slip, and it should not look as though it has. */
static const struct spk_pose jump_table[SPK_ROWS * 2] =
{
    {  0, 12, 16,   0, 0, 0 , 0 },    /* crouch */
    {  0, 13, 15,   0, 0, 0 , 0 },
    {  0,  7, 28,   0, 0, 0 , 0 },    /* launch */
    {  0,  8, 26,   5, 0, 0 , 0 },
    {  0,  9, 24,  15, 0, 0 , 0 },    /* rise */
    {  0, 10, 22,  28, 0, 0 , 0 },
    {  0, 10, 22,  43, 0, 0 , 0 },
    {  0, 10, 22,  58, 0, 0 , 0 },
    {  0, 10, 22,  73, 0, 0 , 0 },
    {  0, 10, 22,  88, 0, 0 , 0 },
    {  0, 10, 22, 103, 0, 0 , 0 },
    {  0, 10, 22, 117, 0, 0 , 0 },
    {  0, 10, 22, 131, 0, 0 , 0 },
    {  0, 10, 22, 145, 0, 0 , 0 },
    {  0, 10, 22, 158, 0, 0 , 0 },
    {  0, 10, 22, 170, 0, 0 , 0 },    /* apex */
    {  0, 10, 22, 176, 0, 0 , 0 },    /* hang */
    {  0, 10, 22, 182, 0, 0 , 0 },
    {  0, 10, 22, 194, 0, 0 , 0 },    /* descend */
    {  0, 10, 22, 207, 0, 0 , 0 },
    {  0, 10, 22, 220, 0, 0 , 0 },
    {  0, 10, 22, 233, 0, 0 , 0 },
    {  0, 10, 22, 246, 0, 0 , 0 },
    {  0, 10, 22, 259, 0, 0 , 0 },
    {  0, 10, 22, 276, 0, 0 , 0 },
    {  0, 10, 22, 300, 0, 0 , 0 },
    {  0, 10, 22, 324, 0, 0 , 0 },
    {  0, 10, 22, 345, 0, 0 , 0 },
    {  0, 10, 22, 360,  4, 0 , 0 },  /* square, and reaching for the foot */
    {  0, 10, 23, 360,  9, 0 , 0 },
    {  0,  9, 24, 360, 14, 0 , 0 },
    {  0,  9, 24, 360, 18, 0 , 0 }
};

/* The beat after a landing: no hop, because the jump was the hop.
 *
 * The foot does not move through any of it. The body arrives already on it,
 * squashes, recovers and balances there, and the tilt is the same 18 in
 * every row -- so there are two landings in the game and they are the same
 * two every time. Ramping the tilt up over the first rows instead makes the
 * touchdown a different pose on every jump, which is what reads as jagged. */
static const struct spk_pose land_table[SPK_ROWS] =
{
    {  0, 13, 16, 0,  18, 0 , 0 },    /* land: squash onto the foot */
    {  0, 13, 16, 0,  18, 0 , 0 },
    {  0, 12, 18, 0,  18, 0 , 0 },
    {  0, 11, 21, 0,  18, 0 , 0 },
    {  0,  9, 24, 0,  18, 0 , 0 },    /* recover: overshoot */
    {  0,  9, 24, 0,  18, 0 , 0 },
    {  1,  9, 24, 0,  18, 0 , 0 },
    {  1, 10, 23, 0,  18, 0 , 0 },
    {  2, 10, 22, 0,  18, 0 , 0 },    /* balance */
    {  2, 10, 22, 0,  18, 0 , 0 },
    {  2, 10, 22, 0,  18, 0 , 0 },
    {  2, 10, 22, 0,  18, 0 , 0 },
    {  2, 10, 22, 0,  18, 0 , 0 },
    {  2, 10, 22, 0,  18, 0 , 0 },
    {  2, 10, 22, 0,  17, 0 , 0 },
    { -2, 11, 20, 0,  12, 0 , 0 }
};

/* Touching something solid: creature or block, walked into or jumped into.
 *
 * There were three of these -- topple, crush and bonk -- and they were all
 * wrong for the same reason. A player is never under a block when it comes
 * down: it arrives on the beat they do, so they walk into its side or jump
 * into its face. Being flattened describes something that did not happen,
 * and it looked it.
 *
 * So: ouch. Squash against the thing, get thrown back and up off it, and
 * drop away behind. It reads the same whatever was hit and whatever the body
 * was doing when it hit it, which is the point -- the player does not need
 * to be told which obstacle it was, only that they touched one. */
static const struct spk_pose ouch_table[SPK_ROWS] =
{
    {  0, 11, 20, 0, 0,   0,   0 },   /* contact */
    {  0, 14, 15, 0, 0,   0,  -2 },   /* squashed against it */
    { -2, 13, 17, 0, 0,  -5,  -6 },   /* thrown off it */
    { -3, 11, 21, 0, 0, -11, -11 },
    { -3, 10, 22, 0, 0, -15, -15 },   /* top of the recoil */
    { -3, 10, 22, 0, 0, -14, -18 },
    { -2, 10, 22, 0, 0, -10, -21 },
    { -1, 10, 22, 0, 0,  -3, -23 },
    {  0, 10, 22, 0, 0,   6, -25 },   /* and past the floor */
    {  0, 10, 22, 0, 0,  18, -26 },
    {  0, 10, 22, 0, 0,  34, -27 },
    {  0, 10, 22, 0, 0,  54, -27 },
    {  0, 10, 22, 0, 0,  78, -27 },
    {  0, 10, 22, 0, 0, 106, -27 },
    {  0, 10, 22, 0, 0, 127, -27 },
    {  0, 10, 22, 0, 0, 127, -27 }
};

/* Falling. The step is taken, the body arrives where the ground is not, and
 * after a moment of no comment at all it goes straight down.
 *
 * §12.5 asks for a teeter and a corner hooked on the ledge instead, and says
 * that catch frame is the whole gag. It is a better drawing and a worse
 * joke: the deadpan is the joke, and a body that flails on the way down is
 * asking to be laughed at rather than being funny. Straight down also has
 * nothing to spiral about, which matters -- the spin had no foot to pivot
 * on and jumped whenever the wrong one was down.
 *
 * The tilt in the first rows is the foot the step was reaching for, so the
 * fall begins in the pose the walk left off in rather than snapping out of
 * it, and then straightens as the ground stops being relevant. */
static const struct spk_pose fall_table[SPK_ROWS] =
{
    {  0,  9, 24, 0,  18,   0 , 0 },  /* the step, taken */
    {  0,  9, 24, 0,  18,   0 , 0 },
    {  0,  9, 24, 0,  18,   1 , 0 },  /* ...onto nothing */
    {  0, 10, 23, 0,  15,   3 , 0 },
    {  0, 10, 22, 0,  10,   8 , 0 },  /* and then, simply, down */
    {  0, 10, 22, 0,   5,  16 , 0 },
    {  0, 10, 22, 0,   0,  27 , 0 },
    {  0, 10, 22, 0,   0,  41 , 0 },
    {  0, 10, 22, 0,   0,  58 , 0 },
    {  0, 10, 22, 0,   0,  78 , 0 },
    {  0, 10, 22, 0,   0, 101 , 0 },
    {  0, 10, 22, 0,   0, 127 , 0 },
    {  0, 10, 22, 0,   0, 127 , 0 },  /* gone */
    {  0, 10, 22, 0,   0, 127 , 0 },
    {  0, 10, 22, 0,   0, 127 , 0 },
    {  0, 10, 22, 0,   0, 127 , 0 }
};


/** Reading a table **/

static int spk_row(int phase, int rows)
{
    int i = (phase * rows) >> 8;

    if (i < 0)
        i = 0;
    else if (i > rows - 1)
        i = rows - 1;

    return i;
}

/* The weak beat lands on the other corner, which is the sign, and lands
 * lighter, which is the scale. */
static int8_t spk_foot(int tilt, bool strong)
{
    return (int8_t)(strong ? tilt : -(tilt * 2) / 3);
}

void spk_pose_idle(struct spk_pose *out, int phase, int move)
{
    *out = idle_table[(move & 3) * SPK_ROWS + spk_row(phase, SPK_ROWS)];
}

void spk_pose_hop(struct spk_pose *out, int phase, bool strong)
{
    *out = hop_table[spk_row(phase, SPK_ROWS)];
    out->base_tilt = spk_foot(out->base_tilt, strong);
}

void spk_pose_land(struct spk_pose *out, int phase, bool strong, int ride)
{
    int row = spk_row(phase, SPK_ROWS);

    *out = land_table[row];
    out->base_tilt = spk_foot(out->base_tilt, strong);

    if (ride <= 0)
        return;

    /* Landing on a creature means landing on its *head*, not on the ground.
     * The body arrives up where the thing was standing and rides it down as
     * it collapses, then pops clear.
     *
     * Squashing flat on the surface at row zero is what the first two
     * versions did, and it is why the crush could not be seen: the impact
     * was over before the first frame drew. There is nothing to watch being
     * squashed under a body that has already landed. */
    if (row < 4)
    {
        out->y_offset = (int8_t)(-(ride * (4 - row)) / 4);
        out->half_width = (int8_t)(out->half_width + row / 2);
        out->height = (int8_t)(out->height - row);
    }
    else if (row < 7)
    {
        /* And off it: the body must be seen to gain by the impact rather
         * than merely survive it. */
        int pop = ride / 2;

        out->y_offset = (int8_t)(-pop + (pop * (row - 4)) / 3);
    }
}

void spk_pose_jump(struct spk_pose *out, int phase, bool land_strong)
{
    *out = jump_table[spk_row(phase, SPK_ROWS * 2)];

    /* Only the last rows carry a tilt, and they take the sign of the foot
     * the body is coming down on rather than the beat it is still in. */
    out->base_tilt = spk_foot(out->base_tilt, land_strong);
}

void spk_pose_ouch(struct spk_pose *out, int phase)
{
    *out = ouch_table[spk_row(phase, SPK_ROWS)];
}

void spk_pose_fall(struct spk_pose *out, int phase, bool from_air,
                  bool strong)
{
    /* A jump that came up short was already falling and has nothing to be
     * surprised about, so it skips the held beat and starts at the drop. */
    int skip = from_air ? 4 : 0;
    int i = spk_row(phase, SPK_ROWS - skip);

    *out = fall_table[i + skip];
    out->base_tilt = spk_foot(out->base_tilt, strong);
}


/** The curves between levels **/

int spk_walk_level(int from, int to, int phase)
{
    int f;

    /* A step into a hole is drawn level with the ground it left: the death
     * is held until the boundary, and the teeter plays at the ledge. */
    if (to < 0)
        to = from;

    /* Up follows the scroll, so the body and the world arrive together.
     * Down accelerates, because a drop that eases out reads as a lift. */
    f = (to >= from) ? spk_ease(phase) : ((phase * phase) >> 8);

    return (from << 8) + (to - from) * f;
}

int spk_arc_level(int from, int to, int phase)
{
    int lin, bell, amp;

    /* A jump into a hole descends to the floor and falls through it. */
    if (to < 0)
        to = 0;

    lin = (from << 8) + (to - from) * phase;
    bell = (4 * phase * (SPK_PHASE - phase)) >> 8;

    /* Chosen so the peak is always two levels above the take-off, whatever
     * the drop on the far side -- which is what makes "rises to level+2" a
     * rule the player can rely on rather than a description of one case. */
    amp = (2 << 8) - ((to - from) << 7);

    return lin + ((amp * bell) >> 8);
}


/** The triangle **/

static void spk_rotate(int *x, int *y, int cx, int cy, int deg)
{
    long s, c;
    int dx, dy;

    if (deg == 0)
        return;

    s = fp14_sin(deg);
    c = fp14_cos(deg);
    dx = *x - cx;
    dy = *y - cy;

    *x = cx + (int)((dx * c - dy * s) >> 14);
    *y = cy + (int)((dx * s + dy * c) >> 14);
}

void spk_pose_points(const struct spk_pose *p, int bx, int by, int pt[3][2])
{
    /* Which foot the body is on. Tilting about the corner it is balanced on
     * keeps that corner on the surface and lifts the other; pivoting about
     * a fixed corner instead would drive the body through the ground on
     * every other beat. */
    int pivot = p->base_tilt >= 0 ? p->half_width : -p->half_width;
    int i;

    pt[0][0] = -p->half_width;   pt[0][1] = 0;
    pt[1][0] =  p->half_width;   pt[1][1] = 0;
    pt[2][0] =  p->apex_dx;      pt[2][1] = -p->height;

    /* Tilt pivots on a base corner and spin on the centroid: one is the
     * body balanced over a foot, the other is the whole of it in the air
     * with nothing to pivot against. */
    for (i = 0; i < 3; i++)
    {
        spk_rotate(&pt[i][0], &pt[i][1], pivot, 0, p->base_tilt);
        spk_rotate(&pt[i][0], &pt[i][1], p->apex_dx / 3, -p->height / 3,
                  p->rotation);

        pt[i][0] += bx + p->x_offset;
        pt[i][1] += by + p->y_offset;
    }
}
