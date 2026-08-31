/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The pattern library, and the assembler that lays patterns end to end.
 *
 * Parts, in order:
 *   - the library
 *   - the ring the course is assembled into
 *   - choosing what comes next
 ****************************************************************************/

#include "config.h"
#include "system.h"           /* ARRAYLEN */
#include "games/spike/spike_gen.h"

/** The library **/

/* Written short, because a pattern is read as a shape rather than as code. */
#define __      0u
#define G       (1u << 0)
#define P1      (1u << 1)
#define P2      (1u << 2)
#define P3      (1u << 3)
#define DI(n)   (SPK_C_DIF | ((unsigned)(n) << SPK_C_DI_SH))
#define CR(n)   (SPK_C_CRF | ((unsigned)(n) << SPK_C_CR_SH))
#define BL(m)   (SPK_C_BLF | ((unsigned)(m) << SPK_C_BL_SH))
#define SW(n)   (SPK_C_SWF | ((unsigned)(n) << SPK_C_SW_SH))
#define SWP     (SPK_SW_PLAT   << SPK_C_SWM_SH)
#define SWG     (SPK_SW_GROUND << SPK_C_SWM_SH)
#define SWX     (SPK_SW_SWAP   << SPK_C_SWM_SH)
#define CR2(n)  (CR(n) | SPK_C_SPK)
#define SPR     SPK_C_SPR

/* Every one of these enters and leaves at level 0. A pattern that climbs
 * comes back down inside itself, which is a restriction the entry and exit
 * fields exist to lift later -- concatenating at height needs nothing new
 * from the assembler, only patterns that agree at the join.
 *
 * A block's index within its pattern is its beat modulo four once the bar
 * rotation is taken out, so BL(3) -- up, stay, down, stay -- reads the same
 * wherever the pattern is placed: index 1 is up on arrival and index 3 is
 * down. Patterns begin on the downbeat, so the two grids are the same one. */
