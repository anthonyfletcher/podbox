/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade systems: the grid -- the bulk work of board puzzles on a
 * Uint8Array of cells, a row after another, w wide: falling blocks,
 * matching, sliding and filling. 0 is an empty cell. The map's own bytes,
 * mem(MEM.MAP), are a board too. A cell's index is row * w + column.
 *
 * Fill and region need a queue as long as the board; it is taken from the
 * heap for the call and is garbage after it (the heap never moves, so the
 * board stays where it is).
 *
 * Parts, in order:
 *   boards      reading one, and a call's scratch
 *   regions     fill, region
 *   lines       runs, full, collapse, fall, slide
 *   shapes      fits, place, rotate
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)

/* ---- boards ------------------------------------------------------------ */

/* A Uint8Array w wide, a whole number of rows */
int board_of(struct vm *vm, struct value b, struct value w,
                    const char *fn, struct board *o)
{
    int32_t n = is_num(w) ? floor_int(w) : 0;
    if (b.tag != T_U8 || n < 1 || U8A(vm, b)->len % (uint32_t)n ||
        !U8A(vm, b)->len)
        return vm_fail(vm, fn, "() needs a Uint8Array board and its width, "
                       "a whole number of rows", "");
    o->c = U8A(vm, b)->d;
    o->w = n;
    o->h = (int32_t)(U8A(vm, b)->len / (uint32_t)n);
    return 1;
}

/* bytes of zeroed scratch for this call, or NULL with the error set */
void *scratch(struct vm *vm, uint32_t bytes)
{
    uint32_t off = gc_alloc(vm, T_U8, sizeof(uint32_t) + bytes);
    return off ? ((struct u8a *)MEM(vm, off))->d : NULL;
}

static int32_t whole(struct value v, int32_t dflt)
{
    return is_num(v) ? floor_int(v) : dflt;
}

/* An Int32Array to write into, and its length */
static int32_t *ints_of(struct vm *vm, struct value v, uint32_t *len,
                        const char *fn)
{
    if (v.tag != T_I32) {
        vm_fail(vm, fn, "() writes to an Int32Array", "");
        return NULL;
    }
    *len = I32(vm, v)->len;
    return I32(vm, v)->d;
}

/* ---- regions ------------------------------------------------------------ */

/* The 4-connected cells equal to the one at start, breadth first, into
 * queue; seen marks them. Returns how many. */
static uint32_t flood(const struct board *b, int32_t start, uint32_t *queue,
                      uint8_t *seen)
{
    uint8_t v = b->c[start];
    uint32_t head = 0, tail = 0;
    queue[tail++] = (uint32_t)start;
    seen[start >> 3] |= (uint8_t)(1 << (start & 7));
    while (head < tail) {
        int32_t i = (int32_t)queue[head++], x = i % b->w;
        int32_t next[4] = { x > 0 ? i - 1 : -1, x < b->w - 1 ? i + 1 : -1,
                            i - b->w, i + b->w };
        for (int k = 0; k < 4; k++) {
            int32_t j = next[k];
            if (j < 0 || j >= b->w * b->h || b->c[j] != v ||
                (seen[j >> 3] & (1 << (j & 7))))
                continue;
            seen[j >> 3] |= (uint8_t)(1 << (j & 7));
            queue[tail++] = (uint32_t)j;
        }
    }
    return tail;
}

/* The board, the start cell and the region's scratch, or 0 */
static int region_setup(struct vm *vm, struct value *a, const char *fn,
                        struct board *b, int32_t *start, uint32_t **queue,
                        uint8_t **seen)
{
    int32_t x = whole(a[2], -1), y = whole(a[3], -1);
    if (!board_of(vm, a[0], a[1], fn, b))
        return 0;
    *start = -1;
    if (x < 0 || y < 0 || x >= b->w || y >= b->h)
        return 1;
    uint32_t cells = (uint32_t)(b->w * b->h);
    uint8_t *mem = scratch(vm, cells * 4 + (cells + 7) / 8);
    if (!mem)
        return 0;
    *queue = (uint32_t *)mem;
    *seen = mem + cells * 4;
    *start = y * b->w + x;
    return 1;
}

/* grid.fill(b, w, x, y, v): the region of equal cells at x, y set to v;
 * returns the cells changed */
