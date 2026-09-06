/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Finds the pulse: the period and phase of the beat the music is keeping.
 *
 * Onsets are evidence for a beat, not the beat itself. A bassline puts an
 * onset on every eighth note and a fill puts six in a bar, so a game built
 * straight on the onset stream asks for input at times a listener does not
 * feel as beats. What a rhythm game needs is the underlying pulse, which
 * carries on through a bar that happens to be silent.
 *
 * The method is autocorrelation of the onset envelope, which is the cheapest
 * thing that works and is entirely deterministic: no training, no state that
 * outlives a track, and the same audio always yields the same tempo.
 *
 * Parts, in order:
 *   - the envelope: onset strength, decimated and level-normalised
 *   - the tempo search, and the prior that resolves double and half time
 *   - the phase search, and confidence
 *   - projecting beats forward
 ****************************************************************************/

#include <string.h>
#include "config.h"
#include "audio/beat_track.h"

#ifdef BEAT_TRACE
void beat_trace(uint8_t (*at)(unsigned int), unsigned int n,
                    const int32_t *r, unsigned int lo, unsigned int hi,
                    unsigned int best, int32_t var, int32_t mean);
#endif

/* Hops per envelope bin. Four hops is 1024 samples, 23.2ms at 44.1kHz --
 * fine enough to place a beat well inside human tolerance, coarse enough
 * that the tempo search is a rounding error of CPU. */
#define BEAT_DECIM      2

/* Bins retained: about 16 seconds.
 *
 * Length here buys steadiness, which is what a listener actually notices --
 * a readout that holds one defensible answer reads as working, one that hops
 * between the beat and its half reads as broken even when both are real.
 * Measured across 36 tracks, going from 512 bins to 1536 more than halved the
 * number of times the tempo changed within a track (28 to 11 on ordinary pop)
 * while leaving accuracy flat. The cost is following a genuine tempo change
 * more slowly, which almost no track in a library asks for. */
#define BEAT_ENV_LEN    1536

/* Tempo range, in bins. A bin is 10.7ms at 48kHz, so this spans roughly 216
 * down to 44 BPM. The slow end has to reach genuinely slow music: at 86 bins
 * it stopped at 65 BPM, and every track below that was forced to answer at
 * double whatever it heard. */
#define BEAT_LAG_MIN    26
#define BEAT_LAG_MAX    128

/* Re-estimate roughly once a second, a slice of the lag sweep at a time.
 *
 * The sweep is about 150,000 multiply-accumulates. Run in one go it is a
 * frame that misses its deadline once a second, which in an animating
 * caller is a visible stutter -- and the estimate has a whole second to
 * arrive in, so there is no reason for any one frame to carry it. Sliced at
 * these rates the sweep spans 13 calls and 78 bins, near enough the same
 * second, and no call costs more than a fraction of a frame.
 *
 * Counted in bins rather than in frames so the schedule follows the audio,
 * not the frame rate: the same track must analyse the same way whatever the
 * screen is managing to draw. And a sweep that finishes its lags early waits
 * out the rest of BEAT_UPDATE_BINS rather than starting the next one -- the
 * re-estimate rate sets how fast the lock may change its mind and is tuned,
 * so spreading the work must not quietly raise it. */
#define BEAT_UPDATE_BINS 86
#define BEAT_SWEEP_LAGS  8
#define BEAT_SWEEP_BINS  6


/* How close to the peak a half-lag must score to be preferred over it, in
 * sixteenths. Too low and a genuinely slow track is doubled; too high and
 * the backbeat wins.
 *
 * Twelve, and the number is the rig's rather than an intuition. At sixteen
 * the test reads "only if the half is at least as strong as the peak", which
 * a real faster level rarely is -- it is spread over twice as many events --
 * so eight of ~/bttest's sixty-three cases lock an octave slow. Twelve
 * recovers five of them and no other verdict moves. The curve is flat from
 * eleven down and the six-eight cases start to degrade there, so this is the
 * far end of the safe part of it rather than the peak of the score.
 *
 * The step-down is judged on r[], which carries the tempo prior. Judging it
 * on the raw correlation instead scores identically, case for case: the
 * weighting is not what decides this. */
