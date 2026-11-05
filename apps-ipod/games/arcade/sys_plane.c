/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade systems: the plane -- the active map laid flat as a floor and
 * seen from a camera, with sprites standing on it.
 *
 * World units are pixels, as for actors: a map cell is 8. The camera has a
 * position, a height and a heading in turns, the engine's own sin and cos:
 * a cart moving by cos(a) * v, sin(a) * v goes the way the camera looks.
 * For a screen row dy below the horizon, the floor there is z * f / dy
 * away, f the focal length in pixels, and one screen pixel along the row
 * is that distance over f; so each row is a start and a step, and the
 * floor is the rows' loop (core_draw_floor()).
 *
 * Parts, in order:
 *   the camera   plane.view, and what is worked out from it
 *   the floor    plane.draw
 *   points       plane.project, plane.pick
 *   billboards   plane.billboards
 ***************************************************************************/
#include <stddef.h>
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)

static int32_t fx(struct value v)
{
    return is_num(v) ? wrap32(scaled(v)) : 0;
}

static int32_t mul(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 16);
}

/* ---- the camera --------------------------------------------------------- */

/* plane.view(x, y, z, angle, horizon, fov) */
NATIVE(plane_view)
{
    (void)out; (void)argc;
    for (int i = 0; i < 6; i++)
        if (!is_num(a[i]))
            return vm_fail(vm, "plane.view() needs six numbers: x, y, height, "
                           "angle, horizon and field of view", "", "");
    struct planeview *v = &vm->plane;
    v->x = fx(a[0]);
    v->y = fx(a[1]);
    v->z = fx(a[2]);
    if (v->z < 0x1000) v->z = 0x1000;          /* 1/16 of a pixel up */
    v->horizon = floor_int(a[4]);
    int32_t angle = fx(a[3]), fov = fx(a[5]);
    if (fov < 0x0400) fov = 0x0400;             /* 1/64 of a turn */
    if (fov > 0x7000) fov = 0x7000;             /* under half a turn */
    v->dx = core_sine((uint32_t)angle + 0x4000);
    v->dy = -core_sine((uint32_t)angle);
    uint32_t half = (uint32_t)fov / 2;
    int32_t s = core_sine(half), c = core_sine(half + 0x4000);
    v->focal = (int32_t)(((int64_t)(SCREEN_W / 2) << 32) / s * c >> 16);
    v->set = 1;
    v->depth_set = 0;
    vm_charge(vm, 10);
    return 1;
}

static int view_set(struct vm *vm, const char *fn)
{
    if (!vm->plane.set)
        return vm_fail(vm, fn, "() needs a camera first: plane.view()", "");
    return 1;
}

/* ---- the floor ----------------------------------------------------------- */

/* The farthest the floor is drawn, 16.16: twice the largest map, so a row's
 * ends stay inside 32 bits */
#define FAR_FLOOR ((int64_t)4096 << 16)

/* How far away the floor is at dy rows below the horizon, 16.16. K, the
 * height times the focal length, fits 32 bits at any usual height, and a
 * 32-bit division is a fifth of a 64-bit one on the ARM7. */
static int32_t distance(const struct planeview *v, int32_t dy)
{
    int64_t k = (int64_t)v->z * v->focal >> 16, d;
    if (k < 0x80000000LL)
        d = (uint32_t)k / (uint32_t)dy;
    else
        d = k / dy;
    return d > FAR_FLOOR ? (int32_t)FAR_FLOOR : (int32_t)d;
}

/* plane.fog(near, far, colour): the ground from near to far world pixels
 * away goes to colour, dithered, as distance does -- it hides the shimmer
 * of texels skipped near the horizon; a colour of -1 turns it off. Kept
 * until changed; plane.draw and land.draw use it. */
NATIVE(plane_fog)
{
    (void)argc; (void)out;
    for (int i = 0; i < 3; i++)
        if (!is_num(a[i]))
            return vm_fail(vm, "plane.fog() needs near, far and a colour", "",
                           "");
    struct planeview *v = &vm->plane;
    int32_t c = floor_int(a[2]);
    v->fog_on = c >= 0 && c <= 255;
    v->fog_near = fx(a[0]);
    v->fog_far = fx(a[1]);
    v->fog_colour = (uint8_t)(c & 0xff);
    vm_charge(vm, 4);
    return 1;
}

