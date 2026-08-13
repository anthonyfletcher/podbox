/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to settings_changed.c.
 ****************************************************************************/

#ifndef _SETTINGS_CHANGED_H_
#define _SETTINGS_CHANGED_H_

#include <stdbool.h>

/* List every setting that no longer holds its default, editable in place.
 * Returns true if USB was attached while it was open. */
bool settings_changed_screen(void);

#endif /* _SETTINGS_CHANGED_H_ */