#define BEAT_OCTAVE_FRAC 12

/* Below this the tempo is not believed and nothing is projected.
 *
 * Measured on the rig: material with every event displaced at random scores
 * 19, the densest rhythmic track scores 32, ordinary ones 48-87. The bar
 * sits in that gap. Confidence is the fraction of the envelope variance the
 * beat lag accounts for, so it is comparable across tracks. */
#define BEAT_LOCK_CONF  12

/* How far apart two successive period estimates may be and still count as
 * the same tempo, and how many must agree before the grid is trusted. Two
 * runs is about two seconds of consistency. */
#define BEAT_AGREE_PCT  6
#define BEAT_AGREE_RUNS 2

/* Successive estimates that must *disagree* before a lock is given up.
 *
 * Acquiring and holding are not the same decision. A tempo estimate wobbles
 * over a fill, a breakdown or a bar of held chords, and dropping the lock on
 * the first disagreement makes the readout flicker on and off through a
 * track that has a perfectly steady pulse. Hard to acquire, slow to let go:
 * the grid stays where it was until the music has genuinely moved. */
#define BEAT_HOLD_RUNS  3

/* Tempo inertia: how far from the held tempo a lag still counts as "the
 * same", and how much its correlation is favoured while locked.
 *
 * Accuracy was never the complaint -- changing its mind was. A track whose
 * estimate hops between the beat, its half and a three-against-two reading
 * reads as broken even when every one of those answers is a real
 * periodicity, while a track that holds one answer reads as working. So a
 * held tempo gets a thumb on the scale and only a clearly better candidate
 * displaces it. */
#define BEAT_INERTIA_PCT   14
#define BEAT_INERTIA_BOOST 1   /* r += r >> this, so about 6% */

#define BEAT_DIFF(a, b) ((a) > (b) ? (a) - (b) : (b) - (a))

/* Recent beat-carrying onsets kept for the phase snap, and the fewest of
 * them that will be trusted to move the grid. */
#define BEAT_ONSETS     32
#define BEAT_SNAP_MIN   4

/* How far either side of the anchor an onset may vote on the phase, in
 * beats. Counted in beats and not milliseconds: a fixed window holds five
 * beats at 75 BPM and sixteen at 200, so the slow end silently fell below
 * BEAT_SNAP_MIN and never snapped at all. Eight beats also bounds how much
 * of the period error leaks into the phase -- about 1ms per beat. */
#define BEAT_SNAP_BEATS   8

/* Envelope, scaled to 0-255 against a decaying peak so the tempo search is
 * independent of how loud the track is. */
static uint8_t      env[BEAT_ENV_LEN];
static int          env_head;
static unsigned int env_count;

static int32_t      decim_acc;
static int          decim_n;
static int32_t      env_peak;

static unsigned long newest_bin_ms;
static unsigned int  samplerate;
static unsigned int  bins_since_update;

/* The correlation curve, and where the sweep across it has got to.
 *
 * Static, not on the stack: with the lag range widened this is about a
 * kilobyte, which is a lot to ask of a UI thread that also runs the skin
 * engine. It also has to survive between slices of a sweep. */
static int32_t      r[BEAT_LAG_MAX + 2];
static unsigned int sweep_lag;      /* Next lag to correlate; 0 starts one */
static int32_t      sweep_mean;
static int32_t      sweep_var;
static int32_t      sweep_best;
static unsigned int sweep_best_lag;
static unsigned int bins_since_sweep;

/* The envelope as one sweep sees it: copied when the sweep starts, newest
 * first. A sweep runs for about a second and the envelope moves under it, so
 * without the copy the lags at the two ends of the curve would describe
 * different music while being compared against each other -- which is the
 * one thing the curve is for.
 *
 * Newest first also means the correlation indexes it directly, so the inner
 * loop loses the wraparound branch env_at() needs. */
static uint8_t      senv[BEAT_ENV_LEN];
static unsigned int senv_count;

static unsigned long onsets[BEAT_ONSETS];
static unsigned int  onsets_seen;

