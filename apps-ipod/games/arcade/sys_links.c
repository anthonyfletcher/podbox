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
 * are: per point x, y, z, ox, oy, oz (where it was) and pin; per link a,
 * b (the points) and len; and gravity, drag (a fraction of the speed lost
 * a tick), passes (4 by default; more is stiffer) and the counts used,
 * points and links. Positions are world pixels; z is depth, toward the
 * viewer below 0, which links measure in but drawing leaves out: 0 for a
 * rope, and how a cloth ripples. With tear above 0 a link stretched more
 * than that share past its length breaks: its a becomes -1, it pulls no
 * more, and broken counts it. links.cloth lays a grid of points and links
 * -- one cloth to a pool, remembered in cols, rows, first and firstlink --
 * and links.fill draws it as cells, each shaded by the way it faces.
 *
 * Parts, in order:
 *   pools      links.pool, and checking one handed back
 *   building   links.point, links.link, links.cloth
 *   the step   links.step
 *   drawing    links.draw, links.fill
 ***************************************************************************/
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)

#define MAX_POINTS 1024
#define MAX_LINKS  2048
#define BATCH      256          /* links.draw's segments at a time */

/* ---- pools --------------------------------------------------------------- */

enum { L_X, L_Y, L_Z, L_OX, L_OY, L_OZ, L_PIN, L_POINT_ARRAYS,
       L_A = L_POINT_ARRAYS, L_B, L_LEN, L_ARRAYS,
       L_GRAVITY = L_ARRAYS, L_DRAG, L_PASSES, L_POINTS, L_LINKS, L_TEAR,
       L_BROKEN, L_COLS, L_ROWS, L_FIRST, L_FIRSTLINK, L_FIELDS };
static const char *const link_keys[L_FIELDS] = {
    "x", "y", "z", "ox", "oy", "oz", "pin", "a", "b", "len",
    "gravity", "drag", "passes", "points", "links", "tear", "broken",
    "cols", "rows", "first", "firstlink"
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
    ELEMS(vm, l.f[L_Z])[i] = mk(T_INT, 0);
    ELEMS(vm, l.f[L_OZ])[i] = mk(T_INT, 0);
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
                     const struct value *z, uint32_t i, uint32_t j)
{
    int64_t dx = (int64_t)fx(x[j]) - fx(x[i]);
    int64_t dy = (int64_t)fx(y[j]) - fx(y[i]);
    int64_t dz = (int64_t)fx(z[j]) - fx(z[i]);
    return (int32_t)root((uint64_t)(dx * dx) + (uint64_t)(dy * dy) +
                         (uint64_t)(dz * dz));
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
                        ELEMS(vm, l.f[L_Z]), (uint32_t)i, (uint32_t)j));
    set_used(vm, a[0], L_LINKS, k + 1);
    *out = mk(T_INT, (int32_t)k);
    return 1;
}

/* links.cloth(p, x, y, cols, rows, gap, [pin]): a grid of cols by rows
 * points gap apart, its top left at x, y, at depth 0, each linked to the
 * one beside it and the one below -- the links across first, row by row,
 * then the links down -- and each cell across both its diagonals, which
 * keeps it from shearing; pin 1 (the default) pins the top row, 2 its two
 * corners, 0 none.
 * Its first point, or -1 when the pool has no room for it */
