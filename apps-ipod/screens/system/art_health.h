/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to art_health.c.
 ****************************************************************************/
#ifndef _ART_HEALTH_H
#define _ART_HEALTH_H

#include <stdbool.h>

/* List the folders the artwork cache found no art for -- artist folders when
 * 'artists' is set, album folders otherwise. True if the list was left for the
 * root menu; false otherwise, which includes having nothing to show and
 * saying so. */
bool art_health_screen(bool artists);

#endif /* _ART_HEALTH_H */
