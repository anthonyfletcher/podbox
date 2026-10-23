/***************************************************************************
 * GNU General Public License (version 2 only)
 * Port of NV Speech Player, Copyright (C) 2014 NV Access Limited
 * Based on klsyn-88 (linguistics.berkeley.edu/phonlab/resources/)
 *
 * The formant speech generator: turns a queue of parameter frames into PCM.
 * A Klatt synthesiser -- a glottal pulse and two noise sources, shaped by a
 * cascade of resonators for voiced sound and a parallel bank for frication.
 *
 * Fixed point throughout. Frame parameters are Q16 (synth.h), the signal
 * Q18 (SIG_ONE), resonator coefficients Q28, or Q20 for the antiresonator,
 * whose coefficients run to several hundred. While one frame fades into the
 * next the parameters, and so the coefficients, are recomputed every
 * CONTROL_PERIOD samples rather than every sample; pitch alone moves every
 * sample.
 *
 * Rendering is in blocks: a run of samples over which nothing but pitch
 * changes (frames_plain() says how long) has its sources made sample by
 * sample, then passes through each resonator in turn. A bank whose input is
 * silent and whose resonators have died away is skipped, and so is a
 * parallel resonator no phoneme uses, and the nasal pair while no nasal
 * sounds.
 *
 * Single-threaded: the queue and the generator are driven from one thread.
 *
 * Parts, in order:
 *   - sources: noise, voice
 *   - resonators: coefficients, block filters, the cascade and parallel banks
 *   - the frame queue, which fades each frame into the next
 *   - the public interface
 ****************************************************************************/

#include <stdlib.h>
#include <string.h>

#include "fixedpoint.h"
#include "system/fracmul.h"
#include "synth.h"
#include "synth_phonemes.h"

#define SYNTH_QUEUE_SIZE 64     /* frames; a power of two */
#define BLOCK_MAX 64            /* samples rendered as one block */

#ifndef CONTROL_PERIOD
#define CONTROL_PERIOD 16       /* samples; a power of two */
#endif

#define SIG_BITS 18
#define SIG_ONE (1 << SIG_BITS)
#define Q30_ONE (1ll << 30)
#define PI_Q30 3373259426ll

/* A resonator whose input has stopped is let go once its state is this
 * small, so that an idle bank reaches zero and can be skipped: 64 is about
 * 2 LSB at the output. Trap: at 2 the parallel bank never settles and runs
 * on 98% of samples, against 23% at 64. */
#define SETTLED 64

static int sample_rate;
static uint32_t phase_per_hz;   /* 2^32 / sample_rate */
static uint64_t bw_to_x;        /* Q16 Hz to pi.bw/sample_rate Q30, >> 32 */
static uint32_t hz_to_sin_index; /* Q16 Hz to a sin_table index Q16, >> 32 */

/* Q16 product, for a parameter applied to a value: ((int64_t)x * p) >> 16,
 * as one smull on ARM, where the compiler's 64-bit arithmetic does not
 * notice that both halves fit and multiplies three times. */
static inline int32_t mul16(int32_t x, int32_t p)
{
    return FRACMUL_SHL(x, p, 15);
}

/* fade from old_val towards new_val by ratio (Q16) */
static inline int32_t fade_value(int32_t old_val, int32_t new_val,
                                 int32_t ratio)
{
    return old_val + mul16(new_val - old_val, ratio);
}

/** Sources **/

static uint32_t noise_seed;

/* Uniform, 0 to 1. */
static int32_t noise_uniform(void)
{
#ifdef SYNTH_LIBC_RAND
    /* NV Speech Player's own source, for comparison with its renders */
    return ((int64_t)rand() << SIG_BITS) / RAND_MAX;
#else
    noise_seed = noise_seed * 1103515245u + 12345u;
    return noise_seed >> (32 - SIG_BITS);
#endif
}

/* Aspiration: uniform plus 0.75 of the last value, tilted to the low end, 0
 * to 4. */
static int32_t noise_next(int32_t *last)
{
    *last = noise_uniform() + ((*last * 3) >> 2);
    return *last;
}

/* Frication: uniform and centred, -0.5 to 0.5. NV Speech Player tilts it as
 * it does aspiration, which leaves f and v a dull hum beside the vowels. */
