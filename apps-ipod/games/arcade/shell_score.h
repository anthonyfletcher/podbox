/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to shell_score.c: a cart's high scores on the player -- the
 * file they are kept in, and the screens that show them.
 ****************************************************************************/
#ifndef SHELL_SCORE_H
#define SHELL_SCORE_H

#include <stdbool.h>

struct vm;

/* A cart starting: its tables read in, and one more play counted */
void shell_scores_load(struct vm *vm);
/* The cart's time running starts again (after loading, and on resuming
 * from a pause), and stops (on pausing) */
void shell_scores_run(bool running);
/* Written out when a score changed, or always when the cart is leaving;
 * a refused write is tried again next time */
void shell_scores_write(struct vm *vm, bool leaving);

/* The pause menu's High Scores, and score.show()'s table, with rank (1-10,
 * 0 none) marked. Both are lists drawn by the player, so the shell hands
 * the screen back first; true when the owner went out to the root. */
bool shell_scores_menu(struct vm *vm);
bool shell_score_table(struct vm *vm, int board, int rank);

/* Each cart's file is here, <id>.sco */
#define SCORES_DIR "/.rockbox/games/scores"

/* A cart's record from its file, without the cart: false when there is
 * none to read. The same High Scores menu for it, Clear writing the file
 * back; true when the owner went out to the root. */
struct arcade_scores;
bool shell_scores_read(const char *path, struct arcade_scores *s);
bool shell_scores_show(struct arcade_scores *s);

#endif
