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
 *   circles     bounce off one another, bounce off the map
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
       A_FRAMES, A_SPEED, A_MAT, A_ZONE,
       A_ARRAYS, A_GRAVITY = A_ARRAYS, A_DRAG, A_PLATFORMS, A_FIELD,
       A_RULES, A_FIELD_X, A_FIELD_Y, A_DENSITY, A_ONEWAY, A_SPLASH,
       A_SLOPE_UP, A_SLOPE_DOWN, A_LADDERS, A_FIELDS };
static const char *const actor_keys[A_FIELDS] = {
    "on", "x", "y", "dx", "dy", "w", "h", "sp", "flip", "cell",
    "frames", "speed", "mat", "zone",
    "gravity", "drag", "platforms", "field", "rules", "field_x", "field_y",
    "density", "oneway", "splash", "slope_up", "slope_down", "ladders"
};

/* A particle is a pixel of colour c, or with sp set, that sprite's top
 * left w x h, flipped by flip (1 across, 2 up and down) and with its
 * colours 1 to shade taken from c's ramp: colour k drawn as c + k - 1.
 * floor and bounce: one falling through floor turns back up at bounce of
 * its speed, unless that is under a quarter pixel a tick. spin: a sprite
 * turns a quarter (flip 0, 1, 3, 2) every spin ticks. cull: one that
 * particles.draw finds wholly off the screen, through the camera it draws
 * by, is gone -- but under a camera turned or zoomed, where no box on the
 * screen is a box in the world. A negative drag speeds them up. */
enum { P_LIFE, P_X, P_Y, P_DX, P_DY, P_C, P_SP, P_FLIP,
       P_ARRAYS,                    /* PARTICLE_ARRAYS, in vm.h */
       P_GRAVITY = P_ARRAYS, P_DRAG, P_FIELD, P_FIELD_X, P_FIELD_Y,
       P_W, P_H, P_SHADE, P_FLOOR, P_BOUNCE, P_SPIN, P_CULL, P_FIELDS };
_Static_assert(P_ARRAYS == PARTICLE_ARRAYS, "cells.lift's arrays");
static const char *const particle_keys[P_FIELDS] = {
    "life", "x", "y", "dx", "dy", "c", "sp", "flip", "gravity", "drag",
    "field", "field_x", "field_y", "w", "h", "shade", "floor", "bounce",
    "spin", "cull"
};

/* always inlined: -Os would call it, and the pool systems read several
 * fields an actor a tick */
static inline __attribute__((always_inline)) int32_t fx(struct value v)
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
    if (!vm_root(vm, *out))
        return 0;
    for (int f = 0; f < fields; f++) {
        struct value v = mk(T_INT, 0);
        if (f < arrays) {
            if (!vm_new_array(vm, (uint32_t)n, &v)) {
                vm_unroot(vm);
                return 0;
            }
            struct value *e = BUF(vm, ARR(vm, v)->buf)->v;
            for (int32_t i = 0; i < n; i++)
                e[i] = mk(T_INT, fill[f]);
        }
        BUF(vm, OBJ(vm, *out)->buf)->v[f] = v;
    }
    vm_unroot(vm);
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
    /* on, x, y, dx, dy, w, h, sp, flip, cell, frames, speed, mat, zone */
    static const int32_t fill[A_ARRAYS] = { 0, 0, 0, 0, 0, 8, 8, -1, 0, -1,
                                            1, 8, -1, -1 };
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
            ELEMS(vm, f[A_ZONE])[i] = mk(T_INT, -1);
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
 * flush at a solid cell; returns the side that hit, or -1. Off the map is
 * open, so only the map's cells are tried, and counted in *checked. */
static int sweep(const struct vm *vm, int32_t *pos, int32_t v, int32_t size,
                 int32_t lo, int32_t hi, int32_t mask, int32_t oneway,
                 int x_axis, int32_t *hx, int32_t *hy, uint32_t *checked)
{
    /* a one-way cell stops only what comes down into it from above */
    if (!x_axis && v > 0)
        mask |= oneway;
    int32_t a0 = lo >> CELL_SHIFT;
    int32_t a1 = (int32_t)(((int64_t)lo + hi - 1) >> CELL_SHIFT);
    int32_t across = x_axis ? vm->map_h : vm->map_w;
    int32_t along = x_axis ? vm->map_w : vm->map_h;
    if (a0 < 0)
        a0 = 0;
    if (a1 > across - 1)
        a1 = across - 1;
    if (a0 > a1 || !mask) {
        *pos = (int32_t)((uint32_t)*pos + (uint32_t)v);
        return -1;
    }
    while (v) {
        int32_t step = v > MAX_STEP ? MAX_STEP : v < -MAX_STEP ? -MAX_STEP : v;
        int32_t np = *pos + step;
        /* the leading edge's cell, and whether this step entered it */
        int32_t cell = step > 0 ? (np + size - 1) >> CELL_SHIFT
                                : np >> CELL_SHIFT;
        int32_t was = step > 0 ? (*pos + size - 1) >> CELL_SHIFT
                               : *pos >> CELL_SHIFT;
        if (cell != was && cell >= 0 && cell < along) {
            *checked += (uint32_t)(a1 - a0 + 1);
            int32_t at = x_axis ? column_hit(vm, cell, a0, a1, mask)
                                : row_hit(vm, cell, a0, a1, mask);
            if (at >= 0) {
                *pos = step > 0 ? (cell << CELL_SHIFT) - size
                                : (cell + 1) << CELL_SHIFT;
                *hx = x_axis ? cell : at;
                *hy = x_axis ? at : cell;
                /* RIGHT, DOWN; LEFT, UP */
                return step > 0 ? (x_axis ? 1 : 3) : (x_axis ? 0 : 2);
            }
        }
        *pos = np;
        v -= step;
    }
    return -1;
}

/* What an actor moves among besides the map: another pool's actors as
 * solid boxes (platforms), and a cells image whose top left is at fx0, fy0
 * world pixels, its materials' flags saying solid, liquid or harmful */
struct world
{
    struct vm *vm;
    uint32_t np;
    const struct value *pon, *px, *py, *pdx, *pdy, *pw, *ph;
    int oneway;                 /* the platforms are solid from above only */
    int field;
    struct cellworld cw;
    int32_t fx0, fy0;
};

/* The world a pool names in its fields; 0 with the error set when one of
 * them is not what it should be */
static int world_of(struct vm *vm, const struct value *f, struct world *w)
{
    w->vm = vm;
    w->np = 0;
    w->oneway = 0;
    w->field = 0;
    if (f[A_PLATFORMS].tag == T_OBJ) {
        struct value pf[A_FIELDS];
        if (!actors(vm, f[A_PLATFORMS], pf, &w->np, "actors.move"))
            return 0;
        w->pon = ELEMS(vm, pf[A_ON]);
        w->px = ELEMS(vm, pf[A_X]);
        w->py = ELEMS(vm, pf[A_Y]);
        w->pdx = ELEMS(vm, pf[A_DX]);
        w->pdy = ELEMS(vm, pf[A_DY]);
        w->pw = ELEMS(vm, pf[A_W]);
        w->ph = ELEMS(vm, pf[A_H]);
        w->oneway = vm_truthy(vm, pf[A_ONEWAY]);
    }
    if (f[A_FIELD].tag == T_IMG) {
        if (!cells_world(vm, f[A_FIELD], f[A_RULES], &w->cw))
            return 0;
        w->field = 1;
        w->fx0 = is_num(f[A_FIELD_X]) ? floor_int(f[A_FIELD_X]) : 0;
        w->fy0 = is_num(f[A_FIELD_Y]) ? floor_int(f[A_FIELD_Y]) : 0;
    }
    return 1;
}

/* The field's material at world pixel x, y, or -1 off it */
static int32_t field_at(const struct world *w, int32_t x, int32_t y)
{
    x -= w->fx0;
    y -= w->fy0;
    if (x < 0 || y < 0 || x >= w->cw.w || y >= w->cw.h)
        return -1;
    return w->cw.c[y * w->cw.w + x];
}

/* The first solid field material along the pixel line at `at` on one axis,
 * over the other axis's pixels p0..p1, or -1 */
