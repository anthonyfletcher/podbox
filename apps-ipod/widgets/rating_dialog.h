/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to rating_dialog.c: picking a rating as five stars.
 ****************************************************************************/
#ifndef _RATING_DIALOG_H_
#define _RATING_DIALOG_H_

#include <stdbool.h>

/* Ask for a rating, 0 to 10 in half stars, starting from *rating. The wheel
 * moves half a star, Select keeps it, Back leaves it as it was and holding
 * Select clears it. True when a rating was kept, which *rating then holds. */
bool rating_dialog(const char *title, int *rating);

#endif
