/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade systems: cells -- a field of materials, one byte a cell, stepped
 * a tick at a time by a table of rules: falling sand, water, fire, smoke,
 * and Life. The field is an image, so blit() draws it and the palette
 * colours each material, or a Uint8Array board.
 *
 * Each material has a way of moving (staying, falling as a powder,
 * falling and flowing as a liquid, rising and spreading as a gas), a
 * density -- a mover swaps into any neighbour lighter than itself, so
 * sand sinks through water and walls are 255 -- a chance a tick of
 * becoming another material, and reactions: beside material b, become c
 * and turn b to d, with a chance out of 256. A tick scans bottom up,
 * left to right and right to left on alternate ticks, and a cell that
 * has moved or changed this tick is not touched again, so nothing moves
 * twice and nothing drifts to one side. Chances use the cart's random
 * generator, so a run replays.
 *
 * The field is kept in 16x16 chunks, and a chunk sleeps once nothing in
 * it or beside it changed: a settled pile costs nothing. A sleeping
 * chunk's bytes are summed, so drawing into it by any means wakes it.
 *
 * Parts, in order:
 *   rules      cells.rules, and checking a rules record handed back
 *   fields     reading one, its sleep state
 *   the step   cells.step
 *   life       cells.life
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)

#define MATERIALS 256
#define REACTIONS 64
#define CHUNK_SHIFT 4
#define CHUNK     (1 << CHUNK_SHIFT)
#define FIELD_MAX 256           /* cells across and down */

enum { MOVE_STATIC, MOVE_POWDER, MOVE_LIQUID, MOVE_GAS };

/* ---- rules ------------------------------------------------------------- */

enum { R_MOVE, R_SPREAD, R_DENSITY, R_DECAY, R_BECOMES,
       R_A, R_B, R_C, R_D, R_CHANCE, R_FIELDS };
static const char *const rule_keys[R_FIELDS] = {
    "move", "spread", "density", "decay", "becomes",
    "a", "b", "c", "d", "chance"
};

static uint32_t rule_len(int f)
{
    return f < R_A ? MATERIALS : REACTIONS;
}

/* cells.rules(): a record of Uint8Arrays for the cart to fill -- per
 * material move (MOVE.*), spread, density, decay (a chance a tick out of
 * 256) and becomes; per reaction a, b, c, d and chance */
NATIVE(cells_rules)
{
    (void)a; (void)argc;
    if (!vm_new_record(vm, rule_keys, R_FIELDS, out))
        return 0;
    vm->rules_shape = OBJ(vm, *out)->shape;
    vm->tmp[vm->ntmp++] = *out;
    for (int f = 0; f < R_FIELDS; f++) {
        uint32_t n = rule_len(f);
        uint32_t off = gc_alloc(vm, T_U8, (uint32_t)(sizeof(struct u8a) -
                                                     sizeof(struct hdr)) + n);
        if (!off) {
            vm->ntmp--;
            return 0;
        }
        ((struct u8a *)MEM(vm, off))->len = n;
        BUF(vm, OBJ(vm, *out)->buf)->v[f] = mk(T_U8, (int32_t)off);
    }
    vm->ntmp--;
    vm_charge(vm, 40);
    return 1;
}

struct rules
{
    const uint8_t *f[R_FIELDS];
};

static int rules_of(struct vm *vm, struct value v, struct rules *r)
{
    uint32_t s = v.tag == T_OBJ ? OBJ(vm, v)->shape : 0;
    while (s && s != vm->rules_shape)
        s = vm->shapes[s].parent;
    if (!vm->rules_shape || s != vm->rules_shape)
        return vm_fail(vm, "cells.step() needs rules from cells.rules()",
                       "", "");
    const struct value *f = BUF(vm, OBJ(vm, v)->buf)->v;
    for (int i = 0; i < R_FIELDS; i++) {
        if (f[i].tag != T_U8 || U8A(vm, f[i])->len < rule_len(i))
            return vm_fail(vm, "cells.step(): the rules' ", rule_keys[i],
                           " is no longer its Uint8Array");
        r->f[i] = U8A(vm, f[i])->d;
    }
    return 1;
}

/* ---- fields -------------------------------------------------------------- */

struct field
{
    uint8_t *c;
    int32_t w, h;
    uint32_t id;                /* the object, for its sleep state */
};

