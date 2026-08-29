/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to beat_debug.c.
 ****************************************************************************/

#ifndef BEAT_DEBUG_H
#define BEAT_DEBUG_H

#include <stdbool.h>

/* Run the analysis trace until the user leaves. True if they left for the
 * root menu, matching the rest of the debug screens. */
bool beat_debug_screen(void);

#endif /* BEAT_DEBUG_H */