static bool          locked;
static unsigned int  period_ms;
static unsigned int  confidence;
static unsigned long anchor_ms;   /* A beat, from which the rest are counted */
static unsigned int  agree_ms;    /* Period the last estimate settled on */
static unsigned int  agree_run;   /* Consecutive estimates that matched it */
static unsigned int  hold_run;    /* Consecutive ones that did not */


/** The envelope **/

/* Integer square root, never called more than once per bin. */
static uint32_t isqrt(uint32_t v)
{
    uint32_t rem = 0, root = 0;
    int i;

    for (i = 0; i < 16; i++)
    {
        root <<= 1;
        rem = (rem << 2) | (v >> 30);
        v <<= 2;

        if (root < rem)
            rem -= ++root;
    }

    return root >> 1;
}

static uint8_t env_at(unsigned int age)
{
    int i = env_head - (int)age;

    while (i < 0)
        i += BEAT_ENV_LEN;

    return env[i];
}

/* Milliseconds spanned by 'bins' envelope bins. Kept in 32 bits: bins is at
 * most BEAT_ENV_LEN, so the numerator peaks around 262 million. */
static unsigned int bins_to_ms(unsigned int bins)
{
    if (samplerate == 0)
        return 0;

    return (bins * BEAT_DECIM * 256u * 1000u) / samplerate;
}

/* Half the audio a bin spans, which is how far its last hop's timestamp
 * sits from the middle of it. */
static unsigned int bin_centre_offset_ms(void)
{
    if (samplerate == 0)
        return 0;

    return ((BEAT_DECIM - 1) * 256u * 1000u) / (2u * samplerate);
}

void beat_track_reset(void)
{
    memset(env, 0, sizeof (env));
    memset(onsets, 0, sizeof (onsets));
    onsets_seen = 0;
    env_head = 0;
    env_count = 0;
    decim_acc = 0;
    decim_n = 0;
    env_peak = 1;
    newest_bin_ms = 0;
    bins_since_update = 0;

    /* Abandon a half-finished sweep: its remaining lags would be compared
     * against a snapshot of the previous track. */
    sweep_lag = 0;
    senv_count = 0;
    bins_since_sweep = BEAT_UPDATE_BINS;

    locked = false;
    period_ms = 0;
    confidence = 0;
    anchor_ms = 0;
    agree_ms = 0;
    agree_run = 0;
    hold_run = 0;
}


/** Tempo **/

/* Mean level of the retained envelope. */
static int32_t env_mean(void)
{
    int32_t sum = 0;
    unsigned int i;

    if (env_count == 0)
        return 0;

    for (i = 0; i < env_count; i++)
        sum += env_at(i);

    return sum / (int32_t)env_count;
}

/* Covariance at one lag -- autocorrelation of the envelope with its mean
 * removed -- averaged over the overlap so lags are comparable.
 *
 * The mean subtraction is not a refinement, it is the difference between
 * working and not. Correlating the envelope directly makes every term a
 * product of two positive numbers, so the result is dominated by the square
 * of the mean and is nearly the same at every lag. Sparse music hides this
 * because its envelope returns to zero between hits; loudness-compressed
 * music never does, and the correlation goes flat, confidence collapses and
 * nothing ever locks. Measured on a dense track: confidence 20, no lock.
 *
 * Terms are (+-255)^2 at worst and there are under 500 of them, so the sum
 * stays inside 32 bits -- which is why the envelope is scaled to a byte. */
/* Read from the sweep's snapshot rather than the live ring, so it needs no
 * wraparound and every lag of one sweep sees the same audio. */
static int32_t sweep_autocorr(unsigned int lag, int32_t mean)
{
    unsigned int n, i;
    int32_t sum = 0;

    if (senv_count <= lag)
        return 0;

    n = senv_count - lag;

    for (i = 0; i < n; i++)
        sum += ((int32_t)senv[i] - mean) * ((int32_t)senv[i + lag] - mean);

    return sum / (int32_t)n;
}