const struct spk_pattern spk_patterns[] =
{
    { 4, 0, 0, 0, SPK_T_REST,
      { G, G, G, G } },

    { 8, 0, 0, 0, SPK_T_REST | SPK_T_TEACH,
      { G, G|P2, G, G|P2, G, G|P2, G, G } },

    { 4, 0, 0, 0, SPK_T_REST | SPK_T_DIAMOND,
      { G, G, G|P1|DI(1), G } },

    { 4, 0, 0, 0, SPK_T_REST | SPK_T_DIAMOND,
      { G, G|DI(0), G|DI(0), G } },

    { 8, 0, 0, 0, SPK_T_REST | SPK_T_DIAMOND,
      { G, G|DI(0), G, G, G|DI(0), G|DI(0), G, G } },

    { 8, 0, 0, 1, SPK_T_DIAMOND,
      { G, G, G|DI(2), G, G, G|DI(2), G, G } },

    { 8, 0, 0, 2, SPK_T_DIAMOND,
      { G, G|DI(2), G|DI(0), G|DI(2), G|DI(0), G|DI(2), G|DI(0), G } },

    { 8, 0, 0, 2, SPK_T_DIAMOND | SPK_T_TEACH,
      { G, G, G|P1, G|P1, G|P2, G|P2|DI(2), G, G } },

    { 8, 0, 0, 2, SPK_T_DIAMOND,
      { G, G, G|P1|DI(1), G|P2, G|P1|DI(1), G, G|DI(0), G } },

    { 8, 0, 0, 2, SPK_T_DIAMOND,
      { G, G, G|P1, G, G|P2|DI(2), G, G|P3|DI(3), G } },

    { 8, 0, 0, 2, SPK_T_DIAMOND,
      { G, G|P2|DI(0), G|P2|DI(2), G|P2|DI(0), G|P2|DI(2), G|P2|DI(0),
        G|P2|DI(0), G } },

    { 8, 0, 0, 2, SPK_T_DIAMOND,
      { G, G, G|P1|DI(1), G, G|P2|DI(2), G|P1|DI(1), G|DI(0), G } },

    { 8, 0, 0, 3, SPK_T_DIAMOND,
      { G, G|P1, G|P1, G|P2, G|P2, G|P3|DI(3), G|P3|DI(3), G } },

    { 8, 0, 0, 4, SPK_T_DIAMOND,
      { G, G|P1, G|P1, G|P2, G|P2|DI(2), __, G|P3|DI(3), G } },

    { 4, 0, 0, 1, SPK_T_GAP | SPK_T_TEACH,
      { G, G, __, G } },

    { 4, 0, 0, 1, SPK_T_GAP,
      { G, __, G, G } },

    { 4, 0, 0, 1, SPK_T_GAP | SPK_T_DIAMOND,
      { G|DI(0), G, __, G|DI(0) } },

    { 8, 0, 0, 2, SPK_T_GAP,
      { G, G, __, G, G, __, G, G } },

    { 8, 0, 0, 2, SPK_T_GAP | SPK_T_DIAMOND,
      { G, G, __, G|DI(0), __, G, G, G } },

    { 8, 0, 0, 2, SPK_T_GAP | SPK_T_DIAMOND,
      { G, G|DI(0), G, __, G|DI(0), G, __, G } },

    { 8, 0, 0, 3, SPK_T_GAP | SPK_T_DIAMOND,
      { G, __, G|DI(0), __, G|DI(0), __, G, G } },

    { 8, 0, 0, 3, SPK_T_GAP | SPK_T_DIAMOND,
      { G, G|P1, G, G|P2|DI(2), __, G|P2, G|P2|DI(2), G } },

    { 8, 0, 0, 4, SPK_T_BAIT | SPK_T_GAP | SPK_T_DIAMOND,
      { G, G|DI(2), G, G|DI(2), __, G, G, G } },

    { 8, 0, 0, 2, SPK_T_GAP | SPK_T_DIAMOND,
      { G, G, P1, P1|DI(1), P1, G, G, G } },

    { 8, 0, 0, 3, SPK_T_DIAMOND,
      { G, G, G|P1|DI(1), G|P1, G|P2|DI(2), P2|DI(2), G|P1, G } },

    { 8, 0, 0, 3, SPK_T_GAP | SPK_T_DIAMOND,
      { G, G, P1|DI(1), P1, P1, P2|DI(2), P2, G } },

    { 8, 0, 0, 3, SPK_T_GAP | SPK_T_DIAMOND,
      { G, P1, P1|DI(1), P1|DI(1), __, G, G, G } },

    { 8, 0, 0, 4, SPK_T_GAP | SPK_T_DIAMOND,
      { G, G, P1|DI(1), P1|DI(1), __, P1|DI(1), G, G } },

    { 8, 0, 0, 4, SPK_T_GAP | SPK_T_DIAMOND | SPK_T_BAIT,
      { G, G, P1|DI(1), P1, P1|DI(2), __, G, G } },

    { 8, 0, 0, 4, SPK_T_GAP | SPK_T_DIAMOND,
      { G, P1, P1, __, P2|DI(2), P2, P3|DI(3), G } },

    { 8, 0, 0, 4, SPK_T_GAP | SPK_T_DIAMOND,
      { G, P1, P1|DI(1), P2, P2|DI(2), __, G, G } },

    { 8, 0, 0, 5, SPK_T_GAP | SPK_T_DIAMOND,
      { G, P1, P1|DI(1), P1|DI(1), G, __, G, G } },

    { 12, 0, 0, 6, SPK_T_GAP | SPK_T_DIAMOND,
      { G, P1, P1|DI(1), __, P2|DI(2), P3, P3|DI(3), __, P2|DI(2), __,
        P1|DI(1), G } },

    { 12, 0, 0, 6, SPK_T_GAP | SPK_T_DIAMOND | SPK_T_BAIT,
      { G, __, P1|DI(1), P1|DI(1), __, P2, P3|DI(3), P2, P2|DI(2), __,
        P1|DI(1), G } },

    { 4, 0, 0, 2, SPK_T_CREATURE | SPK_T_TEACH,
      { G, G, G|CR(0), G } },

    { 4, 0, 0, 2, SPK_T_CREATURE | SPK_T_DIAMOND,
      { G, G|DI(2), G|CR(0), G } },

    { 8, 0, 0, 2, SPK_T_GAP | SPK_T_CREATURE | SPK_T_DIAMOND,
      { G, G|DI(2), G|CR(0), __, G|DI(0), G, G, G } },

    { 4, 0, 0, 3, SPK_T_CREATURE | SPK_T_GAP,
      { G, G, __, G|CR(0) } },

    { 8, 0, 0, 3, SPK_T_CREATURE | SPK_T_GAP,
      { G, G, G|CR(0), __, G|CR(0), G, G|P1|DI(1), G } },

    { 8, 0, 0, 3, SPK_T_CREATURE | SPK_T_DIAMOND,
      { G, G|DI(2), G|CR(0), G|DI(2), G, G|CR(0), G|DI(0), G } },

    { 12, 0, 0, 4, SPK_T_GAP | SPK_T_CREATURE | SPK_T_DIAMOND | SPK_T_BAIT,
      { G, G, G|P1|DI(1), G, G|P2|DI(2), G, G|P3|DI(3), G|P3|DI(0),
        G|P3|CR(3), G|P2, G|P1|CR(1), G } },

    /* The spike, taught against the habit it breaks. Pressing a beat early
       lands on its head and is fatal; not pressing at all walks into it
       and is fatal; the one press that works clears the cell entirely. It
       is the stomp's own verb aimed one cell further. */
    { 4, 0, 0, 5, SPK_T_CREATURE | SPK_T_DIAMOND | SPK_T_TEACH | SPK_T_SPIKED,
      { G, G|DI(0), G|CR2(0), G } },

    { 8, 0, 0, 5, SPK_T_GAP | SPK_T_CREATURE | SPK_T_DIAMOND | SPK_T_SPIKED,
      { G, G|DI(0), G|CR2(0), G|DI(0), __, G|DI(0), G, G } },

    { 8, 0, 0, 5, SPK_T_CREATURE | SPK_T_DIAMOND | SPK_T_BAIT | SPK_T_SPIKED,
      { G, G|CR2(0), G, G|DI(0), G|CR(0), __, G, G } },

    { 4, 0, 0, 6, SPK_T_CREATURE | SPK_T_DIAMOND | SPK_T_BAIT,
      { G, G|DI(2), G|CR2(0), G } },

    { 8, 0, 0, 3, SPK_T_CREATURE | SPK_T_DIAMOND | SPK_T_BAIT | SPK_T_SPIKED,
      { G, G|DI(2), G|CR2(0), G, G|DI(2), G|CR2(0), G|DI(0), G } },

    { 8, 0, 0, 6, SPK_T_CREATURE | SPK_T_DIAMOND | SPK_T_BAIT | SPK_T_SPIKED,
      { G, G|DI(2), G|CR(0), G, G|DI(2), G|CR2(0), G|DI(0), G } },

    { 8, 0, 0, 6, SPK_T_CREATURE | SPK_T_DIAMOND | SPK_T_SPIKED,
      { G, G, G|CR(0), G|DI(0), G|CR2(0), G, __, G } },

    { 8, 0, 0, 6, SPK_T_GAP | SPK_T_CREATURE | SPK_T_SPIKED,
      { G, G, G|P1, G|CR2(0), P2, __, P3, G } },

    { 8, 0, 0, 6, SPK_T_GAP | SPK_T_CREATURE | SPK_T_SPIKED,
      { G, G, G|P1, G, G|P2, G|CR2(0), P3, G } },

    { 4, 0, 0, 2, SPK_T_BLOCK,
      { G, G, G, G|BL(6) } },

    { 4, 0, 0, 2, SPK_T_BLOCK | SPK_T_TEACH,
      { G, G|BL(3), G, G } },

    { 4, 0, 0, 2, SPK_T_BLOCK,
      { G, G|BL(9), G, G } },

    { 4, 0, 0, 2, SPK_T_BLOCK | SPK_T_GAP,
      { G, G|BL(3), __, G } },

    { 4, 0, 0, 2, SPK_T_BLOCK,
      { G, G, G|BL(12), G } },

    { 8, 0, 0, 3, SPK_T_BLOCK,
      { G, G|BL(9), G, G, G|BL(12), G, G, G } },

    { 8, 0, 0, 3, SPK_T_BLOCK,
      { G, G|BL(3), G, G|BL(3), G, G|BL(3), G, G } },

    { 8, 0, 0, 4, SPK_T_BLOCK,
      { G, G|BL(3), G, G|BL(6), G, G|BL(12), G, G } },

    { 12, 0, 0, 5, SPK_T_BLOCK,
      { G, G|BL(3), G, G|BL(6), G, G|BL(12), G, G|BL(3), G, G|BL(6), G,
        G|BL(12) } },

    { 4, 0, 0, 2, SPK_T_GAP | SPK_T_SWITCH | SPK_T_TEACH,
      { G, G|SW(0), G|DI(0)|SWG, G } },

    { 8, 0, 0, 2, SPK_T_SWITCH | SPK_T_DIAMOND,
      { G, G, G|SW(0), G, G|P1|DI(1)|SWP, G|P1|DI(1)|SWP, G|P1|SWP, G } },

    { 8, 0, 0, 3, SPK_T_SWITCH | SPK_T_GAP | SPK_T_DIAMOND,
      { G, G, G|SW(0), __, G|P1|DI(1)|SWP, G|P1|DI(1)|SWP, G, G } },

    { 8, 0, 0, 3, SPK_T_SWITCH | SPK_T_DIAMOND,
      { G, G, G|SW(0), G, G|P1|SWP, G|P1|SWP, G|P2|DI(2)|SWP,
        G|P2|DI(2)|SWP } },

    { 8, 0, 0, 3, SPK_T_SWITCH | SPK_T_DIAMOND | SPK_T_GAP,
      { G, G, G|SW(0), G, G|P1|DI(1)|SWP, G|P1|SWP, __, G } },

    { 12, 0, 0, 3, SPK_T_GAP | SPK_T_SWITCH | SPK_T_DIAMOND,
      { G, G, G|SW(0), __, G|P1|SWP, G|P1|SWP, G|P2|DI(2), G|P2|DI(2),
        G|P2|DI(2), G|P2|DI(2), G|P2|DI(2), G } },

    { 12, 0, 0, 3, SPK_T_GAP | SPK_T_SWITCH | SPK_T_BAIT,
      { G, G|DI(0), G|SW(0), G, G|DI(0)|SWG, G, G|DI(0)|SWG, G,
        G|DI(0)|SWG, G, G|DI(0)|SWG, G } },

    { 12, 0, 0, 3, SPK_T_GAP | SPK_T_SWITCH,
      { G, G, G|SW(0), G|P1, G|P1, G|P2|DI(0)|SWX, G|P2|DI(0),
        G|P3|DI(0)|SWX, G|P3|DI(0), G|P3|DI(0)|SWX, G, G } },

    { 12, 0, 0, 4, SPK_T_GAP | SPK_T_SWITCH | SPK_T_BAIT,
      { G, G|DI(0), G|SW(0), G, G|P1|SWP, G, G|P2|DI(2)|SWP, G,
        G|P3|DI(3)|SWP, G|P3|DI(3)|SWP, G|P2|DI(2)|SWP, G } },

    { 12, 0, 0, 5, SPK_T_GAP | SPK_T_SWITCH,
      { G, G, G|SW(0), __, G|P1|SWP, __, G|P2|DI(2)|SWP, __,
        G|P3|DI(3)|SWP, G|P3|DI(3)|SWP, G|P3|DI(3)|SWP, G } },

    { 8, 0, 0, 5, SPK_T_SWITCH | SPK_T_GAP,
      { G, G, G|SW(0), G, G|SWG, G|DI(0)|SWG, G|SWG, G } },

    { 12, 0, 0, 6, SPK_T_GAP | SPK_T_SWITCH | SPK_T_DIAMOND,
      { G, G, G|SW(0), G|P1|SWX, G|P1|SWX, G|P2|DI(0)|SWX,
        G|P2|DI(0)|SWX, G|P3|DI(0)|SWX, G|P3|DI(0)|SWX, G|P3|DI(0)|SWX,
        G|P3|DI(0)|SWX, G } },

    { 12, 0, 0, 6, SPK_T_GAP | SPK_T_SWITCH | SPK_T_DIAMOND,
      { G, G, G|SW(0), G|P1|SWG, G|P1|SWG, G|P2|SWG, G|P2|DI(2)|SWG,
        G|P3|SWG, G|P3|DI(3)|SWG, G|P3|DI(3)|SWG, G|P3|DI(3)|SWG, G } },

    { 12, 0, 0, 6, SPK_T_GAP | SPK_T_SWITCH | SPK_T_BAIT,
      { G, G|DI(0), G|SW(0), G, __, G|SWG, G|DI(0)|SWG, G|SWG,
        G|DI(0)|SWG, G|SWG, __, G } },

    { 4, 0, 0, 3, SPK_T_DIAMOND | SPK_T_SPRING,
      { G, G, G|SPR, G|DI(3) } },

    { 8, 0, 0, 4, SPK_T_DIAMOND | SPK_T_SPRING,
      { G, G, G|SPR, G, G|P3|DI(3), G|P3|DI(3), G|P3|DI(3), G } },

    { 8, 0, 0, 4, SPK_T_GAP | SPK_T_DIAMOND | SPK_T_SPRING,
      { G, G, __, G|SPR, G|DI(3), G, G, G } },

    { 8, 0, 0, 5, SPK_T_GAP | SPK_T_DIAMOND | SPK_T_SPRING,
      { G, G, G|SPR, DI(3), G|SPR, DI(3), G, G } },

    { 12, 0, 0, 5, SPK_T_GAP | SPK_T_DIAMOND | SPK_T_SPRING,
      { G, G, G|SPR, __, P3|DI(3), P3|DI(3), G|SPR, __, P3|DI(3),
        P3|DI(3), G, G } },

    { 8, 0, 0, 5, SPK_T_GAP | SPK_T_DIAMOND | SPK_T_BAIT | SPK_T_SPRING,
      { G, G|DI(0), G|SPR, DI(3), G, __, G|SPR, G|DI(3) } },

    { 8, 0, 0, 5, SPK_T_SPRING | SPK_T_DIAMOND | SPK_T_GAP,
      { G, G, __, G|SPR, G, G|P3, G|P3|DI(3), G } },

    { 12, 0, 0, 6, SPK_T_CREATURE | SPK_T_DIAMOND | SPK_T_SPRING,
      { G, G|DI(2), G|SPR, G, G|P3|CR(3), G|SPR, G, G|P3, G|P3|DI(3),
        G|P3|DI(3), G, G } },

    { 12, 0, 0, 6, SPK_T_CREATURE | SPK_T_DIAMOND | SPK_T_BAIT | SPK_T_SPRING,
      { G, G|DI(0), G|SPR, G, G|P3|DI(3), G|P3|DI(3), G|SPR, G|P3|DI(3),
        G|P3|CR(3), G|P3|DI(3), G, G } },

    /* Moves the player to level 3 for level 3 phrases */
    { 4, 0, 1, 4, SPK_T_GAP | SPK_T_DIAMOND,
      { G, G, P1, P1|DI(1) } },

    { 4, 1, 1, 4, SPK_T_REST | SPK_T_GAP | SPK_T_DIAMOND,
      { P1, P1|DI(1), P1|DI(1), P1 } },

    { 4, 1, 1, 4, SPK_T_GAP | SPK_T_CREATURE | SPK_T_DIAMOND,
      { P1, P1|CR(1), P1, P1 } },

    { 4, 1, 1, 4, SPK_T_GAP | SPK_T_DIAMOND,
      { P1, P1|DI(1), __, P1|DI(1) } },

    { 8, 1, 1, 4, SPK_T_GAP | SPK_T_DIAMOND,
      { P1, __, P1|DI(1), __, P1|DI(1), __, P1, P1 } },

    { 8, 1, 1, 4, SPK_T_GAP | SPK_T_DIAMOND,
      { P1, P1|DI(1), P2, P2|DI(2), P1, P1|DI(1), P1, P1 } },

    { 8, 1, 1, 4, SPK_T_GAP | SPK_T_CREATURE | SPK_T_DIAMOND | SPK_T_SPIKED,
      { P1, P1, P1|CR(1), P1|DI(1), G|P2, G|P2|CR2(0), G|P1, P1 } },

    { 8, 1, 1, 4, SPK_T_GAP | SPK_T_BLOCK | SPK_T_DIAMOND,
      { P1|DI(1), G, G|BL(3), G, G, G|BL(3), G, P1|DI(1) } },

    { 8, 1, 1, 4, SPK_T_GAP | SPK_T_BLOCK | SPK_T_DIAMOND,
      { P1|DI(1), G, G|SPR, G, G|P2|DI(2), G|P2|DI(2), G|P2|DI(2), P1 } },

    { 8, 2, 2, 4, SPK_T_GAP | SPK_T_BLOCK | SPK_T_DIAMOND,
      { P2|DI(2), P1, G|SPR, G, G|P3|DI(3), G|P1|P3|DI(3), G|P3|DI(3),
        P2 } },

    /* Moves the player from level 1 to level 2 for level 2 phrases */
    { 4, 1, 2, 5, SPK_T_GAP,
      { P1, P1, P2, P2|DI(2) } },

    /* Moves the player to level 2 for level 2 phrases */
    { 8, 0, 2, 5, SPK_T_GAP,
      { G, G, G, G, P1, P1, P2, P2 } },

    { 4, 2, 2, 5, SPK_T_REST | SPK_T_GAP | SPK_T_DIAMOND,
      { P2, P2|DI(2), P2|DI(2), P2 } },

    { 4, 2, 2, 5, SPK_T_GAP | SPK_T_DIAMOND | SPK_T_CREATURE,
      { P2, P2|DI(2), P2|CR(2), P2 } },

    { 4, 2, 2, 5, SPK_T_GAP | SPK_T_DIAMOND | SPK_T_SPIKED,
      { P2, P2, P2|CR2(2), P2 } },

    { 4, 2, 2, 5, SPK_T_GAP | SPK_T_SPRING,
      { P2, G|SPR, __, P2 } },

    { 8, 2, 2, 5, SPK_T_GAP | SPK_T_DIAMOND,
      { P2, __, P2|DI(2), __, P2|DI(2), __, P2, P2 } },

    { 8, 2, 2, 5, SPK_T_GAP | SPK_T_DIAMOND,
      { P2, G|SPR, __, G|P3|DI(3), P3|DI(3), P1|P3|DI(3), __, P2 } },

    { 8, 2, 2, 5, SPK_T_GAP | SPK_T_DIAMOND,
      { P2, __, P2|DI(2), __, P3, P2|DI(2), __, P2 } },

    { 8, 2, 2, 4, SPK_T_GAP | SPK_T_BLOCK | SPK_T_DIAMOND,
      { G|P2, G|P2|DI(2), P3, G|P3|DI(3), G|P3|DI(3)|BL(3), G|P3,
        G|P2|DI(2), G|P2 } },

    { 12, 2, 2, 5, SPK_T_GAP | SPK_T_BLOCK | SPK_T_DIAMOND,
      { P2|DI(2), P1, G, G|BL(3), G, G|BL(3), G, G, P1, P1|DI(1), P2,
        P2|DI(2) } },

    { 8, 2, 1, 5, SPK_T_GAP | SPK_T_BLOCK | SPK_T_DIAMOND,
      { P2|DI(2), P1, G, G|BL(3), G, G, P1, P1 } },

    /* Moves the player from level 2 to level 1 for level 1 phrases */
    { 4, 2, 1, 5, SPK_T_GAP,
      { P2|DI(2), P2|DI(2), P1, P1 } },

    /* Moves the player from level 2 to level 3 for level 3 phrases */
    { 4, 2, 3, 6, SPK_T_GAP,
      { P2, P2, P3, P3 } },

    /* Moves the player to level 3 for level 3 phrases */
    { 8, 0, 3, 6, SPK_T_GAP,
      { G, G, P1, P1, P2, P2, P3, P3 } },

    /* Moves the player to level 3 for level 3 phrases */
    { 8, 0, 3, 6, SPK_T_GAP | SPK_T_SPRING,
      { G, G, G|SPR, __, P3, P3, P3, P3 } },

    /* Moves the player to level 3 for level 3 phrases */
    { 8, 0, 2, 6, SPK_T_GAP | SPK_T_SWITCH,
      { G, G, G|SW(0), G, P1|SWP, P1|SWP, P2|SWP, P2|SWP } },

    /* Moves the player from level 2 to level 3 for level 3 phrases */
    { 4, 3, 3, 6, SPK_T_REST | SPK_T_GAP | SPK_T_DIAMOND,
      { P3, P3|DI(3), P3|DI(3), P3 } },

    /* Moves the player from level 2 to level 3 for level 3 phrases */
    { 4, 3, 3, 6, SPK_T_GAP | SPK_T_CREATURE,
      { P3, P3, P3|CR(3), P3 } },

    /* Moves the player from level 2 to level 3 for level 3 phrases */
    { 4, 3, 3, 6, SPK_T_GAP | SPK_T_SPIKED,
      { P3, P3, P3|CR2(3), P3 } },

    /* Moves the player from level 2 to level 3 for level 3 phrases */
    { 4, 3, 3, 6, SPK_T_GAP | SPK_T_CREATURE,
      { P3, __, P3|CR(3), P3 } },

    /* Moves the player from level 2 to level 3 for level 3 phrases */
    { 4, 3, 3, 6, SPK_T_GAP | SPK_T_SPRING,
      { P3, G|SPR, __, P3 } },

    { 8, 3, 3, 6, SPK_T_GAP | SPK_T_DIAMOND,
      { P3, __, P3|SW(3), __, P3|SWP, __, P3|SWP, P3|SWP } },

    { 8, 3, 3, 6, SPK_T_GAP | SPK_T_DIAMOND,
      { P3, G|SPR, __, P3, P3, P3|SW(3), __, P3|SWP } },

    { 8, 3, 3, 6, SPK_T_GAP | SPK_T_DIAMOND,
      { P3, P3, P3|SW(3), G|SPR, __, P3|SWP, P3|SWP, P3|SWP } },

    { 8, 3, 3, 6, SPK_T_GAP | SPK_T_CREATURE | SPK_T_DIAMOND | SPK_T_BAIT
                  | SPK_T_SPRING,
      { P3, G, G|CR(0), G, G|DI(0), G|SPR, __, P3 } },

    { 8, 3, 3, 6, SPK_T_GAP | SPK_T_CREATURE | SPK_T_DIAMOND | SPK_T_SPRING,
      { P3, G, G|SW(0), G|DI(0), G, G|SPR, __, P3|SWP } },

    { 16, 3, 3, 6, SPK_T_GAP | SPK_T_CREATURE | SPK_T_BLOCK | SPK_T_DIAMOND,
      { P3, P2, P1, G, G|CR(0), G, G|BL(6), G, G|CR(0), G, P1, P1, P2,
        P2|DI(2), P3, P3|DI(3) } },

    { 16, 3, 2, 6, SPK_T_GAP | SPK_T_BLOCK | SPK_T_DIAMOND | SPK_T_SPIKED,
      { P3, P2, P1, G, G|BL(3), G, G|CR2(0), G, G|BL(3), G, G|CR2(0), G,
        P1, P1, P2, P2 } },

    { 16, 3, 3, 6, SPK_T_GAP | SPK_T_CREATURE | SPK_T_BLOCK | SPK_T_DIAMOND
                   | SPK_T_BAIT,
      { P3, P2, P1, G, G|BL(3), G|DI(0), G|CR(0), G|DI(0), G|SW(0), G,
        P1|SWP, P1|DI(1)|SWP, P2|SWP, P2|DI(2)|SWP, P3|SWP, P3|DI(3)|SWP } },

    /* Moves the player from level 3 to level 2 for level 2 phrases */
    { 4, 3, 2, 5, SPK_T_GAP,
      { P3, P3, P2, P2 } },

    { 4, 3, 0, 6, SPK_T_REST,
      { P3, P2|DI(2), P1, G } },

    { 4, 2, 0, 5, SPK_T_REST,
      { P2, P2|DI(2), P1, G } },

    { 4, 1, 0, 4, SPK_T_REST,
      { P1, P1|DI(1), G, G } },

    { 16, 0, 0, 5, SPK_T_BLOCK | SPK_T_DIAMOND | SPK_T_BAIT,
      { G, G, G|P1, G, G|P2|DI(0), G, G|P3|DI(3), G|P3|DI(3), G|P3|DI(3),
        G|P3|DI(3)|BL(3), G|P3|DI(3), G|P3|DI(3)|BL(3), G|P3|DI(3),
        G|P3|DI(3)|BL(3), G|P3|DI(3), G|DI(0) } },

    { 16, 0, 0, 5, SPK_T_BLOCK | SPK_T_DIAMOND | SPK_T_BAIT,
      { G, G, G|P1, G, G|P2|DI(2), G, G|P3|DI(0), G|P3|DI(3)|BL(3),
        G|P3|DI(0), G|P3|BL(12), G|P3|DI(0), G|P3|BL(3), G|P3|DI(0),
        G|P3|BL(12), G|P3|DI(0), G } },

    { 16, 0, 0, 5, SPK_T_BLOCK | SPK_T_DIAMOND | SPK_T_BAIT,
      { G, G, G|P1, G, G|P2|DI(2), G, G|P3|DI(0), G|P3|DI(3)|BL(6),
        G|P3|DI(0), G|P3|BL(12), G|P3|DI(0), G|P3|BL(6), G|P3|DI(0),
        G|P3|BL(12), G|P3|DI(0), G } }
};

