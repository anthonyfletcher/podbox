/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Spectrum analyser bars for the skin. Runs a Goertzel filter bank over
 * recent PCM samples and exposes per-bar levels.
 ****************************************************************************/

/* Lightweight real-time spectrum band levels for the WPS %Sb tag.
 *
 * Modeled on apps/recorder/peakmeter.c's data flow (peek the live mixer
 * buffer, keep smoothed state, let the skin engine poll it), but computes
 * a handful of discrete frequency-band magnitudes via the Goertzel
 * algorithm instead of a single overall peak. Deliberately not a full FFT
 * (see apps/plugins/fft/fft.c, which needs a worker thread and CPU boost
 * to stay off the UI thread at 1024+-point transforms): Goertzel cost
 * scales with block size x band count, not transform size, so a handful
 * of bands stays cheap enough to run continuously on the UI thread the
 * same way peakmeter already does.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "pcm.h"
#include "pcm_mixer.h"
#include "fixedpoint.h"
#include "spectrum_meter.h"

/* Samples per channel analyzed per update. Small enough to stay cheap,
 * large enough to resolve the lowest band frequency reasonably. */
#define SPECTRUM_BLOCK_SIZE 256

/* Band center frequencies, log-spaced ~60Hz-12kHz (bass to treble).
 * bar_to_band() picks evenly-spaced entries from this table when fewer
 * than SPECTRUM_MAX_BANDS bars are requested.
 *
 * These are analysed exactly, but a SPECTRUM_BLOCK_SIZE window resolves
 * samplerate/SPECTRUM_BLOCK_SIZE (~172Hz at 44.1kHz), so the lowest bands
 * sit well inside one resolution cell and read partly as each other. They
 * are distinct, not independent; separating them needs a longer block. */
static const int band_freq_hz[SPECTRUM_MAX_BANDS] =
{
    60, 150, 400, 1000, 2500, 4000, 7000, 12000
};

/* Smoothed 0-100 level per band, held apart for each channel (0 = left,
 * 1 = right). Both banks always run: a stereo display reads them apart,
 * and the mono one averages them. */
static int spectrum_level[2][SPECTRUM_MAX_BANDS];

/* Instant attack, exponential release (divide the gap by 2^shift each
 * update) -- mirrors typical VU meter ballistics. */
#define SPECTRUM_RELEASE_SHIFT 3

/* Rough perceptual (log-like) compression range: ln(raw magnitude) below
 * SPECTRUM_LOG_MIN maps to level 0, above SPECTRUM_LOG_MAX maps to level
 * 100. Now that goertzel_magnitude() normalizes back to an amplitude-
 * comparable scale (0-32767ish), these are calibrated against that range:
 * ln(50)=~4 (near-silent noise floor) and ln(20000)=~10 (a hot but not
 * necessarily full-scale signal reaches 100, giving some headroom rather
 * than requiring literal 0dBFS). Starting-point constants pending
 * on-device tuning by ear/eye. */
#define SPECTRUM_LOG_MIN (4L << 16)
#define SPECTRUM_LOG_MAX (10L << 16)

/* Goertzel magnitude of 'freq_hz' within 'count' samples taken every
 * 'stride' entries of 'samples', for a mixer output rate of 'samplerate'
 * Hz. The stride is what lets one channel of an interleaved stereo buffer
 * be filtered where it lies, with no de-interleaving copy. Fixed point
 * throughout, with the filter coefficient 2*cos(2*pi*freq_hz/samplerate)
 * held in Q29.
 *
 * The precision is load-bearing, so resist simplifying it back to a
 * coarser angle: cos() flattens out near 0 Hz, so a small error in the
 * coefficient is a large error in the frequency it actually tunes to.
 * At 60Hz a Q14 coefficient steps in jumps of ~40Hz, and fp14_cos()'s
 * whole degrees are worse still -- 1 degree is 122Hz here, which puts
 * the two lowest bands on the same coefficient and draws them identically.
 * Q29 rather than more because it is the highest that keeps 2*cos inside
 * 32 bits, so both operands of the inner multiply stay the width they
 * were at Q14 and only the product needs 64. */
static int goertzel_magnitude(const int16_t *samples, int count, int stride,
                              int freq_hz, int samplerate)
{
    unsigned long phase = (unsigned long)
                          (((uint64_t)freq_hz << 32) / samplerate);
    long cos_s31, coeff_q29;
    long q1 = 0, q2 = 0;
    long long mag_sq;
    int i;

    /* fp_sincos() returns cos in s0.31, i.e. 2*cos already in Q30. */
    fp_sincos(phase, &cos_s31);
    coeff_q29 = cos_s31 >> 1;

    for (i = 0; i < count; i++)
    {
        long q0 = (long)(((long long)coeff_q29 * q1) >> 29) - q2
                + samples[i * stride];
        q2 = q1;
        q1 = q0;
    }

    /* Q1/Q2 grow proportional to (count/2 * amplitude) for a tone at the
     * target frequency -- e.g. at count=256 a full-scale (32767) signal
     * pushes them to roughly 128x that. Divide that gain back out here, or
     * mag_sq saturates the clamp below at a small fraction of full scale and
     * every bar pegs at maximum instead of tracking loudness. */
    q1 /= (count / 2);
    q2 /= (count / 2);

    mag_sq = (long long)q1 * q1 + (long long)q2 * q2
           - (((long long)coeff_q29 * q1) >> 29) * q2;
    if (mag_sq < 0)
        mag_sq = 0; /* rounding can occasionally push this slightly negative */
    if (mag_sq > 0x7fffffffLL)
        mag_sq = 0x7fffffffLL;

    return (int)fp_sqrt((long)mag_sq, 0);
}

