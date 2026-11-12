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
 *   lift       cells.lift
 *   life       cells.life
 ***************************************************************************/
#include <stddef.h>
#include <string.h>
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)

#define MATERIALS 256
#define REACTIONS 64
#define CHUNK_SHIFT 4
#define CHUNK     (1 << CHUNK_SHIFT)
#define FIELD_MAX 256           /* cells across and down */
#define MAX_BOXES 256           /* actors woken round in a step */
#define STEP_UNITS 6            /* a waking cell's tick, as the rig weighs it */

enum { MOVE_STATIC, MOVE_POWDER, MOVE_LIQUID, MOVE_GAS };

/* What a material can do, one byte each for the scan to read at once.
 * K_SAME: with nothing but itself round it, it is stuck -- it has no life
 * span and does not react with itself. */
enum { K_MOVES = 1, K_FLOWS = 2, K_GAS = 4, K_REACTS = 8, K_DECAYS = 16,
       K_SAME = 32 };

/* ---- rules ------------------------------------------------------------- */

enum { R_MOVE, R_SPREAD, R_DENSITY, R_DECAY, R_BECOMES, R_FLAGS,
       R_A, R_B, R_C, R_D, R_CHANCE, R_FIELDS };
static const char *const rule_keys[R_FIELDS] = {
    "move", "spread", "density", "decay", "becomes", "flags",
    "a", "b", "c", "d", "chance"
};

static uint32_t rule_len(int f)
{
    return f < R_A ? MATERIALS : REACTIONS;
}

/* cells.rules(): a record of Uint8Arrays for the cart to fill -- per
 * material move (MOVE.*), spread (how many cells a tick a liquid or gas
 * runs along when it cannot fall; 0 is 2), density,
 * flags (CELL.*, what an actor makes of it), decay (a chance a tick out of
 * 256) and becomes; per reaction a, b, c, d and chance */
NATIVE(cells_rules)
{
    (void)a; (void)argc;
    if (!vm_new_record(vm, rule_keys, R_FIELDS, out))
        return 0;
    vm->rules_shape = OBJ(vm, *out)->shape;
    if (!vm_root(vm, *out))
        return 0;
    for (int f = 0; f < R_FIELDS; f++) {
        uint32_t n = rule_len(f);
        uint32_t off = gc_alloc(vm, T_U8, (uint32_t)(sizeof(struct u8a) -
                                                     sizeof(struct hdr)) + n);
        if (!off) {
            vm_unroot(vm);
            return 0;
        }
        ((struct u8a *)MEM(vm, off))->len = n;
        BUF(vm, OBJ(vm, *out)->buf)->v[f] = mk(T_U8, (int32_t)off);
    }
    vm_unroot(vm);
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
        /* a whole aligned row of the chunk a word at a time */
        if (x1 - x0 == CHUNK && !((uintptr_t)p & 3)) {
            const uint32_t *q = (const uint32_t *)(const void *)p;
            uint32_t a = q[0], b = q[1], c = q[2], d = q[3];
            s1 += a; s2 += s1;
            s1 += b; s2 += s1;
            s1 += c; s2 += s1;
            s1 += d; s2 += s1;
            continue;
        }
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
    old->box[0] = 0;
    old->box[1] = 0;
    old->box[2] = f->w - 1;
    old->box[3] = f->h - 1;
    memset(old->awake, 1, sizeof(old->awake));
    memset(old->dir, 0, sizeof(old->dir));
    return old;
}

/* sl's box of changed cells grown to take in x0..x1, y0..y1 */
static void grow(struct cellsleep *sl, int32_t x0, int32_t y0, int32_t x1,
                 int32_t y1)
{
    if (sl->box[0] > sl->box[2]) {
        sl->box[0] = x0; sl->box[1] = y0; sl->box[2] = x1; sl->box[3] = y1;
        return;
    }
    if (x0 < sl->box[0]) sl->box[0] = x0;
    if (y0 < sl->box[1]) sl->box[1] = y0;
    if (x1 > sl->box[2]) sl->box[2] = x1;
    if (y1 > sl->box[3]) sl->box[3] = y1;
}

/* Cells x0..x1, y0..y1 of field id, w by h, drawn into or written outside
 * cells.step -- particles settling into it -- for the box the next step
 * gives: a chunk awake when drawn on is not summed, so nothing else would
 * see it */