const int spk_pattern_count =
    (int)(sizeof (spk_patterns) / sizeof (spk_patterns[0]));

/* What the wait walks on: bare ground, and nothing else.
 *
 * Not a library rest, and that is the point. A rest is still a phrase, and
 * several of them carry a platform row -- "a press here changes nothing" is
 * a thing the player has to be taught somewhere. Anything standing on the
 * field when the tempo latches is re-laid at the track's own cell index and
 * visibly moves; the re-seat is only invisible if there is nothing there to
 * move. */
static const struct spk_pattern spk_bare =
{
    4, 0, 0, 0, SPK_T_REST, { G, G, G, G }
};

#undef __
#undef G
#undef P1
#undef P2
#undef P3
#undef DI
#undef CR
#undef BL
#undef SW
#undef SWP
#undef SWG
#undef SWX
#undef CR2
#undef SPR


/** The ring **/

/* Cells kept. The field draws four behind the player and nine ahead, and a
 * jump resolves two past that, so anything over about twenty is enough --
 * this is four whole sixteen-cell patterns of slack on top. */
#define SPK_RING     64

static unsigned int ring[SPK_RING];
static int          ring_start[SPK_RING];    /* first cell of its pattern */
static int          filled;                 /* cells assembled so far */
static int          base;                   /* first cell still in the ring */