static int32_t noise_white(void)
{
    return noise_uniform() - SIG_ONE / 2;
}

static struct
{
    uint32_t pitch_phase;
    uint32_t vibrato_phase;
    int32_t aspiration;         /* noise_next() state */
} src;

/* n samples of both sources, each halved and scaled by the pre-formant
 * gain: cascade[] the voice and aspiration, parallel[] the frication. f
 * holds for the block but for pitch, which starts at f's and moves by
 * pitch_step after each sample. */
#ifdef CPU_ARM
/* synth_arm.S reads these at its SA_ offsets */
struct source_args
{
    int32_t pitch, pitch_step;
    uint32_t phase_per_hz;
    int32_t open, turb_amp, voice_amp, asp_amp, fric_amp, gain;
    uint32_t pitch_phase;
    int32_t aspiration;
    uint32_t seed;
};

void synth_source_block(struct source_args *a, int n,
                        int32_t *cascade, int32_t *parallel);
#endif

static void source_block(const synth_param_t *f,
                                    int32_t pitch_step, int n,
                                    int32_t *cascade, int32_t *parallel)
{
    int32_t pitch = f[SP_VOICE_PITCH];
    uint32_t pitch_phase = src.pitch_phase;
    uint32_t vibrato_step = ((uint64_t)f[SP_VIBRATO_SPEED] * phase_per_hz)
                            >> 16;
    int32_t asp_last = src.aspiration;
    int32_t open = f[SP_GLOTTAL_OPEN_QUOTIENT] << (SIG_BITS - 16);
    int32_t turb_amp = f[SP_VOICE_TURBULENCE];
    int32_t voice_amp = f[SP_VOICE_AMPLITUDE];
    int32_t asp_amp = f[SP_ASPIRATION_AMPLITUDE];
    int32_t fric_amp = f[SP_FRICATION_AMPLITUDE];
    int32_t gain = f[SP_PRE_FORMANT_GAIN];

#if defined(CPU_ARM) && !defined(SYNTH_LIBC_RAND)
    if (!f[SP_VIBRATO_PITCH_OFFSET])
    {
        struct source_args a =
        {
            pitch, pitch_step, phase_per_hz, open, turb_amp, voice_amp,
            asp_amp, fric_amp, gain, pitch_phase, asp_last, noise_seed,
        };
        synth_source_block(&a, n, cascade, parallel);
        src.pitch_phase = a.pitch_phase;
        src.aspiration = a.aspiration;
        src.vibrato_phase += vibrato_step * n;
        noise_seed = a.seed;
        return;
    }
#endif

    for (int i = 0; i < n; i++)
    {
        if (i > 0)
            pitch += pitch_step;
        int64_t hz = pitch;
        src.vibrato_phase += vibrato_step;
        if (f[SP_VIBRATO_PITCH_OFFSET])
        {
            long cos;
            long sin = fp_sincos(src.vibrato_phase, &cos);
            /* 1 + sin * 0.06 * offset, Q16 */
            int32_t vibrato = SYNTH_ONE + mul16(
                ((int64_t)sin * 3932) >> 31, f[SP_VIBRATO_PITCH_OFFSET]);
            hz = mul16(hz, vibrato);
        }
        pitch_phase += (hz * phase_per_hz) >> 16;

        int32_t voice = pitch_phase >> (32 - SIG_BITS);
        int32_t asp = mul16(noise_next(&asp_last), 13107);         /* 0.2 */
        int32_t turbulence = mul16(asp, turb_amp);
        if (voice < open)
            turbulence = mul16(turbulence, 655);                    /* 0.01 */
        voice = (voice * 2) - SIG_ONE + turbulence;
        voice = mul16(voice, voice_amp);
        asp = mul16(asp, asp_amp);
        cascade[i] = mul16(asp + voice, gain) >> 1;

        int32_t fric = mul16(mul16(noise_white(), 49152),           /* 0.75 */
                             fric_amp);
        parallel[i] = mul16(fric, gain) >> 1;
    }
    src.pitch_phase = pitch_phase;
    src.aspiration = asp_last;
}

/** Resonators **/

/* synth_arm.S reads a to p2 at offsets 0 to 16 */
struct resonator
{
    int32_t a, b, c;    /* Q28; Q20 for the antiresonator */
    int32_t p1, p2;
    int32_t frequency, bandwidth;   /* what a, b and c were made for */
};

