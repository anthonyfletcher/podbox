/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The downbeat.
 *
 * The tracker counts the bar from wherever its lock landed and says so
 * itself: finding the music's own "one" needs meter detection it does not
 * attempt. What it does give is an even grid of beats, and what the
 * analyser gives is the strength of every low-band onset. A kick drum lands
 * on the downbeat more often than it lands anywhere else, so accumulating
 * onset strength into four bins -- one per beat position -- and taking the
 * strongest is enough to find it, over enough bars.
 *
 * The bars are the ones already gone. Onset times are facts, so they can be
 * binned against a grid that was not known when they were heard: the run
 * records them through the whole wait and asks the question once, at the
 * latch, from a history that by then is usually half a minute deep. Nothing
 * is waited for that was not being waited for anyway.
 *
 * Parts, in order:
 *   - the onsets heard
 *   - binning them
 ****************************************************************************/

#include <stddef.h>
#include "config.h"
#include "audio/beat_analyse.h"
#include "games/spike/spike_bar.h"

/* Low-band onsets kept. They fire a few times a second, so this reaches
 * back the better part of a minute -- far more than the eight bars the
 * decision wants, and the surplus is what covers a track that opens
 * without a kick. */
#define SPK_BAR_ONSETS   128

/* Eight bars, as beats: the least the decision is taken on. */
#define SPK_BAR_BEATS    32

/* And twelve, which is the most it looks back over. Counted in beats rather
 * than in seconds so that a slow track still reaches its eight bars. The
 * period is good to a millisecond or two and that error accumulates as
 * phase across the span, so 48 beats has drifted about a seventh of one by
 * the far end -- well inside the tolerance below, which is what lets the
 * oldest onsets still be binned where they belong. */
#define SPK_BAR_SPAN     48

/* An onset counts toward a beat only if it is near one. A quarter of a beat
 * either side takes the swing and the loose drummer and leaves the
 * off-beats out, which is what stops a backbeat-heavy track binning its
 * snares into the wrong four. */
#define SPK_BAR_TOL      4       /* beat_ms divided by this */

/* What the strongest bin must beat: the mean of the other three, times
 * this over ten. A real downbeat is half again as strong as its
 * neighbours; anything less is a track whose bar this cannot find, and
 * saying so is better than committing to a rotation that puts every phrase
 * a beat out of step for the whole song. */
#define SPK_BAR_MARGIN   14

/* At least this many onsets in the span: one a bar over the eight, which is
 * what a track that kicks only on the downbeat gives. Asking for more would
 * refuse the clearest case there is. */
#define SPK_BAR_MIN_HITS 8


/** The onsets heard **/

static unsigned long on_ms[SPK_BAR_ONSETS];
static unsigned char on_str[SPK_BAR_ONSETS];
static int  on_head;            /* where the next one goes */
static int  on_count;
static unsigned long on_last;   /* newest window already taken in */

/* What the last decision was taken on, for the screen that reports it. */
static int  last_hits;
static int  last_margin10;

void spk_bar_reset(void)
{
    on_head = 0;
    on_count = 0;
    on_last = 0;
    last_hits = 0;
    last_margin10 = 0;
}

void spk_bar_working(int *hits, int *margin10)
{
    *hits = last_hits;
    *margin10 = last_margin10;
}

static void spk_bar_push(unsigned long ms, unsigned char strength)
{
    on_ms[on_head] = ms;
    on_str[on_head] = strength;

    if (++on_head >= SPK_BAR_ONSETS)
        on_head = 0;
    if (on_count < SPK_BAR_ONSETS)
        on_count++;
}

void spk_bar_listen(void)
{
    int fresh = 0, i;

    /* Newest first, back to the last window already taken. The analyser
     * hands out at most BEAT_HISTORY of them and a frame cannot have
     * produced anything like that many, so the walk is a few steps. */
    while (fresh < BEAT_HISTORY)
    {
        const struct beat_window *w = beat_analyse_window(fresh);

        if (w == NULL || w->time_ms <= on_last)
            break;

        fresh++;
    }

    if (fresh == 0)
        return;

    /* And forward again, so the ring stays in time order and evicts its
     * oldest rather than an arbitrary one. */
    for (i = fresh - 1; i >= 0; i--)
    {
        const struct beat_window *w = beat_analyse_window(i);

        if (w == NULL)
            continue;

        on_last = w->time_ms;

        if ((w->onset & (1u << BEAT_LOW)) && w->strength > 0)
            spk_bar_push(w->time_ms, w->strength);
    }
}


/** Binning them **/

int spk_bar_downbeat(long zero_ms, int beat_ms)
{
    long bins[4] = { 0, 0, 0, 0 };
    long newest, oldest = 0, total = 0, best = 0;
    int tol = beat_ms / SPK_BAR_TOL;
    int hits = 0, win = 0, i;

    last_hits = 0;
    last_margin10 = 0;

    if (on_count == 0 || beat_ms <= 0)
        return -1;

    newest = (long)on_ms[(on_head + SPK_BAR_ONSETS - 1) % SPK_BAR_ONSETS];

    for (i = 0; i < on_count; i++)
    {
        int slot = (on_head + SPK_BAR_ONSETS - 1 - i) % SPK_BAR_ONSETS;
        long at = (long)on_ms[slot];
        long delta = at - zero_ms;
        long index, err;

        if (newest - at > (long)SPK_BAR_SPAN * beat_ms)
            break;

        if (delta < 0)
            continue;

        index = (delta + beat_ms / 2) / beat_ms;
        err = delta - index * beat_ms;

        if (err > tol || err < -tol)
            continue;

        bins[index & 3] += on_str[slot];
        oldest = at;
        hits++;
    }

    last_hits = hits;

    if (hits < SPK_BAR_MIN_HITS
        || newest - oldest < (long)SPK_BAR_BEATS * beat_ms)
        return -1;

    for (i = 0; i < 4; i++)
    {
        total += bins[i];
        if (bins[i] > best)
        {
            best = bins[i];
            win = i;
        }
    }

    /* best against the mean of the other three, in tenths: 10 is level with
     * them and 20 is twice as strong. Kept whatever the verdict, because
     * the number is the evidence and the verdict is only the threshold
     * applied to it. */
    if (total > best)
        last_margin10 = (int)((best * 30) / (total - best));

    if (best * 30 < (total - best) * SPK_BAR_MARGIN)
        return -1;

    return win;
}
