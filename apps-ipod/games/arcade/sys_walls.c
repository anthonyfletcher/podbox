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
#include <stddef.h>
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)

#define MAX_DOORS 64

/* walls.draw(solid, [height], [far], [dark], [doors]): the walls of map
 * cells whose flags share a bit with solid, height world pixels tall (8, a
 * cell: cubes) and as far as far world pixels (512). dark, an array of
 * colours, is what each colour becomes on the faces that run along y, so
 * corners show; plane.fog() hazes the walls as it does the ground. doors,
 * three numbers a door (its cell's column and row, and how open it is, 0
 * to 1), stands a panel across the middle of each such wall cell that
 * slides aside as it opens, the way past it clear. Returns the columns
 * that met a wall. */
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
    struct wallspec ws = { floor_int(a[0]), height, far, NULL, NULL, 0 };
    if (argc > 3 && a[3].tag != T_NULL) {
        static uint8_t dark[256];
        struct column c;
        if (!column_of(vm, a[3], &c) || c.tag == T_INT)
            return vm_fail(vm, "walls.draw() needs dark as an array of "
                           "colours", "", "");
        for (uint32_t i = 0; i < 256; i++)
            dark[i] = i < c.len ? (uint8_t)column_at(&c, i, (int32_t)i)
                                : (uint8_t)i;
        ws.dark = dark;
    }
    if (argc > 4 && a[4].tag != T_NULL) {
        static int32_t doors[MAX_DOORS * 3];
        struct column c;
        if (!column_of(vm, a[4], &c) || c.tag == T_INT)
            return vm_fail(vm, "walls.draw() needs doors as an array, three "
                           "numbers a door", "", "");
        ws.ndoors = (int32_t)(c.len / 3 < MAX_DOORS ? c.len / 3 : MAX_DOORS);
        for (int32_t i = 0; i < ws.ndoors; i++) {
            doors[i * 3] = column_at(&c, (uint32_t)i * 3, -1);
            doors[i * 3 + 1] = column_at(&c, (uint32_t)i * 3 + 1, -1);
            doors[i * 3 + 2] = column_fix(&c, (uint32_t)i * 3 + 2);
        }
        ws.doors = doors;
    }
    *out = mk(T_INT, (int32_t)core_draw_walls(vm, &ws));
    return 1;
}
