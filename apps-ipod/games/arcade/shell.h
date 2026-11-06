/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to shell.c: running an Arcade cart on the player.
 ****************************************************************************/
#ifndef ARCADE_SHELL_H
#define ARCADE_SHELL_H

#include <stdbool.h>

/* Plays the cart at path until the player quits it. Returns GO_TO_PREVIOUS,
 * or GO_TO_ROOT when USB took the screen. */
int arcade_play(const char *path);

struct simplelist_info;

/* One of the shell's lists, shown as a menu over the game: the skin's
 * context-menu rows, which leave no room for icons. As
 * simplelist_show_list(): true when the owner went out to the root. */
bool shell_list(struct simplelist_info *info);

/* Around a dialog a cart opens (ui.*): the screen handed back to the
 * player's own, and taken again after; to_root when the owner went out to
 * the root, which ends the cart once its tick is over. */
void shell_dialog_begin(void);
void shell_dialog_end(bool to_root);

#endif /* ARCADE_SHELL_H */
