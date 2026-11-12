/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade core: maths, random numbers, time, saved values and the console.
 *
 * Numbers are integers or fractions (16.16); vm.h has the rules for
 * mixing them. Angles are turns, as in PICO-8, whose meanings these keep:
 * sin is inverted, sgn(0) is 1. A function that wants a whole number -- an
 * index, a count -- takes a fraction rounded down. The trig tables are
 * integers, so every target gives the same answer.
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"
#include "games/arcade/core_trig.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)

static int bad_arg(struct vm *vm, const char *fn, struct value v)
{
    return vm_fail(vm, fn, "() needs a number, not a ", vm_typename(v.tag));
}

/* A whole-number argument: a fraction is rounded down. */
static int whole(struct vm *vm, struct value *a, int i, const char *fn,
                 int32_t *x)
{
    *x = floor_int(a[i]);
    return is_num(a[i]) ? 1 : bad_arg(vm, fn, a[i]);
}

/* Any number, as 16.16 in 64 bits. */
static int real(struct vm *vm, struct value *a, int i, const char *fn,
                int64_t *x)
{
    *x = scaled(a[i]);
    return is_num(a[i]) ? 1 : bad_arg(vm, fn, a[i]);
}

#define WHOLE(i, x, fn) int32_t x; if (!whole(vm, a, i, fn, &x)) return 0
#define REAL(i, x, fn) int64_t x; if (!real(vm, a, i, fn, &x)) return 0
#define NUMBER(i, fn) if (!is_num(a[i])) return bad_arg(vm, fn, a[i])
#define RET_INT(v) (*out = mk(T_INT, (int32_t)(v)), 1)
#define RET_FIX(v) (*out = mk(T_FIX, wrap32(v)), 1)

/* ---- random: PCG32, seeded the same way everywhere ------------------- */

/* PCG32's stream: one, for rnd() and every rng generator alike */
#define RNG_INC ((0xda3e39cb94b95bdbull << 1) | 1)

uint32_t core_random(struct vm *vm)
{
    uint64_t old = vm->rng_state;
    vm->rng_state = old * 6364136223846793005ull + RNG_INC;
    uint32_t x = (uint32_t)(((old >> 18) ^ old) >> 27);
    uint32_t r = (uint32_t)(old >> 59);
    return (x >> r) | (x << ((0u - r) & 31));
}

void core_seed(struct vm *vm, uint64_t seed)
{
    vm->rng_state = 0;
    core_random(vm);
    vm->rng_state += seed;
    core_random(vm);
}

/* 0 .. n-1, without modulo bias. */
static uint32_t below(struct vm *vm, uint32_t n)
{
    uint32_t floor = (0u - n) % n, r;
    do
        r = core_random(vm);
    while (r < floor);
    return r % n;
}

/* rnd(): a fraction below 1. rnd(n): an integer below n, or a fraction
 * below n when n is one. rnd(array): an element. */
static int draw(struct vm *vm, struct value *a, int argc,
                struct value *out, const char *fn)
{
    if (argc == 0)
        return RET_FIX(core_random(vm) & 0xffff);
    if (a[0].tag == T_ARR) {
        struct arr *p = ARR(vm, a[0]);
        *out = p->len ? BUF(vm, p->buf)->v[below(vm, p->len)] : mk(T_NULL, 0);
        return 1;
    }
    NUMBER(0, fn);
    if (a[0].tag == T_FIX)
        return RET_FIX(a[0].v <= 0 ? 0 : below(vm, (uint32_t)a[0].v));
    return RET_INT(a[0].v <= 0 ? 0 : below(vm, (uint32_t)a[0].v));
}

NATIVE(rnd)
{
    return draw(vm, a, argc, out, "rnd");
}

/* A generator's state as the cart keeps it: an Int32Array of 2, the
 * state's high and low halves */
static int state_arg(struct vm *vm, struct value v, const char *fn)
{
    if (v.tag != T_I32 || I32(vm, v)->len < 2)
        return vm_fail(vm, fn, "() needs an Int32Array of 2 for the state",
                       "");
    return 1;
}

static uint64_t state_of(struct vm *vm, struct value v)
{
    const int32_t *d = I32(vm, v)->d;
    return (uint64_t)(uint32_t)d[0] << 32 | (uint32_t)d[1];
}

static void state_to(struct vm *vm, struct value v, uint64_t s)
{
    int32_t *d = I32(vm, v)->d;
    d[0] = (int32_t)(uint32_t)(s >> 32);
    d[1] = (int32_t)(uint32_t)s;
}

/* srand(seed), or srand(state) to put back what rndstate() kept */
NATIVE(srand)
{
    (void)argc; (void)out;
    if (a[0].tag == T_I32) {
        if (!state_arg(vm, a[0], "srand"))
            return 0;
        vm->rng_state = state_of(vm, a[0]);
        return 1;
    }
    NUMBER(0, "srand");
    core_seed(vm, (uint32_t)a[0].v);
    return 1;
}