static int32_t field_line(const struct world *w, int x_axis, int32_t at,
                          int32_t p0, int32_t p1)
{
    for (int32_t p = p0; p <= p1; p++) {
        int32_t m = x_axis ? field_at(w, at, p) : field_at(w, p, at);
        if (m >= 0 && (w->cw.flags[m] & CELL_SOLID))
            return m;
    }
    return -1;
}

/* After the map's sweep of one axis from p0 to *p (16.16), the actor's
 * size along it and its other extent lo, len: stopped flush by the first
 * solid field cell or platform on the way. Returns the side that hit or
 * -1, with -2 and the material, or -1 and the platform, in hx, hy. */
static int obstacles(const struct world *w, int32_t p0, int32_t *p,
                     int32_t size, int32_t lo, int32_t len, int x_axis,
                     int32_t *hx, int32_t *hy, uint32_t *checked)
{
    int side = -1;
    int32_t o0 = lo >> 16, o1 = (lo + len - 1) >> 16;
    /* only the field's own pixels can stop it */
    int32_t e0 = 0, e1 = -1;
    if (w->field) {
        int32_t f0 = x_axis ? w->fy0 : w->fx0;
        int32_t f1 = f0 + (x_axis ? w->cw.h : w->cw.w) - 1;
        o0 = o0 < f0 ? f0 : o0;
        o1 = o1 > f1 ? f1 : o1;
        e0 = x_axis ? w->fx0 : w->fy0;
        e1 = o0 > o1 ? e0 - 1 : e0 + (x_axis ? w->cw.w : w->cw.h) - 1;
    }
    if (w->field && *p > p0) {
        int32_t e = ((p0 + size - 1) >> 16) + 1, last = (*p + size - 1) >> 16;
        if (e < e0)
            e = e0;
        if (last > e1)
            last = e1;
        for (; e <= last; e++) {
            int32_t m = field_line(w, x_axis, e, o0, o1);
            *checked += (uint32_t)(o1 - o0 + 1);
            if (m >= 0) {
                *p = (e << 16) - size;
                side = x_axis ? 1 : 3;
                *hx = -2;
                *hy = m;
                break;
            }
        }
    } else if (w->field && *p < p0) {
        int32_t e = (p0 >> 16) - 1, last = *p >> 16;
        if (e > e1)
            e = e1;
        if (last < e0)
            last = e0;
        for (; e >= last; e--) {
            int32_t m = field_line(w, x_axis, e, o0, o1);
            *checked += (uint32_t)(o1 - o0 + 1);
            if (m >= 0) {
                *p = (e + 1) << 16;
                side = x_axis ? 0 : 2;
                *hx = -2;
                *hy = m;
                break;
            }
        }
    }
    for (uint32_t j = 0; j < w->np; j++) {
        if (!vm_truthy(w->vm, w->pon[j]))
            continue;
        int32_t bx = fx(w->px[j]), by = fx(w->py[j]);
        int32_t bw = fx(w->pw[j]), bh = fx(w->ph[j]);
        int32_t b0 = x_axis ? bx : by, bs = x_axis ? bw : bh;
        int32_t c0 = x_axis ? by : bx, cs = x_axis ? bh : bw;
        if (!(lo < c0 + cs && c0 < lo + len))
            continue;
        /* a one-way platform stops only what comes down onto its top */
        if (w->oneway && (x_axis || *p < p0))
            continue;
        if (*p > p0 && p0 + size <= b0 && *p + size > b0) {
            *p = b0 - size;
            side = x_axis ? 1 : 3;
            *hx = -1;
            *hy = (int32_t)j;
        } else if (*p < p0 && p0 >= b0 + bs && *p < b0 + bs) {
            *p = b0 + bs;
            side = x_axis ? 0 : 2;
            *hx = -1;
            *hy = (int32_t)j;
        }
    }
    return side;
}

/* Carried: an actor standing on a platform's top, where the platform was
 * before this tick's move, moves with it. Platforms move first. */
static void carry(const struct world *w, int32_t *x, int32_t *y, int32_t W,
                  int32_t H)
{
    for (uint32_t j = 0; j < w->np; j++) {
        if (!vm_truthy(w->vm, w->pon[j]))
            continue;
        int32_t ddx = fx(w->pdx[j]), ddy = fx(w->pdy[j]);
        int32_t bx = fx(w->px[j]) - ddx, by = fx(w->py[j]) - ddy;
        int32_t foot = *y + H - by;
        if (foot >= -(1 << 16) && foot <= (1 << 16) &&
            *x < bx + fx(w->pw[j]) && bx < *x + W) {
            *x += ddx;
            *y = by + ddy - H;
            return;
        }
    }
}

#define HIT_CRUSH 8

/* The nearest air to column px in the ring r out from the box x0..x1,
 * y0..y1 (field pixels): its top first, then its sides from the top down,
 * then its bottom; 1 with it in *ax, *ay. Only the field's pixels are
 * tried, since there is no air outside it; each counted in *checked. */
static int air_near(const struct world *w, int32_t px,
                    int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t r,
                    int32_t *ax, int32_t *ay, uint32_t *checked)
{
    int32_t top = y0 - r, bottom = y1 + r, left = x0 - r, right = x1 + r;
    int32_t best = -1;
    int32_t fx1 = w->fx0 + w->cw.w - 1, fy1 = w->fy0 + w->cw.h - 1;
    /* the top, nearest the cell first */
    int32_t from = left > w->fx0 ? left : w->fx0;
    int32_t to = right < fx1 ? right : fx1;
    if (top < w->fy0 || top > fy1)
        to = from - 1;
    for (int32_t x = from; x <= to; x++) {
        (*checked)++;
        int32_t d = x > px ? x - px : px - x;
        if ((best < 0 || d < best) && field_at(w, x, top) == 0) {
            best = d;
            *ax = x;
            *ay = top;
        }
    }
    if (best >= 0)
        return 1;
    from = top + 1 > w->fy0 ? top + 1 : w->fy0;
    to = bottom < fy1 ? bottom : fy1;
    for (int32_t y = from; y <= to; y++) {
        *checked += 2;
        int32_t first = px - left <= right - px ? left : right;
        int32_t other = first == left ? right : left;
        if (field_at(w, first, y) == 0) { *ax = first; *ay = y; return 1; }
        if (field_at(w, other, y) == 0) { *ax = other; *ay = y; return 1; }
    }
    return 0;
}

/* Splashed: the loose cells -- liquids and powders, anything not solid --
 * under the actor's box pushed out to the nearest air just outside it,
 * above first, then beside it, a ring or two out when the first is full;
 * a cell with no air near stays where it is. For a pool with splash set. */
static int splash(const struct world *w, int32_t X, int32_t Y, int32_t W,
                  int32_t H, uint32_t *checked)
{
    int32_t x0 = X >> 16, y0 = Y >> 16;
    int32_t x1 = (int32_t)(((int64_t)X + W - 1) >> 16);
    int32_t y1 = (int32_t)(((int64_t)Y + H - 1) >> 16);
    /* the box's pixels on the field: only those hold cells */
    int32_t lx0 = x0 > w->fx0 ? x0 : w->fx0;
    int32_t ly0 = y0 > w->fy0 ? y0 : w->fy0;
    int32_t lx1 = x1 < w->fx0 + w->cw.w - 1 ? x1 : w->fx0 + w->cw.w - 1;
    int32_t ly1 = y1 < w->fy0 + w->cw.h - 1 ? y1 : w->fy0 + w->cw.h - 1;
    for (int32_t py = ly0; py <= ly1; py++) {
        /* paid a row at a time, so a large splash is stopped */
        if (*checked >= 8192) {
            if (!vm_spend(w->vm, *checked / 8))
                return 0;
            *checked %= 8;
        }
        for (int32_t px = lx0; px <= lx1; px++) {
            int32_t m = field_at(w, px, py);
            (*checked)++;
            if (m <= 0 || (w->cw.flags[m] & CELL_SOLID))
                continue;
            for (int32_t r = 1; r <= 3; r++) {
                int32_t ax = 0, ay = 0;
                if (air_near(w, px, x0, y0, x1, y1, r, &ax, &ay, checked)) {
                    w->cw.c[(ay - w->fy0) * w->cw.w + (ax - w->fx0)] =
                        (uint8_t)m;
                    w->cw.c[(py - w->fy0) * w->cw.w + (px - w->fx0)] = 0;
                    break;
                }
            }
        }
    }
    return 1;
}

