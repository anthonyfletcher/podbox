/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade systems: points and links -- ropes, chains, rope bridges, cloth,
 * soft bodies and simple ragdolls, by Verlet integration -- and the
 * segment sets they meet. A point keeps where it was a tick ago as well as
 * where it is, so its speed is the difference: a step moves each point on
 * by that, plus gravity, less drag, then pulls every link back toward its
 * length a few passes over, then pushes points out of the segment set and
 * solid map cells. A pinned point stays where the
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
 * and links.fill draws it as cells, each shaded by the way it faces. With
 * a cells field (field, rules, field_x and field_y, as an actor pool has)
 * points are kept out of its solid cells, and slowed in its liquids.
 *
 * A soft body is a ring of points: gx turns gravity aside, slack makes
 * links that pull but never push (a skin), damp draws each point's speed
 * toward the pool's mean (a body that does not wobble), and links.ring
 * holds the ring's area. A segment set (segs.make) is outlines as lines,
 * each with a material, filed in a grid; set in segs, the step keeps each
 * point radius from them, with the material's share of friction and a
 * bounce, and segs.move moves a set and carries what rests on it.
 *
 * Parts, in order:
 *   pools          links.pool, and checking one handed back
 *   building       links.point, links.link, links.cloth
 *   segment sets   segs.make, segs.move, segs.draw
 *   tracing        segs.trace, the map's solid cells as outlines
 *   meeting a set  the lines near a point, crossed and pushed out of
 *   a ring's area  links.ring
 *   the step       links.step
 *   drawing        links.draw, links.fill
 ***************************************************************************/
#include <string.h>
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
       L_BROKEN, L_COLS, L_ROWS, L_FIRST, L_FIRSTLINK, L_FIELD, L_RULES,
       L_FIELD_X, L_FIELD_Y, L_GX, L_RADIUS, L_SLACK, L_FRICTION, L_BOUNCE,
       L_SEGS, L_DAMP, L_FIELDS };
static const char *const link_keys[L_FIELDS] = {
    "x", "y", "z", "ox", "oy", "oz", "pin", "a", "b", "len",
    "gravity", "drag", "passes", "points", "links", "tear", "broken",
    "cols", "rows", "first", "firstlink", "field", "rules", "field_x",
    "field_y", "gx", "radius", "slack", "friction", "bounce", "segs",
    "damp"
};

#define ELEMS(vm, arr) (BUF(vm, ARR(vm, arr)->buf)->v)

/* always inlined: -Os would call it, and links.step reads six a link a
 * pass */
static inline __attribute__((always_inline)) int32_t fx(struct value v)
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
    /* links.step stages the pool in arrays of these sizes */
    if (l->cap_points > MAX_POINTS || l->cap_links > MAX_LINKS)
        return vm_fail(vm, fn, "() needs a pool whose arrays are as "
                       "links.pool() made them", "");
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

/* The square root of a 32-bit square, rounded down: the short links'
 * root, at a fraction of root()'s cost on an ARM7 */