/* Autocorrelation peaks at every multiple of the true period, so a 500ms
 * beat scores as well at 1000ms and at 250ms. Weighting toward a comfortable
 * walking pace settles it the way a listener does.
 *
 * Expressed in milliseconds, not in bins: a bin is 11.6ms at 44.1kHz and
 * 10.7ms at 48kHz, so a prior written in bins silently centres on a
 * different tempo depending on the file. The falloff is gentle -- the
 * octave rule below does the deciding, and a prior sharp enough to decide
 * on its own would drag genuinely fast and slow tracks toward the middle. */
static int32_t tempo_prior(unsigned int lag)
{
    unsigned int ms = bins_to_ms(lag);
    unsigned int d;

    if (ms == 0)
        return 128;

    /* ~545ms, or 110 BPM. */
    d = ms > 545 ? ms - 545 : 545 - ms;
    d = (d * 256) / 545;          /* Fraction of the centre, in 256ths */

    if (d > 96)
        d = 96;

    return 256 - (int32_t)d;
}

/* Sub-bin refinement by parabolic interpolation through the peak, in
 * sixteenths of a bin. Without it the period is quantised to 23ms, and a
 * beat projected 1.8s ahead lands up to 80ms out. */
static int refine_lag16(unsigned int lag, const int32_t *r)
{
    int32_t a = r[lag - 1], b = r[lag], c = r[lag + 1];
    int32_t denom = 2 * (a - 2 * b + c);
    int32_t num = (a - c) * 16;

    if (lag <= BEAT_LAG_MIN || lag >= BEAT_LAG_MAX || denom == 0)
        return (int)lag * 16;

    /* A true peak has a negative denominator; anything else is not a peak
     * and is left where it is. */
    if (denom >= 0)
        return (int)lag * 16;

    num /= denom;
    if (num > 8)
        num = 8;
    if (num < -8)
        num = -8;

    return (int)lag * 16 + (int)num;
}

/* Which offset within the period the beats actually fall on: the one whose
 * bins carry the most onset strength.
 *
 * Returned in sixteenths of a bin. A bin is 23ms, and a beat grid a whole
 * bin out is audibly late, so the peak is interpolated the same way the
 * period is rather than being left on the lattice. */
static int best_phase16(unsigned int lag)
{
    static int32_t ps[BEAT_LAG_MAX];
    int32_t bestv = -1;
    int32_t a, b, c, denom, num;
    unsigned int p, best = 0;

    for (p = 0; p < lag; p++)
    {
        int32_t sum = 0;
        unsigned int age;

        for (age = p; age < env_count; age += lag)
            sum += env_at(age);

        ps[p] = sum;

        if (sum > bestv)
        {
            bestv = sum;
            best = p;
        }
    }

    /* Phase is circular, so the neighbours of phase 0 wrap rather than
     * running off the end. */
    a = ps[(best + lag - 1) % lag];
    b = ps[best];
    c = ps[(best + 1) % lag];

    denom = 2 * (a - 2 * b + c);
    if (denom >= 0)
        return (int)best * 16;

    num = ((a - c) * 16) / denom;
    if (num > 8)
        num = 8;
    if (num < -8)
        num = -8;

    return (int)best * 16 + (int)num;
}

/* Nudge the grid onto the onsets it is supposed to be describing.
 *
 * The envelope is decimated to 23ms bins, so the phase it yields is right to
 * about a bin and reads as consistently late; the onset times feeding it are
 * good to a few milliseconds. Taking the period from the one and the phase
 * from the other uses each for what it is accurate at.
 *
 * Only onsets already near a beat vote. A run that is nowhere near the grid
 * means the tempo is wrong, and dragging the phase toward it would hide that
 * rather than fix it. */
