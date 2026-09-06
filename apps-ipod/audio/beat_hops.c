/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Turns PCM into onsets: the hop machine behind every beat feature.
 *
 * Samples arrive a block at a time and are analysed in fixed hops placed on
 * the track's own timeline. Each hop yields three band levels, a spectral
 * flux per band, an onset decision, and a broadband envelope sample for the
 * tempo tracker. Nothing here knows where the samples came from -- a live
 * caller reads them out of the PCM buffer ahead of playback (beat_analyse.c),
 * an offline one decodes them itself.
 *
 * Parts, in order:
 *   - tuning constants and module state
 *   - position: the timeline hops are placed on, and starting over
 *   - analysis: band magnitudes, spectral flux, onset decision, and the
 *     broadband envelope the tempo is tracked from
 *   - feeding samples in
 *   - read-out
 ****************************************************************************/

#include <string.h>
#include "config.h"
#include "audio/spectrum_meter.h"
#include "fixedpoint.h"
#include "audio/beat_hops.h"
#include "audio/beat_track.h"

/* How often the analysis runs, in samples: 172 times a second at 44.1kHz. */
#define BEAT_HOP              256

/* Samples retained, and so the longest window a group may ask for.
 *
 * A window shorter than one cycle of the frequency it is tuned to does not
 * measure that frequency at all -- it tracks the envelope of the fragment
 * instead, and fires on anything loud. One cycle of 60Hz is 16.7ms, or 735
 * samples, so the bass bands need 1024 (23.2ms, 43Hz resolution) where the
 * bands above 400Hz are comfortable inside a single hop. */
#define BEAT_RETAIN           1024

/* Per group: which bands it covers (end exclusive), how many of the retained
 * samples its Goertzels read, the threshold multiplier in eighths, an
 * absolute floor to keep silence quiet, and how long it is deaf after firing.
 *
 * Mid carries vocals, guitars, piano and snare all at once, so its flux is
 * busy in a way bass and treble are not, so it is thresholded harder and
 * held deaf longer to keep its rate comparable with the other two.
 *
 * Tuning constants -- these are the numbers to move if a group fires too
 * often or not at all. */
static const struct
{
    int first, end;
    int window;
    int k;
    int floor;
    int rel;
    int refractory_ms;
} group_tune[BEAT_GROUPS] =
{
    /*                bands  window            k  floor  rel  deaf */
    [BEAT_LOW]  = { 0, 2, BEAT_RETAIN, 13,  800,  10, 130 },
    [BEAT_MID]  = { 2, 5, BEAT_HOP,    24, 2000,  10, 250 },
    [BEAT_HIGH] = { 5, 8, BEAT_HOP,    13,  800,   4, 130 },
};

/* The running mean the threshold is measured against is exponential with
 * this shift: ~128 hops, or 0.74s at 44.1kHz. Tracking the mix rather than
 * an absolute level is what makes one setting work across a quiet acoustic
 * track and a loudness-compressed one, and what makes volume and ReplayGain
 * changes cancel. */
#define BEAT_MEAN_SHIFT       7

/* A second threshold, measured against the loudest flux lately rather than
 * the average of it, and the one that separates a kick from a bass note.
 *
 * The mean cannot do that job. Between events the flux is near zero, so the
 * mean sits far below every real onset and a multiplier on it passes the
 * quiet ones however large the multiplier is made. What marks a kick out is
 * being several times stronger than the onsets around it, and that is a
 * comparison against the recent peak.
 *
 * The peak rises instantly and decays by this shift each hop -- about 1.5s
 * to 1/e at 44.1kHz -- so a passage that goes quiet lowers the bar with it
 * rather than going deaf. group_tune[].rel is the fraction of the peak a
 * candidate must reach, in sixteenths. */
#define BEAT_PEAK_SHIFT       8

/* Onsets kept per group to estimate a rate. Enough to be steady, few enough
 * to follow a change of section. */
#define BEAT_RATE_DEPTH       8

static unsigned int  samplerate;
static unsigned long base_ms;       /* Track time of the last stamp */
static unsigned long base_frames;   /* Frames analysed since that stamp */
static bool          base_stamped;  /* A caller has said where in the track
                                       the next frame sits, so base_ms means
                                       something */
static bool          align_pending; /* The hop grid has yet to be put back on
                                       the track's own timeline */

/* The retained samples, oldest first, so a group's window is always the last
 * group_tune[].window of them. */
