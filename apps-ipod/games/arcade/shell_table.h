/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to shell_table.c: a cart's high scores as tables.
 ****************************************************************************/
#ifndef ARCADE_SHELL_TABLE_H
#define ARCADE_SHELL_TABLE_H

#include <stdbool.h>

struct arcade_scores;

/* The tables of s, a page a board, then its achievements and how much it
 * was played, turned with the wheel or Right: open at board with rank
 * (1-10, 0 none) marked. clear empties and saves the boards, NULL when the
 * owner may not. True when the owner went out to the root (MENU or USB). */
bool shell_table(struct arcade_scores *s, int board, int rank,
                 void (*clear)(struct arcade_scores *s, void *ctx), void *ctx);

/* count rows named by name, drawn as the tables are, each opened with
 * pick(row), until backed out of. pick and this are true when the owner
 * went out to the root. */
bool shell_table_list(const char *title, const char *(*name)(int row),
                      int count, bool (*pick)(int row));

#endif /* ARCADE_SHELL_TABLE_H */
