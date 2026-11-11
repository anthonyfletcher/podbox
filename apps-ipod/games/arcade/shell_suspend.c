/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Suspending a cart on the player. Quitting from the pause menu writes the
 * whole arena beside the cart's save, as /.rockbox/games/saves/<id>.sus;
 * the next start resumes it and asks, over the game, whether to carry on
 * from it (shell.c); either way the file is gone after, so a game is
 * resumed at most once from one Quit.
 *
 * The arena holds addresses in this firmware build (see arcade_snapshot),
 * so the file opens with what makes them hold -- where the engine's code
 * and the arena are, the arena's size, and the cart's hash -- and a file
 * whose marks differ is not offered.
 ****************************************************************************/
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "games/arcade/arcade.h"
#include "games/arcade/shell.h"
#include "games/arcade/shell_suspend.h"

struct head
{
    char magic[4];
    uint32_t size, hash, spare;
    uint64_t code, arena;
};

static void suspend_path(const struct vm *vm, char *path, size_t size)
{
    shell_id_path(path, size, SAVE_DIR, arcade_id(vm), ".sus");
}

/* What a snapshot of this cart, in this build, at this arena carries */
static void head_of(const struct vm *vm, const void *mem, uint32_t size,
                    struct head *h)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "PBSS", 4);
    h->size = size;
    h->hash = arcade_cart_hash(vm);
    h->code = (uint64_t)(uintptr_t)arcade_tick;
    h->arena = (uint64_t)(uintptr_t)mem;
}

/* Written beside any old one and renamed over it, so a full disk leaves
 * nothing half-written to be offered */
void shell_suspend_write(struct vm *vm)
{
    char path[MAX_PATH];
    uint32_t size;
    const void *mem = arcade_snapshot(vm, &size);
    struct head h;
    if (arcade_owns_music(vm) || !arcade_suspends(vm))
        return;
    head_of(vm, mem, size, &h);
    suspend_path(vm, path, sizeof(path));
    shell_write_file(SAVE_DIR, path, &h, sizeof(h), mem, size);
}

struct vm *shell_suspend_offer(struct vm *vm, void *mem, uint32_t size,
                               const struct arcade_host *host,
                               bool *resumed)
{
    char path[MAX_PATH];
    struct head h, want;
    *resumed = false;
    suspend_path(vm, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return vm;
    head_of(vm, mem, size, &want);
    bool fits = read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h) &&
                !memcmp(&h, &want, sizeof(h));
    struct vm *back = NULL;
    if (fits && read(fd, mem, size) == (ssize_t)size)
        back = arcade_resume(mem, size, host);
    close(fd);
    remove(path);
    if (back)
        *resumed = true;
    /* NULL when the arena was overwritten and is not a game: the caller
     * loads the cart afresh */
    return back ? back : fits ? NULL : vm;
}