/* Pushed: a platform that moved into the actor this tick -- clear of it
 * where the platform was before, overlapping it now -- shoves it out
 * ahead, along the way the platform crossed into it, swept against the
 * map as any move is. One with a wall behind it cannot be shoved out and
 * is crushed: HIT.CRUSH, with the platform. One-way platforms push
 * nothing. Platforms move first. */
static void push(const struct world *w, struct contacts *c, uint32_t i,
                 int32_t *X, int32_t *Y, int32_t W, int32_t H, int32_t smask,
                 int32_t omask, uint32_t *checked)
{
    if (w->oneway)
        return;
    for (uint32_t j = 0; j < w->np; j++) {
        if (!vm_truthy(w->vm, w->pon[j]))
            continue;
        int32_t bx = fx(w->px[j]), by = fx(w->py[j]);
        int32_t bw = fx(w->pw[j]), bh = fx(w->ph[j]);
        int32_t ddx = fx(w->pdx[j]), ddy = fx(w->pdy[j]);
        int32_t ox = bx - ddx, oy = by - ddy;
        if (!(*X < bx + bw && bx < *X + W && *Y < by + bh && by < *Y + H))
            continue;
        int across = ox + bw <= *X || ox >= *X + W;
        int down = oy + bh <= *Y || oy >= *Y + H;
        if (!across && !down)
            continue;               /* inside it before: left as it is */
        int32_t hx, hy, side, to;
        if (across) {
            to = ddx > 0 ? bx + bw : bx - W;
            side = sweep(w->vm, X, to - *X, W, *Y, H, smask, omask, 1, &hx,
                         &hy, checked);
        } else {
            to = ddy > 0 ? by + bh : by - H;
            side = sweep(w->vm, Y, to - *Y, H, *X, W, smask, omask, 0, &hx,
                         &hy, checked);
        }
        if (side >= 0)
            contact(c, i, HIT_CRUSH, -1, (int32_t)j);
    }
}

/* In the field after its move: drag and lift from the liquid it is in, the
 * first harmful material it touches (HIT.TOUCH), and, if it is made of a
 * material, the rules' reactions with the cells just outside it -- the
 * actor as a, the cell becoming d -- reported once (HIT.REACT, with c,
 * what the actor would become, for the cart to decide) */
static void in_field(const struct world *w, struct contacts *c, uint32_t i,
                     int32_t X, int32_t Y, int32_t W, int32_t H, int32_t *DX,
                     int32_t *DY, int32_t g, int32_t density, int32_t mat,
                     uint32_t *checked)
{
    int32_t x0 = X >> 16, y0 = Y >> 16;
    int32_t x1 = (int32_t)(((int64_t)X + W - 1) >> 16);
    int32_t y1 = (int32_t)(((int64_t)Y + H - 1) >> 16);
    /* every pixel it covers counts, but only those on the field are read */
    uint32_t cells = (uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1);
    uint32_t wet = 0, heavy = 0;
    int32_t harm = -1;
    int32_t fx1 = w->fx0 + w->cw.w - 1, fy1 = w->fy0 + w->cw.h - 1;
    int32_t qx0 = x0 < w->fx0 ? w->fx0 : x0, qx1 = x1 > fx1 ? fx1 : x1;
    int32_t qy0 = y0 < w->fy0 ? w->fy0 : y0, qy1 = y1 > fy1 ? fy1 : y1;
    for (int32_t py = qy0; py <= qy1; py++)
        for (int32_t px = qx0; px <= qx1; px++) {
            int32_t m = field_at(w, px, py);
            (*checked)++;
            if (m < 0)
                continue;
            uint8_t fl = w->cw.flags[m];
            if (fl & CELL_LIQUID) {
                wet++;
                if (w->cw.density[m] > heavy)
                    heavy = w->cw.density[m];
            }
            if ((fl & CELL_HARM) && harm < 0)
                harm = m;
        }
    if (wet) {
        /* as much as it is in: half its speed lost when wholly in, and
         * lifted by the liquid's density over its own */
        int32_t in = (int32_t)(((int64_t)wet << 16) / cells);
        *DX -= (int32_t)(((int64_t)*DX * in) >> 17);
        *DY -= (int32_t)(((int64_t)*DY * in) >> 17);
        if (density > 0)
            *DY -= (int32_t)((((int64_t)g * in) >> 16) * (int32_t)heavy /
                             density);
    }
    if (harm >= 0)
        contact(c, i, 6, -2, harm);
    if (mat < 0 || mat > 255)
        return;
    /* the ring just outside it, clockwise from its top left */
    int32_t across = x1 - x0 + 1, down = y1 - y0 + 1;
    for (int32_t k = 0; k < 2 * (across + down); k++) {
        int32_t px, py;
        if (k < across) {
            px = x0 + k;
            py = y0 - 1;
        } else if (k < across + down) {
            px = x1 + 1;
            py = y0 + k - across;
        } else if (k < 2 * across + down) {
            px = x1 - (k - across - down);
            py = y1 + 1;
        } else {
            px = x0 - 1;
            py = y1 - (k - 2 * across - down);
        }
        int32_t m = field_at(w, px, py);
        (*checked)++;
        if (m < 0)
            continue;
        for (int q = 0; q < 64; q++) {
            if (!w->cw.chance[q] || w->cw.a[q] != mat || w->cw.b[q] != m)
                continue;
            if ((core_random(w->vm) & 0xff) < w->cw.chance[q]) {
                w->cw.c[(py - w->fy0) * w->cw.w + (px - w->fx0)] = w->cw.d[q];
                contact(c, i, 7, -2, w->cw.c2[q]);
                return;
            }
            break;
        }
    }
}

/* Where an actor's feet rest on a ramp of 45 degrees: the map cells whose
 * flags share a bit with up rise to the right (/), down's to the left (\).
 * A ramp is not solid: an actor walks into it, and is set on its surface
 * at its feet's middle, as it sinks below it or comes within 4 pixels
 * above it -- so it stays on a ramp walking down -- only while not going
 * up. 1 with Y and DY set when it was. */
#define SLOPE_GLUE (4 << 16)

static int on_slope(const struct vm *vm, int32_t X, int32_t *Y, int32_t W,
                    int32_t H, int32_t *DY, int32_t up, int32_t down,
                    int32_t *hx, int32_t *hy)
{
    int32_t foot = X + W / 2, feet = *Y + H;
    int32_t cx = foot >> CELL_SHIFT, px = (foot >> 16) - (cx << 3);
    if ((!up && !down) || *DY < 0)
        return 0;
    for (int k = 0; k < 2; k++) {
        int32_t cy = ((feet - 1) >> CELL_SHIFT) + k;
        int rising = solid(vm, cx, cy, up);
        if (!rising && !solid(vm, cx, cy, down))
            continue;
        int32_t surf = ((cy << 3) + (rising ? 7 - px : px)) << 16;
        if (feet >= surf - SLOPE_GLUE) {
            *Y = surf - H;
            *DY = 0;
            *hx = cx;
            *hy = cy;
            return 1;
        }
    }
    return 0;
}

/* actors.move(p, solid, trigger, out, [oneway]): as well as the map, the
 * pool's platforms (another pool, which moves first) and its cells field,
 * both named in its fields. Map cells whose flags share a bit with oneway
 * stop an actor only coming down onto them, as do the platforms when
 * their pool's oneway is set: it jumps up through them, and lands on top */
