/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A cart's high scores on the player. Each cart's are one file under
 * /.rockbox/games/scores, named by its id, in the engine's own record
 * (arcade_scores_pack), which names the cart and titles its boards so a
 * list of every cart's can be made without opening the carts. The file
 * also counts the times the cart was started and the time it ran.
 *
 * Parts, in order:
 *   - the file
 *   - time played
 *   - the screens: a board's table, the pause menu's High Scores
 ****************************************************************************/
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"                 /* current_tick */
#include "file.h"
#include "lang.h"
#include "widgets/list.h"
#include "widgets/dialog_prose.h"
#include "games/arcade/arcade.h"
#include "games/arcade/shell.h"
#include "games/arcade/shell_score.h"

#define SCORES_DIR "/.rockbox/games/scores"

/* ---- the file ------------------------------------------------------------ */

/* A write the disk refused, tried again at the next chance */
static bool scores_owed;

static void scores_path(const uint8_t *id, char *path, size_t size)
{
    shell_id_path(path, size, SCORES_DIR, id, ".sco");
}

/* The record in and out; one at a time, so one buffer serves */
static uint8_t record[ARCADE_SCORES_MAX];

/* the cart's time running: since when (-1 paused), and the ticks not yet
 * counted as a whole second */
static long run_from = -1, run_ticks;

void shell_scores_load(struct vm *vm)
{
    static struct arcade_scores saved;
    char path[MAX_PATH];
    struct arcade_scores *s = arcade_scores(vm, NULL);
    scores_owed = false;
    run_from = -1;
    run_ticks = 0;
    scores_path(s->id, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, record, sizeof(record));
        close(fd);
        if (n > 0 && arcade_scores_unpack(&saved, record, (int)n))
            arcade_scores_restore(vm, &saved);
    }
    s->plays++;
}

void shell_scores_run(bool running)
{
    if (running) {
        run_from = current_tick;
        return;
    }
    if (run_from >= 0)
        run_ticks += current_tick - run_from;
    run_from = -1;
}

/* Written beside the old file and renamed over it, as the save is */
void shell_scores_write(struct vm *vm, bool leaving)
{
    int dirty;
    struct arcade_scores *s = arcade_scores(vm, &dirty);
    if (dirty)
        scores_owed = true;
    if (!scores_owed && !leaving)
        return;
    /* time played so far, whole seconds; the rest waits for the next */
    if (run_from >= 0) {
        run_ticks += current_tick - run_from;
        run_from = current_tick;
    }
    s->seconds += (uint32_t)(run_ticks / HZ);
    run_ticks %= HZ;
    scores_owed = true;
    int n = arcade_scores_pack(s, record, sizeof(record));
    if (n <= 0)
        return;
    char path[MAX_PATH];
    scores_path(s->id, path, sizeof(path));
    if (shell_write_file(SCORES_DIR, path, record, (size_t)n, NULL, 0))
        scores_owed = false;
}

/* ---- the screens --------------------------------------------------------- */

struct table
{
    const struct arcade_scores *s;
    int board;
};

/* "1. 12345  NAME  2026-11-06": rank, value, name when one was given, and
 * the date when known */
static const char *table_row(int row, void *data, char *buf, size_t len)
{
    const struct table *t = data;
    const struct arcade_board *b = &t->s->b[t->board];
    if (!b->count)
        return str(LANG_NO_HIGH_SCORES);
    const struct arcade_entry *e = &b->e[row];
    char value[32], date[16] = "";
    arcade_score_text(t->s, t->board, e->value, value, sizeof(value));
    if (e->date)
        snprintf(date, sizeof(date), "%04ld-%02ld-%02ld",
                 (long)(e->date / 10000), (long)(e->date / 100 % 100),
                 (long)(e->date % 100));
    snprintf(buf, len, "%d. %s%s%s%s%s", row + 1, value,
             e->name[0] ? "  " : "", e->name, date[0] ? "  " : "", date);
    return buf;
}

bool shell_score_table(struct vm *vm, int board, int rank)
{
    struct table t = { arcade_scores(vm, NULL), board };
    const struct arcade_board *b = &t.s->b[board];
    struct simplelist_info info;
    simplelist_info_init(&info, (char *)b->title, b->count ? b->count : 1,
                         &t);
    info.get_name = table_row;
    info.selection = rank > 0 ? rank - 1 : 0;
    return shell_list(&info);
}