void cells_drawn(struct vm *vm, uint32_t id, int32_t w, int32_t h, int32_t x0,
                 int32_t y0, int32_t x1, int32_t y1)
{
    for (int i = 0; i < 2; i++) {
        struct cellsleep *sl = &vm->sleep[i];
        if (sl->id == id && sl->w == w && sl->h == h)
            grow(sl, x0, y0, x1, y1);
    }
}

/* chunk cx, cy, as cells, into sl's box */
static void grow_chunk(struct cellsleep *sl, int32_t cx, int32_t cy)
{
    int32_t x = cx * CHUNK, y = cy * CHUNK;
    grow(sl, x, y, x + CHUNK - 1 < sl->w - 1 ? x + CHUNK - 1 : sl->w - 1,
         y + CHUNK - 1 < sl->h - 1 ? y + CHUNK - 1 : sl->h - 1);
}

/* A field and its rules, for actors to move in: 0, with the error set,
 * when either is not what it should be */
int cells_world(struct vm *vm, struct value field, struct value rules,
                struct cellworld *w)
{
    struct field f;
    struct rules r;
    if (!field_of(vm, field, mk(T_INT, 0), "actors.move", &f) ||
        !rules_of(vm, rules, &r))
        return 0;
    w->c = f.c;
    w->w = f.w;
    w->h = f.h;
    w->density = r.f[R_DENSITY];
    w->flags = r.f[R_FLAGS];
    w->a = r.f[R_A];
    w->b = r.f[R_B];
    w->c2 = r.f[R_C];
    w->d = r.f[R_D];
    w->chance = r.f[R_CHANCE];
    return 1;
}

/* ---- the step ------------------------------------------------------------ */

struct stepper
{
    struct field f;
    struct rules r;
    uint8_t *moved;             /* a bit a cell: changed this tick */
    uint8_t *dir;               /* a bit a cell: a liquid's way, 1 right */
    uint8_t changed[(FIELD_MAX / CHUNK) * (FIELD_MAX / CHUNK)];
    int32_t cw;                 /* chunks across */
    uint8_t first[MATERIALS];   /* the first reaction for each a, or 255 */
    uint8_t next[REACTIONS];    /* the next with the same a, or 255 */
    uint8_t kind[MATERIALS];    /* K_*: what each material can do */
    uint8_t dens[MATERIALS];    /* the rules' density, right after kind */
    uint8_t slept[(FIELD_MAX / CHUNK) * (FIELD_MAX / CHUNK)];
    struct vm *vm;
    uint32_t bits;              /* random bits left over */
    int nbits;
    uint32_t moves;
    uint32_t steps;             /* cells_step_cell's calls, for the charge */
    uint32_t pad;
    /* for each first reaction, a bit for each partner b of its chain;
     * word 8 stays 0, for off the field */
    uint32_t partner[REACTIONS][16];
};

/* One cell's tick; in cells_arm.S on the players, which reads the stepper
 * at these offsets */
void cells_step_cell(struct stepper *s, int32_t x, int32_t y);
#ifdef ARCADE_ASM
_Static_assert(offsetof(struct stepper, r) == 16 &&
               offsetof(struct stepper, moved) == 60 &&
               offsetof(struct stepper, changed) == 68 &&
               offsetof(struct stepper, cw) == 324 &&
               offsetof(struct stepper, first) == 328 &&
               offsetof(struct stepper, next) == 584 &&
               offsetof(struct stepper, kind) == 648 &&
               offsetof(struct stepper, dir) == 64 &&
               offsetof(struct stepper, vm) == 1416 &&
               offsetof(struct stepper, bits) == 1420 &&
               offsetof(struct stepper, nbits) == 1424 &&
               offsetof(struct stepper, moves) == 1428 &&
               offsetof(struct stepper, steps) == 1432 &&
               offsetof(struct stepper, partner) == 1440,
               "cells_arm.S's stepper offsets");
#else
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

/* Whether x, y is air or a material that moves */
static inline int open(const struct stepper *s, int32_t x, int32_t y)
{
    if (x < 0 || x >= s->f.w)
        return 0;
    uint8_t c = s->f.c[y * s->f.w + x];
    return !c || (s->kind[c] & K_MOVES);
}

static inline int way(const struct stepper *s, int32_t i)
{
    return (s->dir[i >> 3] >> (i & 7)) & 1;
}

