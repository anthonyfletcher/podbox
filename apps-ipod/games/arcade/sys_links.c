/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade systems: points and links -- ropes, chains, rope bridges, cloth,
 * soft blobs and simple ragdolls, by Verlet integration. A point keeps
 * where it was a tick ago as well as where it is, so its speed is the
 * difference: a step moves each point on by that, plus gravity, less drag,
 * then pulls every link back toward its length a few passes over, then
 * pushes points out of solid map cells. A pinned point stays where the
 * cart puts it -- a rope's end on a hook, or on an actor, set each tick.
 *
 * A pool is a record of plain arrays the cart reads and writes, as actors'
 * are: per point x, y, ox, oy (where it was) and pin; per link a, b (the
 * points) and len; and gravity, drag (a fraction of the speed lost a
 * tick), passes (4 by default; more is stiffer) and the counts used,
 * points and links. Positions are world pixels.
 *
 * Parts, in order:
 *   pools      links.pool, and checking one handed back
 *   building   links.point, links.link
 *   the step   links.step
 *   drawing    links.draw
 ***************************************************************************/
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)

#define MAX_POINTS 1024
#define MAX_LINKS  2048
#define BATCH      256          /* links.draw's segments at a time */

/* ---- pools --------------------------------------------------------------- */

enum { L_X, L_Y, L_OX, L_OY, L_PIN, L_POINT_ARRAYS,
       L_A = L_POINT_ARRAYS, L_B, L_LEN, L_ARRAYS,
       L_GRAVITY = L_ARRAYS, L_DRAG, L_PASSES, L_POINTS, L_LINKS, L_FIELDS };
static const char *const link_keys[L_FIELDS] = {
    "x", "y", "ox", "oy", "pin", "a", "b", "len",
    "gravity", "drag", "passes", "points", "links"
};

#define ELEMS(vm, arr) (BUF(vm, ARR(vm, arr)->buf)->v)

static int32_t fx(struct value v)
{
    return is_num(v) ? wrap32(scaled(v)) : 0;
}

static int32_t whole(struct value v, int32_t dflt)
{
    return is_num(v) ? floor_int(v) : dflt;
}

/* links.pool(points, links) */
NATIVE(links_pool)
{
    (void)argc;
    int32_t np = whole(a[0], 0), nl = whole(a[1], 0);
    if (np < 1 || np > MAX_POINTS || nl < 0 || nl > MAX_LINKS)
        return vm_fail(vm, "links.pool() makes 1 to 1024 points and 0 to "
                       "2048 links", "", "");
    if (!vm_new_record(vm, link_keys, L_FIELDS, out))
        return 0;
    vm->links_shape = OBJ(vm, *out)->shape;
    if (!vm_root(vm, *out))
        return 0;
    for (int f = 0; f < L_FIELDS; f++) {
        struct value v = mk(T_INT, f == L_PASSES ? 4 : 0);
        if (f < L_ARRAYS) {
            uint32_t n = (uint32_t)(f < L_POINT_ARRAYS ? np : nl);
            if (!vm_new_array(vm, n ? n : 1, &v)) {
                vm_unroot(vm);
                return 0;
            }
            for (uint32_t i = 0; i < (n ? n : 1); i++)
                ELEMS(vm, v)[i] = mk(T_INT, 0);
        }
        BUF(vm, OBJ(vm, *out)->buf)->v[f] = v;
    }
    vm_unroot(vm);
    vm_charge(vm, 20 + (uint32_t)(np * 5 + nl * 3) / 4);
    return 1;
}

/* A pool handed back: its fields, and how many points and links its
 * arrays hold */
struct links
{
    struct value f[L_FIELDS];
    uint32_t cap_points, cap_links;
};

static int links_of(struct vm *vm, struct value p, const char *fn,
                    struct links *l)
{
    uint32_t s = p.tag == T_OBJ ? OBJ(vm, p)->shape : 0;
    while (s && s != vm->links_shape)
        s = vm->shapes[s].parent;
    if (!vm->links_shape || s != vm->links_shape)
        return vm_fail(vm, fn, "() needs a pool from links.pool()", "");
    for (int i = 0; i < L_FIELDS; i++)
        l->f[i] = BUF(vm, OBJ(vm, p)->buf)->v[i];
    if (l->f[L_X].tag != T_ARR || l->f[L_A].tag != T_ARR)
        return vm_fail(vm, fn, "() needs a pool whose arrays are as "
                       "links.pool() made them", "");
    for (int i = 0; i < L_ARRAYS; i++) {
        uint32_t want = i < L_POINT_ARRAYS ? ARR(vm, l->f[L_X])->len
                                           : ARR(vm, l->f[L_A])->len;
        if (l->f[i].tag != T_ARR || ARR(vm, l->f[i])->len != want)
            return vm_fail(vm, fn, "() needs a pool whose arrays are as "
                           "links.pool() made them", "");
    }
    l->cap_points = ARR(vm, l->f[L_X])->len;
    l->cap_links = ARR(vm, l->f[L_A])->len;
    return 1;
}