static uint32_t root32(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
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

/* The length of (dx, dy, dz), 16.16: halved until the squares fit, and
 * doubled back after */
static int32_t span(int64_t dx, int64_t dy, int64_t dz)
{
    int shift = 0;
    while (dx > INT32_MAX || dx < -INT32_MAX || dy > INT32_MAX ||
           dy < -INT32_MAX || dz > INT32_MAX || dz < -INT32_MAX) {
        dx /= 2;
        dy /= 2;
        dz /= 2;
        shift++;
    }
    uint64_t d = (uint64_t)root((uint64_t)(dx * dx) + (uint64_t)(dy * dy) +
                                (uint64_t)(dz * dz)) << shift;
    return d > INT32_MAX ? INT32_MAX : (int32_t)d;
}

/* The distance between points i and j, 16.16 */
static int32_t apart(const struct value *x, const struct value *y,
                     const struct value *z, uint32_t i, uint32_t j)
{
    int64_t dx = (int64_t)fx(x[j]) - fx(x[i]);
    int64_t dy = (int64_t)fx(y[j]) - fx(y[i]);
    int64_t dz = (int64_t)fx(z[j]) - fx(z[i]);
    return span(dx, dy, dz);
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
    *out = mk(T_INT, -1);
    /* no pool holds more: and the counts below cannot wrap */
    if (cols > MAX_POINTS || rows > MAX_POINTS)
        return 1;
    uint32_t np = used(&l, L_POINTS, l.cap_points);
    uint32_t nl = used(&l, L_LINKS, l.cap_links);
    uint32_t want = (uint32_t)cols * (uint32_t)rows;
    uint32_t links = (uint32_t)(cols - 1) * (uint32_t)rows +
                     (uint32_t)cols * (uint32_t)(rows - 1) +
                     2 * (uint32_t)(cols - 1) * (uint32_t)(rows - 1);
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

/* The field's material at world pixel x, y (16.16), or -1 off it */
static int32_t cell_at(const struct cellworld *w, int32_t fx0, int32_t fy0,
                       int32_t x, int32_t y)
{
    x = (x >> 16) - fx0;
    y = (y >> 16) - fy0;
    if (x < 0 || y < 0 || x >= w->w || y >= w->h)
        return -1;
    return w->c[y * w->w + x];
}

static int cell_solid(const struct cellworld *w, int32_t fx0, int32_t fy0,
                      int32_t x, int32_t y)
{
    int32_t m = cell_at(w, fx0, fy0, x, y);
    return m >= 0 && (w->flags[m] & CELL_SOLID);
}

/* The square root of v, rounded down */
static uint64_t root64(uint64_t v)
{
    uint64_t r = 0, b = (uint64_t)1 << 62;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return r;
}

/* ---- segment sets --------------------------------------------------------- *
 * The outlines points meet, as polyfill draws them: each outline's lines,
 * each with a material, filed by the grid cells it crosses so a point
 * looks only at the lines near it. A set has an offset, which segs.move
 * changes: a lift or a gate moves without being built again, and carries
 * what rests on it. */

#define MAX_SEGS  4096
#define SEG_CELLS 16384             /* the grid's cells, at most */

/* units: each line a point is tested against, and each point that touches
 * one -- its push, a root and a divide among it */
#define SEG_TEST  50
#define SEG_TOUCH 120

enum { S_X0, S_Y0, S_X1, S_Y1, S_MAT, S_START, S_ITEMS, S_COUNT, S_OX, S_OY,
       S_MX, S_MY, S_MOVED, S_CELL, S_GX, S_GY, S_GW, S_GH, S_FIELDS };
static const char *const seg_keys[S_FIELDS] = {
    "x0", "y0", "x1", "y1", "mat", "start", "items", "count", "ox", "oy",
    "mx", "my", "moved", "cell", "gx", "gy", "gw", "gh"
};

/* A set handed back: its lines (16.16, before the offset), materials, and
 * the grid that files them */
struct segs
{
    const int32_t *x0, *y0, *x1, *y1, *start, *items;
    const uint8_t *mat;
    uint32_t count;
    int32_t ox, oy, mx, my;         /* the offset, and its move this tick */
    int32_t cell, gx, gy, gw, gh;
};

/* An Int32Array (or Uint8Array) of n, zeroed */
static int new_ints(struct vm *vm, uint32_t n, int bytes, struct value *out)
{
    uint32_t off = gc_alloc(vm, bytes == 4 ? T_I32 : T_U8,
                            (uint32_t)(sizeof(struct i32) - sizeof(struct hdr))
                            + n * (uint32_t)bytes);
    if (!off)
        return 0;
    ((struct i32 *)MEM(vm, off))->len = n;
    *out = mk(bytes == 4 ? T_I32 : T_U8, (int32_t)off);
    return 1;
}

static int segs_of(struct vm *vm, struct value v, const char *fn,
                   struct segs *s)
{
    uint32_t sh = v.tag == T_OBJ ? OBJ(vm, v)->shape : 0;
    while (sh && sh != vm->segs_shape)
        sh = vm->shapes[sh].parent;
    if (!vm->segs_shape || sh != vm->segs_shape)
        return vm_fail(vm, fn, "() needs a set from segs.make()", "");
    const struct value *f = BUF(vm, OBJ(vm, v)->buf)->v;
    for (int i = S_X0; i <= S_ITEMS; i++)
        if (f[i].tag != (i == S_MAT ? T_U8 : T_I32))
            return vm_fail(vm, fn, "() needs a set whose arrays are as "
                           "segs.make() made them", "");
    s->x0 = I32(vm, f[S_X0])->d;
    s->y0 = I32(vm, f[S_Y0])->d;
    s->x1 = I32(vm, f[S_X1])->d;
    s->y1 = I32(vm, f[S_Y1])->d;
    s->mat = U8A(vm, f[S_MAT])->d;
    s->start = I32(vm, f[S_START])->d;
    s->items = I32(vm, f[S_ITEMS])->d;
    s->count = (uint32_t)whole(f[S_COUNT], 0);
    s->ox = fx(f[S_OX]);
    s->oy = fx(f[S_OY]);
    int moved = whole(f[S_MOVED], -1) == (int32_t)vm->frame;
    s->mx = moved ? fx(f[S_MX]) : 0;
    s->my = moved ? fx(f[S_MY]) : 0;
    s->cell = whole(f[S_CELL], 16);
    s->gx = whole(f[S_GX], 0);
    s->gy = whole(f[S_GY], 0);
    s->gw = whole(f[S_GW], 0);
    s->gh = whole(f[S_GH], 0);
    /* the arrays as long as the counts say, or the set is not one made */
    uint32_t cells = (uint32_t)s->gw * (uint32_t)s->gh;
    if (s->count > MAX_SEGS || s->cell < 1 || s->gw < 1 || s->gh < 1 ||
        cells > SEG_CELLS || I32(vm, f[S_X0])->len < s->count ||
        I32(vm, f[S_Y0])->len < s->count || I32(vm, f[S_X1])->len < s->count ||
        I32(vm, f[S_Y1])->len < s->count || U8A(vm, f[S_MAT])->len < s->count ||
        I32(vm, f[S_START])->len != cells + 1 ||
        (uint32_t)s->start[cells] > I32(vm, f[S_ITEMS])->len)
        return vm_fail(vm, fn, "() needs a set whose arrays are as "
                       "segs.make() made them", "");
    for (uint32_t c = 0; c < cells; c++)
        if (s->start[c] < 0 || s->start[c] > s->start[c + 1])
            return vm_fail(vm, fn, "() needs a set whose arrays are as "
                           "segs.make() made them", "");
    return 1;
}

/* The cells, row by row, that line k crosses: each passed to file() */
static void seg_cells(const struct segs *s, int32_t ax, int32_t ay,
                      int32_t bx, int32_t by, int32_t *count, int32_t *items,
                      int32_t *fill, int32_t k)
{
    /* in whole pixels, ordered by y */
    int64_t x0 = ax >> 16, y0 = ay >> 16, x1 = bx >> 16, y1 = by >> 16;
    if (y0 > y1) {
        int64_t t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    int32_t r0 = (int32_t)((y0 - s->gy) / s->cell);
    int32_t r1 = (int32_t)((y1 - s->gy) / s->cell);
    for (int32_t r = r0; r <= r1; r++) {
        /* the line's x where it enters and leaves this row of cells */
        int64_t ya = (int64_t)s->gy + (int64_t)r * s->cell;
        int64_t yb = ya + s->cell;
        if (ya < y0) ya = y0;
        if (yb > y1) yb = y1;
        int64_t xa = x0, xb = x1;
        if (y1 != y0) {
            xa = x0 + (x1 - x0) * (ya - y0) / (y1 - y0);
            xb = x0 + (x1 - x0) * (yb - y0) / (y1 - y0);
        }
        if (xa > xb) { int64_t t = xa; xa = xb; xb = t; }
        int32_t c0 = (int32_t)((xa - s->gx) / s->cell);
        int32_t c1 = (int32_t)((xb - s->gx) / s->cell);
        for (int32_t c = c0; c <= c1; c++) {
            int32_t i = r * s->gw + c;
            if (count)
                count[i]++;
            else
                items[fill[i]++] = k;
        }
    }
}

/* segs.make(xs, ys, n, [mats], [ends]): the outlines through the n points,
 * as polyfill takes them -- each closed, its last point joined to its
 * first, but one of two points a single line -- each line taking the
 * material of the point it starts from (0 when mats is left out) */
NATIVE(segs_make)
{
    struct column xs, ys, mats = { T_NULL, NULL, 0, 0 }, ends = mats;
    if (!column_of(vm, a[0], &xs) || !column_of(vm, a[1], &ys) ||
        xs.tag == T_INT || ys.tag == T_INT || !is_num(a[2]))
        return vm_fail(vm, "segs.make() needs arrays of x and y, then how "
                       "many points", "", "");
    int32_t n = whole(a[2], 0);
    if (n < 0)
        n = 0;
    if (n > MAX_SEGS)
        return vm_fail(vm, "segs.make() takes at most 4096 points", "", "");
    if ((uint32_t)n > xs.len || (uint32_t)n > ys.len)
        return vm_fail(vm, "segs.make() has fewer x or y than points", "", "");
    if (argc > 3 && a[3].tag != T_NULL &&
        (!column_of(vm, a[3], &mats) || mats.tag == T_INT ||
         mats.len < (uint32_t)n))
        return vm_fail(vm, "segs.make()'s mats is an array, one for each "
                       "point", "", "");
    uint32_t nends = 0;
    if (argc > 4 && a[4].tag != T_NULL) {
        if (!column_of(vm, a[4], &ends) || ends.tag == T_INT)
            return vm_fail(vm, "segs.make()'s ends is an array", "", "");
        nends = ends.len;
    }
    /* the lines, from the outlines */
    static int32_t X0[MAX_SEGS], Y0[MAX_SEGS], X1[MAX_SEGS], Y1[MAX_SEGS];
    static uint8_t M[MAX_SEGS];
    uint32_t count = 0, s0 = 0;
    for (uint32_t k = 0; k <= nends && s0 < (uint32_t)n; k++) {
        uint32_t e = (uint32_t)n;
        if (k < nends) {
            int32_t v = column_at(&ends, k, 0);
            if (v <= (int32_t)s0 || v > n)
                return vm_fail(vm, "segs.make()'s ends must rise, each past "
                               "the last and none past n", "", "");
            e = (uint32_t)v;
        }
        uint32_t m = e - s0;
        for (uint32_t i = 0; m >= 2 && i < (m == 2 ? 1 : m); i++) {
            uint32_t p = s0 + i, q = s0 + (i + 1) % m;
            X0[count] = column_fix(&xs, p);
            Y0[count] = column_fix(&ys, p);
            X1[count] = column_fix(&xs, q);
            Y1[count] = column_fix(&ys, q);
            M[count] = (uint8_t)(mats.tag == T_NULL ? 0
                                 : column_at(&mats, p, 0));
            count++;
        }
        s0 = e;
    }
    /* the grid over them all: cells of 16 pixels, larger to keep it small */
    int32_t lx = 0, ly = 0, hx = 0, hy = 0;
    for (uint32_t k = 0; k < count; k++) {
        int32_t ax = X0[k] >> 16, ay = Y0[k] >> 16, bx = X1[k] >> 16;
        int32_t by = Y1[k] >> 16;
        if (!k) { lx = hx = ax; ly = hy = ay; }
        if (ax < lx) lx = ax;
        if (bx < lx) lx = bx;
        if (ax > hx) hx = ax;
        if (bx > hx) hx = bx;
        if (ay < ly) ly = ay;
        if (by < ly) ly = by;
        if (ay > hy) hy = ay;
        if (by > hy) hy = by;
    }
    struct segs s;
    s.cell = 16;
    for (;;) {
        s.gw = (int32_t)(((int64_t)hx - lx) / s.cell + 1);
        s.gh = (int32_t)(((int64_t)hy - ly) / s.cell + 1);
        if ((int64_t)s.gw * s.gh <= SEG_CELLS)
            break;
        s.cell *= 2;
    }
    s.gx = lx;
    s.gy = ly;
    uint32_t cells = (uint32_t)(s.gw * s.gh);
    /* each cell's lines: counted, then filed */
    static int32_t per[SEG_CELLS + 1];
    memset(per, 0, sizeof(per));
    for (uint32_t k = 0; k < count; k++)
        seg_cells(&s, X0[k], Y0[k], X1[k], Y1[k], per, NULL, NULL, 0);
    uint32_t items = 0;
    for (uint32_t c = 0; c < cells; c++) {
        uint32_t t = (uint32_t)per[c];
        per[c] = (int32_t)items;
        items += t;
    }
    per[cells] = (int32_t)items;
    if (!vm_new_record(vm, seg_keys, S_FIELDS, out))
        return 0;
    vm->segs_shape = OBJ(vm, *out)->shape;
    if (!vm_root(vm, *out))
        return 0;
    struct value v;
    for (int f = S_X0; f <= S_ITEMS; f++) {
        uint32_t len = f == S_START ? cells + 1 : f == S_ITEMS ? items : count;
        if (!new_ints(vm, len ? len : 1, f == S_MAT ? 1 : 4, &v)) {
            vm_unroot(vm);
            return 0;
        }
        BUF(vm, OBJ(vm, *out)->buf)->v[f] = v;
    }
    vm_unroot(vm);
    struct value *F = BUF(vm, OBJ(vm, *out)->buf)->v;
    memcpy(I32(vm, F[S_X0])->d, X0, count * 4);
    memcpy(I32(vm, F[S_Y0])->d, Y0, count * 4);
    memcpy(I32(vm, F[S_X1])->d, X1, count * 4);
    memcpy(I32(vm, F[S_Y1])->d, Y1, count * 4);
    memcpy(U8A(vm, F[S_MAT])->d, M, count);
    memcpy(I32(vm, F[S_START])->d, per, (cells + 1) * 4);
    int32_t *it = I32(vm, F[S_ITEMS])->d;
    for (uint32_t k = 0; k < count; k++)
        seg_cells(&s, X0[k], Y0[k], X1[k], Y1[k], NULL, it, per, (int32_t)k);
    F[S_COUNT] = mk(T_INT, (int32_t)count);
    F[S_OX] = F[S_OY] = F[S_MX] = F[S_MY] = mk(T_INT, 0);
    F[S_MOVED] = mk(T_INT, -1);
    F[S_CELL] = mk(T_INT, s.cell);
    F[S_GX] = mk(T_INT, s.gx);
    F[S_GY] = mk(T_INT, s.gy);
    F[S_GW] = mk(T_INT, s.gw);
    F[S_GH] = mk(T_INT, s.gh);
    /* measured on the ARM rig (bagprobe): most of it the divisions that
     * find each line's cells */
    vm_charge(vm, 40 + count * 80 + items * 10 + cells / 4);
    return 1;
}

/* segs.move(s, dx, dy): the set moved by dx, dy, this tick's move, which
 * carries the points resting on it */
NATIVE(segs_move)
{
    (void)argc; (void)out;
    struct segs s;
    if (!segs_of(vm, a[0], "segs.move", &s))
        return 0;
    int32_t dx = fx(a[1]), dy = fx(a[2]);
    struct value *F = BUF(vm, OBJ(vm, a[0])->buf)->v;
    F[S_OX] = mk(T_FIX, wrap32((int64_t)s.ox + dx));
    F[S_OY] = mk(T_FIX, wrap32((int64_t)s.oy + dy));
    F[S_MX] = mk(T_FIX, wrap32((int64_t)s.mx + dx));
    F[S_MY] = mk(T_FIX, wrap32((int64_t)s.my + dy));
    F[S_MOVED] = mk(T_INT, (int32_t)vm->frame);
    vm_charge(vm, 6);
    return 1;
}

/* segs.draw(s, colour): each line, where the set is now */
NATIVE(segs_draw)
{
    (void)argc; (void)out;
    struct segs s;
    if (!segs_of(vm, a[0], "segs.draw", &s))
        return 0;
    uint32_t c = (uint32_t)whole(a[1], 6);
    int32_t seg[4 * BATCH];
    uint32_t k = 0;
    while (k < s.count) {
        uint32_t m = 0;
        for (; k < s.count && m < BATCH; k++, m++) {
            seg[m * 4] = (int32_t)(((int64_t)s.x0[k] + s.ox) >> 16);
            seg[m * 4 + 1] = (int32_t)(((int64_t)s.y0[k] + s.oy) >> 16);
            seg[m * 4 + 2] = (int32_t)(((int64_t)s.x1[k] + s.ox) >> 16);
            seg[m * 4 + 3] = (int32_t)(((int64_t)s.y1[k] + s.oy) >> 16);
        }
        core_draw_lines(vm, seg, m, c);
        if (!segs_of(vm, a[0], "segs.draw", &s))
            return 0;
    }
    return 1;
}

/* ---- tracing the map ------------------------------------------------------ *
 * segs.trace: the edges of the map's solid cells joined into closed
 * outlines -- one round each solid region, and one round each hole in it
 * -- each corner then cut by half a cell, as marching squares would, but
 * where a cell is sharp, and rounded by passes that cut every corner
 * again. The outlines come back as polyfill and segs.make take them. */

#define TRACE_POINTS 4096           /* the most points it gives */

enum { T_XS, T_YS, T_ENDS, T_N, T_FIELDS };
static const char *const trace_keys[T_FIELDS] = { "x", "y", "ends", "n" };


/* Chaikin's cut of one outline in place, n points to 2n, a point marked
 * sharp kept where it is: returns the new count, or 0 when it would not
 * fit in room */
static uint32_t trace_cut(int32_t *x, int32_t *y, uint8_t *sharp, uint32_t n,
                          uint32_t room, int32_t *tx, int32_t *ty,
                          uint8_t *ts)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = (i + 1) % n;
        if (sharp[i]) {
            if (m >= room) return 0;
            tx[m] = x[i]; ty[m] = y[i]; ts[m++] = 1;
        }
        if (m + 2 > room) return 0;
        /* a quarter and three quarters along, except beside a sharp point,
         * which keeps the line to itself straight */
        int64_t dx = (int64_t)x[j] - x[i], dy = (int64_t)y[j] - y[i];
        if (!sharp[i]) {
            tx[m] = (int32_t)(x[i] + dx / 4);
            ty[m] = (int32_t)(y[i] + dy / 4);
            ts[m++] = 0;
        }
        if (!sharp[j]) {
            tx[m] = (int32_t)(x[i] + dx * 3 / 4);
            ty[m] = (int32_t)(y[i] + dy * 3 / 4);
            ts[m++] = 0;
        }
    }
    memcpy(x, tx, m * 4);
    memcpy(y, ty, m * 4);
    memcpy(sharp, ts, m);
    return m;
}

/* the working memory let go, and the cart stopped with why */
static int trace_fail(struct vm *vm, const char *why, const char *a,
                      const char *b)
{
    vm_unroot(vm);
    return vm_fail(vm, why, a, b);
}

/* segs.trace(solid, [sharp], [smooth], [cx, cy, cw, ch]): the outlines of
 * the map cells whose flags share a bit with solid, in world pixels; a
 * corner of a cell whose flags share a bit with sharp is kept square, the
 * rest cut by half a cell; smooth (0 to 3) passes round them. Within the
 * cells cx, cy, cw by ch, the whole map when left out. Returns a record:
 * x, y and ends, as polyfill and segs.make take them, and n, the points. */
NATIVE(segs_trace)
{
    int32_t mask = whole(a[0], 0);
    int32_t sharpm = argc > 1 ? whole(a[1], 0) : 0;
    int32_t passes = argc > 2 ? whole(a[2], 0) : 0;
    if (passes < 0) passes = 0;
    if (passes > 3) passes = 3;
    int32_t c0 = 0, r0 = 0, w = vm->map_w, h = vm->map_h;
    if (argc > 3) {
        c0 = whole(a[3], 0);
        r0 = argc > 4 ? whole(a[4], 0) : 0;
        w = argc > 5 ? whole(a[5], w) : w;
        h = argc > 6 ? whole(a[6], h) : h;
    }
    /* within the map */
    if (c0 < 0) { w += c0; c0 = 0; }
    if (r0 < 0) { h += r0; r0 = 0; }
    if (c0 + w > vm->map_w) w = vm->map_w - c0;
    if (r0 + h > vm->map_h) h = vm->map_h - r0;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    /* every edge between a solid cell and one not, solid on its right as
     * it runs: out of each corner of the grid, at most two */
    uint32_t vw = (uint32_t)w + 1, verts = vw * ((uint32_t)h + 1);
    /* the working memory, held through the allocations at the end: per
     * point x, y, a second x and y for cutting, the ends, and sharp twice;
     * then per corner of the grid the ways out (low bits) and walked */
    struct value work;
    uint32_t at_v = TRACE_POINTS * 22, gw = (uint32_t)w + 2;
    uint32_t at_g = at_v + verts;
    if (!new_ints(vm, at_g + gw * ((uint32_t)h + 2), 1, &work) ||
        !vm_root(vm, work))
        return 0;
    uint8_t *mem = U8A(vm, work)->d;
    int32_t *px = (int32_t *)(void *)mem, *py = px + TRACE_POINTS;
    int32_t *tx = py + TRACE_POINTS, *ty = tx + TRACE_POINTS;
    int32_t *ends = ty + TRACE_POINTS;
    uint8_t *ps = (uint8_t *)(ends + TRACE_POINTS);
    uint8_t *tsh = ps + TRACE_POINTS;
    uint8_t *outs = mem + at_v;          /* bits 0-3 out, 4-7 walked */
    /* each cell read once: 1 solid, 2 sharp too, with a border of none
     * round the area so its edges close the outlines */
    uint8_t *grid = mem + at_g;
    {
        const uint8_t *map = MEM(vm, vm->map), *flags = MEM(vm, vm->flags);
        for (int32_t r = 0; r < h; r++)
            for (int32_t c = 0; c < w; c++) {
                uint8_t f = flags[map[(r0 + r) * vm->map_w + c0 + c]];
                grid[(uint32_t)(r + 1) * gw + (uint32_t)(c + 1)] =
                    (uint8_t)((f & mask) ? 1 | ((f & sharpm) ? 2 : 0) : 0);
            }
    }
    /* the ways: 1 right, 2 down, 4 left, 8 up */
    static const int32_t step_x[4] = { 1, 0, -1, 0 }, step_y[4] = { 0, 1, 0, -1 };
    uint32_t edges = 0;
    for (int32_t r = 0; r <= h; r++)
        for (int32_t c = 0; c <= w; c++) {
            uint32_t v = (uint32_t)r * vw + (uint32_t)c;
            /* the four cells round corner c, r */
            const uint8_t *g = grid + (uint32_t)r * gw + (uint32_t)c;
            int ul = g[0] & 1, ur = g[1] & 1;
            int dl = g[gw] & 1, dr = g[gw + 1] & 1;
            if (dr && !ur) outs[v] |= 1;     /* right, solid below */
            if (dl && !dr) outs[v] |= 2;     /* down, solid to the left */
            if (ul && !dl) outs[v] |= 4;     /* left, solid above */
            if (ur && !ul) outs[v] |= 8;     /* up, solid to the right */
        }
    /* walked into outlines: at a corner with two ways out, the one turning
     * right, so regions that touch at a corner stay apart */
    uint32_t n = 0, nends = 0, cells = (uint32_t)(w * h);
    for (uint32_t v0 = 0; v0 < verts; v0++)
        for (int d0 = 0; d0 < 4; d0++) {
            if (!(outs[v0] & (1 << d0)) || (outs[v0] & (16 << d0)))
                continue;
            uint32_t start = n, v = v0;
            int d = d0, last = -1;
            for (;;) {
                outs[v] |= (uint8_t)(16 << d);
                edges++;
                if (d != last) {
                    /* a corner: a point */
                    if (n >= TRACE_POINTS)
                        return trace_fail(vm, "segs.trace() finds more than "
                                       "4096 corners; trace part of the "
                                       "map at a time", "", "");
                    int32_t c = (int32_t)(v % vw), r = (int32_t)(v / vw);
                    px[n] = (c0 + c) << 19;
                    py[n] = (r0 + r) << 19;
                    /* sharp when a solid cell touching it is */
                    ps[n] = 0;
                    const uint8_t *g = grid + (uint32_t)r * gw + (uint32_t)c;
                    ps[n] = (uint8_t)(((g[0] | g[1] | g[gw] | g[gw + 1]) &
                                       2) != 0);
                    n++;
                    last = d;
                }
                v = (uint32_t)((int32_t)v + step_y[d] * (int32_t)vw + step_x[d]);
                /* on: turning right first, then straight, then left */
                int nd = -1;
                static const int turn[3] = { 1, 0, 3 };
                for (int k = 0; k < 3 && nd < 0; k++) {
                    int t = (d + turn[k]) & 3;
                    if ((outs[v] & (1 << t)) && !(outs[v] & (16 << t)))
                        nd = t;
                }
                if (nd < 0)
                    break;
                d = nd;
            }
            /* the start's own corner, when the walk came back round to it
             * along the way it set out */
            if (n - start < 3) {
                n = start;
                continue;
            }
            /* its corners cut by half a cell, but the sharp ones */
            uint32_t m = n - start, k = 0;
            for (uint32_t i = 0; i < m; i++) {
                uint32_t a = start + (i + m - 1) % m, b = start + i;
                uint32_t c = start + (i + 1) % m;
                if (ps[b]) {
                    tx[k] = px[b]; ty[k] = py[b]; tsh[k++] = 1;
                    continue;
                }
                if (k + 2 > TRACE_POINTS)
                    return trace_fail(vm, "segs.trace() finds more than 4096 "
                                   "corners; trace part of the map at a "
                                   "time", "", "");
                /* half a cell, or half the side when shorter */
                int64_t ax = (int64_t)px[a] - px[b], ay = (int64_t)py[a] - py[b];
                int64_t cx2 = (int64_t)px[c] - px[b], cy2 = (int64_t)py[c] - py[b];
                int64_t la = (ax < 0 ? -ax : ax) + (ay < 0 ? -ay : ay);
                int64_t lc = (cx2 < 0 ? -cx2 : cx2) + (cy2 < 0 ? -cy2 : cy2);
                int64_t cut = 4 << 16;
                if (la / 2 < cut) cut = la / 2;
                if (lc / 2 < cut) cut = lc / 2;
                tx[k] = (int32_t)(px[b] + (la ? ax * cut / la : 0));
                ty[k] = (int32_t)(py[b] + (la ? ay * cut / la : 0));
                tsh[k++] = 0;
                tx[k] = (int32_t)(px[b] + (lc ? cx2 * cut / lc : 0));
                ty[k] = (int32_t)(py[b] + (lc ? cy2 * cut / lc : 0));
                tsh[k++] = 0;
            }
            if (start + k > TRACE_POINTS)
                return trace_fail(vm, "segs.trace() finds more than 4096 "
                               "corners; trace part of the map at a time",
                               "", "");
            memcpy(px + start, tx, k * 4);
            memcpy(py + start, ty, k * 4);
            memcpy(ps + start, tsh, k);
            for (int32_t p = 0; p < passes; p++) {
                uint32_t room = TRACE_POINTS - start;
                uint32_t got = trace_cut(px + start, py + start, ps + start,
                                         k, room, tx, ty, tsh);
                if (!got)
                    return trace_fail(vm, "segs.trace() finds more than 4096 "
                                   "points; smooth less, or trace part of "
                                   "the map at a time", "", "");
                k = got;
            }
            n = start + k;
            ends[nends++] = (int32_t)n;
        }
    /* the record: plain arrays of the points and the ends */
    if (!vm_new_record(vm, trace_keys, T_FIELDS, out) || !vm_root(vm, *out)) {
        vm_unroot(vm);
        return 0;
    }
    struct value arr;
    uint32_t lens[3] = { n, n, nends };
    for (int f = 0; f < 3; f++) {
        if (!vm_new_array(vm, lens[f] ? lens[f] : 1, &arr)) {
            vm_unroot(vm);
            vm_unroot(vm);
            return 0;
        }
        struct value *e = BUF(vm, ARR(vm, arr)->buf)->v;
        for (uint32_t i = 0; i < lens[f]; i++)
            e[i] = f == T_ENDS ? mk(T_INT, ends[i])
                               : mk(T_FIX, f == T_XS ? px[i] : py[i]);
        ARR(vm, arr)->len = lens[f];
        BUF(vm, OBJ(vm, *out)->buf)->v[f] = arr;
    }
    vm_unroot(vm);
    vm_unroot(vm);
    BUF(vm, OBJ(vm, *out)->buf)->v[T_N] = mk(T_INT, (int32_t)n);
    /* measured on the ARM rig (traceprobe): the grid, its corners walked,
     * the edges, and the points given */
    vm_charge(vm, 30 + cells * 2 + verts * 4 + edges * 3 + n * 12);
    return 1;
}

/* ---- meeting a segment set ------------------------------------------------ *
 * After the links have pulled, each free point is kept out of the set's
 * lines: first its path from where it was, which may not cross one, then
 * its radius, which none may come within. Where it touches, its speed
 * against the line (less the line's own move) loses its share to the
 * material's friction along the line, and bounces off it by bounce. The
 * work is in 1/256 pixels, where the products fit 64 bits. */

/* What a point met: the line, its material, and the way out, 16.16 */
struct touch
{
    int32_t k;
    int32_t nx, ny;
};

/* The lines filed in the cells round x0..x1, y0..y1 (1/256 pixels, the
 * set's own), each once: their indices into out, how many given */
static uint32_t seg_near(const struct segs *s, int64_t x0, int64_t y0,
                         int64_t x1, int64_t y1, int32_t *out, uint32_t room,
                         uint32_t *stamp, uint32_t mark)
{
    int64_t c8 = (int64_t)s->cell << 8;
    int64_t gx = (int64_t)s->gx << 8, gy = (int64_t)s->gy << 8;
    int64_t a = (x0 - gx) / c8, b = (x1 - gx) / c8;
    int64_t c = (y0 - gy) / c8, d = (y1 - gy) / c8;
    if (x0 < gx) a = -1;
    if (y0 < gy) c = -1;
    if (a < 0) a = 0;
    if (c < 0) c = 0;
    if (b >= s->gw) b = s->gw - 1;
    if (d >= s->gh) d = s->gh - 1;
    if (x1 < gx || y1 < gy)
        return 0;
    uint32_t n = 0;
    for (int64_t r = c; r <= d; r++)
        for (int64_t q = a; q <= b; q++) {
            int32_t i = (int32_t)(r * s->gw + q);
            for (int32_t t = s->start[i]; t < s->start[i + 1]; t++) {
                int32_t k = s->items[t];
                if ((uint32_t)k >= s->count || stamp[k] == mark)
                    continue;
                stamp[k] = mark;
                if (n < room)
                    out[n++] = k;
            }
        }
    return n;
}

/* The point p kept r from line k (all 1/256 pixels): moved out along the
 * way from the line's nearest point, or, sitting on the line, toward the
 * side o came from. 1 when it was moved, with the way out in t. */
static int seg_push(const struct segs *s, int32_t k, int64_t *px, int64_t *py,
                    int64_t ox, int64_t oy, int64_t r, struct touch *t)
{
    int64_t ax = (int64_t)s->x0[k] >> 8, ay = (int64_t)s->y0[k] >> 8;
    int64_t bx = (int64_t)s->x1[k] >> 8, by = (int64_t)s->y1[k] >> 8;
    /* beyond r of the line's box: nowhere near it */
    if (*px + r < (ax < bx ? ax : bx) || *px - r > (ax > bx ? ax : bx) ||
        *py + r < (ay < by ? ay : by) || *py - r > (ay > by ? ay : by))
        return 0;
    int64_t ex = bx - ax, ey = by - ay, ee = ex * ex + ey * ey;
    int64_t u = ee > 0 ? (*px - ax) * ex + (*py - ay) * ey : 0;
    int64_t nx, ny;
    if (u > 0 && u < ee) {
        /* beside the line's middle: out along its normal, the distance by
         * the cross product -- the nearest point's rounding would point
         * the way out along the line */
        int64_t el = (int64_t)root64((uint64_t)ee);
        if (el <= 0)
            return 0;
        nx = (-ey << 16) / el;
        ny = (ex << 16) / el;
        if ((ox - ax) * nx + (oy - ay) * ny < 0) {
            nx = -nx;
            ny = -ny;
        }
        /* how far out on that side, which is below 0 when behind it */
        int64_t side = ((*px - ax) * nx + (*py - ay) * ny) >> 16;
        if (side >= r)
            return 0;
        *px += nx * (r - side) >> 16;
        *py += ny * (r - side) >> 16;
    } else {
        /* off an end: out from the end, away from it */
        int64_t qx = u <= 0 ? ax : bx, qy = u <= 0 ? ay : by;
        int64_t dx = *px - qx, dy = *py - qy;
        int64_t dd = dx * dx + dy * dy;
        if (dd >= r * r)
            return 0;
        int64_t d = (int64_t)root64((uint64_t)dd);
        if (d <= 0)
            return 0;
        nx = (dx << 16) / d;
        ny = (dy << 16) / d;
        *px = qx + (nx * r >> 16);
        *py = qy + (ny * r >> 16);
    }
    t->k = k;
    t->nx = (int32_t)nx;
    t->ny = (int32_t)ny;
    return 1;
}

/* t as touching line k, its way out the side o is on */
static void seg_side(const struct segs *s, int32_t k, int64_t ox, int64_t oy,
                     struct touch *t)
{
    int64_t ax = (int64_t)s->x0[k] >> 8, ay = (int64_t)s->y0[k] >> 8;
    int64_t ex = ((int64_t)s->x1[k] >> 8) - ax;
    int64_t ey = ((int64_t)s->y1[k] >> 8) - ay;
    int64_t el = (int64_t)root64((uint64_t)(ex * ex + ey * ey));
    int64_t nx = el ? (-ey << 16) / el : 0, ny = el ? (ex << 16) / el : 0;
    if ((ox - ax) * nx + (oy - ay) * ny < 0) {
        nx = -nx;
        ny = -ny;
    }
    t->k = k;
    t->nx = (int32_t)nx;
    t->ny = (int32_t)ny;
}

/* Where the path o to p first crosses line k, as a share of the path
 * 16.16, or -1 when it does not */
static int64_t seg_cross(const struct segs *s, int32_t k, int64_t ox,
                         int64_t oy, int64_t px, int64_t py)
{
    int64_t ax = (int64_t)s->x0[k] >> 8, ay = (int64_t)s->y0[k] >> 8;
    int64_t ex = ((int64_t)s->x1[k] >> 8) - ax;
    int64_t ey = ((int64_t)s->y1[k] >> 8) - ay;
    int64_t vx = px - ox, vy = py - oy;
    int64_t den = vx * ey - vy * ex;
    if (!den)
        return -1;
    int64_t wx = ax - ox, wy = ay - oy;
    int64_t t = wx * ey - wy * ex;          /* along the path, times den */
    int64_t u = wx * vy - wy * vx;          /* along the line, times den */
    if (den < 0) {
        den = -den;
        t = -t;
        u = -u;
    }
    if (t < 0 || t > den || u < 0 || u > den)
        return -1;
    while (den > ((int64_t)1 << 46)) {
        den >>= 1;
        t >>= 1;
    }
    return den ? (t << 16) / den : 0;
}

#define NEAR_SEGS 64                /* the lines a point looks at, at most */

/* One point against the set: p and o (16.16, world) changed in place.
 * Returns the material plus one it touched, 0 none, with the way out. */
static int32_t seg_meet(const struct segs *s, int32_t *X, int32_t *Y,
                        int32_t *OX, int32_t *OY, int32_t radius,
                        const int32_t *gravity,
                        const int32_t *friction, uint32_t nfriction,
                        int32_t bounce, uint32_t *stamp, uint32_t *mark,
                        int32_t *nx, int32_t *ny, uint32_t *tests)
{
    /* in the set's own place: p against where it is, o against where it
     * was before this tick's move */
    int64_t px = ((int64_t)*X - s->ox) >> 8, py = ((int64_t)*Y - s->oy) >> 8;
    int64_t ox = ((int64_t)*OX - s->ox + s->mx) >> 8;
    int64_t oy = ((int64_t)*OY - s->oy + s->my) >> 8;
    int64_t r = radius >> 8;
    if (r < 64)
        r = 64;                     /* a quarter pixel */
    /* where it came in, to its 16.16: the collision moves it in 1/256s,
     * and only that move is put back, so no fraction of its place or its
     * speed is lost to rounding at each touch */
    const int64_t px0 = px, py0 = py;
    int32_t gx = gravity[0], gy = gravity[1];
    int32_t near[NEAR_SEGS];
    /* round the path, as far as a push can carry it */
    uint32_t n = seg_near(s, (px < ox ? px : ox) - 3 * r,
                          (py < oy ? py : oy) - 3 * r,
                          (px > ox ? px : ox) + 3 * r,
                          (py > oy ? py : oy) + 3 * r,
                          near, NEAR_SEGS, stamp, ++*mark);
    *tests += n;
    struct touch t = { -1, 0, 0 };
    /* the path across the first line it crosses: slid back out along
     * that line's normal to r on the side it came from, keeping all of
     * its move along the line -- stopping it at the crossing would lose
     * that, every tick, on a curve */
    int64_t first = -1;
    int32_t fk = -1;
    for (uint32_t i = 0; i < n; i++) {
        int64_t c = seg_cross(s, near[i], ox, oy, px, py);
        if (c >= 0 && (first < 0 || c < first)) {
            first = c;
            fk = near[i];
        }
    }
    if (fk >= 0) {
        seg_side(s, fk, ox, oy, &t);
        int64_t ax = (int64_t)s->x0[fk] >> 8, ay = (int64_t)s->y0[fk] >> 8;
        int64_t side = ((px - ax) * t.nx + (py - ay) * t.ny) >> 16;
        if (side < r) {
            px += (int64_t)t.nx * (r - side) >> 16;
            py += (int64_t)t.ny * (r - side) >> 16;
        }
    }
    /* then nothing within r, twice over for corners */
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < n; i++)
            seg_push(s, near[i], &px, &py, ox, oy, r, &t);
    /* last, the path again: a push out of one side of a V can cross the
     * other, and a point is never let through a line -- back along its
     * path to just short of the first it crosses */
    for (int again = 0; again < 3; again++) {
        int64_t c = -1;
        int32_t ck = -1;
        for (uint32_t i = 0; i < n; i++) {
            int64_t cc = seg_cross(s, near[i], ox, oy, px, py);
            if (cc >= 0 && (c < 0 || cc < c)) {
                c = cc;
                ck = near[i];
            }
        }
        if (ck < 0)
            break;
        c = c > 4096 ? c - 4096 : 0;
        px = ox + ((px - ox) * c >> 16);
        py = oy + ((py - oy) * c >> 16);
        if (t.k < 0)
            seg_side(s, ck, ox, oy, &t);
    }
    if (t.k < 0) {
        *nx = *ny = 0;
        return 0;
    }
    /* in 16.16 from here: where it ends, and its speed against the set */
    int64_t X1 = (int64_t)*X + ((px - px0) << 8);
    int64_t Y1 = (int64_t)*Y + ((py - py0) << 8);
    int64_t vx = X1 - *OX - s->mx, vy = Y1 - *OY - s->my;
    /* the speed against the line: along it, less its friction; into it,
     * bounced back by bounce */
    int64_t vn = (vx * t.nx + vy * t.ny) >> 16;
    int64_t tx = vx - (vn * t.nx >> 16), ty = vy - (vn * t.ny >> 16);
    /* sliding -- coming within about 14 degrees of the line, as at each
     * joint of a curve drawn in lines, or along one -- its speed is what it
     * had before this tick's gravity, and what gravity gave or took over
     * the way it went, all of it turned along the line: a curve of lines
     * then slows a point no more than a curve. Friction follows. */
    int64_t qx = (int64_t)*X - *OX - s->mx - gx;
    int64_t qy = (int64_t)*Y - *OY - s->my - gy;
    int64_t qn = (qx * t.nx + qy * t.ny) >> 16;
    if (qx > -(1 << 26) && qx < (1 << 26) && qy > -(1 << 26) &&
        qy < (1 << 26)) {
        int64_t qq = qx * qx + qy * qy;
        int64_t tl = (int64_t)root64((uint64_t)(tx * tx + ty * ty));
        if (qn * qn * 16 < qq && tl > 0) {
            int64_t dx = X1 - *OX - s->mx, dy = Y1 - *OY - s->my;
            if (dx < -(1 << 26) || dx > (1 << 26) || dy < -(1 << 26) ||
                dy > (1 << 26))
                dx = dy = 0;
            /* of its speed across the line, only a curve's turn -- up to a
             * sixth of the whole, about 10 degrees -- is turned along it:
             * more is a push into the line, a ring's pressure say, and is
             * lost as any landing's; let through, a pressed bag would gain
             * speed from its own pressure */
            int64_t keep = qn * qn < qq / 32 ? qn * qn : qq / 32;
            int64_t e = qq - qn * qn + keep +
                        2 * ((int64_t)gx * dx + (int64_t)gy * dy);
            int64_t speed = e > 0 ? (int64_t)root64((uint64_t)e) : 0;
            if (speed > 0) {
                tx = tx * speed / tl;
                ty = ty * speed / tl;
                vn = 0;
            }
        }
    }
    uint8_t m = s->mat[t.k];
    int32_t f = nfriction ? friction[m < nfriction ? m : nfriction - 1] : 0;
    if (f < 0) f = 0;
    if (f > 65536) f = 65536;
    tx -= tx * f >> 16;
    ty -= ty * f >> 16;
    if (vn < 0)
        vn = -(vn * bounce >> 16);
    vx = tx + (vn * t.nx >> 16);
    vy = ty + (vn * t.ny >> 16);
    /* back to the world: o behind where it ends by the speed kept, and by
     * the set's own move, which carries it */
    *X = wrap32(X1);
    *Y = wrap32(Y1);
    *OX = wrap32(X1 - vx - s->mx);
    *OY = wrap32(Y1 - vy - s->my);
    *nx = t.nx;
    *ny = t.ny;
    return m + 1;
}