static int16_t       retain[BEAT_RETAIN];
static int           retain_fill;
static unsigned long hop_ms;        /* Track time of the newest hop */
static unsigned long hop_ms_1;      /* ...of the one before it, which is the
                                       hop the onset test judges */

static int           mag_prev[SPECTRUM_MAX_BANDS];
static bool          have_mag_prev;

static int           flux_1[BEAT_GROUPS];  /* One hop back */
static int           flux_2[BEAT_GROUPS];  /* Two hops back */
static int32_t       flux_mean[BEAT_GROUPS];
static int32_t       flux_peak[BEAT_GROUPS];
static unsigned long onset_ms[BEAT_GROUPS];

static unsigned long onset_at[BEAT_GROUPS][BEAT_RATE_DEPTH];
static unsigned int  onset_seen[BEAT_GROUPS];

static struct beat_window history[BEAT_HISTORY];
static int           history_head;  /* Newest entry */
static unsigned int  history_count;

static unsigned int  stat_windows;

/* Beat-envelope filterbank state: five one-pole lowpasses, the previous
 * hop's band energies and the previous log levels. Cleared on a
 * discontinuity like everything else -- a filter carrying the last track's
 * signal reports a huge spurious onset on the first hop of the next. */
static int32_t       env_lp[5];
static int32_t       env_hop_ring[4][6];
static int           env_hop_idx;
static int32_t       env_band_prev[6];
static int           env_peakiness;     /* Of the hop just measured */


/** Position **/

/* Track time of the next frame to be analysed. */
static unsigned long beat_pos_ms(void)
{
    if (samplerate == 0)
        return base_ms;

    return base_ms + (unsigned long)
                     (((uint64_t)base_frames * 1000) / samplerate);
}

void beat_hops_resync(void)
{
    retain_fill = 0;
    have_mag_prev = false;
    memset(flux_1, 0, sizeof (flux_1));
    memset(flux_2, 0, sizeof (flux_2));
    memset(env_lp, 0, sizeof (env_lp));
    memset(env_hop_ring, 0, sizeof (env_hop_ring));
    env_hop_idx = 0;
    memset(env_band_prev, 0, sizeof (env_band_prev));

    /* Timestamps restart from the new track's own zero, so a refractory
     * deadline recorded against the old one is not a time any more. */
    memset(onset_ms, 0, sizeof (onset_ms));

    /* Neither means anything until a caller stamps the position again, so
     * clearing them here costs nothing and leaves no stale time to read. */
    base_ms = 0;
    base_frames = 0;

    base_stamped = false;
    align_pending = true;
}

void beat_hops_set_pos(unsigned long ms)
{
    base_ms = ms;
    base_frames = 0;
    base_stamped = true;
}

void beat_hops_set_rate(unsigned int sampr)
{
    if (sampr == samplerate)
        return;

    samplerate = sampr;
    beat_hops_resync();
}

unsigned int beat_hops_rate(void)
{
    return samplerate;
}

void beat_hops_reset(unsigned int sampr)
{
    memset(history, 0, sizeof (history));
    memset(flux_mean, 0, sizeof (flux_mean));
    memset(flux_peak, 0, sizeof (flux_peak));
    memset(onset_ms, 0, sizeof (onset_ms));
    memset(onset_seen, 0, sizeof (onset_seen));
    history_head = 0;
    history_count = 0;
    stat_windows = 0;
    samplerate = sampr;

    beat_track_reset();
    beat_hops_resync();
}

/** Analysis **/

/* An onset has to clear both bars: loud against the average, and loud
 * against the loudest lately. */
static int beat_threshold(int group)
{
    int32_t mean = flux_mean[group];
    int from_mean = (int)((mean * group_tune[group].k) >> 3)
                    + group_tune[group].floor;
    int from_peak = (int)((flux_peak[group] * group_tune[group].rel) >> 4);

    return from_mean > from_peak ? from_mean : from_peak;
}

/* Onsets per second, in tenths. Measured over the span the last few onsets
 * cover, and decaying once they stop: the span is taken to the present, not
 * to the newest onset, so a group that has gone quiet reads as quiet rather
 * than holding its last rate. */
static unsigned int beat_rate10(int group, unsigned long now)
{
    unsigned int seen = onset_seen[group];
    unsigned long oldest, span;

    if (seen < BEAT_RATE_DEPTH)
        return 0;

    oldest = onset_at[group][seen % BEAT_RATE_DEPTH];
    if (now <= oldest)
        return 0;

    span = now - oldest;

    return (unsigned int)(((BEAT_RATE_DEPTH - 1) * 10000UL) / span);
}

