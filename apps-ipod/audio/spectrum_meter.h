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

/* Band centre frequencies, log-spaced ~60Hz to 12kHz, lowest first. A
 * SPECTRUM_BLOCK_SIZE window resolves samplerate/256 (~172Hz at 44.1kHz),
 * so the two lowest entries sit inside one resolution cell and read partly
 * as each other: distinct, but not independent. */
extern const int spectrum_band_freq_hz[SPECTRUM_MAX_BANDS];

/* Recomputes all band levels from the current playback PCM buffer. Meant
 * to be called every tick from skin_wait_for_action(), the same way
 * peak_meter_peek() is. Cheap no-op if too little fresh audio data is
 * available since the last call. */
void spectrum_meter_peek(void);

/* Returns a 0-100 smoothed level for bar 'bar' (0-based) out of 'nbars'
 * total bars, averaged across the two channels. 'nbars' is clamped to
 * SPECTRUM_MAX_BANDS. */
int spectrum_meter_get_bar(int bar, int nbars);

/* The same level for one channel alone -- 0 is left, 1 is right. A stereo
 * layout reads the two banks apart so its halves differ with the mix. */
int spectrum_meter_get_bar_channel(int bar, int nbars, int channel);

/* Goertzel magnitude of 'freq_hz' within 'count' samples taken every
 * 'stride' entries of 'samples', for an output rate of 'samplerate' Hz. The
 * stride filters one channel of an interleaved buffer where it lies, with
 * no de-interleaving copy. Roughly amplitude-scaled: a loud on-frequency
 * signal reaches ~46000. */
int spectrum_goertzel_magnitude(const int16_t *samples, int count, int stride,
                                int freq_hz, int samplerate);

/* Compress a raw magnitude to a 0-100 display level, log-like, so quiet
 * passages still move instead of only the loudest band lighting up. */
int spectrum_scale_to_level(int raw);

#endif /* __SPECTRUM_METER_H__ */
