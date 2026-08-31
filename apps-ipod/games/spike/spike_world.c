/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The movement rules, and what the player finds where they land.
 *
 * The course itself comes from spike_gen.c now: this file asks it for a
 * cell and never knows which pattern the cell came from or how it was
 * chosen. That boundary is worth keeping -- the rules are what a build-time
 * checker proves patterns against, so they have to be the same rules
 * whether a cell was hand-authored or assembled.
 *
 * A cell is a bitmask of the levels that carry a surface, plus what else is
 * standing in it. The mask is what lets one cell hold a floor and a platform
 * over it at once: a player at level 0 walks beneath, and one that jumped
 * onto it walks along the top.
 *
 * Parts, in order:
 *   - reading a cell
 *   - what has been taken
 *   - the player
 ****************************************************************************/

#include "config.h"
#include "games/spike/spike_gen.h"
#include "games/spike/spike_world.h"

/** Reading a cell **/

unsigned int spk_world_mask(int cell)
{
    return spk_gen_cell(cell) & SPK_C_MASK;
}

/* What a cell's surfaces would be with the switch in that state. The mode
 * field is read here and nowhere else: PLAT holds the platforms back and
 * leaves the floor, GROUND holds the floor back and leaves the platforms
 * standing over the hole, and SWAP trades one for the other. */
static unsigned int spk_live_with(unsigned int e, bool on)
{
    unsigned int mask = e & SPK_C_MASK;

    switch ((e & SPK_C_SWM) >> SPK_C_SWM_SH)
    {
    case SPK_SW_PLAT:   return on ? mask : (mask & 1u);
    case SPK_SW_GROUND: return on ? mask : (mask & ~1u);
    case SPK_SW_SWAP:   return on ? (mask & 1u) : (mask & ~1u);
    default:            return mask;
    }
}

unsigned int spk_world_live(int cell)
{
    unsigned int e = spk_gen_cell(cell);

    return spk_live_with(e, spk_world_switch_on(cell));
}

unsigned int spk_world_promised(int cell)
{
    unsigned int e = spk_gen_cell(cell);

    if (spk_world_switch_on(cell))
        return 0u;

    return spk_live_with(e, true) & ~spk_live_with(e, false);
}

bool spk_world_blocks(int cell)
{
    return (spk_gen_cell(cell) & SPK_C_BLF) != 0;
}

bool spk_world_block_up(int cell, int beat)
{
    unsigned int pattern = (spk_gen_cell(cell) >> SPK_C_BL_SH) & 15u;

    /* On the bar's four and not on the cell index's, so a pattern is the
     * same puzzle wherever the downbeat put it: bit n is the state on the
     * pattern's own nth beat. */
    return (pattern >> ((beat - spk_gen_bar()) & 3)) & 1u;
}

bool spk_world_switched(int cell)
{
    return (spk_gen_cell(cell) & SPK_C_SWM) != 0;
}

bool spk_world_has_switch(int cell, int *level)
{
    unsigned int e = spk_gen_cell(cell);

    if (!(e & SPK_C_SWF))
        return false;

    *level = (int)((e >> SPK_C_SW_SH) & 3u);

    return true;
}


/** What has been taken **/

/* A diamond eaten, a creature stomped, a switch thrown. Cells run on for
 * ever now, so this is a small ring that stores the cell each slot is
 * *about*: a slot whose cell does not match has nothing taken in it, which
 * makes a stale bit from a lap ago impossible rather than merely unlikely. */
#define SPK_MARKS    64

#define SPK_M_DIAMOND    1u
#define SPK_M_CREATURE   2u
#define SPK_M_SWITCH     4u

static int           mark_cell[SPK_MARKS];
static unsigned char mark_bits[SPK_MARKS];

/* The switch is per pattern: one is live at a time, and leaving the pattern
 * that carried it turns off whatever it was holding up. A pattern has to be
 * true on its own, and a platform still standing because of a switch two
 * phrases back would not be. */
static int switch_pattern = -1;