static void snap_to_onsets(void)
{
    long half = (long)period_ms / 2;
    long span = (long)period_ms * BEAT_SNAP_BEATS;
    long total = 0;
    unsigned int voted = 0;
    unsigned int i, have;

    have = onsets_seen < BEAT_ONSETS ? onsets_seen : BEAT_ONSETS;

    for (i = 0; i < have; i++)
    {
        long delta = (long)onsets[i] - (long)anchor_ms;
        long d;

        /* The anchor is the *most recent* beat, so nearly every stored onset
         * is older than it and the offset is negative. Signed throughout, or
         * the vote is cast by the two or three onsets that happen to follow
         * the anchor and the snap does nothing.
         *
         * Only onsets near the anchor vote: the period is good to about a
         * millisecond, which is nothing over one beat and 20ms over twenty,
         * so a distant onset measures the period error rather than the
         * phase. */
        if (delta > span || delta < -span)
            continue;

        d = delta % (long)period_ms;
        if (d > half)
            d -= (long)period_ms;
        if (d < -half)
            d += (long)period_ms;

        /* Within a quarter beat counts as "on this beat". */
        if (d > half / 2 || d < -half / 2)
            continue;

        total += d;
        voted++;
    }

    if (voted < BEAT_SNAP_MIN)
        return;

    anchor_ms = (unsigned long)((long)anchor_ms + total / (long)voted);
}

/* What the finished sweep decided: the metrical level, the confidence, the
 * period and phase, and whether that is worth locking to. */
static void beat_decide(void)
{
    int32_t best = sweep_best, mean = sweep_mean, var = sweep_var;
    unsigned int best_lag = sweep_best_lag;
    int lag16;

    if (best_lag == 0 || best <= 0 || var <= 0)
    {
        locked = false;
        confidence = 0;
        return;
    }

    /* Step down by two thirds once, then by half for as long as the half
     * scores nearly as well.
     *
     * Autocorrelation peaks at every multiple of the true period, so a
     * backbeat snare gives a genuinely strong correlation at two beats and a
     * track can lock an octave low. What tells "one pulse counted twice"
     * from a real slower tempo is that the faster level is still almost as
     * strong.
     *
     * The two-thirds step is the same argument for the other way a period is
     * overcounted: a triplet subdivision and a three-against-two accent both
     * put a strong peak at one and a half beats, and a listener taps the beat
     * underneath it. It runs *before* the halving so that what the halving
     * sees is the level this chose, and only once -- nothing in music stacks
     * two of them.
     *
     * Trap: the low band is not a better arbiter than either of these,
     * however much a kick drum sounds like one. Onsets there on an ordinary
     * mix are not a clean enough pattern to say which level the bar sits at.
     *
     * What is left on the rig is six-eight, and it is left on purpose: it is
     * tapped at one and a half beats, so the rule that recovers
     * three-against-two is pointed the wrong way for it, and the two cannot
     * be told apart from the envelope. Ten of sixty-three, against seventeen
     * before either step was tuned. */
    {
        unsigned int third = (best_lag * 2) / 3;
        unsigned int half;

        if (third >= BEAT_LAG_MIN && r[third] * 16 >= best * BEAT_OCTAVE_FRAC)
            best_lag = third;

        half = best_lag / 2;

        while (half >= BEAT_LAG_MIN &&
               r[half] * 16 >= best * BEAT_OCTAVE_FRAC)
        {
            best_lag = half;
            half = best_lag / 2;
        }
    }

    /* Correlation at the chosen lag as a fraction of the variance, which is
     * the correlation at lag zero: 100 would mean the envelope repeats
     * exactly, and music with no pulse scores near nothing. Taken from the
     * unweighted covariance so the tempo prior cannot inflate it. */
    {
        int32_t raw = sweep_autocorr(best_lag, mean);

        confidence = raw > 0 ? (unsigned int)((raw * 100) / var) : 0;
        if (confidence > 100)
            confidence = 100;
    }

#ifdef BEAT_TRACE
    beat_trace(env_at, env_count, r, BEAT_LAG_MIN, BEAT_LAG_MAX,
                   best_lag, var, mean);
#endif

    lag16 = refine_lag16(best_lag, r);
    period_ms = (unsigned int)((bins_to_ms(16)
                 * (unsigned int)lag16) / 256);

    /* Anchor on the most recent beat, so the projection in
     * beat_track_next() never has to extrapolate across the whole
     * envelope and the sub-bin period error cannot accumulate. */
    {
        int phase16 = best_phase16(best_lag);
        unsigned int back_ms =
            (unsigned int)((bins_to_ms(16) * (unsigned int)phase16) / 256);

        anchor_ms = newest_bin_ms > back_ms ? newest_bin_ms - back_ms : 0;
    }

    /* Lock on agreement, not on a single strong correlation.
     *
     * Real music rarely correlates as cleanly as a synthetic pattern: the
     * right tempo often explains only 15-30% of the envelope variance, and a
     * threshold set high enough to exclude noise on one track rejects the
     * correct answer on another. What separates a real pulse from a chance
     * peak is that it comes back -- a wrong answer wanders between updates,
     * a right one repeats. So a modest correlation is enough provided
     * successive estimates land on the same tempo.
     */
    if (period_ms > 0 && agree_ms > 0 &&
        BEAT_DIFF(period_ms, agree_ms) * 100 <= agree_ms * BEAT_AGREE_PCT)
    {
        if (agree_run < 255)
            agree_run++;
        hold_run = 0;
    }
    else
    {
        agree_run = 0;
        if (hold_run < 255)
            hold_run++;
    }

    agree_ms = period_ms;

    if (period_ms > 0 && confidence >= BEAT_LOCK_CONF &&
        agree_run >= BEAT_AGREE_RUNS)
    {
        locked = true;
    }
    else if (locked && hold_run >= BEAT_HOLD_RUNS)
    {
        locked = false;
    }

    if (locked)
        snap_to_onsets();
}