NATIVE(actors_move)
{
    struct value f[A_FIELDS];
    uint32_t n;
    int32_t smask, tmask, omask;
    struct world wd;
    if (!actors(vm, a[0], f, &n, "actors.move") ||
        !whole_arg(vm, a, argc, 1, "actors.move", &smask) ||
        !whole_arg(vm, a, argc, 2, "actors.move", &tmask) ||
        !whole_arg(vm, a, argc, 4, "actors.move", &omask) ||
        !world_of(vm, f, &wd))
        return 0;
    if (a[3].tag != T_I32)
        return vm_fail(vm, "actors.move() writes its contacts to an "
                       "Int32Array", "", "");
    struct contacts c = { I32(vm, a[3])->d, I32(vm, a[3])->len / 4, 0 };
    int32_t g = fx(f[A_GRAVITY]), drag = fx(f[A_DRAG]);
    int32_t density = is_num(f[A_DENSITY]) ? floor_int(f[A_DENSITY]) : 0;
    struct value *on = ELEMS(vm, f[A_ON]), *x = ELEMS(vm, f[A_X]);
    struct value *y = ELEMS(vm, f[A_Y]), *dx = ELEMS(vm, f[A_DX]);
    struct value *dy = ELEMS(vm, f[A_DY]), *w = ELEMS(vm, f[A_W]);
    struct value *h = ELEMS(vm, f[A_H]), *cell = ELEMS(vm, f[A_CELL]);
    struct value *mat = ELEMS(vm, f[A_MAT]);
    int splashes = vm_truthy(vm, f[A_SPLASH]);
    int32_t up = is_num(f[A_SLOPE_UP]) ? floor_int(f[A_SLOPE_UP]) : 0;
    int32_t down = is_num(f[A_SLOPE_DOWN]) ? floor_int(f[A_SLOPE_DOWN]) : 0;
    int32_t ladders = is_num(f[A_LADDERS]) ? floor_int(f[A_LADDERS]) : 0;
    uint32_t moved = 0, checked = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!vm_truthy(vm, on[i]))
            continue;
        moved++;
        int32_t X = fx(x[i]), Y = fx(y[i]), W = fx(w[i]), H = fx(h[i]);
        int32_t hx, hy, side, s2, p0;
        if (W < 1) W = 1;
        if (H < 1) H = 1;
        /* on a ladder, by the actor's middle, gravity lets go: the game
         * moves it up and down */
        int32_t gi = ladders && solid(vm, (X + W / 2) >> CELL_SHIFT,
                                      (Y + H / 2) >> CELL_SHIFT, ladders)
                     ? 0 : g;
        int32_t DX = dragged(fx(dx[i]), drag), DY = dragged(fx(dy[i]) + gi, drag);
        if (wd.np) {
            carry(&wd, &X, &Y, W, H);
            push(&wd, &c, i, &X, &Y, W, H, smask, omask, &checked);
        }
        p0 = X;
        side = sweep(vm, &X, DX, W, Y, H, smask, omask, 1, &hx, &hy,
                     &checked);
        if ((s2 = obstacles(&wd, p0, &X, W, Y, H, 1, &hx, &hy, &checked)) >= 0)
            side = s2;
        if (side >= 0)
            contact(&c, i, side, hx, hy);
        p0 = Y;
        side = sweep(vm, &Y, DY, H, X, W, smask, omask, 0, &hx, &hy,
                     &checked);
        if ((s2 = obstacles(&wd, p0, &Y, H, X, W, 0, &hx, &hy, &checked)) >= 0)
            side = s2;
        /* a ramp the feet meet, where it is no lower than any floor the
         * sweep stopped them on: HIT.DOWN, on its feet */
        int32_t sy = Y, sdy = DY, shx, shy;
        if (on_slope(vm, X, &sy, W, H, &sdy, up, down, &shx, &shy) &&
            (side < 0 || sy <= Y)) {
            Y = sy;
            DY = sdy;
            hx = shx;
            hy = shy;
            side = 3;
        }
        if (side >= 0)
            contact(&c, i, side, hx, hy);
        if (wd.field) {
            in_field(&wd, &c, i, X, Y, W, H, &DX, &DY, g, density,
                     is_num(mat[i]) ? floor_int(mat[i]) : -1, &checked);
            if (splashes && !splash(&wd, X, Y, W, H, &checked))
                return 0;
        }
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
        /* the checks, and the platforms, are paid as they come, so a
         * runaway move is stopped */
        checked += wd.np * 4;
        if (checked >= 8192) {
            if (!vm_spend(vm, checked / 8))
                return 0;
            checked %= 8;
        }
    }
    /* measured on the ARM rig (.build/arcade-rig, sysprobe3): about 20
     * units an actor moved, most of it reading and writing its fields */
    vm_charge(vm, 10 + n + moved * 20 + c.n * 8 + checked / 8);
    *out = mk(T_INT, (int32_t)c.n);
    return 1;
}

/* An actor's box for overlaps: its left and right, top and bottom edges,
 * 16.16, and its index */
struct box
{
    int32_t x0, x1, y0, y1;
    uint32_t i;
};

/* Boxes in order of their left edges: Shell's sort, gaps 3k + 1 */
static void boxes_sort(struct box *b, uint32_t n)
{
    uint32_t gap = 1;
    while (gap < n / 3)
        gap = gap * 3 + 1;
    for (; gap > 0; gap /= 3)
        for (uint32_t i = gap; i < n; i++) {
            struct box t = b[i];
            uint32_t j = i;
            for (; j >= gap && b[j - gap].x0 > t.x0; j -= gap)
                b[j] = b[j - gap];
            b[j] = t;
        }
}

/* actors.overlaps(a, b, out): the pairs of live actors, one of a and one
 * of b (or two of a, each pair once, when b is a), whose boxes overlap,
 * into out two numbers a pair, in order of a's actors and then b's; the
 * pairs written. b's are sorted by their left edges, so each of a's is
 * tried against only those near it across */
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
    /* one block: a second scratch() could collect the first */
    struct box *bb = scratch(vm, nb * (sizeof(struct box) + sizeof(uint32_t))
                                 + 1);
    if (!bb)
        return 0;
    uint32_t *found = (uint32_t *)(bb + nb);
    const struct value *bon = ELEMS(vm, fb[A_ON]), *bx = ELEMS(vm, fb[A_X]);
    const struct value *by = ELEMS(vm, fb[A_Y]), *bw = ELEMS(vm, fb[A_W]);
    const struct value *bh = ELEMS(vm, fb[A_H]);
    uint32_t live = 0;
    int64_t widest = 0;
    for (uint32_t j = 0; j < nb; j++) {
        if (!vm_truthy(vm, bon[j]))
            continue;
        struct box *b = &bb[live++];
        b->x0 = fx(bx[j]);
        b->y0 = fx(by[j]);
        b->x1 = b->x0 + fx(bw[j]);
        b->y1 = b->y0 + fx(bh[j]);
        b->i = j;
        if ((int64_t)b->x1 - b->x0 > widest)
            widest = (int64_t)b->x1 - b->x0;
    }
    boxes_sort(bb, live);
    int32_t *o = I32(vm, a[2])->d;
    uint32_t cap = I32(vm, a[2])->len / 2, k = 0, tests = 0;
    for (uint32_t i = 0; i < na && k < cap; i++) {
        if (!vm_truthy(vm, ELEMS(vm, fa[A_ON])[i]))
            continue;
        int32_t ax = fx(ELEMS(vm, fa[A_X])[i]), ay = fx(ELEMS(vm, fa[A_Y])[i]);
        int32_t aw = fx(ELEMS(vm, fa[A_W])[i]), ah = fx(ELEMS(vm, fa[A_H])[i]);
        /* the first whose left edge could reach this one: none further
         * left is wider than the widest */
        int64_t from = (int64_t)ax - widest;
        uint32_t lo = 0, hi = live, n = 0;
        while (lo < hi) {
            uint32_t m = (lo + hi) / 2;
            if (bb[m].x0 <= from)
                lo = m + 1;
            else
                hi = m;
        }
        for (uint32_t m = lo; m < live && bb[m].x0 < (int64_t)ax + aw; m++) {
            const struct box *b = &bb[m];
            tests++;
            if (ax < b->x1 && ay < b->y1 && b->y0 < ay + ah &&
                (!same || b->i > i))
                found[n++] = b->i;
        }
        /* written in b's order, as a pass over all of them would */
        for (uint32_t s = 1; s < n; s++) {
            uint32_t t = found[s], u = s;
            for (; u > 0 && found[u - 1] > t; u--, tests++)
                found[u] = found[u - 1];
            found[u] = t;
        }
        for (uint32_t s = 0; s < n && k < cap; s++, k++) {
            o[k * 2] = (int32_t)i;
            o[k * 2 + 1] = (int32_t)found[s];
        }
        if (tests > 8192) {
            if (!vm_spend(vm, tests / 2))
                return 0;
            tests = 0;
        }
    }
    /* measured on the rig: reading each actor of both pools costs about
     * 14 units, far more than the tests between them */
    vm_charge(vm, 10 + tests / 2 + (live + na) * 14);
    *out = mk(T_INT, (int32_t)k);
    return 1;
}

