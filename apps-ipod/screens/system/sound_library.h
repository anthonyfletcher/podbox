/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to sound_library.c: what the analysis found across the library.
 ****************************************************************************/

#ifndef _SOUND_LIBRARY_H
#define _SOUND_LIBRARY_H

#include <stdbool.h>

/* True where the screen was left for the root, as every screen reached from a
 * menu reports it. */
bool sound_library_screen(void);

#endif /* _SOUND_LIBRARY_H */
