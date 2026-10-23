/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * The equalizer's band filters. Each cookbook filter is unity at one end of
 * the spectrum or both, so the output is the input plus a difference,
 * y = x + e, and e needs fewer multiplies than a general biquad:
 *
 *   peak        e = g (x - x2) + c1 e1 + c2 e2                 (3)
 *   low shelf   e = n0 u + n2 u1 + c1 e1 + c2 e2, u = x + x1   (4)
 *   high shelf  the same, with u = x - x1                      (4)
 *   first-order shelves, for AFR and PBE:  e = k u + p e1      (2)
 *   the same with a gain G, for PBE:  y = G x + e, with G k for k  (3)
 *
 * where x1, x2 are the last two inputs, e1, e2 the last two differences and
 * u1 the last u. struct dsp_filter carries them with its own meaning per
 * type -- coefs and history below -- so a band changing type goes through
 * eq_filter_convert(). The sum is carried down as filter_process() does it:
 * see dsp_filter.c for the frac word. Samples reach 16x full scale, so u and
 * x - x2 can reach 32x: they are held at the 32-bit limits, not wrapped.
 *
 * Parts, in order:
 *   - coefficients: the peak from filter_pk_coefs()' own intermediates, the
 *     shelves in 64 bits from sin^2 and cos^2 of half the cutoff, the
 *     first-order shelves from its sin and cos
 *   - changing a band's type without losing its history
 *   - the filters in C, where eq_filter_arm.S does not stand in for them
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "rbcodecconfig.h"
#include "fixedpoint.h"
#include "fracmul.h"
#include "replaygain.h"
#include "eq_filter.h"

/* coefs: peak { g, c1, c2 } s3.28; shelves { n0, n2, c1, c2 } s4.27;
   first-order shelves { G k, p, G } s3.28. history per channel: peak
   { x2, x1, e1, e2 }, shelves { x1, u1, e1, e2 }, first-order { x1, e1 } */
#define PEAK_SHIFT   4
#define SHELF_SHIFT  5
#define SHELF1_SHIFT 4

/** Coefficients **/

/* alpha, A and a0 are filter_pk_coefs()' own, so a setting overflows here
   exactly where it overflows there. */
static void peak_coefs(unsigned long cutoff, unsigned long Q, long db,
                       struct dsp_filter *f)
{
    long cs;
    const long one = 1 << 28; /* s3.28 */
    const long A = get_replaygain_int(db*5) << 5; /* 10^(db/40), s2.29 */
    const long alpha = fp_sincos(cutoff, &cs)/(2*Q)*10 >> 1; /* s1.30 */
    const long alphadivA = fp_div(alpha, A, 27);
    const long alphaA = FRACMUL(alpha, A);
    const int32_t a0 = one + alphadivA;
    const long rcp_a0 = fp_div(1, a0, 59); /* s0.31 */

    f->coefs[0] = FRACMUL(alphaA - alphadivA, rcp_a0); /* b0 - 1 = a2 - b2 */
    f->coefs[1] = FRACMUL(2*(cs >> 3), rcp_a0);         /* -a1 */
    f->coefs[2] = FRACMUL(alphadivA - one, rcp_a0);     /* -a2 */
    f->shift = PEAK_SHIFT;
}

#define Q26(x) ((int64_t)(x) << 26)
static inline int64_t mul26(int64_t a, int64_t b)
{
    return (a * b) >> 26;
}

/* n / a0 in s4.27 */
static inline int32_t div27(int64_t n, int64_t a0)
{
    return (n << 27) / a0;
}

/* With s = 2 sqrt(A) alpha, m = 1 - cos w and n = 1 + cos w for the low
   shelf (swapped for the high), the cookbook shelf comes to
     a0 = 2 + (A-1) n + s,           n0 = (A-1) ((A+1) m + s) / a0,
     n2 = +-(A-1) ((A+1) m - s) / a0,
     c1 = +-2 ((A+1) n - 2) / a0,    c2 = -(2 + (A-1) n - s) / a0,
   + for the low shelf. m and n come from half the angle, as 2 sin^2 and
   2 cos^2: the smaller directly and the other as 2 less it, since the
   formulas need m + n = 2 exactly. Nothing subtracts nearly equal terms,
   so the poles keep their precision wherever the shelf sits. */