/* actors.zones(p, objs, n, out): which of n map objects (from mapobj(),
 * five numbers each: kind, x, y, w, h) each actor's centre is in. Going
 * into one is reported HIT.ENTER and leaving it HIT.LEAVE, with the
 * object's index and kind; the pool's zone array keeps where each actor
 * is. Returns the contacts written. */
NATIVE(actors_zones)
{
    (void)argc;
    struct value f[A_FIELDS];
    uint32_t n;
    if (!actors(vm, a[0], f, &n, "actors.zones"))
        return 0;
    struct column objs;
    int32_t k = is_num(a[2]) ? floor_int(a[2]) : -1;
    if (!column_of(vm, a[1], &objs) || objs.tag == T_INT || k < 0 ||
        (uint32_t)k > objs.len / 5)
        return vm_fail(vm, "actors.zones() needs the objects from mapobj() "
                       "and how many", "", "");
    if (a[3].tag != T_I32)
        return vm_fail(vm, "actors.zones() writes its contacts to an "
                       "Int32Array", "", "");
    struct contacts c = { I32(vm, a[3])->d, I32(vm, a[3])->len / 4, 0 };
    struct value *on = ELEMS(vm, f[A_ON]), *zone = ELEMS(vm, f[A_ZONE]);
    struct value *x = ELEMS(vm, f[A_X]), *y = ELEMS(vm, f[A_Y]);
    struct value *w = ELEMS(vm, f[A_W]), *h = ELEMS(vm, f[A_H]);
    for (uint32_t i = 0; i < n; i++) {
        if (!vm_truthy(vm, on[i]))
            continue;
        int32_t cx = (fx(x[i]) + fx(w[i]) / 2) >> 16;
        int32_t cy = (fx(y[i]) + fx(h[i]) / 2) >> 16;
        int32_t in = -1;
        for (int32_t j = 0; j < k && in < 0; j++) {
            uint32_t o = (uint32_t)j * 5;
            int32_t ox = column_at(&objs, o + 1, 0);
            int32_t oy = column_at(&objs, o + 2, 0);
            if (cx >= ox && cx < ox + column_at(&objs, o + 3, 0) &&
                cy >= oy && cy < oy + column_at(&objs, o + 4, 0))
                in = j;
        }
        int32_t was = is_num(zone[i]) ? floor_int(zone[i]) : -1;
        if (in == was)
            continue;
        if (was >= 0 && was < k)
            contact(&c, i, 5, was, column_at(&objs, (uint32_t)was * 5, 0));
        if (in >= 0)
            contact(&c, i, 4, in, column_at(&objs, (uint32_t)in * 5, 0));
        zone[i] = mk(T_INT, in);
    }
    /* measured on the rig: about 6 units an actor, and 2 a zone it is
     * tried against */
    vm_charge(vm, 10 + n * (6 + (uint32_t)k * 2) + c.n * 2);
    *out = mk(T_INT, (int32_t)c.n);
    return 1;
}

NATIVE(actors_draw)
{
    (void)out; (void)argc;
    struct value f[A_FIELDS];
    uint32_t n;
    if (!actors(vm, a[0], f, &n, "actors.draw"))
        return 0;
    struct column on, x, y, sp, flip, frames, speed;
    column_of(vm, f[A_ON], &on);
    column_of(vm, f[A_X], &x);
    column_of(vm, f[A_Y], &y);
    column_of(vm, f[A_SP], &sp);
    column_of(vm, f[A_FLIP], &flip);
    column_of(vm, f[A_FRAMES], &frames);
    column_of(vm, f[A_SPEED], &speed);
    core_draw_sprites(vm, &on, &sp, &x, &y, &flip, &frames, &speed, n);
    return 1;
}

/* ---- circles ------------------------------------------------------------- *
 * An actor as a circle: centred in its box, half its width across. They
 * bounce off one another and off the map at the true angle -- a ball off
 * another ball, a peg, a brick's corner. give is how much of the speed
 * into a surface comes back out of it: 1 (the default) all of it, 0 none. */

static uint64_t root(uint64_t v)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else
            r >>= 1;
        bit >>= 2;
    }
    return r;
}

static int give_arg(struct vm *vm, struct value *a, int argc, int i,
                    const char *fn, int32_t *give)
{
    *give = 65536;
    if (i >= argc || a[i].tag == T_NULL)
        return 1;
    if (!is_num(a[i]))
        return vm_fail(vm, fn, "() needs a number for give, not a ",
                       vm_typename(a[i].tag));
    *give = fx(a[i]);
    if (*give < 0)
        *give = 0;
    if (*give > 65536)
        *give = 65536;
    return 1;
}

/* (*vx, *vy) going into a surface whose normal (nx, ny), 16.16 and of
 * length one, points out of it: the part along the normal turned back,
 * give of it kept */
static void rebound(int32_t *vx, int32_t *vy, int32_t nx, int32_t ny,
                    int32_t give)
{
    int64_t along = ((int64_t)*vx * nx + (int64_t)*vy * ny) >> 16;
    if (along >= 0)
        return;
    int64_t k = (along * (65536 + give)) >> 16;
    *vx -= (int32_t)((k * nx) >> 16);
    *vy -= (int32_t)((k * ny) >> 16);
}

/* (dx, dy) as a normal of length one, 16.16, and its length; (1, 0) when
 * it has none */
static int32_t normal(int64_t dx, int64_t dy, int32_t *nx, int32_t *ny)
{
    /* halved until the squares fit; the length doubled back after */
    int shift = 0;
    while (dx > INT32_MAX || dx < -INT32_MAX || dy > INT32_MAX ||
           dy < -INT32_MAX) {
        dx /= 2;
        dy /= 2;
        shift++;
    }
    int64_t d = (int64_t)root((uint64_t)(dx * dx) + (uint64_t)(dy * dy));
    if (!d) {
        *nx = 65536;
        *ny = 0;
        return 0;
    }
    *nx = (int32_t)((dx << 16) / d);
    *ny = (int32_t)((dy << 16) / d);
    d <<= shift;
    return d > INT32_MAX ? INT32_MAX : (int32_t)d;
}

/* actors.bounce(a, b, out, [give]): live circles of a that touch circles
 * of b, pushed apart and bounced. When b is a, both of a pair move, as
 * balls of one weight do, and each pair is found once; otherwise b's
 * stand still, as pegs and bumpers, and only a's give way. The pairs go
 * into out, two numbers each as actors.overlaps writes them; the pairs
 * written */
