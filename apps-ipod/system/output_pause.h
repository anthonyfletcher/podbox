/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Pause on Unplug: playback pauses when the output it was using goes -- the
 * headphones, a USB DAC, a dock or car -- and, set to resume, carries on
 * when that same output comes back.
 ****************************************************************************/
#ifndef _OUTPUT_PAUSE_H
#define _OUTPUT_PAUSE_H

#include <stdbool.h>

void output_pause_init(void);
/* UI thread: headphones in or out */
void output_pause_headphones(bool inserted);
/* UI thread: SYS_OUTPUT_RESUME */
void output_pause_resume(void);

#endif
