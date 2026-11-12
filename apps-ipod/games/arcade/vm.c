/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: the interpreter.
 *
 * Parts, in order:
 *   errors and text    vm_fail, type names, values as text
 *   strings            making, joining, comparing
 *   arrays, records    growth; record keys through shapes, and the
 *                      inline caches that make o.x a compare and a load
 *   indexing           a[i] and o[k], read and written
 *   methods            the built-in methods, and the intrinsics
 *   calls              frames, and calling a cart function from C
 *   run()              the dispatch loop
 *   the host's calls   arcade_start, arcade_tick and the rest
 *
 * Values on the VM stack are the roots the collector sees, so anything
 * that allocates is called with the stack synced (SYNC) and its operands
 * still on it.
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"


/* ---- errors and text ---------------------------------------------------- */

static int fail5(struct vm *vm, const char *a, const char *b, const char *c,
                 const char *d, const char *e)
{
    if (vm->err)
        return 0;
    vm->err = 1;
    uint32_t n = 0, cap = sizeof(vm->msg) - 1;
    const char *parts[5] = { a, b, c, d, e };
    for (int i = 0; i < 5; i++)
        for (const char *p = parts[i]; *p && n < cap; p++)
            vm->msg[n++] = *p;
    vm->msg[n] = 0;
    return 0;
}

int vm_fail(struct vm *vm, const char *a, const char *b, const char *c)
{
    return fail5(vm, a, b, c, "", "");
}

const char *vm_typename(uint32_t tag)
{
    static const char *const names[] = {
        "null", "number", "string", "array", "Int32Array", "Uint8Array",
        "record", "function", "buffer", "number", "image", "FixArray",
        "lazy image not yet loaded with img.load()"
    };
    return tag <= T_LAZY ? names[tag] : "?";
}

int vm_truthy(struct vm *vm, struct value v)
{
    switch (v.tag) {
    case T_NULL: return 0;
    case T_INT: case T_FIX: return v.v != 0;
    case T_STR: return STR(vm, v)->len != 0;
    }
    return 1;
}