static int spk_mark_slot(int cell)
{
    int i = cell % SPK_MARKS;

    return i < 0 ? i + SPK_MARKS : i;
}

static bool spk_marked(int cell, unsigned int what)
{
    int i = spk_mark_slot(cell);

    return mark_cell[i] == cell && (mark_bits[i] & what) != 0;
}

static void spk_mark(int cell, unsigned int what)
{
    int i = spk_mark_slot(cell);

    if (mark_cell[i] != cell)
    {
        mark_cell[i] = cell;
        mark_bits[i] = 0;
    }

    mark_bits[i] |= (unsigned char)what;
}

static void spk_marks_clear(void)
{
    int i;

    for (i = 0; i < SPK_MARKS; i++)
    {
        mark_cell[i] = -1;
        mark_bits[i] = 0;
    }

    switch_pattern = -1;
}

bool spk_world_diamond(int cell, int *level)
{
    unsigned int e = spk_gen_cell(cell);

    if (!(e & SPK_C_DIF) || spk_marked(cell, SPK_M_DIAMOND))
        return false;

    *level = (int)((e >> SPK_C_DI_SH) & 3u);

    return true;
}

bool spk_world_creature(int cell, int *level)
{
    unsigned int e = spk_gen_cell(cell);

    if (!(e & SPK_C_CRF) || spk_marked(cell, SPK_M_CREATURE))
        return false;

    *level = (int)((e >> SPK_C_CR_SH) & 3u);

    return true;
}

/* A spike on the creature's head. Asked only where there is a creature, so
 * it does not repeat the creature's own test: what it answers is which kind
 * of creature, not whether there is one. */
bool spk_world_spiked(int cell)
{
    return (spk_gen_cell(cell) & SPK_C_SPK) != 0;
}

/* A spring. Never taken and never spent -- it is scenery that does something
 * to whoever comes down on it, and one pass crosses it once. */
bool spk_world_spring(int cell)
{
    return (spk_gen_cell(cell) & SPK_C_SPR) != 0;
}

bool spk_world_switch(int cell, int *level)
{
    return spk_world_has_switch(cell, level)
           && !spk_marked(cell, SPK_M_SWITCH);
}

bool spk_world_switch_on(int cell)
{
    return switch_pattern >= 0 && spk_gen_pattern_start(cell) == switch_pattern;
}

int spk_world_surface(int cell, int max_level)
{
    unsigned int mask = spk_world_live(cell);
    int level = max_level;

    if (level > SPK_LEVELS - 1)
        level = SPK_LEVELS - 1;

    for (; level >= 0; level--)
    {
        if (mask & (1u << level))
            return level;
    }

    return -1;
}

int spk_world_respawn(int from)
{
    int cell;

    /* Ground, and two beats of it after, so the player has somewhere to
     * stand and a beat to read the board before anything can kill them --
     * which means nothing standing in those cells either. Bounded because
     * the generator will happily supply cells for ever. */
    for (cell = from; cell < from + 64; cell++)
    {
        int i, level;

        for (i = 0; i < 3; i++)
        {
            if (spk_world_surface(cell + i, 0) != 0
                || spk_world_creature(cell + i, &level)
                || spk_world_blocks(cell + i))
                break;
        }

        if (i == 3)
            return cell;
    }

    return from;
}


/** The player **/

/* Choose the step that covers the beat now beginning. A jump committed
 * inside the input window replaces it before it is ever applied, which is
 * why the landing is only resolved at the far boundary.
 *
 * The ceiling is the player's own level, so a step never gains height. What
 * it finds nothing to stand on it walks off, whether the cell is empty or
 * holds a platform overhead. */