/* rndstate(out): rnd()'s state into an Int32Array of 2, for srand(out) */
NATIVE(rndstate)
{
    (void)argc; (void)out;
    if (!state_arg(vm, a[0], "rndstate"))
        return 0;
    state_to(vm, a[0], vm->rng_state);
    return 1;
}

/* rng.seed(g, seed): a generator of its own in g, an Int32Array of 2, as
 * srand(seed) seeds rnd(); rnd()'s own stream untouched */
NATIVE(rng_seed)
{
    (void)argc; (void)out;
    if (!state_arg(vm, a[0], "rng.seed"))
        return 0;
    NUMBER(1, "rng.seed");
    uint64_t keep = vm->rng_state;
    core_seed(vm, (uint32_t)a[1].v);
    state_to(vm, a[0], vm->rng_state);
    vm->rng_state = keep;
    return 1;
}

/* rng.rnd(g, [x]): as rnd(x), from generator g, which moves on */
NATIVE(rng_rnd)
{
    if (!state_arg(vm, a[0], "rng.rnd"))
        return 0;
    uint64_t keep = vm->rng_state;
    vm->rng_state = state_of(vm, a[0]);
    int ok = draw(vm, a + 1, argc - 1, out, "rng.rnd");
    state_to(vm, a[0], vm->rng_state);
    vm->rng_state = keep;
    return ok;
}

/* ---- arithmetic -------------------------------------------------------- */

NATIVE(flr)
{
    (void)argc;
    NUMBER(0, "flr");
    return RET_INT(floor_int(a[0]));
}

NATIVE(ceil)
{
    (void)argc;
    NUMBER(0, "ceil");
    if (a[0].tag == T_INT)
        return RET_INT(a[0].v);
    return RET_INT(wrap32(((int64_t)a[0].v + 65535) >> 16));
}

NATIVE(abs)
{
    (void)argc;
    NUMBER(0, "abs");
    int32_t v = a[0].v;
    *out = mk(a[0].tag, v < 0 ? (int32_t)(0u - (uint32_t)v) : v);
    return 1;
}

/* min, max and mid return one of their arguments, unchanged. */
NATIVE(min)
{
    (void)argc;
    NUMBER(0, "min");
    NUMBER(1, "min");
    *out = scaled(a[1]) < scaled(a[0]) ? a[1] : a[0];
    return 1;
}

NATIVE(max)
{
    (void)argc;
    NUMBER(0, "max");
    NUMBER(1, "max");
    *out = scaled(a[1]) > scaled(a[0]) ? a[1] : a[0];
    return 1;
}

NATIVE(mid)
{
    (void)argc;
    NUMBER(0, "mid");
    NUMBER(1, "mid");
    NUMBER(2, "mid");
    struct value x = a[0], y = a[1], z = a[2], t;
    if (scaled(x) > scaled(y)) {
        t = x;
        x = y;
        y = t;
    }
    if (scaled(y) > scaled(z))
        y = z;
    *out = scaled(x) > scaled(y) ? x : y;
    return 1;
}

NATIVE(sgn)
{
    (void)argc;
    NUMBER(0, "sgn");
    return RET_INT(a[0].v < 0 ? -1 : 1);
}

static uint64_t isqrt(uint64_t v)
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

/* sqrt of an integer is an integer, rounded down; of a fraction, a
 * fraction. */
NATIVE(sqrt)
{
    (void)argc;
    NUMBER(0, "sqrt");
    if (a[0].v <= 0)
        return a[0].tag == T_FIX ? RET_FIX(0) : RET_INT(0);
    if (a[0].tag == T_INT)
        return RET_INT(isqrt((uint64_t)a[0].v));
    return RET_FIX(isqrt((uint64_t)a[0].v << 16));
}

/* ---- trigonometry ------------------------------------------------------ */

/* The quarter-wave table at j, 0..0x4000, interpolated in 16ths. */
static int32_t quarter(uint32_t j)
{
    uint32_t i = j >> 4, f = j & 15;
    if (!f)
        return sine_q[i];
    return sine_q[i] + (((sine_q[i + 1] - sine_q[i]) * (int32_t)f) >> 4);
}

/* sin(2 pi t) in 16.16, for t in 1/65536 turns: the plane's too. */
int32_t core_sine(uint32_t t)
{
    t &= 0xffff;
    uint32_t i = t & 0x3fff;
    switch (t >> 14) {
    case 0: return quarter(i);
    case 1: return quarter(0x4000 - i);
    case 2: return -quarter(i);
    }
    return -quarter(0x4000 - i);
}

