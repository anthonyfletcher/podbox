/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Measures what a track sounds like, from audio a decoder hands it.
 *
 * Two kinds of measurement, gathered in one pass because both want the same
 * samples. Level, dynamics and stereo width are taken from the PCM directly.
 * Tempo, band balance, onset rate and flatness are taken from beat_hops.c,
 * whose per-hop windows this walks as they appear -- everything it reports
 * was computed for the beat tracker anyway, so the extra axes cost almost
 * nothing beyond reading them.
 *
 * Parts, in order:
 *   - accumulators
 *   - the sink: PCM measurements, then the hop machine
 *   - harvesting hop windows and the tempo
 *   - the result
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "fixedpoint.h"
#include "dsp_core.h"
#include "audio/beat_probe.h"
#include "audio/beat_track.h"
#include "audio/chroma.h"

/* Frames converted at a time. The hop machine wants interleaved 16-bit and
 * the codec produces neither, so a block passes through here; small enough
 * to stay off the stack's conscience, large enough that the per-call costs
 * disappear. */
#define BP_STAGE_FRAMES   256

/* When the tempo is worth keeping: the envelope this full, and the period
 * held this long without drifting more than this.
 *
 * BP_AGREE_PCT matches beat_track's own agreement window -- a second opinion
 * on what counts as the same tempo would only disagree with it.
 *
 * The other two are the knee of a measured curve (18 tracks, ~/sndprobe).
 * Waiting for a full envelope costs 18.4s of audio a track and buys almost
 * nothing: at 60 the stored tempo moves on one track of the eighteen, and
 * that one reads a 181ms spread at either setting, which is the tracker
 * saying it does not believe its own answer. Below 60 it stops being free --
 * the hardest track in the set falls from 129 BPM to 49, which is the tail
 * a half-filled envelope is expected to produce. So 60 is the far end of the
 * safe part rather than the best score, and the run is 37% shorter for it.
 *
 * The hold barely binds once the fill gate is this low; 2000 is where it
 * stops costing anything. */
#define BP_FILL_PCT       60
#define BP_HOLD_MS        2000
#define BP_AGREE_PCT      6

#define BP_DIFF(a, b)     ((a) > (b) ? (a) - (b) : (b) - (a))

static int16_t       stage[BP_STAGE_FRAMES * 2];

/* PCM measurements. Mean square fits a 32-bit word at full scale, but the
 * running sum over forty seconds does not: 1.7 million frames of 2^30. */
static uint64_t      sum_mid_sq;
static uint64_t      sum_side_sq;
static uint32_t      peak_mid;
static uint64_t      frames_in;

/* Hop-window measurements. */
static unsigned int  windows_seen;
static uint32_t      sum_level[BEAT_GROUPS];
static uint32_t      onset_count[BEAT_GROUPS];
static uint32_t      sum_peak;
static uint32_t      sum_strength;
static uint32_t      strength_n;
static uint32_t      sum_mid_level;
static uint64_t      sum_mid_level_sq;
static uint32_t      level_n;

/* Tempo. */
static unsigned int  period_ms;
static unsigned int  confidence;
static unsigned int  period_lo;
static unsigned int  period_hi;
static unsigned int  hold_period;    /* What the hold is measured against */
static unsigned long hold_since_ms;
static bool          settled;

static unsigned int  samplerate;
static unsigned long first_ms;
static unsigned long last_ms;
static bool          have_first;


/** Accumulators **/

void beat_probe_start(void)
{
    memset(sum_level, 0, sizeof (sum_level));
    memset(onset_count, 0, sizeof (onset_count));

    sum_mid_sq = 0;
    sum_side_sq = 0;
    peak_mid = 0;
    frames_in = 0;

    windows_seen = 0;
    sum_peak = 0;
    sum_strength = 0;
    strength_n = 0;
    sum_mid_level = 0;
    sum_mid_level_sq = 0;
    level_n = 0;

    period_ms = 0;
    confidence = 0;
    period_lo = 0;
    period_hi = 0;
    hold_period = 0;
    hold_since_ms = 0;
    settled = false;

    samplerate = 0;
    first_ms = 0;
    last_ms = 0;
    have_first = false;

    /* The rate is not known until the codec announces one, and both of
     * these are reset again the moment it does. */
    beat_hops_reset(0);
    chroma_reset(0);
}

