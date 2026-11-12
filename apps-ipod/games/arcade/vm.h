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

/* The player builds the assembly routines (the *_arm.S files); the C
 * beside each is the reference, and is what the simulator and pbgvm run */
#if defined(ROCKBOX) && !defined(SIMULATOR) && defined(__arm__)
#define ARCADE_ASM 1
#endif

/* Value tags. T_BUF is internal: the element store behind arrays and
 * records, never a value a cart sees. T_FIX is a fraction, 16.16. T_IMG
 * is an image made by img(). */
enum
{
    T_NULL, T_INT, T_STR, T_ARR, T_I32, T_U8, T_OBJ, T_FN, T_BUF, T_FIX,
    T_IMG,
    T_FXA,      /* a FixArray: an Int32Array's storage, each element 16.16 */
    T_LAZY      /* a cart image left in the file: its struct lazyimg */
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
#define MAP_W        128   /* the map of a cart with none of its own */
#define MAP_H        64
#define MAP_MAX      256   /* a cart map's widest and tallest */
#define IMAGE_MAX    1024  /* a cart image's widest and tallest */
#define IMG_MAX      4096  /* an img()'s */
/* A unit is about 40 of the 5G's cycles, a bytecode's worth. Filling and
 * moving memory goes at the speed of its SDRAM: a unit for every 16 bytes. */
#define BYTES_PER_UNIT 16

/* What drawing reads and changes; PICO-8's draw state, plus the targets. */
struct draw
{
    struct value target;        /* 0 the screen, or an img */
    struct value sheet;         /* 0 the cart's sheet, or an img: sheet() */
    int32_t cam_x, cam_y;
    /* camera()'s turn and zoom about its pivot: 16.16 turns (0 none),
     * the zoom 16.16, its cosine and sine times the zoom, and the pivot
     * on the target, 16.16; cam_moved when either moves a point */
    int32_t cam_turn, cam_zoom, cam_cos, cam_sin, cam_px, cam_py;
    int32_t cam_moved;
    int32_t clip_x0, clip_y0, clip_x1, clip_y1;   /* end exclusive */
    int32_t cur_x, cur_y;
    uint16_t pen;               /* low byte the colour, high byte fillp's */
    uint16_t fill;
    uint8_t font;               /* FONT.CAPS or FONT.WORLD; 1 is CAPS at 2 */
    uint8_t font_big;           /* font()'s size, less one */
    uint8_t dpal[256];          /* drawing: colour -> colour */
    uint8_t spal[256];          /* the screen: index -> entry shown */
    uint8_t transparent[32];
    uint16_t xlat[256];         /* dpal[c], or 0x100 if c is transparent */
    uint8_t xlat_stale;         /* set when either changes */
    uint8_t xlat_plain;         /* xlat[c] is c for every c: a copy */
    uint32_t rgb[256];          /* each entry as 0xRRGGBB */
    uint32_t shown[256];        /* rgb[spal[i]], what present uses */
    int palette_changed;
    int16_t dirty_lo, dirty_hi;  /* the screen's rows drawn */
};

#define LAND_BANDS 16

/* The plane's camera, from plane.view(): 16.16 world pixels, the heading
 * as the unit vector (dx, dy), and the focal length in screen pixels */
struct planeview
{
    int32_t x, y, z, dx, dy, focal;
    int32_t horizon;
    int32_t angle, fov;         /* turns, 16.16: plane.sky()'s */
    int set;
    /* walls.draw()'s distance at each screen column, 16.16 world pixels,
     * for billboards behind walls; kept until the next plane.view() */
    int depth_set;
    int32_t depth[SCREEN_W];
    /* land.draw()'s nearer ground at each screen column, for billboards
     * behind hills: the highest row drawn by the time its slices passed
     * each of LAND_BANDS depths; kept until the next plane.view() */
    int land_set;
    int16_t land_top[LAND_BANDS][SCREEN_W];
    /* plane.fog(): from near to far (16.16 world pixels) the ground goes
     * to colour, dithered, and the billboards too unless fog_billboards is
     * 0; kept until changed */
    int fog_on, fog_billboards;
    int32_t fog_near, fog_far;
    uint8_t fog_colour;
};

/* A cells field's 16x16 chunks: which are awake, and each sleeping one's
 * sum, to see it drawn on. Kept for the two fields stepped last. */
struct cellsleep
{
    uint32_t id, used;
    int32_t w, h;
    int32_t box[4];                 /* changed since the last step, x0 > x1
                                     * when nothing has */
    uint8_t awake[256];
    uint32_t sum[256];
    uint8_t dir[256 * 256 / 8];     /* a bit a cell: a liquid's way */
};

/* A voxel landscape to draw: a height and a colour image of one size,
 * powers of two so the world wraps, seen from the plane's camera */
struct landspec
{
    const uint8_t *height, *colour;
    uint32_t wmask, hmask, wshift;
    int32_t far;                /* world pixels */
    int32_t scale;              /* 16.16 world pixels a height step */
    int32_t coarse;             /* 2: every other column, drawn twice */
    const uint8_t *ramp;        /* NULL, or the colour at each height */
    int32_t nramp;
};

/* A row of the floor: its left end and one screen pixel's step, 16.16
 * world pixels */
struct floor_row { int32_t u, v, du, dv, fog; };

/* The wheel as the player's own lists take it, for arcade_scroll() */
struct arcade_scroll {
    int32_t carry;              /* clicks short of a step */
    int32_t dir;                /* the last step's way, 1 or -1 */
    uint32_t vel;               /* the smoothed speed, degrees a second */
    uint32_t quiet;             /* ms since the last step, up to the pause */
};

/* An effect: signed bytes, each times scale a 16-bit sample */
struct sfxinfo
{
    const int8_t *data;
    uint32_t n;
    int32_t scale;
};

/* A cart font, from the FONT chunk: an index of a u32 codepoint and a
 * u32 offset a glyph, in codepoint order, into the glyphs, each an
 * advance, x, y (from the line's top), w and h, then h rows of
 * (w + 7) / 8 bytes. FONT.<name> is 3 and on. */
#define MAX_CART_FONTS 16
#define FIRST_CART_FONT 3

struct cartfont
{
    const uint8_t *index, *glyphs;
    uint32_t n;
    uint8_t line;
};

/* A voice: an effect stepped through in 16.16 at its pitch, or a tone */
enum { VOICE_OFF, VOICE_SFX, VOICE_TONE };
struct voice
{
    uint8_t kind, wave, done;
    int16_t id;                 /* the effect playing, -2 a tone */
    int32_t s0, s1;             /* an effect's samples either side of frac */
    int32_t scale;
    const int8_t *data;
    uint32_t n, next;           /* samples, and the next to decode */
    uint32_t frac, step, pitch; /* 16.16 */
    uint32_t hz, phase, dphase, noise;
    int32_t vol;
    uint64_t left;              /* a tone's length left, 16.16 source samples */
};

#define MAX_SHAPES   2048
#define MAX_RTKEYS   1024   /* string keys made at run time */
#define MAX_FRAMES   200
#define STACK_VALUES 4096
#define STACK_SLACK  8      /* room above maxstack for INCL and sort */
#define MARK_STACK   1024
#define FREE_CLASSES 64     /* exact lists for blocks of 16..504 bytes */
#define YIELD_UNITS  2048

/* A chunk left in the cart file, read when it is used: where its bytes
 * start in the file, how many, and their checksum */
struct lazy
{
    uint32_t at, len, crc;
};
#define LAZY_MAX 64                 /* lazy maps, and lazy images */

struct vm
{
    uint8_t *mem;
    uint32_t size;
    const struct arcade_host *host;