/* plane.draw([top], [bottom], [outside], [coarse]): coarse 2 reads every
 * other pixel and draws each twice, for about half the time */
NATIVE(plane_draw)
{
    (void)out;
    if (!view_set(vm, "plane.draw"))
        return 0;
    const struct planeview *v = &vm->plane;
    int32_t top = v->horizon, bottom = SCREEN_H - 1, outside = -1;
    if (argc > 0 && is_num(a[0])) top = floor_int(a[0]);
    if (argc > 1 && is_num(a[1])) bottom = floor_int(a[1]);
    if (argc > 2 && is_num(a[2])) outside = floor_int(a[2]);
    int coarse = argc > 3 && is_num(a[3]) ? floor_int(a[3]) : 1;
    if (top < v->horizon) top = v->horizon;
    if (top < 0) top = 0;
    if (bottom > SCREEN_H - 1) bottom = SCREEN_H - 1;
    if (outside > 255) outside = -1;
    static struct floor_row rows[SCREEN_H];
    /* 1 / focal in 0.32, so that a row's step is a multiply */
    uint32_t inv = (uint32_t)(((uint64_t)1 << 48) / (uint32_t)v->focal);
    for (int32_t y = top; y <= bottom; y++) {
        /* the distance, and one screen pixel's worth of floor, 16.16 */
        int32_t d = distance(v, y - v->horizon + 1);
        int32_t step = (int32_t)((uint64_t)(uint32_t)d * inv >> 32);
        /* the row's centre, then its left end: right is (-dy, dx) */
        int32_t cx = v->x + mul(v->dx, d), cy = v->y + mul(v->dy, d);
        int32_t rx = -v->dy, ry = v->dx;
        struct floor_row *r = &rows[y];
        r->du = mul(rx, step);
        r->dv = mul(ry, step);
        r->u = cx - r->du * (SCREEN_W / 2);
        r->v = cy - r->dv * (SCREEN_W / 2);
        r->fog = core_fog_level(v, d);
    }
    if (bottom >= top)
        vm_charge(vm, (uint32_t)(bottom - top + 1) * 4);
    core_draw_floor(vm, rows, top, bottom, outside, coarse);
    return 1;
}

/* ---- points -------------------------------------------------------------- */

/* A world point's place on the screen: 0 when it is behind the camera */
static int project(const struct planeview *v, int32_t x, int32_t y, int32_t z,
                   int32_t *sx, int32_t *sy, int32_t *scale, int32_t *depth)
{
    int32_t rx = x - v->x, ry = y - v->y;
    int32_t d = mul(rx, v->dx) + mul(ry, v->dy);         /* along the view */
    int32_t side = mul(rx, -v->dy) + mul(ry, v->dx);     /* to the right */
    if (d < (1 << 16) / 4)
        return 0;
    int32_t s = (int32_t)(((int64_t)v->focal << 16) / d);   /* px a px */
    *scale = s;
    *depth = d;
    *sx = (SCREEN_W / 2 << 16) + mul(side, s);
    *sy = ((v->horizon - 1) << 16) + mul(v->z - z, s);
    return 1;
}

static int out_array(struct vm *vm, struct value o, uint32_t need,
                     const char *fn)
{
    uint32_t len = o.tag == T_ARR ? ARR(vm, o)->len : 0;
    if (len < need)
        return vm_fail(vm, fn, "() writes to an array of at least the "
                       "numbers it gives", "");
    return 1;
}

/* plane.project(x, y, z, out): screen x, y, scale and depth into out */
NATIVE(plane_project)
{
    (void)argc;
    if (!view_set(vm, "plane.project") ||
        !out_array(vm, a[3], 4, "plane.project"))
        return 0;
    int32_t sx, sy, s, d;
    *out = mk(T_INT, 0);
    if (project(&vm->plane, fx(a[0]), fx(a[1]), fx(a[2]), &sx, &sy, &s, &d)) {
        struct value *o = BUF(vm, ARR(vm, a[3])->buf)->v;
        o[0] = mk(T_FIX, sx);
        o[1] = mk(T_FIX, sy);
        o[2] = mk(T_FIX, s);
        o[3] = mk(T_FIX, d);
        *out = mk(T_INT, 1);
    }
    vm_charge(vm, 8);
    return 1;
}