static inline void set_way(struct stepper *s, int32_t i, int right)
{
    if (right)
        s->dir[i >> 3] |= (uint8_t)(1 << (i & 7));
    else
        s->dir[i >> 3] &= (uint8_t)~(1 << (i & 7));
}

/* The mover at x, y into tx, ty, and what was there back, each with its
 * way. Only the mover is done for the tick: the cell it left can still
 * be moved into, or the water a grain sank through would wall the next
 * grain in. */
static void swap(struct stepper *s, int32_t x, int32_t y, int32_t tx,
                 int32_t ty)
{
    int32_t i = y * s->f.w + x, j = ty * s->f.w + tx;
    uint8_t m = s->f.c[i];
    int wi = way(s, i), wj = way(s, j);
    s->f.c[i] = s->f.c[j];
    set_way(s, i, wj);
    set_way(s, j, wi);
    busy(s, x, y);
    set(s, tx, ty, m);
    s->moves++;
}

static const int8_t near_x[4] = { -1, 1, 0, 0 }, near_y[4] = { 0, 0, -1, 1 };

/* Whether material c (-1 off the field) is in partner bits pt */
static inline int partner(const uint32_t *pt, int32_t c)
{
    return c >= 0 && (pt[c >> 5] >> (c & 31) & 1);
}

/* The material at x, y, or -1 off the field */
static inline int32_t at(const struct stepper *s, int32_t x, int32_t y)
{
    if (x < 0 || y < 0 || x >= s->f.w || y >= s->f.h)
        return -1;
    return s->f.c[y * s->f.w + x];
}

/* Whether a cell away from the field's edge can do nothing this tick:
 * nothing lighter where it would move, no partner it reacts with beside
 * it, no life span -- most of a pool or a pile, every tick it is awake.
 * Cells already moved are not looked at, which only errs toward trying. */
__attribute__((always_inline))
static inline int stuck(const struct stepper *s, const uint8_t *p, uint8_t m,
                        uint32_t k, int32_t w)
{
    const uint8_t *dens = s->r.f[R_DENSITY];
    const uint8_t dm = dens[m];
    if (k & K_MOVES) {
        int32_t d = k & K_GAS ? -w : w;
        if (dens[p[d]] < dm || dens[p[d - 1]] < dm || dens[p[d + 1]] < dm)
            return 0;
        if ((k & K_FLOWS) && (dens[p[-1]] < dm || dens[p[1]] < dm))
            return 0;
    }
    if (k & K_REACTS)
        for (uint32_t q = s->first[m]; q != 255; q = s->next[q]) {
            uint8_t b = s->r.f[R_B][q];
            if (p[-1] == b || p[1] == b || p[-w] == b || p[w] == b)
                return 0;
        }
    return 1;
}

/* Whether cell x, y has nothing to do this tick: stone, which does not
 * move, end or react, or a cell stuck() inside a pool or a pile. A leaf
 * of its own: inlined in the scan it spills the scan's registers, and the
 * whole step's call costs four times as much. */
__attribute__((noinline, optimize("O2")))
static int idle(const struct stepper *s, int32_t x, int32_t y)
{
    const int32_t w = s->f.w;
    const uint8_t *p = s->f.c + y * w + x;
    uint8_t m = *p;
    uint32_t k = s->kind[m];
    if (!(k & (K_MOVES | K_REACTS | K_DECAYS)))
        return 1;
    if (k & K_DECAYS)
        return 0;
    return x > 0 && y > 0 && x < w - 1 && y < s->f.h - 1 &&
           stuck(s, p, m, k, w);
}

/* One cell's tick: one reaction with a random neighbour, then its life
 * span, then a move -- so fire spreads on the tick it dies. A material
 * with a life span, or with a partner it reacts with beside it, keeps its
 * chunk awake whether or not it changed: its chances come round again. */