/* ---- achievements ------------------------------------------------------- */

static const char *ach_row(int row, void *data, char *buf, size_t len)
{
    const struct arcade_scores *s = data;
    const struct arcade_achievement *a = &s->ach[row];
    if (a->date)
        return a->title;
    snprintf(buf, len, str(LANG_ACH_LOCKED), a->title);
    return buf;
}

/* Each achievement, and chosen, its text and when it was earned in the
 * box Help uses */
static bool achievements_menu(const struct arcade_scores *s)
{
    static char body[160];
    struct simplelist_info info;
    int selection = 0;
    for (;;) {
        simplelist_info_init(&info, str(LANG_ACHIEVEMENTS), s->nach,
                             (void *)s);
        info.get_name = ach_row;
        info.selection = selection;
        if (shell_list(&info))
            return true;
        selection = info.selection;
        if (selection < 0)
            return false;
        const struct arcade_achievement *a = &s->ach[selection];
        char when[48];
        if (a->date > 1)
            snprintf(when, sizeof(when), str(LANG_ACH_EARNED_ON),
                     (int)(a->date / 10000), (int)(a->date / 100 % 100),
                     (int)(a->date % 100));
        else
            snprintf(when, sizeof(when), "%s",
                     str(a->date ? LANG_ACH_EARNED : LANG_ACH_NOT_YET));
        snprintf(body, sizeof(body), "%s%s%s", a->text,
                 a->text[0] ? "\n\n" : "", when);
        if (dialog_prose_show(a->title, body))
            return true;
    }
}

/* ---- the menu ------------------------------------------------------------ */

/* The boards, the achievements, Clear High Scores (when there are boards),
 * then how often and how long the cart was played */
enum { M_BOARD, M_ACH, M_CLEAR, M_TIMES, M_TIME };

static int row_is(const struct arcade_scores *s, int row, int *board)
{
    *board = row;
    if (row < s->count)
        return M_BOARD;
    row -= s->count;
    if (s->nach && row-- == 0)
        return M_ACH;
    if (s->count && row-- == 0)
        return M_CLEAR;
    return row == 0 ? M_TIMES : M_TIME;
}

static int rows_of(const struct arcade_scores *s)
{
    return s->count + (s->nach > 0) + (s->count > 0) + 2;
}

static const char *menu_row(int row, void *data, char *buf, size_t len)
{
    const struct arcade_scores *s = data;
    int earned = 0, board;
    switch (row_is(s, row, &board)) {
    case M_BOARD:
        return s->b[board].title;
    case M_ACH:
        for (int i = 0; i < s->nach; i++)
            earned += s->ach[i].date != 0;
        snprintf(buf, len, str(LANG_ACH_COUNT), earned, s->nach);
        return buf;
    case M_CLEAR:
        return str(LANG_CLEAR_HIGH_SCORES);
    case M_TIMES:
        snprintf(buf, len, str(LANG_TIMES_PLAYED), (int)s->plays);
        return buf;
    default:
        snprintf(buf, len, str(LANG_TIME_PLAYED), (int)(s->seconds / 3600),
                 (int)(s->seconds / 60 % 60));
        return buf;
    }
}

bool shell_scores_menu(struct vm *vm)
{
    struct arcade_scores *s = arcade_scores(vm, NULL);
    struct simplelist_info info;
    int selection = 0, board;
    for (;;) {
        simplelist_info_init(&info, str(LANG_HIGH_SCORES), rows_of(s), s);
        info.get_name = menu_row;
        info.selection = selection;
        if (shell_list(&info))
            return true;
        selection = info.selection;
        if (selection < 0)
            return false;
        switch (row_is(s, selection, &board)) {
        case M_BOARD:
            if (shell_score_table(vm, board, 0))
                return true;
            break;
        case M_ACH:
            if (achievements_menu(s))
                return true;
            break;
        case M_CLEAR:
            if (dialog_prose_confirm(str(LANG_CLEAR_HIGH_SCORES),
                                     str(LANG_CLEAR_HIGH_SCORES_ASK),
                                     str(LANG_SET_BOOL_YES),
                                     str(LANG_SET_BOOL_NO), false)) {
                for (int i = 0; i < s->count; i++)
                    s->b[i].count = 0;
                scores_owed = true;
                shell_scores_write(vm, false);
            }
            break;
        }
    }
}
