/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: the VM's internal structures, shared by the interpreter, the
 * heap, the loader, the verifier and the natives. Hosts include arcade.h.
 *
 * Everything lives in one arena the host hands over: struct vm at its
 * start, then the loaded tables ("perm", bump-allocated and never freed),
 * then the garbage-collected heap. Values refer to arena objects by
 * offset, never by pointer.
 ***************************************************************************/
#ifndef ARCADE_VM_H
#define ARCADE_VM_H

#include <stdint.h>
#include "games/arcade/arcade.h"

/* Value tags. T_BUF is internal: the element store behind arrays and
 * records, never a value a cart sees. T_FIX is a fraction, 16.16. T_IMG
 * is an image made by img(). */
enum
{
    T_NULL, T_INT, T_STR, T_ARR, T_I32, T_U8, T_OBJ, T_FN, T_BUF, T_FIX,
    T_IMG
};

struct value
{
    uint32_t tag;
    int32_t v;          /* the integer, an arena offset, or a function */
};

/* Every heap object starts with this. info: type in bits 0-7, flags in
 * 8-15, and for a string a key id + 1 in 16-31 (0: not yet a key). */
struct hdr
{
    uint32_t info;
    uint32_t size;      /* bytes, header included, a multiple of 8 */
};

#define GC_MARK  0x100
#define GC_FREE  0x200
#define GC_PERM  0x400  /* in the heap but never collected */

struct str { struct hdr h; uint32_t len; uint8_t b[]; };
struct buf { struct hdr h; struct value v[]; };
/* An array, and a record: a shape and that many values in buf. */
struct arr { struct hdr h; uint32_t len; uint32_t cap; uint32_t buf; };
struct obj { struct hdr h; uint32_t shape; uint32_t cap; uint32_t buf; };
struct i32 { struct hdr h; uint32_t len; int32_t d[]; };
struct u8a { struct hdr h; uint32_t len; uint8_t d[]; };
struct img
{
    struct hdr h;
    uint16_t width, height;
    uint32_t pad;
    uint8_t px[];
};

/* A record's shape is its key list, as a chain: this shape is its parent
 * plus one key, which sits in slot nkeys - 1. Shape 0 has no keys. */
struct shape
{
    uint16_t parent, key, nkeys, child, sibling;
};

struct fninfo
{
    uint8_t arity, nlocals;
    uint16_t maxstack;
    uint32_t off, len;
};

struct frame
{
    uint32_t fn;
    uint32_t pc;        /* where this function resumes, as a code offset */
    uint32_t bp;        /* its first local, as a stack index */
    uint32_t drop;      /* 1 when CALLV left the function value below */
};

struct ic { uint16_t shape, slot; };

enum
{
#define OP(name, operands, pops, pushes) OP_##name,
#define MTH(name, min, max)
#define INTRINSIC(name, min, max)
#include "games/arcade/opcodes.def"
#undef OP
#undef MTH
#undef INTRINSIC
    OP_COUNT
};

enum
{
#define OP(name, operands, pops, pushes)
#define MTH(name, min, max) M_##name,
#define INTRINSIC(name, min, max)
#include "games/arcade/opcodes.def"
#undef OP
#undef MTH
#undef INTRINSIC
    M_COUNT
};

enum
{
#define OP(name, operands, pops, pushes)
#define MTH(name, min, max)
#define INTRINSIC(name, min, max) I_##name,
#include "games/arcade/opcodes.def"
#undef OP
#undef MTH
#undef INTRINSIC
    I_COUNT
};

struct vm;
typedef int (*native_fn)(struct vm *vm, struct value *a, int argc,
                         struct value *out);

struct native_def
{
    uint8_t min, max, ret, parts, level, cost;
    uint16_t version;
};

extern const struct native_def arcade_api[];
extern const native_fn arcade_impl[];
extern const uint32_t arcade_api_count;

/* The surfaces, all 8-bit palette indices. */
#define SCREEN_W     160
#define SCREEN_H     120
#define SHEET_W      128
#define SHEET_H      128
#define MAP_W        128
#define MAP_H        64
#define PIXELS_PER_UNIT 8   /* drawing's charge, until phase 1 measures it */

/* What drawing reads and changes; PICO-8's draw state, plus the targets. */
struct draw
{
    struct value target;        /* 0 the screen, or an img */
    int32_t cam_x, cam_y;
    int32_t clip_x0, clip_y0, clip_x1, clip_y1;   /* end exclusive */
    int32_t cur_x, cur_y;
    uint16_t pen;               /* low byte the colour, high byte fillp's */
    uint16_t fill;
    uint8_t font;
    uint8_t dpal[256];          /* drawing: colour -> colour */
    uint8_t spal[256];          /* the screen: index -> entry shown */
    uint8_t transparent[32];
    uint32_t rgb[256];          /* each entry as 0xRRGGBB */
    uint32_t shown[256];        /* rgb[spal[i]], what present uses */
    int palette_changed;
    int16_t dirty_lo, dirty_hi;  /* the screen's rows drawn */
};

#define MAX_SHAPES   2048
#define MAX_RTKEYS   1024   /* string keys made at run time */
#define MAX_FRAMES   200
#define STACK_VALUES 4096
#define MARK_STACK   1024
#define FREE_CLASSES 64     /* exact lists for blocks of 16..504 bytes */
#define YIELD_UNITS  2048

struct vm
{
    uint8_t *mem;
    uint32_t size;
    const struct arcade_host *host;