__attribute__((optimize("O2")))
void cells_step_cell(struct stepper *s, int32_t x, int32_t y)
{
    s->steps++;
    uint8_t m = s->f.c[y * s->f.w + x];
    const uint8_t *const *r = s->r.f;
    if (r[R_DECAY][m])
        busy(s, x, y);
    if (s->first[m] != 255) {
        const uint32_t *pt = s->partner[s->first[m]];
        uint32_t k = rand8(s) & 3;
        int32_t tx = x + near_x[k], ty = y + near_y[k], b = at(s, tx, ty);
        /* a partner beside it keeps it awake; the one in the chosen
         * place, not yet moved, reacts by chance -- the first reaction
         * with it as b */
        for (int n = 0; n < 4; n++)
            if (partner(pt, at(s, x + near_x[n], y + near_y[n]))) {
                busy(s, x, y);
                break;
            }
        if (partner(pt, b) && !is_moved(s, ty * s->f.w + tx)) {
            uint32_t q = s->first[m];
            while (r[R_B][q] != b)
                q = s->next[q];
            if (rand8(s) < r[R_CHANCE][q]) {
                set(s, x, y, r[R_C][q]);
                set(s, tx, ty, r[R_D][q]);
                return;
            }
        }
    }
    if (r[R_DECAY][m] && rand8(s) < r[R_DECAY][m]) {
        set(s, x, y, r[R_BECOMES][m]);
        return;
    }
    uint8_t mv = r[R_MOVE][m];
    if (mv == MOVE_STATIC)
        return;
    int32_t dy = mv == MOVE_GAS ? -1 : 1, w = s->f.w;
    if (lighter(s, x, y + dy, m)) {
        /* falling, it takes a new way: a stream spreads both ways from
         * where it lands. A liquid or gas drifts a cell that way one tick
         * in four, so a layer left in the air breaks up as it falls. */
        uint32_t k = rand8(s), right = k & 1;
        int32_t tx = right ? x + 1 : x - 1;
        if (mv != MOVE_POWDER && !(k & 6) && open(s, tx, y) &&
            lighter(s, tx, y + dy, m)) {
            swap(s, x, y, tx, y + dy);
            set_way(s, (y + dy) * w + tx, right);
            return;
        }
        swap(s, x, y, x, y + dy);
        set_way(s, (y + dy) * w + x, right);
        return;
    }
    /* not through a corner: the cell beside it must be air or a mover, so
     * a wall drawn on the diagonal holds */
    int32_t side = rand8(s) & 1 ? 1 : -1;
    for (int t = 0; t < 2; t++, side = -side)
        if (open(s, x + side, y) && lighter(s, x + side, y + dy, m)) {
            swap(s, x, y, x + side, y + dy);
            set_way(s, (y + dy) * w + x + side, side > 0);
            return;
        }
    if (mv == MOVE_POWDER)
        return;
    /* Liquids and gases keep a way they are heading, and blocked below
     * they run along it up to spread cells a tick (2 when 0), into the
     * first place on the way they could fall (or rise) into. Blocked
     * along, they turn round; blocked both ways, they stay. So a splash
     * mounds up and runs out, a part-full top layer keeps moving between
     * the walls, and a full layer has nowhere to go and sleeps. Resting on
     * what never moves, or the field's edge, a cell runs only to fall: a
     * film on a floor has nothing to level and sleeps too. */
    int32_t i = y * w + x, d = way(s, i) ? 1 : -1;
    int32_t speed = r[R_SPREAD][m] ? r[R_SPREAD][m] : 2, to = 0;
    for (int32_t k = 1; k <= speed && lighter(s, x + d * k, y, m); k++) {
        to = k;
        if (lighter(s, x + d * k, y + dy, m)) {
            swap(s, x, y, x + d * k, y + dy);
            return;
        }
    }
    int32_t under = at(s, x, y + dy);
    if (under < 0 || (under && !(s->kind[under] & K_MOVES)))
        return;
    if (to) {
        swap(s, x, y, x + d * to, y);
    } else if (lighter(s, x - d, y, m)) {
        set_way(s, i, d < 0);
        busy(s, x, y);
    }
}
#endif

/* Cells x0 to x1 - 1 of row y, left to right or right to left as forward
 * says, each stepped unless idle; air is passed over, four cells a word
 * when the run is in whole aligned words. Returns the cells looked at. */
#ifdef ARCADE_ASM
/* cells_arm.S reads this; the offsets are checked below */
struct run
{
    const uint8_t *c, *moved, *dens;
    int32_t w, h;
    int32_t row, stop;          /* rows row down to stop */
    int32_t runs;               /* each row's runs, from[i] to to[i] - 1 */
    int32_t dx;                 /* 1 or -1 */
    int32_t k;                  /* the assembly's: the run it is on */
    uint32_t span;              /* the row's: cells after x = 1 idle tests */
    const uint8_t *first, *next, *b;
    struct stepper *s;
    int32_t rowbase, y;         /* the row's */
    int32_t from[FIELD_MAX / CHUNK], to[FIELD_MAX / CHUNK];
};
_Static_assert(offsetof(struct run, span) == 40 &&
               offsetof(struct run, y) == 64 &&
               offsetof(struct run, from) == 68 &&
               offsetof(struct run, to) == 132, "cells_arm.S's offsets");