static void spk_begin_move(struct spk_state *st)
{
    st->from = st->level;

    /* A spring is not a press and cannot be declined: it replaces the step
     * that would have covered this beat. Two beats and three levels, from
     * the ground and only from the ground, so the arc is one shape wherever
     * it is met -- and it reaches the top surface, which nothing a player
     * can do from level 0 otherwise does. */
    if (st->sprung)
    {
        st->sprung = false;
        st->motion = SPK_ARC_SPRING;
        st->apex = SPK_LEVELS - 1;
        st->to = spk_world_surface(st->beat + 2, st->apex);
        return;
    }

    st->motion = SPK_WALK;
    st->to = spk_world_surface(st->beat + 1, st->level);
}

/* What the player finds where it has just arrived. A creature is stomped
 * from above and fatal from the side, and the caller decides which of those
 * the arrival was -- or passes 'over', which is neither: a jump clears
 * whatever is in the cell it is airborne across. */
static enum spk_outcome spk_state_arrive(struct spk_state *st, int level,
                                       bool from_above, bool over)
{
    int cl;

    if (!over && spk_world_creature(st->beat, &cl) && cl == level)
    {
        /* From the side it is fatal and from above it is the stomp -- unless
         * it has a spike on its head, which makes both fatal and leaves the
         * jump clear across the cell as the only way past. */
        if (!from_above || spk_world_spiked(st->beat))
            return SPK_HIT;

        spk_mark(st->beat, SPK_M_CREATURE);
        st->stomped = true;
    }

    /* Thrown by being landed on, and by nothing else. Walking over one is
     * how a player who has not spent the press gets past it. */
    if (from_above && !over && spk_world_switch(st->beat, &cl) && cl == level)
    {
        spk_mark(st->beat, SPK_M_SWITCH);
        switch_pattern = spk_gen_pattern_start(st->beat);
        st->threw = true;
    }

    if (spk_world_diamond(st->beat, &cl) && cl == level)
    {
        spk_mark(st->beat, SPK_M_DIAMOND);
        st->got = true;
        st->got_level = cl;
    }

    /* Landed on from above, exactly as a switch is thrown -- walking over one
     * does nothing, so a spring missed just keeps you walking. Recorded here
     * and acted on by spk_begin_move(), which runs immediately after every
     * arrival that could have found one. */
    if (from_above && !over && level == 0 && spk_world_spring(st->beat))
        st->sprung = true;

    return SPK_OK;
}

void spk_world_forget(void)
{
    spk_marks_clear();
}

void spk_state_start_at(struct spk_state *st, int cell, int level)
{
    st->beat = cell;
    st->level = level;
    st->landed = false;
    st->got = false;
    st->got_level = 0;
    st->stomped = false;
    st->threw = false;
    st->sprung = false;
    st->apex = 0;
    spk_marks_clear();
    spk_begin_move(st);
}

void spk_state_start(struct spk_state *st, int cell)
{
    /* Level 0, which is where a run opens and where a respawn returns to.
     * The caller passes a cell with ground under it; spk_world_respawn() is
     * what finds one. */
    spk_state_start_at(st, cell, 0);
}

enum spk_outcome spk_state_peek(const struct spk_state *st)
{
    /* Every death is decided before the camera moves, so a freeze can hold
     * the field exactly where the player met what killed them. */
    if ((st->motion == SPK_WALK || st->motion == SPK_ARC_FALL) && st->to < 0)
        return SPK_FELL;

    /* The creature rule, one beat before the boundary applies it. A step that
     * does not come down to reach one walks into it; a landing comes down on
     * it and stomps it -- and a spike on its head makes the landing fatal
     * too. The two airborne motions are never this case: they pass over the
     * cell, which is what makes a stomp a thing you go and do. */
    if (st->motion == SPK_WALK || st->motion == SPK_ARC_FALL)
    {
        int cl;

        if (spk_world_creature(st->beat + 1, &cl) && cl == st->to)
        {
            bool above = st->motion == SPK_ARC_FALL || st->from > st->to;

            if (!above || spk_world_spiked(st->beat + 1))
                return SPK_HIT;
        }
    }

