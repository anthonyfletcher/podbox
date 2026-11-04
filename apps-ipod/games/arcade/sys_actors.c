/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade systems: actors and particles. A pool is a record the engine
 * makes, its fields plain arrays the cart reads and writes as it likes;
 * these natives do the bulk work over them -- moving and sweeping against
 * the map, finding overlaps, stepping particles -- and report what
 * happened, leaving what it means to the cart.
 *
 * Positions and speeds are pixels, read as 16.16 whatever the cart stored
 * and written back as fractions. A map cell is 8 pixels.
 *
 * Parts, in order:
 *   pools       making one, and checking one handed back
 *   actors      spawn, move, overlaps, draw
 *   particles   emit, step, draw
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)
#define CELL_SHIFT 19           /* 16.16 pixels to 8-pixel cells */
#define MAX_STEP   (4 << 16)    /* a sweep's longest step: half a cell */

/* ---- pools ------------------------------------------------------------ */

enum { A_ON, A_X, A_Y, A_DX, A_DY, A_W, A_H, A_SP, A_FLIP, A_CELL,
       A_ARRAYS, A_GRAVITY = A_ARRAYS, A_DRAG, A_FIELDS };
static const char *const actor_keys[A_FIELDS] = {
    "on", "x", "y", "dx", "dy", "w", "h", "sp", "flip", "cell",
    "gravity", "drag"
};

enum { P_LIFE, P_X, P_Y, P_DX, P_DY, P_C, P_ARRAYS,
       P_GRAVITY = P_ARRAYS, P_DRAG, P_FIELDS };
static const char *const particle_keys[P_FIELDS] = {
    "life", "x", "y", "dx", "dy", "c", "gravity", "drag"
};

static int32_t fx(struct value v)
{
    return is_num(v) ? wrap32(scaled(v)) : 0;
}

/* A record of the keys, its first `arrays` fields arrays of n, each array
 * filled with its entry of fill; *shape records the record's shape, which
 * marks every pool of this kind */
static int make_pool(struct vm *vm, const char *const *keys, int fields,
                     int arrays, const int32_t *fill, int32_t n,
                     uint32_t *shape, const char *fn, struct value *out)
{
    if (n < 1 || n > 4096)
        return vm_fail(vm, fn, "() makes a pool of 1 to 4096", "");
    if (!vm_new_record(vm, keys, (uint32_t)fields, out))
        return 0;
    *shape = OBJ(vm, *out)->shape;
    vm->tmp[vm->ntmp++] = *out;
    for (int f = 0; f < fields; f++) {
        struct value v = mk(T_INT, 0);
        if (f < arrays) {
            if (!vm_new_array(vm, (uint32_t)n, &v)) {
                vm->ntmp--;
                return 0;
            }
            struct value *e = BUF(vm, ARR(vm, v)->buf)->v;
            for (int32_t i = 0; i < n; i++)
                e[i] = mk(T_INT, fill[f]);
        }
        BUF(vm, OBJ(vm, *out)->buf)->v[f] = v;
    }
    vm->ntmp--;
    vm_charge(vm, 20 + (uint32_t)(n * arrays) / 4);
    return 1;
}

/* A pool handed back: of this kind (a cart may add fields of its own),
 * its arrays still arrays of one length. Fills f with the fields' values
 * and *n with the length. */
static int pool_of(struct vm *vm, struct value p, uint32_t shape, int fields,
                   int arrays, struct value *f, uint32_t *n, const char *fn,
                   const char *maker)
{
    uint32_t s = p.tag == T_OBJ ? OBJ(vm, p)->shape : 0;
    while (s && s != shape)
        s = vm->shapes[s].parent;
    if (!shape || s != shape)
        return vm_fail(vm, fn, "() needs a pool from ", maker);
    memcpy(f, BUF(vm, OBJ(vm, p)->buf)->v, (size_t)fields * sizeof(*f));
    for (int i = 0; i < arrays; i++) {
        if (f[i].tag != T_ARR || (i && ARR(vm, f[i])->len != *n))
            return vm_fail(vm, fn, "() needs a pool whose fields are "
                           "arrays of one length, as ", maker);
        if (!i)
            *n = ARR(vm, f[i])->len;
    }
    return 1;
}