    /* The cart, as loaded */
    uint8_t rate, menu_button, music;
    uint8_t no_suspend;         /* Quit keeps nothing to resume */
    uint16_t api;
    uint8_t id[8];
    const uint8_t *meta;
    uint32_t meta_len;
    /* High scores (sys_score.c): the boards, whether one changed since the
     * host last wrote them, and what score.show() asked for */
    struct arcade_scores scores;
    uint8_t scores_dirty;
    int8_t show_board, show_rank;           /* board + 1, 0 none */
    int8_t last_rank[ARCADE_BOARDS];        /* each board latest, 0 none */
    int8_t achieved[8];                     /* earned, for the alert */
    uint8_t nachieved;
    /* Options (sys_opt.c): an owner's change waits for the next tick, and
     * opt.changed() is true during it */
    struct arcade_options opts;
    uint8_t opt_pending, opt_changed;
    uint8_t opt_set;            /* opt.set() since the host last asked */
    uint8_t format;             /* the cart file's: 2 has ending() */
    int32_t beat_offset;        /* beat.offset(): ms taken off each press */
    uint32_t cart_hash;         /* the chunks' CRCs, folded: the cart's bytes */
    uint8_t listening;          /* beat.listen(true): the analyser runs */
    /* Dialogs (sys_ui.c): ui.style()'s colours -- the box, its text and
     * the highlight -- and the dialog showing, NULL none */
    uint8_t ui_box, ui_ink, ui_hi;
    struct dialog *dialog;
    const uint8_t *data;        /* DATA: the cart's data files */
    uint32_t data_len;
    const uint8_t *code;
    struct fninfo *fns;
    uint32_t nfns;
    uint32_t entry[5];          /* $start, init, update, draw, ending;
                                   or NONE */
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
    struct value tmp[8];        /* roots for natives between allocations */
    uint32_t ntmp;
    uint32_t sorting;           /* a .sort() is calling its comparator */
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
    /* the units the slice running holds: YIELD_UNITS, but for a call's
     * first, which carries on the last call's, so yields -- and the
     * profiler's samples -- fall at different places in each tick's work
     * rather than always at the same one */
    uint32_t slice;
    uint32_t ceiling;
    /* For a profiler at a yield: the API slot of the native running, or
     * -1; and whether the yield is the bytecode's own, between
     * instructions */
    int32_t native;
    uint8_t in_code;