static uint32_t decimal(int32_t v, char *b)
{
    char t[12];
    uint32_t n = 0, u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v, k = 0;
    do {
        t[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0)
        b[k++] = '-';
    while (n)
        b[k++] = t[--n];
    return k;
}

/* A fraction to four decimal places, rounded, trailing zeros dropped. */
static uint32_t fix_text(int32_t raw, char *b)
{
    int neg = raw < 0;
    uint64_t m = neg ? (uint64_t)(-(int64_t)raw) : (uint64_t)raw;
    uint32_t ip = (uint32_t)(m >> 16), k = 0;
    uint32_t d = (uint32_t)(((m & 0xffff) * 10000 + 32768) >> 16);
    if (d == 10000) {
        ip++;
        d = 0;
    }
    if (neg && (ip || d))
        b[k++] = '-';
    k += decimal((int32_t)ip, b + k);
    if (d) {
        char t[4];
        int n = 4;
        for (int i = 3; i >= 0; i--, d /= 10)
            t[i] = (char)('0' + d % 10);
        while (t[n - 1] == '0')
            n--;
        b[k++] = '.';
        for (int i = 0; i < n; i++)
            b[k++] = t[i];
    }
    return k;
}

/* Text is written through a bounded writer that counts everything, so
 * one routine both measures and writes. The length saturates, and an
 * array nested in itself stops after TW_MOST elements. */
struct tw
{
    uint8_t *p, *end;
    uint32_t n;
    uint32_t seen;                  /* elements visited */
    int cut;                        /* stop once the buffer is full */
};

/* whether the walk can stop: a cut text is full */
#define TW_DONE(w) ((w)->cut && (w)->p == (w)->end)

#define TW_MOST (1u << 18)

static void tw_put(struct tw *w, const void *b, uint32_t n)
{
    uint32_t room = (uint32_t)(w->end - w->p), k = n < room ? n : room;
    if (k) {
        memcpy(w->p, b, k);
        w->p += k;
    }
    w->n = n > 0xffffffffu - w->n ? 0xffffffffu : w->n + n;
}

/* The charge for a text measured, before it is made */
static int tw_spend(struct vm *vm, const struct tw *w)
{
    if (w->seen > TW_MOST)
        return vm_fail(vm, "text of too many elements", "", "");
    return vm_spend(vm, w->seen + w->n / BYTES_PER_UNIT);
}

static void tw_value(struct vm *vm, struct tw *w, struct value v, int depth);

/* An array's elements joined by sep; null elements are empty, as JS's. */
static void tw_join(struct vm *vm, struct tw *w, struct value v,
                    const uint8_t *sep, uint32_t seplen, int depth)
{
    char num[12];
    if (depth > 4) {
        tw_put(w, "...", 3);
        return;
    }
    if (w->seen > TW_MOST)
        return;
    if (v.tag == T_ARR) {
        struct arr *a = ARR(vm, v);
        w->seen += a->len;
        for (uint32_t i = 0; i < a->len && w->seen <= TW_MOST &&
                             !TW_DONE(w); i++) {
            if (i)
                tw_put(w, sep, seplen);
            struct value e = BUF(vm, a->buf)->v[i];
            if (e.tag != T_NULL)
                tw_value(vm, w, e, depth + 1);
        }
    } else if (v.tag == T_I32) {
        struct i32 *a = I32(vm, v);
        w->seen += a->len;
        for (uint32_t i = 0; i < a->len && !TW_DONE(w); i++) {
            if (i)
                tw_put(w, sep, seplen);
            tw_put(w, num, decimal(a->d[i], num));
        }
    } else if (v.tag == T_FXA) {
        struct i32 *a = I32(vm, v);
        char f[24];
        w->seen += a->len;
        for (uint32_t i = 0; i < a->len && !TW_DONE(w); i++) {
            if (i)
                tw_put(w, sep, seplen);
            tw_put(w, f, fix_text(a->d[i], f));
        }
    } else {
        struct u8a *a = U8A(vm, v);
        w->seen += a->len;
        for (uint32_t i = 0; i < a->len && !TW_DONE(w); i++) {
            if (i)
                tw_put(w, sep, seplen);
            tw_put(w, num, decimal(a->d[i], num));
        }
    }
}

static void tw_value(struct vm *vm, struct tw *w, struct value v, int depth)
{
    char num[12];
    switch (v.tag) {
    case T_NULL: tw_put(w, "null", 4); break;
    case T_INT: tw_put(w, num, decimal(v.v, num)); break;
    case T_FIX: {
        char f[24];
        tw_put(w, f, fix_text(v.v, f));
        break;
    }
    case T_STR: tw_put(w, STR(vm, v)->b, STR(vm, v)->len); break;
    case T_ARR: case T_I32: case T_U8: case T_FXA:
        tw_join(vm, w, v, (const uint8_t *)",", 1, depth);
        break;
    case T_OBJ: tw_put(w, "[object Object]", 15); break;
    case T_FN: tw_put(w, "function", 8); break;
    case T_IMG: case T_LAZY: tw_put(w, "[image]", 7); break;
    }
}

/* v as text into dst, truncated to cap; returns the length written. */
uint32_t vm_text(struct vm *vm, struct value v, char *dst, uint32_t cap)
{
    struct tw w = { (uint8_t *)dst, (uint8_t *)dst + cap, 0, 0, 1 };
    tw_value(vm, &w, v, 0);
    /* each element visited put a byte or more, until the text was full */
    uint32_t made = (uint32_t)(w.p - (uint8_t *)dst);
    vm_charge(vm, (w.seen < made + 1 ? w.seen : made + 1) +
                  made / BYTES_PER_UNIT);
    return made;
}

/* ---- strings ------------------------------------------------------------ */

static uint32_t new_str(struct vm *vm, uint32_t len)
{
    if (len > 0x7fffffffu - 16) {
        vm_fail(vm, "string too long", "", "");
        return 0;
    }
    uint32_t off = gc_alloc(vm, T_STR, sizeof(struct str) + len);
    if (off)
        ((struct str *)MEM(vm, off))->len = len;
    return off;
}

int vm_new_string(struct vm *vm, const uint8_t *b, uint32_t len,
                  struct value *out)
{
    uint32_t off = new_str(vm, len);
    if (!off)
        return 0;
    memcpy(((struct str *)MEM(vm, off))->b, b, len);
    *out = mk(T_STR, (int32_t)off);
    return 1;
}

int vm_tostring(struct vm *vm, struct value v, struct value *out)
{
    if (v.tag == T_STR) {
        *out = v;
        return 1;
    }
    struct tw w = { 0, 0, 0, 0, 0 };
    tw_value(vm, &w, v, 0);
    if (!tw_spend(vm, &w))
        return 0;
    uint32_t off = new_str(vm, w.n);
    if (!off)
        return 0;
    struct str *s = MEM(vm, off);
    struct tw o = { s->b, s->b + w.n, 0, 0, 0 };
    tw_value(vm, &o, v, 0);
    *out = mk(T_STR, (int32_t)off);
    return 1;
}

/* a + b where either is a string; both stay on the VM stack meanwhile. */
static int concat(struct vm *vm, struct value *dst, struct value a,
                  struct value b)
{
    struct tw m = { 0, 0, 0, 0, 0 };
    tw_value(vm, &m, a, 0);
    uint32_t la = m.n;
    tw_value(vm, &m, b, 0);
    /* Formatting and allocating, measured on the 5G */
    if (!tw_spend(vm, &m) || !vm_spend(vm, 20))
        return 0;
    uint32_t off = new_str(vm, m.n);
    if (!off)
        return 0;
    struct str *s = MEM(vm, off);
    struct tw o = { s->b, s->b + m.n, 0, 0, 0 };
    tw_value(vm, &o, a, 0);
    o.p = s->b + la;
    tw_value(vm, &o, b, 0);
    *dst = mk(T_STR, (int32_t)off);
    return 1;
}

static int str_cmp(struct vm *vm, struct value a, struct value b)
{
    struct str *x = STR(vm, a), *y = STR(vm, b);
    uint32_t n = x->len < y->len ? x->len : y->len;
    vm_charge(vm, n / BYTES_PER_UNIT);
    int c = memcmp(x->b, y->b, n);
    if (c)
        return c;
    return x->len < y->len ? -1 : x->len > y->len;
}

static int equal(struct vm *vm, struct value a, struct value b)
{
    if (a.tag != b.tag)
        return is_num(a) && is_num(b) && scaled(a) == scaled(b);
    if (a.tag == T_STR && a.v != b.v) {
        struct str *x = STR(vm, a), *y = STR(vm, b);
        if (x->len != y->len)
            return 0;
        vm_charge(vm, x->len / BYTES_PER_UNIT);
        return !memcmp(x->b, y->b, x->len);
    }
    return a.v == b.v;
}

/* arcade_api.def's slots for the maths NCALL works inline, slots being
 * permanent; natives.c checks them */
enum { INLINE_FLR = 53, INLINE_ABS = 55, INLINE_MIN = 56, INLINE_MAX = 57,
       INLINE_MID = 58, INLINE_SGN = 59 };

/* ---- arrays and records --------------------------------------------- */

static int new_buf(struct vm *vm, uint32_t cap, uint32_t *off)
{
    if (cap > 0x0fffffffu)
        return vm_fail(vm, "array too long", "", "");
    *off = gc_alloc(vm, T_BUF, cap * sizeof(struct value));
    return *off != 0;
}

/* A new array of len nulls. */
int vm_new_array(struct vm *vm, uint32_t len, struct value *out)
{
    uint32_t off = gc_alloc(vm, T_ARR, sizeof(struct arr) - sizeof(struct hdr));
    if (!off)
        return 0;
    struct value a = mk(T_ARR, (int32_t)off);
    if (len) {
        uint32_t b;
        if (!vm_root(vm, a))
            return 0;
        int ok = new_buf(vm, len, &b);
        vm_unroot(vm);
        if (!ok)
            return 0;
        ARR(vm, a)->buf = b;
        ARR(vm, a)->cap = len;
    }
    ARR(vm, a)->len = len;
    *out = a;
    return 1;
}

/* A record with the keys named, every value null, for a system to fill;
 * its shape is the same for every record made from the same keys. */
static int new_obj(struct vm *vm, uint32_t shape, struct value *vals,
                   struct value *out);

int vm_new_record(struct vm *vm, const char *const *keys, uint32_t n,
                  struct value *out)
{
    static struct value nulls[32];
    uint32_t shape = 0;
    if (n > 32)
        return vm_fail(vm, "a record of more than 32 keys", "", "");
    for (uint32_t i = 0; i < n; i++) {
        struct value s;
        if (!vm_new_string(vm, (const uint8_t *)keys[i],
                           (uint32_t)strlen(keys[i]), &s))
            return 0;
        if (!vm_root(vm, s))
            return 0;
        uint32_t k = key_id(vm, s, 1);
        vm_unroot(vm);
        if (k == NONE)
            return 0;
        shape = vm_shape_add(vm, shape, k);
        if (shape == NONE)
            return 0;
    }
    return new_obj(vm, shape, nulls, out);
}

/* Room for at least need elements; the array must be rooted. */
static int arr_reserve(struct vm *vm, struct value a, uint32_t need)
{
    struct arr *p = ARR(vm, a);
    if (need <= p->cap)
        return 1;
    uint32_t cap = p->cap * 2;
    if (cap < need)
        cap = need;
    if (cap < 4)
        cap = 4;
    uint32_t b;
    if (!new_buf(vm, cap, &b))
        return 0;
    p = ARR(vm, a);
    if (p->len)
        memcpy(BUF(vm, b)->v, BUF(vm, p->buf)->v,
               p->len * sizeof(struct value));
    p->buf = b;
    p->cap = cap;
    return 1;
}

static int slot_of(struct vm *vm, uint32_t shape, uint32_t key)
{
    for (uint32_t s = shape; s; s = vm->shapes[s].parent)
        if (vm->shapes[s].key == key)
            return vm->shapes[s].nkeys - 1;
    return -1;
}

static struct value obj_get(struct vm *vm, struct value o, uint32_t key,
                            uint32_t site)
{
    struct obj *p = OBJ(vm, o);
    if (site != NONE) {
        struct ic *ic = &vm->ic[site];
        if (ic->shape == p->shape)
            return BUF(vm, p->buf)->v[ic->slot];
    }
    int slot = slot_of(vm, p->shape, key);
    if (slot < 0)
        return mk(T_NULL, 0);
    if (site != NONE) {
        vm->ic[site].shape = (uint16_t)p->shape;
        vm->ic[site].slot = (uint16_t)slot;
    }
    return BUF(vm, p->buf)->v[slot];
}

struct value vm_get(struct vm *vm, struct value o, const char *name)
{
    if (o.tag != T_OBJ)
        return mk(T_NULL, 0);
    uint32_t key = key_find(vm, name, (uint32_t)strlen(name));
    return key == NONE ? mk(T_NULL, 0) : obj_get(vm, o, key, NONE);
}

/* o and v must be rooted: adding a key may allocate. */
static int obj_set(struct vm *vm, struct value o, uint32_t key, uint32_t site,
                   struct value v)
{
    struct obj *p = OBJ(vm, o);
    if (site != NONE) {
        struct ic *ic = &vm->ic[site];
        if (ic->shape == p->shape) {
            BUF(vm, p->buf)->v[ic->slot] = v;
            return 1;
        }
    }
    int slot = slot_of(vm, p->shape, key);
    if (slot < 0) {
        uint32_t s = vm_shape_add(vm, p->shape, key);
        if (s == NONE)
            return 0;
        slot = vm->shapes[s].nkeys - 1;
        if ((uint32_t)slot >= p->cap) {
            uint32_t cap = p->cap * 2 < 4 ? 4 : p->cap * 2, b;
            if (!new_buf(vm, cap, &b))
                return 0;
            p = OBJ(vm, o);
            if (p->cap)
                memcpy(BUF(vm, b)->v, BUF(vm, p->buf)->v,
                       p->cap * sizeof(struct value));
            p->buf = b;
            p->cap = cap;
        }
        p->shape = s;
    } else if (site != NONE) {
        vm->ic[site].shape = (uint16_t)p->shape;
        vm->ic[site].slot = (uint16_t)slot;
    }
    BUF(vm, p->buf)->v[slot] = v;
    return 1;
}

/* ---- building values from C (json.c) ---------------------------------- */

int vm_new_text(struct vm *vm, uint32_t len, struct value *out)
{
    uint32_t off = new_str(vm, len);
    if (!off)
        return 0;
    *out = mk(T_STR, (int32_t)off);
    return 1;
}

int vm_push(struct vm *vm, struct value a, struct value v)
{
    if (!arr_reserve(vm, a, ARR(vm, a)->len + 1))
        return 0;
    struct arr *p = ARR(vm, a);
    BUF(vm, p->buf)->v[p->len++] = v;
    return 1;
}

int vm_set(struct vm *vm, struct value o, struct value key, struct value v)
{
    uint32_t k = key_id(vm, key, 1);
    return k != NONE && obj_set(vm, o, k, NONE, v);
}

int vm_field(struct vm *vm, struct value o, uint32_t i, struct value *name,
             struct value *v)
{
    struct obj *p = OBJ(vm, o);
    for (uint32_t s = p->shape; s; s = vm->shapes[s].parent)
        if (vm->shapes[s].nkeys == i + 1) {
            *name = mk(T_STR, (int32_t)vm->keyoff[vm->shapes[s].key]);
            *v = BUF(vm, p->buf)->v[i];
            return 1;
        }
    return 0;
}

/* NEWOBJ: the record of a literal, its values the top n of the stack. */
static int new_obj(struct vm *vm, uint32_t shape, struct value *vals,
                   struct value *out)
{
    uint32_t n = vm->shapes[shape].nkeys, b = 0;
    uint32_t off = gc_alloc(vm, T_OBJ, sizeof(struct obj) - sizeof(struct hdr));
    if (!off)
        return 0;
    struct value o = mk(T_OBJ, (int32_t)off);
    if (n) {
        if (!vm_root(vm, o))
            return 0;
        int ok = new_buf(vm, n, &b);
        vm_unroot(vm);
        if (!ok)
            return 0;
        memcpy(BUF(vm, b)->v, vals, n * sizeof(struct value));
    }
    struct obj *p = OBJ(vm, o);
    p->shape = shape;
    p->cap = n;
    p->buf = b;
    *out = o;
    return 1;
}

static int key_name(struct vm *vm, uint32_t key, char *buf, uint32_t cap)
{
    struct str *s = MEM(vm, vm->keyoff[key]);
    uint32_t n = s->len < cap - 1 ? s->len : cap - 1;
    memcpy(buf, s->b, n);
    buf[n] = 0;
    return 0;
}

static int no_field(struct vm *vm, const char *verb, struct value o,
                    uint32_t key)
{
    char name[32];
    key_name(vm, key, name, sizeof(name));
    return fail5(vm, verb, name, o.tag == T_NULL ? " of null" : " of a ",
                 o.tag == T_NULL ? "" : vm_typename(o.tag), "");
}

/* ---- indexing --------------------------------------------------------- */

static int get_index(struct vm *vm, struct value c, struct value i,
                     struct value *out)
{
    if (i.tag == T_FIX)
        i = mk(T_INT, floor_int(i));
    if (i.tag == T_INT) {
        uint32_t k = (uint32_t)i.v;
        switch (c.tag) {
        case T_ARR:
            *out = k < ARR(vm, c)->len ? BUF(vm, ARR(vm, c)->buf)->v[k]
                                       : mk(T_NULL, 0);
            return 1;
        case T_I32:
            *out = k < I32(vm, c)->len ? mk(T_INT, I32(vm, c)->d[k])
                                       : mk(T_NULL, 0);
            return 1;
        case T_FXA:
            *out = k < I32(vm, c)->len ? mk(T_FIX, I32(vm, c)->d[k])
                                       : mk(T_NULL, 0);
            return 1;
        case T_U8:
            *out = k < U8A(vm, c)->len ? mk(T_INT, U8A(vm, c)->d[k])
                                       : mk(T_NULL, 0);
            return 1;
        case T_STR:
            if (k >= STR(vm, c)->len) {
                *out = mk(T_NULL, 0);
                return 1;
            }
            {
                uint8_t ch = STR(vm, c)->b[k];
                return vm_new_string(vm, &ch, 1, out);
            }
        case T_OBJ:
            return vm_fail(vm, "a record's keys are strings, not numbers",
                           "", "");
        }
    } else if (i.tag == T_STR && c.tag == T_OBJ) {
        uint32_t key = key_id(vm, i, 0);
        *out = key == NONE ? mk(T_NULL, 0) : obj_get(vm, c, key, NONE);
        return 1;
    }
    if (c.tag == T_NULL)
        return vm_fail(vm, "can't index null", "", "");
    return fail5(vm, "can't index a ", vm_typename(c.tag), " with a ",
                 vm_typename(i.tag), "");
}

/* c[i] inline, for the common case: an integer index in range of an
 * array or a typed array. 0 leaves it to get_index(). The values are
 * read where they lie, and out may be c itself: whole values copied
 * into locals cost the interpreter a trip through its stack frame. */
static inline int index_fast(struct vm *vm, const struct value *c,
                             const struct value *i, struct value *out)
{
    if (i->tag != T_INT)
        return 0;
    uint32_t k = (uint32_t)i->v, t = c->tag;
    void *o = MEM(vm, (uint32_t)c->v);
    if (t == T_ARR && k < ((struct arr *)o)->len) {
        const struct value *e = &BUF(vm, ((struct arr *)o)->buf)->v[k];
        uint32_t et = e->tag;
        int32_t ev = e->v;
        out->tag = et;
        out->v = ev;
        return 1;
    }
    if ((t == T_I32 || t == T_FXA) && k < ((struct i32 *)o)->len) {
        int32_t ev = ((struct i32 *)o)->d[k];
        out->tag = t == T_I32 ? T_INT : T_FIX;
        out->v = ev;
        return 1;
    }
    if (t == T_U8 && k < ((struct u8a *)o)->len) {
        int32_t ev = ((struct u8a *)o)->d[k];
        out->tag = T_INT;
        out->v = ev;
        return 1;
    }
    return 0;
}

/* c[i] = v inline for an integer index in range of an array, an
 * Int32Array or FixArray given a number, or a Uint8Array other than the
 * screen's, which set_index() marks as drawn on. 0 leaves it to that. */
static inline int store_fast(struct vm *vm, const struct value *c,
                             const struct value *i, const struct value *v)
{
    if (i->tag != T_INT)
        return 0;
    uint32_t k = (uint32_t)i->v, t = c->tag;
    void *o = MEM(vm, (uint32_t)c->v);
    if (t == T_I32 && is_num(*v) && k < ((struct i32 *)o)->len) {
        ((struct i32 *)o)->d[k] = to_int(*v);
        return 1;
    }
    if (t == T_U8 && is_num(*v) && k < ((struct u8a *)o)->len &&
        !IS_SCREEN(vm, *c)) {
        ((struct u8a *)o)->d[k] = (uint8_t)to_int(*v);
        return 1;
    }
    if (t == T_FXA && is_num(*v) && k < ((struct i32 *)o)->len) {
        ((struct i32 *)o)->d[k] = to_fix(*v);
        return 1;
    }
    if (t == T_ARR && k < ((struct arr *)o)->len) {
        struct value *e = &BUF(vm, ((struct arr *)o)->buf)->v[k];
        e->tag = v->tag;
        e->v = v->v;
        return 1;
    }
    return 0;
}

/* n / d, n below d << 32 so the quotient fits 32 bits: shift and
 * subtract, a bit a step, which on the player is five instructions a bit
 * -- its 64 by 64 divide, which the C compiles to, costs some 600 cycles
 * against 170 on the rig */
static uint32_t div_64_32(uint64_t n, uint32_t d)
{
    uint32_t hi = (uint32_t)(n >> 32), lo = (uint32_t)n;
#ifdef ARCADE_ASM
    /* lo shifts out into hi as the quotient's bits shift in: a carry out
     * of hi, or hi reaching d, takes d off and sets the bit. Eight bits a
     * pass, four passes: whole, the 32 would push vm.c's literals out of
     * reach. */
    uint32_t passes;
    __asm__("mov   %2, #4\n"
            "1:\n\t"
            ".rept 8\n\t"
            "adds  %0, %0, %0\n\t"
            "adcs  %1, %1, %1\n\t"
            "cmpcc %1, %3\n\t"
            "subcs %1, %1, %3\n\t"
            "orrcs %0, %0, #1\n\t"
            ".endr\n\t"
            "subs  %2, %2, #1\n\t"
            "bne   1b"
            : "+r"(lo), "+r"(hi), "=&r"(passes) : "r"(d) : "cc");
    return lo;
#else
    uint32_t q = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t top = hi >> 31;
        hi = hi << 1 | lo >> 31;
        lo <<= 1;
        q <<= 1;
        if (top || hi >= d) {
            hi -= d;
            q |= 1;
        }
    }
    return q;
#endif
}