/* The count in field k, kept within the arrays */
static uint32_t used(const struct links *l, int k, uint32_t cap)
{
    int32_t n = whole(l->f[k], 0);
    return n < 0 ? 0 : (uint32_t)n > cap ? cap : (uint32_t)n;
}

static void set_used(struct vm *vm, struct value p, int k, uint32_t n)
{
    BUF(vm, OBJ(vm, p)->buf)->v[k] = mk(T_INT, (int32_t)n);
}

/* ---- building ------------------------------------------------------------ */

/* links.point(p, x, y, [pin]): a point at x, y, at rest; its index, or -1
 * when the pool is full */
NATIVE(links_point)
{
    struct links l;
    if (!links_of(vm, a[0], "links.point", &l))
        return 0;
    uint32_t i = used(&l, L_POINTS, l.cap_points);
    *out = mk(T_INT, -1);
    vm_charge(vm, 4);
    if (i >= l.cap_points)
        return 1;
    struct value x = is_num(a[1]) ? a[1] : mk(T_INT, 0);
    struct value y = is_num(a[2]) ? a[2] : mk(T_INT, 0);
    ELEMS(vm, l.f[L_X])[i] = x;
    ELEMS(vm, l.f[L_Y])[i] = y;
    ELEMS(vm, l.f[L_OX])[i] = x;
    ELEMS(vm, l.f[L_OY])[i] = y;
    ELEMS(vm, l.f[L_PIN])[i] = mk(T_INT, argc > 3 && vm_truthy(vm, a[3]));
    set_used(vm, a[0], L_POINTS, i + 1);
    *out = mk(T_INT, (int32_t)i);
    return 1;
}

/* The square root of a 64-bit square, rounded down */
static uint32_t root(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
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
    return (uint32_t)r;
}

/* The distance between points i and j, 16.16 */
static int32_t apart(const struct value *x, const struct value *y,
                     uint32_t i, uint32_t j)
{
    int64_t dx = (int64_t)fx(x[j]) - fx(x[i]);
    int64_t dy = (int64_t)fx(y[j]) - fx(y[i]);
    return (int32_t)root((uint64_t)(dx * dx + dy * dy));
}

/* links.link(p, i, j, [len]): points i and j held len apart (as far as
 * they are now by default); its index, or -1 when the pool is full */
NATIVE(links_link)
{
    struct links l;
    if (!links_of(vm, a[0], "links.link", &l))
        return 0;
    uint32_t np = used(&l, L_POINTS, l.cap_points);
    int32_t i = whole(a[1], -1), j = whole(a[2], -1);
    if (i < 0 || j < 0 || (uint32_t)i >= np || (uint32_t)j >= np || i == j)
        return vm_fail(vm, "links.link() joins two different points of the "
                       "pool", "", "");
    uint32_t k = used(&l, L_LINKS, l.cap_links);
    *out = mk(T_INT, -1);
    vm_charge(vm, 6);
    if (k >= l.cap_links || !ARR(vm, l.f[L_A])->len)
        return 1;
    ELEMS(vm, l.f[L_A])[k] = mk(T_INT, i);
    ELEMS(vm, l.f[L_B])[k] = mk(T_INT, j);
    ELEMS(vm, l.f[L_LEN])[k] = argc > 3 && is_num(a[3]) ? a[3] :
        mk(T_FIX, apart(ELEMS(vm, l.f[L_X]), ELEMS(vm, l.f[L_Y]),
                        (uint32_t)i, (uint32_t)j));
    set_used(vm, a[0], L_LINKS, k + 1);
    *out = mk(T_INT, (int32_t)k);
    return 1;
}

/* ---- the step ------------------------------------------------------------ */

static int solid_at(const struct vm *vm, int32_t x, int32_t y, int32_t mask)
{
    int32_t cx = x >> 19, cy = y >> 19;
    if (!mask || cx < 0 || cy < 0 || cx >= vm->map_w || cy >= vm->map_h)
        return 0;
    const uint8_t *map = MEM(vm, vm->map), *flags = MEM(vm, vm->flags);
    return flags[map[cy * vm->map_w + cx]] & mask;
}

/* links.step(p, solid): a tick of every point and link, then points out
 * of map cells whose flags share a bit with solid (0, none) */
