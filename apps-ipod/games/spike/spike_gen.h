/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The pattern library and the assembler: where the course comes from once
 * it stops being a fixed array.
 *
 * The unit of generation is a **pattern** -- a hand-authored phrase of four
 * or eight cells with its own entry and exit level. Nothing is ever
 * generated cell by cell, and the reason is the difference between this and
 * a terrain game: a badly placed cell here is not an odd-looking hill, it is
 * an instant death, or a stretch that cannot be crossed at all. Every
 * pattern is solvable in isolation by construction and checked to be so, and
 * matching one's exit level to the next one's entry means concatenating them
 * preserves it. That is the whole safety argument.
 *
 * Two placement rules make the rest fall out, and both are load-bearing:
 *
 *   - **Lengths are a multiple of four, so a pattern always begins on one.**
 *     A rising block's pattern is four beats, so a block's behaviour depends
 *     only on its index *within* its pattern and not on where the pattern
 *     landed. A pattern is therefore the same puzzle wherever it is placed,
 *     which is what lets it be checked once.
 *   - **A switch and everything waiting on it live in the same pattern.**
 *     Otherwise a pattern could arrive carrying platforms whose switch is
 *     somewhere else, and it would not be solvable on its own terms.
 ****************************************************************************/

#ifndef SPIKE_GEN_H
#define SPIKE_GEN_H

#include <stdbool.h>

/* A cell, packed. The authoring macros and the bits the world reads them
 * back through are here together, so there is one place to get it wrong. */
#define SPK_C_MASK   0x000Fu     /* surfaces, one bit a level */
#define SPK_C_DIF    0x0080u     /* a diamond, level in 8-9 */
#define SPK_C_CRF    0x0400u     /* a creature, level in 11-12 */
#define SPK_C_BLF    0x2000u     /* a rising block, pattern in 14-17 */
#define SPK_C_SWF    0x040000u   /* a switch, level in 19-20 */
#define SPK_C_SWM    0x600000u   /* what its switch does here, in 21-22 */
#define SPK_C_SPK    0x800000u   /* the creature here has a spike on its head */
#define SPK_C_SPR   0x1000000u   /* a spring, on the ground and nowhere else */

#define SPK_C_DI_SH  8
#define SPK_C_CR_SH  11
#define SPK_C_BL_SH  14
#define SPK_C_SW_SH  19
#define SPK_C_SWM_SH 21

/* What a switch does to a cell that waits on it. Exactly one of these, which
 * is why it is a field and not a flag each: a cell whose platforms wait and
 * whose floor is traded away at the same time is not a thing, and an encoding
 * that can say it is an encoding that has to be policed in prose. */
#define SPK_SW_NONE     0u
#define SPK_SW_PLAT     1u   /* the platforms wait; the floor stays */
#define SPK_SW_GROUND   2u   /* the ground waits; the platforms stand */
#define SPK_SW_SWAP     3u   /* one for the other, never both */

/* What a pattern is about, so the music can ask for one of a kind. */
#define SPK_T_REST       0x01
#define SPK_T_GAP        0x02
#define SPK_T_CREATURE   0x04
#define SPK_T_BLOCK      0x08
#define SPK_T_SWITCH     0x10
#define SPK_T_DIAMOND    0x20
#define SPK_T_BAIT       0x40
#define SPK_T_TEACH      0x80
#define SPK_T_SPRING     0x0100
#define SPK_T_SPIKED     0x0200

/* Sixteen is four bars at the grid's own tempo, which is about as long as a
 * phrase can be and still be read as one shape rather than as a stretch of
 * course. The validator brute-forces every press sequence over a pattern, so
 * this is also what fixes how long that takes: two million plays for a
 * sixteen, against a thousand for a four. */
#define SPK_PAT_MAX      16

struct spk_pattern
{
    unsigned char  length;      /* a multiple of four, up to SPK_PAT_MAX */
    unsigned char  entry;       /* level the player is on arriving */
    unsigned char  exit;        /* ...and on leaving */
    unsigned char  difficulty;  /* 0 to SPK_TIER_TOP */
    unsigned short tags;
    unsigned int   cells[SPK_PAT_MAX];
};