/* Mark a group's onset on the newest history entry, which at the point this
 * runs is the hop being judged -- one behind the hop just analysed. */
static void beat_mark_onset(int group, unsigned long ms, int strength)
{
    if (history_count > 0)
    {
        history[history_head].onset |= 1 << group;

        /* The loudest group to fire on this hop wins the strength: one
         * number describes the hop, and the generator asks how big the
         * event was, not which band carried it. */
        if (strength > history[history_head].strength)
            history[history_head].strength = (uint8_t)strength;
    }

    onset_at[group][onset_seen[group] % BEAT_RATE_DEPTH] = ms;
    onset_seen[group]++;
}

static void beat_push_window(const int *level, unsigned long ms)
{
    struct beat_window *w;
    int g;

    history_head = (history_head + 1) % BEAT_HISTORY;
    w = &history[history_head];

    w->time_ms = (uint32_t)ms;
    for (g = 0; g < BEAT_GROUPS; g++)
        w->level[g] = (uint8_t)level[g];
    w->onset = 0;
    w->strength = 0;
    w->peakiness = (uint8_t)env_peakiness;

    if (history_count < BEAT_HISTORY)
        history_count++;
}

/* Onset strength for the beat tracker: how much louder each of six wide
 * bands got over the last hop, summed.
 *
 * Deliberately not the eight-band Goertzel flux above. Those are narrow
 * probes at eight exact frequencies, which describes real music far too
 * sparsely to difference -- a mix moves energy between them constantly, so
 * the difference reports that movement rather than anything arriving. On a
 * real track it produced an envelope that swung five-fold between adjacent
 * 10ms bins and correlated under 9% of its variance at every lag.
 *
 * Nor is it broadband energy, which fails the other way: a mastering limiter
 * holds the total almost constant, so on loudness-compressed music there is
 * barely an envelope left to find a beat in.
 *
 * Wide bands split the difference. A drum hit lands mostly in one or two of
 * them, so it survives a limiter holding the sum flat, and every sample
 * contributes rather than eight points of the spectrum. The split is five
 * one-pole lowpasses on the same input, each band the difference of two
 * neighbouring corners -- about twenty operations a sample, and no window,
 * no transform and no table. */
static int beat_envelope(void)
{
    #define BEAT_ABS(v) ((v) < 0 ? -(v) : (v))

    /* Corners at roughly fs/(2*pi*2^shift): ~3.5k, 1.7k, 880, 440, 220Hz. */
    static const int shift[5] = { 1, 2, 3, 4, 5 };

    const int16_t *hop = retain + BEAT_RETAIN - BEAT_HOP;
    int32_t e[6];
    int total = 0;
    int i, b;

    for (b = 0; b < 6; b++)
        e[b] = 0;

    for (i = 0; i < BEAT_HOP; i++)
    {
        /* Filter states carry 8 fractional bits. Without them
         * `lp += (x - lp) >> shift` stalls dead whenever the difference is
         * smaller than 1 << shift, so the widest band stops tracking any
         * signal under 32 counts and quiet passages quantise to silence. */
        int32_t x = (int32_t)hop[i] << 8;

        for (b = 0; b < 5; b++)
            env_lp[b] += (x - env_lp[b]) >> shift[b];

        e[0] += BEAT_ABS(env_lp[4]) >> 8;
        e[1] += BEAT_ABS(env_lp[3] - env_lp[4]) >> 8;
        e[2] += BEAT_ABS(env_lp[2] - env_lp[3]) >> 8;
        e[3] += BEAT_ABS(env_lp[1] - env_lp[2]) >> 8;
        e[4] += BEAT_ABS(env_lp[0] - env_lp[1]) >> 8;
        e[5] += BEAT_ABS(x - env_lp[0]) >> 8;
    }

    /* Two things here are worth more than any amount of tuning, both
     * measured against a reference envelope built with a real FFT:
     *
     * Energy is averaged over four hops, not one. A single 256-sample hop is
     * far too short to estimate a band's level: the estimate is noisy, and
     * the difference between two noisy estimates is mostly noise. Every
     * widening paid, and 1024 samples matches a full FFT of the same length
     * -- measured across five real tracks, 512 scored 5-36% of variance
     * against 9-45% at 1024, and the tempo was right at 1024 in every case.
     * The window is what matters; band count barely moves it.
     *
     * And the difference is taken on the logarithm, which makes it a
     * relative change. A mastering limiter holds absolute level nearly
     * constant, so on compressed music a linear difference has little left
     * to see; a proportional one still reads a hi-hat against a loud mix. */
    for (b = 0; b < 6; b++)
        env_hop_ring[env_hop_idx][b] = e[b];

    env_hop_idx = (env_hop_idx + 1) & 3;

    int32_t sum_v = 0;
    int32_t sum_lv = 0;

    for (b = 0; b < 6; b++)
    {
        int32_t v = (env_hop_ring[0][b] + env_hop_ring[1][b]
                     + env_hop_ring[2][b] + env_hop_ring[3][b]) >> 10;
        int32_t lv;

        if (v > 32767)
            v = 32767;      /* fp16_log takes Q16, so keep the shift safe */

        lv = fp16_log((v + 1) << 16) >> 8;

        if (lv > env_band_prev[b])
            total += (int)(lv - env_band_prev[b]);

        env_band_prev[b] = lv;

        sum_v += v;
        sum_lv += lv;
    }

    /* Peakiness, from the same logs: the gap between the arithmetic and the
     * geometric mean of the six bands, zero when they are level and growing
     * as one takes over. Natural logs in Q8, reported in hundredths of a nat
     * so the number means something rather than filling a byte. */
    {
        int32_t gap = (fp16_log(((sum_v / 6) + 1) << 16) >> 8) - sum_lv / 6;

        if (gap < 0)
            gap = 0;
        gap = (gap * 100) / 256;

        env_peakiness = gap > 255 ? 255 : (int)gap;
    }

    return total;
}

