/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade systems: paths -- getting about a board of cells, the map's own
 * bytes or a grid's, in cells and four directions. A wall is a cell whose
 * sprite flags share a bit with solid, or, when solid is 0, any non-zero
 * cell. Outside the board is a wall.
 *
 * A distance field serves many chasers heading for one place; find is
 * one shortest path, A* by Manhattan distance; sight is whether a line
 * between two cells' centres crosses only open cells. A ray is in world
 * pixels against the active map: where a line from a point first meets a
 * solid cell, and which face, for shots, lasers and bounces.
 *
 * Parts, in order:
 *   walls      what blocks
 *   fields     field, next
 *   paths      find
 *   sight      sight
 *   rays       ray
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)

/* ---- walls ------------------------------------------------------------- */

struct walls
{
    struct board b;
    const uint8_t *flags;
    uint8_t mask;
};

/* The board and solid from a[0..2] */
static int walls_of(struct vm *vm, struct value *a, const char *fn,
                    struct walls *w)
{
    if (!board_of(vm, a[0], a[1], fn, &w->b))
        return 0;
    if (!is_num(a[2]))
        return vm_fail(vm, fn, "() needs the solid flags, or 0 for any "
                       "non-zero cell", "");
    w->mask = (uint8_t)floor_int(a[2]);
    w->flags = MEM(vm, vm->flags);
    return 1;
}

/* always inlined: the searches call it four times a cell, and -Os would
 * make each a call */
static inline __attribute__((always_inline))
int wall(const struct walls *w, int32_t x, int32_t y)
{
    if (x < 0 || y < 0 || x >= w->b.w || y >= w->b.h)
        return 1;
    uint8_t v = w->b.c[y * w->b.w + x];
    return w->mask ? (w->flags[v] & w->mask) != 0 : v != 0;
}

static int32_t whole(struct value v, int32_t dflt)
{
    return is_num(v) ? floor_int(v) : dflt;
}

/* the four steps, in DIR's order: left, right, up, down */
static const int8_t step_x[4] = { -1, 1, 0, 0 };
static const int8_t step_y[4] = { 0, 0, -1, 1 };

/* ---- fields -------------------------------------------------------------- */

/* path.field(b, w, solid, x, y, out): every open cell's distance in steps
 * from cell x, y into out, an Int32Array as long as the board, -1 where
 * it cannot be reached; returns the cells reached */
NATIVE(path_field)
{
    (void)argc;
    struct walls w;
    if (!walls_of(vm, a, "path.field", &w))
        return 0;
    uint32_t cells = (uint32_t)(w.b.w * w.b.h);
    if (a[5].tag != T_I32 || I32(vm, a[5])->len < cells)
        return vm_fail(vm, "path.field() writes to an Int32Array as long as "
                       "the board", "", "");
    if (w.b.w > 65535 || w.b.h > 65535)
        return vm_fail(vm, "path.field() works on boards of up to 65535 "
                       "across and down", "", "");
    int32_t x = whole(a[3], -1), y = whole(a[4], -1);
    /* the queue holds each cell as its row and column, so none is divided
     * to find them: y << 8 | x in two bytes on a board up to 256 across
     * and down, else y << 16 | x in four. It is working memory from the
     * heap's top, given back, so it leaves no hole. */
    int small = w.b.w <= 256 && w.b.h <= 256;
    uint32_t shift = small ? 8 : 16, low = (1u << shift) - 1;
    uint32_t bytes = cells * (small ? 2 : 4);
    uint32_t qoff = gc_scratch(vm, bytes);
    if (!qoff)
        return 0;
    uint16_t *q16 = MEM(vm, qoff);
    uint32_t *q32 = MEM(vm, qoff);
    int32_t *d = I32(vm, a[5])->d;
    for (uint32_t i = 0; i < cells; i++)
        d[i] = -1;
    uint32_t head = 0, tail = 0;
    if (!wall(&w, x, y)) {
        d[y * w.b.w + x] = 0;
        uint32_t c = (uint32_t)y << shift | (uint32_t)x;
        if (small)
            q16[tail++] = (uint16_t)c;
        else
            q32[tail++] = c;
    }
    while (head < tail) {
        uint32_t q = small ? q16[head] : q32[head];
        head++;
        int32_t cx = (int32_t)(q & low), cy = (int32_t)(q >> shift);
        int32_t i = cy * w.b.w + cx;
        for (int k = 0; k < 4; k++) {
            int32_t nx = cx + step_x[k], ny = cy + step_y[k];
            int32_t j = ny * w.b.w + nx;
            if (wall(&w, nx, ny) || d[j] >= 0)
                continue;
            d[j] = d[i] + 1;
            uint32_t c = (uint32_t)ny << shift | (uint32_t)nx;
            if (small)
                q16[tail++] = (uint16_t)c;
            else
                q32[tail++] = c;
        }
    }
    gc_scratch_end(vm, bytes);
    /* measured on the ARM rig (.build/arcade-rig, sysprobe3) */
    vm_charge(vm, 10 + cells + tail * 13 / 2);
    *out = mk(T_INT, (int32_t)tail);
    return 1;
}