/* Integer square root, for putting a variance back on the scale of the thing
 * it describes. */
static uint32_t bp_root(uint32_t v)
{
    uint32_t r = 0;
    uint32_t bit = 1UL << 30;

    while (bit > v)
        bit >>= 2;

    while (bit != 0)
    {
        if (v >= r + bit)
        {
            v -= r + bit;
            r = (r >> 1) + bit;
        }
        else
        {
            r >>= 1;
        }
        bit >>= 2;
    }

    return r;
}


/** Harvesting **/

/* Fold in every hop window the machine has produced since the last call.
 * More than the ring holds cannot be recovered, and would mean a caller
 * feeding seconds at a time. */
static void bp_take_windows(void)
{
    unsigned int now = beat_hops_windows();
    unsigned int fresh = now - windows_seen;
    unsigned int i;

    if (fresh > BEAT_HISTORY)
        fresh = BEAT_HISTORY;

    for (i = fresh; i-- > 0; )
    {
        const struct beat_window *w = beat_hops_window((int)i);
        int g;

        if (w == NULL)
            continue;

        for (g = 0; g < BEAT_GROUPS; g++)
        {
            sum_level[g] += w->level[g];

            if (w->onset & (1u << g))
                onset_count[g]++;
        }

        sum_peak += w->peakiness;

        sum_mid_level += w->level[BEAT_MID];
        sum_mid_level_sq += (uint64_t)w->level[BEAT_MID] * w->level[BEAT_MID];
        level_n++;

        /* Only hops that fired have a strength, and averaging the zeros in
         * would report how often the track has onsets rather than how hard
         * they hit -- which rate10[] already says, better. */
        if (w->strength > 0)
        {
            sum_strength += w->strength;
            strength_n++;
        }
    }

    windows_seen = now;
}

static void bp_take_tempo(unsigned long track_ms)
{
    struct beat_track bt;

    beat_track_get(&bt);

    if (!bt.locked || bt.period_ms == 0)
    {
        /* A lost lock restarts the hold: what came before it described a
         * tempo the tracker has since abandoned. */
        hold_period = 0;
        hold_since_ms = track_ms;
        return;
    }

    period_ms = bt.period_ms;
    confidence = bt.confidence;

    if (period_lo == 0 || bt.period_ms < period_lo)
        period_lo = bt.period_ms;
    if (bt.period_ms > period_hi)
        period_hi = bt.period_ms;

    if (hold_period == 0 ||
        BP_DIFF(bt.period_ms, hold_period) * 100 > hold_period * BP_AGREE_PCT)
    {
        hold_period = bt.period_ms;
        hold_since_ms = track_ms;
    }

    if (beat_track_fill() >= BP_FILL_PCT &&
        track_ms > hold_since_ms &&
        track_ms - hold_since_ms >= BP_HOLD_MS)
    {
        settled = true;
    }
}

bool beat_probe_settled(void)
{
    return settled;
}


/** The sink **/

/* One frame's two channels, brought to 16 bits. */
static void bp_frame(const int32_t *a, const int32_t *b, int i,
                            int mode, int shift, int32_t *l, int32_t *r)
{
    switch (mode)
    {
    case STEREO_INTERLEAVED:
        *l = a[i * 2] >> shift;
        *r = a[i * 2 + 1] >> shift;
        break;

    case STEREO_MONO:
        *l = a[i] >> shift;
        *r = *l;
        break;

    default:  /* STEREO_NONINTERLEAVED */
        *l = a[i] >> shift;
        *r = (b != NULL ? b[i] : a[i]) >> shift;
        break;
    }

    if (*l > 32767) *l = 32767;
    if (*l < -32768) *l = -32768;
    if (*r > 32767) *r = 32767;
    if (*r < -32768) *r = -32768;
}

