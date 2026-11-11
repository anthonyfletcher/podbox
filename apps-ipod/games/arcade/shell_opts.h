/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to shell_opts.c: a cart's options and help on the player --
 * the pause menu's rows for them, and the file the owner's values are
 * kept in.
 ****************************************************************************/
#ifndef SHELL_OPTS_H
#define SHELL_OPTS_H

#include <stdbool.h>
#include <stddef.h>
#include "widgets/list.h"           /* enum list_row_kind */

struct vm;

/* A cart starting: the owner's values put back */
void shell_options_load(struct vm *vm);

/* Option i as a pause-menu row: its title, its kind, its value as text,
 * and where a number sits in its range (0-100, -1 for the others) */
const char *shell_option_title(struct vm *vm, int i);
enum list_row_kind shell_option_kind(struct vm *vm, int i);
const char *shell_option_value(struct vm *vm, int i, char *buf, size_t len);
int shell_option_range(struct vm *vm, int i);
/* Chosen: an on/off flips, the others open the player's picker; a change
 * is kept at once. True when the owner went out to the root. */
bool shell_option_edit(struct vm *vm, int i);
/* The values the game set itself with opt.set(), written where the saves
 * are, so the disk is not woken mid-game for them */
void shell_options_keep(struct vm *vm);
/* Game Sound: the game's own sound muted or not, kept with its options */
void shell_mute_toggle(struct vm *vm);

/* Whether the cart has any help, and showing it; true for out to the root */
bool shell_has_help(struct vm *vm);
bool shell_help(struct vm *vm);

#endif