/* a / b for two numbers not both integers, as vm_fix_op() works it --
 * (a * 65536) / b in 16.16, toward 0 -- when b is not 0 and the quotient
 * fits 32 bits: a fraction over an integer is a.v / b.v, the same
 * quotient; 0 to leave it to vm_fix_op. Out of line: inside run() it
 * would push the loop's literals out of a load's reach. */
static __attribute__((noinline)) int div_fast(struct value *a,
                                              const struct value *b)
{
    uint32_t ta = a->tag, tb = b->tag;
    int32_t x = a->v, y = b->v;
    if (!y)
        return 0;
    if (ta == T_FIX && tb == T_INT) {
        a->v = y == -1 ? (int32_t)(0u - (uint32_t)x) : x / y;
        return 1;
    }
    int64_t n = ta == T_FIX ? (int64_t)x << 16 : (int64_t)x << 32;
    int64_t d = tb == T_FIX ? y : (int64_t)y << 16;
    uint64_t un = n < 0 ? 0 - (uint64_t)n : (uint64_t)n;
    uint64_t ud = d < 0 ? 0 - (uint64_t)d : (uint64_t)d;
    if (ud >> 32 || (un >> 32) >= ud)
        return 0;
    uint32_t q = div_64_32(un, (uint32_t)ud);
    a->v = (int32_t)((n < 0) != (d < 0) ? 0u - q : q);
    a->tag = T_FIX;
    return 1;
}

/* a + b, a - b or a * b in place of a, for two numbers not both
 * integers, as vm_fix_op() works them but without the call; 0 for any
 * other op or value, left to it */
static inline int fix_fast(struct value *a, const struct value *b, int op)
{
    uint32_t ta = a->tag, tb = b->tag;
    if ((ta != T_FIX && ta != T_INT) || (tb != T_FIX && tb != T_INT))
        return 0;
    if (op == OP_DIV)
        return div_fast(a, b);
    int32_t x = a->v, y = b->v;
    if (op == OP_MUL) {
        if (ta == T_INT || tb == T_INT)
            a->v = (int32_t)((uint32_t)x * (uint32_t)y);
        else
            a->v = (int32_t)(((int64_t)x * y) >> 16);
    } else if (op == OP_ADD || op == OP_SUB) {
        uint32_t ux = ta == T_FIX ? (uint32_t)x : (uint32_t)x << 16;
        uint32_t uy = tb == T_FIX ? (uint32_t)y : (uint32_t)y << 16;
        a->v = (int32_t)(op == OP_ADD ? ux + uy : ux - uy);
    } else
        return 0;
    a->tag = T_FIX;
    return 1;
}

/* a < b and the rest for anything but two integers, kept out of the
 * interpreter's loop so its fast path takes no copies: 1 or 0, or -1
 * with the error set */
static int compare_slow(struct vm *vm, const struct value *pa,
                        const struct value *pb, int op)
{
    struct value a = *pa, b = *pb;
    int c;
    if (is_num(a) && is_num(b)) {
        int64_t x = scaled(a), y = scaled(b);
        c = x < y ? -1 : x > y;
    } else if (a.tag == T_STR && b.tag == T_STR)
        c = str_cmp(vm, a, b);
    else {
        vm_fail(vm, "a comparison needs two numbers or two strings, not a ",
                vm_typename(is_num(a) || a.tag == T_STR ? b.tag : a.tag), "");
        return -1;
    }
    switch (op) {
    case 0: return c < 0;
    case 1: return c <= 0;
    case 2: return c > 0;
    default: return c >= 0;
    }
}

/* c, i and v stay on the stack, so growth may allocate. */
static int set_index(struct vm *vm, struct value c, struct value i,
                     struct value v)
{
    if (i.tag == T_FIX)
        i = mk(T_INT, floor_int(i));
    if (i.tag == T_INT) {
        uint32_t k = (uint32_t)i.v;
        switch (c.tag) {
        case T_ARR: {
            struct arr *a = ARR(vm, c);
            if (k == a->len) {
                if (!arr_reserve(vm, c, k + 1))
                    return 0;
                a = ARR(vm, c);
                a->len++;
            } else if (k > a->len)
                return vm_fail(vm, "index past the end of an array", "", "");
            BUF(vm, a->buf)->v[k] = v;
            return 1;
        }
        case T_I32:
        case T_U8:
        case T_FXA:
            if (!is_num(v))
                return vm_fail(vm, "a typed array holds numbers, not a ",
                               vm_typename(v.tag), "");
            if (c.tag == T_I32 && k < I32(vm, c)->len)
                I32(vm, c)->d[k] = to_int(v);
            else if (c.tag == T_FXA && k < I32(vm, c)->len)
                I32(vm, c)->d[k] = to_fix(v);
            else if (c.tag == T_U8 && k < U8A(vm, c)->len) {
                U8A(vm, c)->d[k] = (uint8_t)to_int(v);
                if (IS_SCREEN(vm, c))
                    screen_written(vm, k / SCREEN_W, k / SCREEN_W);
            }
            return 1;
        case T_OBJ:
            return vm_fail(vm, "a record's keys are strings, not numbers",
                           "", "");
        }
    } else if (i.tag == T_STR && c.tag == T_OBJ) {
        uint32_t key = key_id(vm, i, 1);
        if (key == NONE)
            return 0;
        return obj_set(vm, c, key, NONE, v);
    }
    if (c.tag == T_STR)
        return vm_fail(vm, "strings can't be changed", "", "");
    if (c.tag == T_NULL)
        return vm_fail(vm, "can't index null", "", "");
    return vm_fail(vm, "can't index a ", vm_typename(c.tag), "");
}

/* ---- methods ------------------------------------------------------------ */

static int need(struct vm *vm, struct value v, uint32_t tag,
                const char *what)
{
    if (v.tag == tag)
        return 1;
    return vm_fail(vm, what, vm_typename(v.tag), "");
}

/* JS's clamp for slice and splice positions. */
static uint32_t position(int32_t p, uint32_t len)
{
    if (p < 0)
        return (uint32_t)-p >= len ? 0 : len - (uint32_t)-p;
    return (uint32_t)p > len ? len : (uint32_t)p;
}

static int sort_values(struct vm *vm, struct value fn, struct value *v,
                       struct value *tmp, uint32_t n);