_Static_assert(offsetof(struct stepper, dens) ==
               offsetof(struct stepper, kind) + MATERIALS,
               "cells_arm.S finds kind 256 bytes before the density");
uint32_t cells_run_arm(struct run *r);
#else
__attribute__((optimize("O2")))
static uint32_t run(struct stepper *s, int32_t y, int32_t x0, int32_t x1,
                    int forward)
{
    const int32_t w = s->f.w, n = x1 - x0;
    const uint8_t *row = s->f.c + y * w;
    int words = !((uintptr_t)(row + x0) & 3) && !(n & 3);
    int32_t g = words ? 4 : n;
    uint32_t visited = 0;
    for (int32_t q = 0; q < n; q += g) {
        int32_t b = forward ? x0 + q : x1 - g - q;
        if (words && !*(const uint32_t *)(const void *)(row + b))
            continue;
        for (int32_t e = 0; e < g; e++) {
            int32_t x = forward ? b + e : b + g - 1 - e;
            if (!row[x] || is_moved(s, y * w + x))
                continue;
            visited++;
            if (!idle(s, x, y))
                cells_step_cell(s, x, y);
        }
    }
    return visited;
}
#endif

/* Every awake chunk's cells, bottom up, a row left to right or right to
 * left as forward says, side by side awake chunks as one run. Returns the
 * cells looked at. */
/* A chunk row's runs of side by side awake chunks, in the order the scan
 * meets them, as cells from[i] to to[i] - 1. Returns how many. */
static int32_t runs_of(const struct stepper *s, const uint8_t *chunks,
                       int forward, int32_t *from, int32_t *to)
{
    const int32_t w = s->f.w, cw = s->cw;
    int32_t runs = 0;
    for (int32_t k = 0; k < cw; k++) {
        if (!chunks[forward ? k : cw - 1 - k])
            continue;
        int32_t e = k;
        while (e + 1 < cw && chunks[forward ? e + 1 : cw - 2 - e])
            e++;
        int32_t lo = forward ? k : cw - 1 - e;
        int32_t hi = forward ? e : cw - 1 - k;
        int32_t x1 = (hi + 1) << CHUNK_SHIFT;
        from[runs] = lo << CHUNK_SHIFT;
        to[runs++] = x1 < w ? x1 : w;
        k = e;
    }
    return runs;
}

static uint32_t scan(struct stepper *s, const uint8_t *awake, int forward)
{
    uint32_t visited = 0;
    const int32_t h = s->f.h;
#ifdef ARCADE_ASM
    /* a chunk row's rows in one call */
    static struct run r;
    r.c = s->f.c;
    r.moved = s->moved;
    r.dens = s->dens;
    r.w = s->f.w;
    r.h = h;
    r.dx = forward ? 1 : -1;
    r.first = s->first;
    r.next = s->next;
    r.b = s->r.f[R_B];
    r.s = s;
    for (int32_t at = (h - 1) >> CHUNK_SHIFT; at >= 0; at--) {
        r.runs = runs_of(s, awake + at * s->cw, forward, r.from, r.to);
        if (!r.runs)
            continue;
        r.stop = at << CHUNK_SHIFT;
        r.row = r.stop + CHUNK - 1 < h - 1 ? r.stop + CHUNK - 1 : h - 1;
        visited += cells_run_arm(&r);
    }
#else
    int32_t from[FIELD_MAX / CHUNK], to[FIELD_MAX / CHUNK];
    for (int32_t at = (h - 1) >> CHUNK_SHIFT; at >= 0; at--) {
        int32_t runs = runs_of(s, awake + at * s->cw, forward, from, to);
        int32_t stop = at << CHUNK_SHIFT;
        for (int32_t y = stop + CHUNK - 1 < h - 1 ? stop + CHUNK - 1 : h - 1;
             y >= stop; y--)
            for (int32_t i = 0; i < runs; i++)
                visited += run(s, y, from[i], to[i], forward);
    }
#endif
    return visited;
}