/* path.next(field, w, x, y): the way from x, y one step nearer the
 * field's start (DIR.LEFT, RIGHT, UP or DOWN, the first in that order
 * when several are), or -1 at the start or where it cannot be reached */
NATIVE(path_next)
{
    (void)argc;
    int32_t w = whole(a[1], 0), x = whole(a[2], -1), y = whole(a[3], -1);
    if (a[0].tag != T_I32 || w < 1 || I32(vm, a[0])->len % (uint32_t)w)
        return vm_fail(vm, "path.next() needs a field from path.field() and "
                       "its width", "", "");
    const int32_t *d = I32(vm, a[0])->d;
    int32_t h = (int32_t)(I32(vm, a[0])->len / (uint32_t)w);
    *out = mk(T_INT, -1);
    vm_charge(vm, 7);                  /* measured on the rig, sysprobe3 */
    if (x < 0 || y < 0 || x >= w || y >= h || d[y * w + x] <= 0)
        return 1;
    int32_t here = d[y * w + x];
    for (int k = 0; k < 4; k++) {
        int32_t nx = x + step_x[k], ny = y + step_y[k];
        if (nx >= 0 && ny >= 0 && nx < w && ny < h &&
            d[ny * w + nx] == here - 1) {
            *out = mk(T_INT, k);
            break;
        }
    }
    return 1;
}

/* ---- paths --------------------------------------------------------------- */

/* A* keeps per cell: the steps from the start, the distance still to go
 * (worked out once, as the cell is first reached: a division each way),
 * the way it was reached, whether it is open or closed, and where it sits
 * in the heap, an array of cells ordered by steps plus distance to go. */
struct astar
{
    uint16_t *g, *h, *heap, *pos;
    uint8_t *came, *state;
    uint32_t n;
    int32_t w, gx, gy;
};

enum { UNSEEN, OPEN, CLOSED };