/* The whole of what a phrase is chosen by, and every term is a function of
 * the cell it starts on.
 *
 * That is the design and not an implementation detail. A course that depends
 * on how the player is doing is not a level: it cannot be learnt, replaying
 * it is not replaying anything, and two scores on the same song are not
 * comparable. So difficulty ramps with the *track's* own beat count, a death
 * costs points and nothing else, and nothing here remembers what came
 * before -- the map from cell to phrase is fixed for a song and the run is
 * laid over it wherever it happens to start.
 *
 * The tempo is mixed in as a seed so that two songs do not lay the same
 * course at the same cell index. It is latched once and never revisited, so
 * it is as fixed as the song is. */
#define SPK_TIER_CELLS   32      /* beats before the course is allowed to harden */
#define SPK_TIER_TOP     6       /* ...and the hardest it reaches */
#define SPK_TIER_LAST    256     /* ...but the last step costs this many */

/* A rest every so often, on the grid rather than on a count of phrases:
 * players need somewhere to breathe and the respawn needs a target it can
 * always find. Section 9.2 asks for one every eight phrases; phrases average
 * eight cells, so forty-eight beats is comfortably inside that even where
 * the short ones happen to cluster -- and it is about twenty-five seconds at
 * the game's tempo, which is what it has to be to feel like a breath. */
