/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to shelf.c: the Arcade, every Arcade cart on the player.
 ****************************************************************************/
#ifndef ARCADE_SHELF_H
#define ARCADE_SHELF_H

/* The shelf until backed out of: GO_TO_PREVIOUS, or GO_TO_ROOT or
 * GO_TO_WPS when the owner went there */
int arcade_shelf_run(void);

#endif /* ARCADE_SHELF_H */