static int method(struct vm *vm, int m, struct value *r, struct value *a,
                  int argc)
{
    struct value self = *r, out = mk(T_NULL, 0);
    uint32_t work = 0;              /* bytes moved or made, charged after */
    int is_arr = self.tag == T_ARR, is_str = self.tag == T_STR;
    /* positions and counts are whole; splice's values from the third on
     * are what it inserts, kept as they are */
    if (m == M_splice || m == M_slice || m == M_charCodeAt ||
        m == M_substring)
        for (int k = 0; k < argc && (m != M_splice || k < 2); k++)
            if (a[k].tag == T_FIX)
                a[k] = mk(T_INT, to_int(a[k]));
    switch (m) {
    case M_push: {
        if (!need(vm, self, T_ARR, ".push() needs an array, not a "))
            return 0;
        uint32_t n = ARR(vm, self)->len;
        if (!arr_reserve(vm, self, n + (uint32_t)argc))
            return 0;
        struct value *v = BUF(vm, ARR(vm, self)->buf)->v;
        for (int k = 0; k < argc; k++)
            v[n + (uint32_t)k] = a[k];
        n += (uint32_t)argc;
        ARR(vm, self)->len = n;
        out = mk(T_INT, (int32_t)n);
        break;
    }
    case M_pop:
    case M_shift: {
        if (!need(vm, self, T_ARR, m == M_pop ? ".pop() needs an array, not a "
                  : ".shift() needs an array, not a "))
            return 0;
        struct arr *p = ARR(vm, self);
        if (!p->len)
            break;
        struct value *v = BUF(vm, p->buf)->v;
        p->len--;
        if (m == M_pop)
            out = v[p->len];
        else {
            out = v[0];
            memmove(v, v + 1, p->len * sizeof(struct value));
            work = p->len * sizeof(struct value);
        }
        v[p->len] = mk(T_NULL, 0);
        break;
    }
    case M_unshift: {
        if (!need(vm, self, T_ARR, ".unshift() needs an array, not a "))
            return 0;
        uint32_t n = ARR(vm, self)->len, k = (uint32_t)argc;
        if (!arr_reserve(vm, self, n + k))
            return 0;
        struct value *v = BUF(vm, ARR(vm, self)->buf)->v;
        memmove(v + k, v, n * sizeof(struct value));
        work = n * sizeof(struct value);
        for (uint32_t i = 0; i < k; i++)
            v[i] = a[i];
        ARR(vm, self)->len = n + k;
        out = mk(T_INT, (int32_t)(n + k));
        break;
    }
    case M_splice: {
        if (!need(vm, self, T_ARR, ".splice() needs an array, not a "))
            return 0;
        uint32_t len = ARR(vm, self)->len;
        if (!need(vm, a[0], T_INT, ".splice() needs a number, not a "))
            return 0;
        uint32_t s = position(a[0].v, len), k = len - s;
        if (argc > 1) {
            if (!need(vm, a[1], T_INT, ".splice() needs a number, not a "))
                return 0;
            k = a[1].v < 0 ? 0 : (uint32_t)a[1].v > len - s ? len - s
                                                             : (uint32_t)a[1].v;
        }
        uint32_t add = argc > 2 ? (uint32_t)argc - 2 : 0;
        if (add > k && !arr_reserve(vm, self, len - k + add))
            return 0;
        if (!vm_new_array(vm, k, &out))
            return 0;
        struct arr *p = ARR(vm, self);
        struct value *v = BUF(vm, p->buf)->v;
        if (k)
            memcpy(BUF(vm, ARR(vm, out)->buf)->v, v + s, k * sizeof(*v));
        memmove(v + s + add, v + s + k, (len - s - k) * sizeof(*v));
        for (uint32_t i = 0; i < add; i++)
            v[s + i] = a[2 + i];
        for (uint32_t i = len - k + add; i < len; i++)
            v[i] = mk(T_NULL, 0);
        p->len = len - k + add;
        work = (len + add) * sizeof(struct value);
        break;
    }
    case M_slice: {
        if (!is_arr && !is_str)
            return vm_fail(vm, ".slice() needs an array or string, not a ",
                           vm_typename(self.tag), "");
        uint32_t len = is_arr ? ARR(vm, self)->len : STR(vm, self)->len;
        uint32_t s = 0, e = len;
        if (argc > 0) {
            if (!need(vm, a[0], T_INT, ".slice() needs a number, not a "))
                return 0;
            s = position(a[0].v, len);
        }
        if (argc > 1) {
            if (!need(vm, a[1], T_INT, ".slice() needs a number, not a "))
                return 0;
            e = position(a[1].v, len);
        }
        uint32_t k = e > s ? e - s : 0;
        work = is_str ? k : k * sizeof(struct value);
        if (is_str) {
            uint32_t off = new_str(vm, k);
            if (!off)
                return 0;
            memcpy(((struct str *)MEM(vm, off))->b, STR(vm, self)->b + s, k);
            out = mk(T_STR, (int32_t)off);
        } else {
            if (!vm_new_array(vm, k, &out))
                return 0;
            if (k)
                memcpy(BUF(vm, ARR(vm, out)->buf)->v,
                       BUF(vm, ARR(vm, self)->buf)->v + s,
                       k * sizeof(struct value));
        }
        break;
    }
    case M_indexOf:
    case M_lastIndexOf: {
        /* forward from 'from' (0 by default), or back from it (the end by
         * default); a negative 'from' counts from the end, as JS's */
        int back = m == M_lastIndexOf;
        const char *name = back ? ".lastIndexOf()" : ".indexOf()";
        int32_t found = -1, len;
        if (is_arr)
            len = (int32_t)ARR(vm, self)->len;
        else if (self.tag == T_I32 || self.tag == T_FXA)
            len = (int32_t)I32(vm, self)->len;
        else if (self.tag == T_U8)
            len = (int32_t)U8A(vm, self)->len;
        else if (is_str)
            len = (int32_t)STR(vm, self)->len;
        else
            return vm_fail(vm, name, " needs an array or string, not a ",
                           vm_typename(self.tag));
        int32_t from = back ? len - 1 : 0;
        if (argc > 1) {
            if (!is_num(a[1]))
                return vm_fail(vm, name, " needs a number to start from, not a ",
                               vm_typename(a[1].tag));
            from = to_int(a[1]);
            if (from < 0)
                from += len;
            if (back && from >= len)
                from = len - 1;
            if (!back && from < 0)
                from = 0;
        }
        int32_t step = back ? -1 : 1;
        if (is_arr) {
            struct value *v = BUF(vm, ARR(vm, self)->buf)->v;
            for (int32_t i = from; i >= 0 && i < len && found < 0; i += step)
                if (equal(vm, v[i], a[0]))
                    found = i;
            work = (uint32_t)len * 4;
        } else if (self.tag == T_FXA) {
            if (is_num(a[0])) {
                int32_t want = to_fix(a[0]);
                for (int32_t i = from; i >= 0 && i < len && found < 0; i += step)
                    if (I32(vm, self)->d[i] == want)
                        found = i;
                work = (uint32_t)len * 4;
            }
        } else if (self.tag == T_I32 || self.tag == T_U8) {
            if (a[0].tag == T_INT) {
                for (int32_t i = from; i >= 0 && i < len && found < 0; i += step) {
                    int32_t e = self.tag == T_I32 ? I32(vm, self)->d[i]
                                                  : U8A(vm, self)->d[i];
                    if (e == a[0].v)
                        found = i;
                }
                work = (uint32_t)len * 4;
            }
        } else {
            if (a[0].tag != T_STR)
                return vm_fail(vm, name, " on a string needs a string, not a ",
                               vm_typename(a[0].tag));
            struct str *h = STR(vm, self), *nd = STR(vm, a[0]);
            int32_t last = (int32_t)h->len - (int32_t)nd->len;
            if (back && (from > last || argc < 2))
                from = last;
            /* each place tried costs the needle's length at most, and is
             * charged as it goes: a long search is stopped */
            uint32_t cost = 1 + nd->len / BYTES_PER_UNIT, tried = 0;
            for (int32_t i = from; i >= 0 && i <= last && found < 0; i += step) {
                if (!memcmp(h->b + i, nd->b, nd->len))
                    found = i;
                if (++tried == 1024) {
                    tried = 0;
                    if (!vm_spend(vm, cost * 1024))
                        return 0;
                }
            }
            vm_charge(vm, cost * tried);
        }
        out = mk(T_INT, found);
        break;
    }
    case M_join: {
        if (!is_arr && self.tag != T_I32 && self.tag != T_U8 &&
            self.tag != T_FXA)
            return vm_fail(vm, ".join() needs an array, not a ",
                           vm_typename(self.tag), "");
        const uint8_t *sep = (const uint8_t *)",";
        uint32_t seplen = 1;
        if (argc > 0) {
            if (!need(vm, a[0], T_STR, ".join() needs a string, not a "))
                return 0;
            sep = STR(vm, a[0])->b;
            seplen = STR(vm, a[0])->len;
        }
        struct tw m0 = { 0, 0, 0, 0, 0 };
        tw_join(vm, &m0, self, sep, seplen, 0);
        if (!tw_spend(vm, &m0))
            return 0;
        uint32_t off = new_str(vm, m0.n);
        if (!off)
            return 0;
        if (argc > 0)
            sep = STR(vm, a[0])->b;
        struct str *s = MEM(vm, off);
        struct tw o = { s->b, s->b + m0.n, 0, 0, 0 };
        tw_join(vm, &o, self, sep, seplen, 0);
        out = mk(T_STR, (int32_t)off);
        break;
    }
    case M_reverse: {
        uint32_t n, i;
        if (is_arr) {
            n = ARR(vm, self)->len;
            struct value *v = BUF(vm, ARR(vm, self)->buf)->v, t;
            for (i = 0; i < n / 2; i++) {
                t = v[i]; v[i] = v[n - 1 - i]; v[n - 1 - i] = t;
            }
        } else if (self.tag == T_I32 || self.tag == T_FXA) {
            n = I32(vm, self)->len;
            int32_t *d = I32(vm, self)->d, t;
            for (i = 0; i < n / 2; i++) {
                t = d[i]; d[i] = d[n - 1 - i]; d[n - 1 - i] = t;
            }
        } else if (self.tag == T_U8) {
            n = U8A(vm, self)->len;
            uint8_t *d = U8A(vm, self)->d, t;
            for (i = 0; i < n / 2; i++) {
                t = d[i]; d[i] = d[n - 1 - i]; d[n - 1 - i] = t;
            }
            if (IS_SCREEN(vm, self))
                screen_written(vm, 0, SCREEN_H - 1);
        } else
            return vm_fail(vm, ".reverse() needs an array, not a ",
                           vm_typename(self.tag), "");
        work = n * 4;
        out = self;
        break;
    }
    case M_sort: {
        if (!need(vm, self, T_ARR, ".sort() needs an array, not a ") ||
            !need(vm, a[0], T_FN, ".sort() needs a comparator function, "
                  "not a "))
            return 0;
        /* A comparator runs the cart again on the C stack, so it may not
         * sort in turn: one sort at a time keeps that stack shallow */
        if (vm->sorting)
            return vm_fail(vm, ".sort() inside a sort's comparator", "", "");
        /* Sort a copy, so a comparator that changes the array cannot
         * disturb the sort; the copy and its scratch are rooted in tmp. */
        uint32_t n = ARR(vm, self)->len;
        struct value copy, scratch;
        if (!vm_new_array(vm, n, &copy) || !vm_root(vm, copy))
            return 0;
        int ok = vm_new_array(vm, n, &scratch) && vm_root(vm, scratch);
        if (ok) {
            if (n)
                memcpy(BUF(vm, ARR(vm, copy)->buf)->v,
                       BUF(vm, ARR(vm, self)->buf)->v,
                       n * sizeof(struct value));
            vm->sorting = 1;
            ok = sort_values(vm, a[0], BUF(vm, ARR(vm, copy)->buf)->v,
                             n ? BUF(vm, ARR(vm, scratch)->buf)->v : NULL, n);
            vm->sorting = 0;
            vm_unroot(vm);
        }
        vm_unroot(vm);
        if (!ok)
            return 0;
        uint32_t len = ARR(vm, self)->len;
        if (n > len)
            n = len;
        if (n)
            memcpy(BUF(vm, ARR(vm, self)->buf)->v,
                   BUF(vm, ARR(vm, copy)->buf)->v, n * sizeof(struct value));
        work = 3 * n * sizeof(struct value);
        out = self;
        break;
    }
    case M_charCodeAt:
        if (!need(vm, self, T_STR, ".charCodeAt() needs a string, not a ") ||
            !need(vm, a[0], T_INT, ".charCodeAt() needs a number, not a "))
            return 0;
        if ((uint32_t)a[0].v < STR(vm, self)->len)
            out = mk(T_INT, STR(vm, self)->b[a[0].v]);
        break;
    case M_substring: {
        if (!need(vm, self, T_STR, ".substring() needs a string, not a "))
            return 0;
        uint32_t len = STR(vm, self)->len, s, e = len;
        if (!need(vm, a[0], T_INT, ".substring() needs a number, not a "))
            return 0;
        s = a[0].v < 0 ? 0 : (uint32_t)a[0].v > len ? len : (uint32_t)a[0].v;
        if (argc > 1) {
            if (!need(vm, a[1], T_INT, ".substring() needs a number, not a "))
                return 0;
            e = a[1].v < 0 ? 0
                : (uint32_t)a[1].v > len ? len : (uint32_t)a[1].v;
        }
        if (s > e) {
            uint32_t t = s; s = e; e = t;
        }
        uint32_t off = new_str(vm, e - s);
        if (!off)
            return 0;
        memcpy(((struct str *)MEM(vm, off))->b, STR(vm, self)->b + s, e - s);
        work = e - s;
        out = mk(T_STR, (int32_t)off);
        break;
    }
    case M_toUpperCase:
    case M_toLowerCase: {
        if (!need(vm, self, T_STR, m == M_toUpperCase
                  ? ".toUpperCase() needs a string, not a "
                  : ".toLowerCase() needs a string, not a "))
            return 0;
        uint32_t len = STR(vm, self)->len;
        uint32_t off = new_str(vm, len);
        if (!off)
            return 0;
        uint8_t *d = ((struct str *)MEM(vm, off))->b;
        const uint8_t *s = STR(vm, self)->b;
        for (uint32_t i = 0; i < len; i++) {
            uint8_t c = s[i];
            if (m == M_toUpperCase && c >= 'a' && c <= 'z')
                c = (uint8_t)(c - 32);
            else if (m == M_toLowerCase && c >= 'A' && c <= 'Z')
                c = (uint8_t)(c + 32);
            d[i] = c;
        }
        work = len * 2;
        out = mk(T_STR, (int32_t)off);
        break;
    }
    default:
        return vm_fail(vm, "unknown method", "", "");
    }
    vm_charge(vm, work / BYTES_PER_UNIT);
    *r = out;
    return 1;
}