#define ELEMS(vm, arr) (BUF(vm, ARR(vm, arr)->buf)->v)

static int whole_arg(struct vm *vm, struct value *a, int argc, int i,
                     const char *fn, int32_t *out)
{
    *out = 0;
    if (i >= argc || a[i].tag == T_NULL)
        return 1;
    if (!is_num(a[i]))
        return vm_fail(vm, fn, "() needs a number, not a ",
                       vm_typename(a[i].tag));
    *out = floor_int(a[i]);
    return 1;
}

/* drag is a fraction of the speed lost each tick */
static int32_t dragged(int32_t v, int32_t drag)
{
    return drag ? v - (int32_t)(((int64_t)v * drag) >> 16) : v;
}

/* ---- actors ------------------------------------------------------------ */

NATIVE(actors_pool)
{
    int32_t n;
    if (!whole_arg(vm, a, argc, 0, "actors.pool", &n))
        return 0;
    /* on, x, y, dx, dy, w, h, sp, flip, cell */
    static const int32_t fill[A_ARRAYS] = { 0, 0, 0, 0, 0, 8, 8, -1, 0, -1 };
    return make_pool(vm, actor_keys, A_FIELDS, A_ARRAYS, fill, n,
                     &vm->actor_shape, "actors.pool", out);
}

static int actors(struct vm *vm, struct value p, struct value *f, uint32_t *n,
                  const char *fn)
{
    return pool_of(vm, p, vm->actor_shape, A_FIELDS, A_ARRAYS, f, n, fn,
                   "actors.pool()");
}

NATIVE(actors_spawn)
{
    (void)argc;
    struct value f[A_FIELDS];
    uint32_t n;
    if (!actors(vm, a[0], f, &n, "actors.spawn"))
        return 0;
    *out = mk(T_INT, -1);
    for (uint32_t i = 0; i < n; i++)
        if (!vm_truthy(vm, ELEMS(vm, f[A_ON])[i])) {
            ELEMS(vm, f[A_ON])[i] = mk(T_INT, 1);
            ELEMS(vm, f[A_DX])[i] = mk(T_INT, 0);
            ELEMS(vm, f[A_DY])[i] = mk(T_INT, 0);
            ELEMS(vm, f[A_CELL])[i] = mk(T_INT, -1);
            *out = mk(T_INT, (int32_t)i);
            break;
        }
    vm_charge(vm, 4 + n / 8);
    return 1;
}

static int solid(const struct vm *vm, int32_t cx, int32_t cy, int32_t mask)
{
    if (!mask || cx < 0 || cy < 0 || cx >= vm->map_w || cy >= vm->map_h)
        return 0;
    const uint8_t *map = MEM(vm, vm->map), *flags = MEM(vm, vm->flags);
    return flags[map[cy * vm->map_w + cx]] & mask;
}

struct contacts
{
    int32_t *d;
    uint32_t cap, n;
};

static void contact(struct contacts *c, uint32_t actor, int what, int32_t cx,
                    int32_t cy)
{
    if (c->n < c->cap) {
        int32_t *d = c->d + c->n * 4;
        d[0] = (int32_t)actor;
        d[1] = what;
        d[2] = cx;
        d[3] = cy;
        c->n++;
    }
}

/* The first solid cell of column cx over rows y0..y1, or -1 */
static int32_t column_hit(const struct vm *vm, int32_t cx, int32_t y0,
                          int32_t y1, int32_t mask)
{
    for (int32_t cy = y0; cy <= y1; cy++)
        if (solid(vm, cx, cy, mask))
            return cy;
    return -1;
}

static int32_t row_hit(const struct vm *vm, int32_t cy, int32_t x0, int32_t x1,
                       int32_t mask)
{
    for (int32_t cx = x0; cx <= x1; cx++)
        if (solid(vm, cx, cy, mask))
            return cx;
    return -1;
}