static void shelf_coefs(bool low, unsigned long cutoff, unsigned long Q,
                        long db, struct dsp_filter *f)
{
    long cs, ch;
    const int64_t sqrt_a = (int64_t)get_replaygain_int(db*5/2) << 2;
    const int64_t a = mul26(sqrt_a, sqrt_a);
    const long alpha = fp_sincos(cutoff, &cs)/(2*Q)*10 >> 1; /* s1.30 */
    const int64_t s = (sqrt_a * alpha) >> 29;
    const int64_t sh = fp_sincos(cutoff/2, &ch);
    const int64_t sin2 = sh < ch ? (sh*sh) >> 35
                                 : Q26(2) - (((int64_t)ch*ch) >> 35);
    const int64_t cos2 = Q26(2) - sin2;
    const int64_t m = low ? sin2 : cos2, n = low ? cos2 : sin2;
    const int64_t am1 = a - Q26(1), ap1 = a + Q26(1);
    const int64_t a0 = Q26(2) + mul26(am1, n) + s;
    const int sign = low ? 1 : -1;

    f->coefs[0] = div27(mul26(am1, mul26(ap1, m) + s), a0);
    f->coefs[1] = sign * div27(mul26(am1, mul26(ap1, m) - s), a0);
    f->coefs[2] = sign * div27(2*(mul26(ap1, n) - Q26(2)), a0);
    f->coefs[3] = -div27(Q26(2) + mul26(am1, n) - s, a0);
    f->shift = SHELF_SHIFT;
}

/* filter_shelf_coefs()' shelf, with g = 10^(db/40) and s, c the sine and
   cosine of half the cutoff: for the low shelf
     k = s (g^2 - 1) / (g c + s),  p = (g c - s) / (g c + s),
   and for the high, s and c swapped and p negated. Gains to +9 dB, and
   G = 10^(gain/20) to the same. */
void eq_shelf1_coefs(bool low, unsigned long cutoff, long db, long gain,
                     struct dsp_filter *f)
{
    long sn, cs;
    const int64_t g = get_replaygain_int(db*5); /* s7.24 */
    const int64_t G = get_replaygain_int(gain*10); /* s7.24 */
    const int64_t gg1 = ((g*g) >> 24) - (1 << 24);
    sn = fp_sincos(cutoff/2, &cs);
    const int64_t s = low ? sn : cs, c = low ? cs : sn; /* s0.31 */
    const int64_t den = ((g*c) >> 24) + s;

    f->coefs[0] = ((s*gg1) / (den >> 4) * G) >> 24;
    f->coefs[1] = (low ? 1 : -1) * (((((g*c) >> 24) - s) << 28) / den);
    f->coefs[2] = G << 4;
    f->shift = SHELF1_SHIFT;
}

void eq_filter_coefs(enum eq_filter_type type, unsigned long cutoff,
                     unsigned long Q, long db, struct dsp_filter *f)
{
    if (type == EQ_FILTER_PEAK)
        peak_coefs(cutoff, Q, db, f);
    else
        shelf_coefs(type == EQ_FILTER_LOW_SHELF, cutoff, Q, db, f);
}

/** Changing type **/

void eq_filter_convert(struct dsp_filter *f, enum eq_filter_type from,
                       enum eq_filter_type to)
{
    for (int c = 0; c < 2; c++)
    {
        int32_t *h = f->history[c];
        int32_t x1, x2;

        if (from == EQ_FILTER_PEAK)
            x2 = h[0], x1 = h[1];
        else if (from == EQ_FILTER_LOW_SHELF)
            x1 = h[0], x2 = h[1] - x1;
        else
            x1 = h[0], x2 = x1 - h[1];

        if (to == EQ_FILTER_PEAK)
            h[0] = x2, h[1] = x1;
        else if (to == EQ_FILTER_LOW_SHELF)
            h[0] = x1, h[1] = x1 + x2;
        else
            h[0] = x1, h[1] = x1 - x2;
    }
}