/* a[i] belongs after b: the comparator returned more than 0. Both are
 * pushed as its arguments, inside the stack slack. */
static int after(struct vm *vm, struct value fn, struct value x,
                 struct value y, int *res)
{
    struct value r;
    vm->sp[0] = x;
    vm->sp[1] = y;
    vm->sp += 2;
    if (!vm_call(vm, fn, 2, &r))
        return 0;
    if (!is_num(r))
        return vm_fail(vm, "a sort comparator must return a number, not a ",
                       vm_typename(r.tag), "");
    *res = r.v > 0;
    return 1;
}

/* Stable: insertion sort for short runs, then bottom-up merging. */
static int sort_values(struct vm *vm, struct value fn, struct value *v,
                       struct value *tmp, uint32_t n)
{
    const uint32_t run = 8;
    uint32_t i, j, w;
    int gt;
    for (i = 0; i < n; i += run) {
        uint32_t end = i + run < n ? i + run : n;
        for (j = i + 1; j < end; j++) {
            struct value x = v[j];
            uint32_t k = j;
            while (k > i) {
                if (!after(vm, fn, v[k - 1], x, &gt))
                    return 0;
                if (!gt)
                    break;
                v[k] = v[k - 1];
                k--;
            }
            v[k] = x;
        }
    }
    for (w = run; w < n; w *= 2) {
        for (i = 0; i < n; i += 2 * w) {
            uint32_t mid = i + w < n ? i + w : n;
            uint32_t end = i + 2 * w < n ? i + 2 * w : n;
            uint32_t l = i, r = mid, o = i;
            while (l < mid && r < end) {
                if (!after(vm, fn, v[l], v[r], &gt))
                    return 0;
                tmp[o++] = gt ? v[r++] : v[l++];
            }
            while (l < mid)
                tmp[o++] = v[l++];
            while (r < end)
                tmp[o++] = v[r++];
        }
        memcpy(v, tmp, n * sizeof(struct value));
    }
    return 1;
}

/* The arithmetic of two numbers when either is a fraction. */
int vm_fix_op(struct vm *vm, int op, struct value a, struct value b,
              struct value *out)
{
    int64_t x = scaled(a), y = scaled(b), r = 0;
    switch (op) {
    case OP_ADD: r = x + y; break;
    case OP_SUB: r = x - y; break;
    case OP_MUL:
        /* An integer times a fraction is exact; two fractions drop the
         * product's low 16 bits, rounding down. */
        if (a.tag == T_INT)
            r = (int64_t)a.v * y;
        else if (b.tag == T_INT)
            r = x * (int64_t)b.v;
        else
            r = (x * y) >> 16;
        break;
    case OP_DIV: {
        if (!y)
            return vm_fail(vm, "division by zero", "", "");
        int64_t n = x * 65536;
        r = (n == INT64_MIN && y == -1) ? n : n / y;
        break;
    }
    case OP_MOD:
        if (!y)
            return vm_fail(vm, "division by zero", "", "");
        r = x % y;
        break;
    }
    *out = mk(T_FIX, wrap32(r));
    return 1;
}

/* A fraction to a whole power, squaring as numbers.js does. */
static int fix_pow(struct vm *vm, struct value x, int32_t e,
                   struct value *out)
{
    struct value r = mk(T_FIX, 65536);
    uint32_t n = e < 0 ? 0u - (uint32_t)e : (uint32_t)e;
    for (; n; n >>= 1) {
        if (n & 1)
            vm_fix_op(vm, OP_MUL, r, x, &r);
        vm_fix_op(vm, OP_MUL, x, x, &x);
    }
    if (e < 0)
        return vm_fix_op(vm, OP_DIV, mk(T_FIX, 65536), r, out);
    *out = r;
    return 1;
}

static int digit(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

static int intrinsic(struct vm *vm, int id, struct value *a, int argc,
                     struct value *out)
{
    switch (id) {
    case I_fromCharCode: {
        if (!is_num(a[0]))
            return vm_fail(vm, "String.fromCharCode() needs a number, not a ",
                           vm_typename(a[0].tag), "");
        uint8_t c = (uint8_t)to_int(a[0]);
        return vm_new_string(vm, &c, 1, out);
    }
    case I_parseInt: {
        *out = mk(T_NULL, 0);
        int radix = 10, given = 0;
        if (argc > 1) {
            int32_t rx = to_int(a[1]);
            if (!is_num(a[1]) || rx < 2 || rx > 36)
                return 1;
            radix = rx;
            given = 1;
        }
        if (is_num(a[0]) && radix == 10) {
            *out = mk(T_INT, to_int(a[0]));
            return 1;
        }
        if (a[0].tag != T_STR)
            return 1;
        const uint8_t *s = STR(vm, a[0])->b, *e = s + STR(vm, a[0])->len;
        while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
            s++;
        int neg = 0;
        if (s < e && (*s == '-' || *s == '+'))
            neg = *s++ == '-';
        if ((radix == 16 || !given) && e - s >= 2 && s[0] == '0' &&
            (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
            radix = 16;
        }
        uint32_t v = 0;
        const uint8_t *first = s;
        while (s < e && digit(*s) < radix)
            v = v * (uint32_t)radix + (uint32_t)digit(*s++);
        if (s == first)
            return 1;
        *out = mk(T_INT, (int32_t)(neg ? 0u - v : v));
        return 1;
    }
    case I_pow: {
        if (!is_num(a[0]) || !is_num(a[1]))
            return vm_fail(vm, "Math.pow() needs numbers, not a ",
                           vm_typename(is_num(a[0]) ? a[1].tag : a[0].tag), "");
        if (a[1].tag == T_FIX && (a[1].v & 0xffff))
            return vm_fail(vm, "Math.pow() needs a whole exponent", "", "");
        int32_t e = floor_int(a[1]);
        if (a[0].tag == T_FIX)
            return fix_pow(vm, a[0], e, out);
        int32_t b = a[0].v;
        uint32_t r = 1, x = (uint32_t)b;
        if (e < 0)
            r = b == 1 ? 1 : b == -1 ? ((e & 1) ? 0xffffffffu : 1) : 0;
        else
            for (; e; e >>= 1, x *= x)
                if (e & 1)
                    r *= x;
        *out = mk(T_INT, (int32_t)r);
        return 1;
    }
    case I_sign:
        if (!is_num(a[0]))
            return vm_fail(vm, "Math.sign() needs a number, not a ",
                           vm_typename(a[0].tag), "");
        *out = mk(T_INT, a[0].v > 0 ? 1 : a[0].v < 0 ? -1 : 0);
        return 1;
    case I_round:
    case I_trunc:
        if (!is_num(a[0]))
            return vm_fail(vm, id == I_round ? "Math.round() needs a number, "
                           "not a " : "Math.trunc() needs a number, not a ",
                           vm_typename(a[0].tag), "");
        if (id == I_trunc)
            *out = mk(T_INT, to_int(a[0]));
        else
            *out = mk(T_INT, a[0].tag == T_FIX
                      ? wrap32(((int64_t)a[0].v + 32768) >> 16) : a[0].v);
        return 1;
    }
    return vm_fail(vm, "unknown intrinsic", "", "");
}

/* A native's work beyond its own instruction, counted toward the next
 * yield; budget never reaches 0 here, which only the dispatch loop
 * handles. */
void vm_charge(struct vm *vm, uint32_t units)
{
    while (units >= vm->budget) {
        units -= vm->budget;
        vm->units += vm->slice;
        vm->slice = YIELD_UNITS;
        vm->budget = YIELD_UNITS;
        if (vm->host && vm->host->yield)
            vm->host->yield(vm->host->ctx);
    }
    vm->budget -= units;
}

int vm_spend(struct vm *vm, uint32_t units)
{
    vm_charge(vm, units);
    if (vm->units > vm->ceiling)
        return vm_fail(vm, "The game stopped responding", "", "");
    return 1;
}

/* ---- calls -------------------------------------------------------------- */

/* Enters function fi with argc arguments on top of vm->sp, fitting them to
 * its arity. With drop, the function value lies below the arguments and
 * is removed on return. */
static int enter(struct vm *vm, uint32_t fi, uint32_t argc, uint32_t drop)
{
    const struct fninfo *f = &vm->fns[fi];
    struct value *sp = vm->sp;
    if (vm->nframes >= MAX_FRAMES)
        return vm_fail(vm, "too much recursion", "", "");
    if ((uint32_t)(sp - vm->stack) + f->nlocals + f->maxstack + STACK_SLACK
        + (argc < f->arity ? f->arity - argc : 0) > STACK_VALUES)
        return vm_fail(vm, "too much recursion", "", "");
    while (argc < f->arity) {
        *sp++ = mk(T_NULL, 0);
        argc++;
    }
    sp -= argc - f->arity;
    struct frame *fr = &vm->frames[vm->nframes++];
    fr->fn = fi;
    fr->bp = (uint32_t)(sp - f->arity - vm->stack);
    fr->drop = drop;
    fr->pc = f->off;
    for (uint32_t i = f->arity; i < f->nlocals; i++)
        *sp++ = mk(T_NULL, 0);
    vm->sp = sp;
    return 1;
}

static int run(struct vm *vm, uint32_t stop);

int vm_call(struct vm *vm, struct value fn, int argc, struct value *out)
{
    if (fn.tag != T_FN)
        return vm_fail(vm, "can't call a ", vm_typename(fn.tag), "");
    uint32_t base = vm->nframes;
    if (!enter(vm, (uint32_t)fn.v, (uint32_t)argc, 0) || !run(vm, base))
        return 0;
    *out = *--vm->sp;
    return 1;
}

/* ---- run() -------------------------------------------------------------- */

#define U8()  (*pc++)
#define S8()  ((int8_t)*pc++)
#define U16() (pc += 2, (uint32_t)(pc[-2] | (pc[-1] << 8)))
#define S16() ((int16_t)U16())
#define S32() (pc += 4, (int32_t)(pc[-4] | (pc[-3] << 8) | (pc[-2] << 16) | \
                                  ((uint32_t)pc[-1] << 24)))