void beat_probe_sink(const void *ch1, const void *ch2, int count,
                     const struct track_pcm *fmt, unsigned long track_ms)
{
    const int32_t *a = ch1;
    const int32_t *b = ch2;
    int done = 0;

    if (count <= 0 || a == NULL)
        return;

    if (fmt->frequency != samplerate)
    {
        samplerate = fmt->frequency;
        beat_hops_set_rate(samplerate);
        chroma_reset(samplerate);
    }

    if (samplerate == 0)
        return;

    if (!have_first)
    {
        first_ms = track_ms;
        have_first = true;
    }
    last_ms = track_ms;

    /* The block's own place in the track, so hops land on the track's
     * timeline rather than on wherever the decode happened to start. */
    beat_hops_set_pos(track_ms);

    while (done < count)
    {
        int n = count - done;
        int i;

        if (n > BP_STAGE_FRAMES)
            n = BP_STAGE_FRAMES;

        for (i = 0; i < n; i++)
        {
            int32_t l, r, mid, side;

            bp_frame(a, b, done + i, fmt->stereo_mode, fmt->shift, &l, &r);

            stage[i * 2]     = (int16_t)l;
            stage[i * 2 + 1] = (int16_t)r;

            mid = (l + r) / 2;
            side = (l - r) / 2;

            sum_mid_sq += (uint64_t)((int64_t)mid * mid);
            sum_side_sq += (uint64_t)((int64_t)side * side);

            if ((uint32_t)(mid < 0 ? -mid : mid) > peak_mid)
                peak_mid = (uint32_t)(mid < 0 ? -mid : mid);
        }

        frames_in += n;
        beat_hops_feed(stage, n);
        chroma_feed(stage, n);
        done += n;
    }

    bp_take_windows();
    bp_take_tempo(track_ms);
}


/** The result **/

/* Decibels of an amplitude against full scale, in tenths. */
static int bp_db10_amplitude(uint32_t amp)
{
    if (amp == 0)
        return -960;

    /* Q16 of amp/32768, which is amp*2 exactly. */
    return (int)((fp_decibels(amp * 2, 16) * 10) / 65536);
}

/* The same for a mean square, which is the amplitude's decibels doubled. */
static int bp_db10_power(uint32_t mean_sq)
{
    uint32_t ratio_q16 = mean_sq / 16384;   /* mean_sq / 32768^2, in Q16 */

    if (ratio_q16 == 0)
        return -960;

    return (int)((fp_decibels(ratio_q16, 16) * 10) / 65536 / 2);
}

void beat_probe_result(struct track_sound *out)
{
    uint32_t mean_sq;
    int g;

    memset(out, 0, sizeof (*out));

    chroma_get(&out->chroma);

    out->samplerate  = samplerate;
    out->analysed_ms = last_ms > first_ms ? last_ms - first_ms : 0;
    out->windows     = windows_seen;
    out->settled     = settled;

    out->period_ms    = period_ms;
    out->confidence   = confidence;
    out->tempo_spread = period_hi > period_lo ? period_hi - period_lo : 0;

    if (frames_in == 0)
        return;

    mean_sq = (uint32_t)(sum_mid_sq / frames_in);
    out->loudness_db10 = bp_db10_power(mean_sq);

    /* Crest as the gap between the two, so neither needs a square root and
     * the pair stay on one scale. */
    out->crest_db10 = (unsigned int)
                      (bp_db10_amplitude(peak_mid) - out->loudness_db10);

    /* Width against the mid rather than against full scale: what matters is
     * how much of this track is in the sides, not how loud it is. */
    if (mean_sq > 0)
    {
        uint64_t side = sum_side_sq / frames_in;
        uint64_t w = (side * 255) / mean_sq;

        out->width = (unsigned int)(w > 255 ? 255 : w);
    }

    if (level_n > 0)
    {
        uint32_t mean = sum_mid_level / level_n;
        uint32_t mean_of_sq = (uint32_t)(sum_mid_level_sq / level_n);

        for (g = 0; g < BEAT_GROUPS; g++)
            out->level[g] = (unsigned char)(sum_level[g] / level_n);

        out->peakiness = (unsigned char)(sum_peak / level_n);

        if (mean_of_sq > mean * mean)
            out->level_spread = (unsigned char)
                                bp_root(mean_of_sq - mean * mean);
    }

    if (strength_n > 0)
        out->strength = (unsigned char)(sum_strength / strength_n);

    /* Counted over the whole window rather than read from the hop machine,
     * whose own rate is measured across the last few onsets and so describes
     * the moment the decode stopped. */
    if (out->analysed_ms > 0)
    {
        for (g = 0; g < BEAT_GROUPS; g++)
            out->rate10[g] = (unsigned int)
                             ((onset_count[g] * 10000UL) / out->analysed_ms);
    }
}
