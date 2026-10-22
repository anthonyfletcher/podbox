/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The equaliser's frequency response -- for the editor's curve and for Auto
 * Headroom -- and the DSP's limits on a band. Platform-free: fixed point
 * only, so it builds and runs on a host.
 *
 * The response comes from each band's settings through the same cookbook
 * biquads the DSP uses, never from the DSP's stored coefficients. With
 * phi = sin^2(w/2) at the point, sigma = sin^2(w0/2) at the band, and both
 * divided by the larger of the two (sn, pn; their ratio is t), each filter's
 * squared magnitude is a ratio of non-negative terms:
 *
 *   peak        [(1-t)^2 + W A^2/Q^2] / [(1-t)^2 + W/(A^2 Q^2)]
 *   low shelf   A^2 [(A sn - u pn)^2 + W A/Q^2] / [(sn - v pn)^2 + W A/Q^2]
 *   high shelf  A^4 times the low shelf at 1/A
 *
 * where W = (1-sigma)(1-phi) t, u = 1 + (A-1) sigma, v = A - (A-1) sigma and
 * A = 10^(gain/40). Nothing cancels, so 32 Hz is as exact as 8 kHz.
 *
 * Parts, in order:
 *   - fixed-point helpers (s.30 in 64 bits unless named otherwise)
 *   - one band: prepared once, then evaluated at a point
 *   - the curve: point frequencies and the per-band rows
 *   - peak and headroom
 *   - the DSP's limits
 ****************************************************************************/

#include <stdint.h>
#include <stdbool.h>
#include "fixedpoint.h"
#include "eq_curve.h"

#define ONE30       (1LL << 30)
#define LN1000_16   452709          /* ln(1000), s15.16 */
#define PRECUT_MAX  240             /* the Precut setting's range, tenths */

/** fixed-point helpers **/

/* |a| < 2^33 and |b| <= 2^30 */
static int64_t mul30(int64_t a, int64_t b)
{
    return (a * b) >> 30;
}

/* a >= 0 of any size, 0 <= b <= 2^30 */
static int64_t mul30_wide(int64_t a, int64_t b)
{
    return (a >> 30) * b + (((a & (ONE30 - 1)) * b) >> 30);
}

/* |x| < 2^33 */
static int64_t sq30(int64_t x)
{
    x >>= 2;
    return (x * x) >> 26;
}

/* sin(w/2) for a frequency in Hz, s15.16, as s0.31. */
static int32_t sin_half(int64_t hz16, unsigned int fs)
{
    long cos;
    return fp_sincos((unsigned long)((hz16 << 15) / fs), &cos);
}

/* 10^(gain/40) for a gain in tenths of a dB. */
static int64_t gain_factor30(int gain)
{
    return (int64_t)fp_factor(((long)gain << 16) / 20, 16) << 14;
}

static int hz_limit(int hz, unsigned int fs)
{
    int top = (int)(fs * 49 / 100);
    return hz < 1 ? 1 : hz > top ? top : hz;
}

/** one band **/

struct band_pre
{
    bool flat;
    bool peak;
    int32_t ssig;               /* sin(w0/2), s0.31 */
    int64_t one_m_sig;          /* 1 - sigma */
    int64_t a;                  /* A; 1/A for a high shelf */
    int64_t g, h;               /* peak: A^2/Q^2 and 1/(A^2 Q^2) */
    int64_t k, u, v;            /* shelf: A/Q^2, u, v */
    int half_cdb;               /* shelf: gain/2, hundredths of a dB */
};

static void band_prepare(struct band_pre *p, const struct eq_band_setting *s,
                         unsigned int fs)
{
    p->flat = s->gain == 0;
    if (p->flat)
        return;

    int q = s->q ? s->q : 1;    /* as the DSP reads it */
    int64_t invq2 = (100LL << 16) / (q * q);            /* 1/Q^2, s.16 */
    int64_t sig;

    p->ssig = sin_half((int64_t)hz_limit(s->cutoff, fs) << 16, fs);
    sig = ((int64_t)p->ssig * p->ssig) >> 32;
    p->one_m_sig = ONE30 - sig;
    p->peak = s->type == EQ_FILTER_PEAK;
    p->a = gain_factor30(s->gain);
    p->half_cdb = s->gain * 5;

    if (p->peak)
    {
        int64_t a2 = sq30(p->a);
        p->g = (a2 * invq2) >> 16;
        p->h = (((ONE30 << 30) / a2) * invq2) >> 16;
    }
    else
    {
        if (s->type == EQ_FILTER_HIGH_SHELF)
            p->a = (ONE30 << 30) / p->a;
        p->k = (p->a * invq2) >> 16;
        p->u = ONE30 + mul30(p->a - ONE30, sig);
        p->v = p->a - mul30(p->a - ONE30, sig);
    }
}