/* An image, or a Uint8Array board w wide, of at most 256 by 256 */
static int field_of(struct vm *vm, struct value v, struct value w,
                    const char *fn, struct field *o)
{
    if (v.tag == T_IMG) {
        struct img *im = MEM(vm, (uint32_t)v.v);
        o->c = im->px;
        o->w = im->width;
        o->h = im->height;
    } else {
        struct board b;
        if (!board_of(vm, v, w, fn, &b))
            return 0;
        o->c = b.c;
        o->w = b.w;
        o->h = b.h;
    }
    o->id = (uint32_t)v.v;
    if (o->w > FIELD_MAX || o->h > FIELD_MAX)
        return vm_fail(vm, fn, "() works on fields of up to 256 by 256", "");
    return 1;
}

/* A chunk's bytes summed, Fletcher's way, to see whether it was drawn on */
static uint32_t chunk_sum(const struct field *f, int32_t cx, int32_t cy)
{
    int32_t x0 = cx * CHUNK, y0 = cy * CHUNK;
    int32_t x1 = x0 + CHUNK < f->w ? x0 + CHUNK : f->w;
    int32_t y1 = y0 + CHUNK < f->h ? y0 + CHUNK : f->h;
    uint32_t s1 = 1, s2 = 0;
    for (int32_t y = y0; y < y1; y++) {
        const uint8_t *p = f->c + y * f->w + x0;
        for (int32_t x = x0; x < x1; x++) {
            s1 += *p++;
            s2 += s1;
        }
    }
    return s1 ^ (s2 << 12) ^ (s2 >> 20);
}

/* The sleep state for this field: the one it used last, or the older of
 * the two slots, all awake */
static struct cellsleep *sleep_for(struct vm *vm, const struct field *f)
{
    struct cellsleep *s = vm->sleep;
    for (int i = 0; i < 2; i++)
        if (s[i].id == f->id && s[i].w == f->w && s[i].h == f->h) {
            s[i].used = ++vm->sleep_clock;
            return &s[i];
        }
    struct cellsleep *old = s[0].used <= s[1].used ? &s[0] : &s[1];
    old->id = f->id;
    old->w = f->w;
    old->h = f->h;
    old->used = ++vm->sleep_clock;
    memset(old->awake, 1, sizeof(old->awake));
    return old;
}

/* ---- the step ------------------------------------------------------------ */

struct stepper
{
    struct field f;
    struct rules r;
    uint8_t *moved;             /* a bit a cell: changed this tick */
    uint8_t changed[(FIELD_MAX / CHUNK) * (FIELD_MAX / CHUNK)];
    int32_t cw;                 /* chunks across */
    uint8_t first[MATERIALS];   /* the first reaction for each a, or 255 */
    uint8_t next[REACTIONS];    /* the next with the same a, or 255 */
    struct vm *vm;
    uint32_t bits;              /* random bits left over */
    int nbits;
    uint32_t moves;
};

static uint32_t rand8(struct stepper *s)
{
    if (s->nbits < 8) {
        s->bits = core_random(s->vm);
        s->nbits = 32;
    }
    uint32_t r = s->bits & 0xff;
    s->bits >>= 8;
    s->nbits -= 8;
    return r;
}

static int is_moved(const struct stepper *s, int32_t i)
{
    return s->moved[i >> 3] & (1 << (i & 7));
}

/* Cell x, y's chunk is to be awake next tick */
static inline void busy(struct stepper *s, int32_t x, int32_t y)
{
    s->changed[(y >> CHUNK_SHIFT) * s->cw + (x >> CHUNK_SHIFT)] = 1;
}

/* Cell x, y becomes m and is done for this tick */
static inline void set(struct stepper *s, int32_t x, int32_t y, uint8_t m)
{
    int32_t i = y * s->f.w + x;
    s->f.c[i] = m;
    s->moved[i >> 3] |= (uint8_t)(1 << (i & 7));
    busy(s, x, y);
}

/* Whether m can move into x, y: on the field, not yet touched this tick,
 * and lighter */
static inline int lighter(const struct stepper *s, int32_t x, int32_t y,
                          uint8_t m)
{
    if (x < 0 || y < 0 || x >= s->f.w || y >= s->f.h)
        return 0;
    int32_t j = y * s->f.w + x;
    return !is_moved(s, j) &&
           s->r.f[R_DENSITY][s->f.c[j]] < s->r.f[R_DENSITY][m];
}

/* The mover at x, y into tx, ty, and what was there back. Only the mover
 * is done for the tick: the cell it left can still be moved into, or the
 * water a grain sank through would wall the next grain in. */
static void swap(struct stepper *s, int32_t x, int32_t y, int32_t tx,
                 int32_t ty)
{
    int32_t i = y * s->f.w + x, j = ty * s->f.w + tx;
    uint8_t m = s->f.c[i];
    s->f.c[i] = s->f.c[j];
    busy(s, x, y);
    set(s, tx, ty, m);
    s->moves++;
}