static int32_t to_go(const struct astar *s, uint32_t i)
{
    int32_t x = (int32_t)i % s->w, y = (int32_t)i / s->w;
    int32_t dx = x - s->gx, dy = y - s->gy;
    return (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
}

/* Whether cell i comes before cell j: the lower estimate, and on a tie
 * the one nearer the goal */
static int before(const struct astar *s, uint32_t i, uint32_t j)
{
    int32_t hi = s->h[i], hj = s->h[j];
    int32_t fi = s->g[i] + hi, fj = s->g[j] + hj;
    return fi < fj || (fi == fj && hi < hj);
}

static void put(struct astar *s, uint32_t k, uint16_t cell)
{
    s->heap[k] = cell;
    s->pos[cell] = (uint16_t)k;
}

static void up(struct astar *s, uint32_t k)
{
    uint16_t c = s->heap[k];
    while (k > 0 && before(s, c, s->heap[(k - 1) / 2])) {
        put(s, k, s->heap[(k - 1) / 2]);
        k = (k - 1) / 2;
    }
    put(s, k, c);
}

static uint16_t pop(struct astar *s)
{
    uint16_t top = s->heap[0], c = s->heap[--s->n];
    uint32_t k = 0;
    for (;;) {
        uint32_t l = 2 * k + 1, best = k;
        uint16_t bc = c;
        if (l < s->n && before(s, s->heap[l], bc))
            best = l, bc = s->heap[l];
        if (l + 1 < s->n && before(s, s->heap[l + 1], bc))
            best = l + 1, bc = s->heap[l + 1];
        if (best == k)
            break;
        put(s, k, bc);
        k = best;
    }
    if (s->n)
        put(s, k, c);
    return top;
}

/* path.find(b, w, solid, x0, y0, x1, y1, out): a shortest way from cell
 * x0, y0 to x1, y1, its cells after the first into out as far as it
 * reaches, start to goal; returns its length in steps, or -1 when there
 * is none */
NATIVE(path_find)
{
    (void)argc;
    struct walls w;
    if (!walls_of(vm, a, "path.find", &w))
        return 0;
    if (a[7].tag != T_I32)
        return vm_fail(vm, "path.find() writes to an Int32Array", "", "");
    uint32_t cells = (uint32_t)(w.b.w * w.b.h);
    if (cells > 65535)
        return vm_fail(vm, "path.find() works on boards of up to 65535 cells",
                       "", "");
    int32_t x0 = whole(a[3], -1), y0 = whole(a[4], -1);
    int32_t x1 = whole(a[5], -1), y1 = whole(a[6], -1);
    *out = mk(T_INT, -1);
    if (wall(&w, x0, y0) || wall(&w, x1, y1)) {
        vm_charge(vm, 10);
        return 1;
    }
    uint8_t *mem = scratch(vm, cells * 10);
    if (!mem)
        return 0;
    struct astar s;
    s.g = (uint16_t *)mem;
    s.h = s.g + cells;
    s.heap = s.h + cells;
    s.pos = s.heap + cells;
    s.came = (uint8_t *)(s.pos + cells);
    s.state = s.came + cells;
    s.n = 0;
    s.w = w.b.w;
    s.gx = x1;
    s.gy = y1;
    uint32_t start = (uint32_t)(y0 * w.b.w + x0);
    uint32_t goal = (uint32_t)(y1 * w.b.w + x1);
    s.state[start] = OPEN;
    s.h[start] = (uint16_t)to_go(&s, start);
    put(&s, s.n++, (uint16_t)start);
    uint32_t expanded = 0;
    int found = 0;
    while (s.n) {
        uint32_t i = pop(&s);
        s.state[i] = CLOSED;
        expanded++;
        if (i == goal) {
            found = 1;
            break;
        }
        int32_t cx = (int32_t)i % w.b.w, cy = (int32_t)i / w.b.w;
        for (int k = 0; k < 4; k++) {
            int32_t nx = cx + step_x[k], ny = cy + step_y[k];
            if (wall(&w, nx, ny))
                continue;
            uint32_t j = (uint32_t)(ny * w.b.w + nx);
            uint16_t g = (uint16_t)(s.g[i] + 1);
            if (s.state[j] == CLOSED || (s.state[j] == OPEN && s.g[j] <= g))
                continue;
            s.g[j] = g;
            s.came[j] = (uint8_t)k;
            if (s.state[j] == UNSEEN) {
                s.state[j] = OPEN;
                s.h[j] = (uint16_t)to_go(&s, j);
                put(&s, s.n++, (uint16_t)j);
            }
            up(&s, s.pos[j]);
        }
    }
    /* measured on the rig (sysprobe3): about 34 units a cell expanded,
     * the queue's ordering most of it */
    vm_charge(vm, 10 + cells / 2 + expanded * 34);
    if (!found)
        return 1;
    /* back from the goal by the ways each cell was reached, into the heap's
     * room, then out start first */
    uint32_t len = s.g[goal], k = len;
    uint16_t *route = s.heap;
    for (uint32_t i = goal; i != start;) {
        route[--k] = (uint16_t)i;
        int d = s.came[i];
        i = (uint32_t)((int32_t)i - step_x[d] - step_y[d] * w.b.w);
    }
    int32_t *o = I32(vm, a[7])->d;
    uint32_t room = I32(vm, a[7])->len;
    for (uint32_t i = 0; i < len && i < room; i++)
        o[i] = route[i];
    *out = mk(T_INT, (int32_t)len);
    return 1;
}

/* ---- sight --------------------------------------------------------------- */

/* Whether the line between the centres of cells x, y and x1, y1 crosses
 * only open cells, the two ends aside. Through a corner exactly, both
 * cells beside it must be open: no seeing through a diagonal gap. */
static int line_clear(const struct walls *w, int32_t x, int32_t y,
                      int32_t x1, int32_t y1)
{
    int32_t nx = x1 - x, ny = y1 - y;
    int32_t sx = nx < 0 ? -1 : 1, sy = ny < 0 ? -1 : 1;
    nx = nx < 0 ? -nx : nx;
    ny = ny < 0 ? -ny : ny;
    int clear = 1;
    for (int32_t i = 0, j = 0; clear && (i < nx || j < ny);) {
        /* which edge the line meets first: the next column's or row's */
        int64_t cmp = (int64_t)(1 + 2 * i) * ny - (int64_t)(1 + 2 * j) * nx;
        if (cmp == 0) {
            if (wall(w, x + sx, y) || wall(w, x, y + sy))
                clear = 0;
            x += sx; y += sy; i++; j++;
        } else if (cmp < 0) {
            x += sx; i++;
        } else {
            y += sy; j++;
        }
        if ((x != x1 || y != y1) && wall(w, x, y))
            clear = 0;
    }
    return clear;
}

/* path.sight(b, w, solid, x0, y0, x1, y1): line_clear() between them */
NATIVE(path_sight)
{
    (void)argc;
    struct walls w;
    if (!walls_of(vm, a, "path.sight", &w))
        return 0;
    int32_t x = whole(a[3], 0), y = whole(a[4], 0);
    int32_t x1 = whole(a[5], 0), y1 = whole(a[6], 0);
    int clear = line_clear(&w, x, y, x1, y1);
    int32_t nx = x1 - x, ny = y1 - y;
    nx = nx < 0 ? -nx : nx;
    ny = ny < 0 ? -ny : ny;
    vm_charge(vm, 8 + (uint32_t)(nx + ny) / 4);     /* measured, sysprobe3 */
    *out = mk(T_INT, clear);
    return 1;
}

/* path.view(b, w, solid, x, y, r, out, [mark]): every cell within r of
 * x, y that path.sight sees -- walls included, so a room's walls show --
 * into out, a Uint8Array the board's size: cleared first and each seen
 * cell 1; with mark, only the seen cells set to it, the rest kept, for a
 * map that remembers. How many are seen. */
NATIVE(path_view)
{
    struct walls w;
    if (!walls_of(vm, a, "path.view", &w))
        return 0;
    int32_t x = whole(a[3], 0), y = whole(a[4], 0), r = whole(a[5], 0);
    int32_t cells = w.b.w * w.b.h;
    if (a[6].tag != T_U8 || (int32_t)U8A(vm, a[6])->len < cells)
        return vm_fail(vm, "path.view() needs a Uint8Array the board's "
                       "size to mark", "", "");
    int keep = argc > 7 && is_num(a[7]);
    uint8_t mark = keep ? (uint8_t)floor_int(a[7]) : 1, *o = U8A(vm, a[6])->d;
    if (!keep)
        memset(o, 0, (size_t)cells);
    if (r < 0)
        r = 0;
    int32_t seen = 0;
    uint32_t steps = 0;
    /* a viewer off the board sees nothing of it */
    if (x < 0 || y < 0 || x >= w.b.w || y >= w.b.h) {
        vm_charge(vm, 20);
        *out = mk(T_INT, 0);
        return 1;
    }
    /* the square round x, y as far as it lies on the board, its distances
     * squared in 64 bits: r may reach far past it */
    int64_t r2 = (int64_t)r * r + r;
    int32_t cy0 = (int64_t)y - r > 0 ? y - r : 0;
    int32_t cy1 = (int64_t)y + r < w.b.h - 1 ? y + r : w.b.h - 1;
    int32_t cx0 = (int64_t)x - r > 0 ? x - r : 0;
    int32_t cx1 = (int64_t)x + r < w.b.w - 1 ? x + r : w.b.w - 1;
    for (int32_t cy = cy0; cy <= cy1; cy++)
        for (int32_t cx = cx0; cx <= cx1; cx++) {
            int64_t dx = (int64_t)cx - x, dy = (int64_t)cy - y;
            if (dx * dx + dy * dy > r2)
                continue;
            steps += (uint32_t)((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));
            if (line_clear(&w, x, y, cx, cy)) {
                o[cy * w.b.w + cx] = mark;
                seen++;
            }
        }
    vm_charge(vm, 20 + (uint32_t)seen + steps / 4);
    *out = mk(T_INT, seen);
    return 1;
}

/* ---- rays ---------------------------------------------------------------- */

static int map_solid(const struct vm *vm, int32_t cx, int32_t cy, int32_t mask)
{
    if (cx < 0 || cy < 0 || cx >= vm->map_w || cy >= vm->map_h)
        return 0;
    const uint8_t *map = MEM(vm, vm->map), *flags = MEM(vm, vm->flags);
    return (flags[map[cy * vm->map_w + cx]] & mask) != 0;
}

/* path.ray(x, y, dx, dy, solid, out, [len]): from world pixel x, y along
 * dx, dy, the first map cell whose flags share a bit with solid, within
 * len pixels (1024 by default, 65536 at most). Into out, an array of at
 * least five: the point hit, the cell's column and row, and the face of
 * the cell it met (DIR.LEFT for its left face; -1 when it starts inside
 * one). Returns 1 on a hit, 0 when the ray goes clear. Off the map is
 * open. */
NATIVE(path_ray)
{
    if (!vm_out_ok(vm, a[5], 5))
        return vm_fail(vm, "path.ray() writes to an array or Int32Array of at "
                       "least five", "", "");
    int32_t mask = whole(a[4], 0);
    int64_t x = scaled(a[0]), y = scaled(a[1]);
    int64_t dx = scaled(a[2]), dy = scaled(a[3]);
    int64_t far = argc > 6 && is_num(a[6]) ? scaled(a[6]) : (int64_t)1024 << 16;
    if (far > (int64_t)65536 << 16)
        far = (int64_t)65536 << 16;
    *out = mk(T_INT, 0);
    /* the direction as a unit vector, 16.16; only its angle matters, so it
     * is scaled down until its square fits */
    while (dx >= (1 << 30) || dx <= -(1 << 30) || dy >= (1 << 30) ||
           dy <= -(1 << 30)) {
        dx /= 2;
        dy /= 2;
    }
    int64_t l2 = dx * dx + dy * dy;
    if (!l2 || !mask) {
        vm_charge(vm, 6);
        return 1;
    }
    uint64_t l = 1;
    while (l * l < (uint64_t)l2)
        l <<= 1;
    for (uint64_t step = l >> 1; step; step >>= 1)
        if ((l - step) * (l - step) >= (uint64_t)l2)
            l -= step;
    int64_t ux = (dx << 16) / (int64_t)l, uy = (dy << 16) / (int64_t)l;
    int32_t cx = (int32_t)(x >> 19), cy = (int32_t)(y >> 19);
    int32_t sx = ux > 0 ? 1 : -1, sy = uy > 0 ? 1 : -1, face = -1;
    /* the distance along the ray to the next column's and row's edge, and
     * between them, 16.16 pixels */
    const int64_t never = (int64_t)1 << 48;
    int64_t tx = never, ty = never, stepx = never, stepy = never;
    if (ux) {
        int64_t edge = ux > 0 ? ((int64_t)(cx + 1) << 19) - x
                              : x - ((int64_t)cx << 19);
        tx = (edge << 16) / (ux > 0 ? ux : -ux);
        stepx = ((int64_t)8 << 32) / (ux > 0 ? ux : -ux);
    }
    if (uy) {
        int64_t edge = uy > 0 ? ((int64_t)(cy + 1) << 19) - y
                              : y - ((int64_t)cy << 19);
        ty = (edge << 16) / (uy > 0 ? uy : -uy);
        stepy = ((int64_t)8 << 32) / (uy > 0 ? uy : -uy);
    }
    int64_t t = 0;
    uint32_t cells = 0;
    while (!map_solid(vm, cx, cy, mask)) {
        if (tx < ty) {
            t = tx;
            tx += stepx;
            cx += sx;
            face = sx > 0 ? 0 : 1;          /* DIR.LEFT, DIR.RIGHT */
        } else {
            t = ty;
            ty += stepy;
            cy += sy;
            face = sy > 0 ? 2 : 3;          /* DIR.UP, DIR.DOWN */
        }
        cells++;
        if (t > far) {
            vm_charge(vm, 10 + cells * 11);
            return 1;
        }
    }
    /* the point hit, on the face exactly: the unit vector's rounding
     * would put it a hair short */
    int64_t hx = x + ((ux * t) >> 16), hy = y + ((uy * t) >> 16);
    if (face == 0 || face == 1)
        hx = (int64_t)(face == 0 ? cx : cx + 1) << 19;
    else if (face == 2 || face == 3)
        hy = (int64_t)(face == 2 ? cy : cy + 1) << 19;
    vm_out_set(vm, a[5], 0, mk(T_FIX, wrap32(hx)));
    vm_out_set(vm, a[5], 1, mk(T_FIX, wrap32(hy)));
    vm_out_set(vm, a[5], 2, mk(T_INT, cx));
    vm_out_set(vm, a[5], 3, mk(T_INT, cy));
    vm_out_set(vm, a[5], 4, mk(T_INT, face));
    *out = mk(T_INT, 1);
    /* measured on the rig (sysprobe3): a cell crossed is a 64-bit divide
     * or two, about 11 units */
    vm_charge(vm, 10 + cells * 11);
    return 1;
}