/* The library, for the assembler and for the build-time checker that proves
 * each of them can be crossed at all. */
extern const struct spk_pattern spk_patterns[];
extern const int spk_pattern_count;

/* Whether a phrase can be walked into at that level: either it says so, or
 * it carries a surface there at both ends. A phrase with a floor and a
 * storey above it is two phrases, and which is played depends only on where
 * the player arrives; entered above its own floor it leaves at that level.
 *
 * Public for the validator, which has to prove a phrase at every level the
 * assembler might enter it at. A copy of the rule over there would be right
 * the day it was written and wrong the day this one moved. */
bool spk_pat_at(const struct spk_pattern *p, int level);

/* Start a course at that cell. Everything before it is forgotten.
 *
 * 'cell' is the beat index the run picks the track up on -- track time over
 * the grid period -- so a game started half-way through a song lays the
 * phrases that belong there rather than replaying the opening of the
 * course. */
void spk_gen_reset(int cell);

/* Lay bare ground until this is turned off again -- level nought and
 * nothing above it, no diamonds, no library rest.
 *
 * Bare rather than restful, and the difference showed on screen: a rest is
 * still a phrase and several carry a platform row, so the wait for a tempo
 * walked under floating slabs and they jumped when the world was re-seated
 * at the latch. Nothing can be scored on ground the run has not started on,
 * and nothing there can move when the cell index changes under it.
 *
 * A mode rather than a one-shot: the wait for a tempo is open-ended, and the
 * assembler runs ahead of the player and can extend the course by several
 * patterns inside one frame. */
void spk_gen_set_flat(bool flat);

/* The track's tempo, mixed into every choice, so that two songs do not lay
 * the same course at the same cell index. Latched once with the tempo and
 * never revisited, which is what keeps it as fixed as the song is. */
void spk_gen_set_seed(int seed);

/* Which residue of four the music's downbeat falls on, so that phrases begin
 * where bars do. Zero where it was never found.
 *
 * It moves the phrase boundaries and nothing else: every choice the
 * assembler makes is a function of the cell with this rotation taken out,
 * so a run that found the bar and one that did not lay the same phrases in
 * the same order, offset by up to three cells. Set before the reset that
 * lays the first pattern; the rising blocks turn with it. */
void spk_gen_set_bar(int rot);
int spk_gen_bar(void);

/* What the music has been doing over the bar just heard, as three numbers
 * out of a hundred: level against the loudest the track has been, movement
 * between the beats of the bar, and this bar against the one before it.
 *
 * Taken as data rather than measured here, so that the thing which decides
 * the course can be driven by a harness and does not drag the audio mixer
 * in behind it. Set once a beat; it biases the *choice* among phrases that
 * were already allowed and never widens the set, so the tier ramp is still
 * the ceiling and a phrase the tier forbids stays forbidden. */
void spk_gen_set_mood(int energy, int flux, int trend);

/* The cell at that index, assembling more of the course if it has not been
 * reached yet. Only ever asked for cells at or ahead of where the run is:
 * the ring behind the player is small and is overwritten. */
unsigned int spk_gen_cell(int cell);

/* The first cell not yet laid, which is where the next pattern will start.
 * It runs a pattern or two ahead of the field, so a caller that wants to
 * stop offering obstacles before the track ends has to ask where the
 * *assembler* is, not where the player is. */
int spk_gen_next_cell(void);

/* Which pattern a cell belongs to, and where it starts. The switch state is
 * per pattern, so the world needs to know when one has been left. */
int spk_gen_pattern_start(int cell);

/* Diamonds in the pattern a cell belongs to. Section 8's phrase bonus is for
 * taking every one of them, which is what makes a bait cost something: its
 * two routes are authored so neither can. */
int spk_gen_pattern_diamonds(int cell);

#endif /* SPIKE_GEN_H */
