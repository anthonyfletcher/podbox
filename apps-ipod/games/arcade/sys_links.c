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
 * bounce, and segs.move moves a set and carries what rests on it. segs
 * may be an array of sets -- the level, and its gates and lifts -- each
 * met in turn where its bounds reach a point's path.
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
static uint32_t root32(uint32_t v);
static uint32_t root(uint64_t v)
{
    if (!(v >> 32))
        return root32((uint32_t)v);
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
    uint32_t r = 0, bit = v >> 16 ? 1u << 30 : 1u << 14;
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

/* ---- segment sets --------------------------------------------------------- *
 * The outlines points meet, as polyfill draws them: each outline's lines,
 * each with a material, filed by the grid cells it crosses so a point
 * looks only at the lines near it. A set has an offset, which segs.move
 * changes: a lift or a gate moves without being built again, and carries
 * what rests on it. */

#define MAX_SEGS  4096
#define MAX_SETS  16                /* a pool's segs, as an array */
#define SEG_CELLS 16384             /* the grid's cells, at most */
#define SEG_LONG  2048              /* a line too long to size the grid */

/* units, measured on the ARM rig (linkprice, linkcontact): each free
 * point looked at, each line found near it, each of those whose box
 * reaches its path and is tested in full, each point that touches one --
 * its push, roots and 64-bit divides among it -- and each of those that
 * slides along it, two roots and two divides more; and the grid cells and
 * filed lines its search passes over, a unit each SEG_SEEN, so a wide
 * search is paid for though it finds few lines */
#define SEG_POINT 36
#define SEG_TEST  5
#define SEG_KEPT  17
#define SEG_TOUCH 98
#define SEG_SLIDE 39
#define SEG_SEEN  4
#define SEG_SKIP  2                 /* a set whose bounds are not reached */

/* what the points' meetings with the lines did, for the charge */
struct seg_counts
{
    uint32_t tests, kept, slid, seen;
};

enum { S_X0, S_Y0, S_X1, S_Y1, S_MAT, S_START, S_ITEMS, S_COUNT, S_OX, S_OY,
       S_MX, S_MY, S_MOVED, S_CELL, S_GX, S_GY, S_GW, S_GH, S_BX0, S_BY0,
       S_BX1, S_BY1, S_SIDE, S_FIELDS };
static const char *const seg_keys[S_FIELDS] = {
    "x0", "y0", "x1", "y1", "mat", "start", "items", "count", "ox", "oy",
    "mx", "my", "moved", "cell", "gx", "gy", "gw", "gh", "bx0", "by0",
    "bx1", "by1", "side"
};

/* A set handed back: its lines (16.16, before the offset), materials, and
 * the grid that files them */
struct segs
{
    const int32_t *x0, *y0, *x1, *y1, *start, *items;
    uint32_t nitems;
    const uint8_t *mat;
    uint32_t count;
    int32_t ox, oy, mx, my;         /* the offset, and its move this tick */
    int32_t cell, gx, gy, gw, gh;
    int32_t bx0, by0, bx1, by1;     /* its lines' bounds, whole pixels */
    uint32_t gen;                   /* its normals' mark, below */
    const uint8_t *side;            /* each line's air side, or NULL */
};

/* side[k]: which way line k's unflipped normal, (-ey, ex), faces -- toward
 * the air (SIDE_AIR), toward the solid (SIDE_SOLID), or not known
 * (SIDE_NONE: a line of two points), when a point beside it is put on the
 * side it came from */
enum { SIDE_NONE, SIDE_AIR, SIDE_SOLID };

/* Each line's normal, unflipped, 16.16, once worked out while a set is in
 * use: a root and two 64-bit divides that every point resting on the line,
 * and each push pass, would make again. A line is k in its set, and sets
 * share the slots, so each is marked with the gen of the set it is for. */
static int32_t norm_x[MAX_SEGS], norm_y[MAX_SEGS];
static uint32_t norm_mark[MAX_SEGS], norm_gen;

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
    if (!++norm_gen)
        ++norm_gen;                 /* 0 marks a slot never filled */
    s->gen = norm_gen;
    s->side = f[S_SIDE].tag == T_U8 && U8A(vm, f[S_SIDE])->len >= s->count
              ? U8A(vm, f[S_SIDE])->d : NULL;
    s->bx0 = whole(f[S_BX0], INT32_MIN);
    s->by0 = whole(f[S_BY0], INT32_MIN);
    s->bx1 = whole(f[S_BX1], INT32_MAX);
    s->by1 = whole(f[S_BY1], INT32_MAX);
    /* the arrays as long as the counts say, or the set is not one made;
     * the cells in 64 bits, since a cart can write any width and height */
    uint64_t cells = (uint64_t)(uint32_t)s->gw * (uint32_t)s->gh;
    if (s->count > MAX_SEGS || s->cell < 1 || s->gw < 1 || s->gh < 1 ||
        cells > SEG_CELLS || I32(vm, f[S_X0])->len < s->count ||
        I32(vm, f[S_Y0])->len < s->count || I32(vm, f[S_X1])->len < s->count ||
        I32(vm, f[S_Y1])->len < s->count || U8A(vm, f[S_MAT])->len < s->count ||
        I32(vm, f[S_START])->len != cells + 1 ||
        (uint32_t)s->start[cells] > I32(vm, f[S_ITEMS])->len)
        return vm_fail(vm, fn, "() needs a set whose arrays are as "
                       "segs.make() made them", "");
    s->nitems = I32(vm, f[S_ITEMS])->len;
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
    /* a line beyond the grid filed in the cells along its edge, where a
     * point beyond it looks */
    int64_t r0 = (y0 - s->gy) / s->cell, r1 = (y1 - s->gy) / s->cell;
    if (r0 < 0) r0 = 0;
    if (r1 < 0) r1 = 0;
    if (r0 > s->gh - 1) r0 = s->gh - 1;
    if (r1 > s->gh - 1) r1 = s->gh - 1;
    for (int32_t r = (int32_t)r0; r <= r1; r++) {
        /* the line's x where it enters and leaves this row of cells, the
         * edge rows reaching as far as the line does */
        int64_t ya = r == 0 ? y0 : (int64_t)s->gy + (int64_t)r * s->cell;
        int64_t yb = r == s->gh - 1 ? y1
                     : (int64_t)s->gy + (int64_t)(r + 1) * s->cell;
        if (ya < y0) ya = y0;
        if (yb > y1) yb = y1;
        int64_t xa = x0, xb = x1;
        if (y1 != y0) {
            xa = x0 + (x1 - x0) * (ya - y0) / (y1 - y0);
            xb = x0 + (x1 - x0) * (yb - y0) / (y1 - y0);
        }
        if (xa > xb) { int64_t t = xa; xa = xb; xb = t; }
        int64_t c0 = (xa - s->gx) / s->cell, c1 = (xb - s->gx) / s->cell;
        if (c0 < 0) c0 = 0;
        if (c1 < 0) c1 = 0;
        if (c0 > s->gw - 1) c0 = s->gw - 1;
        if (c1 > s->gw - 1) c1 = s->gw - 1;
        for (int32_t c = (int32_t)c0; c <= c1; c++) {
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
    static uint8_t M[MAX_SEGS], SD[MAX_SEGS];
    /* each closed outline's first line and how many */
    static uint32_t ofirst[MAX_SEGS], olen[MAX_SEGS];
    uint32_t count = 0, s0 = 0, outlines = 0;
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
        if (m >= 3) {
            ofirst[outlines] = count;
            olen[outlines++] = m;
        }
        for (uint32_t i = 0; m >= 2 && i < (m == 2 ? 1 : m); i++) {
            uint32_t p = s0 + i, q = s0 + (i + 1) % m;
            X0[count] = column_fix(&xs, p);
            Y0[count] = column_fix(&ys, p);
            X1[count] = column_fix(&xs, q);
            Y1[count] = column_fix(&ys, q);
            M[count] = (uint8_t)(mats.tag == T_NULL ? 0
                                 : column_at(&mats, p, 0));
            SD[count] = SIDE_NONE;
            count++;
        }
        s0 = e;
    }
    /* Which side of each line of a closed outline is solid: inside an odd
     * number of outlines, as polyfill fills them. An outline's inside is
     * on the left of its lines when it winds one way (its area above 0),
     * on the right the other; and it is solid when as many outlines again
     * hold it as an even number. So a point already in the solid is put
     * out into the air, and one at a convex corner is not taken for coming
     * from inside by where the next line would run on past the corner. */
    for (uint32_t i = 0; i < outlines; i++) {
        int64_t area = 0;
        for (uint32_t j = 0; j < olen[i]; j++) {
            uint32_t k = ofirst[i] + j;
            area += ((int64_t)X0[k] >> 8) * ((int64_t)Y1[k] >> 8) -
                    ((int64_t)X1[k] >> 8) * ((int64_t)Y0[k] >> 8);
        }
        if (!area)
            continue;
        /* a point just off its first line's middle, inside it */
        uint32_t f0 = ofirst[i];
        int64_t mx = (((int64_t)X0[f0] + X1[f0]) >> 9);
        int64_t my = (((int64_t)Y0[f0] + Y1[f0]) >> 9);
        uint32_t held = 0;
        for (uint32_t o = 0; o < outlines; o++) {
            if (o == i)
                continue;
            int in = 0;
            for (uint32_t j = 0; j < olen[o]; j++) {
                uint32_t k = ofirst[o] + j;
                int64_t ay = (int64_t)Y0[k] >> 8, by = (int64_t)Y1[k] >> 8;
                if ((ay > my) == (by > my))
                    continue;
                int64_t ax = (int64_t)X0[k] >> 8, bx = (int64_t)X1[k] >> 8;
                /* where the line crosses the row my, against mx */
                if (((mx - ax) * (by - ay) < (bx - ax) * (my - ay)) ==
                    (by - ay > 0))
                    in = !in;
            }
            held += (uint32_t)in;
        }
        /* the unflipped normal faces the inside when the area is above 0 */
        int inside_solid = !(held & 1);
        uint8_t sd = (area > 0) == inside_solid ? SIDE_SOLID : SIDE_AIR;
        for (uint32_t j = 0; j < olen[i]; j++)
            SD[ofirst[i] + j] = sd;
    }
    /* The grid over the lines a level is made of: cells of 16 pixels,
     * larger to keep it small. A line longer than SEG_LONG -- a wall
     * reaching far beyond the level, as segs.trace's outside makes -- is
     * left out of its size, so it cannot spread the cells thin, and is
     * filed in the grid's edge cells; all of them when every line is. */
    int32_t lx = 0, ly = 0, hx = 0, hy = 0;
    int any = 0;
    for (int pass = 0; pass < 2 && !any; pass++)
        for (uint32_t k = 0; k < count; k++) {
            int32_t ax = X0[k] >> 16, ay = Y0[k] >> 16, bx = X1[k] >> 16;
            int32_t by = Y1[k] >> 16;
            int64_t span = (int64_t)(bx > ax ? bx - ax : ax - bx) +
                           (by > ay ? by - ay : ay - by);
            if (!pass && span > SEG_LONG)
                continue;
            if (!any) { lx = hx = ax; ly = hy = ay; any = 1; }
            if (ax < lx) lx = ax;
            if (bx < lx) lx = bx;
            if (ax > hx) hx = ax;
            if (bx > hx) hx = bx;
            if (ay < ly) ly = ay;
            if (by < ly) ly = by;
            if (ay > hy) hy = ay;
            if (by > hy) hy = by;
        }
    /* every line's bounds, the long ones too, for a step to skip the set
     * where no point comes near it */
    int32_t bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    for (uint32_t k = 0; k < count; k++) {
        int32_t ax = X0[k] >> 16, ay = Y0[k] >> 16, bx = X1[k] >> 16;
        int32_t by = Y1[k] >> 16;
        if (!k) { bx0 = bx1 = ax; by0 = by1 = ay; }
        if (ax < bx0) bx0 = ax;
        if (bx < bx0) bx0 = bx;
        if (ax > bx1) bx1 = ax;
        if (bx > bx1) bx1 = bx;
        if (ay < by0) by0 = ay;
        if (by < by0) by0 = by;
        if (ay > by1) by1 = ay;
        if (by > by1) by1 = by;
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
    if (!new_ints(vm, count ? count : 1, 1, &v)) {
        vm_unroot(vm);
        return 0;
    }
    BUF(vm, OBJ(vm, *out)->buf)->v[S_SIDE] = v;
    vm_unroot(vm);
    struct value *F = BUF(vm, OBJ(vm, *out)->buf)->v;
    memcpy(I32(vm, F[S_X0])->d, X0, count * 4);
    memcpy(I32(vm, F[S_Y0])->d, Y0, count * 4);
    memcpy(I32(vm, F[S_X1])->d, X1, count * 4);
    memcpy(I32(vm, F[S_Y1])->d, Y1, count * 4);
    memcpy(U8A(vm, F[S_MAT])->d, M, count);
    memcpy(U8A(vm, F[S_SIDE])->d, SD, count);
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
    F[S_BX0] = mk(T_INT, bx0);
    F[S_BY0] = mk(T_INT, by0);
    F[S_BX1] = mk(T_INT, bx1 + 1);
    F[S_BY1] = mk(T_INT, by1 + 1);
    /* measured on the ARM rig (bagprobe): most of it the divisions that
     * find each line's cells */
    vm_charge(vm, 40 + count * 80 + items * 10 + cells / 4 +
                  outlines * count / 4);
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
 * outlines -- one round each solid region, and one round each hole in it.
 * A cell is a full square, or by a table of shapes a slope: half of it cut
 * corner to corner, or a shallow slope rising half a cell across it. Each
 * shape is an outline on a lattice of half cells; where two cells' sides
 * meet, a half cell at a time, they cancel, and what is left is walked.
 * A corner between two square sides is cut by half a cell, as marching
 * squares would, but where a cell is sharp; a slope's line is kept whole;
 * then passes round every corner again. */

#define TRACE_POINTS 4096           /* the most points it gives */
#define TRACE_SHAPES 14             /* the shapes, 0 for none */
#define TRACE_FAR    4096           /* the far outline beyond the area */

enum { T_XS, T_YS, T_ENDS, T_N, T_FIELDS };
static const char *const trace_keys[T_FIELDS] = { "x", "y", "ends", "n" };

/* Each shape's corners in half cells, solid on the right as they run --
 * clockwise on the screen: the count, then x, y pairs */
static const int8_t trace_shape[TRACE_SHAPES][9] = {
    { 0 },
    { 4, 0, 0, 2, 0, 2, 2, 0, 2 },  /* 1 full */
    { 3, 0, 2, 2, 0, 2, 2 },        /* 2 floor rising right */
    { 3, 0, 0, 2, 2, 0, 2 },        /* 3 floor rising left */
    { 3, 0, 0, 2, 0, 2, 2 },        /* 4 ceiling falling right */
    { 3, 0, 0, 2, 0, 0, 2 },        /* 5 ceiling falling left */
    { 3, 0, 2, 2, 1, 2, 2 },        /* 6 shallow floor right, low half */
    { 4, 0, 1, 2, 0, 2, 2, 0, 2 },  /* 7 shallow floor right, high half */
    { 4, 0, 0, 2, 1, 2, 2, 0, 2 },  /* 8 shallow floor left, high half */
    { 3, 0, 1, 2, 2, 0, 2 },        /* 9 shallow floor left, low half */
    { 3, 0, 0, 2, 0, 2, 1 },        /* 10-13: 6-9 upside down */
    { 4, 0, 0, 2, 0, 2, 2, 0, 1 },
    { 4, 0, 0, 2, 0, 2, 1, 0, 2 },
    { 3, 0, 0, 2, 0, 0, 1 },
};

/* The ways an edge runs, in half cells, clockwise on the screen from
 * right; the opposite way is six on. A square side runs a half cell at a
 * time, a slope's line whole. */
#define TRACE_WAYS 12
static const int8_t way_x[TRACE_WAYS] = { 1, 2, 2, 0, -2, -2,
                                          -1, -2, -2, 0, 2, 2 };
static const int8_t way_y[TRACE_WAYS] = { 0, 1, 2, 1, 2, 1,
                                          0, -1, -2, -1, -2, -1 };
static const int16_t way_deg[TRACE_WAYS] = { 0, 27, 45, 90, 135, 153,
                                             180, 207, 225, 270, 315, 333 };
#define WAY_SQUARE(d) ((d) % 3 == 0)

static int trace_way(int dx, int dy)
{
    for (int d = 0; d < TRACE_WAYS; d++)
        if (way_x[d] == dx && way_y[d] == dy)
            return d;
    return -1;
}

/* An edge of len steps the way d from v laid on the lattice, cancelling
 * the edge beside it that runs the other way: the edges left, counted */
static void trace_lay(uint16_t *outs, uint32_t v, int d, int len,
                      uint32_t lw, uint32_t *edges)
{
    int32_t step = way_y[d] * (int32_t)lw + way_x[d];
    int back = d < TRACE_WAYS / 2 ? d + TRACE_WAYS / 2 : d - TRACE_WAYS / 2;
    for (int k = 0; k < len; k++) {
        uint32_t to = (uint32_t)((int32_t)v + step);
        if (outs[to] & (1u << back)) {
            outs[to] &= (uint16_t)~(1u << back);
            (*edges)--;
        } else {
            outs[v] |= (uint16_t)(1u << d);
            (*edges)++;
        }
        v = to;
    }
}

/* The first point of the lattice from v with a way out, or verts: kept
 * apart from the walk, so its loop has registers to itself */
static uint32_t trace_next(const uint16_t *outs, uint32_t v, uint32_t verts)
{
    while (v < verts && !outs[v])
        v++;
    return v;
}

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

/* the working memory, held bytes of it, given back, and the cart stopped
 * with why */
static int trace_fail(struct vm *vm, uint32_t held, const char *why)
{
    gc_scratch_end(vm, held);
    return vm_fail(vm, why, "", "");
}

/* segs.trace(solid, [sharp], [smooth], [cx, cy, cw, ch], [shapes],
 * [outside]): the outlines of the map cells whose flags share a bit with
 * solid, in world pixels; a corner of a cell whose flags share a bit with
 * sharp is kept square; smooth (0 to 3) passes round them. Within the
 * cells cx, cy, cw by ch, the whole map when left out. shapes, by sprite,
 * gives a cell's shape (0 or missing, full). outside makes the cells
 * beyond the area solid -- true, all of them, else beyond the sides it
 * names -- a ring of cells whose own outside is taken far out. Returns
 * a record: x, y and ends, as polyfill and segs.make take them, and n,
 * the points. */
NATIVE(segs_trace)
{
    int32_t mask = whole(a[0], 0);
    int32_t sharpm = argc > 1 ? whole(a[1], 0) : 0;
    int32_t passes = argc > 2 ? whole(a[2], 0) : 0;
    if (passes < 0) passes = 0;
    if (passes > 3) passes = 3;
    int32_t c0 = 0, r0 = 0, w = vm->map_w, h = vm->map_h;
    if (argc > 3 && a[3].tag != T_NULL) {
        c0 = whole(a[3], 0);
        r0 = argc > 4 ? whole(a[4], 0) : 0;
        w = argc > 5 ? whole(a[5], w) : w;
        h = argc > 6 ? whole(a[6], h) : h;
    }
    struct column shapes;
    shapes.tag = T_NULL;
    if (argc > 7 && a[7].tag != T_NULL &&
        (!column_of(vm, a[7], &shapes) || shapes.tag == T_INT))
        return vm_fail(vm, "segs.trace()'s shapes is an array, a shape for "
                       "each sprite", "", "");
    /* the sides beyond which all is solid: 1 (true) all four, else 2
     * left, 4 right, 8 above and 16 below, kept as bits 0-3 */
    int32_t outside = argc < 9 ? 0 : whole(a[8], 0);
    outside = outside & 1 ? 15 : (outside >> 1) & 15;
    /* within the map, its edges worked in 64 bits: a cart may pass any
     * 32-bit corner and size */
    int64_t ex = (int64_t)c0 + w, ey = (int64_t)r0 + h;
    int64_t sx = c0 < 0 ? 0 : c0 > vm->map_w ? vm->map_w : c0;
    int64_t sy = r0 < 0 ? 0 : r0 > vm->map_h ? vm->map_h : r0;
    if (ex > vm->map_w) ex = vm->map_w;
    if (ey > vm->map_h) ey = vm->map_h;
    c0 = (int32_t)sx;
    r0 = (int32_t)sy;
    w = ex > sx ? (int32_t)(ex - sx) : 0;
    h = ey > sy ? (int32_t)(ey - sy) : 0;
    /* the cells traced: the area, and with outside a ring of solid cells
     * round it, kept square; and round those a border of none */
    int32_t ring = outside ? 1 : 0;
    uint32_t cw = (uint32_t)(w + 2 * ring), ch = (uint32_t)(h + 2 * ring);
    uint32_t gw = cw + 2, lw = 2 * cw + 1, lh = 2 * ch + 1;
    uint32_t verts = lw * lh;
    /* first the cells -- each its shape, and 16 when sharp -- and per
     * point of the lattice the ways out of it */
    uint32_t gbytes = ((gw * (ch + 2) + 1 + 1) & ~1u) + verts * 2;
    uint32_t goff = gc_scratch(vm, gbytes);
    if (!goff)
        return 0;
    uint8_t *grid = MEM(vm, goff);
    uint16_t *outs = (uint16_t *)(void *)(grid + ((gw * (ch + 2) + 1) & ~1u));
    {
        const uint8_t *map = MEM(vm, vm->map), *flags = MEM(vm, vm->flags);
        for (uint32_t r = 0; r < ch; r++)
            for (uint32_t c = 0; c < cw; c++) {
                uint8_t g;
                int beyond = ring && (r == 0 || c == 0 || r == ch - 1 ||
                                      c == cw - 1);
                if (beyond)
                    g = ((c == 0 && (outside & 1)) ||
                         (c == cw - 1 && (outside & 2)) ||
                         (r == 0 && (outside & 4)) ||
                         (r == ch - 1 && (outside & 8))) ? 1 | 16 : 0;
                else {
                    uint8_t sp = map[(uint32_t)(r0 + (int32_t)r - ring) *
                                     (uint32_t)vm->map_w +
                                     (uint32_t)(c0 + (int32_t)c - ring)];
                    uint8_t f = flags[sp];
                    g = 0;
                    if (f & mask) {
                        int32_t s = shapes.tag == T_NULL ? 1
                                    : sp < shapes.len
                                    ? column_at(&shapes, sp, 1) : 1;
                        g = (uint8_t)(s >= 1 && s < TRACE_SHAPES ? s : 1);
                        if (f & sharpm)
                            g |= 16;
                    }
                }
                grid[(r + 1) * gw + c + 1] = g;
            }
    }
    /* each cell's outline laid on the lattice, a side cancelling the side
     * of the cell beside it that runs the other way; a full cell's side
     * against another full cell is left out, as the two would cancel */
    uint32_t edges = 0;
    for (uint32_t r = 0; r < ch; r++)
        for (uint32_t c = 0; c < cw; c++) {
            const uint8_t *gc = grid + (r + 1) * gw + c + 1;
            int shape = *gc & 15;
            uint32_t v = 2 * r * lw + 2 * c;
            if (!shape)
                continue;
            if (shape == 1) {
                if ((gc[-(int32_t)gw] & 15) != 1)
                    trace_lay(outs, v, 0, 2, lw, &edges);
                if ((gc[1] & 15) != 1)
                    trace_lay(outs, v + 2, 3, 2, lw, &edges);
                if ((gc[gw] & 15) != 1)
                    trace_lay(outs, v + 2 * lw + 2, 6, 2, lw, &edges);
                if ((gc[-1] & 15) != 1)
                    trace_lay(outs, v + 2 * lw, 9, 2, lw, &edges);
                continue;
            }
            const int8_t *sh = trace_shape[shape];
            for (int i = 0; i < sh[0]; i++) {
                int j = i + 1 < sh[0] ? i + 1 : 0;
                int x = sh[1 + 2 * i], y = sh[2 + 2 * i];
                int dx = sh[1 + 2 * j] - x, dy = sh[2 + 2 * j] - y;
                int len = 1, d;
                if (dx == 0 || dy == 0) {
                    len = dx + dy < 0 ? -(dx + dy) : dx + dy;
                    d = trace_way(dx / len, dy / len);
                } else
                    d = trace_way(dx, dy);
                trace_lay(outs, v + (uint32_t)y * lw + (uint32_t)x, d, len, lw,
                          &edges);
            }
        }
    /* The points: two for each corner cut, and twice as many each pass.
     * A corner falls only where an edge does not carry on from one the
     * same way, or at a point with more than one way out -- each point
     * has as many edges in as out, so with one out its one in is the edge
     * behind it -- which bounds them far below the edges. Per point x, y,
     * a second x and y for cutting, the ends, sharp twice and the way
     * out. */
    uint32_t corners = 0;
    for (uint32_t v = 0; v < verts; v++) {
        uint32_t o = outs[v];
        if (!o)
            continue;
        int many = (o & (o - 1)) != 0;
        for (int d = 0; o >> d; d++) {
            if (!(o & (1u << d)))
                continue;
            int64_t p = (int64_t)v - way_y[d] * (int64_t)lw - way_x[d];
            if (many || p < 0 || p >= verts || !(outs[p] & (1u << d)))
                corners++;
        }
    }
    uint32_t cap = corners > TRACE_POINTS ? TRACE_POINTS
                                          : (corners * 2) << passes;
    if (cap > TRACE_POINTS)
        cap = TRACE_POINTS;
    if (cap < 4)
        cap = 4;
    uint32_t woff = gc_scratch(vm, cap * 23);
    if (!woff) {
        gc_scratch_end(vm, gbytes);
        return 0;
    }
    uint32_t held = ((gbytes + 7) & ~7u) + ((cap * 23 + 7) & ~7u);
    uint8_t *mem = MEM(vm, woff);
    int32_t *px = (int32_t *)(void *)mem, *py = px + cap;
    int32_t *tx = py + cap, *ty = tx + cap;
    int32_t *ends = ty + cap;
    uint8_t *ps = (uint8_t *)(ends + cap);
    uint8_t *tsh = ps + cap, *pd = tsh + cap;
    /* the lattice's origin in half cells of the world, and 16.16 a half
     * cell */
    int32_t ox = 2 * (c0 - ring), oy = 2 * (r0 - ring);
    const char *too_many = "segs.trace() finds more than 4096 points; "
                           "smooth less, or trace part of the map at a time";
    /* walked into outlines: at a point with more than one way out, the
     * one turning furthest right, so regions that touch at a corner stay
     * apart */
    uint32_t n = 0, nends = 0, cells = cw * ch;
    for (uint32_t v0 = trace_next(outs, 0, verts); v0 < verts;
         v0 = trace_next(outs, v0 + 1, verts)) {
        for (int d0 = 0; outs[v0] && d0 < TRACE_WAYS; d0++) {
            if (!(outs[v0] & (1u << d0)))
                continue;
            uint32_t start = n, v = v0;
            int32_t lx = (int32_t)(v0 % lw), ly = (int32_t)(v0 / lw);
            int d = d0, last = -1;
            for (;;) {
                outs[v] &= (uint16_t)~(1u << d);
                if (d != last) {
                    /* a corner: a point, sharp when a sharp solid cell
                     * touches it */
                    if (n >= cap)
                        return trace_fail(vm, held, too_many);
                    px[n] = (ox + lx) << 18;
                    py[n] = (oy + ly) << 18;
                    /* the ring's own outside stands for all beyond the
                     * area: taken far out */
                    if (ring) {
                        if (lx == 0)
                            px[n] = (c0 * 8 - TRACE_FAR) << 16;
                        if (lx == (int32_t)lw - 1)
                            px[n] = ((c0 + w) * 8 + TRACE_FAR) << 16;
                        if (ly == 0)
                            py[n] = (r0 * 8 - TRACE_FAR) << 16;
                        if (ly == (int32_t)lh - 1)
                            py[n] = ((r0 + h) * 8 + TRACE_FAR) << 16;
                    }
                    pd[n] = (uint8_t)d;
                    uint8_t touch = 0;
                    int32_t cx0 = (lx + 1) >> 1, cx1 = (lx >> 1) + 1;
                    int32_t cy0 = (ly + 1) >> 1, cy1 = (ly >> 1) + 1;
                    for (int32_t cy = cy0; cy <= cy1; cy++)
                        for (int32_t cx = cx0; cx <= cx1; cx++)
                            touch |= grid[(uint32_t)cy * gw + (uint32_t)cx];
                    ps[n++] = (touch & 16) != 0;
                    last = d;
                }
                v = (uint32_t)((int32_t)v + way_y[d] * (int32_t)lw +
                               way_x[d]);
                lx += way_x[d];
                ly += way_y[d];
                int nd = -1, best = -180;
                for (int t = 0; outs[v] >> t; t++) {
                    if (!(outs[v] & (1u << t)))
                        continue;
                    int rel = way_deg[t] - way_deg[d];
                    if (rel >= 180) rel -= 360;
                    if (rel < -180) rel += 360;
                    if (rel > best) {
                        best = rel;
                        nd = t;
                    }
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
            uint32_t m = n - start, k = 0;
            /* each corner between two square sides cut by half a
             * cell, but the sharp ones; a slope's ends kept */
            for (uint32_t i = 0; i < m; i++) {
                uint32_t a = start + (i + m - 1) % m, b = start + i;
                uint32_t c = start + (i + 1) % m;
                if (k + 2 > cap)
                    return trace_fail(vm, held, too_many);
                if (ps[b] || !WAY_SQUARE(pd[a]) || !WAY_SQUARE(pd[b])) {
                    tx[k] = px[b]; ty[k] = py[b]; tsh[k++] = ps[b];
                    continue;
                }
                /* half a cell, or half the side when shorter */
                int64_t ax = (int64_t)px[a] - px[b];
                int64_t ay = (int64_t)py[a] - py[b];
                int64_t cx2 = (int64_t)px[c] - px[b];
                int64_t cy2 = (int64_t)py[c] - py[b];
                int64_t la = (ax < 0 ? -ax : ax) + (ay < 0 ? -ay : ay);
                int64_t lc = (cx2 < 0 ? -cx2 : cx2) +
                             (cy2 < 0 ? -cy2 : cy2);
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
            if (start + k > cap)
                return trace_fail(vm, held, too_many);
            memcpy(px + start, tx, k * 4);
            memcpy(py + start, ty, k * 4);
            memcpy(ps + start, tsh, k);
            for (int32_t p = 0; p < passes; p++) {
                uint32_t got = trace_cut(px + start, py + start, ps + start,
                                         k, cap - start, tx, ty, tsh);
                if (!got)
                    return trace_fail(vm, held, too_many);
                k = got;
            }
            n = start + k;
            ends[nends++] = (int32_t)n;
        }
    }
    /* the record: plain arrays of the points and the ends */
    if (!vm_new_record(vm, trace_keys, T_FIELDS, out)) {
        gc_scratch_end(vm, held);
        return 0;
    }
    if (!vm_root(vm, *out)) {
        gc_scratch_end(vm, held);
        return 0;
    }
    struct value arr;
    uint32_t lens[3] = { n, n, nends };
    for (int f = 0; f < 3; f++) {
        if (!vm_new_array(vm, lens[f] ? lens[f] : 1, &arr)) {
            vm_unroot(vm);
            gc_scratch_end(vm, held);
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
    gc_scratch_end(vm, held);
    BUF(vm, OBJ(vm, *out)->buf)->v[T_N] = mk(T_INT, (int32_t)n);
    /* measured on the ARM rig (traceprobe): the grid, its lattice walked,
     * the edges, and the points given */
    vm_charge(vm, 30 + cells * 2 + verts * 3 / 2 + edges * 3 + n * 9);
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
 * set's own), each once: their indices into out, how many given; the
 * cells and filed lines passed over added to *seen */
static uint32_t seg_near(const struct segs *s, int64_t x0, int64_t y0,
                         int64_t x1, int64_t y1, int32_t *out, uint32_t room,
                         uint32_t *stamp, uint32_t mark, uint32_t *seen)
{
    /* in whole pixels from the grid's corner, and its cells by a 32-bit
     * divide once inside it: a 64-bit one costs a point more than all the
     * rest of its search */
    int64_t xa = (x0 >> 8) - s->gx, xb = (x1 >> 8) - s->gx;
    int64_t ya = (y0 >> 8) - s->gy, yb = (y1 >> 8) - s->gy;
    /* beyond the grid, its edge cells: segs.make files there the long
     * lines that do not size it */
    uint32_t cell = (uint32_t)s->cell;
    int64_t wide = (int64_t)s->gw * cell, high = (int64_t)s->gh * cell;
    int32_t a = xa <= 0 ? 0 : xa >= wide ? s->gw - 1
              : (int32_t)((uint32_t)xa / cell);
    int32_t b = xb <= 0 ? 0 : xb >= wide ? s->gw - 1
              : (int32_t)((uint32_t)xb / cell);
    int32_t c = ya <= 0 ? 0 : ya >= high ? s->gh - 1
              : (int32_t)((uint32_t)ya / cell);
    int32_t d = yb <= 0 ? 0 : yb >= high ? s->gh - 1
              : (int32_t)((uint32_t)yb / cell);
    uint32_t n = 0;
    *seen += (uint32_t)((d - c + 1) * (b - a + 1));
    for (int32_t r = c; r <= d; r++)
        for (int32_t q = a; q <= b; q++) {
            int32_t i = r * s->gw + q;
            /* held under 2^30, far past what stops a cart, so the sum
             * charged cannot wrap */
            *seen += (uint32_t)(s->start[i + 1] - s->start[i]);
            if (*seen > 1u << 30)
                *seen = 1u << 30;
            int32_t t0 = s->start[i], t1 = s->start[i + 1];
            if (t0 < 0 || (uint32_t)t1 > s->nitems)
                continue;
            for (int32_t t = t0; t < t1; t++) {
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

/* Line k's normal, unflipped: (-ey, ex) over its length, 16.16, 0 for a
 * line of no length */
static void seg_normal(const struct segs *s, int32_t k, int64_t *nx,
                       int64_t *ny)
{
    if (norm_mark[k] != s->gen) {
        int64_t ax = (int64_t)s->x0[k] >> 8, ay = (int64_t)s->y0[k] >> 8;
        int64_t ex = ((int64_t)s->x1[k] >> 8) - ax;
        int64_t ey = ((int64_t)s->y1[k] >> 8) - ay;
        int64_t el = (int64_t)root((uint64_t)(ex * ex + ey * ey));
        norm_x[k] = (int32_t)(el ? (-ey << 16) / el : 0);
        norm_y[k] = (int32_t)(el ? (ex << 16) / el : 0);
        norm_mark[k] = s->gen;
    }
    *nx = norm_x[k];
    *ny = norm_y[k];
}

/* Whether line k's unflipped normal is to be turned round to face out: by
 * the side known to be air, or else by the side o is on, from (o - a) . n */
static int seg_flip(const struct segs *s, int32_t k, int64_t from)
{
    if (s->side && s->side[k] != SIDE_NONE)
        return s->side[k] == SIDE_SOLID;
    return from < 0;
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
        seg_normal(s, k, &nx, &ny);
        if (seg_flip(s, k, (ox - ax) * nx + (oy - ay) * ny)) {
            nx = -nx;
            ny = -ny;
        }
        /* how far out on that side, which is below 0 when behind it; by a
         * side known to be air, a point more than r behind is past the
         * line, outside by another -- above a triangle, behind its base
         * -- or through it, which the path's crossing deals with */
        int64_t side = ((*px - ax) * nx + (*py - ay) * ny) >> 16;
        if (side >= r ||
            (side <= -r && s->side && s->side[k] != SIDE_NONE))
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
        int64_t d = (int64_t)root((uint64_t)dd);
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
    int64_t nx, ny;
    seg_normal(s, k, &nx, &ny);
    if (seg_flip(s, k, (ox - ax) * nx + (oy - ay) * ny)) {
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
    int64_t bx = (int64_t)s->x1[k] >> 8, by = (int64_t)s->y1[k] >> 8;
    /* boxes apart: no crossing, without the products */
    if ((ox > px ? ox : px) < (ax < bx ? ax : bx) ||
        (ox < px ? ox : px) > (ax > bx ? ax : bx) ||
        (oy > py ? oy : py) < (ay < by ? ay : by) ||
        (oy < py ? oy : py) > (ay > by ? ay : by))
        return -1;
    int64_t ex = bx - ax, ey = by - ay;
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

/* A point's lines, each marked when its box reaches within r of the box
 * round the point's path. While the point stays in that box (in), a line
 * left unmarked fails seg_push's and seg_cross's own box tests, so it is
 * skipped; once a move carries the point out, every line is tested */
struct nearby
{
    int32_t k[NEAR_SEGS];
    uint8_t keep[NEAR_SEGS];
    uint32_t n;
    int64_t x0, y0, x1, y1;         /* the box, 1/256 pixels */
    int in;
};

/* whether p is still in the box, the marks then holding */
static void near_check(struct nearby *nb, int64_t px, int64_t py)
{
    if (nb->in && (px < nb->x0 || px > nb->x1 || py < nb->y0 ||
                   py > nb->y1))
        nb->in = 0;
}

/* The box round the path o to p, m beyond it, and each line marked that
 * reaches within r of it: how many are marked */
static uint32_t near_mark(const struct segs *s, struct nearby *nb,
                          int64_t ox, int64_t oy, int64_t px, int64_t py,
                          int64_t m, int64_t r)
{
    nb->x0 = (px < ox ? px : ox) - m;
    nb->y0 = (py < oy ? py : oy) - m;
    nb->x1 = (px > ox ? px : ox) + m;
    nb->y1 = (py > oy ? py : oy) + m;
    nb->in = 1;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < nb->n; i++) {
        int32_t k = nb->k[i];
        int64_t ax = (int64_t)s->x0[k] >> 8, ay = (int64_t)s->y0[k] >> 8;
        int64_t bx = (int64_t)s->x1[k] >> 8, by = (int64_t)s->y1[k] >> 8;
        int keep = !(nb->x1 + r < (ax < bx ? ax : bx) ||
                     nb->x0 - r > (ax > bx ? ax : bx) ||
                     nb->y1 + r < (ay < by ? ay : by) ||
                     nb->y0 - r > (ay > by ? ay : by));
        nb->keep[i] = (uint8_t)keep;
        kept += (uint32_t)keep;
    }
    return kept;
}

/* The first of the lines that the path o to p crosses: its share of the
 * path (as seg_cross gives it) and in *k the line, or -1 and -1 when it
 * crosses none */
static int64_t seg_first(const struct segs *s, const struct nearby *nb,
                         int64_t ox, int64_t oy, int64_t px, int64_t py,
                         int32_t *k)
{
    int64_t first = -1;
    *k = -1;
    for (uint32_t i = 0; i < nb->n; i++) {
        if (nb->in && !nb->keep[i])
            continue;
        int32_t j = nb->k[i];
        int64_t c = seg_cross(s, j, ox, oy, px, py);
        if (c < 0 || (first >= 0 && c >= first))
            continue;
        /* a line whose air side is known is crossed only going into the
         * solid: a point coming out of it, put there by a push, goes */
        if (s->side && s->side[j] != SIDE_NONE) {
            int64_t nx, ny;
            seg_normal(s, j, &nx, &ny);
            int64_t out = (px - ox) * nx + (py - oy) * ny;
            if (s->side[j] == SIDE_AIR ? out > 0 : out < 0)
                continue;
        }
        first = c;
        *k = j;
    }
    return first;
}

/* One point against the set: p and o (16.16, world) changed in place, o
 * carrying its speed; its path and the side it is on are from start,
 * where it was as the tick began. Returns the material plus one it
 * touched, 0 none, with the way out. */
static int32_t seg_meet(const struct segs *s, int32_t *X, int32_t *Y,
                        int32_t *OX, int32_t *OY, int32_t SX, int32_t SY,
                        int32_t radius, const int32_t *gravity,
                        const int32_t *friction, uint32_t nfriction,
                        int32_t bounce, uint32_t *stamp, uint32_t *mark,
                        int32_t *nx, int32_t *ny, struct seg_counts *cnt)
{
    /* in the set's own place: p against where it is, the start against
     * where the set was before this tick's move */
    int64_t px = ((int64_t)*X - s->ox) >> 8, py = ((int64_t)*Y - s->oy) >> 8;
    int64_t ox = ((int64_t)SX - s->ox + s->mx) >> 8;
    int64_t oy = ((int64_t)SY - s->oy + s->my) >> 8;
    int64_t r = radius >> 8;
    if (r < 64)
        r = 64;                     /* a quarter pixel */
    /* where it came in, to its 16.16: the collision moves it in 1/256s,
     * and only that move is put back, so no fraction of its place or its
     * speed is lost to rounding at each touch */
    const int64_t px0 = px, py0 = py;
    int32_t gx = gravity[0], gy = gravity[1];
    struct nearby nb;
    /* round the path, as far as a push can carry it */
    nb.n = seg_near(s, (px < ox ? px : ox) - 3 * r,
                    (py < oy ? py : oy) - 3 * r,
                    (px > ox ? px : ox) + 3 * r,
                    (py > oy ? py : oy) + 3 * r,
                    nb.k, NEAR_SEGS, stamp, ++*mark, &cnt->seen);
    uint32_t n = nb.n, k_in = near_mark(s, &nb, ox, oy, px, py, 2 * r, r);
    cnt->tests += n;
    struct touch t = { -1, 0, 0 };
    /* the path across the first line it crosses: slid back out along
     * that line's normal to r on the side it came from, keeping all of
     * its move along the line -- stopping it at the crossing would lose
     * that, every tick, on a curve */
    int32_t fk;
    const int64_t pxa = px, pya = py;
    seg_first(s, &nb, ox, oy, px, py, &fk);
    if (fk >= 0) {
        seg_side(s, fk, ox, oy, &t);
        int64_t ax = (int64_t)s->x0[fk] >> 8, ay = (int64_t)s->y0[fk] >> 8;
        int64_t side = ((px - ax) * t.nx + (py - ay) * t.ny) >> 16;
        if (side < r) {
            px += (int64_t)t.nx * (r - side) >> 16;
            py += (int64_t)t.ny * (r - side) >> 16;
            near_check(&nb, px, py);
        }
    }
    /* then nothing within r, twice over for corners */
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < n; i++) {
            if (nb.in && !nb.keep[i])
                continue;
            if (seg_push(s, nb.k[i], &px, &py, ox, oy, r, &t))
                near_check(&nb, px, py);
        }
    /* last, the path again: a push out of one side of a V can cross the
     * other, and a point is never let through a line -- back along its
     * path to just short of the first it crosses */
    for (int again = fk < 0 && px == pxa && py == pya ? 3 : 0; again < 3;
         again++) {
        int32_t ck;
        int64_t c = seg_first(s, &nb, ox, oy, px, py, &ck);
        if (ck < 0)
            break;
        c = c > 4096 ? c - 4096 : 0;
        px = ox + ((px - ox) * c >> 16);
        py = oy + ((py - oy) * c >> 16);
        near_check(&nb, px, py);
        if (t.k < 0)
            seg_side(s, ck, ox, oy, &t);
    }
    /* the lines tested in full: those marked, or all once it left */
    cnt->kept += nb.in ? k_in : n;
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
        int64_t tl = (int64_t)root((uint64_t)(tx * tx + ty * ty));
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
            int64_t speed = e > 0 ? (int64_t)root((uint64_t)e) : 0;
            if (speed > 0) {
                tx = tx * speed / tl;
                ty = ty * speed / tl;
                vn = 0;
            }
            cnt->slid++;
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
 * the lines as it goes. Returns the area it held.
 * A ring crushed under a tenth of its area -- heaped in a corner, or wound
 * round itself, where the way it grows fastest leads nowhere -- is sped
 * instead toward a circle of the area, each point to its place in the
 * ring's order, k / n of a turn, at most two pixels a tick: the order
 * unwinds it. The circle stands off the ring's middle against the pool's
 * gravity, so a ring crushed into a corner by its weight opens into the
 * air above it rather than into the lines. */
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
    if (!stiff)
        return 1;
    /* crushed: the circle's radius and middle, 16.16, the radius from want
     * / pi square pixels, pi as 205887 / 65536 */
    int crushed = want > 0 && have < want / 10;
    int64_t r = 0, cx = 0, cy = 0;
    if (crushed) {
        int64_t w = want > (int64_t)1 << 40 ? (int64_t)1 << 40 : want;
        r = root((uint64_t)w * 65536 / 205887 * 65536);
        for (int32_t k = 0; k < n; k++) {
            cx += fx(x[first + k]);
            cy += fx(y[first + k]);
        }
        cx /= n;
        cy /= n;
        /* a quarter radius against gravity, gx across and gravity down:
         * a whole radius sends the ring hopping out of the corner */
        int64_t gvx = fx(l.f[L_GX]), gvy = fx(l.f[L_GRAVITY]);
        int64_t glen = root((uint64_t)(gvx * gvx + gvy * gvy));
        if (glen) {
            cx -= r * gvx / glen / 4;
            cy -= r * gvy / glen / 4;
        }
        vm_charge(vm, (uint32_t)n * 12);
    } else if (!grow)
        return 1;
    /* the share of each point's way to move, 16.16, at most half a pixel
     * a tick for each pixel of its way */
    /* each product held inside 64 bits, its sign kept: a ring crushed to
     * under a pixel has a tiny grow, and a huge area or stiffness after it
     * would otherwise wrap */
    int64_t lambda = 0;
    if (grow) {
        int64_t gap = want - have;
        if (gap > (int64_t)1 << 46) gap = (int64_t)1 << 46;
        if (gap < -((int64_t)1 << 46)) gap = -((int64_t)1 << 46);
        lambda = (gap << 16) / grow;
        if (lambda > INT32_MAX) lambda = INT32_MAX;
        if (lambda < -INT32_MAX) lambda = -INT32_MAX;
        lambda = lambda * stiff >> 16;
        if (lambda > 32768) lambda = 32768;
        if (lambda < -32768) lambda = -32768;
    }
    for (int32_t k = 0; k < n; k++) {
        uint32_t i = (uint32_t)(first + k);
        if (vm_truthy(vm, pin[i]))
            continue;
        uint32_t ip = (uint32_t)(first + (k + n - 1) % n);
        uint32_t in = (uint32_t)(first + (k + 1) % n);
        int64_t gx = ((int64_t)(fx(y[in]) - fx(y[ip])) >> 8) / 2 * sign;
        int64_t gy = ((int64_t)(fx(x[ip]) - fx(x[in])) >> 8) / 2 * sign;
        int64_t dx, dy;
        if (crushed) {
            uint32_t t = (uint32_t)((int64_t)k * 65536 / n);
            dx = cx + (r * core_sine(t + 0x4000) >> 16) - fx(x[i]);
            dy = cy + (r * core_sine(t) >> 16) - fx(y[i]);
            dx = (dx * stiff >> 16) / 4;
            dy = (dy * stiff >> 16) / 4;
            if (dx > 131072) dx = 131072;
            if (dx < -131072) dx = -131072;
            if (dy > 131072) dy = 131072;
            if (dy < -131072) dy = -131072;
        } else {
            dx = (gx * lambda) >> 8;
            dy = (gy * lambda) >> 8;
        }
        ox[i] = mk(T_FIX, wrap32(fx(ox[i]) - dx));
        oy[i] = mk(T_FIX, wrap32(fx(oy[i]) - dy));
    }
    return 1;
}

/* A link as the passes work it: ij its ends' places in the points' array
 * in bytes, i in the low half and j in the high, each with its low bit set
 * when that end is pinned (QLINK_NONE for a broken link, or one with both
 * ends pinned); l2 its length squared in 1/256 pixels, and inv 2^47 over
 * that, 0 for a link of a pixel or less, or 128 or more, which goes the
 * exact way. Its layout is links_arm.S's. */
struct qlink
{
    uint32_t ij;
    int32_t l2, inv;
};
#define QLINK_NONE 0xffffffffu

_Static_assert(sizeof(struct qlink) == 12 && MAX_POINTS * 12 <= 65536,
               "links_arm.S's link, and a point's place in 16 bits");

/* link_run()'s ways: a slack pool's squashed links are let be; narrow,
 * only a link stretched within a quarter goes the short way */
#define RUN_SLACK  1
#define RUN_NARROW 2

#ifdef ARCADE_ASM
const struct qlink *link_run_arm(int32_t *P, const struct qlink *q,
                                 const struct qlink *end, int ways);
#endif

/* 1 - (1 + e)^-1/2, 16.16, for e from -1/2 by sixteenths to 2: the share
 * of a link's vector its ends move past a quarter's stretch, between two
 * entries by the straight line, within 0.2 per cent. links_arm.S has the
 * same. */
#ifndef ARCADE_ASM
static const int32_t link_share[41] = {
    -27146, -21845, -17361, -13503, -10138, -7170, -4525, -2149, 0, 1957,
    3748, 5396, 6919, 8331, 9647, 10875, 12026, 13107, 14125, 15086,
    15995, 16857, 17675, 18454, 19195, 19903, 20579, 21226, 21845, 22440,
    23011, 23559, 24087, 24596, 25086, 25559, 26016, 26458, 26885, 27298,
    27699,
};
#endif

/* One pass of links q up to end the short way, until a link that needs
 * the exact way, which is returned (end when there is none). With e the
 * stretch of a link's square, (d^2 - L^2) / L^2, the share of the vector
 * its ends move, (d - L) / d, is 1 - (1 + e)^-1/2: to the third order
 * e/2 - 3e^2/8 + 5e^3/16, within 1 per cent while e is within a quarter,
 * and link_share beyond it to a stretch of 2 (a link 1.73 times its
 * length) and a squash of a half. No root, no divide, all in 32 bits, the
 * differences wrapped as the positions are. The exact way takes a link
 * too long or short for inv, more than 64 pixels across, or stretched or
 * squashed past those -- past a quarter when RUN_NARROW. */
static const struct qlink *link_run(int32_t *P, const struct qlink *q,
                                    const struct qlink *end, int ways)
{
#ifdef ARCADE_ASM
    return link_run_arm(P, q, end, ways);
#else
    int slack = ways & RUN_SLACK;
    for (; q < end; q++) {
        uint32_t ij = q->ij;
        if (ij == QLINK_NONE)
            continue;
        if (!q->inv)
            return q;
        int32_t *Pi = (int32_t *)((char *)P + (ij & 0xfffc));
        int32_t *Pj = (int32_t *)((char *)P + ((ij >> 16) & 0xfffc));
        int32_t ax = (int32_t)((uint32_t)Pj[0] - (uint32_t)Pi[0]) >> 8;
        int32_t ay = (int32_t)((uint32_t)Pj[1] - (uint32_t)Pi[1]) >> 8;
        int32_t az = (int32_t)((uint32_t)Pj[2] - (uint32_t)Pi[2]) >> 8;
        if ((uint32_t)(ax + 16384) >= 32768 ||
            (uint32_t)(ay + 16384) >= 32768 ||
            (uint32_t)(az + 16384) >= 32768)
            return q;
        int32_t d2 = ax * ax + ay * ay + az * az;
        int32_t e = (int32_t)((int64_t)(d2 - q->l2) * q->inv >> 31);
        int32_t sh;
        if ((uint32_t)(e + 16384) < 32768) {
            if (slack && e < 0)
                continue;
            int32_t e2 = (e * e) >> 16, e3 = (e2 * e) >> 16;
            sh = e / 2 - (3 * e2 >> 3) + (5 * e3 >> 4);
        } else {
            uint32_t v = (uint32_t)(e + 32768);
            if ((ways & RUN_NARROW) || v >= 163840)
                return q;
            if (slack && e < 0)
                continue;
            const int32_t *t = &link_share[v >> 12];
            sh = t[0] + ((t[1] - t[0]) * (int32_t)(v & 4095) >> 12);
        }
        /* the move in 16.16 is (a * sh) >> 8, all of it to the free end
         * when the other is pinned, else >> 9 to each */
        ax *= sh;
        ay *= sh;
        az *= sh;
        if (ij & 1) {
            Pj[0] = (int32_t)((uint32_t)Pj[0] - (uint32_t)(ax >> 8));
            Pj[1] = (int32_t)((uint32_t)Pj[1] - (uint32_t)(ay >> 8));
            Pj[2] = (int32_t)((uint32_t)Pj[2] - (uint32_t)(az >> 8));
        } else if (ij & 0x10000) {
            Pi[0] = (int32_t)((uint32_t)Pi[0] + (uint32_t)(ax >> 8));
            Pi[1] = (int32_t)((uint32_t)Pi[1] + (uint32_t)(ay >> 8));
            Pi[2] = (int32_t)((uint32_t)Pi[2] + (uint32_t)(az >> 8));
        } else {
            Pi[0] = (int32_t)((uint32_t)Pi[0] + (uint32_t)(ax >> 9));
            Pi[1] = (int32_t)((uint32_t)Pi[1] + (uint32_t)(ay >> 9));
            Pi[2] = (int32_t)((uint32_t)Pi[2] + (uint32_t)(az >> 9));
            Pj[0] = (int32_t)((uint32_t)Pj[0] - (uint32_t)(ax >> 9));
            Pj[1] = (int32_t)((uint32_t)Pj[1] - (uint32_t)(ay >> 9));
            Pj[2] = (int32_t)((uint32_t)Pj[2] - (uint32_t)(az >> 9));
        }
    }
    return end;
#endif
}

/* links.step(p, solid, [out], [which]): a tick of every point and link,
 * then points out of the segs' lines, out of map cells whose flags share
 * a bit with solid (0, none), and out of the pool's field's solid cells.
 * out takes each point's material plus one and the way out; which, the
 * index of the set it met in segs, or -1. */
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
    uint32_t broken = 0, exact = 0;
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
     * a pass: P the points (x, y, z), pinned, and Q the links */
    static int32_t P[MAX_POINTS * 3];
    static struct qlink Q[MAX_LINKS];
    static uint8_t pinned[MAX_POINTS];
    for (uint32_t i = 0; i < np; i++) {
        P[i * 3] = fx(x[i]);
        P[i * 3 + 1] = fx(y[i]);
        P[i * 3 + 2] = fx(z[i]);
        pinned[i] = (uint8_t)vm_truthy(vm, pin[i]);
    }
    /* inv for the last few lengths, as most of a pool's links share a few */
    uint32_t seen_l8[8] = { 0 }, seen_inv[8];
    for (uint32_t k = 0; k < nl; k++) {
        int32_t i = whole(la[k], -1), j = whole(lb[k], -1);
        struct qlink *q = &Q[k];
        q->ij = QLINK_NONE;
        if (i < 0 || j < 0 || (uint32_t)i >= np || (uint32_t)j >= np ||
            (pinned[i] && pinned[j]))
            continue;
        q->ij = ((uint32_t)i * 12 | pinned[i]) |
                ((uint32_t)j * 12 | pinned[j]) << 16;
        int32_t L = fx(len[k]);
        uint32_t l8 = (uint32_t)L >> 8;
        q->l2 = (int32_t)(l8 * l8);
        q->inv = 0;
        if (L > 0 && l8 > 256 && l8 < 32768) {
            uint32_t h = l8 & 7;
            if (seen_l8[h] != l8) {
                uint64_t r = (1u << 31) / l8;
                seen_l8[h] = l8;
                seen_inv[h] = (uint32_t)(r * r >> 15);
            }
            q->inv = (int32_t)seen_inv[h];
        }
    }
    const struct qlink *qend = Q + nl;
    for (int32_t p = 0; p < passes; p++) {
        /* the first pass measures each link for its tear, the exact way:
         * taking only links stretched under 12 per cent the short way, so
         * with a tear of an eighth or more what it takes cannot tear;
         * under that, the first pass goes the exact way */
        int quick = !(p == 0 && tear > 0 && tear < 8192);
        int ways = (slack ? RUN_SLACK : 0) |
                   (p == 0 && tear > 0 ? RUN_NARROW : 0);
        for (uint32_t k = 0; k < nl; k++) {
            if (quick) {
                const struct qlink *q = link_run(P, Q + k, qend, ways);
                if (q == qend)
                    break;
                k = (uint32_t)(q - Q);
            }
            uint32_t ij = Q[k].ij;
            if (ij == QLINK_NONE)
                continue;
            exact++;
            int pi = ij & 1, pj = (ij >> 16) & 1;
            int32_t *Pi = (int32_t *)((char *)P + (ij & 0xfffc));
            int32_t *Pj = (int32_t *)((char *)P + ((ij >> 16) & 0xfffc));
            int32_t xi = Pi[0], yi = Pi[1], zi = Pi[2];
            int32_t xj = Pj[0], yj = Pj[1], zj = Pj[2];
            int32_t L = fx(len[k]);
            int64_t dx = (int64_t)xj - xi, dy = (int64_t)yj - yi;
            int64_t dz = (int64_t)zj - zi;
            /* A link under 64 pixels -- a cloth's or a rope's -- is worked
             * in 1/256ths of a pixel, where its square fits 32 bits and its
             * root and divide are 32-bit; a longer one exactly, in 64 */
            const int64_t NEAR = (int64_t)1 << 22;
            int short_link = dx > -NEAR && dx < NEAR && dy > -NEAR &&
                             dy < NEAR && dz > -NEAR && dz < NEAR;
            int32_t d, d8 = 0;
            int64_t s;
            if (short_link) {
                int32_t ax = (int32_t)dx >> 8, ay = (int32_t)dy >> 8;
                int32_t az = (int32_t)dz >> 8;
                d8 = (int32_t)root32((uint32_t)(ax * ax) + (uint32_t)(ay * ay) +
                                     (uint32_t)(az * az));
                d = d8 << 8;
            } else
                d = span(dx, dy, dz);
            /* torn on the first pass, at the stretch the tick left it */
            if (p == 0 && tear > 0 && d > L + (((int64_t)L * tear) >> 16)) {
                la[k] = mk(T_INT, -1);
                Q[k].ij = QLINK_NONE;
                broken++;
                continue;
            }
            if (d < 16)
                continue;
            /* the share of the gap each end moves, 16.16 of the vector;
             * a slack link only pulls */
            int64_t gap = (int64_t)d - L;
            if (slack && gap < 0)
                continue;
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
    }
    /* back into the pool; a pinned point never moves, so is left as it is */
    for (uint32_t i = 0; i < np; i++)
        if (!pinned[i]) {
            x[i] = mk(T_FIX, P[i * 3]);
            y[i] = mk(T_FIX, P[i * 3 + 1]);
            z[i] = mk(T_FIX, P[i * 3 + 2]);
        }
    /* out of the segment sets' lines, and what each point touched */
    struct seg_counts sc = { 0, 0, 0, 0 };
    uint32_t touched = 0, segpts = 0, skipped = 0;
    static struct segs sets[MAX_SETS];
    uint32_t nsets = 0;
    struct value sv = l.f[L_SEGS];
    if (sv.tag == T_OBJ) {
        if (!segs_of(vm, sv, "links.step", &sets[0]))
            return 0;
        nsets = 1;
    } else if (sv.tag == T_ARR) {
        struct column sl;
        column_of(vm, sv, &sl);
        if (sl.len > MAX_SETS)
            return vm_fail(vm, "links.step() meets at most 16 sets", "", "");
        for (uint32_t s = 0; s < sl.len; s++)
            if (!segs_of(vm, ((const struct value *)sl.p)[s], "links.step",
                         &sets[nsets++]))
                return 0;
    }
    if (nsets) {
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
        /* what each touched, three numbers a point, and which set, when
         * asked */
        struct value o = argc > 2 ? a[2] : mk(T_NULL, 0);
        struct value w = argc > 3 ? a[3] : mk(T_NULL, 0);
        int give = o.tag != T_NULL, which = w.tag != T_NULL;
        if (give && !vm_out_ok(vm, o, np * 3))
            return vm_fail(vm, "links.step() writes three numbers a point "
                           "to an array or Int32Array", "", "");
        if (which && !vm_out_ok(vm, w, np))
            return vm_fail(vm, "links.step() writes the set each point met "
                           "to an array or Int32Array of one a point", "", "");
        /* how far a point's meeting looks past its path: seg_meet's 3r */
        int64_t reach = (((int64_t)(radius > 0 ? radius : 0) * 3) >> 16) + 2;
        for (uint32_t i = 0; i < np; i++) {
            int32_t m = 0, NX = 0, NY = 0, met = -1;
            if (!pinned[i]) {
                int32_t X = fx(x[i]), Y = fx(y[i]), OX = fx(ox[i]);
                int32_t OY = fx(oy[i]);
                const int32_t OX0 = OX, OY0 = OY;
                for (uint32_t s = 0; s < nsets; s++) {
                    const struct segs *sg = &sets[s];
                    /* the path's box in the set's place, wide enough for
                     * where it was before this tick's move: from the
                     * tick's start, which seg_meet judges from, as well as
                     * from the speed an earlier set left it */
                    int64_t sx = sg->ox >> 16, sy = sg->oy >> 16;
                    int64_t mx = sg->mx >> 16, my = sg->my >> 16;
                    int32_t lx = OX < OX0 ? OX : OX0, hx = OX > OX0 ? OX : OX0;
                    int32_t ly = OY < OY0 ? OY : OY0, hy = OY > OY0 ? OY : OY0;
                    int64_t ax = (X < lx ? X : lx) >> 16;
                    int64_t bx = (X > hx ? X : hx) >> 16;
                    int64_t ay = (Y < ly ? Y : ly) >> 16;
                    int64_t by = (Y > hy ? Y : hy) >> 16;
                    int64_t mw = reach + (mx < 0 ? -mx : mx) + 1;
                    int64_t mh = reach + (my < 0 ? -my : my) + 1;
                    if (bx + mw < sx + sg->bx0 || ax - mw > sx + sg->bx1 ||
                        by + mh < sy + sg->by0 || ay - mh > sy + sg->by1) {
                        skipped++;
                        continue;
                    }
                    segpts++;
                    /* each set judges the side a point is on from where it
                     * was as the tick began, not from where a set met
                     * before it put its speed -- a bounce off the ground
                     * puts that below the point, and the lift above it
                     * would take it for coming up from inside -- but takes
                     * its speed as that set left it, or a push out of one
                     * set would become speed off the next */
                    int32_t nx, ny;
                    int32_t k = seg_meet(sg, &X, &Y, &OX, &OY, OX0, OY0,
                                         radius, grav, fr, nfr, bounce,
                                         stamp, &mark, &nx, &ny, &sc);
                    if (k) {
                        m = k;
                        NX = nx;
                        NY = ny;
                        met = (int32_t)s;
                    }
                }
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
            if (which)
                vm_out_set(vm, w, i, mk(T_INT, met));
        }
    } else {
        /* no set to meet: what each touched is nothing, still written */
        struct value o = argc > 2 ? a[2] : mk(T_NULL, 0);
        struct value w = argc > 3 ? a[3] : mk(T_NULL, 0);
        if (o.tag != T_NULL) {
            if (!vm_out_ok(vm, o, np * 3))
                return vm_fail(vm, "links.step() writes three numbers a "
                               "point to an array or Int32Array", "", "");
            for (uint32_t i = 0; i < np * 3; i++)
                vm_out_set(vm, o, i, mk(T_INT, 0));
        }
        if (w.tag != T_NULL) {
            if (!vm_out_ok(vm, w, np))
                return vm_fail(vm, "links.step() writes the set each point "
                               "met to an array or Int32Array of one a "
                               "point", "", "");
            for (uint32_t i = 0; i < np; i++)
                vm_out_set(vm, w, i, mk(T_INT, -1));
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
    /* measured on the ARM rig (.build/arcade-rig, linkprobe, bagprobe
     * and linkbase): a link a pass 3.75 units the short way, 15 more the
     * exact way, its root and divide; a point 4 more damped or dragged */
    vm_charge(vm, 10 + np * 18 + nl * 3 + nl * (uint32_t)passes * 15 / 4 +
                  exact * 15 + (damp || drag ? np * 4 : 0) +
                  (mask ? np * 4 : 0) + (field ? np * 7 : 0) +
                  segpts * SEG_POINT + skipped * SEG_SKIP +
                  sc.tests * SEG_TEST +
                  sc.kept * SEG_KEPT + touched * SEG_TOUCH +
                  sc.slid * SEG_SLIDE + sc.seen / SEG_SEEN);
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
