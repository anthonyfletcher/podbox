/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to browser_flat.c.
 ****************************************************************************/
#ifndef _BROWSER_FLAT_H
#define _BROWSER_FLAT_H

#include <stdbool.h>

/* Every image on the player when 'images' is set, every document otherwise,
 * in one flat list. Owns the screen until the user leaves. Returns a GO_TO_*
 * code. */
int browser_flat(bool images);

#endif /* _BROWSER_FLAT_H */