/* Moves *pos by v along one axis, the box's other extent fixed, stopping
 * flush at a solid cell; returns the side that hit, or -1 */
static int sweep(const struct vm *vm, int32_t *pos, int32_t v, int32_t size,
                 int32_t lo, int32_t hi, int32_t mask, int x_axis,
                 int32_t *hx, int32_t *hy)
{
    int32_t a0 = lo >> CELL_SHIFT, a1 = (lo + hi - 1) >> CELL_SHIFT;
    while (v) {
        int32_t step = v > MAX_STEP ? MAX_STEP : v < -MAX_STEP ? -MAX_STEP : v;
        int32_t np = *pos + step, cell, at;
        if (step > 0) {
            cell = (np + size - 1) >> CELL_SHIFT;
            if (cell != (*pos + size - 1) >> CELL_SHIFT &&
                (at = x_axis ? column_hit(vm, cell, a0, a1, mask)
                             : row_hit(vm, cell, a0, a1, mask)) >= 0) {
                *pos = (cell << CELL_SHIFT) - size;
                *hx = x_axis ? cell : at;
                *hy = x_axis ? at : cell;
                return x_axis ? 1 : 3;          /* RIGHT, DOWN */
            }
        } else {
            cell = np >> CELL_SHIFT;
            if (cell != *pos >> CELL_SHIFT &&
                (at = x_axis ? column_hit(vm, cell, a0, a1, mask)
                             : row_hit(vm, cell, a0, a1, mask)) >= 0) {
                *pos = (cell + 1) << CELL_SHIFT;
                *hx = x_axis ? cell : at;
                *hy = x_axis ? at : cell;
                return x_axis ? 0 : 2;          /* LEFT, UP */
            }
        }
        *pos = np;
        v -= step;
    }
    return -1;
}

/* actors.move(p, solid, trigger, out) */
NATIVE(actors_move)
{
    struct value f[A_FIELDS];
    uint32_t n;
    int32_t smask, tmask;
    if (!actors(vm, a[0], f, &n, "actors.move") ||
        !whole_arg(vm, a, argc, 1, "actors.move", &smask) ||
        !whole_arg(vm, a, argc, 2, "actors.move", &tmask))
        return 0;
    if (a[3].tag != T_I32)
        return vm_fail(vm, "actors.move() writes its contacts to an "
                       "Int32Array", "", "");
    struct contacts c = { I32(vm, a[3])->d, I32(vm, a[3])->len / 4, 0 };
    int32_t g = fx(f[A_GRAVITY]), drag = fx(f[A_DRAG]);
    struct value *on = ELEMS(vm, f[A_ON]), *x = ELEMS(vm, f[A_X]);
    struct value *y = ELEMS(vm, f[A_Y]), *dx = ELEMS(vm, f[A_DX]);
    struct value *dy = ELEMS(vm, f[A_DY]), *w = ELEMS(vm, f[A_W]);
    struct value *h = ELEMS(vm, f[A_H]), *cell = ELEMS(vm, f[A_CELL]);
    uint32_t moved = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!vm_truthy(vm, on[i]))
            continue;
        moved++;
        int32_t X = fx(x[i]), Y = fx(y[i]), W = fx(w[i]), H = fx(h[i]);
        int32_t DX = dragged(fx(dx[i]), drag), DY = dragged(fx(dy[i]) + g, drag);
        int32_t hx, hy, side;
        if (W < 1) W = 1;
        if (H < 1) H = 1;
        if ((side = sweep(vm, &X, DX, W, Y, H, smask, 1, &hx, &hy)) >= 0)
            contact(&c, i, side, hx, hy);
        if ((side = sweep(vm, &Y, DY, H, X, W, smask, 0, &hx, &hy)) >= 0)
            contact(&c, i, side, hx, hy);
        x[i] = mk(T_FIX, X);
        y[i] = mk(T_FIX, Y);
        dx[i] = mk(T_FIX, DX);
        dy[i] = mk(T_FIX, DY);
        /* the trigger cell under the centre, reported on the way in */
        int32_t cx = (X + W / 2) >> CELL_SHIFT, cy = (Y + H / 2) >> CELL_SHIFT;
        if (tmask && solid(vm, cx, cy, tmask)) {
            int32_t idx = cy * vm->map_w + cx;
            if (floor_int(cell[i]) != idx || !is_num(cell[i])) {
                contact(&c, i, 4, cx, cy);
                cell[i] = mk(T_INT, idx);
            }
        } else
            cell[i] = mk(T_INT, -1);
    }
    vm_charge(vm, 10 + n / 4 + moved * 6 + c.n * 2);
    *out = mk(T_INT, (int32_t)c.n);
    return 1;
}