    /* A rising block fills one level of the cell: the ground when it is
     * down, SPK_BLOCK_LEVEL when it is up. What matters is whether the player
     * is at that level -- a step arrives at 'to', and a jump passes over the
     * cell two levels above where it left. The cell a move ends over is
     * always beat + 1 whichever move it is, and the beat it is judged on is
     * that cell's own index, so this reads the same for all three. */
    if (spk_world_blocks(st->beat + 1))
    {
        int fills = spk_world_block_up(st->beat + 1, st->beat + 1)
                    ? SPK_BLOCK_LEVEL : 0;

        /* A spring never bonks, and that falls out of the arithmetic
         * rather than needing a case: its apex is the top surface and a
         * raised block fills SPK_BLOCK_LEVEL, below it. A launch clears
         * one, as it clears everything else. */
        if (st->motion == SPK_ARC_RISE || st->motion == SPK_ARC_SPRING)
        {
            if (st->apex == fills)
                return SPK_BONKED;
        }
        else if (st->to == fills)
            return SPK_CRUSHED;
    }

    return SPK_OK;
}

enum spk_outcome spk_state_advance(struct spk_state *st)
{
    enum spk_outcome out = spk_state_peek(st);

    if (out != SPK_OK)
        return out;

    st->beat++;
    st->got = false;
    st->stomped = false;
    st->threw = false;

    switch (st->motion)
    {
    case SPK_ARC_RISE:
    case SPK_ARC_SPRING:
        /* Over the intervening cell, and its surfaces are not consulted --
         * that is what passing over means. A diamond at the apex is still
         * collected, which is the whole of why the high ones are worth a
         * press -- and why one over the cell after a spring is worth a
         * spring, being higher than any press reaches. */
        st->level = -1;
        st->motion = SPK_ARC_FALL;
        out = spk_state_arrive(st, st->apex, false, true);
        break;

    case SPK_ARC_FALL:
        st->level = st->to;
        st->landed = true;
        out = spk_state_arrive(st, st->level, true, false);
        spk_begin_move(st);
        break;

    case SPK_WALK:
    default:
        st->landed = false;
        out = spk_state_arrive(st, st->to, st->from > st->to, false);
        st->level = st->to;
        spk_begin_move(st);
        break;
    }

    return out;
}

/* Whether the move the player did not make would have ended the line.
 *
 * This is what an obstacle *is*, as one rule rather than a list: it covers a
 * hole, a surface out of reach, a creature at walking level and a block that
 * is down, without any of them being a case of its own. Flat ground answers
 * false, which is the whole point of scoring it -- a long safe stretch is
 * worth nothing and a short dangerous one is worth something.
 *
 * Asked before the boundary is crossed, and only where there was a choice to
 * make: the second half of a jump is not one, because it was decided a beat
 * ago. Nothing here mutates -- the copy is peeked at, never advanced, so the
 * world's memory of what has been taken is untouched. */
bool spk_state_avoided(const struct spk_state *st)
{
    struct spk_state other = *st;

    if (st->motion == SPK_WALK)
    {
        spk_state_jump(&other);

        /* Where a jump comes down is settled the moment it is committed,
         * and nothing to land on would not be reported until the far
         * boundary -- so it is read here rather than peeked for. */
        if (other.to < 0)
            return true;
    }
    else if (st->motion == SPK_ARC_RISE)
        spk_begin_move(&other);
    else
        return false;

    return spk_state_peek(&other) != SPK_OK;
}

void spk_state_end_beat(struct spk_state *st)
{
    st->got = false;
    st->stomped = false;
    st->threw = false;
}

bool spk_state_jump(struct spk_state *st)
{
    if (st->motion != SPK_WALK)
        return false;

    st->motion = SPK_ARC_RISE;
    st->from = st->level;

    /* The arc reaches SPK_ARC_UP; the landing is capped a level lower. A
     * diamond at the apex is still collected on the way through -- what the
     * lower cap takes away is the *standing* on it, not the reaching. */
    st->apex = st->level + SPK_ARC_UP;
    st->to = spk_world_surface(st->beat + 2, st->from + SPK_CLIMB_UP);

    return true;
}
