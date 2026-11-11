/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A cart's high scores on the player. Each cart's are one file under
 * /.rockbox/games/scores, named by its id, in the engine's own record
 * (arcade_scores_pack), which names the cart and titles its boards so a
 * list of every cart's can be made without opening the carts. The file
 * also counts the times the cart was started and the time it ran.
 *
 * The screens work from a record, so the Arcade shows a cart's high
 * scores, from its file, as the pause menu shows the running cart's.
 *
 * Parts, in order:
 *   - the file
 *   - time played
 *   - the screens, which shell_table.c draws
 ****************************************************************************/
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"                 /* current_tick */
#include "file.h"
#include "lang.h"
#include "games/arcade/arcade.h"
#include "games/arcade/shell.h"
#include "games/arcade/shell_score.h"
#include "games/arcade/shell_table.h"


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

/* A record into its file, written beside the old one and renamed over it,
 * as the save is; false when the disk refused it */
static bool write_record(const struct arcade_scores *s)
{
    int n = arcade_scores_pack(s, record, sizeof(record));
    char path[MAX_PATH];
    if (n <= 0)
        return false;
    scores_path(s->id, path, sizeof(path));
    return shell_write_file(SCORES_DIR, path, record, (size_t)n, NULL, 0);
}

bool shell_scores_read(const char *path, struct arcade_scores *s)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;
    if (fd < 0)
        return false;
    n = read(fd, record, sizeof(record));
    close(fd);
    return n > 0 && arcade_scores_unpack(s, record, (int)n);
}

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
    if (write_record(s))
        scores_owed = false;
}

/* ---- the screens -------------------------------------------------------- */

/* Clear High Scores: the boards emptied and the file written, the running
 * cart's (ctx its vm) or a record read from its file */
static void clear_boards(struct arcade_scores *s, void *ctx)
{
    struct vm *vm = ctx;
    for (int i = 0; i < s->count; i++)
        s->b[i].count = 0;
    if (vm) {
        scores_owed = true;
        shell_scores_write(vm, false);
    } else {
        write_record(s);
    }
}

bool shell_score_table(struct vm *vm, int board, int rank)
{
    return shell_table(arcade_scores(vm, NULL), board, rank, clear_boards, vm);
}

bool shell_scores_menu(struct vm *vm)
{
    return shell_table(arcade_scores(vm, NULL), 0, 0, clear_boards, vm);
}

bool shell_scores_show(struct arcade_scores *s)
{
    return shell_table(s, 0, 0, clear_boards, NULL);
}