NATIVE(grid_fill)
{
    (void)argc;
    struct board b;
    int32_t start;
    uint32_t *queue;
    uint8_t *seen;
    if (!region_setup(vm, a, "grid.fill", &b, &start, &queue, &seen))
        return 0;
    uint32_t n = 0;
    uint8_t v = (uint8_t)whole(a[4], 0);
    if (start >= 0 && b.c[start] != v) {
        n = flood(&b, start, queue, seen);
        for (uint32_t i = 0; i < n; i++)
            b.c[queue[i]] = v;
    }
    /* measured on the ARM rig (.build/arcade-rig, sysprobe): the flood
     * about 7 units a cell */
    vm_charge(vm, 10 + n * 7);
    *out = mk(T_INT, (int32_t)n);
    return 1;
}

/* grid.region(b, w, x, y, out): the region of equal cells at x, y, its
 * indices into out as far as it reaches; returns the region's size */
NATIVE(grid_region)
{
    (void)argc;
    struct board b;
    int32_t start;
    uint32_t *queue, len;
    uint8_t *seen;
    if (!ints_of(vm, a[4], &len, "grid.region") ||
        !region_setup(vm, a, "grid.region", &b, &start, &queue, &seen))
        return 0;
    uint32_t n = start >= 0 ? flood(&b, start, queue, seen) : 0;
    int32_t *o = I32(vm, a[4])->d;
    for (uint32_t i = 0; i < n && i < len; i++)
        o[i] = (int32_t)queue[i];
    vm_charge(vm, 10 + n * 7);         /* the flood, as grid.fill */
    *out = mk(T_INT, (int32_t)n);
    return 1;
}

/* ---- lines --------------------------------------------------------------- */

/* Marks the cells of a line (count cells, step apart from first) that sit
 * in runs of at least n equal non-zero values */
static void mark_runs(const struct board *b, int32_t first, int32_t step,
                      int32_t count, int32_t n, uint8_t *mark)
{
    int32_t run = 0;
    for (int32_t k = 0; k <= count; k++) {
        uint8_t v = k < count ? b->c[first + k * step] : 0;
        if (k > 0 && k < count && v && v == b->c[first + (k - 1) * step]) {
            run++;
            continue;
        }
        if (run >= n)
            for (int32_t j = k - run; j < k; j++)
                mark[first + j * step] = 1;
        run = v ? 1 : 0;
    }
}

/* grid.runs(b, w, n, out): every cell in a row or column run of at least n
 * equal non-zero cells, each once and in order, into out as far as it
 * reaches; returns how many */
NATIVE(grid_runs)
{
    (void)argc;
    struct board b;
    uint32_t len;
    int32_t n = whole(a[2], 0);
    if (!board_of(vm, a[0], a[1], "grid.runs", &b) ||
        !ints_of(vm, a[3], &len, "grid.runs"))
        return 0;
    if (n < 2)
        return vm_fail(vm, "grid.runs() finds runs of 2 or more", "", "");
    uint32_t cells = (uint32_t)(b.w * b.h);
    uint8_t *mark = scratch(vm, cells);
    if (!mark)
        return 0;
    for (int32_t y = 0; y < b.h; y++)
        mark_runs(&b, y * b.w, 1, b.w, n, mark);
    for (int32_t x = 0; x < b.w; x++)
        mark_runs(&b, x, b.w, b.h, n, mark);
    int32_t *o = I32(vm, a[3])->d;
    uint32_t k = 0;
    for (uint32_t i = 0; i < cells; i++)
        if (mark[i]) {
            if (k < len)
                o[k] = (int32_t)i;
            k++;
        }
    /* measured on the ARM rig (.build/arcade-rig, sysprobe) */
    vm_charge(vm, 10 + cells * 9 / 4);
    *out = mk(T_INT, (int32_t)k);
    return 1;
}

/* grid.full(b, w, out): the rows with no empty cell, top down, into out;
 * returns how many */
NATIVE(grid_full)
{
    (void)argc;
    struct board b;
    uint32_t len;
    if (!board_of(vm, a[0], a[1], "grid.full", &b) ||
        !ints_of(vm, a[2], &len, "grid.full"))
        return 0;
    int32_t *o = I32(vm, a[2])->d;
    uint32_t k = 0;
    for (int32_t y = 0; y < b.h; y++) {
        if (memchr(b.c + y * b.w, 0, (size_t)b.w))
            continue;
        if (k < len)
            o[k] = y;
        k++;
    }
    /* measured on the ARM rig (.build/arcade-rig, sysprobe2 and 3), as are the
     * grid charges below */
    vm_charge(vm, 10 + (uint32_t)(b.w * b.h) / 4);
    *out = mk(T_INT, (int32_t)k);
    return 1;
}