    /* The cart, as loaded */
    uint8_t rate, menu_button, music;
    uint16_t api;
    uint8_t id[8];
    const uint8_t *meta;
    uint32_t meta_len;
    const uint8_t *code;
    struct fninfo *fns;
    uint32_t nfns;
    uint32_t entry[4];          /* $start, init, update, draw; or NONE */
    uint32_t *strings;          /* offsets of the constant strings */
    uint32_t nstrings;
    uint16_t *cshape;           /* a CNST shape's runtime shape */
    uint32_t ncshapes;
    struct shape *shapes;
    uint32_t nshapes;
    struct ic *ic;
    uint32_t nic;
    struct value *globals;
    uint32_t nglobals;
    const uint8_t *lines;
    uint32_t lines_len;

    /* String keys: id -> string offset, and a hash of contents -> id + 1 */
    uint32_t *keyoff;
    uint32_t nkeys;
    uint16_t *keyhash;
    uint32_t hashmask;

    /* Running */
    struct value *stack;
    struct value *sp;
    struct frame *frames;
    uint32_t nframes;
    struct value tmp[4];        /* roots for natives between allocations */
    uint32_t ntmp;
    uint32_t s_typeof[4];       /* "number", "string", "object", "function" */

    /* The heap */
    uint32_t perm;              /* next free byte of the loaded tables */
    uint32_t heap_lo, heap_hi, bump;
    uint32_t freelist[FREE_CLASSES];
    uint32_t freebig;
    uint32_t since_gc, gc_threshold;
    uint32_t *mstack;
    uint32_t msp;
    int moverflow;

    /* Cost */
    uint32_t units;             /* in the current call from the host */
    uint32_t budget;            /* until the next yield */
    uint32_t ceiling;

    /* The core's state */
    struct draw d;
    uint32_t screen;            /* its pixels */
    uint32_t sheet, flags, map;
    uint32_t frame;
    uint8_t held, pressed;      /* btn() and btnp() bits */
    uint16_t held_ticks[5];
    int32_t wheel, wheelpos;
    int quit;
    struct value menu_label[5], menu_fn[5];   /* menuitem(); roots */
    uint64_t rng_state, rng_inc;
    int32_t save[256];
    uint8_t save_frac[32];      /* which saved values are fractions */
    int save_dirty;

    /* The first error; once set, the cart has stopped */
    int err;
    uint32_t err_fn, err_pc;
    int err_where;              /* err_fn and err_pc are meaningful */
    char msg[112];
};

#define NONE 0xffffu

#define MEM(vm, off) ((void *)((vm)->mem + (off)))
#define STR(vm, x)   ((struct str *)MEM(vm, (uint32_t)(x).v))
#define ARR(vm, x)   ((struct arr *)MEM(vm, (uint32_t)(x).v))
#define OBJ(vm, x)   ((struct obj *)MEM(vm, (uint32_t)(x).v))
#define I32(vm, x)   ((struct i32 *)MEM(vm, (uint32_t)(x).v))
#define U8A(vm, x)   ((struct u8a *)MEM(vm, (uint32_t)(x).v))
#define BUF(vm, off) ((struct buf *)MEM(vm, off))

static inline struct value mk(uint32_t tag, int32_t v)
{
    struct value r;
    r.tag = tag;
    r.v = v;
    return r;
}

/* Numbers. An integer and a fraction meet as 64-bit 16.16, where an
 * integer n is n * 65536 exactly; results wrap to 32 bits. These are the
 * rules tools/arcade/src/numbers.js states for the compiler. */
static inline int is_num(struct value v)
{
    return v.tag == T_INT || v.tag == T_FIX;
}

static inline int64_t scaled(struct value v)
{
    return v.tag == T_FIX ? (int64_t)v.v : (int64_t)v.v * 65536;
}

static inline int32_t wrap32(int64_t x)
{
    return (int32_t)(uint32_t)(uint64_t)x;
}

/* A fraction toward zero, as bitwise operators and typed arrays take it. */
static inline int32_t to_int(struct value v)
{
    if (v.tag != T_FIX)
        return v.v;
    return v.v >= 0 ? v.v >> 16 : -(int32_t)((-(int64_t)v.v) >> 16);
}

/* A fraction rounded down, as an index or a pixel coordinate takes it. */
static inline int32_t floor_int(struct value v)
{
    return v.tag == T_FIX ? v.v >> 16 : v.v;
}

/* vm.c */
int vm_fail(struct vm *vm, const char *a, const char *b, const char *c);
int vm_call(struct vm *vm, struct value fn, int argc, struct value *out);
int vm_truthy(struct vm *vm, struct value v);
int vm_tostring(struct vm *vm, struct value v, struct value *out);
uint32_t vm_text(struct vm *vm, struct value v, char *dst, uint32_t cap);
int vm_fix_op(struct vm *vm, int op, struct value a, struct value b,
              struct value *out);
void vm_charge(struct vm *vm, uint32_t units);
const char *vm_typename(uint32_t tag);
int vm_new_string(struct vm *vm, const uint8_t *b, uint32_t len,
                  struct value *out);
int vm_new_array(struct vm *vm, uint32_t len, struct value *out);

/* gc.c */
void gc_init(struct vm *vm);
uint32_t gc_alloc(struct vm *vm, uint32_t type, uint32_t bytes);
void gc_collect(struct vm *vm);

/* loader.c */
uint32_t perm_alloc(struct vm *vm, uint32_t bytes);
uint32_t key_id(struct vm *vm, struct value s, int add);
uint32_t vm_shape_add(struct vm *vm, uint32_t parent, uint32_t key);

/* core_math.c */
void core_seed(struct vm *vm, uint64_t seed);

/* core_draw.c */
int core_draw_init(struct vm *vm);

/* verify.c */
int verify_cart(struct vm *vm);

#endif