/* The band's response at a point, hundredths of a dB. */
static int band_eval(const struct band_pre *p, int32_t sphi)
{
    if (p->flat)
        return 0;

    int32_t lo = sphi < p->ssig ? sphi : p->ssig;
    int32_t hi = sphi < p->ssig ? p->ssig : sphi;
    int64_t phi = ((int64_t)sphi * sphi) >> 32;
    int64_t r = ((int64_t)lo << 30) / (hi > 0 ? hi : 1);
    int64_t t = (r * r) >> 30;
    int64_t w = mul30(mul30(p->one_m_sig, ONE30 - phi), t);
    int64_t num, den, ratio;

    if (p->peak)
    {
        int64_t base = sq30(ONE30 - t);
        num = base + mul30_wide(p->g, w);
        den = base + mul30_wide(p->h, w);
    }
    else
    {
        int64_t sn = sphi >= p->ssig ? t : ONE30;
        int64_t pn = sphi >= p->ssig ? ONE30 : t;
        int64_t kw = mul30_wide(p->k, w);
        num = sq30(mul30(p->a, sn) - mul30(p->u, pn)) + kw;
        den = sq30(sn - mul30(p->v, pn)) + kw;
    }

    ratio = (num << 16) / (den > 0 ? den : 1);
    if (ratio < 1)
        ratio = 1;
    if (ratio > 0x7fffffff)
        ratio = 0x7fffffff;
    /* 10 log10(x) = 4.3429 ln(x); 27795 / 2^22 is 4.3429 / 65536 * 100 */
    int cdb = (int)(((int64_t)fp16_log((int)ratio) * 27795) >> 22);
    return p->peak ? cdb : p->half_cdb + cdb;
}

/** the curve **/

static int32_t point_hz16(int point)
{
    return 20 * fp16_exp(point * LN1000_16 / (EQ_CURVE_POINTS - 1));
}

int eq_curve_point_hz(int point)
{
    return (point_hz16(point) + 0x8000) >> 16;
}

int eq_curve_position(int hz)
{
    if (hz <= 20)
        return 0;
    int64_t pos = (int64_t)fp16_log(hz * 65536 / 20) * 65536 / LN1000_16;
    return pos > 65536 ? 65536 : (int)pos;
}

void eq_curve_init(struct eq_curve *c, unsigned int fs)
{
    int64_t top = (int64_t)(fs * 49 / 100) << 16;

    c->fs = fs;
    for (int i = 0; i < EQ_CURVE_POINTS; i++)
    {
        int64_t hz16 = point_hz16(i);
        c->sin_half[i] = sin_half(hz16 < top ? hz16 : top, fs);
        for (int b = 0; b < EQ_NUM_BANDS; b++)
            c->band_cdb[b][i] = 0;
    }
}

void eq_curve_set_band(struct eq_curve *c, int band,
                       const struct eq_band_setting *s)
{
    struct band_pre p;

    band_prepare(&p, s, c->fs);
    for (int i = 0; i < EQ_CURVE_POINTS; i++)
    {
        int v = band_eval(&p, c->sin_half[i]);
        c->band_cdb[band][i] = v > 32767 ? 32767 : v < -32767 ? -32767 : v;
    }
}

int eq_curve_total(const struct eq_curve *c, int point)
{
    int sum = 0;
    for (int b = 0; b < EQ_NUM_BANDS; b++)
        sum += c->band_cdb[b][point];
    return sum;
}

/** peak and headroom **/

int eq_curve_peak(const struct eq_band_setting *bands, unsigned int fs)
{
    struct band_pre p[EQ_NUM_BANDS];
    int64_t top = (int64_t)(fs * 49 / 100) << 16;
    int peak = -32767;

    for (int b = 0; b < EQ_NUM_BANDS; b++)
        band_prepare(&p[b], &bands[b], fs);

    for (int i = 0; i < EQ_CURVE_POINTS + EQ_NUM_BANDS; i++)
    {
        int64_t hz16 = i < EQ_CURVE_POINTS ? point_hz16(i)
            : (int64_t)hz_limit(bands[i - EQ_CURVE_POINTS].cutoff, fs) << 16;
        int32_t sphi = sin_half(hz16 < top ? hz16 : top, fs);
        int sum = 0;

        for (int b = 0; b < EQ_NUM_BANDS; b++)
            sum += band_eval(&p[b], sphi);
        if (sum > peak)
            peak = sum;
    }
    return peak;
}

int eq_curve_headroom(const struct eq_band_setting *bands, unsigned int fs)
{
    int peak = eq_curve_peak(bands, fs);
    int tenths;

    if (peak <= 0)
        return 0;
    tenths = (peak + 49) / 50 * 5;
    return tenths > PRECUT_MAX ? PRECUT_MAX : tenths;
}

/** the DSP's limits **/

/* filter_pk_coefs() holds alpha = sin(w0)/2Q as s1.30, and b0 = 1 + alpha A
 * and a0 = 1 + alpha/A as s3.28, all before it divides by a0. The shelves
 * share the alpha. */
bool eq_band_fits(const struct eq_band_setting *s, unsigned int fs)
{
    long cos;
    int q = s->q ? s->q : 1;
    int32_t sw0;
    int64_t alpha, a, amax;

    if (s->gain == 0)
        return true;
    sw0 = fp_sincos((unsigned long)(((int64_t)s->cutoff << 32) / fs), &cos);
    if (sw0 < 0)
        return false;
    alpha = ((int64_t)sw0 * 10 / (2 * q)) >> 1;
    if (alpha >= 2 * ONE30)
        return false;
    if (s->type != EQ_FILTER_PEAK)
        return true;

    a = gain_factor30(s->gain);
    amax = a >= ONE30 ? a : (ONE30 << 30) / a;
    return ONE30 + (((alpha >> 2) * (amax >> 2)) >> 26) < 8 * ONE30;
}