/* Maps a raw Goertzel magnitude to a 0-100 display level with a rough
 * log-like compression, so quiet passages still show visible movement
 * instead of just the loudest band lighting up. */
static int scale_to_level(int raw)
{
    long logval;
    int level;

    if (raw <= 0)
        return 0;
    /* raw can reach ~46000 for a loud on-frequency signal (see
     * goertzel_magnitude's mag_sq clamp); "raw << 16" must stay inside
     * signed 32-bit range for fp16_log's Q16 input. */
    if (raw > 32767)
        raw = 32767;

    logval = fp16_log(raw << 16);
    if (logval <= SPECTRUM_LOG_MIN)
        return 0;
    if (logval >= SPECTRUM_LOG_MAX)
        return 100;

    level = (int)(((logval - SPECTRUM_LOG_MIN) * 100)
                  / (SPECTRUM_LOG_MAX - SPECTRUM_LOG_MIN));
    return level;
}

/* Move one band toward `level`: instant attack, exponential release. */
static void approach_level(int channel, int band, int level)
{
    if (level > spectrum_level[channel][band])
        spectrum_level[channel][band] = level; /* instant attack */
    else if (spectrum_level[channel][band] > level)
    {
        /* >> SPECTRUM_RELEASE_SHIFT truncates to 0 once the remaining
         * gap drops below 8, which would otherwise freeze the level a
         * few points short of the true (quieter) target forever.
         * Guarantee at least 1 unit of decay per update so it always
         * reaches the target. */
        int decay = (spectrum_level[channel][band] - level)
                    >> SPECTRUM_RELEASE_SHIFT;
        if (decay < 1)
            decay = 1;
        spectrum_level[channel][band] -= decay;
    }
}

void spectrum_meter_peek(void)
{
    int count;
    const int16_t *pcm = mixer_channel_get_buffer(PCM_MIXER_CHAN_PLAYBACK, &count);
    int samplerate = mixer_get_frequency();
    int channel, band;

    if (!pcm || count < SPECTRUM_BLOCK_SIZE || samplerate <= 0)
    {
        /* No audio to analyse: paused, stopped, or a tick with too little
         * fresh data. Fall toward silence at the release rate rather than
         * holding the last frame -- otherwise the bars stop dead at whatever
         * height the final block left them, and stay there. Playback resuming
         * is caught on the next tick by the instant attack, so a lull that
         * turns out to be momentary costs a unit or two of height. */
        for (channel = 0; channel < 2; channel++)
            for (band = 0; band < SPECTRUM_MAX_BANDS; band++)
                approach_level(channel, band, 0);
        return;
    }

    /* 'count' is frames, so the buffer holds two interleaved int16 per frame
     * and channel N is every second sample starting at offset N. */
    for (channel = 0; channel < 2; channel++)
    {
        for (band = 0; band < SPECTRUM_MAX_BANDS; band++)
        {
            int raw = goertzel_magnitude(pcm + channel, SPECTRUM_BLOCK_SIZE, 2,
                                         band_freq_hz[band], samplerate);

            approach_level(channel, band, scale_to_level(raw));
        }
    }
}

/* Which band table entry display bar 'bar' of 'nbars' reads, or -1 when the
 * bar is out of range. Fewer bars than bands pick evenly-spaced entries. */
static int bar_to_band(int bar, int nbars)
{
    int band;

    if (nbars <= 0)
        return -1;
    if (nbars > SPECTRUM_MAX_BANDS)
        nbars = SPECTRUM_MAX_BANDS;
    if (bar < 0 || bar >= nbars)
        return -1;

    band = (bar * SPECTRUM_MAX_BANDS) / nbars;
    if (band >= SPECTRUM_MAX_BANDS)
        band = SPECTRUM_MAX_BANDS - 1;

    return band;
}

int spectrum_meter_get_bar(int bar, int nbars)
{
    int band = bar_to_band(bar, nbars);

    if (band < 0)
        return 0;

    return (spectrum_level[0][band] + spectrum_level[1][band]) / 2;
}

int spectrum_meter_get_bar_channel(int bar, int nbars, int channel)
{
    int band = bar_to_band(bar, nbars);

    if (band < 0 || channel < 0 || channel > 1)
        return 0;

    return spectrum_level[channel][band];
}