/* plane.pick(sx, sy, out): the floor's x, y under a screen point */
NATIVE(plane_pick)
{
    (void)argc;
    if (!view_set(vm, "plane.pick") || !out_array(vm, a[2], 2, "plane.pick"))
        return 0;
    const struct planeview *v = &vm->plane;
    int32_t dy = floor_int(a[1]) - v->horizon + 1;
    *out = mk(T_INT, 0);
    if (dy >= 1 && is_num(a[0])) {
        int32_t d = distance(v, dy);
        int32_t side = (int32_t)((int64_t)(fx(a[0]) - (SCREEN_W / 2 << 16)) *
                                 d / v->focal);
        struct value *o = BUF(vm, ARR(vm, a[2])->buf)->v;
        o[0] = mk(T_FIX, v->x + mul(v->dx, d) + mul(-v->dy, side));
        o[1] = mk(T_FIX, v->y + mul(v->dy, d) + mul(v->dx, side));
        *out = mk(T_INT, 1);
    }
    vm_charge(vm, 8);
    return 1;
}

/* ---- billboards ---------------------------------------------------------- */

#define MAX_BILLBOARDS 256

/* plane.billboards(xs, ys, zs, sprites, sizes, flips, n, [ws], [hs]): each
 * sprite stands on the floor at its point, its feet z above it, size world
 * pixels tall, drawn far to near. ws and hs are its sprites across and
 * down, as spr() takes them (1 by default); its width follows. Each
 * argument is an array of any kind, or one number for all. */
NATIVE(plane_billboards)
{
    if (!view_set(vm, "plane.billboards"))
        return 0;
    int32_t n = is_num(a[6]) ? floor_int(a[6]) : 0;
    if (n < 0) n = 0;
    if (n > MAX_BILLBOARDS) n = MAX_BILLBOARDS;
    struct column c[8];
    static const struct column one = { T_INT, NULL, 0xffffffffu, 1 };
    c[6] = one;
    c[7] = one;
    for (int i = 0; i < 8; i++) {
        int at = i < 6 ? i : i + 1;
        if (at >= argc || (i >= 6 && a[at].tag == T_NULL))
            continue;
        if (!column_of(vm, a[at], &c[i]) || c[i].len < (uint32_t)n)
            return vm_fail(vm, "plane.billboards() takes arrays (or numbers) "
                           "of at least n", "", "");
    }
    static int32_t order[MAX_BILLBOARDS], dep[MAX_BILLBOARDS];
    static int32_t px[MAX_BILLBOARDS], py[MAX_BILLBOARDS];
    static int32_t pw[MAX_BILLBOARDS], ph[MAX_BILLBOARDS];
    int32_t k = 0;
    for (int32_t i = 0; i < n; i++) {
        int32_t s = column_at(&c[3], (uint32_t)i, -1), sx, sy, sc, d;
        int32_t cw = column_at(&c[6], (uint32_t)i, 1);
        int32_t ch = column_at(&c[7], (uint32_t)i, 1);
        if (s < 0 || s > 255 || cw < 1 || ch < 1 || cw > 16 || ch > 16 ||
            !project(&vm->plane, column_fix(&c[0], (uint32_t)i),
                     column_fix(&c[1], (uint32_t)i),
                     column_fix(&c[2], (uint32_t)i), &sx, &sy, &sc, &d))
            continue;
        /* screen pixels tall, and wide by the sprites' shape */
        int32_t h = mul(column_fix(&c[4], (uint32_t)i), sc) >> 16;
        int32_t w = h * cw / ch;
        if (h < 1 || w < 1 || h > 4 * SCREEN_H || w > 4 * SCREEN_W)
            continue;
        px[i] = (sx >> 16) - w / 2;
        py[i] = (sy >> 16) - h;
        pw[i] = w;
        ph[i] = h;
        dep[i] = d;
        /* far to near: an insertion sort, as the list is short */
        int32_t j = k++;
        while (j > 0 && dep[order[j - 1]] < d) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    for (int32_t j = 0; j < k; j++) {
        uint32_t i = (uint32_t)order[j];
        core_draw_billboard(vm, column_at(&c[3], i, 0), px[i], py[i], pw[i],
                            ph[i], column_at(&c[6], i, 1),
                            column_at(&c[7], i, 1),
                            column_true(vm, &c[5], i, 0), dep[i]);
    }
    vm_charge(vm, 10 + (uint32_t)n * 4);
    *out = mk(T_INT, k);
    return 1;
}
