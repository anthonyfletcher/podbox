/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to sound_scan.c: the library-wide sound analysis.
 ****************************************************************************/

#ifndef SOUND_SCAN_H
#define SOUND_SCAN_H

#include <stdbool.h>

/* Measure every track in the library and write the sound index.
 *
 * Modal and slow -- hours, not minutes. Playback stops for the duration
 * because only one codec may be loaded at a time, and the screen offers
 * nothing but stopping. An unfinished run is resumed rather than restarted,
 * whichever row was chosen, so an interruption costs nothing.
 *
 * 'rebuild' discards the finished index and measures everything again. It
 * does not discard an unfinished run: a scan stopped at ninety percent of a
 * fifteen-hour job must not be restartable only from the beginning, so a
 * working file always wins and the resume prompt is where starting over is
 * chosen.
 *
 * Returns true if the screen wants the menu redrawn. */
bool sound_scan_screen(bool rebuild);

#endif /* SOUND_SCAN_H */