/* Hand the registers to a helper that may allocate or run code, and take
 * them back after. */
#define SYNC() (vm->sp = sp, vm->budget = budget, \
                vm->frames[vm->nframes - 1].pc = (uint32_t)(pc - code))
#define RELOAD() (sp = vm->sp, budget = vm->budget)

#define NEXT() do { if (--budget == 0) goto slice; \
                    goto *labels[*pc++]; } while (0)
#define FAIL(a, b) do { vm_fail(vm, a, b, ""); goto fail; } while (0)

static inline int truthy(struct vm *vm, struct value v)
{
    if (v.tag == T_INT)
        return v.v != 0;
    return vm_truthy(vm, v);
}

/* Each handler jumps to the next itself: at -Os GCC merges the computed
 * gotos into one shared dispatch, three branches longer. No handler takes
 * the address of a or b, so they stay in registers. */
__attribute__((optimize("O2", "no-gcse")))
static int run(struct vm *vm, uint32_t stop)
{
    static const void *const labels[OP_COUNT] = {
#define OP(name, ops, pops, pushes) &&op_##name,
#define MTH(name, min, max)
#define INTRINSIC(name, min, max)
#include "games/arcade/opcodes.def"
#undef OP
#undef MTH
#undef INTRINSIC
    };
    const uint8_t *code = vm->code, *pc;
    struct value *sp, *lp, a, b;
    uint32_t budget, n;
    struct frame *fr = &vm->frames[vm->nframes - 1];

    pc = code + fr->pc;
    lp = vm->stack + fr->bp;
    RELOAD();
    if (budget == 0)
        budget = 1;
    goto *labels[*pc++];

slice:
    /* Every YIELD_UNITS units: let the codec run, and stop a cart that has
     * run away. Where the bytecode is goes in its frame, for a profiler. */
    vm->units += vm->slice;
    vm->slice = YIELD_UNITS;
    budget = YIELD_UNITS;
    if (vm->host && vm->host->yield) {
        vm->frames[vm->nframes - 1].pc = (uint32_t)(pc - code);
        vm->in_code = 1;
        vm->host->yield(vm->host->ctx);
        vm->in_code = 0;
    }
    if (vm->units > vm->ceiling) {
        pc++;
        FAIL("The game stopped responding", "");
    }
    goto *labels[*pc++];

op_NOP:
    NEXT();
op_POP:
    sp--;
    NEXT();
op_DUP:
    sp[0] = sp[-1];
    sp++;
    NEXT();
op_DUP2:
    sp[0] = sp[-2];
    sp[1] = sp[-1];
    sp += 2;
    NEXT();
op_PUSHNULL:
    *sp++ = mk(T_NULL, 0);
    NEXT();
op_PUSHI8:
    *sp++ = mk(T_INT, S8());
    NEXT();
op_PUSHI16:
    *sp++ = mk(T_INT, S16());
    NEXT();
op_PUSHI32:
    *sp++ = mk(T_INT, S32());
    NEXT();
op_PUSHS:
    *sp++ = mk(T_STR, (int32_t)vm->strings[U16()]);
    NEXT();
op_PUSHFN:
    *sp++ = mk(T_FN, (int32_t)U16());
    NEXT();
op_LDL:
    *sp++ = lp[U8()];
    NEXT();
op_STL:
    lp[U8()] = *--sp;
    NEXT();
op_LDG:
    *sp++ = vm->globals[U16()];
    NEXT();
op_STG:
    vm->globals[U16()] = *--sp;
    NEXT();
op_INCL: {
    struct value *l = &lp[U8()];
    int32_t k = S8();
    if (l->tag == T_INT) {
        l->v = (int32_t)((uint32_t)l->v + (uint32_t)k);
        NEXT();
    }
    if (l->tag == T_FIX) {
        l->v = wrap32((int64_t)l->v + (int64_t)k * 65536);
        NEXT();
    }
    if (l->tag != T_STR)
        FAIL("+ needs numbers or strings, not a ", vm_typename(l->tag));
    sp[0] = *l;
    sp[1] = mk(T_INT, k);
    sp += 2;
    SYNC();
    if (!concat(vm, l, sp[-2], sp[-1]))
        goto fail;
    RELOAD();
    sp -= 2;
    NEXT();
}
op_PUSHF:
    *sp++ = mk(T_FIX, S32());
    NEXT();

op_ADD:
    if (sp[-2].tag == T_INT && sp[-1].tag == T_INT) {
        sp[-2].v = (int32_t)((uint32_t)sp[-2].v + (uint32_t)sp[-1].v);
        sp--;
        NEXT();
    }
    if (fix_fast(&sp[-2], &sp[-1], OP_ADD)) {
        sp--;
        NEXT();
    }
    a = sp[-2]; b = sp[-1];
    if (is_num(a) && is_num(b)) {
        vm_fix_op(vm, OP_ADD, a, b, &sp[-2]);
        sp--;
        NEXT();
    }
    if (a.tag != T_STR && b.tag != T_STR)
        FAIL("+ needs numbers or strings, not a ",
             vm_typename(is_num(a) ? b.tag : a.tag));
    SYNC();
    if (!concat(vm, &sp[-2], a, b))
        goto fail;
    RELOAD();
    sp--;
    NEXT();

/* units on top of an op's own, from the budget: as vm_charge() takes them
 * when the slice has fewer left, so it still ends where it should */
#define EXTRA(n) do { \
    if (budget > (n)) \
        budget -= (n); \
    else { \
        SYNC(); \
        vm_charge(vm, (n)); \
        RELOAD(); \
    } \
} while (0)

/* What a divide or remainder with a fraction costs beyond its op,
 * measured on the rig (cal, vmbench): about 440 cycles a divide, and a
 * 64-bit remainder some 600 */
#define FIXCOST_SUB 0
#define FIXCOST_MUL 0
#define FIXCOST_DIV 9
#define FIXCOST_MOD 13

/* Arithmetic: integers inline, anything with a fraction through
 * vm_fix_op, anything else an error. */
