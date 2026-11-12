/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to shell.c: running an Arcade cart on the player.
 ****************************************************************************/
#ifndef ARCADE_SHELL_H
#define ARCADE_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Plays the cart at path until the player quits it. Returns GO_TO_PREVIOUS,
 * or GO_TO_ROOT when USB took the screen. */
int arcade_play(const char *path);

struct simplelist_info;

/* One of the shell's lists, shown as a menu over the game: the skin's
 * context-menu rows, which leave no room for icons. As
 * simplelist_show_list(): true when the owner went out to the root. */
bool shell_list(struct simplelist_info *info);

/* A walk of the library for a cart, yielding as it goes: the time is the
 * player's, so the stall that stops a cart not responding is timed again
 * from now */
void shell_walk_yield(void);

/* The power going off or the player restarting, with a cart open: the game
 * kept to resume, as Quit keeps it */
void shell_power_off(void);

/* Each cart's saved numbers, <id>.sav, and a game left with Quit, <id>.sus */
#define SAVE_DIR "/.rockbox/games/saves"

/* path as dir/<the cart id's sixteen hex digits><ext> */
void shell_id_path(char *path, size_t size, const char *dir,
                   const uint8_t *id, const char *ext);

/* a then b written to path beside the old file, in dir (made if missing),
 * and renamed over it: whether it was */
bool shell_write_file(const char *dir, const char *path, const void *a,
                      size_t na, const void *b, size_t nb);

#endif /* ARCADE_SHELL_H */