    /* The core's state */
    struct draw d;
    uint32_t screen;            /* its pixels */
    uint32_t sheet, flags, map;
    int32_t map_w, map_h;       /* the active map's size */

    /* The cart's assets: its chunks as loaded, and what core_draw.c makes
     * of them */
    const uint8_t *pals, *shet, *maps, *imgs;
    uint32_t pals_len, shet_len, maps_len, imgs_len;
    struct mapinfo *mapdir;
    uint32_t nmaps;
    int32_t active_map;         /* -1 while the map is the empty default */
    uint16_t *img_globals;      /* pairs: a global's slot, an image's number */
    uint32_t nimg_globals;
    struct lazy *mapc, *imgc;       /* LAZY_MAX each, made as first needed */
    uint32_t nmapc, nimgc;
    struct lazyimg *lazyimgs;
    uint32_t nlazyimgs;
    const uint8_t *sfx;
    uint32_t sfx_len, nsfx;
    struct sfxinfo *sfxdir;
    struct cartfont fonts[MAX_CART_FONTS];
    uint32_t nfonts;
    struct voice voices[4];
    uint8_t muted;              /* the owner muted the game's sound */
    uint32_t actor_shape, particle_shape;   /* what makes a record a pool */
    struct planeview plane;
    uint32_t rules_shape;                   /* what makes a record rules */
    uint32_t links_shape;                   /* ... and links */
    uint32_t segs_shape;                    /* ... and segment sets */
    struct cellsleep sleep[2];
    uint32_t sleep_clock;
    uint32_t frame;
    int64_t t_base;             /* t() at frame_base, 16.16, for setrate() */
    uint32_t frame_base;
    uint32_t skipped;           /* frames not drawn since skipped() */
    uint8_t paused;             /* paused since resumed() */
    uint8_t held, pressed;      /* btn() and btnp() bits */
    uint8_t released;           /* btnr()'s: down last tick, up this one */
    uint16_t held_ticks[5];
    int32_t wheel, wheelpos;
    int32_t wheelsteps;         /* this tick's, by arcade_scroll() */
    struct arcade_scroll scroll;
    struct arcade_music heard;  /* this tick's, from arcade_music() */
    int32_t beat_inbar;         /* from the last beat.next() */
    int32_t beat_margin;        /* from the last beat.downbeat() */
    int quit;
    struct value menu_label[5], menu_fn[5];   /* menuitem(); roots */
    uint64_t rng_state;
    int32_t save[256];
    uint8_t save_frac[32];      /* which saved values are fractions */
    int save_dirty;