NATIVE(sin)
{
    (void)argc;
    REAL(0, t, "sin");
    return RET_FIX(-core_sine((uint32_t)t));
}

NATIVE(cos)
{
    (void)argc;
    REAL(0, t, "cos");
    return RET_FIX(core_sine((uint32_t)t + 0x4000));
}

/* The angle of (dx, -dy) in turns, 0 to just under 1, so that y down the
 * screen turns the same way as sin's inversion. */
NATIVE(atan2)
{
    (void)argc;
    REAL(0, x, "atan2");
    REAL(1, dy, "atan2");
    int64_t y = -dy;
    if (!x && !y)
        return RET_FIX(0);
    uint64_t ax = x < 0 ? (uint64_t)-x : (uint64_t)x;
    uint64_t ay = y < 0 ? (uint64_t)-y : (uint64_t)y;
    uint64_t lo = ax >= ay ? ay : ax, hi = ax >= ay ? ax : ay;
    while (hi >= (1ull << 40)) {
        hi >>= 1;
        lo >>= 1;
    }
    uint32_t r = (uint32_t)((lo << 16) / hi);
    uint32_t i = r >> 8, f = r & 255;
    int32_t o = atan_o[i];
    if (f)
        o += ((atan_o[i + 1] - atan_o[i]) * (int32_t)f) >> 8;
    if (ay > ax)
        o = 16384 - o;
    if (x < 0)
        o = 32768 - o;
    if (y < 0)
        o = 65536 - o;
    return RET_FIX(o & 0xffff);
}

/* ---- time, saving, the console ---------------------------------------- */

/* Seconds since the cart started, as a fraction. */
NATIVE(t)
{
    (void)a; (void)argc;
    return RET_FIX(vm->t_base + (int64_t)((uint64_t)(vm->frame - vm->frame_base)
                                          * 65536 / vm->rate));
}

NATIVE(frame)
{
    (void)a; (void)argc;
    return RET_INT(vm->frame);
}

NATIVE(date)
{
    (void)argc; (void)out;
    int32_t d[7] = { 0 };
    if (a[0].tag != T_I32 || I32(vm, a[0])->len < 7)
        return vm_fail(vm, "date() needs an Int32Array of 7", "", "");
    if (vm->host && vm->host->date)
        vm->host->date(vm->host->ctx, d);
    memcpy(I32(vm, a[0])->d, d, sizeof(d));
    return 1;
}

NATIVE(dget)
{
    (void)argc;
    WHOLE(0, i, "dget");
    if ((uint32_t)i >= 256)
        return RET_INT(0);
    *out = mk(vm->save_frac[i >> 3] & (1 << (i & 7)) ? T_FIX : T_INT,
              vm->save[i]);
    return 1;
}

NATIVE(dset)
{
    (void)argc; (void)out;
    WHOLE(0, i, "dset");
    NUMBER(1, "dset");
    if ((uint32_t)i >= 256)
        return 1;
    uint8_t bit = (uint8_t)(1 << (i & 7));
    uint8_t frac = a[1].tag == T_FIX ? bit : 0;
    uint8_t *f = &vm->save_frac[i >> 3];
    if (vm->save[i] != a[1].v || (*f & bit) != frac) {
        vm->save[i] = a[1].v;
        *f = (uint8_t)((*f & ~bit) | frac);
        vm->save_dirty = 1;
    }
    return 1;
}

/* a[0] is the count of pieces that follow. */
NATIVE(printh)
{
    static char line[256];
    uint32_t n = 0;
    (void)out;
    if (!vm_parts(vm, a, argc, "printh"))
        return 0;
    int parts = a[0].v;
    for (int i = 1; i <= parts && i < argc; i++)
        n += vm_text(vm, a[i], line + n, sizeof(line) - n);
    if (vm->host && vm->host->print)
        vm->host->print(vm->host->ctx, line, (int)n);
    return 1;
}

/* IDE tools, passed to the host's hooks; the player has none. */
NATIVE(watch)
{
    static char name[64], value[128];
    (void)argc; (void)out;
    if (!vm->host || !vm->host->watch)
        return 1;
    uint32_t n = vm_text(vm, a[0], name, sizeof(name) - 1);
    name[n] = 0;
    n = vm_text(vm, a[1], value, sizeof(value) - 1);
    value[n] = 0;
    vm->host->watch(vm->host->ctx, name, value);
    return 1;
}

NATIVE(prof)
{
    static char name[64];
    (void)out;
    if (!vm->host || !vm->host->prof)
        return 1;
    if (argc < 1 || a[0].tag == T_NULL) {
        vm->host->prof(vm->host->ctx, NULL);
        return 1;
    }
    uint32_t n = vm_text(vm, a[0], name, sizeof(name) - 1);
    name[n] = 0;
    vm->host->prof(vm->host->ctx, name);
    return 1;
}