#define SPK_REST_CELLS   48

/* Where §9.3's conditions bite, out of a hundred.
 *
 * Wide bands with a lot of nothing between them, on purpose. Every one of
 * these is a threshold on a measured number, and a phrase either side of a
 * threshold is a different course -- so they are set where the music has
 * plainly done something rather than where it has merely drifted, and most
 * bars fall in the gap and choose on nothing. */
#define SPK_MOOD_HIGH    65      /* loud, against the track's own loudest */
#define SPK_MOOD_LOW     30
#define SPK_MOOD_BUSY    55      /* the bar's beats differ from each other */
#define SPK_MOOD_CALM    25
#define SPK_MOOD_RISE    15      /* this bar against the one before */
#define SPK_MOOD_DROP    (-30)

static int  tier_seed;          /* the track's tempo, mixed into every pick */
static int  last_pick = -1;     /* the phrase before, which is not offered */
static int  last_start;         /* ...and where it began, for the rest count */
static bool flat;               /* rests only, for as long as it is set */
static int  last_exit;          /* level the pattern before left the player on */
static int  bar_rot;            /* the residue of four the downbeat sits on */

/* The bar just heard, or neutral where nothing has said otherwise. */
static int  mood_energy = 50;
static int  mood_flux = 50;
static int  mood_trend;


