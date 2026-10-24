/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to eq_curve.c.
 ****************************************************************************/
#ifndef _EQ_CURVE_H
#define _EQ_CURVE_H

#include <stdint.h>
#include <stdbool.h>
#include "eq.h"

/* Points across 20 Hz - 20 kHz, log-spaced. */
#define EQ_CURVE_POINTS 151

/* The response at every point, one band at a time, in hundredths of a dB.
 * Big enough to keep off the stack. */
struct eq_curve
{
    unsigned int fs;
    int32_t sin_half[EQ_CURVE_POINTS];      /* sin(w/2), s0.31 */
    int16_t band_cdb[EQ_NUM_BANDS][EQ_CURVE_POINTS];
};

/* Point frequencies for sample rate fs; every band reads flat. */
void eq_curve_init(struct eq_curve *c, unsigned int fs);
/* Recompute one band's row. */
void eq_curve_set_band(struct eq_curve *c, int band,
                       const struct eq_band_setting *s);
/* Sum of every band at a point, hundredths of a dB. */
int eq_curve_total(const struct eq_curve *c, int point);

/* Where f sits along the axis, 0 (20 Hz) to 65536 (20 kHz). */
int eq_curve_position(int hz);

/* The highest the combined response reaches, hundredths of a dB: the points
 * above plus each band's own frequency, so a narrow peak is not missed. */
int eq_curve_peak(const struct eq_band_setting *bands, unsigned int fs);
/* The same, for the bands curve c holds: only their own frequencies are
 * evaluated, which is what lets the editor keep up with the wheel. */
int eq_curve_peak_of(const struct eq_curve *c,
                     const struct eq_band_setting *bands);
/* Precut, in tenths of a dB, that keeps that peak at or below 0 dB --
 * rounded up to 0.5 dB and held within the Precut setting's range. */
int eq_curve_headroom(int peak);

/* False if the DSP's fixed-point coefficients would overflow for this band,
 * so the player would no longer play what the curve shows. */
bool eq_band_fits(const struct eq_band_setting *s, unsigned int fs);

#endif