#ifdef CPU_ARM
void synth_resonate_block(struct resonator *r, const int32_t *in,
                          int32_t *out, int n);
void synth_antiresonate_block(struct resonator *r, const int32_t *in,
                              int32_t *out, int n);
void synth_resonate_pair_block(struct resonator *r1, struct resonator *r2,
                               int32_t *buf, int n);
void synth_parallel_formant_block(struct resonator *r, const int32_t *in,
                                  int32_t *sum, int n, int32_t pa);
#endif

/* sin over the first quadrant in 1024 steps, Q31, for set_coefficients() */
static int32_t sin_table[1025];

/* 2^30 / k: the terms of 1 - e^-x */
static const int32_t inv_k[] =
{
    0, 0, 536870912, 357913941, 268435456, 214748365, 178956971,
    153391689, 134217728, 119304647, 107374182,
};

static inline int64_t mul30(int64_t a, int64_t b)
{
    return (a * b) >> 30;
}

/* 1 - e^-x, Q30, as x(1 - x/2(1 - x/3(...))): exact where the subtraction
 * would not be. Ten terms hold Q30 for any bandwidth the table uses. */
static int64_t one_minus_exp_neg(int64_t x)
{
    int64_t t = Q30_ONE;
    for (int k = 10; k >= 2; k--)
        t = Q30_ONE - mul30(mul30(x, t), inv_k[k]);
    return mul30(x, t);
}

/* sin(pi.f/sample_rate), Q31, f Q16 Hz below the Nyquist frequency */
static int64_t half_angle_sin(int32_t frequency)
{
    uint32_t pos = ((uint64_t)abs(frequency) * hz_to_sin_index) >> 32;
    unsigned int i = pos >> 16;
    if (i >= 1024)
        return sin_table[1024];
    int32_t s0 = sin_table[i];
    return s0 + (((int64_t)(sin_table[i + 1] - s0) * (pos & 0xffff)) >> 16);
}

/* a = 1 - b - c is computed as (1-r)^2 + 4r.sin^2(theta/2), the same value,
 * because for a low narrow formant it is tiny and the subtraction would
 * leave little of it. */
static void set_coefficients(struct resonator *r, bool anti,
                             int32_t frequency, int32_t bandwidth)
{
    r->frequency = frequency;
    r->bandwidth = bandwidth;

    int64_t x = ((uint64_t)bandwidth * bw_to_x) >> 32;
    int64_t one_minus_rad = one_minus_exp_neg(x);
    int64_t rad = Q30_ONE - one_minus_rad;
    int64_t rad2 = mul30(rad, rad);
    int64_t sin = half_angle_sin(frequency);
    int64_t sin2 = (sin * sin) >> 32;

    int64_t a = mul30(one_minus_rad, one_minus_rad) + mul30(4 * rad, sin2);
    int64_t b = Q30_ONE - a + rad2;
    int64_t c = -rad2;

    if (anti && frequency != 0)
    {
        int64_t inv_a = (Q30_ONE << 30) / a >> 10;      /* Q20 */
        r->a = inv_a;
        r->b = -(b * inv_a) >> 30;
        r->c = -(c * inv_a) >> 30;
    }
    else
    {
        r->a = a >> 2;
        r->b = b >> 2;
        r->c = c >> 2;
    }
}

/* in and out may be the same buffer */
static void resonate_block(struct resonator *r, const int32_t *in,
                           int32_t *out, int n)
{
#ifdef CPU_ARM
    synth_resonate_block(r, in, out, n);
#else
    int32_t a = r->a, b = r->b, c = r->c, p1 = r->p1, p2 = r->p2;
    for (int i = 0; i < n; i++)
    {
        int64_t acc = (int64_t)a * in[i] + (int64_t)b * p1
                      + (int64_t)c * p2 + (1 << 27);
        p2 = p1;
        p1 = acc >> 28;
        out[i] = p1;
    }
    r->p1 = p1;
    r->p2 = p2;
#endif
}

/* The antiresonator: a zero, fed back its inputs rather than its outputs. A
 * frequency of 0 leaves it with ordinary Q28 coefficients. */