/* One slice of the lag sweep, and the decision once it reaches the end.
 *
 * The mean and the variance are taken once, at the start of a sweep, and
 * held for all of it: the lags are only meaningful against each other, and
 * the envelope moves on underneath them while the sweep runs. */
static void beat_update(void)
{
    unsigned int end;

    if (env_count < BEAT_LAG_MAX * 3)
        return;   /* Not enough history to see three cycles of a slow tempo */

    if (sweep_lag == 0)
    {
        unsigned int i;

        if (bins_since_sweep < BEAT_UPDATE_BINS)
            return;

        bins_since_sweep = 0;

        for (i = 0; i < env_count; i++)
            senv[i] = env_at(i);

        senv_count = env_count;
        sweep_mean = env_mean();
        sweep_var = sweep_autocorr(0, sweep_mean);
        sweep_best = 0;
        sweep_best_lag = 0;
        sweep_lag = BEAT_LAG_MIN;
    }

    end = sweep_lag + BEAT_SWEEP_LAGS;
    if (end > BEAT_LAG_MAX + 1)
        end = BEAT_LAG_MAX + 1;

    for (; sweep_lag < end; sweep_lag++)
    {
        r[sweep_lag] = (sweep_autocorr(sweep_lag, sweep_mean)
                        * tempo_prior(sweep_lag)) >> 8;

        /* Favour the tempo already being held, so the grid does not hop
         * between metrical levels that all correlate about as well. */
        if (locked && period_ms > 0 && r[sweep_lag] > 0)
        {
            unsigned int ms = bins_to_ms(sweep_lag);
            unsigned int d = ms > period_ms ? ms - period_ms : period_ms - ms;

            if (d * 100 <= period_ms * BEAT_INERTIA_PCT)
                r[sweep_lag] += r[sweep_lag] >> BEAT_INERTIA_BOOST;
        }

        if (sweep_best_lag == 0 || r[sweep_lag] > sweep_best)
        {
            sweep_best = r[sweep_lag];
            sweep_best_lag = sweep_lag;
        }
    }

    if (sweep_lag <= BEAT_LAG_MAX)
        return;

    sweep_lag = 0;
    beat_decide();
}


/** Input **/

void beat_track_onset(unsigned long ms)
{
    onsets[onsets_seen % BEAT_ONSETS] = ms;
    onsets_seen++;
}