NATIVE(links_cloth)
{
    struct links l;
    if (!links_of(vm, a[0], "links.cloth", &l))
        return 0;
    int32_t cols = whole(a[3], 0), rows = whole(a[4], 0);
    int32_t pin = argc > 6 ? whole(a[6], 1) : 1;
    if (cols < 2 || rows < 2 || !is_num(a[1]) || !is_num(a[2]) ||
        !is_num(a[5]) || fx(a[5]) <= 0)
        return vm_fail(vm, "links.cloth() needs x, y, at least 2 columns and "
                       "rows, and a gap above 0", "", "");
    uint32_t np = used(&l, L_POINTS, l.cap_points);
    uint32_t nl = used(&l, L_LINKS, l.cap_links);
    uint32_t want = (uint32_t)cols * (uint32_t)rows;
    uint32_t links = (uint32_t)(cols - 1) * (uint32_t)rows +
                     (uint32_t)cols * (uint32_t)(rows - 1) +
                     2 * (uint32_t)(cols - 1) * (uint32_t)(rows - 1);
    *out = mk(T_INT, -1);
    vm_charge(vm, 10);
    if (want > l.cap_points - np || links > l.cap_links - nl ||
        !ARR(vm, l.f[L_A])->len)
        return 1;
    struct value *x = ELEMS(vm, l.f[L_X]), *y = ELEMS(vm, l.f[L_Y]);
    struct value *ox = ELEMS(vm, l.f[L_OX]), *oy = ELEMS(vm, l.f[L_OY]);
    struct value *z = ELEMS(vm, l.f[L_Z]), *oz = ELEMS(vm, l.f[L_OZ]);
    struct value *pn = ELEMS(vm, l.f[L_PIN]);
    struct value *la = ELEMS(vm, l.f[L_A]), *lb = ELEMS(vm, l.f[L_B]);
    struct value *len = ELEMS(vm, l.f[L_LEN]);
    int32_t x0 = fx(a[1]), y0 = fx(a[2]), gap = fx(a[5]);
    int32_t diag = (int32_t)root(2 * (uint64_t)gap * (uint64_t)gap);
    for (int32_t r = 0; r < rows; r++)
        for (int32_t c = 0; c < cols; c++) {
            uint32_t i = np + (uint32_t)(r * cols + c);
            int on = r == 0 && (pin == 1 || (pin == 2 && (c == 0 ||
                                                          c == cols - 1)));
            x[i] = ox[i] = mk(T_FIX, wrap32((int64_t)x0 + (int64_t)gap * c));
            y[i] = oy[i] = mk(T_FIX, wrap32((int64_t)y0 + (int64_t)gap * r));
            z[i] = oz[i] = mk(T_INT, 0);
            pn[i] = mk(T_INT, on);
        }
    uint32_t k = nl;
    for (int32_t r = 0; r < rows; r++)
        for (int32_t c = 0; c + 1 < cols; c++, k++) {
            la[k] = mk(T_INT, (int32_t)np + r * cols + c);
            lb[k] = mk(T_INT, (int32_t)np + r * cols + c + 1);
            len[k] = mk(T_FIX, gap);
        }
    for (int32_t r = 0; r + 1 < rows; r++)
        for (int32_t c = 0; c < cols; c++, k++) {
            la[k] = mk(T_INT, (int32_t)np + r * cols + c);
            lb[k] = mk(T_INT, (int32_t)np + (r + 1) * cols + c);
            len[k] = mk(T_FIX, gap);
        }
    for (int32_t r = 0; r + 1 < rows; r++)
        for (int32_t c = 0; c + 1 < cols; c++, k += 2) {
            int32_t i = (int32_t)np + r * cols + c;
            la[k] = mk(T_INT, i);
            lb[k] = mk(T_INT, i + cols + 1);
            la[k + 1] = mk(T_INT, i + 1);
            lb[k + 1] = mk(T_INT, i + cols);
            len[k] = len[k + 1] = mk(T_FIX, diag);
        }
    set_used(vm, a[0], L_POINTS, np + want);
    set_used(vm, a[0], L_LINKS, nl + links);
    set_used(vm, a[0], L_COLS, (uint32_t)cols);
    set_used(vm, a[0], L_ROWS, (uint32_t)rows);
    set_used(vm, a[0], L_FIRST, np);
    set_used(vm, a[0], L_FIRSTLINK, nl);
    vm_charge(vm, want * 3 + links * 2);
    *out = mk(T_INT, (int32_t)np);
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
    int32_t tear = fx(l.f[L_TEAR]);
    uint32_t broken = 0;
    int32_t passes = whole(l.f[L_PASSES], 4);
    if (passes < 1) passes = 1;
    if (passes > 16) passes = 16;
    struct value *x = ELEMS(vm, l.f[L_X]), *y = ELEMS(vm, l.f[L_Y]);
    struct value *ox = ELEMS(vm, l.f[L_OX]), *oy = ELEMS(vm, l.f[L_OY]);
    struct value *z = ELEMS(vm, l.f[L_Z]), *oz = ELEMS(vm, l.f[L_OZ]);
    struct value *pin = ELEMS(vm, l.f[L_PIN]);
    /* on by what each moved last tick, less drag, plus gravity */
    for (uint32_t i = 0; i < np; i++) {
        int32_t X = fx(x[i]), Y = fx(y[i]), Z = fx(z[i]);
        if (vm_truthy(vm, pin[i])) {
            ox[i] = x[i];
            oy[i] = y[i];
            oz[i] = z[i];
            continue;
        }
        int32_t vx = X - fx(ox[i]), vy = Y - fx(oy[i]), vz = Z - fx(oz[i]);
        if (drag) {
            vx -= (int32_t)(((int64_t)vx * drag) >> 16);
            vy -= (int32_t)(((int64_t)vy * drag) >> 16);
            vz -= (int32_t)(((int64_t)vz * drag) >> 16);
        }
        ox[i] = mk(T_FIX, X);
        oy[i] = mk(T_FIX, Y);
        oz[i] = mk(T_FIX, Z);
        x[i] = mk(T_FIX, X + vx);
        y[i] = mk(T_FIX, Y + vy + g);
        z[i] = mk(T_FIX, Z + vz);
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
            int32_t zi = fx(z[i]), zj = fx(z[j]);
            int64_t dx = (int64_t)xj - xi, dy = (int64_t)yj - yi;
            int64_t dz = (int64_t)zj - zi;
            int32_t d = (int32_t)root((uint64_t)(dx * dx) +
                                      (uint64_t)(dy * dy) +
                                      (uint64_t)(dz * dz));
            /* torn on the first pass, at the stretch the tick left it */
            if (p == 0 && tear > 0 &&
                d > fx(len[k]) + (((int64_t)fx(len[k]) * tear) >> 16)) {
                la[k] = mk(T_INT, -1);
                broken++;
                continue;
            }
            if (d < 16)
                continue;
            /* the share of the gap each end moves, 16.16 of the vector */
            int64_t gap = (int64_t)d - fx(len[k]);
            int64_t s = (gap << 16) / d;
            int64_t mx = (dx * s) >> 16, my = (dy * s) >> 16;
            int64_t mz = (dz * s) >> 16;
            if (pi) {
                x[j] = mk(T_FIX, (int32_t)(xj - mx));
                y[j] = mk(T_FIX, (int32_t)(yj - my));
                z[j] = mk(T_FIX, (int32_t)(zj - mz));
            } else if (pj) {
                x[i] = mk(T_FIX, (int32_t)(xi + mx));
                y[i] = mk(T_FIX, (int32_t)(yi + my));
                z[i] = mk(T_FIX, (int32_t)(zi + mz));
            } else {
                x[i] = mk(T_FIX, (int32_t)(xi + mx / 2));
                y[i] = mk(T_FIX, (int32_t)(yi + my / 2));
                z[i] = mk(T_FIX, (int32_t)(zi + mz / 2));
                x[j] = mk(T_FIX, (int32_t)(xj - mx / 2));
                y[j] = mk(T_FIX, (int32_t)(yj - my / 2));
                z[j] = mk(T_FIX, (int32_t)(zj - mz / 2));
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
    if (broken)
        set_used(vm, a[0], L_BROKEN,
                 (uint32_t)whole(l.f[L_BROKEN], 0) + broken);
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

/* Which of nr colours a cell takes, its corners in turn at px, py, pz
 * (16.16): its normal by the cross of its diagonals, against the light,
 * (-1, -1, -2) over its length, in 16.16 */
static uint32_t facing(const int64_t *px, const int64_t *py,
                       const int64_t *pz, uint32_t nr)
{
    /* the diagonals in 8.8 pixels, which a cell of any size fits */
    int64_t ux = (px[2] - px[0]) >> 8, uy = (py[2] - py[0]) >> 8;
    int64_t uz = (pz[2] - pz[0]) >> 8;
    int64_t vx = (px[3] - px[1]) >> 8, vy = (py[3] - py[1]) >> 8;
    int64_t vz = (pz[3] - pz[1]) >> 8;
    int64_t nx = uy * vz - uz * vy, ny = uz * vx - ux * vz;
    int64_t nz = ux * vy - uy * vx;
    while (nx >= 1 << 30 || nx <= -(1 << 30) || ny >= 1 << 30 ||
           ny <= -(1 << 30) || nz >= 1 << 30 || nz <= -(1 << 30)) {
        nx /= 2;
        ny /= 2;
        nz /= 2;
    }
    uint32_t len = root((uint64_t)(nx * nx) + (uint64_t)(ny * ny) +
                        (uint64_t)(nz * nz));
    if (!len)
        return 0;
    int64_t lit = (nx * -26755 + ny * -26755 + nz * -53510) >> 16;
    if (lit < 0)
        lit = -lit;
    uint32_t at = (uint32_t)((lit * (int64_t)nr) / len);
    return at < nr ? at : nr - 1;
}

/* links.fill(p, ramp): the pool's cloth as filled cells, through the
 * camera, depth left out. A cell takes the ramp's colour by how squarely
 * it faces a light from the upper left, in front: the first colour edge
 * on to it, the last facing it, either side of the cloth alike, so folds
 * shade as it moves. A cell with a broken link is left open. The cells
 * drawn */
NATIVE(links_fill)
{
    (void)argc;
    struct links l;
    struct column ramp;
    if (!links_of(vm, a[0], "links.fill", &l))
        return 0;
    if (!column_of(vm, a[1], &ramp) || ramp.tag == T_INT || ramp.len < 1)
        return vm_fail(vm, "links.fill() needs its ramp as an array of "
                       "colours", "", "");
    uint32_t np = used(&l, L_POINTS, l.cap_points);
    uint32_t nl = used(&l, L_LINKS, l.cap_links);
    int32_t cols = whole(l.f[L_COLS], 0), rows = whole(l.f[L_ROWS], 0);
    int32_t first = whole(l.f[L_FIRST], 0), fl = whole(l.f[L_FIRSTLINK], 0);
    *out = mk(T_INT, 0);
    if (cols < 2 || rows < 2 || first < 0 || fl < 0 ||
        (uint32_t)first + (uint32_t)(cols * rows) > np ||
        (uint32_t)fl + (uint32_t)((cols - 1) * rows + cols * (rows - 1)) > nl)
        return vm_fail(vm, "links.fill() needs a pool with a cloth from "
                       "links.cloth()", "", "");
    struct value *x = ELEMS(vm, l.f[L_X]), *y = ELEMS(vm, l.f[L_Y]);
    struct value *z = ELEMS(vm, l.f[L_Z]);
    struct value *la = ELEMS(vm, l.f[L_A]);
    uint32_t down = (uint32_t)fl + (uint32_t)((cols - 1) * rows);
    uint32_t nr = ramp.len, drawn = 0, n = 0;
    static int32_t q[BATCH * 8];
    static uint8_t qc[BATCH];
    for (int32_t r = 0; r + 1 < rows; r++)
        for (int32_t c = 0; c + 1 < cols; c++) {
            /* the cell's four links: above, below, left, right */
            uint32_t top = (uint32_t)fl + (uint32_t)(r * (cols - 1) + c);
            uint32_t bot = top + (uint32_t)(cols - 1);
            uint32_t lft = down + (uint32_t)(r * cols + c), rgt = lft + 1;
            if (whole(la[top], -1) < 0 || whole(la[bot], -1) < 0 ||
                whole(la[lft], -1) < 0 || whole(la[rgt], -1) < 0)
                continue;
            uint32_t i = (uint32_t)(first + r * cols + c);
            uint32_t pts[4] = { i, i + 1, i + (uint32_t)cols + 1,
                                i + (uint32_t)cols };
            int64_t px[4], py[4], pz[4];
            for (int k = 0; k < 4; k++) {
                px[k] = fx(x[pts[k]]);
                py[k] = fx(y[pts[k]]);
                pz[k] = fx(z[pts[k]]);
                q[n * 8 + k * 2] = (int32_t)(px[k] >> 16);
                q[n * 8 + k * 2 + 1] = (int32_t)(py[k] >> 16);
            }
            uint32_t at = facing(px, py, pz, nr);
            qc[n] = (uint8_t)column_at(&ramp, at, 0);
            drawn++;
            if (++n == BATCH) {
                core_draw_quads(vm, q, qc, n);
                n = 0;
            }
        }
    if (n)
        core_draw_quads(vm, q, qc, n);
    vm_charge(vm, 10 + (uint32_t)(cols * rows) * 2);
    *out = mk(T_INT, (int32_t)drawn);
    return 1;
}