static void antiresonate_block(struct resonator *r, const int32_t *in,
                               int32_t *out, int n)
{
#ifdef CPU_ARM
    if (r->frequency)
    {
        synth_antiresonate_block(r, in, out, n);
        return;
    }
#endif
    int shift = r->frequency ? 20 : 28;
    int32_t a = r->a, b = r->b, c = r->c, p1 = r->p1, p2 = r->p2;
    for (int i = 0; i < n; i++)
    {
        int32_t x = in[i];
        int64_t acc = (int64_t)a * x + (int64_t)b * p1 + (int64_t)c * p2;
        out[i] = (acc + (1ll << (shift - 1))) >> shift;
        p2 = p1;
        p1 = x;
    }
    r->p1 = p1;
    r->p2 = p2;
}

static struct
{
    struct resonator cascade[6], n0, np;  /* n0 is the antiresonator */
    struct resonator parallel[6];
} res;

static void update_resonator(struct resonator *r, bool anti,
                             int32_t frequency, int32_t bandwidth)
{
    if (frequency != r->frequency || bandwidth != r->bandwidth)
        set_coefficients(r, anti, frequency, bandwidth);
}

static void update_coefficients(const synth_param_t *f)
{
    update_resonator(&res.n0, true, f[SP_CF_N0], f[SP_CB_N0]);
    update_resonator(&res.np, false, f[SP_CF_NP], f[SP_CB_NP]);
    for (int i = 0; i < 6; i++)
    {
        update_resonator(&res.cascade[i], false, f[SP_CF1 + i], f[SP_CB1 + i]);
        if (synth_parallel_used & (1 << i))
            update_resonator(&res.parallel[i], false,
                             f[SP_PF1 + i], f[SP_PB1 + i]);
    }
}

static bool all_zero(const int32_t *x, int n)
{
    for (int i = 0; i < n; i++)
        if (x[i])
            return false;
    return true;
}

/* True if every resonator of a bank is at rest. Those within SETTLED of it
 * are put there, which the bank's silent input allows. */
static bool bank_settled(struct resonator *r, int count)
{
    bool settled = true;
    for (int i = 0; i < count; i++)
    {
        if (abs(r[i].p1) <= SETTLED && abs(r[i].p2) <= SETTLED)
            r[i].p1 = r[i].p2 = 0;
        else
            settled = false;
    }
    return settled;
}

/* in: the voice, halved, n samples; out: the cascade's output */
static void cascade_block(const synth_param_t *f, const int32_t *in,
                          int32_t *out, int n)
{
    static int32_t np[BLOCK_MAX];

    if (all_zero(in, n) && bank_settled(&res.n0, 1)
        && bank_settled(&res.np, 1) && bank_settled(res.cascade, 6))
    {
        memset(out, 0, n * sizeof(*out));
        return;
    }
    /* The nasal pair is heard only through ca. Without it, it rests, and a
     * nasal starts it from rest rather than from its history. */
    int32_t ca = f[SP_CA_NP];
    if (ca)
    {
        antiresonate_block(&res.n0, in, np, n);
        resonate_block(&res.np, np, np, n);
        for (int i = 0; i < n; i++)
            out[i] = in[i] + mul16(np[i] - in[i], ca);
    }
    else
    {
        res.n0.p1 = res.n0.p2 = res.np.p1 = res.np.p2 = 0;
        if (out != in)
            memcpy(out, in, n * sizeof(*out));
    }
    for (int k = 5; k >= 0; k -= 2)
    {
#ifdef CPU_ARM
        synth_resonate_pair_block(&res.cascade[k], &res.cascade[k - 1],
                                  out, n);
#else
        resonate_block(&res.cascade[k], out, out, n);
        resonate_block(&res.cascade[k - 1], out, out, n);
#endif
    }
}

/* in: the frication, halved, n samples; out: the parallel bank's output.
 * in and out may be the same buffer. */
