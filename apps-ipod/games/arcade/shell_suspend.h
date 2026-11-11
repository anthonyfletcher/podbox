/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to shell_suspend.c: a cart suspended on Quit and offered again
 * at its next start.
 ****************************************************************************/
#ifndef SHELL_SUSPEND_H
#define SHELL_SUSPEND_H

#include <stdbool.h>
#include <stdint.h>
#include "games/arcade/arcade.h"

/* Quit from the pause menu: the game written out, unless it owns the
 * music or its cart.json says "suspend": false */
void shell_suspend_write(struct vm *vm);

/* A cart just loaded into mem: when it was suspended in this build, it is
 * resumed, for the owner to be asked over it whether to carry on. Returns
 * the cart to run -- the one loaded, or the resumed one with *resumed set
 * -- or NULL when the arena was overwritten without making a game, to be
 * loaded again. The file is gone after either way. */
struct vm *shell_suspend_offer(struct vm *vm, void *mem, uint32_t size,
                               const struct arcade_host *host,
                               bool *resumed);

#endif