/* One hop: band magnitudes, then flux against the previous hop, then the
 * onset decision for the hop before that. */
static void beat_run_hop(void)
{
    int mag[SPECTRUM_MAX_BANDS];
    int level[BEAT_GROUPS];
    int flux[BEAT_GROUPS];
    int band, g;

    /* Every window ends at the newest sample and reaches back as far as its
     * group asks, so groups share the retained samples without any of them
     * being analysed at the wrong length. */
    for (g = 0; g < BEAT_GROUPS; g++)
    {
        const int16_t *src = retain + BEAT_RETAIN - group_tune[g].window;
        int peak = 0;

        flux[g] = 0;

        for (band = group_tune[g].first; band < group_tune[g].end; band++)
        {
            mag[band] = spectrum_goertzel_magnitude(
                            src, group_tune[g].window, 1,
                            spectrum_band_freq_hz[band], samplerate);

            if (mag[band] > peak)
                peak = mag[band];

            /* Positive differences only: an onset is energy arriving, and
             * the fall after it says nothing about the next one. A plain
             * energy delta instead of this detects nothing on loudness-
             * compressed material, where the level barely moves between
             * hits. */
            if (have_mag_prev && mag[band] > mag_prev[band])
                flux[g] += mag[band] - mag_prev[band];
        }

        level[g] = spectrum_scale_to_level(peak);
    }

    memcpy(mag_prev, mag, sizeof (mag_prev));
    have_mag_prev = true;

    /* Judge the hop one back, so a peak is only called once both its
     * shoulders are known. The cost is one hop of latency, ~6ms. */
    for (g = 0; g < BEAT_GROUPS; g++)
    {
        if (history_count >= 2 &&
            flux_1[g] > flux_2[g] && flux_1[g] >= flux[g] &&
            flux_1[g] > beat_threshold(g) &&
            hop_ms_1 - onset_ms[g] >= (unsigned long)
                                      group_tune[g].refractory_ms)
        {
            /* Scaled so that matching the peak of the onsets around it
             * reads 128 and section 4.1's accent -- half again above that
             * peak -- reads 192. Scaling so the peak itself read full would
             * saturate nearly every onset, since an onset only fires by
             * standing near the peak in the first place. */
            int peak = flux_peak[g] > 0 ? (int)flux_peak[g] : 1;
            int strength = (int)(((int64_t)flux_1[g] * 128) / peak);

            beat_mark_onset(g, hop_ms_1,
                                strength > 255 ? 255 : strength);
            onset_ms[g] = hop_ms_1;

            /* Low carries the pulse in nearly all music, so it is the
             * group whose timing the beat grid is snapped to. */
            if (g == BEAT_LOW)
                beat_track_onset(hop_ms_1);
        }

        /* Decay first, then admit the hop just judged, so a candidate is
         * always measured against the peak of what came before it and never
         * against itself. */
        flux_peak[g] -= flux_peak[g] >> BEAT_PEAK_SHIFT;
        if (flux_1[g] > flux_peak[g])
            flux_peak[g] = flux_1[g];

        flux_2[g] = flux_1[g];
        flux_1[g] = flux[g];
        flux_mean[g] += (flux[g] - flux_mean[g]) >> BEAT_MEAN_SHIFT;
    }

    /* The pulse is tracked from broadband energy, not from the band flux
     * above.
     *
     * Eight Goertzels describe real music far too sparsely to difference:
     * they measure whether there is energy at eight exact frequencies, and
     * a mix moves between them constantly, so the difference is dominated by
     * that movement rather than by anything arriving. Measured on a real
     * track, the envelope built that way swung five-fold between adjacent
     * 10ms bins and correlated at under 9% of its variance at every lag --
     * noise, and it never locked. Synthetic tracks hide it completely,
     * because they contain the very tones being probed.
     *
     * Energy over the whole hop uses every sample instead of eight points of
     * it, and a beat is an energy event whatever its pitch. The band flux
     * stays where it belongs, telling a kick from a hat. */
    beat_track_push(beat_envelope(), hop_ms, samplerate);

    hop_ms_1 = hop_ms;
    beat_push_window(level, hop_ms);
    stat_windows++;
}