    /* The first error; once set, the cart has stopped */
    int err;
    uint32_t err_fn, err_pc;
    int err_where;              /* err_fn and err_pc are meaningful */
    char msg[112];
};

/* A cart map: its cells, LZ4 -- in the arena, or left in the cart file as
 * lazy chunk mapc[lazy] -- and where each object's record starts */
struct mapinfo
{
    uint16_t w, h, nobj;
    uint32_t zlen;
    const uint8_t *z;
    const uint8_t **obj;
    int32_t lazy;               /* -1 when z holds the cells */
};


/* A cart image left in the file: IMG.<name>'s global holds a T_LAZY of
 * this until img.load() puts there a T_IMG of the copy it makes */
struct lazyimg
{
    uint16_t w, h;
    uint16_t chunk, slot;           /* its imgc, and the global naming it */
    uint32_t copy;                  /* the loaded copy's offset, or 0 */
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
 * rules arcade/sdk/src/numbers.js states for the compiler. */
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

/* A number as a FixArray keeps it: 16.16, wrapping past 32767 */
static inline int32_t to_fix(struct value v)
{
    return v.tag == T_FIX ? v.v : (int32_t)((uint32_t)v.v << 16);
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

/* A cart wrote screen rows y0..y1 through mem(MEM.SCREEN), not by drawing */
static inline void screen_written(struct vm *vm, int32_t y0, int32_t y1)
{
    if (y0 < vm->d.dirty_lo)
        vm->d.dirty_lo = (int16_t)y0;
    if (y1 > vm->d.dirty_hi)
        vm->d.dirty_hi = (int16_t)y1;
}

/* Whether a Uint8Array is the screen */
#define IS_SCREEN(vm, c) ((uint32_t)(c).v + sizeof(struct u8a) == (vm)->screen)

/* n bytes of UTF-8 text cut short of a last character missing bytes, as a
 * copy cut to fit leaves one */
static inline uint32_t utf8_whole(const char *s, uint32_t n)
{
    uint32_t lead = n;
    while (lead > 0 && ((uint8_t)s[lead - 1] & 0xc0) == 0x80)
        lead--;
    if (lead > 0) {
        uint8_t c = (uint8_t)s[lead - 1];
        uint32_t need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        if (lead - 1 + need > n)
            return lead - 1;
    }
    return n;
}

/* The bytes of cp in UTF-8 */
static inline int utf8_bytes(uint32_t cp)
{
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

/* cp written at d in UTF-8: its bytes */
static inline int utf8_put(void *d, uint32_t cp)
{
    uint8_t *o = d;
    if (cp < 0x80) {
        o[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (uint8_t)(0xc0 | cp >> 6);
        o[1] = (uint8_t)(0x80 | (cp & 63));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (uint8_t)(0xe0 | cp >> 12);
        o[1] = (uint8_t)(0x80 | ((cp >> 6) & 63));
        o[2] = (uint8_t)(0x80 | (cp & 63));
        return 3;
    }
    o[0] = (uint8_t)(0xf0 | cp >> 18);
    o[1] = (uint8_t)(0x80 | ((cp >> 12) & 63));
    o[2] = (uint8_t)(0x80 | ((cp >> 6) & 63));
    o[3] = (uint8_t)(0x80 | (cp & 63));
    return 4;
}

/* The little-endian 32 bits at p */
static inline uint32_t rd_le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* v written little-endian at p */
static inline void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* vm.c */
int vm_fail(struct vm *vm, const char *a, const char *b, const char *c);
int vm_call(struct vm *vm, struct value fn, int argc, struct value *out);

/* v kept from the collector until vm_unroot(), for a native between
 * allocations; 0, the cart stopped, once every root is in use */
static inline int vm_root(struct vm *vm, struct value v)
{
    if (vm->ntmp >= sizeof(vm->tmp) / sizeof(vm->tmp[0]))
        return vm_fail(vm, "too many values held at once", "", "");
    vm->tmp[vm->ntmp++] = v;
    return 1;
}

static inline void vm_unroot(struct vm *vm)
{
    vm->ntmp--;
}

/* For a PARTS native (arcade_api.def): whether a[0], the count of the
 * pieces that follow it, is one the call holds */
static inline int vm_parts(struct vm *vm, const struct value *a, int argc,
                           const char *fn)
{
    if (a[0].tag != T_INT || a[0].v < 0 || a[0].v >= argc)
        return vm_fail(vm, fn, "() was called with its text damaged", "");
    return 1;
}
int vm_truthy(struct vm *vm, struct value v);
int vm_tostring(struct vm *vm, struct value v, struct value *out);
uint32_t vm_text(struct vm *vm, struct value v, char *dst, uint32_t cap);
/* An array a native writes its results to: a plain array, which keeps
 * fractions, or an Int32Array, which takes them rounded down. out_ok says
 * whether o is one at least need long; out_set writes element i. */
int vm_out_ok(struct vm *vm, struct value o, uint32_t need);
void vm_out_set(struct vm *vm, struct value o, uint32_t i, struct value v);
int vm_fix_op(struct vm *vm, int op, struct value a, struct value b,
              struct value *out);
void vm_charge(struct vm *vm, uint32_t units);
int32_t arcade_scroll(struct arcade_scroll *s, int32_t clicks, uint32_t rate);
/* vm_charge() for a native's long work, as it goes: 0, the cart stopped,
 * once the call is past the ceiling */
int vm_spend(struct vm *vm, uint32_t units);
/* core_draw.c, for the engine's own dialogs: n bytes of s drawn on the
 * current target in the current font and size, colour c, returning the x
 * after it; its width; one line's height */
int32_t draw_text(struct vm *vm, const char *s, uint32_t n, int32_t x,
                  int32_t y, uint32_t c);
int32_t text_width(struct vm *vm, const char *s, uint32_t n);
int32_t text_line(struct vm *vm);
int text_has(struct vm *vm, const char *s, uint32_t n);
const char *vm_typename(uint32_t tag);
int vm_new_string(struct vm *vm, const uint8_t *b, uint32_t len,
                  struct value *out);
int vm_new_array(struct vm *vm, uint32_t len, struct value *out);
int vm_new_record(struct vm *vm, const char *const *keys, uint32_t n,
                  struct value *out);
/* For values built from C (json.c). A string of len bytes for the caller
 * to fill; v added to the end of array a; record o's field named by the
 * string key set to v -- each with its arguments rooted, since each may
 * allocate. A record's fields in the order they were added: the i-th's
 * name, as a string, and value; 0 past the last. */
int vm_new_text(struct vm *vm, uint32_t len, struct value *out);
int vm_push(struct vm *vm, struct value a, struct value v);
int vm_set(struct vm *vm, struct value o, struct value key, struct value v);
int vm_field(struct vm *vm, struct value o, uint32_t i, struct value *name,
             struct value *v);

/* gc.c */
void gc_init(struct vm *vm);
uint32_t gc_alloc(struct vm *vm, uint32_t type, uint32_t bytes);
uint32_t gc_scratch(struct vm *vm, uint32_t bytes);
void gc_scratch_end(struct vm *vm, uint32_t bytes);
void gc_collect(struct vm *vm);

/* loader.c */
uint32_t perm_alloc(struct vm *vm, uint32_t bytes);
int lz4_decode(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t size);
int lazy_unpack(struct vm *vm, const struct lazy *c, uint8_t *dst,
                uint32_t size);
uint32_t key_id(struct vm *vm, struct value s, int add);
uint32_t key_find(struct vm *vm, const char *b, uint32_t len);
/* A record's field by its name, from C; null when o is not a record or has
 * no such field */
struct value vm_get(struct vm *vm, struct value o, const char *name);
uint32_t vm_shape_add(struct vm *vm, uint32_t parent, uint32_t key);

/* core_math.c */
void core_seed(struct vm *vm, uint64_t seed);
uint32_t core_random(struct vm *vm);
int32_t core_sine(uint32_t turns);

/* core_draw.c */
int core_draw_init(struct vm *vm);
/* A cart's array read as numbers whatever its kind -- values, an
 * Int32Array or a Uint8Array -- or one number for every element (tag
 * T_INT, in k), or absent (tag T_NULL) */
struct column
{
    uint32_t tag;
    const void *p;
    uint32_t len;
    int32_t k;
};
int column_of(struct vm *vm, struct value v, struct column *c);
int32_t column_at(const struct column *c, uint32_t i, int32_t none);
int32_t column_fix(const struct column *c, uint32_t i);
int column_true(struct vm *vm, const struct column *c, uint32_t i, int dflt);

/* For the systems: sprites from columns (on and flip may be absent), and
 * particles' pixels from a pool's arrays of values */
void core_draw_sprites(struct vm *vm, const struct column *on,
                       const struct column *sp, const struct column *x,
                       const struct column *y, const struct column *flip,
                       const struct column *frames,
                       const struct column *speed, uint32_t n);
void core_draw_particles(struct vm *vm, struct value *const *arr,
                         uint32_t n, int32_t w, int32_t h, int32_t shade);
void core_draw_quads(struct vm *vm, const int32_t *q, const uint8_t *c,
                     uint32_t n);
void core_draw_lines(struct vm *vm, const int32_t *seg, uint32_t n,
                     uint32_t c);
uint32_t core_draw_land(struct vm *vm, const struct landspec *l);
/* The floor's rows, or the ceiling's: each map cell's sprite, through
 * remap when it is not NULL */
void core_draw_floor(struct vm *vm, const struct floor_row *rows,
                     int32_t top, int32_t bottom, int32_t outside,
                     int coarse, const uint8_t *remap);
int32_t core_fog_level(const struct planeview *v, int64_t d);
/* How many sprites the sheet sheet() chose has */
int32_t core_sheet_sprites(struct vm *vm);
void core_draw_billboard(struct vm *vm, int32_t s, int32_t x, int32_t y,
                         int32_t w, int32_t h, int32_t cw, int32_t ch,
                         int flip, int32_t depth);
/* walls.draw()'s: the flags of wall cells, their height (16.16) and the
 * farthest drawn; a palette map for faces along y, or NULL; and doors as
 * column, row and openness (16.16) three numbers each */
struct wallspec
{
    int32_t mask, height, far;
    const uint8_t *dark;
    const int32_t *doors;
    int32_t ndoors;
};
uint32_t core_draw_walls(struct vm *vm, const struct wallspec *ws);
void core_draw_sky(struct vm *vm, const uint8_t *px, int32_t w, int32_t h,
                   int32_t lift);
int sound_load(struct vm *vm);

/* sys_grid.c, for the systems on boards: a Uint8Array w wide, and a
 * call's scratch from the heap, garbage after it */
struct board
{
    uint8_t *c;
    int32_t w, h;
};
int board_of(struct vm *vm, struct value b, struct value w, const char *fn,
             struct board *o);
void *scratch(struct vm *vm, uint32_t bytes);

/* sys_cells.c, for actors: a cells field and its rules' tables */
struct cellworld
{
    uint8_t *c;
    int32_t w, h;
    const uint8_t *density, *flags, *a, *b, *c2, *d, *chance;
};
int cells_world(struct vm *vm, struct value field, struct value rules,
                struct cellworld *w);
void cells_drawn(struct vm *vm, uint32_t id, int32_t w, int32_t h, int32_t x0,
                 int32_t y0, int32_t x1, int32_t y1);
enum { CELL_SOLID = 1, CELL_LIQUID = 2, CELL_HARM = 4 };

/* sys_actors.c, for cells: actors' boxes as obstacles, and a particle
 * pool's PARTICLE_ARRAYS arrays (life, x, y, dx, dy, c, sp, flip) for
 * cells.lift */
#define PARTICLE_ARRAYS 8
int32_t actors_boxes(struct vm *vm, struct value pools, uint8_t *bits,
                     int32_t w, int32_t h, int32_t *boxes, int32_t cap);
int particles_arrays(struct vm *vm, struct value p, struct value **arr,
                     uint32_t *n, int32_t *ox, int32_t *oy);

/* verify.c */
int verify_cart(struct vm *vm);

#endif