static void parallel_block(const synth_param_t *f, const int32_t *in,
                           int32_t *out, int n)
{
    static int32_t formant[BLOCK_MAX], sum[BLOCK_MAX];

    if (all_zero(in, n) && bank_settled(res.parallel, 6))
    {
        memset(out, 0, n * sizeof(*out));
        return;
    }
    memset(sum, 0, n * sizeof(*sum));
    for (int k = 0; k < 6; k++)
    {
        if (!(synth_parallel_used & (1 << k)))
            continue;
        int32_t pa = f[SP_PA1 + k];
#ifdef CPU_ARM
        if (pa)
        {
            synth_parallel_formant_block(&res.parallel[k], in, sum, n, pa);
            continue;
        }
#endif
        resonate_block(&res.parallel[k], in, formant, n);
        if (pa)
            for (int i = 0; i < n; i++)
                sum[i] += mul16(formant[i] - in[i], pa);
    }
    int32_t bypass = f[SP_PARALLEL_BYPASS];
    for (int i = 0; i < n; i++)
        out[i] = fade_value(sum[i], in[i], bypass);
}

/** The frame queue **/

struct request
{
    uint32_t min_samples;
    uint32_t fade_samples;      /* the formants' */
    uint32_t amp_fade_samples;  /* the rest's, <= fade_samples */
    bool silence;
    struct synth_frame frame;
    int32_t pitch_inc;          /* per sample, while the frame is held */
};

static struct
{
    struct request queue[SYNTH_QUEUE_SIZE];
    unsigned int head, tail;    /* tail - head is the number queued */
    struct request old, new;
    bool fading;                /* new is being faded in */
    bool changed;               /* cur has moved beyond its pitch */
    struct synth_frame cur;
    bool cur_silent;
    uint32_t counter;           /* samples since the current request began */
} fm;

/* Step one sample. Returns the frame to render, or NULL for none. */
static const synth_param_t *frame_next(void)
{
    fm.counter++;
    if (fm.fading)
    {
        if (fm.counter > fm.new.fade_samples)
        {
            fm.old = fm.new;
            fm.fading = false;
        }
        else if (((fm.counter - 1) & (CONTROL_PERIOD - 1)) == 0
                 || fm.counter == fm.new.fade_samples)
        {
            int32_t ratio = ((int64_t)fm.counter << 16) / fm.new.fade_samples;
            int32_t fast = fm.counter >= fm.new.amp_fade_samples ? SYNTH_ONE
                : ((int64_t)fm.counter << 16) / fm.new.amp_fade_samples;
            for (int i = 0; i < SYNTH_PARAM_COUNT; i++)
            {
                bool formant = (i >= SP_CF1 && i <= SP_CB_NP)
                               || (i >= SP_PF1 && i <= SP_PB6);
                fm.cur.p[i] = fade_value(fm.old.frame.p[i], fm.new.frame.p[i],
                                         formant ? ratio : fast);
            }
            fm.changed = true;
        }
    }
    else if (fm.counter > fm.old.min_samples)
    {
        if (fm.tail != fm.head)
        {
            fm.cur_silent = false;
            fm.new = fm.queue[fm.head++ % SYNTH_QUEUE_SIZE];
            fm.fading = true;
            if (fm.new.silence)
            {
                fm.new.frame = fm.old.frame;
                fm.new.frame.p[SP_PRE_FORMANT_GAIN] = 0;
                fm.new.frame.p[SP_VOICE_PITCH] = fm.cur.p[SP_VOICE_PITCH];
                fm.new.pitch_inc = 0;
            }
            else if (fm.old.silence)
            {
                fm.old.frame = fm.new.frame;
                fm.old.frame.p[SP_PRE_FORMANT_GAIN] = 0;
            }
            fm.counter = 0;
            fm.new.frame.p[SP_VOICE_PITCH] +=
                fm.new.pitch_inc * (int32_t)fm.new.fade_samples;
        }
        else
        {
            fm.cur_silent = true;
        }
    }
    else
    {
        fm.cur.p[SP_VOICE_PITCH] += fm.old.pitch_inc;
        fm.old.frame.p[SP_VOICE_PITCH] = fm.cur.p[SP_VOICE_PITCH];
    }
    return fm.cur_silent ? NULL : fm.cur.p;
}

/* How many more calls of frame_next() would be plain: counting a sample
 * and, while a frame is held, stepping its pitch, and nothing else. A tick,
 * the end of a fade and the start of the next frame are not plain. */