NATIVE(actors_bounce)
{
    struct value fa[A_FIELDS], fb[A_FIELDS];
    uint32_t na, nb;
    int32_t give;
    if (!actors(vm, a[0], fa, &na, "actors.bounce") ||
        !actors(vm, a[1], fb, &nb, "actors.bounce") ||
        !give_arg(vm, a, argc, 3, "actors.bounce", &give))
        return 0;
    if (a[2].tag != T_I32)
        return vm_fail(vm, "actors.bounce() writes its pairs to an "
                       "Int32Array", "", "");
    int same = a[0].v == a[1].v;
    struct box *bb = scratch(vm, nb * sizeof(struct box) + 1);
    if (!bb)
        return 0;
    struct value *bon = ELEMS(vm, fb[A_ON]), *bx = ELEMS(vm, fb[A_X]);
    struct value *by = ELEMS(vm, fb[A_Y]), *bw = ELEMS(vm, fb[A_W]);
    struct value *bdx = ELEMS(vm, fb[A_DX]), *bdy = ELEMS(vm, fb[A_DY]);
    struct value *aon = ELEMS(vm, fa[A_ON]), *ax = ELEMS(vm, fa[A_X]);
    struct value *ay = ELEMS(vm, fa[A_Y]), *aw = ELEMS(vm, fa[A_W]);
    struct value *adx = ELEMS(vm, fa[A_DX]), *ady = ELEMS(vm, fa[A_DY]);
    /* b's circles by their left edges, as actors.overlaps sorts boxes */
    uint32_t live = 0;
    int64_t widest = 0;
    for (uint32_t j = 0; j < nb; j++) {
        if (!vm_truthy(vm, bon[j]))
            continue;
        struct box *b = &bb[live++];
        b->x0 = fx(bx[j]);
        b->x1 = b->x0 + fx(bw[j]);
        b->i = j;
        if ((int64_t)b->x1 - b->x0 > widest)
            widest = (int64_t)b->x1 - b->x0;
    }
    boxes_sort(bb, live);
    int32_t *o = I32(vm, a[2])->d;
    uint32_t cap = I32(vm, a[2])->len / 2, k = 0, tests = 0;
    for (uint32_t i = 0; i < na; i++) {
        if (!vm_truthy(vm, aon[i]))
            continue;
        int32_t ra = fx(aw[i]) / 2;
        int64_t from = (int64_t)fx(ax[i]) - widest;
        uint32_t lo = 0, hi = live;
        while (lo < hi) {
            uint32_t m = (lo + hi) / 2;
            if (bb[m].x0 <= from)
                lo = m + 1;
            else
                hi = m;
        }
        for (uint32_t m = lo; m < live; m++) {
            uint32_t j = bb[m].i;
            /* past this circle's right edge, none further can touch */
            if (bb[m].x0 >= (int64_t)fx(ax[i]) + 2 * ra)
                break;
            if (same && j <= i)
                continue;
            tests++;
            int32_t rb = fx(bw[j]) / 2;
            int64_t dx = ((int64_t)fx(ax[i]) + ra) - ((int64_t)fx(bx[j]) + rb);
            int64_t dy = ((int64_t)fx(ay[i]) + ra) - ((int64_t)fx(by[j]) + rb);
            int64_t reach = (int64_t)ra + rb;
            /* reach is within 31 bits: further apart cannot touch, and
             * nearer, the squares fit */
            if (dx > INT32_MAX || dx < -INT32_MAX || dy > INT32_MAX ||
                dy < -INT32_MAX || dx * dx + dy * dy >= reach * reach)
                continue;
            int32_t nx, ny, d = normal(dx, dy, &nx, &ny);
            int32_t over = (int32_t)(reach - d);
            int32_t vax = fx(adx[i]), vay = fx(ady[i]);
            if (same) {
                /* half the overlap each; the meeting speed along the line
                 * between them shared, give of it kept */
                int32_t half = over / 2;
                ax[i] = mk(T_FIX, fx(ax[i]) + (int32_t)(((int64_t)half * nx) >> 16));
                ay[i] = mk(T_FIX, fx(ay[i]) + (int32_t)(((int64_t)half * ny) >> 16));
                bx[j] = mk(T_FIX, fx(bx[j]) - (int32_t)(((int64_t)half * nx) >> 16));
                by[j] = mk(T_FIX, fx(by[j]) - (int32_t)(((int64_t)half * ny) >> 16));
                int32_t vbx = fx(bdx[j]), vby = fx(bdy[j]);
                int64_t along = ((int64_t)(vax - vbx) * nx +
                                 (int64_t)(vay - vby) * ny) >> 16;
                if (along < 0) {
                    int64_t jn = (along * (65536 + give)) >> 17;
                    vax -= (int32_t)((jn * nx) >> 16);
                    vay -= (int32_t)((jn * ny) >> 16);
                    vbx += (int32_t)((jn * nx) >> 16);
                    vby += (int32_t)((jn * ny) >> 16);
                    bdx[j] = mk(T_FIX, vbx);
                    bdy[j] = mk(T_FIX, vby);
                }
            } else {
                ax[i] = mk(T_FIX, fx(ax[i]) + (int32_t)(((int64_t)over * nx) >> 16));
                ay[i] = mk(T_FIX, fx(ay[i]) + (int32_t)(((int64_t)over * ny) >> 16));
                rebound(&vax, &vay, nx, ny, give);
            }
            adx[i] = mk(T_FIX, vax);
            ady[i] = mk(T_FIX, vay);
            if (k < cap) {
                o[k * 2] = (int32_t)i;
                o[k * 2 + 1] = (int32_t)j;
                k++;
            }
        }
        if (tests > 8192) {
            if (!vm_spend(vm, tests / 2))
                return 0;
            tests = 0;
        }
    }
    /* measured on the rig: reading each actor of both pools costs about
     * 16 units */
    vm_charge(vm, 10 + tests / 2 + (live + na) * 16 + k * 8);
    *out = mk(T_INT, (int32_t)k);
    return 1;
}

#define MAX_STEPS 16
#define CELL (8 << 16)

/* actors.bounce_map(p, flags, out, [give]): live circles moved by their
 * speed, the pool's gravity and drag as actors.move takes them, bouncing
 * off map cells whose flags meet flags -- off a face square on, off a
 * corner aslant. Each cell struck goes into out as three numbers: the
 * actor, the cell's column and row; the cells written */