NATIVE(links_step)
{
    (void)argc;
    struct links l;
    if (!links_of(vm, a[0], "links.step", &l))
        return 0;
    int32_t mask = whole(a[1], 0);
    uint32_t np = used(&l, L_POINTS, l.cap_points);
    uint32_t nl = used(&l, L_LINKS, l.cap_links);
    int32_t g = fx(l.f[L_GRAVITY]), drag = fx(l.f[L_DRAG]);
    int32_t passes = whole(l.f[L_PASSES], 4);
    if (passes < 1) passes = 1;
    if (passes > 16) passes = 16;
    struct value *x = ELEMS(vm, l.f[L_X]), *y = ELEMS(vm, l.f[L_Y]);
    struct value *ox = ELEMS(vm, l.f[L_OX]), *oy = ELEMS(vm, l.f[L_OY]);
    struct value *pin = ELEMS(vm, l.f[L_PIN]);
    /* on by what each moved last tick, less drag, plus gravity */
    for (uint32_t i = 0; i < np; i++) {
        int32_t X = fx(x[i]), Y = fx(y[i]);
        if (vm_truthy(vm, pin[i])) {
            ox[i] = x[i];
            oy[i] = y[i];
            continue;
        }
        int32_t vx = X - fx(ox[i]), vy = Y - fx(oy[i]);
        if (drag) {
            vx -= (int32_t)(((int64_t)vx * drag) >> 16);
            vy -= (int32_t)(((int64_t)vy * drag) >> 16);
        }
        ox[i] = mk(T_FIX, X);
        oy[i] = mk(T_FIX, Y);
        x[i] = mk(T_FIX, X + vx);
        y[i] = mk(T_FIX, Y + vy + g);
    }
    /* each link pulled toward its length, split between its ends, all of
     * it to the free end when the other is pinned */
    struct value *la = ELEMS(vm, l.f[L_A]), *lb = ELEMS(vm, l.f[L_B]);
    struct value *len = ELEMS(vm, l.f[L_LEN]);
    for (int32_t p = 0; p < passes; p++)
        for (uint32_t k = 0; k < nl; k++) {
            int32_t i = whole(la[k], -1), j = whole(lb[k], -1);
            if (i < 0 || j < 0 || (uint32_t)i >= np || (uint32_t)j >= np)
                continue;
            int pi = vm_truthy(vm, pin[i]), pj = vm_truthy(vm, pin[j]);
            if (pi && pj)
                continue;
            int32_t xi = fx(x[i]), yi = fx(y[i]), xj = fx(x[j]), yj = fx(y[j]);
            int64_t dx = (int64_t)xj - xi, dy = (int64_t)yj - yi;
            int32_t d = (int32_t)root((uint64_t)(dx * dx + dy * dy));
            if (d < 16)
                continue;
            /* the share of the gap each end moves, 16.16 of the vector */
            int64_t gap = (int64_t)d - fx(len[k]);
            int64_t s = (gap << 16) / d;
            int64_t mx = (dx * s) >> 16, my = (dy * s) >> 16;
            if (pi) {
                x[j] = mk(T_FIX, (int32_t)(xj - mx));
                y[j] = mk(T_FIX, (int32_t)(yj - my));
            } else if (pj) {
                x[i] = mk(T_FIX, (int32_t)(xi + mx));
                y[i] = mk(T_FIX, (int32_t)(yi + my));
            } else {
                x[i] = mk(T_FIX, (int32_t)(xi + mx / 2));
                y[i] = mk(T_FIX, (int32_t)(yi + my / 2));
                x[j] = mk(T_FIX, (int32_t)(xj - mx / 2));
                y[j] = mk(T_FIX, (int32_t)(yj - my / 2));
            }
        }
    /* out of the walls: back along whichever axis came in */
    if (mask)
        for (uint32_t i = 0; i < np; i++) {
            int32_t X = fx(x[i]), Y = fx(y[i]);
            if (!solid_at(vm, X, Y, mask))
                continue;
            int32_t OX = fx(ox[i]), OY = fx(oy[i]);
            if (!solid_at(vm, OX, Y, mask))
                x[i] = mk(T_FIX, OX);
            else if (!solid_at(vm, X, OY, mask))
                y[i] = mk(T_FIX, OY);
            else {
                x[i] = mk(T_FIX, OX);
                y[i] = mk(T_FIX, OY);
            }
        }
    vm_charge(vm, 10 + np * 4 + nl * (uint32_t)passes * 6 + (mask ? np : 0));
    *out = mk(T_INT, (int32_t)np);
    return 1;
}

/* ---- drawing ------------------------------------------------------------- */

/* links.draw(p, colour): every link as a line, through the camera */
NATIVE(links_draw)
{
    (void)argc; (void)out;
    struct links l;
    if (!links_of(vm, a[0], "links.draw", &l))
        return 0;
    uint32_t np = used(&l, L_POINTS, l.cap_points);
    uint32_t nl = used(&l, L_LINKS, l.cap_links);
    struct value *x = ELEMS(vm, l.f[L_X]), *y = ELEMS(vm, l.f[L_Y]);
    struct value *la = ELEMS(vm, l.f[L_A]), *lb = ELEMS(vm, l.f[L_B]);
    static int32_t seg[BATCH * 4];
    uint32_t n = 0;
    uint32_t c = is_num(a[1]) ? (uint32_t)floor_int(a[1]) : 7;
    for (uint32_t k = 0; k < nl; k++) {
        int32_t i = whole(la[k], -1), j = whole(lb[k], -1);
        if (i < 0 || j < 0 || (uint32_t)i >= np || (uint32_t)j >= np)
            continue;
        seg[n * 4] = floor_int(x[i]);
        seg[n * 4 + 1] = floor_int(y[i]);
        seg[n * 4 + 2] = floor_int(x[j]);
        seg[n * 4 + 3] = floor_int(y[j]);
        if (++n == BATCH) {
            core_draw_lines(vm, seg, n, c);
            n = 0;
        }
    }
    if (n)
        core_draw_lines(vm, seg, n, c);
    return 1;
}