/** Feeding **/

/* Fold a block of frames into the retained samples, analysing each time a
 * hop's worth of new ones has arrived. */
void beat_hops_feed(const int16_t *pcm, int frames)
{
    int i = 0;

    /* Hops fall on multiples of BEAT_HOP counted from the track's own zero,
     * not from wherever the caller happened to start feeding. A caller
     * places itself only to within a block, so without this the whole grid
     * shifts by up to 46ms between one run over a track and the next --
     * every window covers different samples, marginal onsets flip, and the
     * same track is quietly a different reading each time. Nothing looks
     * wrong when it is missing; two runs over one track just stop comparing
     * like with like.
     *
     * A stamp is in milliseconds and nothing finer is offered, so the frame
     * it recovers is within half a millisecond of the true one -- 22 samples
     * at 44.1kHz. That residual cannot be removed at this resolution, and it
     * is a ninetieth of the block it replaces. */
    if (align_pending)
    {
        unsigned long frame;
        unsigned int skip;

        if (!base_stamped)
            return;

        frame = (unsigned long)(((uint64_t)base_ms * samplerate + 500) / 1000)
                + base_frames;
        skip = (BEAT_HOP - (unsigned int)(frame % BEAT_HOP))
               % BEAT_HOP;

        if (skip >= (unsigned int)frames)
        {
            base_frames += frames;
            return;
        }

        base_frames += skip;
        i = (int)skip;
        align_pending = false;
    }

    for (; i < frames; i++)
    {
        if (retain_fill == BEAT_RETAIN - BEAT_HOP)
            hop_ms = beat_pos_ms();

        retain[retain_fill++] =
            (pcm[i * 2] + pcm[i * 2 + 1]) / 2;
        base_frames++;

        if (retain_fill == BEAT_RETAIN)
        {
            beat_run_hop();

            /* Slide the window on by one hop, keeping the tail the longer
             * windows still need. */
            memmove(retain, retain + BEAT_HOP,
                    (BEAT_RETAIN - BEAT_HOP) * sizeof (retain[0]));
            retain_fill = BEAT_RETAIN - BEAT_HOP;
        }
    }
}


/** Read-out **/

const struct beat_window *beat_hops_window(int back)
{
    int index;

    if (back < 0 || (unsigned int)back >= history_count)
        return NULL;

    index = history_head - back;
    if (index < 0)
        index += BEAT_HISTORY;

    return &history[index];
}

unsigned long beat_hops_newest_ms(void)
{
    return history_count > 0 ? history[history_head].time_ms : 0;
}

unsigned int beat_hops_windows(void)
{
    return stat_windows;
}

void beat_hops_bands(struct beat_bands *out)
{
    int g;

    for (g = 0; g < BEAT_GROUPS; g++)
    {
        out->flux[g]      = flux_1[g];
        out->threshold[g] = beat_threshold(g);
        out->onsets[g]    = onset_seen[g];
        out->rate10[g]    = beat_rate10(g, hop_ms);
    }
}