/** Choosing what comes next **/

/* Deterministic, and keyed on where in the track the phrase falls rather
 * than on how many have been laid. Both halves of that matter: the same
 * point in the same song gives the same phrase, and two runs that pick the
 * track up at different points do not lay the same phrases in the same
 * order. */
static unsigned long spk_gen_hash(unsigned long n)
{
    n ^= n >> 16;
    n *= 0x7feb352dul;
    n ^= n >> 15;
    n *= 0x846ca68bul;
    n ^= n >> 16;

    return n;
}

/* §10's ramp, against the track's beat count. A song opens on gaps and
 * nothing else and has everything by the time it is half a minute in,
 * whatever the player has been doing. */
/* Where the assembler is, with the bar rotation taken out.
 *
 * Every choice below is a function of this and never of the cell index, and
 * that is the whole of what lets the downbeat move the phrase boundaries
 * without changing the course: a run that found the bar and a run that did
 * not lay the *same phrases in the same order*, offset by the rotation and
 * by nothing else. Hash the cell itself instead and a rotation of one
 * re-hashes every phrase in the song, which is a different level. */
static int spk_gen_at(int cell)
{
    return cell - bar_rot;
}

static int spk_tier_at(int cell)
{
    int t = 1 + cell / SPK_TIER_CELLS;

    /* The last step is held back past its turn on the ramp. Its phrases stop
     * testing one thing at a time, and on an even ramp a track arrives there
     * before it is half over and then stays. SPK_TIER_LAST is a multiple of
     * SPK_TIER_CELLS, so the teaching window still lines up with the band it
     * opens.
     *
     * A rising bar still buys the tier above the ramp's, so between the two
     * counts the top tier is what a track climbs to and falls back from
     * rather than what it sits at. */
    if (t >= SPK_TIER_TOP && cell < SPK_TIER_LAST)
        t = SPK_TIER_TOP - 1;

    return t > SPK_TIER_TOP ? SPK_TIER_TOP : t;
}