/* cells.step(field, w, rules, [actors]): one tick of the field -- an
 * image, or a Uint8Array board w wide -- by the rules. actors, a pool or
 * an array of pools, are obstacles while it steps: their boxes are marked
 * as cells already moved, which nothing moves into, so water flows round
 * a character and sand piles on a moving platform; the chunks under them
 * are woken. With out, an Int32Array of 4: the box x0, y0, x1, y1 round
 * what changed since the last step -- whole chunks stepped, whole rows
 * drawn on, and cells particles settled into -- left as it was when
 * nothing did.
 * Returns the cells that moved. */
NATIVE(cells_step)
{
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
    memcpy(s.dens, s.r.f[R_DENSITY], MATERIALS);
    static int32_t boxes[MAX_BOXES * 4];
    int32_t nbox = 0;
    if (argc > 3 && a[3].tag != T_NULL &&
        (nbox = actors_boxes(vm, a[3], s.moved, s.f.w, s.f.h, boxes,
                             MAX_BOXES)) < 0)
        return 0;
    s.vm = vm;
    s.nbits = 0;
    s.moves = 0;
    s.steps = 0;
    s.cw = (s.f.w + CHUNK - 1) / CHUNK;
    int32_t chh = (s.f.h + CHUNK - 1) / CHUNK;
    memset(s.changed, 0, sizeof(s.changed));
    memset(s.first, 255, sizeof(s.first));
    for (int32_t q = REACTIONS - 1; q >= 0; q--)
        if (s.r.f[R_CHANCE][q]) {
            s.next[q] = s.first[s.r.f[R_A][q]];
            s.first[s.r.f[R_A][q]] = (uint8_t)q;
        }
    /* how far a liquid runs in a tick, in chunks, for the wake below */
    int32_t across = 1;
    for (int32_t m = 0; m < MATERIALS; m++) {
        static const uint8_t moves[4] = {
            0, K_MOVES, K_MOVES | K_FLOWS, K_MOVES | K_FLOWS | K_GAS
        };
        uint8_t mv = s.r.f[R_MOVE][m];
        uint8_t k = mv <= MOVE_GAS ? moves[mv] : 0;
        if (k & K_FLOWS) {
            int32_t spread = s.r.f[R_SPREAD][m];
            int32_t n = (spread ? spread : 2) / CHUNK + 1;
            if (n > across)
                across = n;
        }
        if (s.first[m] != 255) {
            uint32_t *pt = s.partner[s.first[m]];
            k |= K_REACTS;
            memset(pt, 0, sizeof(s.partner[0]));
            for (uint32_t q = s.first[m]; q != 255; q = s.next[q])
                pt[s.r.f[R_B][q] >> 5] |= 1u << (s.r.f[R_B][q] & 31);
        }
        k |= s.r.f[R_DECAY][m] ? K_DECAYS : K_SAME;
        s.kind[m] = k;
    }
    for (uint32_t q = 0; q < REACTIONS; q++)
        if (s.r.f[R_CHANCE][q] && s.r.f[R_A][q] == s.r.f[R_B][q])
            s.kind[s.r.f[R_A][q]] &= (uint8_t)~K_SAME;
    struct cellsleep *sl = sleep_for(vm, &s.f);
    s.dir = sl->dir;
    /* a sleeping chunk drawn on since wakes */
    uint32_t summed = 0;
    for (int32_t cy = 0, k = 0; cy < chh; cy++)
        for (int32_t cx = 0; cx < s.cw; cx++, k++) {
            s.slept[k] = 0;
            if (!sl->awake[k]) {
                summed++;
                if (chunk_sum(&s.f, cx, cy) != sl->sum[k]) {
                    sl->awake[k] = 1;
                    grow_chunk(sl, cx, cy);
                } else
                    s.slept[k] = 1;
            }
        }
    /* the chunks round each actor are awake: something may rest on it */
    for (int32_t b = 0; b < nbox; b++)
        for (int32_t cy = (boxes[b * 4 + 1] >> CHUNK_SHIFT) - 1;
             cy <= (boxes[b * 4 + 3] >> CHUNK_SHIFT) + 1; cy++)
            for (int32_t cx = (boxes[b * 4] >> CHUNK_SHIFT) - 1;
                 cx <= (boxes[b * 4 + 2] >> CHUNK_SHIFT) + 1; cx++)
                if (cx >= 0 && cy >= 0 && cx < s.cw && cy < chh)
                    sl->awake[cy * s.cw + cx] = 1;
    uint32_t visited = scan(&s, sl->awake, vm->frame & 1);
    /* the box of what changed: the chunks this step changed, those drawn
     * on since, and cells particles settled into; given, and begun again */
    for (int32_t cy = 0, k = 0; cy < chh; cy++)
        for (int32_t cx = 0; cx < s.cw; cx++, k++)
            if (s.changed[k])
                grow_chunk(sl, cx, cy);
    if (argc > 4 && a[4].tag != T_NULL) {
        if (a[4].tag != T_I32 || I32(vm, a[4])->len < 4)
            return vm_fail(vm, "cells.step() writes its box to an Int32Array "
                           "of 4", "", "");
        if (sl->box[0] <= sl->box[2])
            memcpy(I32(vm, a[4])->d, sl->box, sizeof(sl->box));
    }
    sl->box[0] = 1;
    sl->box[2] = 0;
    /* and next tick the chunks round each actor, as changed */
    for (int32_t b = 0; b < nbox; b++)
        for (int32_t cy = (boxes[b * 4 + 1] >> CHUNK_SHIFT) - 1;
             cy <= (boxes[b * 4 + 3] >> CHUNK_SHIFT) + 1; cy++)
            for (int32_t cx = (boxes[b * 4] >> CHUNK_SHIFT) - 1;
                 cx <= (boxes[b * 4 + 2] >> CHUNK_SHIFT) + 1; cx++)
                if (cx >= 0 && cy >= 0 && cx < s.cw && cy < chh)
                    s.changed[cy * s.cw + cx] = 1;
    /* Next tick: the chunks that changed and those near them; the rest
     * sleep with their sums. Near is a chunk up or down, and sideways as
     * far as a liquid runs in a tick. */
    uint16_t band[FIELD_MAX / CHUNK + 1];
    band[0] = 0;
    for (int32_t k = 0, cx = 0, cy = 0; k < s.cw * chh;
         k++, cx = cx + 1 < s.cw ? cx + 1 : (cy++, 0)) {
        int32_t wake;
        /* each chunk row's band (the row and those above and below) as a
         * running count of changed chunks along it, once a row */
        if (cx == 0) {
            int32_t n = 0;
            for (int32_t i = 0; i < s.cw; i++) {
                for (int32_t j = cy - 1; j <= cy + 1; j++)
                    if (j >= 0 && j < chh && s.changed[j * s.cw + i]) {
                        n++;
                        break;
                    }
                band[i + 1] = (uint16_t)n;
            }
        }
        int32_t lo = cx - across < 0 ? 0 : cx - across;
        int32_t hi = cx + across >= s.cw ? s.cw - 1 : cx + across;
        wake = band[hi + 1] != band[lo];
        sl->awake[k] = (uint8_t)wake;
        /* a chunk that slept and nothing moved into keeps its sum */
        if (!wake && (!s.slept[k] || s.changed[k])) {
            sl->sum[k] = chunk_sum(&s.f, cx, cy);
            summed++;
        }
    }
    /* measured on the ARM rig (.build/arcade-rig, sysprobe): the parts
     * as they were weighed, three and a half times over */
    vm_charge(vm, (20 + visited + s.moves + summed * 7) * 7 / 2 +
                  s.steps * STEP_UNITS);
    *out = mk(T_INT, (int32_t)s.moves);
    return 1;
}

