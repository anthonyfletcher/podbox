/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade systems: a voxel landscape -- hills to fly over and between, the
 * plane's companion, seen from the same camera (plane.view). The ground
 * is two images of one size: heights, a byte a world pixel, and colours,
 * a palette number each. Both are a power of two across and down, and the
 * world wraps, so a 128x128 landscape goes on for ever. noise.fill makes
 * both: heights from noise, colours from the same noise through a ramp.
 *
 * Parts, in order:
 *   the ground   reading the two images
 *   natives      land.draw, land.height
 ***************************************************************************/
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)

/* ---- the ground ---------------------------------------------------------- */

static int pow2(uint32_t n, uint32_t *shift)
{
    for (uint32_t s = 0; s < 16; s++)
        if (n == 1u << s) {
            *shift = s;
            return 1;
        }
    return 0;
}

/* An image a power of two each way, its pixels and size */
static int ground(struct vm *vm, struct value v, const char *fn,
                  const uint8_t **px, uint32_t *w, uint32_t *h,
                  uint32_t *wshift)
{
    uint32_t hs;
    if (v.tag != T_IMG)
        return vm_fail(vm, fn, "() needs its heights and colours as images",
                       "");
    const struct img *im = MEM(vm, (uint32_t)v.v);
    if (!pow2(im->width, wshift) || !pow2(im->height, &hs))
        return vm_fail(vm, fn, "() needs images a power of two across and "
                       "down, as 64, 128 or 256", "");
    *px = im->px;
    *w = im->width;
    *h = im->height;
    return 1;
}

static int32_t scale_of(int argc, struct value *a, int i)
{
    return argc > i && is_num(a[i]) ? (int32_t)scaled(a[i]) : 1 << 16;
}

/* ---- natives ------------------------------------------------------------- */

/* land.draw(heights, colours, [far], [scale], [coarse]): the landscape
 * from the plane's camera, as far as far world pixels (512 by default), a
 * height byte being scale world pixels (1). coarse 2 samples every other
 * column and draws each twice: about half the time, for a busy cart or
 * music on the 5G. Draws the ground only: the sky, and anything standing
 * on the ground, are the cart's, drawn before and after. Returns the
 * ground samples taken. */
NATIVE(land_draw)
{
    if (!vm->plane.set)
        return vm_fail(vm, "land.draw() needs a camera first: plane.view()",
                       "", "");
    struct landspec l;
    uint32_t w, h, cw, ch, shift, cs;
    if (!ground(vm, a[0], "land.draw", &l.height, &w, &h, &shift) ||
        !ground(vm, a[1], "land.draw", &l.colour, &cw, &ch, &cs))
        return 0;
    if (w != cw || h != ch)
        return vm_fail(vm, "land.draw() needs its heights and colours the "
                       "same size", "", "");
    l.wmask = w - 1;
    l.hmask = h - 1;
    l.wshift = shift;
    l.far = argc > 2 && is_num(a[2]) ? floor_int(a[2]) : 512;
    if (l.far < 8) l.far = 8;
    if (l.far > 4096) l.far = 4096;
    l.scale = scale_of(argc, a, 3);
    l.coarse = argc > 4 && is_num(a[4]) ? floor_int(a[4]) : 1;
    *out = mk(T_INT, (int32_t)core_draw_land(vm, &l));
    return 1;
}

/* land.height(heights, x, y, [scale]): the ground's height at world pixel
 * x, y, in world pixels -- for a craft to fly into a hill */
NATIVE(land_height)
{
    const uint8_t *px;
    uint32_t w, h, shift;
    if (!ground(vm, a[0], "land.height", &px, &w, &h, &shift))
        return 0;
    if (!is_num(a[1]) || !is_num(a[2]))
        return vm_fail(vm, "land.height() needs x and y", "", "");
    uint32_t x = (uint32_t)floor_int(a[1]) & (w - 1);
    uint32_t y = (uint32_t)floor_int(a[2]) & (h - 1);
    int64_t v = (int64_t)px[(y << shift) | x] * scale_of(argc, a, 3);
    *out = mk(T_FIX, wrap32(v));
    vm_charge(vm, 4);
    return 1;
}
