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

/* Mono samples analyzed per update. Small enough to stay cheap, large
 * enough to resolve the lowest band frequency reasonably. */
#define SPECTRUM_BLOCK_SIZE 256

/* Band center frequencies, log-spaced ~60Hz-12kHz (bass to treble).
 * spectrum_meter_get_bar() picks evenly-spaced entries from this table
 * when fewer than SPECTRUM_MAX_BANDS bars are requested. */
static const int band_freq_hz[SPECTRUM_MAX_BANDS] =
{
    60, 150, 400, 1000, 2500, 4000, 7000, 12000
};

/* Smoothed 0-100 level per band. */
static int spectrum_level[SPECTRUM_MAX_BANDS];

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

/* Goertzel magnitude of 'freq_hz' within 'count' mono samples, for a
 * mixer output rate of 'samplerate' Hz. Fixed point throughout (Q14 via
 * fp14_cos, same convention apps/plugins/fft/fft.c uses for fp_sqrt). */
static int goertzel_magnitude(const int16_t *samples, int count,
                              int freq_hz, int samplerate)
{
    int k = (int)(((long long)count * freq_hz) / samplerate);
    int angle_deg = (int)(((long long)k * 360) / count);
    long coeff_q14 = 2 * fp14_cos(angle_deg);
    long q1 = 0, q2 = 0;
    long long mag_sq;
    int i;

    for (i = 0; i < count; i++)
    {
        long q0 = (long)(((long long)coeff_q14 * q1) >> 14) - q2 + samples[i];
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
           - (((long long)coeff_q14 * q1) >> 14) * q2;
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
static void approach_level(int band, int level)
{
    if (level > spectrum_level[band])
        spectrum_level[band] = level; /* instant attack */
    else if (spectrum_level[band] > level)
    {
        /* >> SPECTRUM_RELEASE_SHIFT truncates to 0 once the remaining
         * gap drops below 8, which would otherwise freeze the level a
         * few points short of the true (quieter) target forever.
         * Guarantee at least 1 unit of decay per update so it always
         * reaches the target. */
        int decay = (spectrum_level[band] - level) >> SPECTRUM_RELEASE_SHIFT;
        if (decay < 1)
            decay = 1;
        spectrum_level[band] -= decay;
    }
}

void spectrum_meter_peek(void)
{
    int count;
    const int16_t *pcm = mixer_channel_get_buffer(PCM_MIXER_CHAN_PLAYBACK, &count);
    int16_t mono[SPECTRUM_BLOCK_SIZE];
    int samplerate = mixer_get_frequency();
    int band, i;

    if (!pcm || count < SPECTRUM_BLOCK_SIZE || samplerate <= 0)
    {
        /* No audio to analyse: paused, stopped, or a tick with too little
         * fresh data. Fall toward silence at the release rate rather than
         * holding the last frame -- otherwise the bars stop dead at whatever
         * height the final block left them, and stay there. Playback resuming
         * is caught on the next tick by the instant attack, so a lull that
         * turns out to be momentary costs a unit or two of height. */
        for (band = 0; band < SPECTRUM_MAX_BANDS; band++)
            approach_level(band, 0);
        return;
    }

    for (i = 0; i < SPECTRUM_BLOCK_SIZE; i++)
        mono[i] = (int16_t)(((int)pcm[2 * i] + (int)pcm[2 * i + 1]) >> 1);

    for (band = 0; band < SPECTRUM_MAX_BANDS; band++)
    {
        int raw = goertzel_magnitude(mono, SPECTRUM_BLOCK_SIZE,
                                     band_freq_hz[band], samplerate);

        approach_level(band, scale_to_level(raw));
    }
}

int spectrum_meter_get_bar(int bar, int nbars)
{
    int band;

    if (nbars <= 0)
        return 0;
    if (nbars > SPECTRUM_MAX_BANDS)
        nbars = SPECTRUM_MAX_BANDS;
    if (bar < 0 || bar >= nbars)
        return 0;

    band = (bar * SPECTRUM_MAX_BANDS) / nbars;
    if (band >= SPECTRUM_MAX_BANDS)
        band = SPECTRUM_MAX_BANDS - 1;

    return spectrum_level[band];
}