NATIVE(actors_bounce_map)
{
    struct value f[A_FIELDS];
    uint32_t n;
    int32_t mask, give;
    if (!actors(vm, a[0], f, &n, "actors.bounce_map") ||
        !whole_arg(vm, a, argc, 1, "actors.bounce_map", &mask) ||
        !give_arg(vm, a, argc, 3, "actors.bounce_map", &give))
        return 0;
    if (a[2].tag != T_I32)
        return vm_fail(vm, "actors.bounce_map() writes the cells it struck "
                       "to an Int32Array", "", "");
    int32_t *o = I32(vm, a[2])->d;
    uint32_t cap = I32(vm, a[2])->len / 3, k = 0, checked = 0, moved = 0;
    int32_t g = fx(f[A_GRAVITY]), drag = fx(f[A_DRAG]);
    struct value *on = ELEMS(vm, f[A_ON]), *x = ELEMS(vm, f[A_X]);
    struct value *y = ELEMS(vm, f[A_Y]), *dx = ELEMS(vm, f[A_DX]);
    struct value *dy = ELEMS(vm, f[A_DY]), *w = ELEMS(vm, f[A_W]);
    for (uint32_t i = 0; i < n; i++) {
        if (!vm_truthy(vm, on[i]))
            continue;
        moved++;
        int32_t W = fx(w[i]) < 2 ? 2 : fx(w[i]), r = W / 2;
        int64_t cx = (int64_t)fx(x[i]) + r, cy = (int64_t)fx(y[i]) + r;
        int32_t vx = dragged(fx(dx[i]), drag), vy = dragged(fx(dy[i]) + g, drag);
        /* steps of at most half the radius, so a ball never passes
         * through a cell's corner between two looks */
        int32_t span = vx < 0 ? -vx : vx, sy = vy < 0 ? -vy : vy;
        if (sy > span)
            span = sy;
        int32_t steps = r / 2 > 0 ? span / (r / 2) + 1 : 1;
        if (steps > MAX_STEPS)
            steps = MAX_STEPS;
        int32_t hits[4][2], nh = 0;
        for (int32_t s = 0; s < steps; s++) {
            /* paid a step at a time, so a large circle is stopped */
            if (checked >= 8192) {
                if (!vm_spend(vm, checked / 8))
                    return 0;
                checked %= 8;
            }
            cx += vx / steps;
            cy += vy / steps;
            int32_t gx0 = (int32_t)((cx - r) >> CELL_SHIFT);
            int32_t gx1 = (int32_t)((cx + r - 1) >> CELL_SHIFT);
            int32_t gy0 = (int32_t)((cy - r) >> CELL_SHIFT);
            int32_t gy1 = (int32_t)((cy + r - 1) >> CELL_SHIFT);
            /* off the map nothing is solid: only its cells are tried */
            if (gx0 < 0) gx0 = 0;
            if (gy0 < 0) gy0 = 0;
            if (gx1 >= vm->map_w) gx1 = vm->map_w - 1;
            if (gy1 >= vm->map_h) gy1 = vm->map_h - 1;
            for (int32_t gy = gy0; gy <= gy1; gy++)
                for (int32_t gx = gx0; gx <= gx1; gx++) {
                    checked++;
                    if (!solid(vm, gx, gy, mask))
                        continue;
                    /* the cell's point nearest the centre */
                    int64_t l = (int64_t)gx * CELL, t = (int64_t)gy * CELL;
                    int64_t qx = cx < l ? l : cx > l + CELL ? l + CELL : cx;
                    int64_t qy = cy < t ? t : cy > t + CELL ? t + CELL : cy;
                    int64_t ex = cx - qx, ey = cy - qy;
                    int32_t nx, ny, d;
                    if (ex == 0 && ey == 0) {
                        /* the centre inside: out by the nearest face */
                        int64_t left = cx - l, right = l + CELL - cx;
                        int64_t up = cy - t, down = t + CELL - cy;
                        int64_t least = left < right ? left : right;
                        if ((up < down ? up : down) < least) {
                            nx = 0;
                            ny = up < down ? -65536 : 65536;
                            cy = up < down ? t - r : t + CELL + r;
                        } else {
                            ny = 0;
                            nx = left < right ? -65536 : 65536;
                            cx = left < right ? l - r : l + CELL + r;
                        }
                    } else {
                        d = normal(ex, ey, &nx, &ny);
                        if (d >= r)
                            continue;
                        cx += ((int64_t)(r - d) * nx) >> 16;
                        cy += ((int64_t)(r - d) * ny) >> 16;
                    }
                    rebound(&vx, &vy, nx, ny, give);
                    /* each cell once a tick */
                    int seen = 0;
                    for (int32_t h = 0; h < nh; h++)
                        seen |= hits[h][0] == gx && hits[h][1] == gy;
                    if (!seen && nh < 4) {
                        hits[nh][0] = gx;
                        hits[nh++][1] = gy;
                        if (k < cap) {
                            o[k * 3] = (int32_t)i;
                            o[k * 3 + 1] = gx;
                            o[k * 3 + 2] = gy;
                            k++;
                        }
                    }
                }
        }
        x[i] = mk(T_FIX, (int32_t)(cx - r));
        y[i] = mk(T_FIX, (int32_t)(cy - r));
        dx[i] = mk(T_FIX, vx);
        dy[i] = mk(T_FIX, vy);
        if (checked >= 8192) {
            if (!vm_spend(vm, checked / 8))
                return 0;
            checked %= 8;
        }
    }
    /* measured on the rig: about 32 units a circle moved */
    vm_charge(vm, 10 + n + moved * 32 + k * 8 + checked / 8);
    *out = mk(T_INT, (int32_t)k);
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

/* A sprite particle's size: the pool's w and h, 1 to 16, 8 when unset */
static void sprite_size(const struct value *f, int32_t *w, int32_t *h)
{
    *w = is_num(f[P_W]) ? floor_int(f[P_W]) : 0;
    *h = is_num(f[P_H]) ? floor_int(f[P_H]) : 0;
    if (*w < 1 || *w > 16)
        *w = 8;
    if (*h < 1 || *h > 16)
        *h = 8;
}

/* particles.bounds(p, out): the box the live particles cover as drawn --
 * the camera's offset taken off, a sprite its size -- into out, x0 y0 x1
 * y1 inclusive; how many are live, out untouched when none */
NATIVE(particles_bounds)
{
    (void)argc;
    struct value f[P_FIELDS];
    uint32_t n, live = 0;
    if (!particles(vm, a[0], f, &n, "particles.bounds"))
        return 0;
    if (a[1].tag != T_I32 || I32(vm, a[1])->len < 4)
        return vm_fail(vm, "particles.bounds() needs an Int32Array of 4",
                       "", "");
    int32_t w, h;
    sprite_size(f, &w, &h);
    struct value *lf = ELEMS(vm, f[P_LIFE]), *x = ELEMS(vm, f[P_X]);
    struct value *y = ELEMS(vm, f[P_Y]), *sp = ELEMS(vm, f[P_SP]);
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (floor_int(lf[i]) <= 0)
            continue;
        int sprite = is_num(sp[i]) && floor_int(sp[i]) > 0;
        int32_t px = floor_int(x[i]) - vm->d.cam_x;
        int32_t py = floor_int(y[i]) - vm->d.cam_y;
        int32_t qx = px + (sprite ? w - 1 : 0), qy = py + (sprite ? h - 1 : 0);
        if (!live++) {
            x0 = px; y0 = py; x1 = qx; y1 = qy;
        } else {
            if (px < x0) x0 = px;
            if (py < y0) y0 = py;
            if (qx > x1) x1 = qx;
            if (qy > y1) y1 = qy;
        }
    }
    if (live) {
        int32_t *d = I32(vm, a[1])->d;
        d[0] = x0; d[1] = y0; d[2] = x1; d[3] = y1;
    }
    vm_charge(vm, 10 + n / 4);
    *out = mk(T_INT, (int32_t)live);
    return 1;
}

/* A random amount from -range to range, 16.16 */
static int32_t spread(struct vm *vm, int32_t range)
{
    if (range <= 0)
        return 0;
    uint32_t r = core_random(vm) & 0xffff;
    return (int32_t)(((int64_t)range * (int32_t)(r * 2 - 0xffff)) >> 16);
}

/* particles.emit(p, count, x, y, dx, dy, spread, life, c, [sp]): with sp,
 * each is that sprite, flipped at random */
NATIVE(particles_emit)
{
    struct value f[P_FIELDS];
    uint32_t n;
    int32_t count, life, c, sprite = 0;
    if (!particles(vm, a[0], f, &n, "particles.emit") ||
        !whole_arg(vm, a, argc, 1, "particles.emit", &count) ||
        !whole_arg(vm, a, argc, 7, "particles.emit", &life) ||
        !whole_arg(vm, a, argc, 8, "particles.emit", &c) ||
        (argc > 9 && !whole_arg(vm, a, argc, 9, "particles.emit", &sprite)))
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
        ELEMS(vm, f[P_SP])[i] = mk(T_INT, sprite > 0 ? sprite : 0);
        ELEMS(vm, f[P_FLIP])[i] =
            mk(T_INT, sprite > 0 ? (int32_t)(core_random(vm) & 3) : 0);
        made++;
    }
    /* measured on the rig (sysprobe2): about 10 units a particle, its
     * random numbers most of it */
    vm_charge(vm, 10 + (uint32_t)made * 10 + n / 16);
    *out = mk(T_INT, made);
    return 1;
}

/* A cells field particles fall into: its pixels, size and place */
struct dropfield
{
    uint8_t *c;
    int32_t w, h, x0, y0;
    int32_t box[4];             /* the cells settled into, x0 > x1 for none */
};

/* A particle moved from ox, oy to nx, ny (16.16 world pixels) into
 * something in the field, or out through its floor, settles as a cell of
 * its colour -- the colour is the material -- where it was, or the
 * nearest air above or beside that; either way it is gone as a particle.
 * Air is material 0. */
static void settle(struct dropfield *f, struct value *life, uint32_t i,
                   int32_t ox, int32_t oy, int32_t nx, int32_t ny,
                   struct value c, uint32_t *scanned)
{
    int32_t px = (nx >> 16) - f->x0, py = (ny >> 16) - f->y0;
    int32_t qx = (ox >> 16) - f->x0, qy = (oy >> 16) - f->y0;
    int in = px >= 0 && py >= 0 && px < f->w && py < f->h;
    if (in && !f->c[py * f->w + px])
        return;
    /* landed: on something, or at the field's floor */
    if (!in && !(py >= f->h && px >= 0 && px < f->w))
        return;
    /* where it was, brought into the field; taken, the nearest air up its
     * column, or a column or two either side */
    if (qx < 0) qx = 0;
    if (qx >= f->w) qx = f->w - 1;
    if (qy >= f->h) qy = f->h - 1;
    if (qy >= 0) {
        uint8_t m = (uint8_t)(is_num(c) ? floor_int(c) : 0);
        static const int8_t side[5] = { 0, -1, 1, -2, 2 };
        for (int s = 0; s < 5; s++) {
            int32_t cx = qx + side[s];
            if (cx < 0 || cx >= f->w)
                continue;
            int32_t cy = qy;
            while (cy >= 0 && f->c[cy * f->w + cx])
                cy--;
            *scanned += (uint32_t)(qy - cy);
            if (cy >= 0) {
                f->c[cy * f->w + cx] = m;
                if (f->box[0] > f->box[2]) {
                    f->box[0] = f->box[2] = cx;
                    f->box[1] = f->box[3] = cy;
                } else {
                    if (cx < f->box[0]) f->box[0] = cx;
                    if (cx > f->box[2]) f->box[2] = cx;
                    if (cy < f->box[1]) f->box[1] = cy;
                    if (cy > f->box[3]) f->box[3] = cy;
                }
                break;
            }
        }
    }
    life[i] = mk(T_INT, 0);
}

/* For cells.lift(): a particle pool's arrays, checked, its length, and
 * where it places its field (field_x, field_y) */