/* §9.3's table, as tags to prefer. Zero where the music is saying nothing
 * in particular, which is most of the time and is meant to be.
 *
 * A preference and not a filter: it reorders phrases the tier already
 * allows. Filtering would let a loud bar reach past the ramp, and the ramp
 * is what stops a creature arriving in the first phrase of a song. */
static unsigned short spk_gen_favoured(void)
{
    bool loud = mood_energy >= SPK_MOOD_HIGH;
    bool quiet = mood_energy <= SPK_MOOD_LOW;
    bool busy = mood_flux >= SPK_MOOD_BUSY;
    bool still = mood_flux <= SPK_MOOD_CALM;

    if (loud && busy)
        return SPK_T_CREATURE;
    if (loud && still)
        return SPK_T_GAP;
    if (quiet && still)
        return SPK_T_REST | SPK_T_DIAMOND;

    return 0;
}

/* Candidates one pick may consider: the whole library, because at the top
 * tier every pattern qualifies at once. Sized from the table so that adding
 * a pattern cannot silently put it out of reach, and static because the
 * generator is single-threaded and this grows with the library on a stack
 * the frame loop shares. */
static int candidates[ARRAYLEN(spk_patterns)];

/* Whether a phrase can be walked into at that level.
 *
 * Either it says so, or it carries a surface there at both ends -- a phrase
 * with a floor and a storey above it is two phrases, and which one is played
 * depends only on where the player arrives. Entered above its own floor it
 * leaves at that level too, so the join holds either way.
 *
 * Derived from the cells rather than declared, because a flag beside them
 * would be a second thing to keep in step with the first. Read from the mask
 * and not from what a switch has done: the entry and the exit are properties
 * of the phrase, and a switch is thrown inside it. */
bool spk_pat_at(const struct spk_pattern *p, int level)
{
    unsigned int bit = 1u << level;

    if ((int)p->entry == level)
        return true;

    return (p->cells[0] & SPK_C_MASK & bit)
           && (p->cells[p->length - 1] & SPK_C_MASK & bit);
}