/* ---- lift ---------------------------------------------------------------- */

/* cells.lift(field, rules, x, y, r, pool, [speed]): the loose cells within
 * r of field cell x, y -- any that move, powder, liquid or gas; stone
 * stays -- thrown out as particles of their colour, outward from x, y at
 * up to speed (2) pixels a tick, the cells left as air. With the pool's
 * field set to this one they fall back in as cells: a blast that throws
 * sand and water. Returns the cells lifted. */
NATIVE(cells_lift)
{
    struct field f;
    struct rules r;
    struct value *p[PARTICLE_ARRAYS];
    uint32_t n;
    int32_t ox, oy;
    if (!field_of(vm, a[0], mk(T_INT, 0), "cells.lift", &f) ||
        !rules_of(vm, a[1], &r) ||
        !particles_arrays(vm, a[5], p, &n, &ox, &oy))
        return 0;
    if (!is_num(a[2]) || !is_num(a[3]) || !is_num(a[4]))
        return vm_fail(vm, "cells.lift() needs x, y and a radius", "", "");
    int32_t x = floor_int(a[2]), y = floor_int(a[3]), rad = floor_int(a[4]);
    int32_t speed = argc > 6 && is_num(a[6]) ? (int32_t)scaled(a[6]) : 2 << 16;
    if (rad < 0) rad = 0;
    if (rad > 32) rad = 32;
    uint32_t slot = 0, lifted = 0;
    for (int32_t dy = -rad; dy <= rad; dy++)
        for (int32_t dx = -rad; dx <= rad; dx++) {
            int32_t cx = x + dx, cy = y + dy;
            if (dx * dx + dy * dy > rad * rad || cx < 0 || cy < 0 ||
                cx >= f.w || cy >= f.h)
                continue;
            uint8_t m = f.c[cy * f.w + cx];
            if (!m || r.f[R_MOVE][m] == MOVE_STATIC)
                continue;
            while (slot < n && is_num(p[0][slot]) && floor_int(p[0][slot]) > 0)
                slot++;
            if (slot >= n)
                goto full;
            /* outward from the centre, faster nearer it, and up a little */
            int32_t d = dx * dx + dy * dy, near = rad * rad - d + 1;
            int32_t k = (int32_t)(((int64_t)speed * near) / (rad * rad + 1));
            int32_t len = 1;
            while (len * len < d)
                len++;
            p[0][slot] = mk(T_INT, 40 + (int32_t)(core_random(vm) & 31));
            p[1][slot] = mk(T_INT, cx + ox);
            p[2][slot] = mk(T_INT, cy + oy);
            p[3][slot] = mk(T_FIX, d ? k * dx / len : 0);
            p[4][slot] = mk(T_FIX, (d ? k * dy / len : 0) - speed / 2);
            p[5][slot] = mk(T_INT, m);
            p[6][slot] = mk(T_INT, 0);  /* a pixel, not a sprite */
            p[7][slot] = mk(T_INT, 0);
            f.c[cy * f.w + cx] = 0;
            lifted++;
        }
full:
    vm_charge(vm, 10 + (uint32_t)((2 * rad + 1) * (2 * rad + 1)) / 2 +
              lifted * 4);
    *out = mk(T_INT, (int32_t)lifted);
    return 1;
}