static uint32_t frames_plain(void)
{
    uint32_t c = fm.counter;
    if (fm.fading)
    {
        uint32_t fade = fm.new.fade_samples;
        if (c >= fade)
            return 0;
        uint32_t to_tick = (1 - c) & (CONTROL_PERIOD - 1);
        if (to_tick == 0)
            to_tick = CONTROL_PERIOD;
        uint32_t to_end = fade - c;
        return (to_tick < to_end ? to_tick : to_end) - 1;
    }
    return c < fm.old.min_samples ? fm.old.min_samples - c : 0;
}

/* The pitch step of a plain call */
static int32_t frames_pitch_step(void)
{
    return fm.fading ? 0 : fm.old.pitch_inc;
}

/* Make n plain calls of frame_next() at once. */
static void frames_skip(uint32_t n)
{
    fm.counter += n;
    if (!fm.fading)
    {
        fm.cur.p[SP_VOICE_PITCH] += (int32_t)n * fm.old.pitch_inc;
        fm.old.frame.p[SP_VOICE_PITCH] = fm.cur.p[SP_VOICE_PITCH];
    }
}

/** Interface **/

void synth_init(int rate)
{
    sample_rate = rate;
    phase_per_hz = (1ull << 32) / rate;
    bw_to_x = ((uint64_t)PI_Q30 << 16) / rate;
    hz_to_sin_index = (2048ull << 32) / rate;
    for (int i = 0; i <= 1024; i++)
    {
        long cos;
        long sin = fp_sincos((unsigned long)i << 20, &cos);
        sin_table[i] = i == 1024 ? 0x7fffffff : sin;
    }
    noise_seed = 1;
    memset(&src, 0, sizeof(src));
    memset(&res, 0, sizeof(res));       /* zero coefficients: silent */
    memset(&fm, 0, sizeof(fm));
    fm.old.silence = true;
    fm.cur_silent = true;
}

int synth_queue_space(void)
{
    return SYNTH_QUEUE_SIZE - (fm.tail - fm.head);
}

bool synth_queue_frame(const struct synth_frame *frame, int32_t min_us,
                       int32_t fade_us, int32_t formant_fade_us)
{
    if (synth_queue_space() == 0)
        return false;

    struct request *r = &fm.queue[fm.tail % SYNTH_QUEUE_SIZE];
    r->min_samples = (int64_t)min_us * sample_rate / 1000000;
    r->amp_fade_samples = (int64_t)fade_us * sample_rate / 1000000;
    if (r->amp_fade_samples < 1)
        r->amp_fade_samples = 1;
    r->fade_samples = r->amp_fade_samples;
    if (formant_fade_us > fade_us)
        r->fade_samples = (int64_t)formant_fade_us * sample_rate / 1000000;
    r->silence = frame == NULL;
    if (frame)
    {
        r->frame = *frame;
        r->pitch_inc = r->min_samples
            ? (frame->p[SP_END_VOICE_PITCH] - frame->p[SP_VOICE_PITCH])
              / (int32_t)r->min_samples
            : 0;
    }
    fm.tail++;
    return true;
}

int synth_generate(int16_t *buf, int count)
{
    static int32_t cascade[BLOCK_MAX], parallel[BLOCK_MAX];
    int done = 0;

    while (done < count)
    {
        const synth_param_t *f = frame_next();
        if (!f)
            break;
        if (fm.changed)
        {
            update_coefficients(f);
            fm.changed = false;
        }

        /* The block: this sample and the plain ones after it, over which
         * everything in f but pitch holds. */
        uint32_t n = 1 + frames_plain();
        if (n > BLOCK_MAX)
            n = BLOCK_MAX;
        if (n > (uint32_t)(count - done))
            n = count - done;
        source_block(f, frames_pitch_step(), n, cascade, parallel);
        frames_skip(n - 1);

        cascade_block(f, cascade, cascade, n);
        parallel_block(f, parallel, parallel, n);

        int32_t out_gain = f[SP_OUTPUT_GAIN];
        for (uint32_t i = 0; i < n; i++)
        {
            /* x 4000 >> SIG_BITS, as one multiply */
            int32_t out = FRACMUL_SHL(mul16(cascade[i] + parallel[i], out_gain),
                                      4000, 31 - SIG_BITS);
            buf[done + i] = out > 32000 ? 32000 : out < -32000 ? -32000 : out;
        }
        done += n;
    }
    return done;
}