/* ---- a ring's area ------------------------------------------------------- */

/* links.ring(p, first, n, area, stiffness): points first to first + n - 1,
 * a closed ring, sped toward holding area square pixels -- all the way by
 * the next step at stiffness 1, a share of it below -- each along the way
 * its area grows fastest. The speed is given through where each point
 * was, not by moving it: links.step then carries it, and keeps it out of
 * the lines as it goes. Returns the area it held. */
NATIVE(links_ring)
{
    (void)argc;
    struct links l;
    if (!links_of(vm, a[0], "links.ring", &l))
        return 0;
    uint32_t np = used(&l, L_POINTS, l.cap_points);
    int32_t first = whole(a[1], 0), n = whole(a[2], 0);
    if (first < 0 || n < 3 || (uint32_t)first + (uint32_t)n > np)
        return vm_fail(vm, "links.ring() needs 3 or more points of the pool",
                       "", "");
    int64_t want = scaled(a[3]);                /* 16.16 square pixels */
    int32_t stiff = fx(a[4]);
    struct value *x = ELEMS(vm, l.f[L_X]), *y = ELEMS(vm, l.f[L_Y]);
    struct value *ox = ELEMS(vm, l.f[L_OX]), *oy = ELEMS(vm, l.f[L_OY]);
    struct value *pin = ELEMS(vm, l.f[L_PIN]);
    /* in 1/256 pixels: twice the area, and the area's growth for a move
     * of each point, the way it grows fastest */
    int64_t twice = 0, grow = 0;
    for (int32_t k = 0; k < n; k++) {
        uint32_t i = (uint32_t)(first + k);
        uint32_t j = (uint32_t)(first + (k + 1) % n);
        int64_t xi = fx(x[i]) >> 8, yi = fx(y[i]) >> 8;
        int64_t xj = fx(x[j]) >> 8, yj = fx(y[j]) >> 8;
        twice += xi * yj - xj * yi;
    }
    int sign = twice < 0 ? -1 : 1;
    int64_t have = twice * sign / 2;            /* 1/65536 square pixels */
    for (int32_t k = 0; k < n; k++) {
        uint32_t i = (uint32_t)(first + k);
        if (vm_truthy(vm, pin[i]))
            continue;
        uint32_t ip = (uint32_t)(first + (k + n - 1) % n);
        uint32_t in = (uint32_t)(first + (k + 1) % n);
        int64_t gx = ((int64_t)(fx(y[in]) - fx(y[ip])) >> 8) / 2;
        int64_t gy = ((int64_t)(fx(x[ip]) - fx(x[in])) >> 8) / 2;
        grow += gx * gx + gy * gy;
    }
    *out = mk(T_FIX, wrap32(have));
    vm_charge(vm, 10 + (uint32_t)n * 24);
    if (!grow || !stiff)
        return 1;
    /* the share of each point's way to move, 16.16, at most half a pixel
     * a tick for each pixel of its way */
    int64_t lambda = ((want - have) << 16) / grow;
    lambda = lambda * stiff >> 16;
    if (lambda > 32768) lambda = 32768;
    if (lambda < -32768) lambda = -32768;
    for (int32_t k = 0; k < n; k++) {
        uint32_t i = (uint32_t)(first + k);
        if (vm_truthy(vm, pin[i]))
            continue;
        uint32_t ip = (uint32_t)(first + (k + n - 1) % n);
        uint32_t in = (uint32_t)(first + (k + 1) % n);
        int64_t gx = ((int64_t)(fx(y[in]) - fx(y[ip])) >> 8) / 2 * sign;
        int64_t gy = ((int64_t)(fx(x[ip]) - fx(x[in])) >> 8) / 2 * sign;
        ox[i] = mk(T_FIX, wrap32(fx(ox[i]) - ((gx * lambda) >> 8)));
        oy[i] = mk(T_FIX, wrap32(fx(oy[i]) - ((gy * lambda) >> 8)));
    }
    return 1;
}

