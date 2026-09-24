/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to spectrum_meter.c.
 ****************************************************************************/

#ifndef __SPECTRUM_METER_H__
#define __SPECTRUM_METER_H__

#include <stdint.h>

#define SPECTRUM_FPS 10
#define SPECTRUM_MAX_BANDS 8

/* Band centre frequencies, log-spaced ~60Hz to 12kHz, lowest first. The two
 * lowest are read over a longer window than the rest, which is what makes
 * them independent of each other -- see spectrum_meter_peek(). */
extern const int spectrum_band_freq_hz[SPECTRUM_MAX_BANDS];

/* Recomputes band levels from the current playback PCM buffer. Meant to be
 * called every tick from skin_wait_for_action(), the same way
 * peak_meter_peek() is.
 *
 * The two lowest bands need four times as many frames as the rest and the
 * mixer does not always have them, so they update on roughly half the calls
 * and hold their level in between. Everything falls toward silence when
 * there is too little audio for even a short window. */
void spectrum_meter_peek(void);

/* Returns a 0-100 smoothed level for bar 'bar' (0-based) out of 'nbars'
 * total bars, averaged across the two channels. 'nbars' is clamped to
 * SPECTRUM_MAX_BANDS. */
int spectrum_meter_get_bar(int bar, int nbars);

/* The same level for one channel alone -- 0 is left, 1 is right. A stereo
 * layout reads the two banks apart so its halves differ with the mix. */
int spectrum_meter_get_bar_channel(int bar, int nbars, int channel);

/* The peak cap for the same bar: the highest level it has reached lately,
 * held for half a second and then released downward at an accelerating
 * rate. Never reads below the level the bar is drawn at, so a cap always
 * marks its own bar or sits above it. */
int spectrum_meter_get_peak(int bar, int nbars);
int spectrum_meter_get_peak_channel(int bar, int nbars, int channel);

/* The Q29 filter coefficient for one frequency at one rate.
 *
 * About a thousand cycles on the 5G -- a 64-bit divide and a CORDIC -- so a
 * caller filtering a fixed set of frequencies builds a table of these once
 * when the rate changes and passes them to spectrum_goertzel_at(), rather
 * than paying it again for every window. */
long spectrum_goertzel_coeff(int freq_hz, int samplerate);

/* Goertzel magnitude at a precomputed coefficient, within 'count' samples
 * taken every 'stride' entries of 'samples'. The stride filters one channel
 * of an interleaved buffer where it lies, with no de-interleaving copy.
 * Roughly amplitude-scaled: a loud on-frequency signal reaches ~46000. */
int ICODE_ATTR spectrum_goertzel_at(const int16_t *samples, int count,
                                    int stride, long coeff_q29);

/* Compress a raw magnitude to a 0-100 display level, log-like, so quiet
 * passages still move instead of only the loudest band lighting up. */
int spectrum_scale_to_level(int raw);

#endif /* __SPECTRUM_METER_H__ */