static const int8_t near_x[4] = { -1, 1, 0, 0 }, near_y[4] = { 0, 0, -1, 1 };

/* The material at x, y, or -1 off the field */
static inline int32_t at(const struct stepper *s, int32_t x, int32_t y)
{
    if (x < 0 || y < 0 || x >= s->f.w || y >= s->f.h)
        return -1;
    return s->f.c[y * s->f.w + x];
}

/* One cell's tick: one reaction with a random neighbour, then its life
 * span, then a move -- so fire spreads on the tick it dies. A material
 * with a life span, or with a partner it reacts with beside it, keeps its
 * chunk awake whether or not it changed: its chances come round again. */
__attribute__((optimize("O2")))
static void step_cell(struct stepper *s, int32_t x, int32_t y)
{
    uint8_t m = s->f.c[y * s->f.w + x];
    const uint8_t *const *r = s->r.f;
    if (r[R_DECAY][m])
        busy(s, x, y);
    if (s->first[m] != 255) {
        uint32_t k = rand8(s) & 3;
        int32_t tx = x + near_x[k], ty = y + near_y[k], b = at(s, tx, ty);
        for (uint32_t q = s->first[m]; q != 255; q = s->next[q]) {
            for (int n = 0; n < 4; n++)
                if (at(s, x + near_x[n], y + near_y[n]) == r[R_B][q])
                    busy(s, x, y);
            if (r[R_B][q] != b || is_moved(s, ty * s->f.w + tx))
                continue;
            if (rand8(s) < r[R_CHANCE][q]) {
                set(s, x, y, r[R_C][q]);
                set(s, tx, ty, r[R_D][q]);
                return;
            }
            break;
        }
    }
    if (r[R_DECAY][m] && rand8(s) < r[R_DECAY][m]) {
        set(s, x, y, r[R_BECOMES][m]);
        return;
    }
    uint8_t mv = r[R_MOVE][m];
    if (mv == MOVE_STATIC)
        return;
    int32_t dy = mv == MOVE_GAS ? -1 : 1;
    if (lighter(s, x, y + dy, m)) {
        swap(s, x, y, x, y + dy);
        return;
    }
    int32_t side = rand8(s) & 1 ? 1 : -1;
    for (int t = 0; t < 2; t++, side = -side)
        if (lighter(s, x + side, y + dy, m)) {
            swap(s, x, y, x + side, y + dy);
            return;
        }
    if (mv == MOVE_POWDER)
        return;
    /* liquids and gases flow sideways, as far as spread, one way then
     * the other */
    int32_t far = r[R_SPREAD][m] ? r[R_SPREAD][m] : 1;
    for (int t = 0; t < 2; t++, side = -side) {
        int32_t to = 0;
        for (int32_t k = 1; k <= far && lighter(s, x + side * k, y, m); k++)
            to = k;
        if (to) {
            swap(s, x, y, x + side * to, y);
            return;
        }
    }
}

/* Every awake chunk's cells, bottom up, a row left to right or right to
 * left as forward says; air is passed over. Returns the cells stepped. */
__attribute__((optimize("O2")))
static uint32_t scan(struct stepper *s, const uint8_t *awake, int forward)
{
    uint32_t visited = 0;
    const int32_t w = s->f.w, cw = s->cw;
    for (int32_t y = s->f.h - 1; y >= 0; y--) {
        const uint8_t *chunks = awake + (y >> CHUNK_SHIFT) * cw;
        const uint8_t *row = s->f.c + y * w;
        for (int32_t k = 0; k < cw; k++) {
            int32_t cx = forward ? k : cw - 1 - k;
            if (!chunks[cx])
                continue;
            int32_t x0 = cx << CHUNK_SHIFT;
            int32_t x1 = x0 + CHUNK < w ? x0 + CHUNK : w;
            for (int32_t n = x0; n < x1; n++) {
                int32_t x = forward ? n : x1 - 1 - (n - x0);
                if (!row[x] || is_moved(s, y * w + x))
                    continue;
                visited++;
                step_cell(s, x, y);
            }
        }
    }
    return visited;
}

/* cells.step(field, w, rules): one tick of the field -- an image, or a
 * Uint8Array board w wide -- by the rules. Returns the cells that moved. */
