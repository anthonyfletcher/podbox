/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to shell.c: running an Arcade cart on the player.
 ****************************************************************************/
#ifndef ARCADE_SHELL_H
#define ARCADE_SHELL_H

/* Plays the cart at path until the player quits it. Returns GO_TO_PREVIOUS,
 * or GO_TO_ROOT when USB took the screen. */
int arcade_play(const char *path);

#endif /* ARCADE_SHELL_H */