void eq_filter_process(struct dsp_filter *f, enum eq_filter_type type,
                       int32_t * const buf[], int count,
                       unsigned int channels)
{
    if (type == EQ_FILTER_PEAK)
        eq_peak_process(f, buf, count, channels);
    else if (type == EQ_FILTER_LOW_SHELF)
        eq_low_shelf_process(f, buf, count, channels);
    else
        eq_high_shelf_process(f, buf, count, channels);
}

/** The filters in C **/

#if !defined(CPU_ARM) || defined(CPU_ARM_MICRO)
/* e from the sum, carrying the bits below it in frac */
static inline int32_t carry_down(int64_t acc, unsigned int shift,
                                 uint32_t *frac)
{
    uint64_t out = ((uint64_t)acc << shift) + *frac;
    *frac = out;
    return out >> 32;
}

static inline int32_t sat32(int64_t v)
{
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v;
}

void eq_peak_process(struct dsp_filter *f, int32_t * const buf[], int count,
                     unsigned int channels)
{
    const int32_t *c = f->coefs;

    for (unsigned int ch = 0; ch < channels; ch++)
    {
        int32_t *h = f->history[ch], *p = buf[ch];

        for (int i = 0; i < count; i++)
        {
            int32_t x = p[i];
            int64_t acc = (int64_t)sat32((int64_t)x - h[0]) * c[0]
                        + (int64_t)h[2] * c[1] + (int64_t)h[3] * c[2];
            int32_t e = carry_down(acc, PEAK_SHIFT, &f->frac[ch]);
            h[0] = h[1], h[1] = x, h[3] = h[2], h[2] = e;
            p[i] = x + e;
        }
    }
}

static void shelf_process(struct dsp_filter *f, int32_t * const buf[],
                          int count, unsigned int channels, bool low)
{
    const int32_t *c = f->coefs;

    for (unsigned int ch = 0; ch < channels; ch++)
    {
        int32_t *h = f->history[ch], *p = buf[ch];

        for (int i = 0; i < count; i++)
        {
            int32_t x = p[i];
            int32_t u = sat32(low ? (int64_t)x + h[0] : (int64_t)x - h[0]);
            int64_t acc = (int64_t)u * c[0] + (int64_t)h[1] * c[1]
                        + (int64_t)h[2] * c[2] + (int64_t)h[3] * c[3];
            int32_t e = carry_down(acc, SHELF_SHIFT, &f->frac[ch]);
            h[0] = x, h[1] = u, h[3] = h[2], h[2] = e;
            p[i] = x + e;
        }
    }
}

void eq_low_shelf_process(struct dsp_filter *f, int32_t * const buf[],
                          int count, unsigned int channels)
{
    shelf_process(f, buf, count, channels, true);
}

void eq_high_shelf_process(struct dsp_filter *f, int32_t * const buf[],
                           int count, unsigned int channels)
{
    shelf_process(f, buf, count, channels, false);
}

static void shelf1_process(struct dsp_filter *f, int32_t * const buf[],
                           int count, unsigned int channels, bool low,
                           bool gain)
{
    const int32_t *c = f->coefs;

    for (unsigned int ch = 0; ch < channels; ch++)
    {
        int32_t *h = f->history[ch], *p = buf[ch];

        for (int i = 0; i < count; i++)
        {
            int32_t x = p[i];
            int32_t u = sat32(low ? (int64_t)x + h[0] : (int64_t)x - h[0]);
            int64_t acc = (int64_t)u * c[0] + (int64_t)h[1] * c[1];
            int32_t e = carry_down(acc, SHELF1_SHIFT, &f->frac[ch]);
            h[0] = x, h[1] = e;
            if (gain)
                x = ((int64_t)x * c[2]) >> (32 - SHELF1_SHIFT);
            p[i] = x + e;
        }
    }
}

void eq_low_shelf1_process(struct dsp_filter *f, int32_t * const buf[],
                           int count, unsigned int channels)
{
    shelf1_process(f, buf, count, channels, true, false);
}

void eq_high_shelf1_process(struct dsp_filter *f, int32_t * const buf[],
                            int count, unsigned int channels)
{
    shelf1_process(f, buf, count, channels, false, false);
}

void eq_high_shelf1_gain_process(struct dsp_filter *f, int32_t * const buf[],
                                 int count, unsigned int channels)
{
    shelf1_process(f, buf, count, channels, false, true);
}
#endif /* CPU */