static const struct spk_pattern *spk_gen_pick(int entry)
{
    int n = 0, i;
    int at = spk_gen_at(filled);
    int tier = spk_tier_at(at);
    bool owed = at / SPK_REST_CELLS
                != spk_gen_at(last_start) / SPK_REST_CELLS;

    /* A breakdown should read as one. The only place the music overrides
     * rather than leans: a sharp drop in level is the one musical event a
     * player hears as an instruction. */
    if (mood_trend <= SPK_MOOD_DROP)
        owed = true;

    /* Rising against the bar before buys one tier, and no more -- the ramp
     * still decides what a song has unlocked by this point in it. */
    if (mood_trend >= SPK_MOOD_RISE && tier < SPK_TIER_TOP)
        tier++;

    /* The run has not started, or has stopped offering anything: bare
     * ground, so that nothing can be scored on it and nothing on it moves
     * when the world is re-seated at the latch. */
    if (flat)
        return &spk_bare;

    if (owed)
    {
        for (i = 0; i < spk_pattern_count; i++)
            if ((spk_patterns[i].tags & SPK_T_REST)
                && spk_pat_at(&spk_patterns[i], entry))
                candidates[n++] = i;
    }

    /* The phrase a tier band opens on teaches what the band unlocked, where
     * the library has something that teaches it. On the grid, like
     * everything else here, so it happens at the same place every time. */
    if (n == 0 && tier > 1 && (at % SPK_TIER_CELLS) < SPK_PAT_MAX)
    {
        for (i = 0; i < spk_pattern_count; i++)
            if ((spk_patterns[i].tags & SPK_T_TEACH)
                && spk_pat_at(&spk_patterns[i], entry)
                && (int)spk_patterns[i].difficulty == tier)
                candidates[n++] = i;
    }

    if (n == 0)
    {
        for (i = 0; i < spk_pattern_count; i++)
        {
            const struct spk_pattern *p = &spk_patterns[i];

            if (!spk_pat_at(p, entry))
                continue;
            if (p->difficulty > (unsigned char)tier)
                continue;

            candidates[n++] = i;
        }
    }

    /* What the music is asking for, among what is already allowed. */
    if (n > 1)
    {
        unsigned short want = spk_gen_favoured();

        if (want)
        {
            int keep = 0;

            for (i = 0; i < n; i++)
                if (spk_patterns[candidates[i]].tags & want)
                    candidates[keep++] = candidates[i];

            if (keep)
                n = keep;
        }
    }

    /* Not the one just laid. This is the only thing here that remembers
     * anything, and it is safe: two runs over the same song walk the same
     * phrase boundaries, so they remember the same phrase. A run that joined
     * the song elsewhere can disagree until the two walks land on the same
     * cell, which takes a phrase or two, and then never again. */
    if (n > 1)
    {
        int keep = 0;

        for (i = 0; i < n; i++)
            if (candidates[i] != last_pick)
                candidates[keep++] = candidates[i];

        if (keep)
            n = keep;
    }

    if (n == 0)
        return &spk_patterns[0];

    i = candidates[spk_gen_hash((unsigned long)at
                               + (unsigned long)tier_seed * 2654435761ul)
                   % (unsigned long)n];
    last_pick = i;
    last_start = filled;

    return &spk_patterns[i];
}

static void spk_gen_extend(void)
{
    int at = last_exit;
    const struct spk_pattern *p = spk_gen_pick(at);
    int i;

    /* A phrase entered above its own floor is left at the level it was
     * entered at. One entered where it says it should be leaves where it
     * says -- and so does the bare ground the wait walks on, which matches
     * nothing and is handed back regardless. */
    last_exit = ((int)p->entry != at && spk_pat_at(p, at)) ? at : p->exit;
    for (i = 0; i < p->length; i++)
    {
        int cell = filled + i;

        ring[cell % SPK_RING] = p->cells[i];
        ring_start[cell % SPK_RING] = filled;
    }

    filled += p->length;

    /* What the ring can no longer answer for. The field draws four cells
     * behind the player and the course is laid a pattern or two ahead, so
     * this is far behind anything that will be asked about again. */
    if (filled - base > SPK_RING)
        base = filled - SPK_RING;
}

void spk_gen_reset(int cell)
{
    flat = false;
    last_pick = -1;
    last_exit = 0;
    /* Back to the four grid, then on to the downbeat's residue -- and a
     * whole four back, so the start is never ahead of the cell asked for.
     *
     * Written this way so that spk_gen_at(filled) comes out the same for
     * every rotation: the walk begins at the same *un-rotated* phrase
     * whatever the bar turned out to be, and the courses then correspond
     * exactly rather than converging after a phrase or two. */
    filled = cell - (cell % 4) + bar_rot - 4;

    /* The ring is indexed by cell modulo its length, so a start before zero
     * indexes it backwards. Adding a whole four keeps both properties this
     * is built on: the residue is still the downbeat's, and every rotation
     * still shares one un-rotated origin, because they all move together. */
    while (filled < 0)
        filled += 4;

    base = filled;
    last_start = filled;
}

void spk_gen_set_mood(int energy, int flux, int trend)
{
    mood_energy = energy;
    mood_flux = flux;
    mood_trend = trend;
}

void spk_gen_set_bar(int rot)
{
    bar_rot = rot & 3;
}

int spk_gen_bar(void)
{
    return bar_rot;
}

void spk_gen_set_seed(int seed)
{
    tier_seed = seed;
}

void spk_gen_set_flat(bool on)
{
    flat = on;
}

int spk_gen_next_cell(void)
{
    return filled;
}

unsigned int spk_gen_cell(int cell)
{
    if (cell < base)
        return 1u;              /* behind the ring: flat ground, and safe */

    while (cell >= filled)
        spk_gen_extend();

    return ring[cell % SPK_RING];
}

int spk_gen_pattern_diamonds(int cell)
{
    int start = spk_gen_pattern_start(cell);
    int n = 0, i;

    for (i = 0; i < SPK_PAT_MAX; i++)
    {
        if (i && spk_gen_pattern_start(start + i) != start)
            break;

        if (spk_gen_cell(start + i) & SPK_C_DIF)
            n++;
    }

    return n;
}

int spk_gen_pattern_start(int cell)
{
    if (cell < base)
        return cell;

    while (cell >= filled)
        spk_gen_extend();

    return ring_start[cell % SPK_RING];
}