void beat_track_push(int flux_sum, unsigned long ms, unsigned int sampr)
{
    int32_t scaled;

    samplerate = sampr;
    decim_acc += flux_sum;

    if (++decim_n < BEAT_DECIM)
        return;

    /* The peak only keeps values inside a byte. It must decay far more
     * slowly than a beat, or it becomes an automatic gain control whose
     * release time is the very period being measured: a loud beat raises the
     * divisor and the next one is scaled down, which removes the beat
     * modulation from the envelope. A shift of 6 gave a 0.74s time constant
     * against a 0.6s beat and flattened the correlation on any material
     * dense enough not to fall silent between hits. This is ~24s.
     *
     * Nothing downstream cares about the scale -- the correlation subtracts
     * the mean and divides by the variance -- so slow is free. */
    env_peak -= env_peak >> 11;
    if (decim_acc > env_peak)
        env_peak = decim_acc;
    if (env_peak < 1)
        env_peak = 1;

    /* Square-root compression. Onset strength is very peaky: one snare can
     * be ten times a kick, which on a linear scale leaves everything else
     * inside the bottom tenth of the byte and quantised almost flat. */
    {
        /* Trap: isqrt() returns zero for anything under 4, so the peak has
         * to be tested rather than trusted -- env_peak is clamped to 1 on a
         * reset and through silence, and dividing by its root then takes the
         * player to a panic screen. Below that resolution there is no ratio
         * worth having, and no onset is the honest answer. */
        uint32_t peak_root = isqrt((uint32_t)env_peak);

        scaled = peak_root
                 ? (int32_t)((isqrt((uint32_t)decim_acc) * 255) / peak_root)
                 : 0;
    }

    if (scaled > 255)
        scaled = 255;
    if (scaled < 0)
        scaled = 0;

    env_head = (env_head + 1) % BEAT_ENV_LEN;
    env[env_head] = (uint8_t)scaled;
    if (env_count < BEAT_ENV_LEN)
        env_count++;

    /* Label the bin with the middle of the audio it covers, not the end of
     * it. A bin sums BEAT_DECIM hops and is pushed on the last of them, so
     * stamping it with that hop's time places every bin half a group late
     * and drags the whole beat grid after the music. */
    newest_bin_ms = ms > bin_centre_offset_ms() ? ms - bin_centre_offset_ms()
                                                : 0;
    decim_acc = 0;
    decim_n = 0;

    bins_since_sweep++;

    if (++bins_since_update >= BEAT_SWEEP_BINS)
    {
        bins_since_update = 0;
        beat_update();
    }
}


/** Output **/

unsigned int beat_track_fill(void)
{
    return (unsigned int)((env_count * 100) / BEAT_ENV_LEN);
}

void beat_track_get(struct beat_track *beat)
{
    beat->locked     = locked;
    beat->period_ms  = period_ms;
    beat->bpm        = period_ms ? 60000u / period_ms : 0;
    beat->confidence = confidence;
    beat->last_ms    = anchor_ms;
    beat->count      = 0;

    if (locked && period_ms > 0 && newest_bin_ms > anchor_ms)
        beat->count = (unsigned int)((newest_bin_ms - anchor_ms) / period_ms);
}

bool beat_track_next(unsigned long from_ms, int n,
                        unsigned long *beat_ms, int *in_bar)
{
    long delta, index;

    if (!locked || period_ms == 0)
        return false;

    /* Signed throughout. The anchor is the newest beat the analyser has
     * seen, which runs a whole look-ahead ahead of what is being heard, so
     * every question the screen asks is about times *before* it. Refusing
     * those left the beat grid and the bar count permanently blank while the
     * tempo readout worked, which reads as the beat tracker being broken
     * when it is only the projection. */
    delta = (long)from_ms - (long)anchor_ms;
    index = delta / (long)period_ms;

    /* Round toward the first beat at or after from_ms; C truncates toward
     * zero, so negative deltas need the other nudge. */
    if (index * (long)period_ms < delta)
        index++;

    index += n;

    *beat_ms = (unsigned long)((long)anchor_ms + index * (long)period_ms);

    /* The bar position is counted from the anchor, so "1" is wherever the
     * lock happened to land rather than the music's true downbeat. Finding
     * that needs meter detection this does not attempt; what the count is
     * good for is telling whether the beats are evenly placed and on time. */
    if (in_bar != NULL)
        *in_bar = (int)(((index % 4) + 4) % 4);

    return true;
}
