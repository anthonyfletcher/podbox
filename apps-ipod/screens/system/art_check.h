/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to art_check.c.
 ****************************************************************************/
#ifndef _ART_CHECK_H
#define _ART_CHECK_H

#include <stdbool.h>

/* Pick a file, make its art again and show how each picture was found. True
 * if the screen was left for the root menu or USB. */
bool art_check_screen(void);

#endif /* _ART_CHECK_H */