NATIVE(cells_step)
{
    (void)argc;
    static struct stepper s;
    if (!field_of(vm, a[0], a[1], "cells.step", &s.f) ||
        !rules_of(vm, a[2], &s.r))
        return 0;
    uint32_t cells = (uint32_t)(s.f.w * s.f.h);
    s.moved = scratch(vm, (cells + 7) / 8);
    if (!s.moved)
        return 0;
    /* scratch may have collected: read the field and rules again */
    field_of(vm, a[0], a[1], "cells.step", &s.f);
    rules_of(vm, a[2], &s.r);
    s.vm = vm;
    s.nbits = 0;
    s.moves = 0;
    s.cw = (s.f.w + CHUNK - 1) / CHUNK;
    int32_t chh = (s.f.h + CHUNK - 1) / CHUNK;
    memset(s.changed, 0, sizeof(s.changed));
    memset(s.first, 255, sizeof(s.first));
    for (int32_t q = REACTIONS - 1; q >= 0; q--)
        if (s.r.f[R_CHANCE][q]) {
            s.next[q] = s.first[s.r.f[R_A][q]];
            s.first[s.r.f[R_A][q]] = (uint8_t)q;
        }
    struct cellsleep *sl = sleep_for(vm, &s.f);
    /* a sleeping chunk drawn on since wakes */
    uint32_t summed = 0, awake = 0;
    for (int32_t k = 0; k < s.cw * chh; k++)
        if (!sl->awake[k]) {
            summed++;
            if (chunk_sum(&s.f, k % s.cw, k / s.cw) != sl->sum[k])
                sl->awake[k] = 1;
        }
    uint32_t visited = scan(&s, sl->awake, vm->frame & 1);
    /* next tick: the chunks that changed and their neighbours; the rest
     * sleep with their sums */
    for (int32_t k = 0; k < s.cw * chh; k++) {
        int32_t cx = k % s.cw, cy = k / s.cw, wake = 0;
        for (int32_t j = -1; j <= 1 && !wake; j++)
            for (int32_t i = -1; i <= 1 && !wake; i++) {
                int32_t nx = cx + i, ny = cy + j;
                if (nx >= 0 && ny >= 0 && nx < s.cw && ny < chh &&
                    s.changed[ny * s.cw + nx])
                    wake = 1;
            }
        sl->awake[k] = (uint8_t)wake;
        if (wake)
            awake++;
        else {
            sl->sum[k] = chunk_sum(&s.f, cx, cy);
            summed++;
        }
    }
    vm_charge(vm, 20 + visited + s.moves + summed * 16);
    *out = mk(T_INT, (int32_t)s.moves);
    return 1;
}

/* ---- life ---------------------------------------------------------------- */

/* cells.life(field, w, birth, survive, [born]): one generation of a
 * Life-like rule: a cell with n live neighbours of eight (off the field
 * is dead) is born if bit n of birth is set, and a live one survives if
 * bit n of survive is. Live is non-zero; a cell born is born (1 by
 * default). Life is birth 8, survive 12; smoothing a cave of walls is
 * birth 0x1e0, survive 0x1f0. Returns the cells that changed. */
NATIVE(cells_life)
{
    struct field f;
    if (!field_of(vm, a[0], a[1], "cells.life", &f))
        return 0;
    if (!is_num(a[2]) || !is_num(a[3]))
        return vm_fail(vm, "cells.life() needs the birth and survival "
                       "counts as bits", "", "");
    uint32_t birth = (uint32_t)floor_int(a[2]);
    uint32_t survive = (uint32_t)floor_int(a[3]);
    uint8_t born = argc > 4 && is_num(a[4]) ? (uint8_t)floor_int(a[4]) : 1;
    uint32_t cells = (uint32_t)(f.w * f.h);
    uint8_t *old = scratch(vm, cells);
    if (!old)
        return 0;
    field_of(vm, a[0], a[1], "cells.life", &f);
    memcpy(old, f.c, cells);
    int32_t changed = 0;
    for (int32_t y = 0; y < f.h; y++)
        for (int32_t x = 0; x < f.w; x++) {
            int n = 0;
            for (int32_t j = -1; j <= 1; j++)
                for (int32_t i = -1; i <= 1; i++) {
                    int32_t nx = x + i, ny = y + j;
                    if ((i || j) && nx >= 0 && ny >= 0 && nx < f.w &&
                        ny < f.h && old[ny * f.w + nx])
                        n++;
                }
            uint8_t v = old[y * f.w + x], to;
            if (v)
                to = survive & (1u << n) ? v : 0;
            else
                to = birth & (1u << n) ? born : 0;
            if (to != v) {
                f.c[y * f.w + x] = to;
                changed++;
            }
        }
    vm_charge(vm, 10 + cells * 3);
    *out = mk(T_INT, changed);
    return 1;
}