/* grid.collapse(b, w, rows, n): the n rows listed taken out, those above
 * dropping into their place and empty rows coming in at the top; returns
 * the rows removed */
NATIVE(grid_collapse)
{
    (void)argc;
    struct board b;
    struct column rows;
    int32_t n = whole(a[3], -1);
    if (!board_of(vm, a[0], a[1], "grid.collapse", &b))
        return 0;
    if (!column_of(vm, a[2], &rows) || rows.tag == T_INT || n < 0 ||
        (uint32_t)n > rows.len)
        return vm_fail(vm, "grid.collapse() takes an array of rows and how "
                       "many of them to remove", "", "");
    uint8_t *gone = scratch(vm, (uint32_t)b.h);
    if (!gone)
        return 0;
    int32_t removed = 0;
    for (int32_t i = 0; i < n; i++) {
        int32_t r = column_at(&rows, (uint32_t)i, -1);
        if (r >= 0 && r < b.h && !gone[r]) {
            gone[r] = 1;
            removed++;
        }
    }
    int32_t dst = b.h - 1;
    for (int32_t src = b.h - 1; src >= 0; src--) {
        if (gone[src])
            continue;
        if (dst != src)
            memcpy(b.c + dst * b.w, b.c + src * b.w, (size_t)b.w);
        dst--;
    }
    if (dst >= 0)
        memset(b.c, 0, (size_t)((dst + 1) * b.w));
    vm_charge(vm, 10 + (uint32_t)n + (uint32_t)(b.w * b.h) / BYTES_PER_UNIT);
    *out = mk(T_INT, removed);
    return 1;
}

/* grid.fall(b, w): every non-zero cell down through the empty ones below
 * it; returns the cells that moved */
NATIVE(grid_fall)
{
    (void)argc;
    struct board b;
    if (!board_of(vm, a[0], a[1], "grid.fall", &b))
        return 0;
    int32_t moved = 0;
    for (int32_t x = 0; x < b.w; x++) {
        int32_t dst = b.h - 1;
        for (int32_t y = b.h - 1; y >= 0; y--) {
            uint8_t v = b.c[y * b.w + x];
            if (!v)
                continue;
            if (dst != y) {
                b.c[dst * b.w + x] = v;
                b.c[y * b.w + x] = 0;
                moved++;
            }
            dst--;
        }
    }
    vm_charge(vm, 10 + (uint32_t)(b.w * b.h) * 3 / 4);
    *out = mk(T_INT, moved);
    return 1;
}

/* grid.slide(b, w, dir, [merged]): every non-zero cell slid as far as it
 * goes toward side dir (DIR.LEFT, RIGHT, UP or DOWN), two equal cells
 * meeting becoming one of the value after, once a move -- 2048's rule.
 * merged, an Int32Array, gets the merged cells' indices and then -1 if
 * there is room; returns the cells changed, 0 when nothing moved. */
NATIVE(grid_slide)
{
    struct board b;
    int32_t dir = whole(a[2], -1);
    uint32_t mlen = 0, nmerged = 0;
    int32_t *merged = NULL;
    if (!board_of(vm, a[0], a[1], "grid.slide", &b))
        return 0;
    if (dir < 0 || dir > 3)
        return vm_fail(vm, "grid.slide() slides toward DIR.LEFT, RIGHT, UP "
                       "or DOWN", "", "");
    if (argc > 3 && a[3].tag != T_NULL &&
        !(merged = ints_of(vm, a[3], &mlen, "grid.slide")))
        return 0;
    /* a line runs from the side slid toward: its first cell and step */
    int32_t lines = dir < 2 ? b.h : b.w, count = dir < 2 ? b.w : b.h;
    int32_t changed = 0;
    for (int32_t l = 0; l < lines; l++) {
        int32_t first, step;
        switch (dir) {
        case 0: first = l * b.w; step = 1; break;
        case 1: first = l * b.w + b.w - 1; step = -1; break;
        case 2: first = l; step = b.w; break;
        default: first = (b.h - 1) * b.w + l; step = -b.w; break;
        }
        int32_t put = 0, last = -1;     /* last: a cell put and unmerged */
        for (int32_t k = 0; k < count; k++) {
            uint8_t v = b.c[first + k * step];
            if (!v)
                continue;
            b.c[first + k * step] = 0;
            if (last >= 0 && b.c[first + last * step] == v) {
                int32_t at = first + last * step;
                b.c[at] = v < 255 ? (uint8_t)(v + 1) : v;
                if (merged && nmerged < mlen)
                    merged[nmerged] = at;
                nmerged++;
                changed++;
                last = -1;
                continue;
            }
            if (put != k)
                changed++;
            b.c[first + put * step] = v;
            last = put++;
        }
    }
    if (merged && nmerged < mlen)
        merged[nmerged] = -1;
    vm_charge(vm, 10 + (uint32_t)(b.w * b.h));
    *out = mk(T_INT, changed);
    return 1;
}