#define ARITH(name, what, expr) \
op_##name: \
    if (sp[-2].tag == T_INT && sp[-1].tag == T_INT) { \
        const int32_t av = sp[-2].v, bv = sp[-1].v; \
        expr; \
        sp--; \
        NEXT(); \
    } \
    if (fix_fast(&sp[-2], &sp[-1], OP_##name)) { \
        sp--; \
        if (FIXCOST_##name) \
            EXTRA(FIXCOST_##name); \
        NEXT(); \
    } \
    a = sp[-2]; b = sp[-1]; \
    if (!is_num(a) || !is_num(b)) \
        FAIL(what " needs numbers, not a ", \
             vm_typename(is_num(a) ? b.tag : a.tag)); \
    if (!vm_fix_op(vm, OP_##name, a, b, &sp[-2])) \
        goto fail; \
    sp--; \
    if (FIXCOST_##name) \
        EXTRA(FIXCOST_##name); \
    NEXT();

ARITH(SUB, "-", sp[-2].v = (int32_t)((uint32_t)av - (uint32_t)bv))
ARITH(MUL, "*", sp[-2].v = (int32_t)((uint32_t)av * (uint32_t)bv))
ARITH(DIV, "/",
      if (bv == 0) FAIL("division by zero", "");
      sp[-2].v = bv == -1 ? (int32_t)(0u - (uint32_t)av) : av / bv)
ARITH(MOD, "%",
      if (bv == 0) FAIL("division by zero", "");
      sp[-2].v = bv == -1 ? 0 : av % bv)

op_NEG:
    if (sp[-1].tag == T_INT)
        sp[-1].v = (int32_t)(0u - (uint32_t)sp[-1].v);
    else if (sp[-1].tag == T_FIX)
        sp[-1].v = wrap32(-(int64_t)sp[-1].v);
    else
        FAIL("- needs a number, not a ", vm_typename(sp[-1].tag));
    NEXT();

/* Bitwise: a fraction is truncated to an integer first, as in JS. */
#define BITS(name, what, expr) \
op_##name: { \
    a = sp[-2]; b = sp[-1]; \
    if (!is_num(a) || !is_num(b)) \
        FAIL(what " needs numbers, not a ", \
             vm_typename(is_num(a) ? b.tag : a.tag)); \
    int32_t x = to_int(a), y = to_int(b); \
    sp[-2] = mk(T_INT, (expr)); \
    sp--; \
    NEXT(); \
}

BITS(BAND, "&", x & y)
BITS(BOR, "|", x | y)
BITS(BXOR, "^", x ^ y)
BITS(SHL, "<<", (int32_t)((uint32_t)x << (y & 31)))
BITS(SHR, ">>", x >> (y & 31))
BITS(USHR, ">>>", (int32_t)((uint32_t)x >> (y & 31)))

op_BNOT:
    if (!is_num(sp[-1]))
        FAIL("~ needs a number, not a ", vm_typename(sp[-1].tag));
    sp[-1] = mk(T_INT, ~to_int(sp[-1]));
    NEXT();
/* Two of one kind but different strings are equal by their place alone;
 * strings are compared at a charge for their length, so the registers go
 * over for them */
#define EQUAL(x, y, r) do { \
    if ((x).tag == (y).tag && ((x).tag != T_STR || (x).v == (y).v)) \
        r = (x).v == (y).v; \
    else if ((x).tag == T_STR && (y).tag == T_STR) { \
        SYNC(); \
        r = equal(vm, x, y); \
        RELOAD(); \
    } else \
        r = equal(vm, x, y); \
} while (0)
op_EQ:
    EQUAL(sp[-2], sp[-1], n);
    sp[-2] = mk(T_INT, (int32_t)n);
    sp--;
    NEXT();
op_NE:
    EQUAL(sp[-2], sp[-1], n);
    sp[-2] = mk(T_INT, !n);
    sp--;
    NEXT();

#define COMPARE(name, op, text) \
op_##name: \
    if (sp[-2].tag == T_INT && sp[-1].tag == T_INT) { \
        sp[-2].v = sp[-2].v op sp[-1].v; \
        sp--; \
        NEXT(); \
    } \
    a = sp[-2]; b = sp[-1]; \
    if (is_num(a) && is_num(b)) \
        sp[-2] = mk(T_INT, scaled(a) op scaled(b)); \
    else if (a.tag == T_STR && b.tag == T_STR) { \
        SYNC(); \
        sp[-2] = mk(T_INT, str_cmp(vm, a, b) op 0); \
        RELOAD(); \
    } else \
        FAIL(text " needs two numbers or two strings, not a ", \
             vm_typename(is_num(a) || a.tag == T_STR ? b.tag : a.tag)); \
    sp--; \
    NEXT();
COMPARE(LT, <, "<")
COMPARE(LE, <=, "<=")
COMPARE(GT, >, ">")
COMPARE(GE, >=, ">=")

/* Compare and jump: integers inline, then fractions, then strings. */
#define JCOMPARE(name, op, k) \
op_##name: { \
    int32_t off = S16(); \
    sp -= 2; \
    if (sp[0].tag == T_INT && sp[1].tag == T_INT) { \
        if (sp[0].v op sp[1].v) \
            pc += off; \
        NEXT(); \
    } \
    SYNC(); \
    int hold = compare_slow(vm, &sp[0], &sp[1], k); \
    RELOAD(); \
    if (hold < 0) \
        goto fail; \
    if (hold) \
        pc += off; \
    NEXT(); \
}
JCOMPARE(JLT, <, 0)
JCOMPARE(JLE, <=, 1)
JCOMPARE(JGT, >, 2)
JCOMPARE(JGE, >=, 3)

/* A local against a small integer, and jump: LDL, PUSHI8 and a JCOMPARE */
#define JCOMPARE_LI(name, op) \
op_##name: { \
    a = lp[U8()]; \
    int32_t k = S8(); \
    int32_t off = S16(); \
    if (a.tag == T_INT) { \
        if (a.v op k) \
            pc += off; \
        NEXT(); \
    } \
    b = mk(T_INT, k); \
    if (!is_num(a)) \
        FAIL("a comparison needs two numbers or two strings, not a ", \
             vm_typename(a.tag == T_STR ? b.tag : a.tag)); \
    if (scaled(a) op scaled(b)) \
        pc += off; \
    NEXT(); \
}
JCOMPARE_LI(JLTLI, <)
JCOMPARE_LI(JLELI, <=)
JCOMPARE_LI(JGTLI, >)
JCOMPARE_LI(JGELI, >=)

op_JEQ:
op_JNE: {
    int ne = pc[-1] == OP_JNE;
    int32_t off = S16();
    sp -= 2;
    EQUAL(sp[0], sp[1], n);
    if ((int)n != ne)
        pc += off;
    NEXT();
}

op_NOT:
    sp[-1] = mk(T_INT, !truthy(vm, sp[-1]));
    NEXT();
op_TYPEOF: {
    uint32_t t = sp[-1].tag, k = is_num(sp[-1]) ? 0 : t == T_STR ? 1
                                    : t == T_FN ? 3 : 2;
    sp[-1] = mk(T_STR, (int32_t)vm->s_typeof[k]);
    NEXT();
}

op_JMP: {
    int32_t off = S16();
    pc += off;
    NEXT();
}
op_JF: {
    int32_t off = S16();
    if (!truthy(vm, *--sp))
        pc += off;
    NEXT();
}
op_JT: {
    int32_t off = S16();
    if (truthy(vm, *--sp))
        pc += off;
    NEXT();
}
op_ANDJ: {
    int32_t off = S16();
    if (!truthy(vm, sp[-1]))
        pc += off;
    else
        sp--;
    NEXT();
}
op_ORJ: {
    int32_t off = S16();
    if (truthy(vm, sp[-1]))
        pc += off;
    else
        sp--;
    NEXT();
}

op_CALL: {
    /* as enter() does it, but with the arguments the arity always, so
     * none to pad or drop, and without the call */
    n = U16();
    const struct fninfo *f = &vm->fns[n];
    SYNC();
    if (vm->nframes >= MAX_FRAMES ||
        (uint32_t)(sp - vm->stack) + f->nlocals + f->maxstack + STACK_SLACK >
        STACK_VALUES)
        FAIL("too much recursion", "");
    fr = &vm->frames[vm->nframes++];
    fr->fn = n;
    fr->bp = (uint32_t)(sp - f->arity - vm->stack);
    fr->drop = 0;
    fr->pc = f->off;
    for (uint32_t i = f->arity; i < f->nlocals; i++)
        *sp++ = mk(T_NULL, 0);
    vm->sp = sp;
    pc = code + f->off;
    lp = vm->stack + fr->bp;
    NEXT();
}
op_CALLV:
    n = U8();
    if (sp[-(int)n - 1].tag != T_FN)
        FAIL("can't call a ", vm_typename(sp[-(int)n - 1].tag));
    SYNC();
    if (!enter(vm, (uint32_t)sp[-(int)n - 1].v, n, 1))
        goto fail;
    fr = &vm->frames[vm->nframes - 1];
    pc = code + fr->pc;
    lp = vm->stack + fr->bp;
    sp = vm->sp;
    NEXT();

op_RET:
    a = sp[-1];
    goto leave;
op_RETNULL:
    a = mk(T_NULL, 0);
leave:
    fr = &vm->frames[--vm->nframes];
    sp = vm->stack + fr->bp - fr->drop;
    *sp++ = a;
    if (vm->nframes == stop) {
        vm->sp = sp;
        vm->budget = budget;
        return 1;
    }
    fr = &vm->frames[vm->nframes - 1];
    pc = code + fr->pc;
    lp = vm->stack + fr->bp;
    NEXT();

op_NCALL: {
    uint32_t slot = U16(), argc = U8();
    /* the small maths given numbers, as core_math.c works them, without
     * the call: about 105 cycles round 25 on the rig. Anything else goes
     * to the native, which reports it. */
    switch (slot) {
    case INLINE_FLR:
        if (is_num(sp[-1])) {
            sp[-1] = mk(T_INT, floor_int(sp[-1]));
            NEXT();
        }
        break;
    case INLINE_ABS:
        if (is_num(sp[-1])) {
            if (sp[-1].v < 0)
                sp[-1].v = (int32_t)(0u - (uint32_t)sp[-1].v);
            NEXT();
        }
        break;
    case INLINE_MIN:
    case INLINE_MAX:
        if (is_num(sp[-2]) && is_num(sp[-1])) {
            int64_t x = scaled(sp[-2]), y = scaled(sp[-1]);
            if (slot == INLINE_MIN ? y < x : y > x)
                sp[-2] = sp[-1];
            sp--;
            NEXT();
        }
        break;
    case INLINE_MID:
        if (is_num(sp[-3]) && is_num(sp[-2]) && is_num(sp[-1])) {
            struct value x = sp[-3], y = sp[-2], z = sp[-1], t;
            if (scaled(x) > scaled(y)) {
                t = x;
                x = y;
                y = t;
            }
            if (scaled(y) > scaled(z))
                y = z;
            sp[-3] = scaled(x) > scaled(y) ? x : y;
            sp -= 2;
            NEXT();
        }
        break;
    case INLINE_SGN:
        if (is_num(sp[-1])) {
            sp[-1] = mk(T_INT, sp[-1].v < 0 ? -1 : 1);
            NEXT();
        }
        break;
    }
    struct value out = mk(T_NULL, 0);
    int32_t outer = vm->native;
    SYNC();
    vm->native = (int32_t)slot;
    if (!arcade_impl[slot](vm, sp - argc, (int)argc, &out))
        goto fail;
    vm->native = outer;
    RELOAD();
    sp -= argc;
    if (arcade_api[slot].ret)
        *sp++ = out;
    NEXT();
}

op_NEWARR: {
    struct value out;
    n = U16();
    SYNC();
    if (!vm_new_array(vm, n, &out))
        goto fail;
    if (n)
        memcpy(BUF(vm, ARR(vm, out)->buf)->v, sp - n, n * sizeof(struct value));
    sp -= n;
    *sp++ = out;
    NEXT();
}
op_NEWI32:
op_NEWFXA:
op_NEWU8: {
    int is32 = pc[-1] != OP_NEWU8;
    uint32_t kind = pc[-1] == OP_NEWI32 ? T_I32 : is32 ? T_FXA : T_U8;
    if (!is_num(sp[-1]) || to_int(sp[-1]) < 0)
        FAIL(kind == T_I32 ? "new Int32Array() needs a length, not a "
             : kind == T_FXA ? "new FixArray() needs a length, not a "
                             : "new Uint8Array() needs a length, not a ",
             is_num(sp[-1]) ? "negative number" : vm_typename(sp[-1].tag));
    n = (uint32_t)to_int(sp[-1]);
    if (n > (vm->heap_hi - vm->heap_lo) / (is32 ? 4 : 1))
        FAIL("out of memory", "");
    SYNC();
    uint32_t off = gc_alloc(vm, kind,
                            (uint32_t)(sizeof(struct i32) - sizeof(struct hdr))
                            + n * (is32 ? 4 : 1));
    if (!off)
        goto fail;
    ((struct i32 *)MEM(vm, off))->len = n;
    sp[-1] = mk(kind, (int32_t)off);
    /* cleared at the speed of memory */
    vm_charge(vm, n * (is32 ? 4 : 1) / BYTES_PER_UNIT);
    RELOAD();
    NEXT();
}
op_NEWOBJ: {
    struct value out;
    uint32_t shape = vm->cshape[U16()];
    n = vm->shapes[shape].nkeys;
    SYNC();
    if (!new_obj(vm, shape, sp - n, &out))
        goto fail;
    sp -= n;
    *sp++ = out;
    NEXT();
}
op_GETIDX: {
    if (!index_fast(vm, &sp[-2], &sp[-1], &sp[-2])) {
        struct value got;
        SYNC();
        if (!get_index(vm, sp[-2], sp[-1], &got))
            goto fail;
        sp[-2] = got;
    }
    sp--;
    NEXT();
}
op_GETGL: {
    const struct value *c = &vm->globals[U16()], *i = &lp[U8()];
    if (!index_fast(vm, c, i, sp)) {
        struct value got;
        SYNC();
        if (!get_index(vm, *c, *i, &got))
            goto fail;
        *sp = got;
    }
    sp++;
    NEXT();
}
op_SETIDX:
    if (!store_fast(vm, &sp[-3], &sp[-2], &sp[-1])) {
        SYNC();
        if (!set_index(vm, sp[-3], sp[-2], sp[-1]))
            goto fail;
    }
    sp -= 3;
    NEXT();
op_GETPROP: {
    uint32_t key = U16(), site = U16();
    if (sp[-1].tag != T_OBJ) {
        no_field(vm, "can't read .", sp[-1], key);
        goto fail;
    }
    /* the cache's hit here, without the call */
    struct obj *ob = OBJ(vm, sp[-1]);
    if (vm->ic[site].shape == ob->shape)
        sp[-1] = BUF(vm, ob->buf)->v[vm->ic[site].slot];
    else
        sp[-1] = obj_get(vm, sp[-1], key, site);
    NEXT();
}
op_GETGP: {
    struct value o = vm->globals[U16()];
    uint32_t key = U16(), site = U16();
    if (o.tag != T_OBJ) {
        no_field(vm, "can't read .", o, key);
        goto fail;
    }
    struct obj *ob = OBJ(vm, o);
    if (vm->ic[site].shape == ob->shape)
        *sp++ = BUF(vm, ob->buf)->v[vm->ic[site].slot];
    else
        *sp++ = obj_get(vm, o, key, site);
    NEXT();
}
op_SETPROP: {
    uint32_t key = U16(), site = U16();
    if (sp[-2].tag != T_OBJ) {
        no_field(vm, "can't set .", sp[-2], key);
        goto fail;
    }
    /* the cache's hit here, without the call */
    struct obj *ob = OBJ(vm, sp[-2]);
    if (vm->ic[site].shape == ob->shape) {
        struct value *e = &BUF(vm, ob->buf)->v[vm->ic[site].slot];
        e->tag = sp[-1].tag;
        e->v = sp[-1].v;
        sp -= 2;
        NEXT();
    }
    SYNC();
    if (!obj_set(vm, sp[-2], key, site, sp[-1]))
        goto fail;
    sp -= 2;
    NEXT();
}
op_LEN:
    a = sp[-1];
    switch (a.tag) {
    case T_STR: n = STR(vm, a)->len; break;
    case T_ARR: n = ARR(vm, a)->len; break;
    case T_I32: case T_FXA: n = I32(vm, a)->len; break;
    case T_U8: n = U8A(vm, a)->len; break;
    default: FAIL(".length of a ", vm_typename(a.tag));
    }
    sp[-1] = mk(T_INT, (int32_t)n);
    NEXT();
op_METHOD: {
    uint32_t m = U8(), argc = U8();
    SYNC();
    if (!method(vm, (int)m, sp - argc - 1, sp - argc, (int)argc))
        goto fail;
    RELOAD();
    sp -= argc;
    NEXT();
}
op_INTR: {
    uint32_t id = U8(), argc = U8();
    SYNC();
    struct value got;
    if (!intrinsic(vm, (int)id, sp - argc, (int)argc, &got))
        goto fail;
    RELOAD();
    sp -= argc;
    *sp++ = got;
    NEXT();
}

fail:
    if (!vm->err_where) {
        fr = &vm->frames[vm->nframes - 1];
        vm->err_where = 1;
        vm->err_fn = fr->fn;
        vm->err_pc = (uint32_t)(pc - code) - vm->fns[fr->fn].off - 1;
    }
    vm->sp = sp;
    return 0;
}

/* ---- the host's calls -------------------------------------------------- */

static int call_entry(struct vm *vm, int which)
{
    struct value r;
    uint32_t fi = vm->entry[which];
    if (vm->err)
        return 0;
    if (fi == NONE)
        return 1;
    vm->sp = vm->stack;
    vm->nframes = 0;
    if (!vm_call(vm, mk(T_FN, (int32_t)fi), 0, &r))
        return 0;
    return 1;
}

static void begin(struct vm *vm)
{
    vm->units = 0;
    if (!vm->budget || vm->budget > YIELD_UNITS)
        vm->budget = YIELD_UNITS;
    vm->slice = vm->budget;
}

static void end(struct vm *vm)
{
    vm->units += vm->slice - vm->budget;
}

int arcade_menu_choose(struct vm *vm, int i, int button)
{
    struct value r;
    if (vm->err)
        return -1;
    if (i < 1 || i > 5 || vm->menu_fn[i - 1].tag != T_FN)
        return 0;
    begin(vm);
    vm->sp = vm->stack;
    vm->nframes = 0;
    *vm->sp++ = mk(T_INT, button);
    int ok = vm_call(vm, vm->menu_fn[i - 1], 1, &r);
    end(vm);
    if (!ok)
        return -1;
    return vm_truthy(vm, r) ? 1 : 0;
}

/* init() may run this many times a tick's ceiling: a cart's one-off
 * setup -- a generated world, a big noise.fill -- is not a hung loop */
#define INIT_CEILINGS 4

int arcade_end(struct vm *vm)
{
    if (vm->err || vm->entry[4] == NONE)
        return !vm->err;
    begin(vm);
    int ok = call_entry(vm, 4);
    end(vm);
    return ok;
}

int arcade_start(struct vm *vm)
{
    uint32_t tick_ceiling = vm->ceiling;
    uint32_t setup = tick_ceiling <= 0xffffffffu / INIT_CEILINGS
                     ? tick_ceiling * INIT_CEILINGS : 0xffffffffu;
    vm->ceiling = setup;
    begin(vm);
    int ok = call_entry(vm, 0) && call_entry(vm, 1);
    end(vm);
    /* back to a tick's, unless the host lowered it meanwhile (to stop
     * a call running too long by the clock) */
    if (vm->ceiling == setup)
        vm->ceiling = tick_ceiling;
    return ok;
}

/* The beat analyser's work while a cart listens to the music: about a
 * third of the 5G's time playing, at 1.6 us a unit there. It runs in the player's game thread between
 * ticks, so a tick reports it with the cart's own units -- the cost the
 * IDE's meter shows -- though it counts nothing toward the ceiling. */
#define ANALYSER_UNITS_PER_SECOND 208000

int arcade_tick(struct vm *vm, int draw)
{
    begin(vm);
    vm->frame++;
    /* the first tick reports the options as changed, so a cart reads them
     * where it reads a change */
    vm->opt_changed = vm->opt_pending || vm->frame == 1;
    vm->opt_pending = 0;
    int ok = call_entry(vm, 2) && (!draw || call_entry(vm, 3));
    vm->opt_changed = 0;
    end(vm);
    if (vm->listening && vm->host && vm->host->playing &&
        vm->host->playing(vm->host->ctx))
        vm->units += ANALYSER_UNITS_PER_SECOND / vm->rate;
    return ok;
}

void arcade_idle(struct vm *vm)
{
    if (!vm->err && vm->since_gc > vm->gc_threshold) {
        vm->sp = vm->stack;
        gc_collect(vm);
    }
}

uint32_t arcade_units(const struct vm *vm)
{
    return vm->units;
}

int vm_out_ok(struct vm *vm, struct value o, uint32_t need)
{
    if (o.tag == T_ARR)
        return ARR(vm, o)->len >= need;
    if (o.tag == T_I32 || o.tag == T_FXA)
        return I32(vm, o)->len >= need;
    return 0;
}

void vm_out_set(struct vm *vm, struct value o, uint32_t i, struct value v)
{
    if (o.tag == T_ARR)
        BUF(vm, ARR(vm, o)->buf)->v[i] = v;
    else if (o.tag == T_FXA)
        I32(vm, o)->d[i] = to_fix(v);
    else
        I32(vm, o)->d[i] = v.tag == T_FIX ? v.v >> 16 : v.v;
}

uint32_t arcade_units_now(const struct vm *vm)
{
    return vm->units + (vm->slice - vm->budget);
}

int arcade_where(const struct vm *vm, uint32_t *fns, int max, uint32_t *pc,
                 int32_t *native)
{
    uint32_t n = vm->nframes;
    *native = vm->in_code ? -1 : vm->native;
    *pc = 0;
    if (!n)
        return 0;
    const struct frame *top = &vm->frames[n - 1];
    uint32_t off = vm->fns[top->fn].off;
    *pc = top->pc > off ? top->pc - off - 1 : 0;
    for (int i = 0; i < max && (uint32_t)i < n; i++)
        fns[i] = vm->frames[n - 1 - (uint32_t)i].fn;
    return (int)n;
}

void arcade_set_ceiling(struct vm *vm, uint32_t units)
{
    vm->ceiling = units;
}

const void *arcade_snapshot(const struct vm *vm, uint32_t *size)
{
    *size = vm->size;
    return vm->mem;
}

uint32_t arcade_cart_hash(const struct vm *vm)
{
    return vm->cart_hash;
}

int arcade_owns_music(const struct vm *vm)
{
    return vm->music == 2;
}

int arcade_suspends(const struct vm *vm)
{
    return !vm->no_suspend;
}

int arcade_between_calls(const struct vm *vm)
{
    return !vm->err && vm->nframes == 0;
}

struct vm *arcade_resume(void *mem, uint32_t size,
                         const struct arcade_host *host)
{
    struct vm *vm = mem;
    if (size < sizeof(struct vm) || vm->mem != mem || vm->size != size ||
        vm->err)
        return NULL;
    /* what belongs to the tick it was taken after, and to the host */
    vm->host = host;
    vm->sp = vm->stack;
    vm->nframes = 0;
    vm->ntmp = 0;
    vm->units = 0;
    vm->opt_pending = vm->opt_changed = 0;
    vm->show_board = 0;
    vm->d.dirty_lo = 0;
    vm->d.dirty_hi = SCREEN_H - 1;
    vm->d.palette_changed = 1;
    return vm;
}

const int32_t *arcade_save(struct vm *vm, int *dirty,
                           const uint8_t **fractions)
{
    if (dirty)
        *dirty = vm->save_dirty;
    if (fractions)
        *fractions = vm->save_frac;
    vm->save_dirty = 0;
    return vm->save;
}

void arcade_restore(struct vm *vm, const int32_t save[256],
                    const uint8_t fractions[32])
{
    memcpy(vm->save, save, sizeof(vm->save));
    memcpy(vm->save_frac, fractions, sizeof(vm->save_frac));
}
