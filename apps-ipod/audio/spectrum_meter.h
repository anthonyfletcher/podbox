/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to spectrum_meter.c.
 ****************************************************************************/

#ifndef __SPECTRUM_METER_H__
#define __SPECTRUM_METER_H__

#define SPECTRUM_FPS 10
#define SPECTRUM_MAX_BANDS 8

/* Recomputes all band levels from the current playback PCM buffer. Meant
 * to be called every tick from skin_wait_for_action(), the same way
 * peak_meter_peek() is. Cheap no-op if too little fresh audio data is
 * available since the last call. */
void spectrum_meter_peek(void);

/* Returns a 0-100 smoothed level for bar 'bar' (0-based) out of 'nbars'
 * total bars. 'nbars' is clamped to SPECTRUM_MAX_BANDS. */
int spectrum_meter_get_bar(int bar, int nbars);

#endif /* __SPECTRUM_METER_H__ */