/* ---- shapes -------------------------------------------------------------- */

/* A shape: a Uint8Array sw wide, its 0s empty */
static int shape_of(struct vm *vm, struct value s, struct value sw,
                    const char *fn, struct board *o)
{
    int32_t n = is_num(sw) ? floor_int(sw) : 0;
    if (s.tag != T_U8 || n < 1 || !U8A(vm, s)->len ||
        U8A(vm, s)->len % (uint32_t)n)
        return vm_fail(vm, fn, "() needs a Uint8Array shape and its width, "
                       "a whole number of rows", "");
    o->c = U8A(vm, s)->d;
    o->w = n;
    o->h = (int32_t)(U8A(vm, s)->len / (uint32_t)n);
    return 1;
}

static int fit_or_place(struct vm *vm, struct value *a, int place,
                        const char *fn, struct value *out)
{
    struct board b, s;
    if (!board_of(vm, a[0], a[1], fn, &b) || !shape_of(vm, a[2], a[3], fn, &s))
        return 0;
    int32_t x = whole(a[4], 0), y = whole(a[5], 0), n = 0, fits = 1;
    for (int32_t j = 0; j < s.h; j++)
        for (int32_t i = 0; i < s.w; i++) {
            uint8_t v = s.c[j * s.w + i];
            int32_t bx = x + i, by = y + j;
            if (!v)
                continue;
            if (place) {
                if (bx >= 0 && bx < b.w && by >= 0 && by < b.h) {
                    b.c[by * b.w + bx] = v;
                    n++;
                }
            } else if (bx < 0 || bx >= b.w || by >= b.h ||
                       (by >= 0 && b.c[by * b.w + bx])) {
                fits = 0;
            }
        }
    vm_charge(vm, 8 + (uint32_t)(s.w * s.h) * 6 / 5);
    *out = mk(T_INT, place ? n : fits);
    return 1;
}

/* grid.fits(b, w, s, sw, x, y): whether shape s at column x, row y lands
 * only on empty cells; above the top is free, past the sides and the
 * bottom is not */
NATIVE(grid_fits)
{
    (void)argc;
    return fit_or_place(vm, a, 0, "grid.fits", out);
}

/* grid.place(b, w, s, sw, x, y): shape s's non-zero cells written onto the
 * board where they land on it; returns how many */
NATIVE(grid_place)
{
    (void)argc;
    return fit_or_place(vm, a, 1, "grid.place", out);
}

/* grid.rotate(s, sw, out, turns): shape s turned clockwise by quarter
 * turns (negative turns go the other way) into out, a Uint8Array as long;
 * returns its width after */
NATIVE(grid_rotate)
{
    (void)argc;
    struct board s;
    if (!shape_of(vm, a[0], a[1], "grid.rotate", &s))
        return 0;
    uint32_t len = (uint32_t)(s.w * s.h);
    if (a[2].tag != T_U8 || U8A(vm, a[2])->len < len)
        return vm_fail(vm, "grid.rotate() writes to a Uint8Array as long as "
                       "the shape", "", "");
    int32_t t = whole(a[3], 1) & 3;
    /* the shape copied first, so out may be s itself */
    uint8_t *src = scratch(vm, len);
    if (!src)
        return 0;
    memcpy(src, s.c, len);
    uint8_t *dst = U8A(vm, a[2])->d;
    int32_t w = s.w, h = s.h, nw = t & 1 ? h : w;
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
            int32_t nx, ny;
            switch (t) {
            case 0: nx = x; ny = y; break;
            case 1: nx = h - 1 - y; ny = x; break;
            case 2: nx = w - 1 - x; ny = h - 1 - y; break;
            default: nx = y; ny = w - 1 - x; break;
            }
            dst[ny * nw + nx] = src[y * w + x];
        }
    vm_charge(vm, 8 + len * 5 / 4);
    *out = mk(T_INT, nw);
    return 1;
}
