/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to shell_store.c: a cart's own stored files, as its store
 * calls reach them.
 ****************************************************************************/
#ifndef SHELL_STORE_H
#define SHELL_STORE_H

#include "games/arcade/arcade.h"

extern const struct arcade_store shell_store;

/* A cart starting: its files are the ones in its own folder */
void shell_store_start(const struct vm *vm);

#endif
