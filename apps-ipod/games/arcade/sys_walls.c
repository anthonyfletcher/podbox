/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade systems: first-person walls -- the map's solid cells stood up as
 * walls round the plane's camera (plane.view), a maze in the old style.
 * Each wall is its cell's sprite, stretched floor to ceiling; the floor is
 * plane.draw's, drawn first, and the ceiling the cart's. Each screen
 * column's distance to its wall is kept until the next plane.view, and
 * plane.billboards draws a sprite only where it is nearer than the wall,
 * so enemies are hidden round corners. The drawing is core_draw_walls().
 ***************************************************************************/
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)

/* walls.draw(solid, [height], [far]): the walls of map cells whose flags
 * share a bit with solid, height world pixels tall (8, a cell: cubes) and
 * as far as far world pixels (512). Returns the columns that met a wall. */
NATIVE(walls_draw)
{
    if (!vm->plane.set)
        return vm_fail(vm, "walls.draw() needs a camera first: plane.view()",
                       "", "");
    if (!is_num(a[0]))
        return vm_fail(vm, "walls.draw() needs the flags of solid cells", "",
                       "");
    int32_t height = argc > 1 && is_num(a[1]) ? (int32_t)scaled(a[1]) : 8 << 16;
    int32_t far = argc > 2 && is_num(a[2]) ? floor_int(a[2]) : 512;
    if (far < 8) far = 8;
    if (far > 4096) far = 4096;
    *out = mk(T_INT, (int32_t)core_draw_walls(vm, floor_int(a[0]), height,
                                              far));
    return 1;
}