/* actors.overlaps(a, b, out) */
NATIVE(actors_overlaps)
{
    (void)argc;
    struct value fa[A_FIELDS], fb[A_FIELDS];
    uint32_t na, nb;
    if (!actors(vm, a[0], fa, &na, "actors.overlaps") ||
        !actors(vm, a[1], fb, &nb, "actors.overlaps"))
        return 0;
    if (a[2].tag != T_I32)
        return vm_fail(vm, "actors.overlaps() writes its pairs to an "
                       "Int32Array", "", "");
    int same = a[0].v == a[1].v;
    int32_t *o = I32(vm, a[2])->d;
    uint32_t cap = I32(vm, a[2])->len / 2, k = 0, tests = 0;
    for (uint32_t i = 0; i < na; i++) {
        if (!vm_truthy(vm, ELEMS(vm, fa[A_ON])[i]))
            continue;
        int32_t ax = fx(ELEMS(vm, fa[A_X])[i]), ay = fx(ELEMS(vm, fa[A_Y])[i]);
        int32_t aw = fx(ELEMS(vm, fa[A_W])[i]), ah = fx(ELEMS(vm, fa[A_H])[i]);
        for (uint32_t j = same ? i + 1 : 0; j < nb; j++) {
            if (!vm_truthy(vm, ELEMS(vm, fb[A_ON])[j]))
                continue;
            tests++;
            int32_t bx = fx(ELEMS(vm, fb[A_X])[j]), by = fx(ELEMS(vm, fb[A_Y])[j]);
            int32_t bw = fx(ELEMS(vm, fb[A_W])[j]), bh = fx(ELEMS(vm, fb[A_H])[j]);
            if (ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah &&
                k < cap) {
                o[k * 2] = (int32_t)i;
                o[k * 2 + 1] = (int32_t)j;
                k++;
            }
        }
    }
    vm_charge(vm, 10 + tests / 2);
    *out = mk(T_INT, (int32_t)k);
    return 1;
}

NATIVE(actors_draw)
{
    (void)out; (void)argc;
    struct value f[A_FIELDS];
    uint32_t n;
    if (!actors(vm, a[0], f, &n, "actors.draw"))
        return 0;
    struct column on, x, y, sp, flip;
    column_of(vm, f[A_ON], &on);
    column_of(vm, f[A_X], &x);
    column_of(vm, f[A_Y], &y);
    column_of(vm, f[A_SP], &sp);
    column_of(vm, f[A_FLIP], &flip);
    core_draw_sprites(vm, &on, &sp, &x, &y, &flip, n);
    return 1;
}

/* ---- particles ----------------------------------------------------------- */

NATIVE(particles_pool)
{
    int32_t n;
    if (!whole_arg(vm, a, argc, 0, "particles.pool", &n))
        return 0;
    static const int32_t fill[P_ARRAYS] = { 0 };
    return make_pool(vm, particle_keys, P_FIELDS, P_ARRAYS, fill, n,
                     &vm->particle_shape, "particles.pool", out);
}

static int particles(struct vm *vm, struct value p, struct value *f,
                     uint32_t *n, const char *fn)
{
    return pool_of(vm, p, vm->particle_shape, P_FIELDS, P_ARRAYS, f, n, fn,
                   "particles.pool()");
}

