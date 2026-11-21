/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Debug > EQ on COP Check: the equalizer's bands on the COP must give the
 * same samples as on the CPU. The same pseudo-random stereo goes through the
 * current bands on each core, from the same cleared history and in the
 * DSP's own block size, and the two outputs are compared sample by sample.
 * Music may keep playing: the playing EQ waits for the check and is put
 * back as it was.
 *
 * The buffers are reached through SDRAM's uncached alias, the only memory
 * outside IRAM the COP may share, so both timings are of uncached memory
 * and slower than the EQ's own, which works in IRAM.
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "eq_cop.h"
#include "screens/system/eq_cop_check.h"
#include "widgets/list.h"

#define CHECK_FRAMES 1024

/* Whole cache lines, so no neighbour's line written back later can land on
   what the uncached alias wrote */
static int32_t frames[4][CHECK_FRAMES] CACHEALIGN_ATTR;

static uint32_t lcg;

/* About a quarter of full scale, room for the bands' boost */
static int32_t noise(void)
{
    lcg = lcg * 1664525u + 1013904223u;
    return (int32_t)lcg >> 5;
}

static const char *state_text(enum eq_cop_state s)
{
    switch (s)
    {
    case EQ_COP_RUNNING:   return "on the COP";
    case EQ_COP_NO_THREAD: return "on the CPU: no worker thread";
    case EQ_COP_TIMED_OUT: return "on the CPU: the COP stopped answering";
    default:               return "on the CPU";
    }
}

bool eq_cop_check_screen(void)
{
    struct simplelist_info info;
    int32_t *base, *cpu[2], *cop[2];
    unsigned long t0, cpu_us, cop_us;
    bool ran, filtered = false;
    int differ = -1;

    commit_discard_dcache();
    base = UNCACHED_ADDR(&frames[0][0]);
    cpu[0] = base;
    cpu[1] = base + CHECK_FRAMES;
    cop[0] = base + 2 * CHECK_FRAMES;
    cop[1] = base + 3 * CHECK_FRAMES;

    lcg = 1;
    for (int i = 0; i < CHECK_FRAMES; i++)
    {
        cpu[0][i] = cop[0][i] = noise();
        cpu[1][i] = cop[1][i] = noise();
    }

    cpu_boost(true);
    t0 = USEC_TIMER;
    dsp_eq_check_run(cpu, CHECK_FRAMES, false);
    cpu_us = USEC_TIMER - t0;
    t0 = USEC_TIMER;
    ran = dsp_eq_check_run(cop, CHECK_FRAMES, true);
    cop_us = USEC_TIMER - t0;
    cpu_boost(false);

    lcg = 1;
    for (int i = 0; i < CHECK_FRAMES; i++)
    {
        int32_t l = noise(), r = noise();

        if (cpu[0][i] != l || cpu[1][i] != r)
            filtered = true;
        if (differ < 0 && (cpu[0][i] != cop[0][i] || cpu[1][i] != cop[1][i]))
            differ = i;
    }

    simplelist_info_init(&info, "EQ on COP Check", 0, NULL);
    info.scroll_all = true;
    simplelist_reset_lines();
    simplelist_addline("The EQ runs %s", state_text(eq_cop_state()));
    if (!filtered)
        simplelist_addline("No band is on: nothing to compare");
    else if (!ran)
        simplelist_addline("The COP did not take the check");
    else if (differ < 0)
        simplelist_addline("Matched: every sample");
    else
        simplelist_addline("DIFFERED, from frame %d", differ);
    simplelist_addline("%d frames: CPU %lu us, COP %lu us", CHECK_FRAMES,
                       cpu_us, cop_us);
    return simplelist_show_list(&info);
}
