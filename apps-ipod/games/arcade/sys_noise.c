/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade systems: noise -- smooth random values for generated levels,
 * terrain and clouds. Value noise: a random number at every whole point
 * of a lattice, from a hash of the point and the seed, blended smoothly
 * between; octaves add finer lattices at half the weight each. The same
 * point and seed always give the same value, here and in fill().
 *
 * Parts, in order:
 *   the lattice   hash, one octave, the octaves summed
 *   natives       noise.at, noise.fill
 ***************************************************************************/
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)

#define MAX_OCTAVES 8

/* ---- the lattice --------------------------------------------------------- */

/* The lattice's value at a whole point, 0 to 65535 */
static uint32_t hash(int32_t x, int32_t y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^
                 seed * 0xcb1ab31fu;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return h & 0xffff;
}

/* 3t^2 - 2t^3 for t in 0..65536, so the blend has no creases */
static uint32_t smooth(uint32_t t)
{
    return (uint32_t)(((uint64_t)t * t * (3 * 65536 - 2 * t)) >> 32);
}

/* a to b by t, 0 to 65536 */
static int32_t blend(int32_t a, int32_t b, uint32_t t)
{
    return a + (int32_t)(((int64_t)(b - a) * t) >> 16);
}

/* One octave at x, y in 16.16 lattice units: 0 to 65535 */
static uint32_t octave(int32_t x, int32_t y, uint32_t seed)
{
    int32_t ix = x >> 16, iy = y >> 16;
    uint32_t fx = smooth((uint32_t)x & 0xffff);
    uint32_t fy = smooth((uint32_t)y & 0xffff);
    int32_t top = blend((int32_t)hash(ix, iy, seed),
                        (int32_t)hash(ix + 1, iy, seed), fx);
    int32_t bot = blend((int32_t)hash(ix, iy + 1, seed),
                        (int32_t)hash(ix + 1, iy + 1, seed), fx);
    return (uint32_t)blend(top, bot, fy);
}

/* The octaves summed and scaled back to 0 to 65535: each finer by two,
 * half the weight, its own seed */
static uint32_t noise(int32_t x, int32_t y, uint32_t seed, int octaves)
{
    uint32_t sum = 0, total = 0, weight = 1u << (MAX_OCTAVES - 1);
    for (int o = 0; o < octaves; o++) {
        sum += octave(x, y, seed + (uint32_t)o * 0x9e3779b9u) * weight;
        total += weight;
        x *= 2;
        y *= 2;
        weight >>= 1;
    }
    return sum / total;
}

/* ---- natives ------------------------------------------------------------- */

static int32_t whole(struct value v, int32_t dflt)
{
    return is_num(v) ? floor_int(v) : dflt;
}

static int octaves_of(struct value v)
{
    int32_t o = whole(v, 1);
    return o < 1 ? 1 : o > MAX_OCTAVES ? MAX_OCTAVES : (int)o;
}

/* noise.at(x, y, seed, [octaves]): the noise at x, y in lattice units, a
 * fraction from 0 to just under 1 */
NATIVE(noise_at)
{
    if (!is_num(a[0]) || !is_num(a[1]))
        return vm_fail(vm, "noise.at() needs x and y", "", "");
    uint32_t n = noise(wrap32(scaled(a[0])), wrap32(scaled(a[1])),
                       (uint32_t)whole(a[2], 0),
                       argc > 3 ? octaves_of(a[3]) : 1);
    /* measured on the ARM rig (.build/arcade-rig, sysprobe): about 11
     * units an octave */
    vm_charge(vm, 4 + 11 * (uint32_t)(argc > 3 ? octaves_of(a[3]) : 1));
    *out = mk(T_FIX, (int32_t)n);
    return 1;
}

/* noise.fill(dst, w, ox, oy, scale, octaves, seed, ramp): every cell of
 * dst -- a Uint8Array w wide, or an image, which brings its own width --
 * from the noise at ((ox + x) / scale, (oy + y) / scale). ramp is the
 * values to use, low to high, as an array of numbers, or a count n for
 * 0 to n - 1. Returns the cells filled. */
NATIVE(noise_fill)
{
    (void)argc;
    uint8_t *px;
    int32_t w, h;
    if (a[0].tag == T_IMG) {
        struct img *im = MEM(vm, (uint32_t)a[0].v);
        px = im->px;
        w = im->width;
        h = im->height;
    } else {
        struct board b;
        if (!board_of(vm, a[0], a[1], "noise.fill", &b))
            return 0;
        px = b.c;
        w = b.w;
        h = b.h;
    }
    struct column ramp;
    int32_t levels;
    if (is_num(a[7])) {
        levels = whole(a[7], 0);
        ramp.tag = T_NULL;
    } else if (column_of(vm, a[7], &ramp)) {
        levels = (int32_t)ramp.len;
    } else {
        levels = 0;
    }
    if (levels < 1 || levels > 256)
        return vm_fail(vm, "noise.fill() needs a ramp: an array of values, or "
                       "a count from 1 to 256", "", "");
    if (!is_num(a[4]) || scaled(a[4]) < 0x1000)
        return vm_fail(vm, "noise.fill() needs a scale of at least 1/16",
                       "", "");
    /* lattice units a cell, and the first cell's place */
    int32_t step = (int32_t)(((int64_t)1 << 32) / scaled(a[4]));
    int32_t x0 = (int32_t)((scaled(a[2]) << 16) / scaled(a[4]));
    int32_t y0 = (int32_t)((scaled(a[3]) << 16) / scaled(a[4]));
    int octaves = octaves_of(a[5]);
    uint32_t seed = (uint32_t)whole(a[6], 0);
    /* paid a row at a time, so a fill too big for the tick is stopped */
    for (int32_t y = 0; y < h; y++) {
        /* measured on the rig (sysprobe2): about 12 units a cell an octave */
        if (!vm_spend(vm, (uint32_t)w * (uint32_t)octaves * 12))
            return 0;
        for (int32_t x = 0; x < w; x++) {
            uint32_t n = noise(x0 + x * step, y0 + y * step, seed, octaves);
            uint32_t k = (n * (uint32_t)levels) >> 16;
            px[y * w + x] = (uint8_t)(ramp.tag == T_NULL ? (int32_t)k
                                      : column_at(&ramp, k, 0));
        }
    }
    vm_charge(vm, 10);
    *out = mk(T_INT, w * h);
    return 1;
}