/* A random amount from -range to range, 16.16 */
static int32_t spread(struct vm *vm, int32_t range)
{
    if (range <= 0)
        return 0;
    uint32_t r = core_random(vm) & 0xffff;
    return (int32_t)(((int64_t)range * (int32_t)(r * 2 - 0xffff)) >> 16);
}

/* particles.emit(p, count, x, y, dx, dy, spread, life, c) */
NATIVE(particles_emit)
{
    struct value f[P_FIELDS];
    uint32_t n;
    int32_t count, life, c;
    if (!particles(vm, a[0], f, &n, "particles.emit") ||
        !whole_arg(vm, a, argc, 1, "particles.emit", &count) ||
        !whole_arg(vm, a, argc, 7, "particles.emit", &life) ||
        !whole_arg(vm, a, argc, 8, "particles.emit", &c))
        return 0;
    if (life > 30000)
        life = 30000;
    for (int i = 2; i <= 6; i++)
        if (!is_num(a[i]))
            return vm_fail(vm, "particles.emit() needs numbers for x, y, dx, "
                           "dy and spread", "", "");
    int32_t x = fx(a[2]), y = fx(a[3]), dx = fx(a[4]), dy = fx(a[5]);
    int32_t sp = fx(a[6]);
    struct value *lf = ELEMS(vm, f[P_LIFE]);
    int32_t made = 0;
    for (uint32_t i = 0; i < n && made < count; i++) {
        if (floor_int(lf[i]) > 0)
            continue;
        int32_t l = life + (int32_t)(((int64_t)spread(vm, life << 16) / 4) >> 16);
        lf[i] = mk(T_INT, l < 1 ? 1 : l);
        ELEMS(vm, f[P_X])[i] = mk(T_FIX, x);
        ELEMS(vm, f[P_Y])[i] = mk(T_FIX, y);
        ELEMS(vm, f[P_DX])[i] = mk(T_FIX, dx + spread(vm, sp));
        ELEMS(vm, f[P_DY])[i] = mk(T_FIX, dy + spread(vm, sp));
        ELEMS(vm, f[P_C])[i] = mk(T_INT, c);
        made++;
    }
    vm_charge(vm, 10 + (uint32_t)made * 2 + n / 16);
    *out = mk(T_INT, made);
    return 1;
}

NATIVE(particles_step)
{
    (void)out; (void)argc;
    struct value f[P_FIELDS];
    uint32_t n;
    if (!particles(vm, a[0], f, &n, "particles.step"))
        return 0;
    int32_t g = fx(f[P_GRAVITY]), drag = fx(f[P_DRAG]);
    struct value *lf = ELEMS(vm, f[P_LIFE]), *x = ELEMS(vm, f[P_X]);
    struct value *y = ELEMS(vm, f[P_Y]), *dx = ELEMS(vm, f[P_DX]);
    struct value *dy = ELEMS(vm, f[P_DY]);
    uint32_t live = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t l = floor_int(lf[i]);
        if (l <= 0)
            continue;
        live++;
        lf[i] = mk(T_INT, l - 1);
        int32_t DX = dragged(fx(dx[i]), drag), DY = dragged(fx(dy[i]) + g, drag);
        dx[i] = mk(T_FIX, DX);
        dy[i] = mk(T_FIX, DY);
        x[i] = mk(T_FIX, fx(x[i]) + DX);
        y[i] = mk(T_FIX, fx(y[i]) + DY);
    }
    vm_charge(vm, 10 + n / 8 + live * 2);
    return 1;
}

NATIVE(particles_draw)
{
    (void)out; (void)argc;
    struct value f[P_FIELDS];
    uint32_t n;
    if (!particles(vm, a[0], f, &n, "particles.draw"))
        return 0;
    core_draw_dots(vm, ELEMS(vm, f[P_LIFE]), ELEMS(vm, f[P_X]),
                   ELEMS(vm, f[P_Y]), ELEMS(vm, f[P_C]), n);
    return 1;
}