/* ---- life ---------------------------------------------------------------- */

/* One generation over w x h: live[] holds 1 for each live cell of the
 * last, c[] is changed where a cell is born or dies. A row at a time,
 * each column's three cells summed once into sums[], so a cell's count
 * is three sums less itself rather than eight tests at the edges.
 * Returns the cells changed. */
__attribute__((optimize("O2")))
static int32_t life_rows(uint8_t *c, const uint8_t *live, int32_t w,
                         int32_t h, uint32_t birth, uint32_t survive,
                         uint8_t born)
{
    static uint8_t sums[FIELD_MAX + 2];
    int32_t changed = 0;
    sums[0] = 0;
    sums[w + 1] = 0;
    for (int32_t y = 0; y < h; y++) {
        const uint8_t *row = live + y * w;
        const uint8_t *up = y > 0 ? row - w : NULL;
        const uint8_t *down = y < h - 1 ? row + w : NULL;
        for (int32_t x = 0; x < w; x++)
            sums[x + 1] = (uint8_t)(row[x] + (up ? up[x] : 0) +
                                    (down ? down[x] : 0));
        uint8_t *out = c + y * w;
        for (int32_t x = 0; x < w; x++) {
            uint32_t n = (uint32_t)(sums[x] + sums[x + 1] + sums[x + 2] -
                                    row[x]);
            uint8_t v = out[x];
            uint8_t to = v ? (survive >> n & 1 ? v : 0)
                           : (birth >> n & 1 ? born : 0);
            if (to != v) {
                out[x] = to;
                changed++;
            }
        }
    }
    return changed;
}

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
    uint8_t *live = scratch(vm, cells);
    if (!live)
        return 0;
    field_of(vm, a[0], a[1], "cells.life", &f);   /* the scratch may move it */
    for (uint32_t i = 0; i < cells; i++)
        live[i] = f.c[i] != 0;
    int32_t changed = life_rows(f.c, live, f.w, f.h, birth, survive, born);
    /* measured on the ARM rig (.build/arcade-rig, sysprobe) */
    vm_charge(vm, 10 + cells * 7 / 4);
    *out = mk(T_INT, changed);
    return 1;
}