/* links.step(p, solid): a tick of every point and link, then points out
 * of map cells whose flags share a bit with solid (0, none), and out of
 * the pool's field's solid cells */
NATIVE(links_step)
{
    struct links l;
    if (!links_of(vm, a[0], "links.step", &l))
        return 0;
    int32_t mask = whole(a[1], 0);
    uint32_t np = used(&l, L_POINTS, l.cap_points);
    uint32_t nl = used(&l, L_LINKS, l.cap_links);
    int32_t g = fx(l.f[L_GRAVITY]), gx = fx(l.f[L_GX]), drag = fx(l.f[L_DRAG]);
    int slack = vm_truthy(vm, l.f[L_SLACK]);
    int32_t tear = fx(l.f[L_TEAR]);
    uint32_t broken = 0;
    int32_t passes = whole(l.f[L_PASSES], 4);
    if (passes < 1) passes = 1;
    if (passes > 16) passes = 16;
    struct value *x = ELEMS(vm, l.f[L_X]), *y = ELEMS(vm, l.f[L_Y]);
    struct value *ox = ELEMS(vm, l.f[L_OX]), *oy = ELEMS(vm, l.f[L_OY]);
    struct value *z = ELEMS(vm, l.f[L_Z]), *oz = ELEMS(vm, l.f[L_OZ]);
    struct value *pin = ELEMS(vm, l.f[L_PIN]);
    struct cellworld cw;
    int field = l.f[L_FIELD].tag == T_IMG;
    int32_t fx0 = whole(l.f[L_FIELD_X], 0), fy0 = whole(l.f[L_FIELD_Y], 0);
    if (field && !cells_world(vm, l.f[L_FIELD], l.f[L_RULES], &cw))
        return 0;
    /* on by what each moved last tick, less drag, plus gravity; in the
     * field's liquid, half again of what is left */
    /* damp: each point's speed drawn toward the pool's mean, which stills
     * a body's wobble without slowing it as a whole */
    int32_t damp = fx(l.f[L_DAMP]);
    int64_t mvx = 0, mvy = 0, mvz = 0;
    uint32_t free = 0;
    if (damp) {
        for (uint32_t i = 0; i < np; i++)
            if (!vm_truthy(vm, pin[i])) {
                mvx += fx(x[i]) - fx(ox[i]);
                mvy += fx(y[i]) - fx(oy[i]);
                mvz += fx(z[i]) - fx(oz[i]);
                free++;
            }
        if (free) {
            mvx /= free;
            mvy /= free;
            mvz /= free;
        }
    }
    for (uint32_t i = 0; i < np; i++) {
        int32_t X = fx(x[i]), Y = fx(y[i]), Z = fx(z[i]);
        if (vm_truthy(vm, pin[i])) {
            ox[i] = x[i];
            oy[i] = y[i];
            oz[i] = z[i];
            continue;
        }
        int32_t vx = X - fx(ox[i]), vy = Y - fx(oy[i]), vz = Z - fx(oz[i]);
        if (free) {
            vx -= (int32_t)(((vx - mvx) * damp) >> 16);
            vy -= (int32_t)(((vy - mvy) * damp) >> 16);
            vz -= (int32_t)(((vz - mvz) * damp) >> 16);
        }
        if (drag) {
            vx -= (int32_t)(((int64_t)vx * drag) >> 16);
            vy -= (int32_t)(((int64_t)vy * drag) >> 16);
            vz -= (int32_t)(((int64_t)vz * drag) >> 16);
        }
        if (field) {
            int32_t m = cell_at(&cw, fx0, fy0, X, Y);
            if (m >= 0 && (cw.flags[m] & CELL_LIQUID)) {
                vx /= 2;
                vy /= 2;
                vz /= 2;
            }
        }
        ox[i] = mk(T_FIX, X);
        oy[i] = mk(T_FIX, Y);
        oz[i] = mk(T_FIX, Z);
        x[i] = mk(T_FIX, X + vx + gx);
        y[i] = mk(T_FIX, Y + vy + g);
        z[i] = mk(T_FIX, Z + vz);
    }
    /* each link pulled toward its length, split between its ends, all of
     * it to the free end when the other is pinned */
    struct value *la = ELEMS(vm, l.f[L_A]), *lb = ELEMS(vm, l.f[L_B]);
    struct value *len = ELEMS(vm, l.f[L_LEN]);
    /* The passes work on plain numbers, read from the pool's arrays once
     * and written back once, rather than six read and six written a link
     * a pass: P the points (x, y, z), pinned, and each link's ends (-1
     * for none or a broken one) and length */
    static int32_t P[MAX_POINTS * 3], A[MAX_LINKS], B[MAX_LINKS];
    static int32_t LEN[MAX_LINKS];
    static uint8_t pinned[MAX_POINTS];
    for (uint32_t i = 0; i < np; i++) {
        P[i * 3] = fx(x[i]);
        P[i * 3 + 1] = fx(y[i]);
        P[i * 3 + 2] = fx(z[i]);
        pinned[i] = (uint8_t)vm_truthy(vm, pin[i]);
    }
    for (uint32_t k = 0; k < nl; k++) {
        int32_t i = whole(la[k], -1), j = whole(lb[k], -1);
        int ok = i >= 0 && j >= 0 && (uint32_t)i < np && (uint32_t)j < np &&
                 !(pinned[i] && pinned[j]);
        A[k] = ok ? i : -1;
        B[k] = j;
        LEN[k] = fx(len[k]);
    }
    for (int32_t p = 0; p < passes; p++)
        for (uint32_t k = 0; k < nl; k++) {
            int32_t i = A[k], j = B[k];
            if (i < 0)
                continue;
            int pi = pinned[i], pj = pinned[j];
            int32_t *Pi = P + i * 3, *Pj = P + j * 3;
            int32_t xi = Pi[0], yi = Pi[1], zi = Pi[2];
            int32_t xj = Pj[0], yj = Pj[1], zj = Pj[2];
            int64_t dx = (int64_t)xj - xi, dy = (int64_t)yj - yi;
            int64_t dz = (int64_t)zj - zi;
            /* A link under 64 pixels -- a cloth's or a rope's -- is worked
             * in 1/256ths of a pixel, where its square fits 32 bits and its
             * root and divide are 32-bit; a longer one exactly, in 64 */
            const int64_t NEAR = (int64_t)1 << 22;
            int short_link = dx > -NEAR && dx < NEAR && dy > -NEAR &&
                             dy < NEAR && dz > -NEAR && dz < NEAR;
            int32_t d, d8 = 0;
            if (short_link) {
                int32_t ax = (int32_t)dx >> 8, ay = (int32_t)dy >> 8;
                int32_t az = (int32_t)dz >> 8;
                d8 = (int32_t)root32((uint32_t)(ax * ax) + (uint32_t)(ay * ay) +
                                     (uint32_t)(az * az));
                d = d8 << 8;
            } else
                d = span(dx, dy, dz);
            /* torn on the first pass, at the stretch the tick left it */
            if (p == 0 && tear > 0 &&
                d > LEN[k] + (((int64_t)LEN[k] * tear) >> 16)) {
                la[k] = mk(T_INT, -1);
                A[k] = -1;
                broken++;
                continue;
            }
            if (d < 16)
                continue;
            /* the share of the gap each end moves, 16.16 of the vector;
             * a slack link only pulls */
            int64_t gap = (int64_t)d - LEN[k];
            if (slack && gap < 0)
                continue;
            int64_t s;
            if (short_link && d8 > 0 && gap > -NEAR && gap < NEAR)
                s = (int32_t)gap * 256 / d8;
            else
                s = (gap << 16) / d;
            /* in 64 bits, wrapped as stored: a cart's far-flung points or
             * long links can make a move of 32 bits or more */
            int64_t mx = (dx * s) >> 16, my = (dy * s) >> 16;
            int64_t mz = (dz * s) >> 16;
            if (pi) {
                Pj[0] = wrap32(xj - mx);
                Pj[1] = wrap32(yj - my);
                Pj[2] = wrap32(zj - mz);
            } else if (pj) {
                Pi[0] = wrap32(xi + mx);
                Pi[1] = wrap32(yi + my);
                Pi[2] = wrap32(zi + mz);
            } else {
                Pi[0] = wrap32(xi + mx / 2);
                Pi[1] = wrap32(yi + my / 2);
                Pi[2] = wrap32(zi + mz / 2);
                Pj[0] = wrap32(xj - mx / 2);
                Pj[1] = wrap32(yj - my / 2);
                Pj[2] = wrap32(zj - mz / 2);
            }
        }
    /* back into the pool; a pinned point never moves, so is left as it is */
    for (uint32_t i = 0; i < np; i++)
        if (!pinned[i]) {
            x[i] = mk(T_FIX, P[i * 3]);
            y[i] = mk(T_FIX, P[i * 3 + 1]);
            z[i] = mk(T_FIX, P[i * 3 + 2]);
        }
    /* out of the segment set's lines, and what each point touched */
    uint32_t tests = 0, touched = 0;
    if (l.f[L_SEGS].tag == T_OBJ) {
        struct segs sg;
        if (!segs_of(vm, l.f[L_SEGS], "links.step", &sg))
            return 0;
        static int32_t fr[256];
        uint32_t nfr = 0;
        struct column fc;
        if (is_num(l.f[L_FRICTION])) {
            fr[0] = fx(l.f[L_FRICTION]);
            nfr = 1;
        } else if (column_of(vm, l.f[L_FRICTION], &fc) && fc.tag != T_INT)
            for (; nfr < fc.len && nfr < 256; nfr++)
                fr[nfr] = column_fix(&fc, nfr);
        static uint32_t stamp[MAX_SEGS], mark;
        if (mark > 0xfffff000u) {
            memset(stamp, 0, sizeof(stamp));
            mark = 0;
        }
        int32_t radius = fx(l.f[L_RADIUS]), bounce = fx(l.f[L_BOUNCE]);
        const int32_t grav[2] = { gx, g };
        /* what each touched, three numbers a point, when asked */
        struct value o = argc > 2 ? a[2] : mk(T_NULL, 0);
        int give = o.tag != T_NULL;
        if (give && !vm_out_ok(vm, o, np * 3))
            return vm_fail(vm, "links.step() writes three numbers a point "
                           "to an array or Int32Array", "", "");
        for (uint32_t i = 0; i < np; i++) {
            int32_t m = 0, NX = 0, NY = 0;
            if (!pinned[i]) {
                int32_t X = fx(x[i]), Y = fx(y[i]), OX = fx(ox[i]);
                int32_t OY = fx(oy[i]);
                m = seg_meet(&sg, &X, &Y, &OX, &OY, radius, grav, fr, nfr, bounce,
                             stamp, &mark, &NX, &NY, &tests);
                if (m) {
                    x[i] = mk(T_FIX, X);
                    y[i] = mk(T_FIX, Y);
                    ox[i] = mk(T_FIX, OX);
                    oy[i] = mk(T_FIX, OY);
                    touched++;
                }
            }
            if (give) {
                vm_out_set(vm, o, i * 3, mk(T_INT, m));
                vm_out_set(vm, o, i * 3 + 1, mk(T_FIX, NX));
                vm_out_set(vm, o, i * 3 + 2, mk(T_FIX, NY));
            }
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
    /* out of the field's solid cells the same way */
    if (field)
        for (uint32_t i = 0; i < np; i++) {
            int32_t X = fx(x[i]), Y = fx(y[i]);
            if (!cell_solid(&cw, fx0, fy0, X, Y))
                continue;
            int32_t OX = fx(ox[i]), OY = fx(oy[i]);
            if (!cell_solid(&cw, fx0, fy0, OX, Y))
                x[i] = mk(T_FIX, OX);
            else if (!cell_solid(&cw, fx0, fy0, X, OY))
                y[i] = mk(T_FIX, OY);
            else {
                x[i] = mk(T_FIX, OX);
                y[i] = mk(T_FIX, OY);
            }
        }
    if (broken)
        set_used(vm, a[0], L_BROKEN,
                 (uint32_t)whole(l.f[L_BROKEN], 0) + broken);
    /* measured on the ARM rig (.build/arcade-rig, sysprobe): a link a
     * pass about 17 units, most of it its root and divide */
    vm_charge(vm, 10 + np * 14 + nl * (uint32_t)passes * 17 +
                  (mask ? np * 4 : 0) + (field ? np * 7 : 0) +
                  tests * SEG_TEST + touched * SEG_TOUCH);
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
    /* the fields are the cart's to change: each bounded by the pool
     * before any product, so none can wrap */
    if (cols < 2 || rows < 2 || (uint32_t)cols > np || (uint32_t)rows > np ||
        first < 0 || fl < 0 || (uint32_t)first > np || (uint32_t)fl > nl ||
        (uint32_t)(cols * rows) > np - (uint32_t)first ||
        (uint32_t)((cols - 1) * rows + cols * (rows - 1)) > nl - (uint32_t)fl)
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