int particles_arrays(struct vm *vm, struct value p, struct value **arr,
                     uint32_t *n, int32_t *ox, int32_t *oy)
{
    struct value f[P_FIELDS];
    if (!particles(vm, p, f, n, "cells.lift"))
        return 0;
    for (int i = 0; i < P_ARRAYS; i++)
        arr[i] = ELEMS(vm, f[i]);
    *ox = is_num(f[P_FIELD_X]) ? floor_int(f[P_FIELD_X]) : 0;
    *oy = is_num(f[P_FIELD_Y]) ? floor_int(f[P_FIELD_Y]) : 0;
    return 1;
}

/* For cells.step(): the cells of a field under each actor of a pool (or
 * an array of pools) set in bits, a bit a cell, as the pools' field_x
 * and field_y place them; each actor's box, in field cells, into boxes
 * (four numbers, up to cap of them). Returns how many, or -1 with the
 * error set. */
int32_t actors_boxes(struct vm *vm, struct value pools, uint8_t *bits,
                     int32_t w, int32_t h, int32_t *boxes, int32_t cap)
{
    int32_t k = 0;
    uint32_t np = pools.tag == T_ARR ? ARR(vm, pools)->len : 1, set = 0;
    for (uint32_t q = 0; q < np; q++) {
        struct value p = pools.tag == T_ARR ? ELEMS(vm, pools)[q] : pools;
        struct value f[A_FIELDS];
        uint32_t n;
        if (!actors(vm, p, f, &n, "cells.step") || !vm_spend(vm, n / 4))
            return -1;
        int32_t ox = is_num(f[A_FIELD_X]) ? floor_int(f[A_FIELD_X]) : 0;
        int32_t oy = is_num(f[A_FIELD_Y]) ? floor_int(f[A_FIELD_Y]) : 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!vm_truthy(vm, ELEMS(vm, f[A_ON])[i]))
                continue;
            int32_t X = fx(ELEMS(vm, f[A_X])[i]), Y = fx(ELEMS(vm, f[A_Y])[i]);
            int32_t x0 = (X >> 16) - ox, y0 = (Y >> 16) - oy;
            int32_t x1 = ((X + fx(ELEMS(vm, f[A_W])[i]) - 1) >> 16) - ox;
            int32_t y1 = ((Y + fx(ELEMS(vm, f[A_H])[i]) - 1) >> 16) - oy;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 >= w) x1 = w - 1;
            if (y1 >= h) y1 = h - 1;
            if (x0 > x1 || y0 > y1)
                continue;
            for (int32_t y = y0; y <= y1; y++)
                for (int32_t x = x0; x <= x1; x++) {
                    int32_t c = y * w + x;
                    bits[c >> 3] |= (uint8_t)(1 << (c & 7));
                }
            /* paid as it goes: one pool can be listed any number of times */
            set += (uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1);
            if (set >= 8192) {
                if (!vm_spend(vm, set / 8))
                    return -1;
                set %= 8;
            }
            if (k < cap) {
                boxes[k * 4] = x0;
                boxes[k * 4 + 1] = y0;
                boxes[k * 4 + 2] = x1;
                boxes[k * 4 + 3] = y1;
                k++;
            }
        }
    }
    return k;
}

NATIVE(particles_step)
{
    (void)out; (void)argc;
    struct value f[P_FIELDS];
    uint32_t n;
    if (!particles(vm, a[0], f, &n, "particles.step"))
        return 0;
    int32_t g = fx(f[P_GRAVITY]), drag = fx(f[P_DRAG]);
    int32_t floor_y = fx(f[P_FLOOR]), bounce = fx(f[P_BOUNCE]);
    int32_t spin = is_num(f[P_SPIN]) ? floor_int(f[P_SPIN]) : 0;
    /* a quarter turn: flip 0, 1, 3, 2, round again */
    static const uint8_t turned[4] = { 1, 3, 0, 2 };
    struct value *lf = ELEMS(vm, f[P_LIFE]), *x = ELEMS(vm, f[P_X]);
    struct value *y = ELEMS(vm, f[P_Y]), *dx = ELEMS(vm, f[P_DX]);
    struct value *dy = ELEMS(vm, f[P_DY]), *c = ELEMS(vm, f[P_C]);
    struct value *sp = ELEMS(vm, f[P_SP]), *flip = ELEMS(vm, f[P_FLIP]);
    struct dropfield fd = { NULL, 0, 0, 0, 0, { 1, 0, 0, 0 } };
    if (f[P_FIELD].tag == T_IMG) {
        struct img *im = MEM(vm, (uint32_t)f[P_FIELD].v);
        fd.c = im->px;
        fd.w = im->width;
        fd.h = im->height;
        fd.x0 = is_num(f[P_FIELD_X]) ? floor_int(f[P_FIELD_X]) : 0;
        fd.y0 = is_num(f[P_FIELD_Y]) ? floor_int(f[P_FIELD_Y]) : 0;
    }
    uint32_t live = 0, scanned = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t l = floor_int(lf[i]);
        if (l <= 0)
            continue;
        live++;
        lf[i] = mk(T_INT, l - 1);
        /* the columns settle() climbs, paid as they come */
        if (scanned >= 8192) {
            if (!vm_spend(vm, scanned / 8))
                return 0;
            scanned %= 8;
        }
        int32_t DX = dragged(fx(dx[i]), drag), DY = dragged(fx(dy[i]) + g, drag);
        dx[i] = mk(T_FIX, DX);
        dy[i] = mk(T_FIX, DY);
        int32_t ox = fx(x[i]), oy = fx(y[i]);
        int32_t NY = wrap32((int64_t)oy + DY);
        if (floor_y > 0 && DY > 0 && oy < floor_y && NY >= floor_y) {
            int32_t up = (int32_t)(((int64_t)DY * bounce) >> 16);
            if (up >= (1 << 14)) {      /* a quarter pixel a tick */
                NY = floor_y;
                dy[i] = mk(T_FIX, -up);
            }
        }
        int32_t NX = wrap32((int64_t)ox + DX);
        x[i] = mk(T_FIX, NX);
        y[i] = mk(T_FIX, NY);
        if (spin > 0 && is_num(sp[i]) && floor_int(sp[i]) > 0 &&
            (vm->frame + i) % (uint32_t)spin == 0)
            flip[i] = mk(T_INT, turned[floor_int(flip[i]) & 3]);
        if (fd.c)
            settle(&fd, lf, i, ox, oy, NX, NY, c[i], &scanned);
    }
    /* about 7 units a live particle, measured; 2 more over a field */
    if (fd.c && fd.box[0] <= fd.box[2])
        cells_drawn(vm, (uint32_t)f[P_FIELD].v, fd.w, fd.h, fd.box[0],
                    fd.box[1], fd.box[2], fd.box[3]);
    vm_charge(vm, 10 + n / 8 + live * (fd.c ? 9 : 7) + scanned / 8);
    return 1;
}

NATIVE(particles_draw)
{
    (void)out; (void)argc;
    struct value f[P_FIELDS];
    uint32_t n;
    if (!particles(vm, a[0], f, &n, "particles.draw"))
        return 0;
    int32_t w, h;
    sprite_size(f, &w, &h);
    int32_t shade = is_num(f[P_SHADE]) ? floor_int(f[P_SHADE]) : 0;
    struct value *arr[P_ARRAYS];
    for (int i = 0; i < P_ARRAYS; i++)
        arr[i] = ELEMS(vm, f[i]);
    if (shade < 0)
        shade = 0;
    if (shade > 16)
        shade = 16;
    /* culled through the camera this draws by */
    if (vm_truthy(vm, f[P_CULL]) && !vm->d.cam_moved) {
        struct value *lf = arr[P_LIFE], *x = arr[P_X], *y = arr[P_Y];
        struct value *sp = arr[P_SP];
        for (uint32_t i = 0; i < n; i++) {
            if (floor_int(lf[i]) <= 0)
                continue;
            int sprite = is_num(sp[i]) && floor_int(sp[i]) > 0;
            int32_t sx = floor_int(x[i]) - vm->d.cam_x;
            int32_t sy = floor_int(y[i]) - vm->d.cam_y;
            if (sx + (sprite ? w : 1) <= 0 || sx >= SCREEN_W ||
                sy + (sprite ? h : 1) <= 0 || sy >= SCREEN_H)
                lf[i] = mk(T_INT, 0);
        }
        vm_charge(vm, n / 8);
    }
    core_draw_particles(vm, arr, n, w, h, shade);
    return 1;
}
