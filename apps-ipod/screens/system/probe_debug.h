/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to probe_debug.c.
 ****************************************************************************/

#ifndef PROBE_DEBUG_H
#define PROBE_DEBUG_H

#include <stdbool.h>

/* Measure the playing track offline and show what came back. The comparison
 * this exists for is against Debug > Beat analysis, which reads the same
 * track live: the tempo the two report should agree. */
bool probe_debug_screen(void);

#endif /* PROBE_DEBUG_H */
