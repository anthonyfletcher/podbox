/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade core: drawing -- to the 160x120 screen or an image, both 8-bit
 * palette indices -- and the sprite sheet, the map and text.
 *
 * PICO-8's draw state and meanings: the camera offsets drawing, the clip
 * is in target pixels, a colour given to a call becomes the pen, the draw
 * palette remaps what is drawn, the fill pattern's set bits draw the
 * colour's high byte (or nothing, if that is 0), and the screen palette
 * remaps what is shown. A fraction given as a coordinate is rounded down;
 * coordinates are clamped to +-2^20 so no product overflows.
 *
 * Parts, in order:
 *   setup          the surfaces and the default palette
 *   assets         the cart's palettes, sheet, maps and images
 *   arguments      coordinates, colours, flags
 *   pens           one call's clip, colours and pattern; spans; the charge
 *   shapes         circles and ovals as spans, filled or outlined
 *   lines          line, and tline, which steps the same way
 *   triangles, rectangles, text
 *   sprites        the sheet they come from; spr, sspr, sget/sset,
 *                  fget/fset, images
 *   turned         rspr and rblit, rotated and scaled about their centres
 *   the map
 *   state          camera, clip, colour, cursor, palettes, target
 *   memory         the engine's arrays as Uint8Arrays; copy and fill
 *   for the systems  actors' sprites and particles' pixels; the plane's
 *                    floor and billboards
 *   light          lights over a dark scene, a ramp of colour tables
 *   the host's view of the screen
 ***************************************************************************/
#include <stddef.h>
#include <string.h>
#include "games/arcade/vm.h"
#include "games/arcade/core_font.h"
#include "games/arcade/core_textfont.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)
#define FAR (1 << 20)
/* Near the surface: within this of its corner, the fast paths' arithmetic
 * fits in 32 bits */
#define NEAR32 8192

/* ---- setup ------------------------------------------------------------- */

/* PodBox's sixteen. Each index plays the part PICO-8's does -- 0 black,
 * 7 white, 8 red, 12 blue -- so ported code keeps its colour numbers. */
static const uint32_t default_rgb[16] = {
    0x000000, 0x1a2244, 0x6b2a5e, 0x0f6640, 0x8a5236, 0x4f4a52, 0xb8b4bc,
    0xf6f2ea, 0xe8304a, 0xf5962a, 0xf8e43c, 0x3cd250, 0x2f9cf0, 0x7e7aa8,
    0xf27ab4, 0xf9c89a
};

static void reset_palettes(struct draw *d)
{
    for (int i = 0; i < 256; i++) {
        d->dpal[i] = (uint8_t)i;
        d->spal[i] = (uint8_t)i;
    }
    memset(d->transparent, 0, sizeof(d->transparent));
    d->transparent[0] = 1;
    d->xlat_stale = 1;
}

#define XLAT_CLEAR 0x100

/* dpal and transparent as one table: a copy's pixel is one lookup */
static const uint16_t *xlat_of(struct draw *d)
{
    if (d->xlat_stale) {
        int plain = 1;
        for (int c = 0; c < 256; c++) {
            d->xlat[c] = d->transparent[c >> 3] & (1 << (c & 7))
                         ? XLAT_CLEAR : d->dpal[c];
            plain &= d->xlat[c] == c;
        }
        d->xlat_plain = (uint8_t)plain;
        d->xlat_stale = 0;
    }
    return d->xlat;
}

static void reshow(struct draw *d)
{
    for (int i = 0; i < 256; i++)
        d->shown[i] = d->rgb[d->spal[i]];
    d->palette_changed = 1;
}

static void full_clip(struct vm *vm);

/* An engine array, laid out as a Uint8Array whose header sits just before
 * the bytes, so mem() can hand it to the cart as one. Its offset is the
 * bytes'; outside the heap, the collector leaves it alone. */
static uint32_t region(struct vm *vm, uint32_t bytes)
{
    uint32_t off = perm_alloc(vm, sizeof(struct u8a) + bytes);
    if (!off)
        return 0;
    struct u8a *a = MEM(vm, off);
    a->h.info = T_U8;
    a->h.size = (sizeof(struct u8a) + bytes + 7) & ~7u;
    a->len = bytes;
    return off + sizeof(struct u8a);
}

/* ---- assets -------------------------------------------------------------- *
 * The chunks' layouts are arcade/sdk/src/assets.js's. Each is checked
 * whole when the cart loads, so the natives below can trust them. */

static uint32_t le16(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8;
}

static int damaged(struct vm *vm, const char *what)
{
    return vm_fail(vm, "this game file is damaged: ", what, "");
}

/* Palette n's colours and how many, or NULL, from a PALS chunk */
static const uint8_t *palette_in(const uint8_t *p, uint32_t len, uint32_t n,
                                 uint32_t *count)
{
    const uint8_t *end = p + len;
    *count = 0;
    if (!p || len < 1 || n >= p[0])
        return NULL;
    p++;
    for (uint32_t i = 0;; i++) {
        if (end - p < 2)
            return NULL;
        uint32_t k = le16(p);
        if (!k || k > 256 || (uint32_t)(end - p - 2) < k * 3)
            return NULL;
        if (i == n) {
            *count = k;
            return p + 2;
        }
        p += 2 + k * 3;
    }
}

static const uint8_t *palette_at(const struct vm *vm, uint32_t n,
                                 uint32_t *count)
{
    return palette_in(vm->pals, vm->pals_len, n, count);
}

/* A cart's logo, read without loading the cart: the LOGO chunk's picture
 * in the colours of its first palette (PALS, or PodBox's sixteen without
 * one), 0xRRGGBB a pixel into rgb. 1 when it fitted in cap pixels whole. */
int arcade_logo_rgb(const uint8_t *logo, uint32_t logo_len,
                    const uint8_t *pals, uint32_t pals_len,
                    uint32_t *rgb, uint32_t cap, int *w, int *h)
{
    static uint8_t px[ARCADE_LOGO_SIZE * ARCADE_LOGO_SIZE];
    uint32_t k = 0, pw, ph, n;
    const uint8_t *pal = palette_in(pals, pals_len, 0, &k);
    if (!logo || logo_len < 8)
        return 0;
    pw = le16(logo);
    ph = le16(logo + 2);
    n = le16(logo + 4) | le16(logo + 6) << 16;
    if (!pw || !ph || pw * ph > cap || pw * ph > sizeof(px) ||
        n > logo_len - 8 || !lz4_decode(logo + 8, n, px, pw * ph))
        return 0;
    for (uint32_t i = 0; i < pw * ph; i++) {
        uint32_t c = px[i];
        rgb[i] = pal ? (c < k ? (uint32_t)pal[c * 3] << 16 |
                                (uint32_t)pal[c * 3 + 1] << 8 | pal[c * 3 + 2]
                              : 0)
                     : c < 16 ? default_rgb[c] : 0;
    }
    *w = (int)pw;
    *h = (int)ph;
    return 1;
}

static void use_palette(struct vm *vm, const uint8_t *rgb, uint32_t k)
{
    for (uint32_t i = 0; i < k; i++, rgb += 3)
        vm->d.rgb[i] = (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | rgb[2];
    reshow(&vm->d);
}

/* The map directory; *cap is the most cells any map needs */
static int read_maps(struct vm *vm, uint32_t *cap)
{
    *cap = MAP_W * MAP_H;
    if (!vm->maps)
        return 1;
    const uint8_t *p = vm->maps, *end = p + vm->maps_len;
    if (end - p < 2)
        return damaged(vm, "maps");
    uint32_t n = le16(p);
    p += 2;
    uint32_t at = perm_alloc(vm, n * sizeof(struct mapinfo) + 8);
    if (!at)
        return 0;
    vm->mapdir = MEM(vm, at);
    for (uint32_t i = 0; i < n; i++) {
        struct mapinfo *m = &vm->mapdir[i];
        if (end - p < 8)
            return damaged(vm, "maps");
        m->w = (uint16_t)le16(p);
        m->h = (uint16_t)le16(p + 2);
        m->zlen = rd_le32(p + 4);
        p += 8;
        if (!m->w || !m->h || m->w > MAP_MAX || m->h > MAP_MAX)
            return damaged(vm, "maps");
        /* format 3 leaves the cells in the file, a chunk a map */
        if (vm->format >= 3) {
            if (i >= vm->nmapc || m->zlen != vm->mapc[i].len)
                return damaged(vm, "maps");
            m->z = NULL;
            m->lazy = (int32_t)i;
        } else {
            if (m->zlen > (uint32_t)(end - p))
                return damaged(vm, "maps");
            m->z = p;
            m->lazy = -1;
            p += m->zlen;
        }
        if (end - p < 2)
            return damaged(vm, "maps");
        m->nobj = (uint16_t)le16(p);
        p += 2;
        at = perm_alloc(vm, m->nobj * sizeof(uint8_t *) + 8);
        if (!at)
            return 0;
        m->obj = MEM(vm, at);
        for (uint32_t o = 0; o < m->nobj; o++) {
            m->obj[o] = p;
            if (end - p < 10)
                return damaged(vm, "map objects");
            uint32_t props = p[9];
            p += 10;
            for (uint32_t k = 0; k < props; k++) {
                if (end - p < 3 || le16(p) >= vm->nstrings || p[2] > 1)
                    return damaged(vm, "map objects");
                uint32_t type = p[2];
                p += 3;
                if (end - p < (type ? 2 : 4) ||
                    (type && le16(p) >= vm->nstrings))
                    return damaged(vm, "map objects");
                p += type ? 2 : 4;
            }
        }
        if ((uint32_t)m->w * m->h > *cap)
            *cap = (uint32_t)m->w * m->h;
    }
    vm->nmaps = n;
    return 1;
}

/* A map's cells unpacked into dst, read from the cart file when it left
 * them there */
static int map_cells(struct vm *vm, const struct mapinfo *m, uint8_t *dst,
                     uint32_t cells)
{
    if (m->lazy >= 0)
        return lazy_unpack(vm, &vm->mapc[m->lazy], dst, cells);
    if (!lz4_decode(m->z, m->zlen, dst, cells))
        return damaged(vm, "a map");
    return 1;
}

/* Map n becomes the active one, as stored */
static int use_map(struct vm *vm, uint32_t n)
{
    const struct mapinfo *m = &vm->mapdir[n];
    uint32_t cells = (uint32_t)m->w * m->h;
    if (!map_cells(vm, m, MEM(vm, vm->map), cells))
        return 0;
    vm->map_w = m->w;
    vm->map_h = m->h;
    vm->active_map = (int32_t)n;
    ((struct u8a *)MEM(vm, vm->map - sizeof(struct u8a)))->len = cells;
    return 1;
}

static int read_sheet(struct vm *vm)
{
    const uint8_t *p = vm->shet;
    if (!p)
        return 1;
    if (vm->shet_len < 8 || le16(p) != SHEET_W || le16(p + 2) != SHEET_H ||
        rd_le32(p + 4) > vm->shet_len - 8 ||
        vm->shet_len - 8 - rd_le32(p + 4) != 256 ||
        !lz4_decode(p + 8, rd_le32(p + 4), MEM(vm, vm->sheet), SHEET_W * SHEET_H))
        return damaged(vm, "the sheet");
    memcpy(MEM(vm, vm->flags), p + 8 + rd_le32(p + 4), 256);
    return 1;
}

/* Each image as an img outside the heap, and the globals IMG.* name. In
 * format 3 an image flagged lazy is left in the file, a chunk of its own,
 * and named by a T_LAZY of its struct lazyimg until img.load(). */
#define OFF_LAZY 0x80000000u        /* in offs: the image is a lazyimg */
static int read_images(struct vm *vm)
{
    const uint8_t *p = vm->imgs, *end = p + vm->imgs_len;
    uint32_t n = 0, *offs = NULL;
    if (p) {
        if (end - p < 2)
            return damaged(vm, "images");
        n = le16(p);
        p += 2;
        uint32_t at = perm_alloc(vm, n * 4 + 8);
        if (!at)
            return 0;
        offs = MEM(vm, at);
        if (vm->format >= 3 && vm->nimgc) {
            at = perm_alloc(vm, vm->nimgc * sizeof(struct lazyimg) + 8);
            if (!at)
                return 0;
            vm->lazyimgs = MEM(vm, at);
        }
        for (uint32_t i = 0; i < n; i++) {
            if (end - p < 8)
                return damaged(vm, "images");
            uint32_t w = le16(p), h = le16(p + 2), zlen = rd_le32(p + 4);
            p += 8;
            uint32_t flags = 0;
            if (vm->format >= 3) {
                if (end - p < 1)
                    return damaged(vm, "images");
                flags = *p++;
            }
            if (!w || !h || w > IMAGE_MAX || h > IMAGE_MAX)
                return damaged(vm, "images");
            if (flags & 1) {
                uint32_t k = vm->nlazyimgs;
                if (k >= vm->nimgc || zlen != vm->imgc[k].len ||
                    !lazy_unpack(vm, &vm->imgc[k], NULL, 0))
                    return damaged(vm, "images");
                struct lazyimg *r = &vm->lazyimgs[k];
                r->w = (uint16_t)w;
                r->h = (uint16_t)h;
                r->chunk = (uint16_t)k;
                r->slot = 0xffff;
                r->copy = 0;
                vm->nlazyimgs++;
                offs[i] = OFF_LAZY | (uint32_t)((uint8_t *)r - vm->mem);
                continue;
            }
            if (zlen > (uint32_t)(end - p))
                return damaged(vm, "images");
            uint32_t bytes = (uint32_t)sizeof(struct img) + w * h;
            uint32_t off = perm_alloc(vm, bytes);
            if (!off)
                return 0;
            struct img *im = MEM(vm, off);
            im->h.info = T_IMG;
            im->h.size = (bytes + 7) & ~7u;
            im->width = (uint16_t)w;
            im->height = (uint16_t)h;
            if (!lz4_decode(p, zlen, im->px, w * h))
                return damaged(vm, "an image");
            p += zlen;
            offs[i] = off;
        }
    }
    for (uint32_t i = 0; i < vm->nimg_globals; i++) {
        uint32_t slot = vm->img_globals[i * 2], img = vm->img_globals[i * 2 + 1];
        if (slot >= vm->nglobals || img >= n)
            return damaged(vm, "images");
        if (offs[img] & OFF_LAZY) {
            uint32_t off = offs[img] & ~OFF_LAZY;
            ((struct lazyimg *)MEM(vm, off))->slot = (uint16_t)slot;
            vm->globals[slot] = mk(T_LAZY, (int32_t)off);
        } else
            vm->globals[slot] = mk(T_IMG, (int32_t)offs[img]);
    }
    return 1;
}

/* 0 for too little memory, or with the error set */
int core_draw_init(struct vm *vm)
{
    struct draw *d = &vm->d;
    uint32_t cap;
    if (!(vm->screen = region(vm, SCREEN_W * SCREEN_H)) ||
        !(vm->sheet = region(vm, SHEET_W * SHEET_H)) ||
        !(vm->flags = region(vm, 256)) ||
        !read_maps(vm, &cap) ||
        !(vm->map = region(vm, cap)))
        return 0;
    vm->map_w = MAP_W;
    vm->map_h = MAP_H;
    vm->active_map = -1;
    ((struct u8a *)MEM(vm, vm->map - sizeof(struct u8a)))->len = MAP_W * MAP_H;
    memset(d, 0, sizeof(*d));
    d->target = mk(T_INT, 0);
    d->sheet = mk(T_INT, 0);
    d->pen = 6;
    memcpy(d->rgb, default_rgb, sizeof(default_rgb));
    reset_palettes(d);
    reshow(d);
    if (vm->pals) {
        uint32_t k;
        for (uint32_t i = 0; i < vm->pals[0]; i++)
            if (!palette_at(vm, i, &k))
                return damaged(vm, "palettes");
        const uint8_t *first = palette_at(vm, 0, &k);
        use_palette(vm, first, k);
    }
    /* Every map is checked by unpacking it; the first is then active */
    for (uint32_t i = vm->nmaps; i-- > 0;)
        if (!use_map(vm, i))
            return 0;
    if (!read_sheet(vm) || !read_images(vm))
        return 0;
    full_clip(vm);
    d->dirty_lo = 0;
    d->dirty_hi = SCREEN_H - 1;
    return 1;
}

/* ---- arguments ----------------------------------------------------------- */

static int64_t clampc(int64_t v)
{
    return v < -FAR ? -FAR : v > FAR ? FAR : v;
}

static int bad(struct vm *vm, const char *fn, const char *what,
               struct value v)
{
    return vm_fail(vm, fn, what, vm_typename(v.tag));
}

/* The argument helpers are inlined: called, each costs some 50 cycles on
 * the 5G, most of it passing seven arguments, three on the stack. */
#define ARG_INLINE static inline __attribute__((always_inline))

/* Argument i as a whole number, def when missing or null. */
ARG_INLINE int whole(struct vm *vm, struct value *a, int argc, int i,
                     int32_t def, const char *fn, int32_t *out)
{
    *out = def;
    if (i >= argc || a[i].tag == T_NULL)
        return 1;
    if (!is_num(a[i]))
        return bad(vm, fn, "() needs a number, not a ", a[i]);
    *out = floor_int(a[i]);
    return 1;
}

/* A drawing call's colour: given, it becomes the pen, as in PICO-8. */
ARG_INLINE int colour(struct vm *vm, struct value *a, int argc, int i,
                      const char *fn, uint32_t *c)
{
    *c = 0;
    if (i < argc && a[i].tag != T_NULL) {
        if (!is_num(a[i]))
            return bad(vm, fn, "() needs a colour number, not a ", a[i]);
        vm->d.pen = (uint16_t)floor_int(a[i]);
    }
    *c = vm->d.pen;
    return 1;
}

/* A flip, as every call takes one: 1 across, 2 up and down, 3 both; true
 * is 1. */
#define FLIP_X(f) ((f) & 1)
#define FLIP_Y(f) (((f) >> 1) & 1)

#define WHOLE(i, def, x, fn) int32_t x; \
    if (!whole(vm, a, argc, i, def, fn, &x)) return 0
#define COLOUR(i, c, fn) uint32_t c; \
    if (!colour(vm, a, argc, i, fn, &c)) return 0

/* ---- pens --------------------------------------------------------------- */

/* What a drawing call is charged, in units, from its cycles on the 5G: a
 * call's setup, then each row a shape works out, glyph or map tile, and
 * each pixel by its kind as units per 16 pixels -- the pixels drawn, or
 * for copies and tline the pixels read, which cost the same whether or
 * not they are transparent. Rectangles, copies, sprites, map tiles and
 * text are timed on the player (the Timing cart, 2026-11-09). */
#define COST_CALL      20
#define COST_COPY      60   /* copy_rect's setup, on top of the call's */
#define COST_SMALL     6    /* small_sprite's, in its place: measured */
#define COST_SPRITE    18   /* spr's of an 8x8 tile, on top of tile8's */
#define COST_RECT      10   /* rect's and rectfill's, on top of the call's */
#define COST_ROW       2
#define CLS_PIXELS     110  /* cls's pixels a unit */
#define COST_GLYPH     11
#define COST_BIG_GLYPH 56
#define COST_TEXT_GLYPH 26  /* FONT.WORLD's and a cart font's */
#define COST_TILE      5
#define FILL256        4    /* a memset's pixels, per 256: about 1 cycle each */
#define PER16_PATTERN  10
#define PER16_TILE     6
#define PER16_COPY     5
#define PLAIN256       13   /* a plain copy's pixels, per 256 */
#define PER16_SMALL    9    /* small_sprite's pixels */
#define PER16_SCALED   16
#define PER16_DOT      18
#define PER16_TLINE    48
#define PER16_FLOOR    12   /* a row of the floor on the map */
#define PER16_EDGE     18   /* one that leaves it */
#define PER16_TEXT     12   /* FONT.WORLD's and a cart font's dots */

struct surf
{
    uint8_t *px;
    int32_t w, h;
    int is_screen;
};

static int surf_of(struct vm *vm, struct value t, struct surf *s,
                   const char *fn)
{
    s->px = NULL;
    s->w = s->h = 0;
    s->is_screen = 0;
    if (is_num(t)) {
        if (floor_int(t) != 0)
            return vm_fail(vm, fn, "() needs SCREEN or an image", "");
        s->px = MEM(vm, vm->screen);
        s->w = SCREEN_W;
        s->h = SCREEN_H;
        s->is_screen = 1;
        return 1;
    }
    if (t.tag != T_IMG)
        return bad(vm, fn, "() needs SCREEN or an image, not a ", t);
    struct img *im = MEM(vm, (uint32_t)t.v);
    s->px = im->px;
    s->w = im->width;
    s->h = im->height;
    s->is_screen = 0;
    return 1;
}

/* One drawing call: where it draws and with what. */
struct pen
{
    struct surf s;
    int32_t cx0, cy0, cx1, cy1;     /* the clip, within the surface */
    int64_t ox, oy;                 /* minus the camera */
    uint8_t c, alt;
    int alt_on;
    uint16_t pat;
    uint32_t count;                 /* pixels drawn */
    uint32_t cost;                  /* units, besides the pixels */
    uint16_t per256;                /* units per 256 pixels */
    int32_t lo, hi;
    int32_t xlo, xhi;               /* the columns drawn, as area_touched */
    int wide;                       /* columns unknown: a row_touched */
    const struct draw *d;
    const uint16_t *xl;
};

static void begin(struct vm *vm, struct pen *p, uint32_t c, int patterned)
{
    struct draw *d = &vm->d;
    surf_of(vm, d->target, &p->s, "draw");
    p->cx0 = d->clip_x0 < 0 ? 0 : d->clip_x0;
    p->cy0 = d->clip_y0 < 0 ? 0 : d->clip_y0;
    p->cx1 = d->clip_x1 > p->s.w ? p->s.w : d->clip_x1;
    p->cy1 = d->clip_y1 > p->s.h ? p->s.h : d->clip_y1;
    /* A clip off the surface is empty, never inverted: the runs test
     * x - cx0 < cx1 - cx0 unsigned. */
    if (p->cx1 < p->cx0)
        p->cx1 = p->cx0;
    if (p->cy1 < p->cy0)
        p->cy1 = p->cy0;
    p->ox = -(int64_t)d->cam_x;
    p->oy = -(int64_t)d->cam_y;
    p->c = d->dpal[c & 0xff];
    p->pat = patterned ? d->fill : 0;
    p->alt_on = p->pat && ((c >> 8) & 0xff);
    p->alt = d->dpal[(c >> 8) & 0xff];
    p->count = 0;
    p->cost = COST_CALL;
    p->per256 = p->pat ? PER16_PATTERN * 16 : FILL256;
    p->lo = 0x7fffffff;
    p->hi = -1;
    p->xlo = 0x7fffffff;
    p->xhi = -1;
    p->wide = 0;
    p->d = d;
    p->xl = xlat_of(d);
}

/* ---- the camera's turn and zoom ------------------------------------------ *
 * A turned or zoomed camera moves each point drawn about its pivot before
 * the drawing takes the camera's offset off as ever: these give a point
 * back moved but still before that offset. Shapes, lines, points and
 * outlines turn and grow whole, lines staying a pixel wide; sprites,
 * images, text and particles go where their middle moves to, upright and
 * their own size, but for rspr and rblit, which turn and grow too. */

/* a times b, 64 bits from 32: one smull on the player, where -Os would
 * otherwise share turn_fix's 64-bit multiplies with its far path, about
 * 220 cycles a point on the rig against 45 */
static inline __attribute__((always_inline)) int64_t mul32(int32_t a,
                                                           int32_t b)
{
#ifdef ARCADE_ASM
    uint32_t lo, hi;
    __asm__("smull %0, %1, %2, %3" : "=&r"(lo), "=&r"(hi) : "r"(a), "r"(b));
    return (int64_t)((uint64_t)hi << 32 | lo);
#else
    return (int64_t)a * b;
#endif
}

/* x, y, 16.16, turned */
static void turn_fix(const struct draw *d, int64_t *x, int64_t *y)
{
    int64_t ox = ((int64_t)d->cam_x << 16) + d->cam_px;
    int64_t oy = ((int64_t)d->cam_y << 16) + d->cam_py;
    int64_t sx = *x - ox, sy = *y - oy;
    if (sx == (int32_t)sx && sy == (int32_t)sy) {
        /* the same products as below, from 32-bit multiplies */
        int32_t x32 = (int32_t)sx, y32 = (int32_t)sy;
        *x = ox + ((mul32(x32, d->cam_cos) - mul32(y32, d->cam_sin)) >> 16);
        *y = oy + ((mul32(x32, d->cam_sin) + mul32(y32, d->cam_cos)) >> 16);
        return;
    }
    /* far beyond any target, kept where the multiply cannot overflow at
     * the greatest zoom */
    const int64_t far = (int64_t)1 << 40;
    if (sx > far) sx = far;
    if (sx < -far) sx = -far;
    if (sy > far) sy = far;
    if (sy < -far) sy = -far;
    *x = ox + ((sx * d->cam_cos - sy * d->cam_sin) >> 16);
    *y = oy + ((sx * d->cam_sin + sy * d->cam_cos) >> 16);
}

/* The pixel x, y turned: its middle, and the pixel that lands in */
static void turn_px(const struct draw *d, int32_t *x, int32_t *y)
{
    if (!d->cam_moved)
        return;
    int64_t fx = ((int64_t)*x << 16) + 32768, fy = ((int64_t)*y << 16) + 32768;
    turn_fix(d, &fx, &fy);
    *x = (int32_t)clampc(fx >> 16);
    *y = (int32_t)clampc(fy >> 16);
}

/* A w by h picture whose corner is x, y: its corner once its middle has
 * turned */
static void turn_box(const struct draw *d, int32_t *x, int32_t *y, int32_t w,
                     int32_t h)
{
    if (!d->cam_moved)
        return;
    int64_t fx = ((int64_t)*x << 16) + ((int64_t)w << 15);
    int64_t fy = ((int64_t)*y << 16) + ((int64_t)h << 15);
    turn_fix(d, &fx, &fy);
    *x = (int32_t)clampc((fx - ((int64_t)w << 15) + 32768) >> 16);
    *y = (int32_t)clampc((fy - ((int64_t)h << 15) + 32768) >> 16);
}

/* rspr's and rblit's middle, 16.16, moved, and their angle and scale with
 * it: their angle turns the other way from the camera's */
static void turn_turned(const struct draw *d, int64_t *x, int64_t *y,
                        int64_t *angle, int32_t *scale)
{
    if (!d->cam_moved)
        return;
    turn_fix(d, x, y);
    *angle -= d->cam_turn;
    int64_t k = ((int64_t)*scale * d->cam_zoom) >> 16;
    *scale = (int32_t)(k < 0x400 ? 0x400 : k > (64 << 16) ? 64 << 16 : k);
}

/* Row y drawn on; its columns unknown, so a field's box takes the row
 * whole */
static void row_touched(struct pen *p, int32_t y)
{
    if (y < p->lo)
        p->lo = y;
    if (y > p->hi)
        p->hi = y;
    p->wide = 1;
}

/* Columns x0..x1 of row y drawn on; inlined, as every span calls it */
static inline __attribute__((always_inline)) void
area_touched(struct pen *p, int32_t y, int32_t x0, int32_t x1)
{
    if (y < p->lo)
        p->lo = y;
    if (y > p->hi)
        p->hi = y;
    if (x0 < p->xlo)
        p->xlo = x0;
    if (x1 > p->xhi)
        p->xhi = x1;
}

static void finish(struct vm *vm, struct pen *p)
{
    if (p->s.is_screen && p->hi >= 0) {
        if (p->lo < vm->d.dirty_lo)
            vm->d.dirty_lo = (int16_t)p->lo;
        if (p->hi > vm->d.dirty_hi)
            vm->d.dirty_hi = (int16_t)p->hi;
    } else if (p->hi >= 0 && vm->d.target.tag == T_IMG)
        /* an image a cells field steps: what was drawn, for cells.step's
         * box -- whole rows where the columns are not known */
        cells_drawn(vm, (uint32_t)vm->d.target.v, p->s.w, p->s.h,
                    p->wide ? 0 : p->xlo, p->lo,
                    p->wide ? p->s.w - 1 : p->xhi, p->hi);
    vm_charge(vm, p->cost + p->count * p->per256 / 256);
}

/* Pixels x0..x1 of row y, in surface coordinates, clipped. */
static void span(struct pen *p, int64_t y, int64_t x0, int64_t x1)
{
    if (y < p->cy0 || y >= p->cy1)
        return;
    if (x0 < p->cx0)
        x0 = p->cx0;
    if (x1 >= p->cx1)
        x1 = p->cx1 - 1;
    if (x0 > x1)
        return;
    uint8_t *row = p->s.px + (int32_t)y * p->s.w;
    p->cost += COST_ROW;
    if (!p->pat)
        memset(row + x0, p->c, (size_t)(x1 - x0 + 1));
    else {
        int shift = 12 - ((int)(y & 3) << 2);
        for (int32_t x = (int32_t)x0; x <= (int32_t)x1; x++) {
            if (!((p->pat >> (shift + 3 - (x & 3))) & 1))
                row[x] = p->c;
            else if (p->alt_on)
                row[x] = p->alt;
        }
    }
    p->count += (uint32_t)(x1 - x0 + 1);
    area_touched(p, (int32_t)y, (int32_t)x0, (int32_t)x1);
}

static void dot(struct pen *p, int64_t x, int64_t y)
{
    span(p, y, x, x);
}

/* ---- shapes ------------------------------------------------------------- *
 * A shape is a span per row. Filled, each span is drawn; outlined, a row
 * draws the pixels its neighbours above and below do not cover, and
 * always its two ends, which makes a connected outline. */

struct figure
{
    int (*row)(const struct figure *s, int64_t y, int64_t *l, int64_t *r);
    int64_t y0, y1;
    int64_t a, b, c, d;
};

static uint64_t isqrt64(uint64_t v)
{
    if (v <= 0xffffffffu) {
        uint32_t v32 = (uint32_t)v, r32 = 0, b32 = 1u << 30;
        while (b32 > v32)
            b32 >>= 2;
        while (b32) {
            if (v32 >= r32 + b32) {
                v32 -= r32 + b32;
                r32 = (r32 >> 1) + b32;
            } else
                r32 >>= 1;
            b32 >>= 2;
        }
        return r32;
    }
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

/* In 32 bits when both fit: the 5G divides 64-bit numbers in software, at
 * about a thousand cycles each. */
static int64_t floor_div(int64_t n, int64_t d)
{
    if (n == (int32_t)n && d > 0 && d <= INT32_MAX) {
        int32_t a = (int32_t)n, b = (int32_t)d, q32 = a / b;
        return a % b != 0 && a < 0 ? q32 - 1 : q32;
    }
    int64_t q = n / d;
    return (n % d != 0 && ((n < 0) != (d < 0))) ? q - 1 : q;
}

/* A circle: centre (a, b), radius c. The half-width is
 * sqrt(r^2 - dy^2 + r/2), which keeps small circles round. */
static int circle_row(const struct figure *s, int64_t y, int64_t *l, int64_t *r)
{
    int64_t dy = y - s->b;
    if (dy < -s->c || dy > s->c)
        return 0;
    int64_t w = (int64_t)isqrt64((uint64_t)(s->c * s->c - dy * dy + s->c / 2));
    *l = s->a - w;
    *r = s->a + w;
    return 1;
}

/* An oval: twice its centre (a, b) and twice its radii (c, d), so that
 * boxes of even size work in whole numbers. */
static int oval_row(const struct figure *s, int64_t y, int64_t *l, int64_t *r)
{
    int64_t dy = 2 * y - s->b;
    if (dy < -s->d || dy > s->d)
        return 0;
    int64_t hw = s->d == 0 ? s->c
        : (int64_t)isqrt64((uint64_t)((s->c * s->c * (s->d * s->d - dy * dy)) /
                                      (s->d * s->d)));
    *l = -floor_div(-(s->a - hw), 2);
    *r = floor_div(s->a + hw, 2);
    if (*l > *r) {
        *l = floor_div(s->a, 2);
        *r = -floor_div(-s->a, 2);
    }
    return 1;
}

static int shape_row(const struct figure *s, int64_t y, int64_t *l, int64_t *r)
{
    if (y < s->y0 || y > s->y1)
        return 0;
    return s->row(s, y, l, r);
}

static void draw_shape(struct pen *p, const struct figure *s, int outline)
{
    int64_t y0 = s->y0 + p->oy < p->cy0 ? p->cy0 - p->oy : s->y0;
    int64_t y1 = s->y1 + p->oy >= p->cy1 ? p->cy1 - 1 - p->oy : s->y1;
    /* Each row is worked out once: an outline's row below becomes the
     * next row, and that row the one above. */
    int64_t l = 0, r = 0, ul = 0, ur = 0, dl = 0, dr = 0;
    int cur = 0, up = 0, dn = 0;
    if (y0 <= y1) {
        cur = shape_row(s, y0, &l, &r);
        if (outline)
            up = shape_row(s, y0 - 1, &ul, &ur);
    }
    for (int64_t y = y0; y <= y1;
         y++, up = cur, ul = l, ur = r, cur = dn, l = dl, r = dr) {
        if (outline || y < y1)
            dn = shape_row(s, y + 1, &dl, &dr);
        if (!cur)
            continue;
        int64_t sy = y + p->oy;
        if (!outline || !up || !dn) {
            span(p, sy, l + p->ox, r + p->ox);
            continue;
        }
        int64_t in_l = ul > dl ? ul : dl, in_r = ur < dr ? ur : dr;
        int64_t left_end = in_l - 1 < l ? l : in_l - 1;
        int64_t right_start = in_r + 1 > r ? r : in_r + 1;
        if (left_end > r)
            left_end = r;
        if (right_start < l)
            right_start = l;
        if (left_end + 1 >= right_start)
            span(p, sy, l + p->ox, r + p->ox);
        else {
            span(p, sy, l + p->ox, left_end + p->ox);
            span(p, sy, right_start + p->ox, r + p->ox);
        }
    }
}

/* span() for a row inside the clip, in 32 bits, unpatterned */
static void span32(struct pen *p, int32_t y, int32_t l, int32_t r)
{
    if (l < p->cx0)
        l = p->cx0;
    if (r >= p->cx1)
        r = p->cx1 - 1;
    if (l > r)
        return;
    memset(p->s.px + y * p->s.w + l, p->c, (size_t)(r - l + 1));
    p->count += (uint32_t)(r - l + 1);
    p->cost += COST_ROW;
    area_touched(p, y, l, r);
}

/* The integer square root of v, found from w, the last row's */
static int32_t sqrt_from(int32_t v, int32_t w)
{
    while ((w + 1) * (w + 1) <= v)
        w++;
    while (w * w > v)
        w--;
    return w;
}

/* draw_shape() of circle_row for an unpatterned circle near the surface,
 * in 32 bits, each half-width stepped from the row before's rather than
 * taken by a square root. 0 when it does not apply. */
static int circle_near(struct pen *p, int64_t a, int64_t b, int64_t r,
                       int outline)
{
    if (p->pat || r > 4096 || a + p->ox < -NEAR32 || a + p->ox > NEAR32 ||
        b + p->oy < -NEAR32 || b + p->oy > NEAR32)
        return 0;
    int32_t ca = (int32_t)(a + p->ox), cb = (int32_t)(b + p->oy);
    int32_t R = (int32_t)r, top = cb - R, bot = cb + R;
    int32_t y0 = top < p->cy0 ? p->cy0 : top;
    int32_t y1 = bot >= p->cy1 ? p->cy1 - 1 : bot;
    if (y0 > y1)
        return 1;
    int32_t rr = R * R + R / 2, d = y0 - cb;
    int32_t wc = (int32_t)isqrt64((uint64_t)(rr - d * d)), wu = 0, wd = wc;
    if (!outline) {
        for (int32_t y = y0; y <= y1; y++) {
            d = y - cb;
            wc = sqrt_from(rr - d * d, wc);
            span32(p, y, ca - wc, ca + wc);
        }
        /* each row's width found, beyond span32's: measured on the rig */
        p->cost += (uint32_t)(y1 - y0 + 1) * COST_ROW;
        return 1;
    }
    int up = y0 - 1 >= top;
    if (up) {
        d = y0 - 1 - cb;
        wu = sqrt_from(rr - d * d, wc);
    }
    for (int32_t y = y0; y <= y1; y++, up = 1, wu = wc, wc = wd) {
        int dn = y + 1 <= bot;
        if (dn) {
            d = y + 1 - cb;
            wd = sqrt_from(rr - d * d, wc);
        }
        int32_t l = ca - wc, rt = ca + wc;
        p->cost += 2 * COST_ROW;
        if (!up || !dn) {
            span32(p, y, l, rt);
            continue;
        }
        int32_t in_l = ca - (wu < wd ? wu : wd), in_r = ca + (wu < wd ? wu : wd);
        int32_t left_end = in_l - 1 < l ? l : in_l - 1;
        int32_t right_start = in_r + 1 > rt ? rt : in_r + 1;
        if (left_end > rt)
            left_end = rt;
        if (right_start < l)
            right_start = l;
        if (left_end + 1 >= right_start)
            span32(p, y, l, rt);
        else {
            span32(p, y, l, left_end);
            span32(p, y, right_start, rt);
        }
    }
    return 1;
}

static int circle(struct vm *vm, struct value *a, int argc, int outline,
                  const char *fn)
{
    WHOLE(0, 0, x, fn);
    WHOLE(1, 0, y, fn);
    WHOLE(2, 4, r, fn);
    COLOUR(3, c, fn);
    if (r < 0)
        return 1;
    turn_px(&vm->d, &x, &y);
    struct pen p;
    struct figure s = { circle_row, 0, 0, clampc(x), clampc(y),
                       r > FAR ? FAR : r, 0 };
    s.y0 = s.b - s.c;
    s.y1 = s.b + s.c;
    begin(vm, &p, c, 1);
    if (!circle_near(&p, s.a, s.b, s.c, outline))
        draw_shape(&p, &s, outline);
    finish(vm, &p);
    return 1;
}

NATIVE(circ) { (void)out; return circle(vm, a, argc, 1, "circ"); }
NATIVE(circfill) { (void)out; return circle(vm, a, argc, 0, "circfill"); }

/* The box's corners in order, clamped so the oval's arithmetic fits. */
static int box(struct vm *vm, struct value *a, int argc, const char *fn,
               int64_t b[4], int lim)
{
    for (int i = 0; i < 4; i++) {
        int32_t v;
        if (!whole(vm, a, argc, i, 0, fn, &v))
            return 0;
        b[i] = v < -lim ? -lim : v > lim ? lim : v;
    }
    int64_t t;
    if (b[0] > b[2]) { t = b[0]; b[0] = b[2]; b[2] = t; }
    if (b[1] > b[3]) { t = b[1]; b[1] = b[3]; b[3] = t; }
    return 1;
}

static int oval(struct vm *vm, struct value *a, int argc, int outline,
                const char *fn)
{
    int64_t b[4];
    if (!box(vm, a, argc, fn, b, 16384))
        return 0;
    COLOUR(4, c, fn);
    if (vm->d.cam_moved) {
        /* its middle moved, its axes upright and zoomed */
        int64_t mx = (b[0] + b[2] + 1) << 15, my = (b[1] + b[3] + 1) << 15;
        turn_fix(&vm->d, &mx, &my);
        int64_t hw = ((b[2] - b[0] + 1) * vm->d.cam_zoom) >> 1;
        int64_t hh = ((b[3] - b[1] + 1) * vm->d.cam_zoom) >> 1;
        b[0] = clampc((mx - hw + 32768) >> 16);
        b[1] = clampc((my - hh + 32768) >> 16);
        b[2] = clampc((mx + hw + 32768) >> 16) - 1;
        b[3] = clampc((my + hh + 32768) >> 16) - 1;
        if (b[2] < b[0]) b[2] = b[0];
        if (b[3] < b[1]) b[3] = b[1];
    }
    struct pen p;
    struct figure s = { oval_row, b[1], b[3], b[0] + b[2], b[1] + b[3],
                       b[2] - b[0], b[3] - b[1] };
    begin(vm, &p, c, 1);
    draw_shape(&p, &s, outline);
    finish(vm, &p);
    return 1;
}

NATIVE(oval) { (void)out; return oval(vm, a, argc, 1, "oval"); }
NATIVE(ovalfill) { (void)out; return oval(vm, a, argc, 0, "ovalfill"); }

/* ---- lines ------------------------------------------------------------- *
 * Each pixel is placed from the line's own start -- the minor axis is
 * rounded from i * d / n -- so clipping only chooses which i are drawn,
 * and a clipped line is exactly the pixels of the unclipped one. */

typedef void (*plot_fn)(struct pen *p, void *ctx, int64_t x, int64_t y,
                        int64_t i);

/* A line's visible stretch in 32 bits: k pixels from (x, y), pixel i of
 * the line first. Each pixel moves one step along the major axis, and the
 * stepper's remainder r carries the minor step when it leaves [0, den). */
struct run
{
    int32_t x, y, k, i;
    int32_t dmx, dmy, dnx, dny;
    int32_t r, den, inc;
};

typedef void (*run_fn)(struct pen *p, void *ctx, const struct run *run);

/* The step of a run, with its fields in locals (RUN_LOCALS): read through
 * the pointer, they are reloaded after every pixel store. */
#define RUN_LOCALS(run) \
    const int32_t dmx = (run)->dmx, dmy = (run)->dmy, dnx = (run)->dnx, \
                  dny = (run)->dny, inc = (run)->inc, den = (run)->den

#define RUN_STEP(x, y, r) do { \
    x += dmx; \
    y += dmy; \
    r += inc; \
    if (r >= den) { \
        r -= den; \
        x += dnx; \
        y += dny; \
    } else if (r < 0) { \
        r += den; \
        x -= dnx; \
        y -= dny; \
    } \
} while (0)

/* One pixel of a line, written directly: span() is for runs. */
static void plot_dot(struct pen *p, void *ctx, int64_t x, int64_t y, int64_t i)
{
    (void)ctx; (void)i;
    if (x < p->cx0 || x >= p->cx1 || y < p->cy0 || y >= p->cy1)
        return;
    int32_t xi = (int32_t)x, yi = (int32_t)y;
    uint8_t *px = p->s.px + yi * p->s.w + xi;
    if (p->pat && ((p->pat >> (15 - ((yi & 3) << 2) - (xi & 3))) & 1)) {
        if (!p->alt_on)
            return;
        *px = p->alt;
    } else
        *px = p->c;
    p->count++;
    area_touched(p, yi, xi, xi);
}

/* plot_dot along a run, unpatterned. visible() put every pixel's major
 * coordinate inside the clip, so only the minor one is tested, and the
 * pixel is reached by stepping its offset in the surface. */
static void run_dots(struct pen *p, void *ctx, const struct run *run)
{
    (void)ctx;
    uint8_t *px = p->s.px, c = p->c;
    const int32_t w = p->s.w, inc = run->inc, den = run->den;
    int32_t x = run->x, y = run->y, r = run->r, o = y * w + x;
    int32_t lo = p->lo, hi = p->hi;
    uint32_t n = 0;
    if (run->dny) {
        /* x major: y moves on a carry */
        const int32_t sx = run->dmx, c0 = p->cy0;
        const uint32_t ch = (uint32_t)(p->cy1 - p->cy0);
        for (int32_t k = run->k; k > 0; k--, o += sx) {
            if ((uint32_t)(y - c0) < ch) {
                px[o] = c;
                n++;
                if (y < lo) lo = y;
                if (y > hi) hi = y;
            }
            r += inc;
            if (r >= den) {
                r -= den;
                y++;
                o += w;
            } else if (r < 0) {
                r += den;
                y--;
                o -= w;
            }
        }
    } else {
        /* y major: x moves on a carry */
        const int32_t sy = run->dmy, sw = sy * w, c0 = p->cx0;
        const uint32_t cw = (uint32_t)(p->cx1 - p->cx0);
        for (int32_t k = run->k; k > 0; k--, y += sy, o += sw) {
            if ((uint32_t)(x - c0) < cw) {
                px[o] = c;
                n++;
                if (y < lo) lo = y;
                if (y > hi) hi = y;
            }
            r += inc;
            if (r >= den) {
                r -= den;
                x++;
                o++;
            } else if (r < 0) {
                r += den;
                x--;
                o--;
            }
        }
    }
    p->count += n;
    p->lo = lo;
    p->hi = hi;
}

/* The first and last i whose major coordinate s0 + i * step lands in
 * [lo, hi), within 0..n. */
static int visible(int64_t s0, int step, int64_t n, int64_t lo, int64_t hi,
                   int64_t *i0, int64_t *i1)
{
    int64_t a = step > 0 ? lo - s0 : s0 - (hi - 1);
    int64_t b = step > 0 ? hi - 1 - s0 : s0 - lo;
    *i0 = a < 0 ? 0 : a;
    *i1 = b > n ? n : b;
    return *i0 <= *i1;
}

/* floor((2 * i * d + n) / (2 * n)) for i and each i after it: the first by
 * one division, the rest by stepping, which gives the same values. A
 * division per pixel costs hundreds of cycles on the player. |d| <= n. */
struct stepper
{
    int64_t q, r, den, inc;
};

static void step_init(struct stepper *st, int64_t i, int64_t d, int64_t n)
{
    int64_t num = 2 * i * d + n;
    st->den = 2 * n;
    st->inc = 2 * d;
    st->q = floor_div(num, st->den);
    st->r = num - st->q * st->den;
}

static void step_next(struct stepper *st)
{
    st->r += st->inc;
    if (st->r >= st->den) {
        st->r -= st->den;
        st->q++;
    } else if (st->r < 0) {
        st->r += st->den;
        st->q--;
    }
}

/* The stretch from i0 to i1 as a run, if it fits in 32 bits: 1 if it does,
 * 0 if no pixel of it can be in the clip, -1 to step it in 64 bits. */
static int make_run(struct run *run, int64_t major, int64_t minor,
                    int smaj, int x_major, int64_t i0, int64_t i1,
                    const struct stepper *st)
{
    if (st->den >= (1 << 30) || i1 - i0 >= (1 << 30))
        return -1;
    /* A minor coordinate this far out stays off any surface for the
     * whole stretch, which is no longer than one. */
    if (minor < -(1 << 30) || minor > (1 << 30))
        return 0;
    run->k = (int32_t)(i1 - i0 + 1);
    run->i = (int32_t)i0;
    run->r = (int32_t)st->r;
    run->den = (int32_t)st->den;
    run->inc = (int32_t)st->inc;
    run->x = (int32_t)(x_major ? major : minor);
    run->y = (int32_t)(x_major ? minor : major);
    run->dmx = x_major ? smaj : 0;
    run->dmy = x_major ? 0 : smaj;
    run->dnx = !x_major;
    run->dny = x_major;
    return 1;
}

/* plot for each pixel; fast, when given, takes the stretch whole instead
 * whenever it fits in 32 bits. */
/* walk_line's fast way, for a line within NEAR32 of the surface's corner:
 * every product of the stepper's setup fits in 32 bits there. */
static int near32(int64_t v)
{
    return v >= -NEAR32 && v <= NEAR32;
}

static void walk_near(struct pen *p, int32_t x0, int32_t y0, int32_t x1,
                      int32_t y1, plot_fn plot, run_fn fast, void *ctx)
{
    struct run run;
    int32_t dx = x1 - x0, dy = y1 - y0;
    int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int x_major = adx >= ady;
    int32_t s0 = x_major ? x0 : y0, m0 = x_major ? y0 : x0;
    int32_t n = x_major ? adx : ady, d = x_major ? dy : dx;
    int step = (x_major ? dx : dy) < 0 ? -1 : 1;
    int32_t lo = x_major ? p->cx0 : p->cy0, hi = x_major ? p->cx1 : p->cy1;
    /* visible(), in 32 bits */
    int32_t a = step > 0 ? lo - s0 : s0 - (hi - 1);
    int32_t b = step > 0 ? hi - 1 - s0 : s0 - lo;
    int32_t i0 = a < 0 ? 0 : a, i1 = b > n ? n : b;
    if (i0 > i1)
        return;
    if (!n) {
        plot(p, ctx, x0, y0, 0);
        return;
    }
    /* step_init(), in 32 bits */
    int32_t num = 2 * i0 * d + n, den = 2 * n;
    int32_t q = num / den;
    if (num % den != 0 && num < 0)
        q--;
    run.k = i1 - i0 + 1;
    run.i = i0;
    run.r = num - q * den;
    run.den = den;
    run.inc = 2 * d;
    run.x = x_major ? s0 + i0 * step : m0 + q;
    run.y = x_major ? m0 + q : s0 + i0 * step;
    run.dmx = x_major ? step : 0;
    run.dmy = x_major ? 0 : step;
    run.dnx = !x_major;
    run.dny = x_major;
    fast(p, ctx, &run);
}

static void walk_line(struct pen *p, int64_t x0, int64_t y0, int64_t x1,
                      int64_t y1, plot_fn plot, run_fn fast, void *ctx)
{
    struct run run;
    x0 = clampc(x0) + p->ox;
    y0 = clampc(y0) + p->oy;
    x1 = clampc(x1) + p->ox;
    y1 = clampc(y1) + p->oy;
    if (fast && near32(x0) && near32(y0) && near32(x1) && near32(y1)) {
        walk_near(p, (int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1,
                  plot, fast, ctx);
        return;
    }
    int64_t dx = x1 - x0, dy = y1 - y0;
    int64_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy, i, i0, i1;
    if (adx >= ady) {
        int sx = dx < 0 ? -1 : 1;
        if (!visible(x0, sx, adx, p->cx0, p->cx1, &i0, &i1))
            return;
        if (!adx) {
            plot(p, ctx, x0, y0, 0);
            return;
        }
        struct stepper st;
        step_init(&st, i0, dy, adx);
        if (fast) {
            int f = make_run(&run, x0 + i0 * sx, y0 + st.q, sx, 1, i0, i1,
                             &st);
            if (f >= 0) {
                if (f)
                    fast(p, ctx, &run);
                return;
            }
        }
        for (i = i0; i <= i1; i++, step_next(&st))
            plot(p, ctx, x0 + i * sx, y0 + st.q, i);
    } else {
        int sy = dy < 0 ? -1 : 1;
        if (!visible(y0, sy, ady, p->cy0, p->cy1, &i0, &i1))
            return;
        struct stepper st;
        step_init(&st, i0, dx, ady);
        if (fast) {
            int f = make_run(&run, y0 + i0 * sy, x0 + st.q, sy, 0, i0, i1,
                             &st);
            if (f >= 0) {
                if (f)
                    fast(p, ctx, &run);
                return;
            }
        }
        for (i = i0; i <= i1; i++, step_next(&st))
            plot(p, ctx, x0 + st.q, y0 + i * sy, i);
    }
}

NATIVE(line)
{
    (void)out;
    WHOLE(0, 0, x0, "line");
    WHOLE(1, 0, y0, "line");
    WHOLE(2, 0, x1, "line");
    WHOLE(3, 0, y1, "line");
    COLOUR(4, c, "line");
    turn_px(&vm->d, &x0, &y0);
    turn_px(&vm->d, &x1, &y1);
    struct pen p;
    begin(vm, &p, c, 1);
    p.cost += 8;
    p.per256 = PER16_DOT * 16;
    walk_line(&p, x0, y0, x1, y1, plot_dot, p.pat ? NULL : run_dots, NULL);
    finish(vm, &p);
    return 1;
}

struct tline_ctx
{
    struct vm *vm;
    int64_t mx, my, mdx, mdy;   /* map position and step, 16.16 cells */
};

/* A pixel of the map at mx + i * mdx, my + i * mdy. */
static void plot_map(struct pen *p, void *ctx, int64_t x, int64_t y, int64_t i)
{
    p->cost += PER16_TLINE / 16;
    struct tline_ctx *t = ctx;
    int64_t mx = t->mx + i * t->mdx, my = t->my + i * t->mdy;
    int64_t cx = mx >> 16, cy = my >> 16;
    if (cx < 0 || cy < 0 || cx >= t->vm->map_w || cy >= t->vm->map_h)
        return;
    uint8_t *map = MEM(t->vm, t->vm->map), *sheet = MEM(t->vm, t->vm->sheet);
    uint32_t n = map[cy * t->vm->map_w + cx];
    uint32_t sx = (n & 15) * 8 + (uint32_t)((mx & 0xffff) >> 13);
    uint32_t sy = (n >> 4) * 8 + (uint32_t)((my & 0xffff) >> 13);
    uint8_t c = sheet[sy * SHEET_W + sx];
    if (p->d->transparent[c >> 3] & (1 << (c & 7)))
        return;
    if (x < p->cx0 || x >= p->cx1 || y < p->cy0 || y >= p->cy1)
        return;
    p->s.px[y * p->s.w + x] = p->d->dpal[c];
    p->count++;
    area_touched(p, (int32_t)y, (int32_t)x, (int32_t)x);
}

/* plot_map along a run, with the map position in 32 bits */
static void run_map(struct pen *p, void *ctx, const struct run *run)
{
    p->cost += (uint32_t)run->k * PER16_TLINE / 16;
    struct tline_ctx *t = ctx;
    const uint8_t *map = MEM(t->vm, t->vm->map);
    const uint8_t *sheet = MEM(t->vm, t->vm->sheet);
    const uint16_t *xl = p->xl;
    uint8_t *px = p->s.px;
    const uint32_t mw = (uint32_t)t->vm->map_w, mh = (uint32_t)t->vm->map_h;
    RUN_LOCALS(run);
    const int32_t w = p->s.w, cx0 = p->cx0, cy0 = p->cy0;
    int32_t x = run->x, y = run->y, r = run->r;
    int32_t lo = p->lo, hi = p->hi;
    const uint32_t cw = (uint32_t)(p->cx1 - p->cx0);
    const uint32_t ch = (uint32_t)(p->cy1 - p->cy0);
    uint32_t mx = (uint32_t)(int32_t)(t->mx + run->i * t->mdx);
    uint32_t my = (uint32_t)(int32_t)(t->my + run->i * t->mdy);
    const uint32_t mdx = (uint32_t)(int32_t)t->mdx;
    const uint32_t mdy = (uint32_t)(int32_t)t->mdy;
    uint32_t n = 0;
    for (int32_t k = run->k; k > 0; k--, mx += mdx, my += mdy) {
        int32_t cx = (int32_t)mx >> 16, cy = (int32_t)my >> 16;
        if ((uint32_t)cx < mw && (uint32_t)cy < mh &&
            (uint32_t)(x - cx0) < cw && (uint32_t)(y - cy0) < ch) {
            uint32_t m = map[cy * mw + cx];
            uint32_t sx = (m & 15) * 8 + ((mx & 0xffff) >> 13);
            uint32_t sy = (m >> 4) * 8 + ((my & 0xffff) >> 13);
            uint32_t v = xl[sheet[sy * SHEET_W + sx]];
            if (!(v & XLAT_CLEAR)) {
                px[y * w + x] = (uint8_t)v;
                n++;
                if (y < lo) lo = y;
                if (y > hi) hi = y;
            }
        }
        RUN_STEP(x, y, r);
    }
    p->count += n;
    p->lo = lo;
    p->hi = hi;
}

static int fits32(int64_t v)
{
    return v >= INT32_MIN && v <= INT32_MAX;
}

static int real(struct vm *vm, struct value *a, int argc, int i, int64_t def,
                const char *fn, int64_t *out)
{
    *out = def;
    if (i >= argc || a[i].tag == T_NULL)
        return 1;
    if (!is_num(a[i]))
        return bad(vm, fn, "() needs a number, not a ", a[i]);
    *out = scaled(a[i]);
    return 1;
}

NATIVE(tline)
{
    (void)out;
    WHOLE(0, 0, x0, "tline");
    WHOLE(1, 0, y0, "tline");
    WHOLE(2, 0, x1, "tline");
    WHOLE(3, 0, y1, "tline");
    struct tline_ctx t = { vm, 0, 0, 0, 0 };
    if (!real(vm, a, argc, 4, 0, "tline", &t.mx) ||
        !real(vm, a, argc, 5, 0, "tline", &t.my) ||
        !real(vm, a, argc, 6, 8192, "tline", &t.mdx) ||
        !real(vm, a, argc, 7, 0, "tline", &t.mdy))
        return 0;
    struct pen p;
    begin(vm, &p, 0, 0);
    p.per256 = 0;                    /* each pixel walked is charged */
    /* run_map is exact while the map position fits in 32 bits at every
     * pixel, which holds if it does at both ends of the line. */
    int64_t len = clampc(x1) - clampc(x0), ylen = clampc(y1) - clampc(y0);
    if (len < 0) len = -len;
    if (ylen < 0) ylen = -ylen;
    if (ylen > len) len = ylen;
    int fast = fits32(t.mx) && fits32(t.mdx) && fits32(t.my) &&
               fits32(t.mdy) && fits32(t.mx + len * t.mdx) &&
               fits32(t.my + len * t.mdy);
    walk_line(&p, x0, y0, x1, y1, plot_map, fast ? run_map : NULL, &t);
    finish(vm, &p);
    return 1;
}

/* ---- triangles, rectangles, text ---------------------------------------- */

/* The x of edge (px,py)-(qx,qy) at row y, rounded as walk_line rounds. */
static int64_t edge_x(int64_t px, int64_t py, int64_t qx, int64_t qy, int64_t y)
{
    int64_t n = qy - py;
    if (n < 0) {
        n = -n;
        int64_t t;
        t = px; px = qx; qx = t;
        t = py; py = qy; qy = t;
    }
    return px + floor_div(2 * (y - py) * (qx - px) + n, 2 * n);
}

static int32_t floor32(int32_t n, int32_t d)
{
    int32_t q = n / d;
    return n % d != 0 && n < 0 ? q - 1 : q;
}

/* One edge of a filled triangle, top to bottom: its x at row y is edge_x(),
 * kept by stepping -- a whole and a remainder each row -- from one
 * division at the first row drawn. */
struct edge
{
    int32_t lo, hi, y, x, qx, r, whole, part, den;
    int flat;
};

/* The filled triangle at corners v (camera space), unpatterned and near
 * the surface, in 32 bits: the rows the loop below draws. 0 when it does
 * not apply. */
static int triangle_near(struct pen *p, const int64_t v[6])
{
    int32_t X[3], Y[3];
    if (p->pat)
        return 0;
    for (int i = 0; i < 3; i++) {
        int64_t x = v[i * 2] + p->ox, y = v[i * 2 + 1] + p->oy;
        if (x < -NEAR32 || x > NEAR32 || y < -NEAR32 || y > NEAR32)
            return 0;
        X[i] = (int32_t)x;
        Y[i] = (int32_t)y;
    }
    int32_t ymin = Y[0], ymax = Y[0];
    for (int i = 1; i < 3; i++) {
        if (Y[i] < ymin) ymin = Y[i];
        if (Y[i] > ymax) ymax = Y[i];
    }
    if (ymin < p->cy0)
        ymin = p->cy0;
    if (ymax >= p->cy1)
        ymax = p->cy1 - 1;
    struct edge e[3];
    for (int k = 0; k < 3; k++) {
        struct edge *g = &e[k];
        int32_t px = X[k], py = Y[k], qx = X[(k + 1) % 3], qy = Y[(k + 1) % 3];
        if (py > qy) {
            int32_t t = px; px = qx; qx = t;
            t = py; py = qy; qy = t;
        }
        g->lo = py;
        g->hi = qy;
        g->flat = py == qy;
        g->x = px;
        g->qx = qx;
        if (g->flat)
            continue;
        int32_t n = qy - py, dx = qx - px, ys = ymin > py ? ymin : py;
        int32_t num = 2 * (ys - py) * dx + n;
        g->den = 2 * n;
        int32_t q = floor32(num, g->den);
        g->r = num - q * g->den;
        g->whole = floor32(2 * dx, g->den);
        g->part = 2 * dx - g->whole * g->den;
        g->x = px + q;
        g->y = ys;
    }
    for (int32_t y = ymin; y <= ymax; y++) {
        int32_t l = 0, r = -1;
        int any = 0;
        for (int k = 0; k < 3; k++) {
            struct edge *g = &e[k];
            if (y < g->lo || y > g->hi)
                continue;
            int32_t xa = g->x, xb = g->qx;
            if (!g->flat) {
                if (g->y < y) {
                    g->x += g->whole;
                    g->r += g->part;
                    if (g->r >= g->den) {
                        g->r -= g->den;
                        g->x++;
                    }
                    g->y = y;
                }
                xa = xb = g->x;
            }
            if (!any || xa < l) l = xa;
            if (!any || xa > r) r = xa;
            if (xb < l) l = xb;
            if (xb > r) r = xb;
            any = 1;
        }
        p->cost += 3 * COST_ROW;
        if (any)
            span32(p, y, l, r);
    }
    return 1;
}

/* A filled triangle, its corners v (x, y three times) before the camera */
static void fill_triangle(struct pen *p, int64_t v[6])
{
    /* wholly left or right of the clip: no row is walked */
    int64_t xmin = v[0], xmax = v[0];
    for (int i = 2; i < 6; i += 2) {
        if (v[i] < xmin) xmin = v[i];
        if (v[i] > xmax) xmax = v[i];
    }
    if (xmax + p->ox < p->cx0 || xmin + p->ox >= p->cx1)
        return;
    if (triangle_near(p, v))
        return;
    int64_t ymin = v[1], ymax = v[1];
    for (int i = 3; i < 6; i += 2) {
        if (v[i] < ymin) ymin = v[i];
        if (v[i] > ymax) ymax = v[i];
    }
    if (ymin + p->oy < p->cy0)
        ymin = p->cy0 - p->oy;
    if (ymax + p->oy >= p->cy1)
        ymax = p->cy1 - 1 - p->oy;
    for (int64_t y = ymin; y <= ymax; y++) {
        int64_t l = 0, r = -1;
        int any = 0;
        for (int e = 0; e < 3; e++) {
            int64_t px = v[e * 2], py = v[e * 2 + 1];
            int64_t qx = v[(e * 2 + 2) % 6], qy = v[(e * 2 + 3) % 6];
            int64_t lo = py < qy ? py : qy, hi = py < qy ? qy : py, xs[2];
            int nx = 0;
            if (y < lo || y > hi)
                continue;
            if (py == qy) {
                xs[nx++] = px;
                xs[nx++] = qx;
            } else
                xs[nx++] = edge_x(px, py, qx, qy, y);
            for (int k = 0; k < nx; k++) {
                if (!any || xs[k] < l) l = xs[k];
                if (!any || xs[k] > r) r = xs[k];
                any = 1;
            }
        }
        if (any)
            span(p, y + p->oy, l + p->ox, r + p->ox);
    }
}

static int triangle(struct vm *vm, struct value *a, int argc, int outline,
                    const char *fn)
{
    int64_t v[6];
    for (int i = 0; i < 6; i++) {
        int32_t k;
        if (!whole(vm, a, argc, i, 0, fn, &k))
            return 0;
        v[i] = clampc(k);
    }
    COLOUR(6, c, fn);
    for (int i = 0; i < 6 && vm->d.cam_moved; i += 2) {
        int32_t x = (int32_t)v[i], y = (int32_t)v[i + 1];
        turn_px(&vm->d, &x, &y);
        v[i] = x;
        v[i + 1] = y;
    }
    struct pen p;
    begin(vm, &p, c, 1);
    if (outline) {
        run_fn run = p.pat ? NULL : run_dots;
        p.per256 = PER16_DOT * 16;
        walk_line(&p, v[0], v[1], v[2], v[3], plot_dot, run, NULL);
        walk_line(&p, v[2], v[3], v[4], v[5], plot_dot, run, NULL);
        walk_line(&p, v[4], v[5], v[0], v[1], plot_dot, run, NULL);
        finish(vm, &p);
        return 1;
    }
    fill_triangle(&p, v);
    finish(vm, &p);
    return 1;
}

NATIVE(tri) { (void)out; return triangle(vm, a, argc, 1, "tri"); }
NATIVE(trifill) { (void)out; return triangle(vm, a, argc, 0, "trifill"); }

/* ---- polygons ------------------------------------------------------------ *
 * polyfill(): outlines filled by the even-odd rule, a row at a time. Each
 * outline's lines, and its curves cut into lines, become edges; a row's
 * pixels are those whose middles lie between the 1st and 2nd edge it
 * crosses, the 3rd and 4th, and so on, so an outline inside another is a
 * hole. */

#define POLY_POINTS 8192    /* the most points one call takes */
#define POLY_PIECES 16      /* the most lines a curve is cut into */
#define POLY_HELD   512     /* edges built in one pass, in held_edges */
#define POLY_HELD_ROWS 256  /* and the rows that takes */
#define POLY_RUN_LINES 16   /* the lines in each run polyboxes() boxes */

/* units, measured on the ARM rig (polyprobe): the call's set-up beyond
 * any call's, each point each time the outlines are walked, each edge
 * kept (a division), each edge a row crosses, each step of putting a
 * row's crossings in order, each curve, and each line a curve is cut
 * into, beyond its edge's; each span is a row's set-up, COST_ROW, and its
 * pixels the pen's */
#define POLY_CALL  30
#define POLY_POINT 8
#define POLY_EDGE  8
#define POLY_CROSS 3
#define POLY_SHIFT 1
#define POLY_CURVE 19
#define POLY_PIECE 8
#define POLY_TURN  2    /* each point walked, when the camera turns */
#define POLY_RUN   22   /* each run of an outline polyboxes() boxed */
#define POLY_ASIDE 2    /* each edge wholly left or right of the clip */

/* A row's first edge in starts[], plus one, with this bit flipped where an
 * edge wholly left of the clip starts or ends: such an edge is kept only
 * as the rows where an odd number of them lie left, whose fill then starts
 * at the clip's left. One wholly right is dropped: a row's edges always
 * come in pairs, so an odd number left on it means its fill runs on past
 * the clip's right. */
#define POLY_LEFT  0x80000000u

/* An edge: where it crosses the middle of its next row, and its step a
 * row, both 16.16; the row after its last; and the next edge to start on
 * its first row, plus one */
struct pedge
{
    int64_t x, dx;
    int32_t y1;
    int32_t next;
};

/* The edges built in one pass, their places in a row's order, and the
 * first edge on each row: polyfill's, and a turned rectangle's */
static struct pedge held_edges[POLY_HELD];
static struct pedge *held_act[POLY_HELD];
static int32_t held_starts[POLY_HELD_ROWS];

/* scratch() memory for edges, on an 8-byte boundary: the 6G's compiler
 * moves an edge's 64-bit fields with ldrd and strd, whose result ARMv5
 * leaves undefined off one, and scratch() gives 4 */
static uint8_t *edge_scratch(struct vm *vm, uint32_t bytes)
{
    uint8_t *mem = scratch(vm, bytes + 7);
    return mem ? mem + ((-(uintptr_t)mem) & 7) : NULL;
}

struct poly
{
    struct pedge *e;
    int32_t *starts;            /* the first edge on each row, plus one */
    uint32_t n, max;
    int32_t y0, y1;             /* the clip's rows */
    int32_t lo, hi;             /* the rows the edges cover */
    int counting;               /* only count the edges, for the memory */
    uint32_t curves, pieces;
    int64_t xmin, xmax;         /* how far the edges reach, 16.16 */
    int full;                   /* more edges than max */
    int64_t xl, xr;             /* edges past these are aside, 16.16 */
    uint32_t aside;             /* edges wholly left or right */
    uint32_t measured;          /* edges cut to the clip's rows to test */
    const int32_t *boxes;       /* polyboxes()' runs, or none */
    uint32_t run;               /* the next of them */
    uint32_t *tally;            /* the points walked and the runs looked at */
};

/* (n << 16) / d, d above 0, toward 0: the 64-bit divide, but a byte at a
 * time in 32-bit ones while n and d allow -- on the player a 64-bit divide
 * costs several of those */
static int64_t slope(int64_t n, int64_t d)
{
    uint64_t u = n < 0 ? -(uint64_t)n : (uint64_t)n;
    /* the 32-bit way only while its whole part fits in 16 bits */
    if (u >= 0x80000000u || d >= (1 << 24) || (u >> 16) >= (uint64_t)d)
        return (n << 16) / d;
    uint32_t ud = (uint32_t)d, r = (uint32_t)u % ud;
    uint32_t q = (uint32_t)u / ud << 16;
    q |= (r << 8) / ud << 8;
    r = (r << 8) % ud;
    q |= (r << 8) / ud;
    return n < 0 ? -(int64_t)q : (int64_t)q;
}

/* The line a to b, 16.16 on the surface, as an edge over the rows whose
 * middles it spans; 0 when there are more edges than counted */
static int poly_line(struct poly *g, int64_t ax, int64_t ay, int64_t bx,
                     int64_t by)
{
    if (ay == by)
        return 1;
    if (ay > by) {
        int64_t t = ax; ax = bx; bx = t;
        t = ay; ay = by; by = t;
    }
    /* rows whose middle, y + 1/2, is at least ay and below by */
    int64_t r0 = (ay - 32768 + 65535) >> 16;
    int64_t r1 = (by - 32768 + 65535) >> 16;
    int cut = r0 < g->y0 || r1 > g->y1;
    if (r0 < g->y0)
        r0 = g->y0;
    if (r1 > g->y1)
        r1 = g->y1;
    if (r0 >= r1)
        return 1;
    if (ax < g->xmin) g->xmin = ax;
    if (ax > g->xmax) g->xmax = ax;
    if (bx < g->xmin) g->xmin = bx;
    if (bx > g->xmax) g->xmax = bx;
    /* a pixel clear of the clip either side, wherever its rows cross; an
     * edge reaching past the clip's rows -- a tall wall the camera turns
     * -- by where it crosses the first and last of them, as it can lie
     * clear across those though not across its length */
    int64_t lx = ax < bx ? ax : bx, hx = ax > bx ? ax : bx, dx = 0;
    int sloped = 0;
    if (cut && lx <= g->xr && hx >= g->xl) {
        dx = slope(bx - ax, by - ay);
        sloped = 1;
        if (!g->counting)
            g->measured++;
        int64_t xa = ax + (((r0 << 16) + 32768 - ay) * dx >> 16);
        int64_t xb = xa + (r1 - 1 - r0) * dx;
        lx = xa < xb ? xa : xb;
        hx = xa > xb ? xa : xb;
    }
    int left = hx < g->xl;
    if (left || lx > g->xr) {
        g->aside++;
        if (g->counting)
            return 1;
        if (left) {
            g->starts[r0 - g->y0] ^= (int32_t)POLY_LEFT;
            if (r1 < g->y1)
                g->starts[r1 - g->y0] ^= (int32_t)POLY_LEFT;
            if (r0 < g->lo)
                g->lo = (int32_t)r0;
            if (r1 > g->hi)
                g->hi = (int32_t)r1;
        }
        return 1;
    }
    if (g->counting) {
        g->n++;
        return 1;
    }
    if (g->n >= g->max) {
        g->full = 1;
        return 0;
    }
    struct pedge *e = &g->e[g->n];
    e->dx = sloped ? dx : slope(bx - ax, by - ay);
    e->x = ax + (((r0 << 16) + 32768 - ay) * e->dx >> 16);
    e->y1 = (int32_t)r1;
    if (r0 < g->lo)
        g->lo = (int32_t)r0;
    if (r1 > g->hi)
        g->hi = (int32_t)r1;
    int32_t *st = &g->starts[r0 - g->y0];
    e->next = (int32_t)((uint32_t)*st & ~POLY_LEFT);
    *st = (int32_t)(((uint32_t)*st & POLY_LEFT) | ++g->n);
    return 1;
}

/* How many lines the curve from a by c to b is cut into: enough that
 * none strays a quarter pixel from it */
static int32_t poly_pieces(int64_t ax, int64_t ay, int64_t cx, int64_t cy,
                           int64_t bx, int64_t by)
{
    int64_t dx = ax - 2 * cx + bx, dy = ay - 2 * cy + by;
    uint64_t bend = (uint64_t)((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy)) >> 16;
    int32_t k = 1 + (int32_t)isqrt64(bend / 2);
    return k > POLY_PIECES ? POLY_PIECES : k;
}

/* The curve from a, drawn toward c, to b, as lines */
static int poly_curve(struct poly *g, int64_t ax, int64_t ay, int64_t cx,
                      int64_t cy, int64_t bx, int64_t by)
{
    int32_t k = poly_pieces(ax, ay, cx, cy, bx, by);
    if (!g->counting) {
        g->curves++;
        g->pieces += (uint32_t)k;
    }
    /* a + 2t (c - a) + t^2 (a - 2c + b), t in sixteenths of 1/65536 */
    int64_t ux = 2 * (cx - ax), uy = 2 * (cy - ay);
    int64_t vx = ax - 2 * cx + bx, vy = ay - 2 * cy + by;
    int64_t px = ax, py = ay, dt = 65536 / k;
    for (int32_t i = 1; i <= k; i++) {
        int64_t t = i * dt, tt = t * t >> 16, qx = bx, qy = by;
        if (i < k) {
            qx = ax + (ux * t >> 16) + (vx * tt >> 16);
            qy = ay + (uy * t >> 16) + (vy * tt >> 16);
        }
        if (!poly_line(g, px, py, qx, qy))
            return 0;
        px = qx;
        py = qy;
    }
    return 1;
}

/* Point i of the outline, 16.16 on the surface */
static void poly_point(const struct pen *p, const struct column *xs,
                       const struct column *ys, uint32_t i, int64_t *x,
                       int64_t *y)
{
    *x = column_fix(xs, i);
    *y = column_fix(ys, i);
    if (p->d->cam_moved)
        turn_fix(p->d, x, y);
    *x += p->ox << 16;
    *y += p->oy << 16;
}

/* Walking an outline: the point last on it, and the curve point waiting
 * to bend the next stretch, if any */
struct walk
{
    struct poly *g;
    int64_t ox, oy, cx, cy;
    int ctl;
};

/* The outline on to q: a curve point bends the line through it, and two
 * curve points together have a point on the outline halfway between */
static int walk_to(struct walk *w, int64_t qx, int64_t qy, int curve)
{
    if (curve) {
        if (w->ctl) {
            int64_t mx = (w->cx + qx) / 2, my = (w->cy + qy) / 2;
            if (!poly_curve(w->g, w->ox, w->oy, w->cx, w->cy, mx, my))
                return 0;
            w->ox = mx;
            w->oy = my;
        }
        w->cx = qx;
        w->cy = qy;
        w->ctl = 1;
        return 1;
    }
    if (!(w->ctl ? poly_curve(w->g, w->ox, w->oy, w->cx, w->cy, qx, qy)
                 : poly_line(w->g, w->ox, w->oy, qx, qy)))
        return 0;
    w->ox = qx;
    w->oy = qy;
    w->ctl = 0;
    return 1;
}

/* One outline, points s to e - 1 and back to s, as a TrueType outline:
 * lines from point to point, but a point with bend non-zero bends the
 * line through it */
/* A point's place against the clip's rows and right edge: 1 on or above
 * the first row's middle, 2 below the last's, 4 right of it */
static unsigned poly_code(const struct poly *g, int64_t x, int64_t y,
                          int64_t top, int64_t bot)
{
    return (unsigned)(y <= top) | (unsigned)(y > bot) << 1 |
           (unsigned)(x > g->xr) << 2;
}

/* Point i of a column in 16.16, as column_fix, an array's read in place */
static inline __attribute__((always_inline)) int32_t plain_fix(
    const struct column *c, uint32_t i)
{
    if (c->tag == T_ARR) {
        struct value v = ((const struct value *)c->p)[i];
        if (v.tag == T_FIX)
            return v.v;
        if (v.tag == T_INT)
            return (int32_t)((uint32_t)v.v << 16);
        return 0;
    }
    return column_fix(c, i);
}

/* Point i of the outline, as poly_point, with the turn done in place
 * where it is near: the walk along a long outline is most of its cost */
static inline __attribute__((always_inline)) void plain_point(
    const struct pen *p, const struct column *xs, const struct column *ys,
    uint32_t i, int64_t ox, int64_t oy, int64_t *x, int64_t *y)
{
    int64_t fx = plain_fix(xs, i), fy = plain_fix(ys, i);
    const struct draw *d = p->d;
    if (d->cam_moved) {
        int64_t cx = ((int64_t)d->cam_x << 16) + d->cam_px;
        int64_t cy = ((int64_t)d->cam_y << 16) + d->cam_py;
        int64_t sx = fx - cx, sy = fy - cy;
        if (sx == (int32_t)sx && sy == (int32_t)sy) {
            int32_t x32 = (int32_t)sx, y32 = (int32_t)sy;
            fx = cx + ((mul32(x32, d->cam_cos) - mul32(y32, d->cam_sin)) >> 16);
            fy = cy + ((mul32(x32, d->cam_sin) + mul32(y32, d->cam_cos)) >> 16);
        } else
            turn_fix(d, &fx, &fy);
    }
    *x = fx + ox;
    *y = fy + oy;
}

enum { RUN_WALK, RUN_OVER, RUN_LEFT, RUN_RIGHT };

/* Where a run's box lies on the surface against the clip: wholly above or
 * below its rows, wholly left or right of it, or across it and to be
 * walked. The box's corners are turned as the points are, its reach then
 * a pixel wider for their rounding; one too far out to turn as near is
 * walked. lx and hx are how far it reaches. */
static int run_side(const struct poly *g, const struct pen *p,
                    const int32_t *box, int64_t top, int64_t bot,
                    int64_t *lx, int64_t *hx)
{
    const struct draw *d = p->d;
    int64_t x0 = INT64_MAX, y0 = INT64_MAX, x1 = INT64_MIN, y1 = INT64_MIN;
    for (int k = 0; k < 4; k++) {
        int64_t x = box[k & 1 ? 2 : 0], y = box[k & 2 ? 3 : 1];
        if (d->cam_moved) {
            int64_t cx = ((int64_t)d->cam_x << 16) + d->cam_px;
            int64_t cy = ((int64_t)d->cam_y << 16) + d->cam_py;
            int64_t sx = x - cx, sy = y - cy;
            if (sx != (int32_t)sx || sy != (int32_t)sy)
                return RUN_WALK;
            int32_t x32 = (int32_t)sx, y32 = (int32_t)sy;
            x = cx + ((mul32(x32, d->cam_cos) - mul32(y32, d->cam_sin)) >> 16);
            y = cy + ((mul32(x32, d->cam_sin) + mul32(y32, d->cam_cos)) >> 16);
        }
        x += p->ox << 16;
        y += p->oy << 16;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    x0 -= 65536;
    y0 -= 65536;
    x1 += 65536;
    y1 += 65536;
    *lx = x0;
    *hx = x1;
    if (y1 <= top || y0 > bot)
        return RUN_OVER;
    if (x0 > g->xr)
        return RUN_RIGHT;
    if (x1 < g->xl)
        return RUN_LEFT;
    return RUN_WALK;
}

/* An outline without curve points, s to e - 1 and back to s, as
 * poly_line would take each line: one with both ends above the clip's
 * rows, or both below, or level, makes no edge, and one with both right
 * of it only its count, so neither is set up as an edge -- most of a
 * long level outline when a part of it is in view.
 * With polyboxes()' runs, a run wholly above, below or right of the clip
 * is passed over in the same way without its points; one wholly left
 * sets the rows between its ends as left of the clip, as the line from
 * its first point to its last does, which is what its lines come to. */
static int poly_plain(struct poly *g, const struct pen *p,
                      const struct column *xs, const struct column *ys,
                      uint32_t s, uint32_t e)
{
    int64_t top = ((int64_t)g->y0 << 16) + 32768;
    int64_t bot = ((int64_t)(g->y1 - 1) << 16) + 32768;
    int64_t ax, ay, bx, by, sx, sy;
    int64_t ox = (int64_t)p->ox << 16, oy = (int64_t)p->oy << 16;
    plain_point(p, xs, ys, s, ox, oy, &ax, &ay);
    g->tally[0]++;
    sx = ax;
    sy = ay;
    unsigned ca = poly_code(g, ax, ay, top, bot);
    for (uint32_t i = s + 1, next = s + 1; i <= e; i++) {
        if (g->boxes && i == next) {
            /* a run begins: the lines from point i - 1 to point b */
            uint32_t b = i - 1 + POLY_RUN_LINES < e ? i - 1 + POLY_RUN_LINES
                                                    : e;
            int64_t lx, hx;
            int side = run_side(g, p, g->boxes + 3 + 4 * g->run++, top, bot,
                                &lx, &hx);
            g->tally[1]++;
            next = b + 1;
            if (side != RUN_WALK) {
                if (b < e)
                    plain_point(p, xs, ys, b, ox, oy, &bx, &by);
                else {
                    bx = sx;
                    by = sy;
                }
                g->tally[0]++;
                if (side == RUN_LEFT) {
                    if (ay != by && !poly_line(g, ax, ay, bx, by))
                        return 0;
                } else if (side == RUN_RIGHT) {
                    if (lx < g->xmin) g->xmin = lx;
                    if (hx > g->xmax) g->xmax = hx;
                    g->aside++;
                }
                ax = bx;
                ay = by;
                ca = poly_code(g, ax, ay, top, bot);
                i = b;
                continue;
            }
        }
        if (i < e) {
            plain_point(p, xs, ys, i, ox, oy, &bx, &by);
            g->tally[0]++;
        } else {
            bx = sx;
            by = sy;
        }
        unsigned cb = poly_code(g, bx, by, top, bot), both = ca & cb;
        if (ay != by && !(both & 3)) {
            if (both & 4) {
                /* as poly_line counts it: only where its rows meet the
                 * clip's, and wherever it reaches */
                int64_t ly = ay < by ? ay : by, hy = ay > by ? ay : by;
                int64_t r0 = (ly - 32768 + 65535) >> 16;
                int64_t r1 = (hy - 32768 + 65535) >> 16;
                if (r0 < g->y0)
                    r0 = g->y0;
                if (r1 > g->y1)
                    r1 = g->y1;
                if (r0 < r1) {
                    if (ax < g->xmin) g->xmin = ax;
                    if (ax > g->xmax) g->xmax = ax;
                    if (bx < g->xmin) g->xmin = bx;
                    if (bx > g->xmax) g->xmax = bx;
                    g->aside++;
                }
            } else if (!poly_line(g, ax, ay, bx, by))
                return 0;
        }
        ax = bx;
        ay = by;
        ca = cb;
    }
    return 1;
}

static int poly_outline(struct poly *g, const struct pen *p,
                        const struct column *xs, const struct column *ys,
                        const struct column *bend, uint32_t s, uint32_t e)
{
    if (e - s < 2)
        return 1;
    if (bend->tag == T_INT && bend->k == 0)
        return poly_plain(g, p, xs, ys, s, e);
    uint32_t n = e - s, j = 0;
    while (j < n && column_at(bend, s + j, 0))
        j++;
    struct walk w = { g, 0, 0, 0, 0, 0 };
    int64_t qx, qy;
    if (j < n) {
        /* from the first point on the outline, round to it again */
        poly_point(p, xs, ys, s + j, &w.ox, &w.oy);
        int64_t sx = w.ox, sy = w.oy;
        for (uint32_t k = 1; k < n; k++) {
            uint32_t i = s + (j + k) % n;
            poly_point(p, xs, ys, i, &qx, &qy);
            if (!walk_to(&w, qx, qy, column_at(bend, i, 0) != 0))
                return 0;
        }
        return walk_to(&w, sx, sy, 0);
    }
    /* every point a curve point: from halfway between the first two */
    int64_t ax, ay;
    poly_point(p, xs, ys, s, &ax, &ay);
    poly_point(p, xs, ys, s + 1, &w.cx, &w.cy);
    w.ox = (ax + w.cx) / 2;
    w.oy = (ay + w.cy) / 2;
    w.ctl = 1;
    int64_t sx = w.ox, sy = w.oy;
    for (uint32_t k = 2; k <= n; k++) {
        poly_point(p, xs, ys, s + k % n, &qx, &qy);
        if (!walk_to(&w, qx, qy, 1))
            return 0;
    }
    return walk_to(&w, sx, sy, 0);
}

/* Each outline in turn: ends[k] is the point after outline k's last.
 * 0 with the error set for ends out of order; stops at g full. */
static int poly_edges(struct vm *vm, struct poly *g, const struct pen *p,
                      const struct column *xs, const struct column *ys,
                      const struct column *ends, uint32_t nends,
                      const struct column *bend, uint32_t n)
{
    uint32_t s = 0;
    g->run = 0;
    for (uint32_t k = 0; k <= nends && s < n; k++) {
        uint32_t e = n;
        if (k < nends) {
            int32_t v = column_at(ends, k, 0);
            if (v <= (int32_t)s || (uint32_t)v > n)
                return vm_fail(vm, "polyfill()'s ends must rise, each past "
                               "the last and none past n", "", "");
            e = (uint32_t)v;
        }
        if (!poly_outline(g, p, xs, ys, bend, s, e))
            return 1;               /* g is full */
        s = e;
    }
    return 1;
}

/* The rows the edges in g cover, filled by the even-odd rule; the units
 * the walking and the spans' set-up cost, besides their pixels */
static uint32_t poly_rows(struct pen *p, struct poly *g, struct pedge **act)
{
    /* the edges crossing the row, kept in order of where they cross: a
     * row's order is nearly the last's, so putting it right is cheap */
    uint32_t nact = 0, crossings = 0, shifts = 0, spans = 0;
    struct pedge *e0 = g->e;
    /* whether an odd number of edges lie left of the clip on the row */
    uint32_t left = 0;
    for (int32_t y = p->cy0; y < g->lo; y++)
        left ^= (uint32_t)g->starts[y - p->cy0];
    for (int32_t y = g->lo; y < g->hi; y++) {
        left ^= (uint32_t)g->starts[y - p->cy0];
        /* the last row's edges stepped on to this one, less those ended,
         * then those starting on it */
        uint32_t m = 0;
        for (uint32_t k = 0; k < nact; k++) {
            struct pedge *e = act[k];
            if (y < e->y1) {
                e->x += e->dx;
                act[m++] = e;
            }
        }
        for (int32_t i = (int32_t)((uint32_t)g->starts[y - p->cy0] &
                                   ~POLY_LEFT); i; i = e0[i - 1].next)
            act[m++] = &e0[i - 1];
        nact = m;
        for (uint32_t k = 1; k < m; k++) {
            struct pedge *e = act[k];
            int64_t x = e->x;
            uint32_t at = k;
            while (at && act[at - 1]->x > x) {
                act[at] = act[at - 1];
                at--;
                shifts++;
            }
            act[at] = e;
        }
        crossings += m;
        /* the pixels whose middles lie from one crossing to the next,
         * from the clip's left when one lies left, and on to its right
         * when one is left over */
        uint32_t k = 0;
        int64_t l = p->cx0;
        if (left & POLY_LEFT) {
            int64_t r = m ? ((act[0]->x + 32767) >> 16) - 1 : p->cx1 - 1;
            if (l <= r) {
                span(p, y, l, r);
                spans++;
            }
            k = 1;
        }
        for (; k < m; k += 2) {
            l = (act[k]->x + 32767) >> 16;
            int64_t r = k + 1 < m ? ((act[k + 1]->x + 32767) >> 16) - 1
                                  : p->cx1 - 1;
            if (l <= r) {
                span(p, y, l, r);
                spans++;
            }
        }
    }
    return crossings * POLY_CROSS + shifts * POLY_SHIFT + spans * COST_ROW +
           g->aside * POLY_ASIDE;
}

/* A quadrilateral, its corners 16.16 before the camera's offset, filled
 * as polyfill() fills: a rectangle the camera turns */
static int quad_fill(struct vm *vm, uint32_t c, const int64_t qx[4],
                     const int64_t qy[4])
{
    struct pen p;
    begin(vm, &p, c, 1);
    uint32_t rows = (uint32_t)(p.cy1 > p.cy0 ? p.cy1 - p.cy0 : 0);
    /* the four edges held; the rows' starts too, but for an image taller
     * than those take */
    int32_t *starts = held_starts;
    if (rows > POLY_HELD_ROWS) {
        starts = scratch(vm, rows * 4);
        if (!starts)
            return 0;
        begin(vm, &p, c, 1);
    } else
        memset(held_starts, 0, rows * 4);
    struct poly g = { held_edges, starts, 0, 4, p.cy0, p.cy1, p.cy1, p.cy0,
                      0, 0, 0, 0, 0, 0, ((int64_t)p.cx0 - 1) << 16,
                      ((int64_t)p.cx1 + 1) << 16, 0, 0, NULL, 0, NULL };
    for (int k = 0; k < 4; k++)
        poly_line(&g, qx[k] + (p.ox << 16), qy[k] + (p.oy << 16),
                  qx[(k + 1) & 3] + (p.ox << 16),
                  qy[(k + 1) & 3] + (p.oy << 16));
    p.cost += COST_RECT + 4 * POLY_EDGE + poly_rows(&p, &g, held_act);
    finish(vm, &p);
    return 1;
}

/* The runs polyboxes() cuts the outlines into: POLY_RUN_LINES lines each,
 * from each outline's start, the last of an outline as many as are left;
 * an outline of fewer than two points has none. fold, where each outline
 * ends, folded into a number, tells boxes for other ends. 0 with the error
 * set for ends out of order. */
static int poly_runs(struct vm *vm, const char *fn,
                     const struct column *ends, uint32_t nends, uint32_t n,
                     uint32_t *runs, uint32_t *fold)
{
    uint32_t s = 0;
    *runs = 0;
    *fold = 0;
    for (uint32_t k = 0; k <= nends && s < n; k++) {
        uint32_t e = n;
        if (k < nends) {
            int32_t v = column_at(ends, k, 0);
            if (v <= (int32_t)s || (uint32_t)v > n)
                return vm_fail(vm, fn, "()'s ends must rise, each past the "
                               "last and none past n", "");
            e = (uint32_t)v;
        }
        if (e - s >= 2)
            *runs += (e - s + POLY_RUN_LINES - 1) / POLY_RUN_LINES;
        *fold = *fold * 31 + e;
        s = e;
    }
    return 1;
}

/* polyboxes(xs, ys, n, [ends]): polyfill()'s outlines in runs, each with
 * the box round its points, in an Int32Array: n, the runs, the ends'
 * fold, then each run's x0, y0, x1, y1 in 16.16 */
NATIVE(polyboxes)
{
    struct column xs, ys, ends;
    ends.tag = T_INT;
    ends.k = 0;
    if (!column_of(vm, a[0], &xs) || !column_of(vm, a[1], &ys) ||
        xs.tag == T_INT || ys.tag == T_INT || !is_num(a[2]))
        return vm_fail(vm, "polyboxes() needs arrays of x and y, then how "
                       "many points", "", "");
    int32_t n = floor_int(a[2]);
    if (n < 0)
        n = 0;
    if (n > POLY_POINTS)
        return vm_fail(vm, "polyboxes() takes at most 8192 points", "", "");
    if ((uint32_t)n > xs.len || (uint32_t)n > ys.len)
        return vm_fail(vm, "polyboxes() has fewer x or y than points", "",
                       "");
    uint32_t nends = 0, runs, fold;
    if (argc > 3 && a[3].tag != T_NULL) {
        if (!column_of(vm, a[3], &ends) || ends.tag == T_INT)
            return vm_fail(vm, "polyboxes()'s ends is an array", "", "");
        nends = ends.len;
    }
    if (!poly_runs(vm, "polyboxes", &ends, nends, (uint32_t)n, &runs, &fold))
        return 0;
    uint32_t off = gc_alloc(vm, T_I32,
                            (uint32_t)(sizeof(struct i32) -
                                       sizeof(struct hdr)) + (3 + 4 * runs) * 4);
    if (!off)
        return 0;
    struct i32 *box = (struct i32 *)MEM(vm, off);
    box->len = 3 + 4 * runs;
    box->d[0] = n;
    box->d[1] = (int32_t)runs;
    box->d[2] = (int32_t)fold;
    /* the allocation may have collected: the arrays found again */
    column_of(vm, a[0], &xs);
    column_of(vm, a[1], &ys);
    if (nends)
        column_of(vm, a[3], &ends);
    int32_t *r = box->d + 3;
    uint32_t s = 0;
    for (uint32_t k = 0; k <= nends && s < (uint32_t)n; k++) {
        uint32_t e = k < nends ? (uint32_t)column_at(&ends, k, 0) : (uint32_t)n;
        if (e - s >= 2)
            for (uint32_t i = s; i < e; i += POLY_RUN_LINES, r += 4) {
                /* points i to the run's last, the outline's first after
                 * its last */
                uint32_t b = i + POLY_RUN_LINES < e ? i + POLY_RUN_LINES : e;
                r[0] = r[1] = INT32_MAX;
                r[2] = r[3] = INT32_MIN;
                for (uint32_t j = i; j <= b; j++) {
                    uint32_t q = j < e ? j : s;
                    int32_t x = column_fix(&xs, q), y = column_fix(&ys, q);
                    if (x < r[0]) r[0] = x;
                    if (y < r[1]) r[1] = y;
                    if (x > r[2]) r[2] = x;
                    if (y > r[3]) r[3] = y;
                }
            }
        s = e;
    }
    *out = mk(T_I32, (int32_t)off);
    vm_charge(vm, POLY_CALL + (uint32_t)n * POLY_POINT / 2 + runs);
    return 1;
}

/* polyfill(xs, ys, n, [c], [ends], [bend], [boxes]): the n points as
 * outlines, filled by the even-odd rule. ends, when given, is where each
 * outline ends (the index after its last point); bend, where non-zero,
 * makes a point bend the line through it rather than be on it; boxes,
 * from polyboxes() for these outlines, lets the runs out of view be
 * passed over. */
NATIVE(polyfill)
{
    (void)out;
    struct column xs, ys, ends, bend;
    if (!column_of(vm, a[0], &xs) || !column_of(vm, a[1], &ys) ||
        xs.tag == T_INT || ys.tag == T_INT || !is_num(a[2]))
        return vm_fail(vm, "polyfill() needs arrays of x and y, then how "
                       "many points", "", "");
    int32_t n = floor_int(a[2]);
    if (n < 0)
        n = 0;
    if (n > POLY_POINTS)
        return vm_fail(vm, "polyfill() takes at most 8192 points", "", "");
    if ((uint32_t)n > xs.len || (uint32_t)n > ys.len)
        return vm_fail(vm, "polyfill() has fewer x or y than points", "", "");
    COLOUR(3, c, "polyfill");
    uint32_t nends = 0;
    ends.tag = bend.tag = T_INT;
    ends.k = bend.k = 0;
    ends.len = bend.len = 0xffffffffu;
    ends.p = bend.p = NULL;
    if (argc > 4 && a[4].tag != T_NULL) {
        if (!column_of(vm, a[4], &ends) || ends.tag == T_INT)
            return vm_fail(vm, "polyfill()'s ends is an array", "", "");
        nends = ends.len;
    }
    if (argc > 5 && a[5].tag != T_NULL &&
        (!column_of(vm, a[5], &bend) || bend.tag == T_INT ||
         bend.len < (uint32_t)n))
        return vm_fail(vm, "polyfill()'s bend is an array, one for each "
                       "point", "", "");
    const int32_t *boxes = NULL;
    if (argc > 6 && a[6].tag != T_NULL) {
        uint32_t runs, fold;
        if (a[6].tag != T_I32)
            return vm_fail(vm, "polyfill()'s boxes are an Int32Array from "
                           "polyboxes()", "", "");
        if (argc > 5 && a[5].tag != T_NULL)
            return vm_fail(vm, "polyfill()'s boxes are for outlines without "
                           "bend", "", "");
        if (!poly_runs(vm, "polyfill", &ends, nends, (uint32_t)n, &runs,
                       &fold))
            return 0;
        const struct i32 *b = I32(vm, a[6]);
        if (b->len != 3 + 4 * runs || b->d[0] != n ||
            b->d[1] != (int32_t)runs || b->d[2] != (int32_t)fold)
            return vm_fail(vm, "polyfill()'s boxes are from polyboxes() for "
                           "other points or ends", "", "");
        boxes = b->d;
    }
    struct pen p;
    begin(vm, &p, c, 1);
    if (n < 3 || p.cy0 >= p.cy1 || p.cx0 >= p.cx1) {
        finish(vm, &p);
        return 1;
    }
    /* Built in one pass into the held edges when they fit, which is
     * nearly always; otherwise counted, then built again into scratch */
    uint32_t rows = (uint32_t)(p.cy1 - p.cy0), walks = 1;
    int64_t xl = ((int64_t)p.cx0 - 1) << 16, xr = ((int64_t)p.cx1 + 1) << 16;
    struct poly g = { held_edges, held_starts, 0, POLY_HELD, p.cy0, p.cy1,
                      p.cy1, p.cy0, 0, 0, 0, (int64_t)1 << 62,
                      -((int64_t)1 << 62), 0, xl, xr, 0, 0, NULL, 0, NULL };
    struct pedge **act = held_act;
    uint32_t tally[2] = { 0, 0 };
    g.boxes = boxes;
    g.tally = tally;
    if (rows <= POLY_HELD_ROWS) {
        memset(held_starts, 0, rows * 4);
        if (!poly_edges(vm, &g, &p, &xs, &ys, &ends, nends, &bend,
                        (uint32_t)n))
            return 0;
    }
    if (rows > POLY_HELD_ROWS || g.full) {
        walks += rows <= POLY_HELD_ROWS;
        struct poly count = { NULL, NULL, 0, 0, p.cy0, p.cy1, p.cy1, p.cy0,
                              1, 0, 0, (int64_t)1 << 62, -((int64_t)1 << 62),
                              0, xl, xr, 0, 0, NULL, 0, NULL };
        g = count;
        g.boxes = boxes;
        g.tally = tally;
        if (!poly_edges(vm, &g, &p, &xs, &ys, &ends, nends, &bend,
                        (uint32_t)n))
            return 0;
        uint32_t max = g.n;
        if ((g.n || g.aside) && g.xmax >= ((int64_t)p.cx0 << 16) &&
            g.xmin < ((int64_t)p.cx1 << 16)) {
            uint32_t at_act = max * sizeof(struct pedge);
            uint32_t at_starts = at_act + max * sizeof(struct pedge *);
            uint8_t *mem = edge_scratch(vm, at_starts + rows * 4);
            if (!mem)
                return 0;
            /* scratch may have collected: the arrays and surface found
             * again */
            column_of(vm, a[0], &xs);
            column_of(vm, a[1], &ys);
            if (nends)
                column_of(vm, a[4], &ends);
            if (argc > 5 && a[5].tag != T_NULL)
                column_of(vm, a[5], &bend);
            if (boxes)
                boxes = I32(vm, a[6])->d;
            g.boxes = boxes;
            begin(vm, &p, c, 1);
            g.e = (struct pedge *)(void *)mem;
            g.starts = (int32_t *)(void *)(mem + at_starts);
            g.n = 0;
            g.aside = 0;
            g.measured = 0;
            g.max = max;
            g.counting = 0;
            act = (struct pedge **)(void *)(mem + at_act);
            walks++;
            if (!poly_edges(vm, &g, &p, &xs, &ys, &ends, nends, &bend,
                            (uint32_t)n))
                return 0;
            if (g.full)
                return vm_fail(vm, "polyfill() has more edges than it "
                               "counted", "", "");
        }
    }
    /* with boxes, the points the walk took; without, every point each
     * time the outlines were walked */
    uint32_t walked = boxes ? tally[0] : (uint32_t)n * walks;
    p.cost += POLY_CALL + walked * POLY_POINT + tally[1] * POLY_RUN +
              (g.counting ? 0 : g.n * POLY_EDGE + g.curves * POLY_CURVE +
                                g.pieces * POLY_PIECE + g.measured * POLY_EDGE);
    if (vm->d.cam_moved)
        p.cost += walked * POLY_TURN;
    /* wholly left or right of the clip, or above or below it: no row is
     * walked */
    if (!g.counting && (g.n || g.aside) &&
        g.xmax >= ((int64_t)p.cx0 << 16) &&
        g.xmin < ((int64_t)p.cx1 << 16))
        p.cost += poly_rows(&p, &g, act);
    finish(vm, &p);
    return 1;
}

/* polyline(xs, ys, n, [c], [ends], [closed]): a line from each point to
 * the next. ends, when given, is where each run of points ends (the
 * index after its last), as polyfill takes outlines; closed joins each
 * run's last point back to its first. c is a colour, or an array of one
 * for each line -- line i's from point i. */
NATIVE(polyline)
{
    (void)out;
    struct column xs, ys, ends, cols;
    if (!column_of(vm, a[0], &xs) || !column_of(vm, a[1], &ys) ||
        xs.tag == T_INT || ys.tag == T_INT || !is_num(a[2]))
        return vm_fail(vm, "polyline() needs arrays of x and y, then how "
                       "many points", "", "");
    int32_t n = floor_int(a[2]);
    if (n < 0)
        n = 0;
    if (n > POLY_POINTS)
        return vm_fail(vm, "polyline() takes at most 8192 points", "", "");
    if ((uint32_t)n > xs.len || (uint32_t)n > ys.len)
        return vm_fail(vm, "polyline() has fewer x or y than points", "", "");
    uint32_t c = vm->d.pen;
    cols.tag = T_NULL;
    if (argc > 3 && (a[3].tag == T_ARR || a[3].tag == T_I32 ||
                     a[3].tag == T_U8)) {
        column_of(vm, a[3], &cols);
        if (cols.len < (uint32_t)n)
            return vm_fail(vm, "polyline()'s colours are one for each "
                           "point", "", "");
    } else if (!colour(vm, a, argc, 3, "polyline", &c))
        return 0;
    uint32_t nends = 0;
    if (argc > 4 && a[4].tag != T_NULL) {
        if (!column_of(vm, a[4], &ends) || ends.tag == T_INT)
            return vm_fail(vm, "polyline()'s ends is an array", "", "");
        nends = ends.len;
    }
    int closed = argc > 5 && vm_truthy(vm, a[5]);
    const struct draw *d = &vm->d;
    struct pen p;
    begin(vm, &p, c, 1);
    p.per256 = PER16_DOT * 16;
    uint32_t s = 0;
    for (uint32_t k = 0; k <= nends && s < (uint32_t)n; k++) {
        uint32_t e = (uint32_t)n;
        if (k < nends) {
            int32_t v = column_at(&ends, k, 0);
            if (v <= (int32_t)s || v > n)
                return vm_fail(vm, "polyline()'s ends must rise, each past "
                               "the last and none past n", "", "");
            e = (uint32_t)v;
        }
        /* each point turned once, the line's end the next one's start */
        int32_t fx0 = column_at(&xs, s, 0), fy0 = column_at(&ys, s, 0);
        turn_px(d, &fx0, &fy0);
        int32_t x0 = fx0, y0 = fy0;
        for (uint32_t i = s; i < e; i++) {
            int32_t x1 = fx0, y1 = fy0;
            if (i + 1 < e) {
                x1 = column_at(&xs, i + 1, 0);
                y1 = column_at(&ys, i + 1, 0);
                turn_px(d, &x1, &y1);
            } else if (!closed || e - s < 3)
                break;
            if (cols.tag != T_NULL) {
                uint32_t ci = (uint32_t)column_at(&cols, i, 0);
                p.c = d->dpal[ci & 0xff];
                p.alt_on = p.pat && ((ci >> 8) & 0xff);
                p.alt = d->dpal[(ci >> 8) & 0xff];
            }
            p.cost += 18;           /* a line's setup, as core_draw_lines */
            walk_line(&p, x0, y0, x1, y1, plot_dot,
                      p.pat ? NULL : run_dots, NULL);
            x0 = x1;
            y0 = y1;
        }
        s = e;
    }
    finish(vm, &p);
    return 1;
}

/* Rows from..to (inside the clip) of an unpatterned rectangle x0..x1:
 * filled, each row whole; hollow, its two edge columns. */
static void rows(struct pen *p, int32_t from, int32_t to, int64_t x0,
                 int64_t x1, int hollow)
{
    int32_t w = p->s.w, h = to - from + 1;
    uint8_t *row = p->s.px + from * w, c = p->c;
    uint32_t dots;
    if (!hollow) {
        int64_t l = x0 < p->cx0 ? p->cx0 : x0;
        int64_t r = x1 >= p->cx1 ? p->cx1 - 1 : x1;
        if (l > r)
            return;
        for (int32_t k = h; k > 0; k--, row += w)
            memset(row + l, c, (size_t)(r - l + 1));
        dots = (uint32_t)(r - l + 1);
    } else {
        int left = x0 >= p->cx0 && x0 < p->cx1;
        int right = x1 >= p->cx0 && x1 < p->cx1;
        if (!left && !right)
            return;
        for (int32_t k = h; k > 0; k--, row += w) {
            if (left)
                row[x0] = c;
            if (right)
                row[x1] = c;
        }
        dots = (uint32_t)(left + right);
    }
    p->count += dots * (uint32_t)h;
    p->cost += (uint32_t)h;
    row_touched(p, from);
    row_touched(p, to);
}

/* A rectangle the camera turns: filled, the pixels' area x0..x1 + 1 as a
 * quadrilateral; outlined, four lines through its corner pixels */
static int turned_rect(struct vm *vm, const int64_t b[4], uint32_t c,
                       int outline)
{
    const struct draw *d = &vm->d;
    if (!outline) {
        int64_t qx[4] = { b[0], b[2] + 1, b[2] + 1, b[0] };
        int64_t qy[4] = { b[1], b[1], b[3] + 1, b[3] + 1 };
        for (int k = 0; k < 4; k++) {
            qx[k] <<= 16;
            qy[k] <<= 16;
            turn_fix(d, &qx[k], &qy[k]);
        }
        return quad_fill(vm, c, qx, qy);
    }
    int32_t x[4] = { (int32_t)b[0], (int32_t)b[2], (int32_t)b[2],
                     (int32_t)b[0] };
    int32_t y[4] = { (int32_t)b[1], (int32_t)b[1], (int32_t)b[3],
                     (int32_t)b[3] };
    for (int k = 0; k < 4; k++)
        turn_px(d, &x[k], &y[k]);
    struct pen p;
    begin(vm, &p, c, 1);
    p.cost += COST_RECT;
    p.per256 = PER16_DOT * 16;
    for (int k = 0; k < 4; k++)
        walk_line(&p, x[k], y[k], x[(k + 1) & 3], y[(k + 1) & 3], plot_dot,
                  p.pat ? NULL : run_dots, NULL);
    finish(vm, &p);
    return 1;
}

static int rectangle(struct vm *vm, struct value *a, int argc, int outline,
                     const char *fn)
{
    int64_t b[4];
    if (!box(vm, a, argc, fn, b, FAR))
        return 0;
    COLOUR(4, c, fn);
    if (vm->d.cam_moved)
        return turned_rect(vm, b, c, outline);
    struct pen p;
    begin(vm, &p, c, 1);
    p.cost += COST_RECT;
    int64_t x0 = b[0] + p.ox, x1 = b[2] + p.ox;
    int64_t y0 = b[1] + p.oy, y1 = b[3] + p.oy;
    /* An outline two pixels thick or less is all edge. */
    int hollow = outline && y1 - y0 >= 2 && x1 - x0 >= 2;
    int64_t from = y0 + hollow, to = y1 - hollow;
    if (from < p.cy0)
        from = p.cy0;
    if (to >= p.cy1)
        to = p.cy1 - 1;
    if (hollow) {
        span(&p, y0, x0, x1);
        span(&p, y1, x0, x1);
    }
    if (!p.pat) {
        /* Unpatterned, the rows are clipped once and written in 32 bits:
         * span()'s and dot()'s pixels, without a clipped call for each. */
        if (from <= to)
            rows(&p, (int32_t)from, (int32_t)to, x0, x1, hollow);
        finish(vm, &p);
        return 1;
    }
    for (int64_t y = from; y <= to; y++) {
        if (hollow) {
            dot(&p, x0, y);
            dot(&p, x1, y);
        } else
            span(&p, y, x0, x1);
    }
    finish(vm, &p);
    return 1;
}

NATIVE(rect) { (void)out; return rectangle(vm, a, argc, 1, "rect"); }
NATIVE(rectfill) { (void)out; return rectangle(vm, a, argc, 0, "rectfill"); }

NATIVE(pset)
{
    (void)out;
    WHOLE(0, 0, x, "pset");
    WHOLE(1, 0, y, "pset");
    COLOUR(2, c, "pset");
    turn_px(&vm->d, &x, &y);
    /* The screen without a fill pattern, the usual case, without a pen */
    struct draw *d = &vm->d;
    if (d->target.tag != T_IMG && !d->fill) {
        int64_t px = (int64_t)x - d->cam_x, py = (int64_t)y - d->cam_y;
        if (px >= d->clip_x0 && px >= 0 && px < d->clip_x1 && px < SCREEN_W &&
            py >= d->clip_y0 && py >= 0 && py < d->clip_y1 && py < SCREEN_H) {
            ((uint8_t *)MEM(vm, vm->screen))[py * SCREEN_W + px] =
                d->dpal[c & 0xff];
            screen_written(vm, (int32_t)py, (int32_t)py);
        }
        vm_charge(vm, 6);
        return 1;
    }
    struct pen p;
    begin(vm, &p, c, 1);
    p.cost = 6;
    dot(&p, (int64_t)x + p.ox, (int64_t)y + p.oy);
    finish(vm, &p);
    return 1;
}

NATIVE(pget)
{
    struct surf s;
    WHOLE(0, 0, x, "pget");
    WHOLE(1, 0, y, "pget");
    turn_px(&vm->d, &x, &y);
    surf_of(vm, vm->d.target, &s, "pget");
    int64_t px = (int64_t)x - vm->d.cam_x, py = (int64_t)y - vm->d.cam_y;
    *out = mk(T_INT, px < 0 || py < 0 || px >= s.w || py >= s.h ? 0
              : s.px[py * s.w + px]);
    vm_charge(vm, 5);
    return 1;
}

NATIVE(cls)
{
    (void)out;
    struct surf s;
    WHOLE(0, 0, c, "cls");
    surf_of(vm, vm->d.target, &s, "cls");
    memset(s.px, (uint8_t)c, (size_t)s.w * (size_t)s.h);
    full_clip(vm);
    vm->d.cur_x = vm->d.cur_y = 0;
    if (s.is_screen) {
        vm->d.dirty_lo = 0;
        vm->d.dirty_hi = (int16_t)(s.h - 1);
    }
    /* measured on the rig (cal): about 195 units a screen, a memset */
    vm_charge(vm, COST_CALL + (uint32_t)(s.w * s.h) / CLS_PIXELS);
    return 1;
}

/* The text of print's pieces -- a[0] is their count -- in one buffer. */
static uint32_t gather(struct vm *vm, struct value *a, int argc, char *buf,
                       uint32_t cap)
{
    uint32_t n = 0;
    int parts = a[0].v;
    for (int i = 1; i <= parts && i < argc && n < cap; i++)
        n += vm_text(vm, a[i], buf + n, cap - n);
    return n;
}

static int scale_of(struct vm *vm)
{
    return vm->d.font == 1 ? 2 : vm->d.font_big + 1;
}

/* Each small glyph's dots, and its first and last rows with any, worked
 * out once rather than at every glyph drawn */
static uint8_t glyph_dots[95], glyph_rows[95];  /* top << 4 | bottom */
static int glyph_known;

static void glyph_table(void)
{
    for (int i = 0; i < 95; i++) {
        uint16_t g = font_small[i];
        int top = -1, bot = 0, dots = 0;
        for (int r = 0; r < 5; r++) {
            uint32_t bits = (g >> (12 - r * 3)) & 7;
            if (!bits)
                continue;
            dots += (int)((bits & 1) + ((bits >> 1) & 1) + (bits >> 2));
            if (top < 0)
                top = r;
            bot = r;
        }
        glyph_dots[i] = (uint8_t)dots;
        glyph_rows[i] = (uint8_t)(top < 0 ? 0xff : top << 4 | bot);
    }
    glyph_known = 1;
}

/* Small glyph i wholly inside the clip, unpatterned, k pixels to a dot:
 * the pixels span() would draw, without a clipped call for each. */
static void glyph(struct pen *p, uint32_t i, int32_t x, int32_t y, int k)
{
    uint32_t rows = glyph_rows[i];
    if (rows == 0xff)
        return;
    uint16_t g = font_small[i];
    int32_t w = p->s.w;
    int top = (int)(rows >> 4), bot = (int)(rows & 15);
    uint8_t ink = p->c, *row = p->s.px + (y + top * k) * w + x;
    for (int r = top; r <= bot; r++, row += w * k) {
        uint32_t bits = (g >> (12 - r * 3)) & 7;
        if (k == 1) {
            if (bits & 4) row[0] = ink;
            if (bits & 2) row[1] = ink;
            if (bits & 1) row[2] = ink;
        } else if (k == 2) {
            for (uint8_t *d = row; d < row + 2 * w; d += w) {
                if (bits & 4) d[0] = d[1] = ink;
                if (bits & 2) d[2] = d[3] = ink;
                if (bits & 1) d[4] = d[5] = ink;
            }
        } else
            for (int dy = 0; dy < k; dy++) {
                uint8_t *d = row + dy * w;
                for (int c = 0; c < 3; c++, d += k)
                    if ((bits >> (2 - c)) & 1)
                        for (int dx = 0; dx < k; dx++)
                            d[dx] = ink;
            }
    }
    p->count += glyph_dots[i] * (uint32_t)(k * k);
    area_touched(p, y + top * k, x, x + 3 * k - 1);
    area_touched(p, y + bot * k + k - 1, x, x + 3 * k - 1);
}

/* Draws text at (x, y) in camera space, or with draw 0 only measures it.
 * Returns the x after the last glyph; *widest is the longest line. */
static int64_t text(struct pen *p, const char *s, uint32_t n, int64_t x,
                    int64_t y, int k, int draw, int64_t *widest, int *lines)
{
    int64_t cx = x;
    *widest = 0;
    *lines = 1;
    if (draw && !glyph_known)
        glyph_table();
    /* whether the line's glyphs lie inside the clip's rows, once a line */
    int rows_in = draw && !p->pat && y + p->oy >= p->cy0 &&
                  y + p->oy + 5 * k <= p->cy1;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)s[i];
        if (ch == '\n') {
            if (cx - x > *widest)
                *widest = cx - x;
            cx = x;
            y += 6 * k;
            (*lines)++;
            rows_in = draw && !p->pat && y + p->oy >= p->cy0 &&
                      y + p->oy + 5 * k <= p->cy1;
            continue;
        }
        if (ch >= 0x80 && ch < 0xc0)
            continue;               /* the rest of a UTF-8 character */
        if (ch < 32 || ch > 126)
            ch = ch >= 0x80 ? '?' : ' ';
        uint16_t g = font_small[ch - 32];
        if (draw && g)
            p->cost += k == 1 ? COST_GLYPH : COST_BIG_GLYPH;
        if (draw && g && rows_in && cx + p->ox >= p->cx0 &&
            cx + p->ox + 3 * k <= p->cx1) {
            glyph(p, ch - 32u, (int32_t)(cx + p->ox), (int32_t)(y + p->oy),
                  k);
        } else if (draw && g) {
            for (int r = 0; r < 5; r++)
                for (int c = 0; c < 3; c++)
                    if ((g >> (14 - r * 3 - c)) & 1)
                        for (int dy = 0; dy < k; dy++)
                            span(p, y + r * k + dy + p->oy,
                                 cx + c * k + p->ox,
                                 cx + c * k + k - 1 + p->ox);
        }
        cx += 4 * k;
    }
    if (cx - x > *widest)
        *widest = cx - x;
    return cx;
}

/* ---- FONT.WORLD and the cart's fonts ---- *
 * Each glyph its own advance and box in the line. WORLD's are 12-row cells
 * a byte wide: the letters, punctuation and symbols below U+3000 are the
 * engine's own; kana, Hangul and the ideographs come from the host, a
 * glyph at a time. A cart's come from its FONT chunk, up to 32 a side. */

#define TEXT_LINE 12

struct glyph_box
{
    const uint8_t *bits;            /* h rows of stride bytes */
    int32_t adv, x, y, w, h, stride;
};

/* The next character of UTF-8 text at *i, which moves past it; a byte
 * that does not begin one is taken alone, as U+FFFD */
static uint32_t utf8_next(const char *s, uint32_t n, uint32_t *i)
{
    uint8_t c = (uint8_t)s[(*i)++];
    int more = c >= 0xf0 && c < 0xf8 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (c >= 0x80 && !more)
        return 0xfffd;
    uint32_t cp = more == 3 ? c & 7 : more == 2 ? c & 15 : more ? c & 31 : c;
    for (int k = 0; k < more; k++) {
        if (*i >= n || ((uint8_t)s[*i] & 0xc0) != 0x80)
            return 0xfffd;
        cp = cp << 6 | ((uint8_t)s[(*i)++] & 63);
    }
    return cp;
}

/* cp's WORLD glyph: the engine's, the host's (into buf), or the engine's ? */
static const uint8_t *text_glyph(struct vm *vm, uint32_t cp, uint8_t *buf)
{
    if (cp >= 0x20 && cp < 0x7f && textfont_cp[cp - 0x20] == cp)
        return textfont_glyph[cp - 0x20];
    if (cp < 0x10000) {
        int32_t lo = 0, hi = TEXTFONT_CORE - 1;
        while (lo <= hi) {
            int32_t m = (lo + hi) / 2;
            if (textfont_cp[m] == cp)
                return textfont_glyph[m];
            if (textfont_cp[m] < cp)
                lo = m + 1;
            else
                hi = m - 1;
        }
    }
    if (cp >= 0x3000 && vm->host && vm->host->glyph &&
        vm->host->glyph(vm->host->ctx, cp, buf))
        return buf;
    return textfont_glyph['?' - 0x20];
}

/* cp's glyph in a cart font, or NULL */
static const uint8_t *cart_find(const struct cartfont *f, uint32_t cp)
{
    /* most fonts run unbroken from their first codepoint */
    uint32_t guess = cp - rd_le32(f->index);
    if (guess < f->n && rd_le32(f->index + guess * 8) == cp)
        return f->glyphs + rd_le32(f->index + guess * 8 + 4);
    int32_t lo = 0, hi = (int32_t)f->n - 1;
    while (lo <= hi) {
        int32_t m = (lo + hi) / 2;
        uint32_t at = rd_le32(f->index + m * 8);
        if (at == cp)
            return f->glyphs + rd_le32(f->index + m * 8 + 4);
        if (at < cp)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return NULL;
}

/* cp's glyph in a cart font: its own, its ?, or nothing at all */
static void cart_glyph(const struct cartfont *f, uint32_t cp,
                       struct glyph_box *b)
{
    const uint8_t *g = cart_find(f, cp);
    if (!g)
        g = cart_find(f, '?');
    if (!g) {
        b->adv = b->x = b->y = b->w = b->h = b->stride = 0;
        b->bits = NULL;
        return;
    }
    b->adv = g[0];
    b->x = (int8_t)g[1];
    b->y = (int8_t)g[2];
    b->w = g[3];
    b->h = g[4];
    b->stride = (g[3] + 7) >> 3;
    b->bits = g + 5;
}

/* A glyph's row as a word, its first pixel in the top bit */
static uint32_t row_word(const uint8_t *row, int stride, int w)
{
    uint32_t v = (uint32_t)row[0] << 24;
    for (int j = 1; j < stride; j++)
        v |= (uint32_t)row[j] << (24 - 8 * j);
    /* a cart's font can set bits past the width: they would draw outside
     * the box the clip was tested against */
    return w < 32 ? v & ~(0xffffffffu >> w) : v;
}

/* A glyph at (gx, gy) in screen space, k pixels to a dot: nothing when
 * wholly outside the clip; wholly inside and unpatterned, its dots written
 * straight to the pixels, each row only as far as its last; otherwise
 * drawn by span(). */
static void draw_glyph(struct pen *p, const struct glyph_box *b, int64_t gx,
                       int64_t gy, int k)
{
    const uint8_t *row = b->bits;
    p->cost += COST_TEXT_GLYPH;
    if (!p->pat && gx >= p->cx0 && gy >= p->cy0 && gx + b->w * k <= p->cx1 &&
        gy + b->h * k <= p->cy1) {
        int32_t w = p->s.w, top = -1, bot = 0;
        uint32_t dots = 0;
        uint8_t ink = p->c, *line = p->s.px + gy * w + gx;
        for (int r = 0; r < b->h; r++, row += b->stride, line += w * k) {
            uint32_t v = row_word(row, b->stride, b->w);
            if (!v)
                continue;
            if (top < 0)
                top = r;
            bot = r;
            uint8_t *d = line;
            if (k == 1) {
                for (; v; v <<= 1, d++)
                    if (v & 0x80000000u) {
                        *d = ink;
                        dots++;
                    }
                continue;
            }
            for (; v; v <<= 1, d += k)
                if (v & 0x80000000u) {
                    for (int dy = 0; dy < k; dy++)
                        for (int dx = 0; dx < k; dx++)
                            d[dy * w + dx] = ink;
                    dots++;
                }
        }
        if (top >= 0) {
            p->count += dots * (uint32_t)(k * k);
            row_touched(p, (int32_t)(gy + top * k));
            row_touched(p, (int32_t)(gy + bot * k + k - 1));
        }
        return;
    }
    if (gx >= p->cx1 || gy >= p->cy1 || gx + b->w * k <= p->cx0 ||
        gy + b->h * k <= p->cy0)
        return;
    for (int r = 0; r < b->h; r++, row += b->stride) {
        uint32_t v = row_word(row, b->stride, b->w);
        for (int c = 0; v; c++, v <<= 1)
            if (v & 0x80000000u)
                for (int dy = 0; dy < k; dy++)
                    span(p, gy + r * k + dy, gx + c * k, gx + c * k + k - 1);
    }
}

/* The unscaled line of the font being drawn in */
static int32_t line_of(struct vm *vm)
{
    if (vm->d.font >= FIRST_CART_FONT)
        return vm->fonts[vm->d.font - FIRST_CART_FONT].line;
    return vm->d.font == 2 ? TEXT_LINE : 6;
}

/* text() for FONT.WORLD and the cart's fonts, k pixels to a dot */
static int64_t text_wide(struct vm *vm, struct pen *p, const char *s,
                         uint32_t n, int64_t x, int64_t y, int k, int draw,
                         int64_t *widest, int *lines)
{
    uint8_t buf[13];
    const struct cartfont *f = vm->d.font >= FIRST_CART_FONT
        ? &vm->fonts[vm->d.font - FIRST_CART_FONT] : NULL;
    int32_t line = f ? f->line : TEXT_LINE;
    int64_t cx = x;
    if (draw)
        p->per256 = PER16_TEXT * 16;
    *widest = 0;
    *lines = 1;
    for (uint32_t i = 0; i < n;) {
        uint32_t cp = utf8_next(s, n, &i);
        if (cp == '\n') {
            if (cx - x > *widest)
                *widest = cx - x;
            cx = x;
            y += line * k;
            (*lines)++;
            continue;
        }
        if (cp < 0x20)
            cp = ' ';
        struct glyph_box b;
        if (f)
            cart_glyph(f, cp, &b);
        else {
            const uint8_t *g = text_glyph(vm, cp, buf);
            b.adv = g[0];
            b.x = b.y = 0;
            b.w = 8;
            b.h = TEXT_LINE;
            b.stride = 1;
            b.bits = g + 1;
        }
        if (draw && cp != ' ' && b.h)
            draw_glyph(p, &b, cx + b.x * k + p->ox, y + b.y * k + p->oy, k);
        cx += b.adv * k;
    }
    if (cx - x > *widest)
        *widest = cx - x;
    return cx;
}

/* print's and textw's text. Static, because the UI thread's stack is
 * small; there is only ever one engine. */
static char text_buf[1024];

/* print(str), print(str, c), print(str, x, y) or print(str, x, y, c);
 * str arrives as pieces. Returns the x after the last glyph. */
NATIVE(print)
{
    char *buf = text_buf;
    if (!vm_parts(vm, a, argc, "print"))
        return 0;
    uint32_t n = gather(vm, a, argc, buf, sizeof(text_buf));
    int first = 1 + a[0].v, rest = argc - first, k = scale_of(vm);
    int32_t x = vm->d.cur_x, y = vm->d.cur_y;
    uint32_t c = vm->d.pen;
    if (rest == 1 && !colour(vm, a, argc, first, "print", &c))
        return 0;
    if (rest >= 2) {
        if (!whole(vm, a, argc, first, 0, "print", &x) ||
            !whole(vm, a, argc, first + 1, 0, "print", &y) ||
            !colour(vm, a, argc, first + 2, "print", &c))
            return 0;
    }
    struct pen p;
    int64_t widest;
    int lines;
    begin(vm, &p, c, 0);
    int wide = vm->d.font >= 2;
    int32_t tx = x, ty = y;         /* its corner turned, the text upright */
    turn_px(&vm->d, &tx, &ty);
    int64_t end = wide ? text_wide(vm, &p, buf, n, tx, ty, k, 1, &widest,
                                   &lines)
                       : text(&p, buf, n, tx, ty, k, 1, &widest, &lines);
    finish(vm, &p);
    vm->d.cur_x = x;
    vm->d.cur_y = (int32_t)clampc((int64_t)y +
                                  lines * line_of(vm) * k);
    *out = mk(T_INT, (int32_t)clampc(end));
    return 1;
}

NATIVE(textw)
{
    char *buf = text_buf;
    if (!vm_parts(vm, a, argc, "textw"))
        return 0;
    uint32_t n = gather(vm, a, argc, buf, sizeof(text_buf));
    *out = mk(T_INT, text_width(vm, buf, n));
    return 1;
}

/* imgw(i), imgh(i): an image's width and height, a lazy one's too */
static int img_side(struct vm *vm, struct value v, int height,
                    struct value *out)
{
    if (v.tag == T_LAZY) {
        const struct lazyimg *r = MEM(vm, (uint32_t)v.v);
        vm_charge(vm, 2);
        *out = mk(T_INT, height ? r->h : r->w);
        return 1;
    }
    if (v.tag != T_IMG)
        return vm_fail(vm, height ? "imgh" : "imgw",
                       "() needs an image, IMG.name or one img() made", "");
    struct img *im = MEM(vm, (uint32_t)v.v);
    vm_charge(vm, 2);
    *out = mk(T_INT, height ? im->height : im->width);
    return 1;
}

/* The lazy cart image v names, as its T_LAZY or the copy img.load()
 * made; NULL when it names none */
static struct lazyimg *lazy_of(struct vm *vm, struct value v)
{
    for (uint32_t i = 0; i < vm->nlazyimgs; i++) {
        struct lazyimg *r = &vm->lazyimgs[i];
        uint32_t off = (uint32_t)((uint8_t *)r - vm->mem);
        if ((v.tag == T_LAZY && (uint32_t)v.v == off) ||
            (v.tag == T_IMG && r->copy && (uint32_t)v.v == r->copy))
            return r;
    }
    return NULL;
}

/* img.load(IMG.name): a lazy image read from the cart file into a copy on
 * the heap, which IMG.name then is; the copy again when it is loaded, and
 * any image already in memory as it is. Returns the image. */
NATIVE(img_load)
{
    (void)argc;
    struct lazyimg *r = lazy_of(vm, a[0]);
    if (!r) {
        if (a[0].tag != T_IMG)
            return bad(vm, "img.load", "() needs one of the cart's images, "
                       "IMG.name, not a ", a[0]);
        *out = a[0];
        vm_charge(vm, COST_CALL);
        return 1;
    }
    if (!r->copy) {
        uint32_t bytes = (uint32_t)r->w * r->h;
        uint32_t off = gc_alloc(vm, T_IMG, (uint32_t)(sizeof(struct img) -
                                                      sizeof(struct hdr)) +
                                bytes);
        if (!off)
            return 0;
        struct img *im = MEM(vm, off);
        im->width = r->w;
        im->height = r->h;
        struct value v = mk(T_IMG, (int32_t)off);
        if (!vm_root(vm, v))
            return 0;
        int ok = lazy_unpack(vm, &vm->imgc[r->chunk], im->px, bytes);
        vm_unroot(vm);
        if (!ok)
            return 0;
        r->copy = off;
        vm->globals[r->slot] = v;
        /* measured on the ARM rig (lzcost): about 13 cycles a pixel, made
         * and unpacked into */
        vm_charge(vm, COST_CALL + bytes * 3 / 10);
    }
    *out = mk(T_IMG, (int32_t)r->copy);
    return 1;
}

/* img.drop(IMG.name): a lazy image's copy let go, IMG.name lazy again; a
 * copy the cart keeps elsewhere stays until it lets that go too */
NATIVE(img_drop)
{
    (void)argc; (void)out;
    struct lazyimg *r = lazy_of(vm, a[0]);
    if (!r)
        return vm_fail(vm, "img.drop() needs an image listed in cart.json's "
                       "lazy_images, IMG.name", "", "");
    if (r->copy) {
        r->copy = 0;
        vm->globals[r->slot] = mk(T_LAZY, (int32_t)((uint8_t *)r - vm->mem));
    }
    vm_charge(vm, COST_CALL);
    return 1;
}

NATIVE(imgw) { (void)argc; return img_side(vm, a[0], 0, out); }
NATIVE(imgh) { (void)argc; return img_side(vm, a[0], 1, out); }

/* textfit(s, w, [more]): s whole when it fits w pixels in the font set,
 * else cut before a character, spaces at the cut dropped, with more
 * ("..." when left out) after it, so the two fit. */
NATIVE(textfit)
{
    static char more_buf[64];
    if (a[0].tag != T_STR || !is_num(a[1]) ||
        (argc > 2 && a[2].tag != T_STR))
        return vm_fail(vm, "textfit() needs text, a width, and the text "
                       "to end a cut one with", "", "");
    const struct str *s = STR(vm, a[0]);
    int32_t w = floor_int(a[1]);
    uint32_t n = s->len < sizeof(text_buf) ? s->len : sizeof(text_buf) - 1;
    memcpy(text_buf, s->b, n);
    vm_charge(vm, 10 + n / 8);
    if (n == s->len && text_width(vm, text_buf, n) <= w) {
        *out = a[0];
        return 1;
    }
    uint32_t mn = 3;
    if (argc > 2) {
        mn = STR(vm, a[2])->len < sizeof(more_buf) ? STR(vm, a[2])->len
                                                    : sizeof(more_buf) - 1;
        memcpy(more_buf, STR(vm, a[2])->b, mn);
    } else {
        memcpy(more_buf, "...", 3);
    }
    int32_t room = w - text_width(vm, more_buf, mn);
    /* each character's first byte, then the longest run of them that fits:
     * found by halving, a width measured each step */
    static uint16_t starts[sizeof(text_buf)];
    uint32_t count = 0;
    for (uint32_t i = 1; i <= n; i++)
        if (i == n || ((unsigned char)text_buf[i] & 0xc0) != 0x80)
            starts[count++] = (uint16_t)i;
    int32_t lo = -1, hi = (int32_t)count - 1;   /* lo fits, past hi not */
    while (lo < hi) {
        int32_t mid = (lo + hi + 1) / 2;
        if (text_width(vm, text_buf, starts[mid]) <= room)
            lo = mid;
        else
            hi = mid - 1;
    }
    uint32_t cut = lo < 0 ? 0 : starts[lo];
    while (cut > 0 && text_buf[cut - 1] == ' ')
        cut--;
    if (cut + mn >= sizeof(text_buf))
        cut = (uint32_t)sizeof(text_buf) - 1 - mn;
    memcpy(text_buf + cut, more_buf, mn);
    vm_charge(vm, 10 * 11);
    return vm_new_string(vm, (const uint8_t *)text_buf, cut + mn, out);
}

NATIVE(texth)
{
    char *buf = text_buf;
    if (!vm_parts(vm, a, argc, "texth"))
        return 0;
    uint32_t n = gather(vm, a, argc, buf, sizeof(text_buf));
    int lines = 1;
    for (uint32_t i = 0; i < n; i++)
        lines += buf[i] == '\n';
    *out = mk(T_INT, lines * line_of(vm) * scale_of(vm));
    return 1;
}

int32_t draw_text(struct vm *vm, const char *s, uint32_t n, int32_t x,
                  int32_t y, uint32_t c)
{
    struct pen p;
    int64_t widest;
    int lines;
    begin(vm, &p, c, 0);
    int64_t end = vm->d.font >= 2
        ? text_wide(vm, &p, s, n, x, y, scale_of(vm), 1, &widest, &lines)
        : text(&p, s, n, x, y, scale_of(vm), 1, &widest, &lines);
    finish(vm, &p);
    return (int32_t)clampc(end);
}

int32_t text_width(struct vm *vm, const char *s, uint32_t n)
{
    int64_t widest;
    int lines;
    if (vm->d.font >= 2)
        text_wide(vm, NULL, s, n, 0, 0, scale_of(vm), 0, &widest, &lines);
    else
        text(NULL, s, n, 0, 0, scale_of(vm), 0, &widest, &lines);
    return (int32_t)widest;
}

/* Whether the font has every character of s */
int text_has(struct vm *vm, const char *s, uint32_t n)
{
    if (vm->d.font == 2)
        return 1;
    const struct cartfont *f = vm->d.font >= FIRST_CART_FONT
        ? &vm->fonts[vm->d.font - FIRST_CART_FONT] : NULL;
    for (uint32_t i = 0; i < n;) {
        uint32_t cp = utf8_next(s, n, &i);
        if (cp == '\n')
            continue;
        if (f ? !cart_find(f, cp) : cp < 0x20 || cp > 126)
            return 0;
    }
    return 1;
}

int32_t text_line(struct vm *vm)
{
    return line_of(vm) * scale_of(vm);
}

/* font(f) or font(f, size): FONT.CAPS, FONT.WORLD or a cart font; size 1
 * or 2, 1 if left out */
NATIVE(font)
{
    (void)out;
    WHOLE(0, 0, f, "font");
    WHOLE(1, 1, size, "font");
    if (f < 0 || f >= FIRST_CART_FONT + (int32_t)vm->nfonts)
        return vm_fail(vm, "font() needs FONT.CAPS, FONT.WORLD or one of "
                       "the game's fonts", "", "");
    if (size < 1 || size > 2)
        return vm_fail(vm, "font()'s size is 1 or 2", "", "");
    vm->d.font = (uint8_t)f;
    vm->d.font_big = (uint8_t)(size - 1);
    return 1;
}

/* ---- sprites and images ------------------------------------------------ */

/* Copies a w x h rectangle of src, scaled to dw x dh and flipped, to
 * (dx, dy) in camera space, skipping transparent colours. Coordinates are
 * clamped first, so the loops run in 32 bits: the 5G's ARM7 does 64-bit
 * arithmetic slowly. Unscaled -- every sprite, tile and blit -- is a
 * straight copy; scaled steps the source column by the whole and the
 * remainder of w / dw, which gives the same columns as dividing. */
/* An unscaled row: n pixels from s, stepping dir (1, or -1 flipped), to
 * d. Returns the pixels drawn. */
__attribute__((optimize("O2")))
static uint32_t copy_row(uint8_t *d, const uint8_t *s, int32_t n, int32_t dir,
                         const uint16_t *xl)
{
    uint32_t k = 0;
    for (int32_t i = 0; i < n; i++, s += dir) {
        uint32_t c = xl[*s];
        if (!(c & XLAT_CLEAR)) {
            d[i] = (uint8_t)c;
            k++;
        }
    }
    return k;
}

#ifdef ARCADE_ASM
uint32_t copy_row_arm(uint8_t *d, const uint8_t *s, int32_t n,
                      const uint16_t *xl);
#define copy_row_forward(d, s, n, xl) copy_row_arm(d, s, n, xl)
#else
#define copy_row_forward(d, s, n, xl) copy_row(d, s, n, 1, xl)
#endif

/* A scaled row whose source columns are all in the image: col[i] is the
 * column under destination pixel i. Returns the pixels drawn. */
__attribute__((optimize("O2")))
static uint32_t scaled_row(uint8_t *d, const uint8_t *s, const uint16_t *col,
                           int32_t n, const uint16_t *xl)
{
    uint32_t k = 0;
    for (int32_t i = 0; i < n; i++) {
        uint32_t c = xl[s[col[i]]];
        if (!(c & XLAT_CLEAR)) {
            d[i] = (uint8_t)c;
            k++;
        }
    }
    return k;
}

static void copy_rect(struct pen *p, const uint8_t *src, int32_t sw_img,
                      int32_t sh_img, int64_t sx, int64_t sy, int64_t w,
                      int64_t h, int64_t dx, int64_t dy, int64_t dw,
                      int64_t dh, int fx, int fy)
{
    if (w <= 0 || h <= 0 || dw <= 0 || dh <= 0)
        return;
    p->cost += COST_COPY;
    if (w > FAR) w = FAR;
    if (h > FAR) h = FAR;
    if (dw > FAR) dw = FAR;
    if (dh > FAR) dh = FAR;
    sx = clampc(sx);
    sy = clampc(sy);
    dx = clampc(dx) + p->ox;
    dy = clampc(dy) + p->oy;
    int64_t v0 = p->cy0 - dy > 0 ? p->cy0 - dy : 0;
    int64_t v1 = p->cy1 - dy < dh ? p->cy1 - dy : dh;
    int64_t u0 = p->cx0 - dx > 0 ? p->cx0 - dx : 0;
    int64_t u1 = p->cx1 - dx < dw ? p->cx1 - dx : dw;
    const uint16_t *xl = p->xl;
    int32_t stride = p->s.w;

    if (w == dw && h == dh) {
        /* The source column is sx + u, or sx + w - 1 - u flipped; keep it
         * inside the image. */
        if (!fx) {
            if (u0 < -sx) u0 = -sx;
            if (u1 > sw_img - sx) u1 = sw_img - sx;
        } else {
            if (u0 < sx + w - sw_img) u0 = sx + w - sw_img;
            if (u1 > sx + w) u1 = sx + w;
        }
        if (u0 >= u1)
            return;
        /* the pen's own table, every colour itself and none clear, and
         * not flipped: rows copied whole */
        const int plain = xl == p->d->xlat && p->d->xlat_plain && !fx;
        if (v1 > v0)
            p->cost += (uint32_t)((v1 - v0) * (u1 - u0)) *
                       (plain ? PLAIN256 : PER16_COPY * 16) / 256;
        int32_t ua = (int32_t)u0, ub = (int32_t)u1;
        /* A surface copied onto itself: each row read whole before it is
         * written, and the rows taken from the bottom when the copy moves
         * down, so every host draws the source as it was */
        static uint8_t bounce[IMG_MAX];
        const int self = src == p->s.px;
        const int up = self && !fy && dy > sy;
        for (int32_t k = (int32_t)v0; k < (int32_t)v1; k++) {
            int32_t v = up ? (int32_t)v1 - 1 - (k - (int32_t)v0) : k;
            int32_t yy = (int32_t)sy + (fy ? (int32_t)h - 1 - v : v);
            if (yy < 0 || yy >= sh_img)
                continue;
            const uint8_t *srow = src + yy * sw_img;
            if (self) {
                memcpy(bounce, srow, (size_t)sw_img);
                srow = bounce;
            }
            uint8_t *drow = p->s.px + ((int32_t)dy + v) * stride + (int32_t)dx;
            uint32_t n;
            if (fx)
                n = copy_row(drow + ua, srow + (int32_t)sx + (int32_t)w - 1 -
                             ua, ub - ua, -1, xl);
            else if (plain) {
                /* nothing clear and no colour swapped: a plain copy, about
                 * 0.7 cycles a pixel on the rig against 12 */
                memcpy(drow + ua, srow + (int32_t)sx + ua, (size_t)(ub - ua));
                n = (uint32_t)(ub - ua);
            } else
                n = copy_row_forward(drow + ua, srow + (int32_t)sx + ua,
                                     ub - ua, xl);
            if (n) {
                p->count += n;
                area_touched(p, (int32_t)dy + v, (int32_t)dx + ua,
                             (int32_t)dx + ub - 1);
            }
        }
        return;
    }

    if (u1 <= u0 || v1 <= v0)
        return;
    p->cost += (uint32_t)((v1 - v0) * (u1 - u0)) * PER16_SCALED / 16;
    int32_t whole = (int32_t)(w / dw), part = (int32_t)(w % dw);
    int32_t tx0 = (int32_t)(u0 * w / dw), rem0 = (int32_t)(u0 * w % dw);
    int32_t W = (int32_t)w, DW = (int32_t)dw, SX = (int32_t)sx;
    /* The source row steps as the column does: v * h / dh, divided once */
    int32_t hwhole = (int32_t)(h / dh), hpart = (int32_t)(h % dh);
    int32_t ty = (int32_t)(v0 * h / dh), trem = (int32_t)(v0 * h % dh);
    int32_t DH = (int32_t)dh;
    int inside = SX >= 0 && W <= sw_img - SX, stepped = 0;
    for (int32_t v = (int32_t)v0; v < (int32_t)v1; v++) {
        int32_t yy = (int32_t)sy + (fy ? (int32_t)h - 1 - ty : ty);
        ty += hwhole;
        trem += hpart;
        if (trem >= DH) {
            trem -= DH;
            ty++;
        }
        if (yy < 0 || yy >= sh_img)
            continue;
        const uint8_t *srow = src + yy * sw_img;
        uint8_t *drow = p->s.px + ((int32_t)dy + (int32_t)v) * stride
                        + (int32_t)dx;
        int32_t tx = tx0, rem = rem0;
        uint32_t n = 0;
        if (inside) {
            /* Every source column is in the image: the columns, stepped
             * once for the first row drawn, serve every row */
            static uint16_t col[IMG_MAX];
            int32_t cols = (int32_t)(u1 - u0);
            if (!stepped) {
                for (int32_t i = 0; i < cols; i++) {
                    col[i] = (uint16_t)(SX + (fx ? W - 1 - tx : tx));
                    tx += whole;
                    rem += part;
                    if (rem >= DW) {
                        rem -= DW;
                        tx++;
                    }
                }
                stepped = 1;
            }
            n = scaled_row(drow + u0, srow, col, cols, xl);
            if (n) {
                p->count += n;
                area_touched(p, (int32_t)(dy + v), (int32_t)(dx + u0),
                             (int32_t)(dx + u1 - 1));
            }
            continue;
        }
        for (int32_t u = (int32_t)u0; u < (int32_t)u1; u++) {
            int32_t xx = SX + (fx ? W - 1 - tx : tx);
            tx += whole;
            rem += part;
            if (rem >= DW) {
                rem -= DW;
                tx++;
            }
            if (xx < 0 || xx >= sw_img)
                continue;
            uint32_t c = xl[srow[xx]];
            if (c & XLAT_CLEAR)
                continue;
            drow[u] = (uint8_t)c;
            n++;
        }
        if (n) {
            p->count += n;
            area_touched(p, (int32_t)(dy + v), (int32_t)(dx + u0),
                         (int32_t)(dx + u1 - 1));
        }
    }
}

/* An unflipped 8x8 tile, rows stride apart, landing wholly inside the
 * clip at (x, y) in surface coordinates: copy_rect's pixels, without its
 * setup. */
static void tile8(struct pen *p, const uint8_t *src, int32_t stride,
                  int32_t x, int32_t y)
{
    p->cost += 64 * PER16_TILE / 16;
    const uint16_t *xl = p->xl;
    int32_t w = p->s.w;
    uint8_t *d = p->s.px + y * w + x;
    int top = -1, bot = 0;
    uint32_t total = 0;
    for (int r = 0; r < 8; r++, src += stride, d += w) {
        uint32_t n = 0, c;
#define TILE_PX(i) c = xl[src[i]]; \
        if (!(c & XLAT_CLEAR)) { d[i] = (uint8_t)c; n++; }
        TILE_PX(0) TILE_PX(1) TILE_PX(2) TILE_PX(3)
        TILE_PX(4) TILE_PX(5) TILE_PX(6) TILE_PX(7)
#undef TILE_PX
        if (n) {
            total += n;
            if (top < 0)
                top = r;
            bot = r;
        }
    }
    if (top >= 0) {
        p->count += total;
        area_touched(p, y + top, x, x + 7);
        area_touched(p, y + bot, x, x + 7);
    }
}

/* Whether an 8x8 tile at (x, y), in camera space and clamped as copy_rect
 * clamps it, lands wholly inside the clip; if so, where on the surface. */
static int tile_inside(const struct pen *p, int64_t x, int64_t y,
                       int32_t *sx, int32_t *sy)
{
    int64_t dx = clampc(x) + p->ox, dy = clampc(y) + p->oy;
    if (dx < p->cx0 || dy < p->cy0 || dx + 8 > p->cx1 || dy + 8 > p->cy1)
        return 0;
    *sx = (int32_t)dx;
    *sy = (int32_t)dy;
    return 1;
}

/* Where the sprite calls read: the cart's sheet, or the image sheet()
 * chose. Its sprites are its whole 8x8 cells, numbered along the rows. */
struct sheet
{
    const uint8_t *px;
    int32_t w, h;
    int32_t cols, count;
};

static void sheet_of(struct vm *vm, struct sheet *s)
{
    if (vm->d.sheet.tag == T_IMG) {
        struct img *im = MEM(vm, (uint32_t)vm->d.sheet.v);
        s->px = im->px;
        s->w = im->width;
        s->h = im->height;
    } else {
        s->px = MEM(vm, vm->sheet);
        s->w = SHEET_W;
        s->h = SHEET_H;
    }
    s->cols = s->w / 8;
    s->count = s->cols * (s->h / 8);
}

int32_t core_sheet_sprites(struct vm *vm)
{
    struct sheet s;
    sheet_of(vm, &s);
    return s.count;
}

/* Sprite n's top left, n below s->count */
static inline int32_t cell_x(const struct sheet *s, int32_t n)
{
    return n % s->cols * 8;
}

static inline int32_t cell_y(const struct sheet *s, int32_t n)
{
    return n / s->cols * 8;
}

static inline const uint8_t *cell_px(const struct sheet *s, int32_t n)
{
    return s->px + cell_y(s, n) * s->w + cell_x(s, n);
}

/* sheet([i]): the image the sprite calls read from, or the cart's sheet
 * again when there is none or 0 */
NATIVE(sheet)
{
    (void)out;
    if (argc < 1 || (is_num(a[0]) && floor_int(a[0]) == 0)) {
        vm->d.sheet = mk(T_INT, 0);
        return 1;
    }
    if (a[0].tag != T_IMG)
        return bad(vm, "sheet", "() needs an image, or nothing for the cart's "
                   "sheet, not a ", a[0]);
    vm->d.sheet = a[0];
    return 1;
}

/* Whether a w by h block at (x, y), in camera space, misses the clip
 * altogether: nothing to draw, and none of copy_rect's setup to pay for */
static int clipped_out(const struct pen *p, int64_t x, int64_t y, int64_t w,
                       int64_t h)
{
    int64_t dx = clampc(x) + p->ox, dy = clampc(y) + p->oy;
    return dx >= p->cx1 || dy >= p->cy1 || dx + w <= p->cx0 || dy + h <= p->cy0;
}

/* A sprite count in sprites, which may be a fraction: pixels, rounded
 * down. */
static int sprites(struct vm *vm, struct value *a, int argc, int i,
                   const char *fn, int64_t *px)
{
    int64_t v;
    if (!real(vm, a, argc, i, 65536, fn, &v))
        return 0;
    *px = (v * 8) >> 16;
    return 1;
}

NATIVE(spr)
{
    (void)out;
    WHOLE(0, 0, n, "spr");
    WHOLE(1, 0, x, "spr");
    WHOLE(2, 0, y, "spr");
    int64_t w, h;
    if (!sprites(vm, a, argc, 3, "spr", &w) ||
        !sprites(vm, a, argc, 4, "spr", &h))
        return 0;
    struct sheet sh;
    sheet_of(vm, &sh);
    if (n < 0 || n >= sh.count)
        return 1;
    WHOLE(5, 0, flip, "spr");
    int fx = FLIP_X(flip), fy = FLIP_Y(flip);
    turn_box(&vm->d, &x, &y, (int32_t)w, (int32_t)h);
    struct pen p;
    int32_t tx, ty;
    begin(vm, &p, 0, 0);
    p.per256 = 0;                    /* tile8 and copy_rect charge */
    if (w == 8 && h == 8 && !fx && !fy && tile_inside(&p, x, y, &tx, &ty)) {
        p.cost += COST_SPRITE;
        tile8(&p, cell_px(&sh, (int32_t)n), sh.w, tx, ty);
    } else if (!clipped_out(&p, x, y, w, h))
        copy_rect(&p, sh.px, sh.w, sh.h, cell_x(&sh, (int32_t)n),
                  cell_y(&sh, (int32_t)n), w, h, x, y, w, h, fx, fy);
    finish(vm, &p);
    return 1;
}

NATIVE(sspr)
{
    (void)out;
    WHOLE(0, 0, sx, "sspr");
    WHOLE(1, 0, sy, "sspr");
    WHOLE(2, 8, sw, "sspr");
    WHOLE(3, 8, sh, "sspr");
    WHOLE(4, 0, dx, "sspr");
    WHOLE(5, 0, dy, "sspr");
    WHOLE(6, sw, dw, "sspr");
    WHOLE(7, sh, dh, "sspr");
    WHOLE(8, 0, flip, "sspr");
    struct sheet src;
    sheet_of(vm, &src);
    turn_box(&vm->d, &dx, &dy, dw, dh);
    struct pen p;
    begin(vm, &p, 0, 0);
    p.per256 = 0;                    /* copy_rect charges what it reads */
    copy_rect(&p, src.px, src.w, src.h, sx, sy, sw, sh,
              dx, dy, dw, dh, FLIP_X(flip), FLIP_Y(flip));
    finish(vm, &p);
    return 1;
}

NATIVE(sget)
{
    WHOLE(0, 0, x, "sget");
    WHOLE(1, 0, y, "sget");
    struct sheet sh;
    sheet_of(vm, &sh);
    *out = mk(T_INT, x >= 0 && x < sh.w && y >= 0 && y < sh.h
              ? sh.px[y * sh.w + x] : 0);
    return 1;
}

NATIVE(sset)
{
    (void)out;
    WHOLE(0, 0, x, "sset");
    WHOLE(1, 0, y, "sset");
    WHOLE(2, vm->d.pen, c, "sset");
    struct sheet sh;
    sheet_of(vm, &sh);
    if (x >= 0 && x < sh.w && y >= 0 && y < sh.h)
        ((uint8_t *)sh.px)[y * sh.w + x] = (uint8_t)c;
    return 1;
}

/* fget(n) all eight flags; fget(n, f) flag f as 1 or 0. */
NATIVE(fget)
{
    WHOLE(0, 0, n, "fget");
    uint8_t *flags = MEM(vm, vm->flags);
    uint8_t v = (uint32_t)n < 256 ? flags[n] : 0;
    if (argc < 2) {
        *out = mk(T_INT, v);
        return 1;
    }
    WHOLE(1, 0, f, "fget");
    *out = mk(T_INT, (uint32_t)f < 8 ? (v >> f) & 1 : 0);
    return 1;
}

/* fset(n, v) all eight flags; fset(n, v, f) flag f. */
NATIVE(fset)
{
    (void)out;
    WHOLE(0, 0, n, "fset");
    uint8_t *flags = MEM(vm, vm->flags);
    if ((uint32_t)n >= 256)
        return 1;
    if (argc < 3) {
        WHOLE(1, 0, v, "fset");
        flags[n] = (uint8_t)v;
        return 1;
    }
    WHOLE(2, 0, f, "fset");
    if ((uint32_t)f < 8) {
        if (vm_truthy(vm, a[1]))
            flags[n] |= (uint8_t)(1 << f);
        else
            flags[n] &= (uint8_t)~(1 << f);
    }
    return 1;
}

NATIVE(img)
{
    WHOLE(0, 0, w, "img");
    WHOLE(1, 0, h, "img");
    if (w < 1 || h < 1 || w > IMG_MAX || h > IMG_MAX)
        return vm_fail(vm, "img() needs a width and height from 1 to 4096",
                       "", "");
    uint32_t off = gc_alloc(vm, T_IMG, (uint32_t)(sizeof(struct img) -
                                                  sizeof(struct hdr)) +
                            (uint32_t)w * (uint32_t)h);
    if (!off)
        return 0;
    struct img *im = MEM(vm, off);
    im->width = (uint16_t)w;
    im->height = (uint16_t)h;
    *out = mk(T_IMG, (int32_t)off);
    vm_charge(vm, COST_CALL + (uint32_t)(w * h) / BYTES_PER_UNIT);
    return 1;
}

/* blit(i, x, y, [flip]): all of image i, or of the screen or a border. */
NATIVE(blit)
{
    (void)out;
    struct surf s;
    if (!surf_of(vm, a[0], &s, "blit"))
        return 0;
    WHOLE(1, 0, x, "blit");
    WHOLE(2, 0, y, "blit");
    WHOLE(3, 0, flip, "blit");
    turn_box(&vm->d, &x, &y, s.w, s.h);
    struct pen p;
    begin(vm, &p, 0, 0);
    p.per256 = 0;                    /* copy_rect charges what it reads */
    copy_rect(&p, s.px, s.w, s.h, 0, 0, s.w, s.h, x, y, s.w, s.h,
              FLIP_X(flip), FLIP_Y(flip));
    finish(vm, &p);
    return 1;
}

/* blitpart(i, sx, sy, w, h, x, y, [flip]) */
NATIVE(blitpart)
{
    (void)out;
    struct surf s;
    if (!surf_of(vm, a[0], &s, "blitpart"))
        return 0;
    WHOLE(1, 0, sx, "blitpart");
    WHOLE(2, 0, sy, "blitpart");
    WHOLE(3, 0, w, "blitpart");
    WHOLE(4, 0, h, "blitpart");
    WHOLE(5, 0, x, "blitpart");
    WHOLE(6, 0, y, "blitpart");
    WHOLE(7, 0, flip, "blitpart");
    turn_box(&vm->d, &x, &y, w, h);
    struct pen p;
    begin(vm, &p, 0, 0);
    p.per256 = 0;                    /* copy_rect charges what it reads */
    copy_rect(&p, s.px, s.w, s.h, sx, sy, w, h, x, y, w, h,
              FLIP_X(flip), FLIP_Y(flip));
    finish(vm, &p);
    return 1;
}

/* ---- turned ------------------------------------------------------------- */

/* A source rectangle, sw by sh from sx0, sy0 in rows stride apart, and how
 * a destination row steps through it */
/* turned_row_arm() reads these by place: src, stride, sw, sh, du and dv
 * the first six words, xl the eighth */
struct turn
{
    const uint8_t *src;
    int32_t stride;
    uint32_t sw, sh;
    int32_t du, dv;
    int flip;
    const uint16_t *xl;
};

#ifdef ARCADE_ASM
void turned_row_arm(uint8_t *d, int32_t n, int32_t u, int32_t v,
                    const struct turn *t);
#endif

/* One destination row, n pixels from d, its first source point u, v in
 * 16.16 from the rectangle's corner; a pixel only where that is inside */
__attribute__((optimize("O2")))
static void turned_row(uint8_t *d, int32_t n, int32_t u, int32_t v,
                       const struct turn *t)
{
    const uint8_t *src = t->src;
    const int32_t du = t->du, dv = t->dv, stride = t->stride;
    const uint32_t sw = t->sw, sh = t->sh, last = t->sw - 1;
    const uint16_t *xl = t->xl;
    for (int32_t i = 0; i < n; i++, u += du, v += dv) {
        uint32_t x = (uint32_t)(u >> 16), y = (uint32_t)(v >> 16);
        if (x >= sw || y >= sh)
            continue;
        uint32_t c = xl[src[y * stride + (t->flip ? last - x : x)]];
        if (!(c & XLAT_CLEAR))
            d[i] = (uint8_t)c;
    }
}

/* The rectangle drawn about centre cx, cy (16.16, the camera's space),
 * turned by angle (turns, anticlockwise on the screen as sin goes) and
 * scaled. Each destination pixel's centre is mapped back to the source:
 * along a row the source point steps by the inverse rotation over the
 * scale, so a row is a start and a step. */
static void turned(struct pen *p, struct turn *t, int64_t cx, int64_t cy,
                   int32_t angle, int32_t scale)
{
    int32_t c = core_sine((uint32_t)angle + 0x4000);
    int32_t s = core_sine((uint32_t)angle);
    int32_t inv = (int32_t)(((int64_t)1 << 32) / scale);   /* 1 / scale */
    t->du = (int32_t)(((int64_t)c * inv) >> 16);
    t->dv = (int32_t)(((int64_t)s * inv) >> 16);
    /* half the turned rectangle's extent, in screen pixels */
    int64_t ac = c < 0 ? -c : c, as = s < 0 ? -s : s;
    int64_t ex = ((ac * t->sw + as * t->sh) * scale >> 33) + 2;
    int64_t ey = ((as * t->sw + ac * t->sh) * scale >> 33) + 2;
    cx += p->ox << 16;
    cy += p->oy << 16;
    int64_t x0 = (cx >> 16) - ex, x1 = (cx >> 16) + ex;
    int64_t y0 = (cy >> 16) - ey, y1 = (cy >> 16) + ey;
    if (x0 < p->cx0) x0 = p->cx0;
    if (x1 > p->cx1 - 1) x1 = p->cx1 - 1;
    if (y0 < p->cy0) y0 = p->cy0;
    if (y1 > p->cy1 - 1) y1 = p->cy1 - 1;
    if (x0 > x1 || y0 > y1)
        return;
    int32_t n = (int32_t)(x1 - x0 + 1);
    for (int64_t y = y0; y <= y1; y++) {
        /* the first pixel's centre from the drawing's centre */
        int64_t dx = (x0 << 16) + 0x8000 - cx, dy = (y << 16) + 0x8000 - cy;
        int64_t u = (dx * c - dy * s) >> 16, v = (dx * s + dy * c) >> 16;
        u = ((u * inv) >> 16) + ((int64_t)t->sw << 15);
        v = ((v * inv) >> 16) + ((int64_t)t->sh << 15);
#ifdef ARCADE_ASM
        if (!t->flip)
            turned_row_arm(p->s.px + y * p->s.w + x0, n, (int32_t)u,
                           (int32_t)v, t);
        else
#endif
            turned_row(p->s.px + y * p->s.w + x0, n, (int32_t)u, (int32_t)v,
                       t);
    }
    row_touched(p, (int32_t)y0);
    row_touched(p, (int32_t)y1);
    p->cost += (uint32_t)((y1 - y0 + 1) * n) * PER16_FLOOR / 16 +
               (uint32_t)(y1 - y0 + 1) * COST_ROW * 4;
}

/* The scale argument at i, 1 by default, at least 1/64 */
static int scale_arg(struct vm *vm, struct value *a, int argc, int i,
                     const char *fn, int32_t *out)
{
    int64_t k;
    *out = 0x10000;
    if (!real(vm, a, argc, i, 0x10000, fn, &k))
        return 0;
    if (k < 0x400 || k > (64 << 16))
        return vm_fail(vm, fn, "() takes a scale from 1/64 to 64", "");
    *out = (int32_t)k;
    return 1;
}

/* rspr(n, x, y, angle, [scale], [w], [h], [flip]): sprite n, w by h
 * sprites as spr takes them, turned about its centre at x, y. Flipped up
 * and down is flipped across and turned half round, so the rows need only
 * the one mirror. */
NATIVE(rspr)
{
    (void)out;
    WHOLE(0, 0, n, "rspr");
    WHOLE(5, 1, w, "rspr");
    WHOLE(6, 1, h, "rspr");
    WHOLE(7, 0, flip, "rspr");
    int64_t x, y, angle;
    int32_t scale;
    if (!real(vm, a, argc, 1, 0, "rspr", &x) ||
        !real(vm, a, argc, 2, 0, "rspr", &y) ||
        !real(vm, a, argc, 3, 0, "rspr", &angle) ||
        !scale_arg(vm, a, argc, 4, "rspr", &scale))
        return 0;
    struct sheet sh;
    sheet_of(vm, &sh);
    if (n < 0 || n >= sh.count || w < 1 || h < 1)
        return 1;
    int32_t sx = cell_x(&sh, (int32_t)n), sy = cell_y(&sh, (int32_t)n);
    if (w > (sh.w - sx) / 8) w = (sh.w - sx) / 8;
    if (h > (sh.h - sy) / 8) h = (sh.h - sy) / 8;
    struct pen p;
    begin(vm, &p, 0, 0);
    p.per256 = 0;
    struct turn t;
    t.src = sh.px + sy * sh.w + sx;
    t.stride = sh.w;
    t.sw = (uint32_t)w * 8;
    t.sh = (uint32_t)h * 8;
    t.flip = FLIP_X(flip) ^ FLIP_Y(flip);
    t.xl = p.xl;
    if (FLIP_Y(flip))
        angle += 0x8000;
    turn_turned(&vm->d, &x, &y, &angle, &scale);
    turned(&p, &t, x, y, wrap32(angle), scale);
    finish(vm, &p);
    return 1;
}

/* rblit(img, x, y, angle, [scale]): an image turned about its centre */
NATIVE(rblit)
{
    (void)out;
    struct surf s;
    if (!surf_of(vm, a[0], &s, "rblit"))
        return 0;
    int64_t x, y, angle;
    int32_t scale;
    if (!real(vm, a, argc, 1, 0, "rblit", &x) ||
        !real(vm, a, argc, 2, 0, "rblit", &y) ||
        !real(vm, a, argc, 3, 0, "rblit", &angle) ||
        !scale_arg(vm, a, argc, 4, "rblit", &scale))
        return 0;
    struct pen p;
    begin(vm, &p, 0, 0);
    p.per256 = 0;
    struct turn t;
    t.src = s.px;
    t.stride = s.w;
    t.sw = (uint32_t)s.w;
    t.sh = (uint32_t)s.h;
    t.flip = 0;
    t.xl = p.xl;
    turn_turned(&vm->d, &x, &y, &angle, &scale);
    turned(&p, &t, x, y, wrap32(angle), scale);
    finish(vm, &p);
    return 1;
}

/* ---- the map ------------------------------------------------------------ */

/* map(celx, cely, sx, sy, celw, celh, [layers]): each cell's sprite, if
 * not 0 and its flags include all of layers. */
/* map(celx, cely, sx, sy, celw, celh, [layers], [cells, w]): the active
 * map's cells, or with cells -- a Uint8Array, w cells a row, a map
 * loadmap() unpacked into it or any other -- those: a second layer drawn
 * under or over the first, a backdrop, a level made in code */
NATIVE(map)
{
    (void)out;
    uint8_t *map = MEM(vm, vm->map), *flags = MEM(vm, vm->flags);
    int32_t mw = vm->map_w, mh = vm->map_h;
    if (argc > 7 && a[7].tag != T_NULL) {
        WHOLE(8, 0, w, "map");
        if (a[7].tag != T_U8 || w <= 0)
            return vm_fail(vm, "map() draws cells from a Uint8Array, w a row",
                           "", "");
        map = U8A(vm, a[7])->d;
        mw = w;
        mh = (int32_t)(U8A(vm, a[7])->len / (uint32_t)w);
    }
    WHOLE(0, 0, cx, "map");
    WHOLE(1, 0, cy, "map");
    WHOLE(2, 0, sx, "map");
    WHOLE(3, 0, sy, "map");
    WHOLE(4, mw, cw, "map");
    WHOLE(5, mh, ch, "map");
    WHOLE(6, 0, layers, "map");
    struct pen p;
    begin(vm, &p, 0, 0);
    p.per256 = 0;                    /* tile8 and copy_rect charge */
    int64_t j0 = cy < 0 ? -(int64_t)cy : 0, i0 = cx < 0 ? -(int64_t)cx : 0;
    int64_t j1 = (int64_t)mh - cy < ch ? (int64_t)mh - cy : ch;
    int64_t i1 = (int64_t)mw - cx < cw ? (int64_t)mw - cx : cw;
    if (j0 >= j1 || i0 >= i1) {
        finish(vm, &p);
        return 1;
    }
    const uint8_t *sheet = MEM(vm, vm->sheet);
    /* Near the screen, in 32 bits: then a tile's place needs no clamp */
    if (sx >= -(FAR - 1024) && sx <= FAR - 1024 && sy >= -(FAR - 1024) &&
        sy <= FAR - 1024 && p.ox >= -(1 << 29) && p.ox <= (1 << 29) &&
        p.oy >= -(1 << 29) && p.oy <= (1 << 29)) {
        int32_t ox = (int32_t)p.ox, oy = (int32_t)p.oy;
        for (int32_t j = (int32_t)j0; j < (int32_t)j1; j++) {
            int32_t py = sy + j * 8 + oy;
            if (py + 8 <= p.cy0 || py >= p.cy1)
                continue;
            int whole_rows = py >= p.cy0 && py + 8 <= p.cy1;
            const uint8_t *cells = map + (cy + j) * mw + cx;
            for (int32_t i = (int32_t)i0; i < (int32_t)i1; i++) {
                int32_t px = sx + i * 8 + ox;
                if (px + 8 <= p.cx0 || px >= p.cx1)
                    continue;
                uint8_t n = cells[i];
                if (!n || (layers && (flags[n] & layers) != layers))
                    continue;
                p.cost += COST_TILE;
                if (whole_rows && px >= p.cx0 && px + 8 <= p.cx1)
                    tile8(&p, sheet + (n >> 4) * 8 * SHEET_W + (n & 15) * 8,
                          SHEET_W, px, py);
                else
                    copy_rect(&p, sheet, SHEET_W, SHEET_H, (n & 15) * 8,
                              (n >> 4) * 8, 8, 8, px - ox, py - oy, 8, 8, 0,
                              0);
            }
        }
        finish(vm, &p);
        return 1;
    }
    for (int64_t j = j0; j < j1; j++) {
        int64_t py = (int64_t)sy + j * 8 + p.oy;
        if (py + 8 <= p.cy0 || py >= p.cy1)
            continue;
        for (int64_t i = i0; i < i1; i++) {
            int64_t px = (int64_t)sx + i * 8 + p.ox;
            if (px + 8 <= p.cx0 || px >= p.cx1)
                continue;
            uint8_t n = map[(cy + j) * mw + cx + i];
            if (!n || (layers && (flags[n] & layers) != layers))
                continue;
            p.cost += COST_TILE;
            int32_t tx, ty;
            if (tile_inside(&p, px - p.ox, py - p.oy, &tx, &ty)) {
                tile8(&p, MEM(vm, vm->sheet) + (n >> 4) * 8 * SHEET_W +
                      (n & 15) * 8, SHEET_W, tx, ty);
                continue;
            }
            copy_rect(&p, MEM(vm, vm->sheet), SHEET_W, SHEET_H,
                      (n & 15) * 8, (n >> 4) * 8, 8, 8, px - p.ox, py - p.oy,
                      8, 8, 0, 0);
        }
    }
    finish(vm, &p);
    return 1;
}

NATIVE(mget)
{
    WHOLE(0, 0, x, "mget");
    WHOLE(1, 0, y, "mget");
    uint8_t *map = MEM(vm, vm->map);
    *out = mk(T_INT, x >= 0 && y >= 0 && x < vm->map_w && y < vm->map_h
              ? map[y * vm->map_w + x] : 0);
    return 1;
}

NATIVE(mset)
{
    (void)out;
    WHOLE(0, 0, x, "mset");
    WHOLE(1, 0, y, "mset");
    WHOLE(2, 0, v, "mset");
    uint8_t *map = MEM(vm, vm->map);
    if (x >= 0 && y >= 0 && x < vm->map_w && y < vm->map_h)
        map[y * vm->map_w + x] = (uint8_t)v;
    return 1;
}

NATIVE(mapw)
{
    (void)vm; (void)a; (void)argc;
    *out = mk(T_INT, vm->map_w);
    return 1;
}

/* loadmap(MAP.name, [cells]): the active map, as it was built; or with
 * cells, a Uint8Array, the map's cells unpacked into it instead, the active
 * map untouched. The map's width. */
NATIVE(loadmap)
{
    WHOLE(0, 0, n, "loadmap");
    if (n < 0 || (uint32_t)n >= vm->nmaps)
        return vm_fail(vm, "loadmap() needs one of the cart's maps, MAP.name",
                       "", "");
    const struct mapinfo *m = &vm->mapdir[n];
    uint32_t cells = (uint32_t)m->w * m->h;
    if (argc > 1) {
        if (a[1].tag != T_U8 || U8A(vm, a[1])->len < cells)
            return vm_fail(vm, "loadmap() unpacks into a Uint8Array of the "
                           "map's width times its height", "", "");
        if (!map_cells(vm, m, U8A(vm, a[1])->d, cells))
            return 0;
    } else if (!use_map(vm, (uint32_t)n)) {
        return 0;
    }
    vm_charge(vm, COST_CALL + cells / BYTES_PER_UNIT);
    *out = mk(T_INT, m->w);
    return 1;
}

static const uint8_t *map_object(struct vm *vm, struct value *a, int argc,
                                 const char *fn)
{
    int32_t i;
    if (!whole(vm, a, argc, 0, 0, fn, &i))
        return NULL;
    if (vm->active_map < 0 || i < 0 ||
        (uint32_t)i >= vm->mapdir[vm->active_map].nobj) {
        vm_fail(vm, fn, "() needs an object of the active map, 0 to "
                "mapobj()'s count less one", "");
        return NULL;
    }
    return vm->mapdir[vm->active_map].obj[i];
}

/* mapobj(out, max): kind, x, y, w, h of up to max objects into out, an
 * Int32Array or array of five a time; returns how many the map has. */
NATIVE(mapobj)
{
    WHOLE(1, 0, max, "mapobj");
    if (a[0].tag != T_I32 && a[0].tag != T_ARR)
        return bad(vm, "mapobj", "() writes to an Int32Array or an array, "
                   "not a ", a[0]);
    uint32_t len = a[0].tag == T_I32 ? I32(vm, a[0])->len : ARR(vm, a[0])->len;
    uint32_t n = vm->active_map < 0 ? 0 : vm->mapdir[vm->active_map].nobj;
    uint32_t k = max < 0 ? 0 : (uint32_t)max < n ? (uint32_t)max : n;
    if (k * 5 > len)
        return vm_fail(vm, "mapobj() needs room for five numbers an object",
                       "", "");
    for (uint32_t i = 0; i < k; i++) {
        const uint8_t *o = vm->mapdir[vm->active_map].obj[i];
        int32_t v[5] = { o[0], (int16_t)le16(o + 1), (int16_t)le16(o + 3),
                         (int16_t)le16(o + 5), (int16_t)le16(o + 7) };
        for (int j = 0; j < 5; j++) {
            if (a[0].tag == T_I32)
                I32(vm, a[0])->d[i * 5 + j] = v[j];
            else
                BUF(vm, ARR(vm, a[0])->buf)->v[i * 5 + j] = mk(T_INT, v[j]);
        }
    }
    vm_charge(vm, k * 2);
    *out = mk(T_INT, (int32_t)n);
    return 1;
}

/* mapprop(i, name): object i's property, an integer or a string, or null */
NATIVE(mapprop)
{
    const uint8_t *o = map_object(vm, a, argc, "mapprop");
    if (!o)
        return 0;
    if (a[1].tag != T_STR)
        return bad(vm, "mapprop", "() needs a property name, not a ", a[1]);
    const struct str *want = STR(vm, a[1]);
    uint32_t props = o[9];
    *out = mk(T_NULL, 0);
    for (const uint8_t *p = o + 10; props; props--) {
        const struct str *key = MEM(vm, vm->strings[le16(p)]);
        uint32_t type = p[2];
        if (key->len == want->len && !memcmp(key->b, want->b, key->len)) {
            *out = type ? mk(T_STR, (int32_t)vm->strings[le16(p + 3)])
                        : mk(T_INT, (int32_t)rd_le32(p + 3));
            break;
        }
        p += 3 + (type ? 2 : 4);
    }
    vm_charge(vm, 4 + o[9]);
    return 1;
}

NATIVE(maph)
{
    (void)vm; (void)a; (void)argc;
    *out = mk(T_INT, vm->map_h);
    return 1;
}

/* ---- state -------------------------------------------------------------- */

static void full_clip(struct vm *vm)
{
    struct surf s;
    surf_of(vm, vm->d.target, &s, "clip");
    vm->d.clip_x0 = vm->d.clip_y0 = 0;
    vm->d.clip_x1 = s.w;
    vm->d.clip_y1 = s.h;
}

/* camera([x, y], [turn], [px], [py], [zoom]): what is drawn at x, y lands
 * at the target's corner, and with turn (in turns, clockwise) and zoom
 * the picture is turned and grown about px, py on the target, its middle
 * when left out */
NATIVE(camera)
{
    (void)out;
    WHOLE(0, 0, x, "camera");
    WHOLE(1, 0, y, "camera");
    struct draw *d = &vm->d;
    d->cam_x = x;
    d->cam_y = y;
    d->cam_turn = argc > 2 && is_num(a[2])
                  ? (int32_t)(wrap32(scaled(a[2])) & 0xffff) : 0;
    struct surf s;
    surf_of(vm, d->target, &s, "camera");
    d->cam_px = argc > 3 && is_num(a[3]) ? wrap32(scaled(a[3])) : s.w << 15;
    d->cam_py = argc > 4 && is_num(a[4]) ? wrap32(scaled(a[4])) : s.h << 15;
    int64_t zoom;
    if (!real(vm, a, argc, 5, 0x10000, "camera", &zoom))
        return 0;
    if (zoom < 0x1000 || zoom > (16 << 16))
        return vm_fail(vm, "camera", "() takes a zoom from 1/16 to 16",
                       "");
    d->cam_zoom = (int32_t)zoom;
    d->cam_moved = d->cam_turn || zoom != 0x10000;
    d->cam_cos = (int32_t)(((int64_t)core_sine((uint32_t)d->cam_turn +
                                               0x4000) * zoom) >> 16);
    d->cam_sin = (int32_t)(((int64_t)core_sine((uint32_t)d->cam_turn) *
                            zoom) >> 16);
    return 1;
}

/* clip(x0, y0, x1, y1), two corners in target pixels, both inside, as
 * rect takes them; clip() the whole target. */
NATIVE(clip)
{
    (void)out;
    if (argc == 0) {
        full_clip(vm);
        return 1;
    }
    WHOLE(0, 0, x0, "clip");
    WHOLE(1, 0, y0, "clip");
    WHOLE(2, 0, x1, "clip");
    WHOLE(3, 0, y1, "clip");
    vm->d.clip_x0 = x0;
    vm->d.clip_y0 = y0;
    vm->d.clip_x1 = (int32_t)clampc((int64_t)x1 + 1);
    vm->d.clip_y1 = (int32_t)clampc((int64_t)y1 + 1);
    return 1;
}

NATIVE(color)
{
    (void)out;
    WHOLE(0, 6, c, "color");
    vm->d.pen = (uint16_t)c;
    return 1;
}

NATIVE(cursor)
{
    (void)out;
    WHOLE(0, 0, x, "cursor");
    WHOLE(1, 0, y, "cursor");
    vm->d.cur_x = x;
    vm->d.cur_y = y;
    if (argc > 2) {
        uint32_t c;
        return colour(vm, a, argc, 2, "cursor", &c);
    }
    return 1;
}

NATIVE(fillp)
{
    (void)out;
    WHOLE(0, 0, f, "fillp");
    vm->d.fill = (uint16_t)f;
    return 1;
}

/* pal(): everything back. pal(c0, c1): draw c0 as c1. pal(c0, c1, 1):
 * show index c0 as entry c1. */
NATIVE(pal)
{
    (void)out;
    if (argc == 0) {
        reset_palettes(&vm->d);
        reshow(&vm->d);
        return 1;
    }
    WHOLE(0, 0, c0, "pal");
    WHOLE(1, c0, c1, "pal");
    WHOLE(2, 0, which, "pal");
    if (which == 1) {
        vm->d.spal[c0 & 0xff] = (uint8_t)c1;
        reshow(&vm->d);
    } else {
        vm->d.dpal[c0 & 0xff] = (uint8_t)c1;
        vm->d.xlat_stale = 1;
    }
    return 1;
}

NATIVE(loadpal)
{
    (void)out;
    WHOLE(0, 0, n, "loadpal");
    uint32_t k;
    const uint8_t *rgb = n < 0 ? NULL : palette_at(vm, (uint32_t)n, &k);
    if (!rgb)
        return vm_fail(vm, "loadpal() needs one of the cart's palettes, "
                       "PAL.name", "", "");
    use_palette(vm, rgb, k);
    vm_charge(vm, COST_CALL + k / 4);
    return 1;
}

NATIVE(palt)
{
    (void)out;
    vm->d.xlat_stale = 1;
    if (argc == 0) {
        memset(vm->d.transparent, 0, sizeof(vm->d.transparent));
        vm->d.transparent[0] = 1;
        return 1;
    }
    WHOLE(0, 0, c, "palt");
    uint8_t bit = (uint8_t)(1 << (c & 7));
    if (argc > 1 && vm_truthy(vm, a[1]))
        vm->d.transparent[(c & 0xff) >> 3] |= bit;
    else
        vm->d.transparent[(c & 0xff) >> 3] &= (uint8_t)~bit;
    return 1;
}

/* setpal(i, 0xRRGGBB): what entry i looks like. */
NATIVE(setpal)
{
    (void)out;
    WHOLE(0, 0, i, "setpal");
    WHOLE(1, 0, rgb, "setpal");
    vm->d.rgb[i & 0xff] = (uint32_t)rgb & 0xffffff;
    reshow(&vm->d);
    return 1;
}

NATIVE(getpal)
{
    WHOLE(0, 0, i, "getpal");
    *out = mk(T_INT, (int32_t)vm->d.rgb[i & 0xff]);
    return 1;
}

NATIVE(target)
{
    (void)out; (void)argc;
    struct surf s;
    if (!surf_of(vm, a[0], &s, "target"))
        return 0;
    vm->d.target = is_num(a[0]) ? mk(T_INT, floor_int(a[0])) : a[0];
    full_clip(vm);
    return 1;
}

/* ---- memory ------------------------------------------------------------- */

NATIVE(mem)
{
    WHOLE(0, 0, r, "mem");
    uint32_t at = r == 0 ? vm->screen : r == 1 ? vm->sheet
                : r == 2 ? vm->map : r == 3 ? vm->flags : 0;
    if (!at)
        return vm_fail(vm, "mem() needs MEM.SCREEN, MEM.SHEET, MEM.MAP or "
                       "MEM.FLAGS", "", "");
    *out = mk(T_U8, (int32_t)(at - sizeof(struct u8a)));
    return 1;
}

/* The length and element size of an array copy() and fill() work on */
static int span_of(struct vm *vm, struct value v, const char *fn,
                   uint32_t *len, uint32_t *size)
{
    *len = *size = 0;
    switch (v.tag) {
    case T_U8:  *len = U8A(vm, v)->len; *size = 1; return 1;
    case T_I32: case T_FXA: *len = I32(vm, v)->len; *size = 4; return 1;
    case T_ARR: *len = ARR(vm, v)->len; *size = sizeof(struct value);
                return 1;
    }
    return bad(vm, fn, "() needs an array or a typed array, not a ", v);
}

static void *elems(struct vm *vm, struct value v)
{
    return v.tag == T_U8 ? (void *)U8A(vm, v)->d
         : v.tag == T_I32 || v.tag == T_FXA ? (void *)I32(vm, v)->d
         : (void *)BUF(vm, ARR(vm, v)->buf)->v;
}

/* Bytes moved go at the speed of memory. */
static void moved(struct vm *vm, struct value dst, uint32_t at, uint32_t n,
                  uint32_t size)
{
    if (n && dst.tag == T_U8 && IS_SCREEN(vm, dst))
        screen_written(vm, (int32_t)(at / SCREEN_W),
                       (int32_t)((at + n - 1) / SCREEN_W));
    vm_charge(vm, n * size / BYTES_PER_UNIT);
}

/* copy(dst, di, src, si, n): n elements from src at si to dst at di, as
 * memmove, between two arrays of one kind. */
NATIVE(copy)
{
    (void)out;
    uint32_t dl, ds, sl, ss;
    if (!span_of(vm, a[0], "copy", &dl, &ds) ||
        !span_of(vm, a[2], "copy", &sl, &ss))
        return 0;
    if (a[0].tag != a[2].tag)
        return vm_fail(vm, "copy() needs two arrays of one kind", "", "");
    WHOLE(1, 0, di, "copy");
    WHOLE(3, 0, si, "copy");
    WHOLE(4, 0, n, "copy");
    if (di < 0 || si < 0 || n < 0 || (uint32_t)di > dl ||
        (uint32_t)n > dl - (uint32_t)di || (uint32_t)si > sl ||
        (uint32_t)n > sl - (uint32_t)si)
        return vm_fail(vm, "copy() runs past the end of an array", "", "");
    memmove((char *)elems(vm, a[0]) + (uint32_t)di * ds,
            (char *)elems(vm, a[2]) + (uint32_t)si * ss, (uint32_t)n * ds);
    moved(vm, a[0], (uint32_t)di, (uint32_t)n, ds);
    return 1;
}

/* fill(a, v, [i], [n]): n elements from i set to v; to the end by default */
NATIVE(fill)
{
    (void)out;
    uint32_t len, size;
    if (!span_of(vm, a[0], "fill", &len, &size))
        return 0;
    WHOLE(2, 0, i, "fill");
    if (i < 0 || (uint32_t)i > len)
        return vm_fail(vm, "fill() starts past the end of an array", "", "");
    WHOLE(3, (int32_t)(len - (uint32_t)i), n, "fill");
    if (n < 0 || (uint32_t)n > len - (uint32_t)i)
        return vm_fail(vm, "fill() runs past the end of an array", "", "");
    struct value v = a[1];
    if (a[0].tag != T_ARR && !is_num(v))
        return bad(vm, "fill", "() puts numbers in a typed array, not a ", v);
    if (a[0].tag == T_U8)
        memset(U8A(vm, a[0])->d + i, (uint8_t)to_int(v), (uint32_t)n);
    else if (a[0].tag == T_I32 || a[0].tag == T_FXA) {
        int32_t *d = I32(vm, a[0])->d + i,
                x = a[0].tag == T_FXA ? to_fix(v) : to_int(v);
        for (int32_t k = 0; k < n; k++)
            d[k] = x;
    } else {
        struct value *d = BUF(vm, ARR(vm, a[0])->buf)->v + i;
        for (int32_t k = 0; k < n; k++)
            d[k] = v;
    }
    moved(vm, a[0], (uint32_t)i, (uint32_t)n, size);
    return 1;
}

/* ---- for the systems ---------------------------------------------------- */

/* always inlined: -Os would call it, and the particle loops call it
 * several times a particle */
static inline __attribute__((always_inline))
int32_t num_or_0(struct value v)
{
    return is_num(v) ? floor_int(v) : 0;
}

/* 0 when v is not an array of numbers this can read */
int column_of(struct vm *vm, struct value v, struct column *c)
{
    c->tag = v.tag;
    c->k = 0;
    switch (v.tag) {
    case T_ARR:
        c->p = BUF(vm, ARR(vm, v)->buf)->v;
        c->len = ARR(vm, v)->len;
        return 1;
    case T_I32:
    case T_FXA:
        c->p = I32(vm, v)->d;
        c->len = I32(vm, v)->len;
        return 1;
    case T_U8:
        c->p = U8A(vm, v)->d;
        c->len = U8A(vm, v)->len;
        return 1;
    case T_INT:
    case T_FIX:
        c->tag = T_INT;
        c->k = floor_int(v);
        c->p = NULL;
        c->len = 0xffffffffu;
        return 1;
    }
    return 0;
}

/* Element i rounded down; none when it is not a number */
int32_t column_at(const struct column *c, uint32_t i, int32_t none)
{
    switch (c->tag) {
    case T_ARR: {
        struct value v = ((const struct value *)c->p)[i];
        return is_num(v) ? floor_int(v) : none;
    }
    case T_I32:
        return ((const int32_t *)c->p)[i];
    case T_FXA:
        return ((const int32_t *)c->p)[i] >> 16;
    case T_U8:
        return ((const uint8_t *)c->p)[i];
    case T_INT:
        return c->k;
    }
    return none;
}

/* Element i in 16.16, 0 when it is not a number */
int32_t column_fix(const struct column *c, uint32_t i)
{
    if (c->tag == T_ARR) {
        struct value v = ((const struct value *)c->p)[i];
        return is_num(v) ? wrap32(scaled(v)) : 0;
    }
    if (c->tag == T_FXA)
        return ((const int32_t *)c->p)[i];
    return (int32_t)((uint32_t)column_at(c, i, 0) << 16);
}

/* Element i as a condition; dflt when the column is absent */
int column_true(struct vm *vm, const struct column *c, uint32_t i,
                       int dflt)
{
    if (c->tag == T_NULL)
        return dflt;
    if (c->tag == T_ARR)
        return vm_truthy(vm, ((const struct value *)c->p)[i]);
    if (c->tag == T_FXA)
        return ((const int32_t *)c->p)[i] != 0;
    return column_at(c, i, 0) != 0;
}

/* actors.draw() and sprites.draw(): each sprite on with a number 0-255, as
 * spr(sp, x, y, 1, 1, flip) draws it. With frames above 1 it is animated:
 * the sprites after sp in turn, each for speed ticks. */
void core_draw_sprites(struct vm *vm, const struct column *on,
                       const struct column *sp, const struct column *x,
                       const struct column *y, const struct column *flip,
                       const struct column *frames,
                       const struct column *speed, uint32_t n)
{
    struct pen p;
    struct sheet sh;
    sheet_of(vm, &sh);
    begin(vm, &p, 0, 0);
    p.per256 = 0;                    /* tile8 and copy_rect charge */
    for (uint32_t i = 0; i < n; i++) {
        int32_t s = column_at(sp, i, -1);
        if (s < 0 || s >= sh.count || !column_true(vm, on, i, 1))
            continue;
        int32_t k = column_at(frames, i, 1);
        if (k > 1) {
            int32_t t = column_at(speed, i, 8);
            s += (int32_t)(vm->frame / (uint32_t)(t > 0 ? t : 1) %
                           (uint32_t)k);
            if (s >= sh.count)
                continue;
        }
        int32_t sx = column_at(x, i, 0), sy = column_at(y, i, 0);
        turn_box(&vm->d, &sx, &sy, 8, 8);
        int64_t xx = sx, yy = sy;
        int fl = column_at(flip, i, 0) & 3;
        int32_t tx, ty;
        p.cost += COST_CALL / 2;
        if (!fl && tile_inside(&p, xx, yy, &tx, &ty))
            tile8(&p, cell_px(&sh, s), sh.w, tx, ty);
        else if (!clipped_out(&p, xx, yy, 8, 8))
            copy_rect(&p, sh.px, sh.w, sh.h, cell_x(&sh, s), cell_y(&sh, s),
                      8, 8, xx, yy, 8, 8, FLIP_X(fl), FLIP_Y(fl));
    }
    finish(vm, &p);
}

/* sprites.draw(sp, xs, ys, n, [flips]): n sprites from the cart's arrays,
 * each a plain array, Int32Array or Uint8Array; sp may be one number for
 * all */
NATIVE(sprites_draw)
{
    (void)out;
    struct column sp, x, y, flip = { T_NULL, NULL, 0, 0 }, on = flip;
    struct column none = flip;      /* no animation */
    int32_t n = is_num(a[3]) ? floor_int(a[3]) : -1;
    if (!column_of(vm, a[0], &sp) ||
        !column_of(vm, a[1], &x) || x.tag == T_INT ||
        !column_of(vm, a[2], &y) || y.tag == T_INT ||
        (argc > 4 && a[4].tag != T_NULL &&
         (!column_of(vm, a[4], &flip) || flip.tag == T_INT)))
        return vm_fail(vm, "sprites.draw() takes arrays of sprites (or one "
                       "sprite), x and y, a count, and arrays of flips",
                       "", "");
    if (n < 0 || (uint32_t)n > sp.len || (uint32_t)n > x.len ||
        (uint32_t)n > y.len || (flip.tag != T_NULL && (uint32_t)n > flip.len))
        return vm_fail(vm, "sprites.draw() needs n no longer than its arrays",
                       "", "");
    core_draw_sprites(vm, &on, &sp, &x, &y, &flip, &none, &none,
                      (uint32_t)n);
    return 1;
}

/* links.draw(): n segments, four whole pixels each (x0, y0, x1, y1), in
 * colour c, as line() draws them */
void core_draw_lines(struct vm *vm, const int32_t *seg, uint32_t n,
                     uint32_t c)
{
    struct pen p;
    begin(vm, &p, c, 1);
    p.per256 = PER16_DOT * 16;
    for (uint32_t i = 0; i < n; i++, seg += 4) {
        p.cost += 18;               /* a line's setup, measured on the rig */
        int32_t x0 = seg[0], y0 = seg[1], x1 = seg[2], y1 = seg[3];
        turn_px(&vm->d, &x0, &y0);
        turn_px(&vm->d, &x1, &y1);
        walk_line(&p, x0, y0, x1, y1, plot_dot, p.pat ? NULL : run_dots,
                  NULL);
    }
    finish(vm, &p);
}

/* links.fill(): n quads, eight whole pixels each (four corners in turn),
 * each in its own colour, as two triangles */
void core_draw_quads(struct vm *vm, const int32_t *q, const uint8_t *c,
                     uint32_t n)
{
    struct pen p;
    begin(vm, &p, 0, 1);
    for (uint32_t i = 0; i < n; i++, q += 8) {
        int64_t t[6];
        p.c = p.d->dpal[c[i]];
        p.cost += 80;               /* two triangles' setup, measured */
        int32_t w[8];
        for (int k = 0; k < 8; k += 2) {
            w[k] = q[k];
            w[k + 1] = q[k + 1];
            turn_px(&vm->d, &w[k], &w[k + 1]);
        }
        for (int k = 0; k < 6; k++)
            t[k] = clampc(w[k]);
        fill_triangle(&p, t);
        t[2] = clampc(w[6]);
        t[3] = clampc(w[7]);
        fill_triangle(&p, t);
    }
    finish(vm, &p);
}

/* A sprite wholly inside the clip and wholly on its sheet, at dx, dy in
 * surface pixels, flipped by fl (1 across, 2 up and down), through xl: a
 * plain loop, without copy_rect's clipping, for the small sprites particle
 * pools draw by the dozen */
static void small_sprite(struct pen *p, const uint8_t *src, int32_t stride,
                         int32_t w, int32_t h, int32_t dx, int32_t dy,
                         int32_t fl, const uint16_t *xl)
{
    uint8_t *dst = p->s.px + dy * p->s.w + dx;
    int32_t step = fl & 1 ? -1 : 1, first = fl & 1 ? w - 1 : 0;
    uint32_t n = 0;
    for (int32_t v = 0; v < h; v++, dst += p->s.w) {
        const uint8_t *s = src + (fl & 2 ? h - 1 - v : v) * stride + first;
        for (int32_t u = 0; u < w; u++, s += step) {
            uint32_t c = xl[*s];
            if (!(c & XLAT_CLEAR)) {
                dst[u] = (uint8_t)c;
                n++;
            }
        }
    }
    p->count += n;
    p->cost += (uint32_t)(w * h) * PER16_SMALL / 16;
    row_touched(p, dy);
    row_touched(p, dy + h - 1);
}

/* particles.draw(): each live particle a pixel of its colour, or with a
 * sprite (arr[6], flipped by arr[7]) the top left w x h of that sprite,
 * its colours 1 to shade drawn as the particle's colour plus k - 1. The
 * arrays are life, x, y, dx, dy, c, sp, flip. */
void core_draw_particles(struct vm *vm, struct value *const *arr,
                         uint32_t n, int32_t w, int32_t h, int32_t shade)
{
    const struct value *life = arr[0], *x = arr[1], *y = arr[2];
    const struct value *c = arr[5], *sp = arr[6], *flip = arr[7];
    struct pen p;
    struct sheet sh;
    /* the colours a shaded sprite is drawn in: the draw palette's, with
     * 1 to shade moved along to the particle's ramp each time */
    static uint16_t ramp[256];
    sheet_of(vm, &sh);
    begin(vm, &p, 0, 0);
    p.per256 = 0;                    /* copy_rect charges; dots below */
    p.cost += n / 8;
    const uint16_t *plain = p.xl;
    uint32_t dots = 0;
    int32_t ramp_of = -1;           /* the colour ramp[] holds, -1 none */
    if (shade)
        memcpy(ramp, plain, sizeof(ramp));
    for (uint32_t i = 0; i < n; i++) {
        if (num_or_0(life[i]) <= 0)
            continue;
        int32_t s = num_or_0(sp[i]), col = num_or_0(c[i]) & 0xff;
        if (s > 0) {
            if (s >= sh.count)
                continue;
            int32_t sx = num_or_0(x[i]), sy = num_or_0(y[i]);
            turn_box(&vm->d, &sx, &sy, w, h);
            int64_t xx = sx, yy = sy;
            p.cost += COST_CALL / 2;
            const uint16_t *xl = plain;
            if (shade && col > 0) {
                /* the ramp made again only for a new colour: debris from
                 * one brick shares one */
                if (col != ramp_of) {
                    for (int32_t k = 1; k <= shade; k++)
                        ramp[k] = plain[(col + k - 1) & 0xff];
                    ramp_of = col;
                }
                xl = ramp;
            }
            int32_t fl = num_or_0(flip[i]);
            int64_t dx = xx + p.ox, dy = yy + p.oy;
            /* a 16 x 16 sprite at the sheet's right or bottom edge reaches
             * past it: copy_rect clips that */
            if (dx >= p.cx0 && dx + w <= p.cx1 && dy >= p.cy0 &&
                dy + h <= p.cy1 && cell_x(&sh, s) + w <= sh.w &&
                cell_y(&sh, s) + h <= sh.h) {
                p.cost += COST_SMALL;
                small_sprite(&p, cell_px(&sh, s), sh.w, w, h, (int32_t)dx,
                             (int32_t)dy, fl, xl);
            } else if (!clipped_out(&p, xx, yy, w, h)) {
                p.xl = xl;
                copy_rect(&p, sh.px, sh.w, sh.h, cell_x(&sh, s),
                          cell_y(&sh, s), w, h, xx, yy, w, h, fl & 1,
                          (fl & 2) != 0);
                p.xl = plain;
            }
            continue;
        }
        int32_t qx = num_or_0(x[i]), qy = num_or_0(y[i]);
        turn_px(&vm->d, &qx, &qy);
        int64_t px = (int64_t)qx + p.ox, py = (int64_t)qy + p.oy;
        if (px < p.cx0 || px >= p.cx1 || py < p.cy0 || py >= p.cy1)
            continue;
        p.s.px[(int32_t)py * p.s.w + (int32_t)px] = p.d->dpal[col];
        p.count++;
        dots++;
        row_touched(&p, (int32_t)py);
    }
    /* a pixel particle's reading of its arrays costs more than its pixel:
     * about 6 units a particle in all, measured */
    p.cost += dots * (5 + PER16_DOT / 16);
    finish(vm, &p);
}

/* What a row of the floor reads: u and v are 16.16 world pixels, and
 * tile[m] is sprite m's top left in the sheet */
struct floor_env
{
    const uint8_t *map;
    uint32_t mw, umax, vmax;
    const uint8_t *const *tile;
    const uint8_t *outside;
    const uint16_t *xl;
};

/* One row, px to end. The bounds are one unsigned compare an axis, as a
 * negative u or v is past umax or vmax. */
__attribute__((optimize("O2")))
static void floor_pixels(uint8_t *px, const uint8_t *end, uint32_t u,
                         uint32_t v, uint32_t du, uint32_t dv,
                         const struct floor_env *e)
{
    const uint8_t *map = e->map, *const *tile = e->tile;
    const uint32_t mw = e->mw, umax = e->umax, vmax = e->vmax;
    const uint16_t *xl = e->xl;
    do {
        const uint8_t *t;
        if (u < umax && v < vmax)
            t = tile[map[(v >> 19) * mw + (u >> 19)]];
        else
            t = e->outside;
        if (t) {
            uint32_t c = xl[t[((v >> 9) & 0x380) | ((u >> 16) & 7)]];
            if (!(c & XLAT_CLEAR))
                *px = (uint8_t)c;
        }
        px++;
        u += du;
        v += dv;
    } while (px < end);
}

/* A row wholly on the map, as most are: with no bounds to test, the loop
 * keeps all it needs in registers */
__attribute__((optimize("O2")))
static void floor_inside(uint8_t *px, const uint8_t *end, uint32_t u,
                         uint32_t v, uint32_t du, uint32_t dv,
                         const struct floor_env *e)
{
    const uint8_t *map = e->map, *const *tile = e->tile;
    const uint32_t mw = e->mw;
    const uint16_t *xl = e->xl;
    do {
        const uint8_t *t = tile[map[(v >> 19) * mw + (u >> 19)]];
        uint32_t c = xl[t[((v >> 9) & 0x380) | ((u >> 16) & 7)]];
        if (!(c & XLAT_CLEAR))
            *px = (uint8_t)c;
        px++;
        u += du;
        v += dv;
    } while (px < end);
}

/* Whether a row's ends, and so all of it, are on the map */
static int row_inside(int64_t u, int64_t v, int64_t du, int64_t dv,
                      int32_t n, const struct floor_env *e)
{
    int64_t u1 = u + du * (n - 1), v1 = v + dv * (n - 1);
    return u >= 0 && u1 >= 0 && v >= 0 && v1 >= 0 && u < e->umax &&
           u1 < e->umax && v < e->vmax && v1 < e->vmax;
}

/* 4x4 ordered dither: a pixel is fogged where its entry is below the
 * fog's level, 0 (none) to 16 (all) */
static const uint8_t bayer[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};

/* plane.fog()'s level at distance d (16.16 world pixels): 0 to 16 */
int32_t core_fog_level(const struct planeview *v, int64_t d)
{
    if (!v->fog_on || d <= v->fog_near)
        return 0;
    if (d >= v->fog_far || v->fog_far <= v->fog_near)
        return 16;
    return (int32_t)(((d - v->fog_near) * 16) / (v->fog_far - v->fog_near));
}

/* The landscape, as Comanche drew it: front to back, at each depth z the
 * slice of ground across the view from a left end to a right end, stepped
 * across the screen two adds a column. Each column keeps the highest row
 * drawn so far and a hill only shows above it, so nearer ground hides
 * farther; the depth's step grows with it, near ground sampled finely and
 * far ground coarsely. A slice's height-to-row is a line, worked out once
 * a slice, so a sample is a multiply.
 *
 * One slice of ground: columns x0..x1 from u, t stepping su, sv (16.16
 * world pixels); a height byte h is screen row (base - h * rise) >> 16 */
struct slice
{
    const uint8_t *height, *colour, *dpal;
    uint32_t wmask, hmask, wshift;
    uint8_t *px;                /* the column x0's pixel in row 0 */
    int32_t w, x0, x1, cy0;
    int32_t step;               /* columns a sample: 1, or 2 coarse */
    int16_t *top;
    int32_t fog;                /* this slice's fog level */
    uint32_t fogm[4];           /* by x & 3, byte k set: row k of 4 fogged */
    uint32_t fogw;              /* the fog's colour in each byte */
    const uint8_t *ramp;        /* NULL, or the colour at each height */
    int32_t nramp;
};

/* The slice's dither at fog level fog: the plane's pattern, fixed to the
 * screen, so far hills are hatched rather than striped by slice */
static void land_fog(struct slice *s, int32_t fog)
{
    s->fog = fog;
    for (int32_t x = 0; x < 4; x++) {
        uint32_t m = 0;
        for (int32_t k = 0; k < 4; k++)
            if (bayer[k][x & 3] < fog)
                m |= 0xffu << (8 * k);
        s->fogm[x] = m;
    }
}

/* Column x's colours for four rows from row y, in colour c (in each
 * byte): the lowest byte is row y's, and a turn right by 8 the next's */
static inline uint32_t land_rows(const struct slice *s, uint32_t c,
                                 int32_t x, int32_t y)
{
    uint32_t m = s->fogm[x & 3], v = (c & ~m) | (s->fogw & m);
    uint32_t r = (uint32_t)(y & 3) * 8;
    return r ? v >> r | v << (32 - r) : v;
}

/* A column of the slice whose sample shows: from row y down to the top
 * drawn so far, in palette colour colour */
__attribute__((optimize("O2")))
static void land_paint(const struct slice *s, int16_t *top, int32_t y,
                       uint8_t colour, uint32_t *filled, int32_t *lo)
{
    int32_t x = (int32_t)(top - s->top), y1 = *top;
    if (y < s->cy0)
        y = s->cy0;
    uint32_t c = s->dpal[colour] * 0x01010101u;
    uint32_t a = land_rows(s, c, x, y);
    uint8_t *p = s->px + y * s->w + (x - s->x0);
    if (s->step == 2 && x + 1 < s->x1) {
        uint32_t b = land_rows(s, c, x + 1, y);
        for (int32_t r = y; r < y1; r++, p += s->w) {
            p[0] = (uint8_t)a;
            p[1] = (uint8_t)b;
            a = a >> 8 | a << 24;
            b = b >> 8 | b << 24;
        }
        *filled += 2 * (uint32_t)(y1 - y);
    } else {
        for (int32_t r = y; r < y1; r++, p += s->w) {
            *p = (uint8_t)a;
            a = a >> 8 | a << 24;
        }
        *filled += (uint32_t)(y1 - y);
    }
    *top = (int16_t)y;
    if (y < *lo)
        *lo = y;
}

/* ...in the colour of the ground's pixel at */
static inline void land_fill(const struct slice *s, int16_t *top, int32_t y,
                             uint32_t at, uint32_t *filled, int32_t *lo)
{
    land_paint(s, top, y, s->colour[at], filled, lo);
}

/* The slice's loop, the image's index worked out by INDEX(u, t): with the
 * size a constant the masks and shift are in the instructions, not in
 * registers, and the loop keeps everything else in registers. Returns
 * the columns still open, each a sample taken. */
#define LAND_SLICE(name, INDEX)                                              \
__attribute__((optimize("O2")))                                             \
static int32_t name(const struct slice *s, uint32_t u, uint32_t t,          \
                    uint32_t su, uint32_t sv, int32_t base, int32_t rise,   \
                    uint32_t *filled, int32_t *lo)                          \
{                                                                           \
    const uint8_t *height = s->height;                                      \
    const uint32_t wmask = s->wmask, hmask = s->hmask, wshift = s->wshift;  \
    const int32_t cy0 = s->cy0;                                             \
    int16_t *top = s->top + s->x0, *end = s->top + s->x1;                   \
    const int32_t step = s->step;                                           \
    int32_t open = 0;                                                       \
    (void)wmask; (void)hmask; (void)wshift;                                 \
    for (; top < end; top += step, u += su, t += sv) {                      \
        int32_t y1 = *top;                                                  \
        if (y1 <= cy0)                                                      \
            continue;                                                       \
        open++;                                                             \
        uint32_t at = INDEX(u, t);                                          \
        int32_t y = (base - (int32_t)height[at] * rise) >> 16;              \
        if (y < y1)                                                         \
            land_fill(s, top, y, at, filled, lo);                           \
    }                                                                       \
    return open;                                                            \
}

#define AT_ANY(u, t) ((((t) >> 16) & hmask) << wshift | (((u) >> 16) & wmask))
#define AT_64(u, t)  ((((t) >> 10) & 0xfc0) | (((u) >> 16) & 0x3f))
#define AT_128(u, t) ((((t) >> 9) & 0x3f80) | (((u) >> 16) & 0x7f))
#define AT_256(u, t) ((((t) >> 8) & 0xff00) | (((u) >> 16) & 0xff))
LAND_SLICE(land_slice, AT_ANY)
#ifndef ARCADE_ASM
LAND_SLICE(land_slice_64, AT_64)
LAND_SLICE(land_slice_128, AT_128)
LAND_SLICE(land_slice_256, AT_256)
#endif

typedef int32_t land_slice_fn(const struct slice *, uint32_t, uint32_t,
                              uint32_t, uint32_t, int32_t, int32_t,
                              uint32_t *, int32_t *);

/* A slice of ground with a ramp: the height blended from the four pixels
 * round each sample, and the colour the ramp's at that height, so the
 * ground is slopes with smooth edges between its colours, and hills
 * smooth outlines, rather than a block a pixel. Slower than the plain
 * loops, so a cart asks for it. On the player a square map's go through
 * land_smooth_arm_* (draw_arm.S) instead: see land_smooth_loop() */
__attribute__((optimize("O2")))
static int32_t land_slice_smooth(const struct slice *s, uint32_t u, uint32_t t,
                               uint32_t su, uint32_t sv, int32_t base,
                               int32_t rise, uint32_t *filled, int32_t *lo)
{
    const uint8_t *height = s->height;
    const uint32_t wmask = s->wmask, hmask = s->hmask, wshift = s->wshift;
    int16_t *top = s->top + s->x0, *end = s->top + s->x1;
    int32_t open = 0;
    for (; top < end; top += s->step, u += su, t += sv) {
        int32_t y1 = *top;
        if (y1 <= s->cy0)
            continue;
        open++;
        /* a pixel's height is at its centre, half a pixel in */
        uint32_t uu = u - 0x8000, tt = t - 0x8000;
        uint32_t x0 = (uu >> 16) & wmask, x1 = (x0 + 1) & wmask;
        uint32_t r0 = ((tt >> 16) & hmask) << wshift;
        uint32_t r1 = (((tt >> 16) + 1) & hmask) << wshift;
        int32_t fx = (int32_t)(uu >> 8) & 0xff, fy = (int32_t)(tt >> 8) & 0xff;
        int32_t a = height[r0 + x0] * 256 +
                    (height[r0 + x1] - height[r0 + x0]) * fx;
        int32_t b = height[r1 + x0] * 256 +
                    (height[r1 + x1] - height[r1 + x0]) * fx;
        int32_t h = a * 256 + (b - a) * fy;     /* 16.16 of a height byte */
        int32_t y = (int32_t)(((int64_t)base -
                               (((int64_t)h * rise) >> 16)) >> 16);
        if (y < y1) {
            int32_t k = h >> 16;
            land_paint(s, top, y, s->ramp[k < s->nramp ? k : s->nramp - 1],
                       filled, lo);
        }
    }
    return open;
}

#ifdef ARCADE_ASM
/* draw_arm.S: the loops for square maps 8 to 256 across with land_fill()
 * inside them, fine and coarse, reading the slice at these offsets */
land_slice_fn land_slice_arm_8_1, land_slice_arm_8_2,
              land_slice_arm_16_1, land_slice_arm_16_2,
              land_slice_arm_32_1, land_slice_arm_32_2,
              land_slice_arm_64_1, land_slice_arm_64_2,
              land_slice_arm_128_1, land_slice_arm_128_2,
              land_slice_arm_256_1, land_slice_arm_256_2;
/* ...and land_slice_smooth()'s, with land_paint() inside them */
land_slice_fn land_smooth_arm_8_1, land_smooth_arm_8_2,
              land_smooth_arm_16_1, land_smooth_arm_16_2,
              land_smooth_arm_32_1, land_smooth_arm_32_2,
              land_smooth_arm_64_1, land_smooth_arm_64_2,
              land_smooth_arm_128_1, land_smooth_arm_128_2,
              land_smooth_arm_256_1, land_smooth_arm_256_2;
_Static_assert(offsetof(struct slice, ramp) == 76 &&
               offsetof(struct slice, nramp) == 80, "draw_arm.S's offsets");
_Static_assert(offsetof(struct slice, px) == 24 &&
               offsetof(struct slice, w) == 28 &&
               offsetof(struct slice, cy0) == 40 &&
               offsetof(struct slice, top) == 48 &&
               offsetof(struct slice, fogm) == 56 &&
               offsetof(struct slice, fogw) == 72, "draw_arm.S's offsets");
#endif

/* The loop for a map wmask + 1 by hmask + 1, a sample every step columns */
static land_slice_fn *land_loop(uint32_t wmask, uint32_t hmask, int32_t step)
{
    if (wmask != hmask)
        return land_slice;
#ifdef ARCADE_ASM
    static land_slice_fn *const arm[6][2] = {
        { land_slice_arm_8_1, land_slice_arm_8_2 },
        { land_slice_arm_16_1, land_slice_arm_16_2 },
        { land_slice_arm_32_1, land_slice_arm_32_2 },
        { land_slice_arm_64_1, land_slice_arm_64_2 },
        { land_slice_arm_128_1, land_slice_arm_128_2 },
        { land_slice_arm_256_1, land_slice_arm_256_2 },
    };
    for (int32_t k = 0; k < 6; k++)
        if (wmask == (8u << k) - 1)
            return arm[k][step == 2];
    return land_slice;
#else
    (void)step;
    return wmask == 63 ? land_slice_64 : wmask == 127 ? land_slice_128 :
           wmask == 255 ? land_slice_256 : land_slice;
#endif
}

/* The smooth loop for a map wmask + 1 by hmask + 1, a sample every step
 * columns */
static land_slice_fn *land_smooth_loop(uint32_t wmask, uint32_t hmask,
                                       int32_t step)
{
#ifdef ARCADE_ASM
    static land_slice_fn *const arm[6][2] = {
        { land_smooth_arm_8_1, land_smooth_arm_8_2 },
        { land_smooth_arm_16_1, land_smooth_arm_16_2 },
        { land_smooth_arm_32_1, land_smooth_arm_32_2 },
        { land_smooth_arm_64_1, land_smooth_arm_64_2 },
        { land_smooth_arm_128_1, land_smooth_arm_128_2 },
        { land_smooth_arm_256_1, land_smooth_arm_256_2 },
    };
    if (wmask == hmask)
        for (int32_t k = 0; k < 6; k++)
            if (wmask == (8u << k) - 1)
                return arm[k][step == 2];
#else
    (void)wmask; (void)hmask; (void)step;
#endif
    return land_slice_smooth;
}

/* The ground at half, a quarter and an eighth of its size, for far
 * slices, whose samples are that many world pixels apart or more: sampled
 * finely there, a hill's edge and a colour's boundary jump between frames
 * as the camera moves. Heights are the mean of each 2x2, colours the
 * commonest of the four. Built again when the ground's sum changes. */
#define LAND_LEVELS 4
#define MIP_BYTES (128 * 128 + 64 * 64 + 32 * 32)
static struct
{
    const uint8_t *height, *colour;
    uint32_t w, h, sum;
    int32_t levels;
    uint8_t mh[MIP_BYTES], mc[MIP_BYTES];
} mip;

struct landlevel
{
    const uint8_t *height, *colour;
    uint32_t wmask, hmask, wshift;
};

/* The ground's bytes summed, Fletcher's way, four words a step where they
 * come in whole words of four */
__attribute__((optimize("O2")))
static uint32_t ground_sum(const uint8_t *p, uint32_t n)
{
    uint32_t s1 = 1, s2 = 0;
    if (!((uintptr_t)p & 3) && !(n & 15)) {
        const uint32_t *q = (const uint32_t *)(const void *)p;
        for (uint32_t k = 0; k < n / 4; k += 4) {
            uint32_t a = q[k], b = q[k + 1], c = q[k + 2], d = q[k + 3];
            s1 += a;
            s2 += s1;
            s1 += b;
            s2 += s1;
            s1 += c;
            s2 += s1;
            s1 += d;
            s2 += s1;
        }
    } else {
        for (uint32_t k = 0; k < n; k++) {
            s1 += p[k];
            s2 += s1;
        }
    }
    return s1 ^ (s2 << 12) ^ (s2 >> 20);
}

static uint8_t commonest(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    if (a == b || a == c || a == d)
        return a;
    if (b == c || b == d)
        return b;
    return c == d ? c : a;
}

/* The slices' spacing at depth z, 16.16 world pixels */
static inline int64_t land_spacing(int64_t z)
{
    return z < 48 << 16 ? 1 << 16 : z < 96 << 16 ? 2 << 16 :
           z < 192 << 16 ? 4 << 16 : 8 << 16;
}

/* The levels for l's ground, the first the ground itself: their count */
static int32_t land_levels(const struct landspec *l, struct landlevel *lv)
{
    uint32_t w = l->wmask + 1, h = l->hmask + 1;
    uint32_t sum = ground_sum(l->height, w * h) ^
                   ground_sum(l->colour, w * h) * 31;
    if (mip.height != l->height || mip.colour != l->colour || mip.w != w ||
        mip.h != h || mip.sum != sum) {
        mip.height = l->height;
        mip.colour = l->colour;
        mip.w = w;
        mip.h = h;
        mip.sum = sum;
        mip.levels = 1;
        const uint8_t *sh = l->height, *sc = l->colour;
        uint32_t at = 0;
        while (mip.levels < LAND_LEVELS && w >= 16 && h >= 16 &&
               at + (w / 2) * (h / 2) <= MIP_BYTES) {
            uint8_t *dh = mip.mh + at, *dc = mip.mc + at;
            for (uint32_t y = 0; y < h / 2; y++)
                for (uint32_t x = 0; x < w / 2; x++) {
                    uint32_t i = 2 * y * w + 2 * x;
                    dh[y * (w / 2) + x] = (uint8_t)
                        ((sh[i] + sh[i + 1] + sh[i + w] + sh[i + w + 1] + 2)
                         >> 2);
                    dc[y * (w / 2) + x] = commonest(sc[i], sc[i + 1],
                                                    sc[i + w], sc[i + w + 1]);
                }
            sh = dh;
            sc = dc;
            at += (w / 2) * (h / 2);
            w /= 2;
            h /= 2;
            mip.levels++;
        }
    }
    w = l->wmask + 1;
    h = l->hmask + 1;
    lv[0].height = l->height;
    lv[0].colour = l->colour;
    uint32_t at = 0;
    for (int32_t k = 0; k < mip.levels; k++) {
        if (k) {
            lv[k].height = mip.mh + at;
            lv[k].colour = mip.mc + at;
            at += (w >> k) * (h >> k);
        }
        lv[k].wmask = (w >> k) - 1;
        lv[k].hmask = (h >> k) - 1;
        lv[k].wshift = l->wshift - (uint32_t)k;
    }
    return mip.levels;
}

/* land.draw(): the landscape front to back, as Comanche drew it, a slice
 * of ground at a time (land_slice()). Returns the samples taken. */
/* The views keep a value a screen column: drawn into a wider image, they
 * stop at the screen's size */
static void begin_view(struct vm *vm, struct pen *p)
{
    begin(vm, p, 0, 0);
    if (p->cx1 > SCREEN_W)
        p->cx1 = SCREEN_W;
    if (p->cy1 > SCREEN_H)
        p->cy1 = SCREEN_H;
    if (p->cx0 > p->cx1)
        p->cx0 = p->cx1;
    if (p->cy0 > p->cy1)
        p->cy0 = p->cy1;
}

/* The depths, world pixels, at which land.draw() keeps each column's top
 * for billboards behind hills: closer together near */
static const int16_t land_band[LAND_BANDS] = {
    12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536,
    2048
};

uint32_t core_draw_land(struct vm *vm, const struct landspec *l)
{
    struct pen p;
    begin_view(vm, &p);
    p.per256 = 0;
    const struct planeview *v = &vm->plane;
    static int16_t top[SCREEN_W];
    struct slice sl;
    sl.x0 = p.cx0;
    sl.x1 = p.cx1;
    if (sl.x0 >= sl.x1 || p.cy0 >= p.cy1)
        return 0;
    for (int32_t x = 0; x < SCREEN_W; x++)
        top[x] = (int16_t)p.cy1;
    struct planeview *vw = &vm->plane;
    int32_t band = 0;
    sl.height = l->height;
    sl.colour = l->colour;
    sl.dpal = p.d->dpal;
    sl.wmask = l->wmask;
    sl.hmask = l->hmask;
    sl.wshift = l->wshift;
    sl.px = p.s.px + sl.x0;
    sl.w = p.s.w;
    sl.cy0 = p.cy0;
    sl.step = l->coarse == 2 ? 2 : 1;
    sl.fogw = p.d->dpal[v->fog_colour] * 0x01010101u;
    sl.ramp = l->ramp;
    sl.nramp = l->nramp;
    land_fog(&sl, 0);
    sl.top = top;
    uint32_t samples = 0, filled = 0;
    int32_t open = sl.x1 - sl.x0, lo = p.cy1;
    int64_t far = (int64_t)l->far << 16;
    struct landlevel lv[LAND_LEVELS];
    int32_t levels = land_levels(l, lv), level = -1;
    /* the ground's two layers read to see whether they changed */
    p.cost += 2 * (l->wmask + 1) * (l->hmask + 1) / BYTES_PER_UNIT;
    land_slice_fn *slice = land_slice;
    /* 1 / focal, 0.32: a slice's half-width is a multiply */
    int64_t inv_focal = ((int64_t)1 << 48) / v->focal;
    /* The slices stay where they are on the ground as the camera moves
     * along the view, a whole number of their spacing from the view's
     * origin: one kept a fixed depth from the camera slides across the
     * ground's pixels, and its span, rows tall up close, jumps each time it
     * crosses one. Their spacing doubles at 48, 96 and 192 world pixels,
     * to match the levels the samples come from. */
    int64_t ahead = ((int64_t)v->x * v->dx + (int64_t)v->y * v->dy) >> 16;
    for (int64_t z = ((ahead + (2 << 16) + 0xffff) & -(1 << 16)) - ahead;
         z < far && open > 0;
         z = ((ahead + z + land_spacing(z)) & -land_spacing(z)) - ahead) {
        int64_t dz = land_spacing(z);
        /* the ground nearer than each depth passed, as it stands */
        while (band < LAND_BANDS && z >= (int64_t)land_band[band] << 16)
            memcpy(vw->land_top[band++], top, sizeof(top));
        /* screen pixels a world pixel at this depth, 16.16, from 1 / z in
         * 0.32 by a 32-bit division; the screen row of height 0, and the
         * rows a height byte lifts, both 16.16 */
        uint32_t inv_z = 0xffffffffu / (uint32_t)z;
        int64_t per = ((int64_t)v->focal * inv_z) >> 16;
        int64_t base = ((int64_t)v->horizon << 16) +
                       (((int64_t)v->z * per) >> 16);
        int64_t rise = ((int64_t)l->scale * per) >> 16;
        if (base > 0x3fffffff || rise > 0x7fffff)
            continue;
        /* the slice's left end and its step across, 16.16 world pixels */
        int64_t across = (z * inv_focal) >> 32;     /* a column's width */
        int64_t cx = v->x + (((int64_t)v->dx * z) >> 16);
        int64_t cy = v->y + (((int64_t)v->dy * z) >> 16);
        int64_t su = ((int64_t)-v->dy * across) >> 16;
        int64_t sv = ((int64_t)v->dx * across) >> 16;
        int64_t ssu = su * sl.step, ssv = sv * sl.step;
        int32_t fog = core_fog_level(v, z);
        if (fog != sl.fog)
            land_fog(&sl, fog);
        /* the level whose cells are no smaller than the samples' spacing,
         * along the slice or between slices */
        int64_t spacing = across * sl.step > dz ? across * sl.step : dz;
        int32_t k = spacing >= 8 << 16 ? 3 : spacing >= 4 << 16 ? 2 :
                    spacing >= 2 << 16 ? 1 : 0;
        if (k >= levels)
            k = levels - 1;
        if (k != level) {
            level = k;
            sl.height = lv[k].height;
            sl.colour = lv[k].colour;
            sl.wmask = lv[k].wmask;
            sl.hmask = lv[k].hmask;
            sl.wshift = lv[k].wshift;
            slice = sl.ramp ? land_smooth_loop(sl.wmask, sl.hmask, sl.step)
                            : land_loop(sl.wmask, sl.hmask, sl.step);
        }
        open = slice(&sl, (uint32_t)((cx - su * (SCREEN_W / 2) +
                                      su * sl.x0) >> k),
                     (uint32_t)((cy - sv * (SCREEN_W / 2) + sv * sl.x0) >> k),
                     (uint32_t)(ssu >> k), (uint32_t)(ssv >> k),
                     (int32_t)base, (int32_t)rise, &filled, &lo);
        samples += (uint32_t)open;
    }
    while (band < LAND_BANDS)
        memcpy(vw->land_top[band++], top, sizeof(top));
    vw->land_set = 1;
    if (lo < p.cy1) {
        row_touched(&p, lo);
        row_touched(&p, p.cy1 - 1);
    }
    /* measured on the ARM rig (drawprobe): a smooth sample, with a ramp,
     * costs twice a plain one */
    p.cost += samples * (l->ramp ? 11 : 5) / 4 + filled / 4;
    finish(vm, &p);
    return samples;
}

/* plane.draw(): rows top..bottom of the floor, each from its start and
 * step in 16.16 world pixels; a cell off the map is sprite outside, or
 * left alone when outside is negative */
/* A row's fog over pixels x0..x1 of row y: its dithered share in colour c */
static void fog_row(uint8_t *px, int32_t y, int32_t x0, int32_t x1,
                    int32_t level, uint8_t c)
{
    if (level >= 16) {
        memset(px + x0, c, (size_t)(x1 - x0));
        return;
    }
    const uint8_t *b = bayer[y & 3];
    for (int32_t k = 0; k < 4; k++)
        if (b[k] < level)
            for (int32_t x = x0 + ((k - x0) & 3); x < x1; x += 4)
                px[x] = c;
}

void core_draw_floor(struct vm *vm, const struct floor_row *rows,
                     int32_t top, int32_t bottom, int32_t outside,
                     int coarse, const uint8_t *remap)
{
    static const uint8_t *tile[256];
    struct pen p;
    struct floor_env e;
    begin_view(vm, &p);
    p.per256 = 0;                    /* each pixel read is charged */
    const uint8_t *sheet = MEM(vm, vm->sheet);
    for (uint32_t m = 0; m < 256; m++) {
        uint32_t s = remap ? remap[m] : m;
        tile[m] = sheet + (s >> 4) * 8 * SHEET_W + (s & 15) * 8;
    }
    e.map = MEM(vm, vm->map);
    e.mw = (uint32_t)vm->map_w;
    e.umax = (uint32_t)vm->map_w << 19;
    e.vmax = (uint32_t)vm->map_h << 19;
    e.tile = tile;
    e.outside = outside >= 0 ? tile[outside] : NULL;
    e.xl = p.xl;
    const int32_t x0 = p.cx0, x1 = p.cx1;
    if (top < p.cy0) top = p.cy0;
    if (bottom > p.cy1 - 1) bottom = p.cy1 - 1;
    const uint8_t fogc = p.d->dpal[vm->plane.fog_colour];
    if (x0 < x1 && top <= bottom) {
        uint32_t read = 0;          /* pixels times their units per 16 */
        /* coarse: every other pixel read, into here, then doubled */
        static uint8_t half[SCREEN_W / 2 + 1];
        int32_t step = coarse == 2 ? 2 : 1;
        for (int32_t y = top; y <= bottom; y++) {
            const struct floor_row *r = &rows[y];
            uint8_t *px = p.s.px + y * p.s.w;
            if (r->fog >= 16) {
                fog_row(px, y, x0, x1, 16, fogc);
                read += (uint32_t)(x1 - x0);
                continue;
            }
            int32_t u = r->u + r->du * x0, v = r->v + r->dv * x0;
            int32_t du = r->du * step, dv = r->dv * step;
            int32_t n = (x1 - x0 + step - 1) / step;
            uint8_t *to = step == 2 ? half : px + x0;
            if (step == 2)
                memcpy(half, px + x0, (size_t)n);
            if (row_inside(u, v, du, dv, n, &e)) {
                floor_inside(to, to + n, (uint32_t)u, (uint32_t)v,
                             (uint32_t)du, (uint32_t)dv, &e);
                read += (uint32_t)n * PER16_FLOOR;
            } else {
                floor_pixels(to, to + n, (uint32_t)u, (uint32_t)v,
                             (uint32_t)du, (uint32_t)dv, &e);
                read += (uint32_t)n * PER16_EDGE;
            }
            if (step == 2) {
                /* each sample twice: two samples a 32-bit store where
                 * the row starts on a word (the player is little-endian:
                 * the first sample is the low half), else a byte at a
                 * time */
                int32_t i = 0, x = x0;
                if (!((uintptr_t)(px + x0) & 3)) {
                    uint32_t *d = (uint32_t *)(void *)(px + x0);
                    for (; x + 3 < x1; i += 2, x += 4)
                        *d++ = (half[i] | (uint32_t)half[i + 1] << 16) *
                               0x0101u;
                }
                for (; x < x1; i++, x += 2) {
                    px[x] = half[i];
                    if (x + 1 < x1)
                        px[x + 1] = half[i];
                }
                read += (uint32_t)(x1 - x0) * 3;    /* measured on the 5G */
            }
            if (r->fog)
                fog_row(px, y, x0, x1, r->fog, fogc);
        }
        row_touched(&p, top);
        row_touched(&p, bottom);
        /* the rig's per-16 figures, times 7/6 as timed on the 5G */
        p.cost += read * 7 / 96 + 64;
    }
    finish(vm, &p);
}

/* plane.billboards(): cw by ch sprites from s, scaled to w by h, their top
 * left at x, y on the screen, the camera aside */
/* A billboard's sprite through the clip p has, and plane.fog() over it at
 * fog level fog: the sprite drawn again into a mask, its every opaque
 * pixel 1, and the fog's colour put where the mask is set and the screen's
 * dither says, as the ground's is, so it fades into the haze with it */
static void billboard_copy(struct pen *p, const struct sheet *sh, int32_t s,
                           int32_t x, int32_t y, int32_t w, int32_t h,
                           int32_t cw, int32_t ch, int flip, int32_t fog,
                           uint8_t fogc)
{
    copy_rect(p, sh->px, sh->w, sh->h, cell_x(sh, s), cell_y(sh, s),
              cw * 8, ch * 8, x, y, w, h, FLIP_X(flip), FLIP_Y(flip));
    if (fog <= 0)
        return;
    static uint8_t mask[SCREEN_W * SCREEN_H];
    static uint16_t ones[256];
    int32_t x0 = x > p->cx0 ? x : p->cx0, x1 = x + w < p->cx1 ? x + w : p->cx1;
    int32_t y0 = y > p->cy0 ? y : p->cy0, y1 = y + h < p->cy1 ? y + h : p->cy1;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (int32_t r = y0; r < y1; r++)
        memset(mask + r * SCREEN_W + x0, 0, (size_t)(x1 - x0));
    for (int c = 0; c < 256; c++)
        ones[c] = p->xl[c] & XLAT_CLEAR ? XLAT_CLEAR : 1;
    struct pen m = *p;
    m.s.px = mask;
    m.s.w = SCREEN_W;
    m.xl = ones;
    copy_rect(&m, sh->px, sh->w, sh->h, cell_x(sh, s), cell_y(sh, s),
              cw * 8, ch * 8, x, y, w, h, FLIP_X(flip), FLIP_Y(flip));
    /* the table and the second copy: about 141 units a fogged billboard,
     * timed on the 5G */
    p->cost = m.cost + (uint32_t)((x1 - x0) * (y1 - y0)) / 16 + 141;
    for (int32_t r = y0; r < y1; r++) {
        const uint8_t *b = bayer[r & 3], *mr = mask + r * SCREEN_W;
        uint8_t *dr = p->s.px + r * p->s.w;
        for (int32_t c = x0; c < x1; c++)
            if (mr[c] && b[c & 3] < fog)
                dr[c] = fogc;
    }
}

void core_draw_billboard(struct vm *vm, int32_t s, int32_t x, int32_t y,
                         int32_t w, int32_t h, int32_t cw, int32_t ch,
                         int flip, int32_t depth)
{
    struct pen p;
    begin_view(vm, &p);
    p.ox = p.oy = 0;
    p.cost = COST_CALL / 2;
    p.per256 = 0;                    /* copy_rect charges the area */
    const struct planeview *v = &vm->plane;
    struct sheet sheet;
    sheet_of(vm, &sheet);
    int32_t fog = v->fog_billboards ? core_fog_level(v, depth) : 0;
    uint8_t fogc = p.d->dpal[v->fog_colour];
    if (!v->depth_set && !v->land_set) {
        billboard_copy(&p, &sheet, s, x, y, w, h, cw, ch, flip, fog, fogc);
        finish(vm, &p);
        return;
    }
    /* behind hills: in each column, only the rows above the ground drawn
     * nearer than it -- as it stood at the last kept depth short of it */
    const int16_t *hill = NULL;
    if (v->land_set) {
        int32_t k = -1;
        while (k + 1 < LAND_BANDS &&
               ((int32_t)land_band[k + 1] << 16) <= depth)
            k++;
        if (k >= 0)
            hill = v->land_top[k];
    }
    /* behind walls: drawn only in the runs of columns where it is nearer
     * than the wall there; each run, of one ground top, as a clip */
    int32_t c0 = p.cx0, c1 = p.cx1, r1 = p.cy1;
    int32_t a = x < c0 ? c0 : x, b = x + w > c1 ? c1 : x + w;
    for (int32_t i = a; i < b;) {
        while (i < b && v->depth_set && v->depth[i] <= depth)
            i++;
        int32_t j = i;
        while (j < b && (!v->depth_set || v->depth[j] > depth) &&
               (!hill || hill[j] == hill[i]))
            j++;
        if (j > i) {
            p.cx0 = i;
            p.cx1 = j;
            p.cy1 = hill && hill[i] < r1 ? hill[i] : r1;
            if (p.cy1 > p.cy0)
                billboard_copy(&p, &sheet, s, x, y, w, h, cw, ch, flip, fog,
                               fogc);
        }
        i = j;
    }
    finish(vm, &p);
}

/* plane.sky(): column x of the screen looks along the view's angle and as
 * far round as it sits from the middle, the view's fov across the screen;
 * the image's column that far round its turn, its bottom row on the
 * horizon, lift rows up */
void core_draw_sky(struct vm *vm, const uint8_t *px, int32_t w, int32_t h,
                   int32_t lift)
{
    struct pen p;
    begin(vm, &p, 0, 0);
    p.per256 = 0;
    const struct planeview *v = &vm->plane;
    const uint16_t *xl = p.xl;
    int32_t bottom = v->horizon - lift, top = bottom - h;
    int32_t y0 = top > p.cy0 ? top : p.cy0;
    int32_t y1 = bottom < p.cy1 ? bottom : p.cy1;
    uint32_t drawn = 0;
    /* (x - SCREEN_W / 2) * fov / SCREEN_W, rounded toward zero as a 64-bit
     * divide would, without one a column: with fov = q * SCREEN_W + r it is
     * k * q + k * r / SCREEN_W, and k * r is small */
    int64_t fov_q = v->fov / SCREEN_W;
    int32_t fov_r = (int32_t)(v->fov % SCREEN_W);
    if (y0 < y1)
        for (int32_t x = p.cx0; x < p.cx1; x++) {
            /* turns round from the angle, 16.16: east is the image's left */
            int32_t k = x - SCREEN_W / 2;
            int64_t turn = -(int64_t)v->angle + k * fov_q +
                           k * fov_r / SCREEN_W;
            int32_t u = (int32_t)(((turn & 0xffff) * w) >> 16);
            const uint8_t *src = px + (y0 - top) * w + u;
            uint8_t *dst = p.s.px + y0 * p.s.w + x;
            for (int32_t y = y0; y < y1; y++, src += w, dst += p.s.w) {
                uint32_t c = xl[*src];
                if (!(c & XLAT_CLEAR)) {
                    *dst = (uint8_t)c;
                    drawn++;
                }
            }
        }
    if (y0 < y1) {
        row_touched(&p, y0);
        row_touched(&p, y1 - 1);
    }
    p.count += drawn;
    /* timed on the 5G: about 12 units per 16 pixels of the area, the
     * columns read down the image */
    p.cost += (uint32_t)((p.cx1 - p.cx0) * (y1 > y0 ? y1 - y0 : 0)) * 12 / 16;
    finish(vm, &p);
}

/* A door the ray is in: whether it meets the panel across the middle of
 * door cell mx, my between distances d and out, and if so where. The
 * panel runs along x when the cells either side of it along x are walls
 * (a door across a corridor running up and down), else along y; it slides
 * aside by open, a fraction of the cell, and the ray passes the gap */
static int door_hit(const struct planeview *v, const struct wallspec *ws,
                    const uint8_t *map, const uint8_t *flags, int32_t mw,
                    int32_t mx, int32_t my, int64_t rx,
                    int64_t ry, int64_t d, int64_t out, int32_t open,
                    int64_t *hit, int *side, int32_t *tx)
{
    int across = mx > 0 && mx + 1 < mw &&
                 (flags[map[my * mw + mx - 1]] & ws->mask) &&
                 (flags[map[my * mw + mx + 1]] & ws->mask);
    int64_t r = across ? ry : rx;
    if (!r)
        return 0;
    int64_t at = ((int64_t)(across ? my : mx) << 19) + (4 << 16);
    int64_t t = ((at - (across ? v->y : v->x)) << 16) / r;
    if (t < d || t > out)
        return 0;
    int64_t along = across ? v->x + ((rx * t) >> 16)
                           : v->y + ((ry * t) >> 16);
    int64_t inside = along - ((int64_t)(across ? mx : my) << 19);
    int64_t gap = ((int64_t)open * 8);
    if (inside < gap || inside >= 8 << 16)
        return 0;
    *hit = t;
    *side = across;
    *tx = (int32_t)(((inside - gap) >> 16) & 7);
    return 1;
}

/* a wall column's texel that is see-through, beside the 256 colours */
#define WALL_CLEAR 0x100

#ifdef ARCADE_ASM
void wall_column_arm(uint8_t *px, int32_t w, int32_t n, uint32_t ty,
                     uint32_t ts, const uint8_t *const *tab);
#endif

/* n pixels down a wall's column from px, w apart, none see-through: row y's
 * texel is ty's whole part, ty stepping by ts, and its colour from tab[y &
 * 3], the four tables in the order of the rows from px */
static void wall_column(uint8_t *px, int32_t w, int32_t n, uint32_t ty,
                        uint32_t ts, const uint8_t *const *tab)
{
#ifdef ARCADE_ASM
    wall_column_arm(px, w, n, ty, ts, tab);
#else
    for (int32_t y = 0; y < n; y++, ty += ts, px += w)
        *px = tab[y & 3][(ty >> 16) & 7];
#endif
}

/* door i of the list's openness, 16.16 (0 shut), or -1 when cell mx, my
 * is not a door */
static int32_t door_open(const struct wallspec *ws, int32_t mx, int32_t my)
{
    for (int32_t i = 0; i < ws->ndoors; i++)
        if (ws->doors[i * 3] == mx && ws->doors[i * 3 + 1] == my) {
            int32_t o = ws->doors[i * 3 + 2];
            return o < 0 ? 0 : o > 1 << 16 ? 1 << 16 : o;
        }
    return -1;
}

/* walls.draw(): for each screen column a ray through the map's cells from
 * the plane's camera, cell edge to cell edge, to the first whose flags
 * share a bit with mask, within far world pixels. Its distance along the
 * view (not along the ray, which would bow the walls) sets the wall's
 * height on the screen: from the floor to height world pixels up, the
 * cell's sprite stretched over it, the column of the sprite where the ray
 * met the face. Faces along y take the dark table when there is one, and
 * every face the plane's fog for its distance. Each column's distance is
 * kept for the billboards. Returns the columns that met a wall. */
uint32_t core_draw_walls(struct vm *vm, const struct wallspec *ws)
{
    struct pen p;
    begin_view(vm, &p);
    p.per256 = 0;
    struct planeview *v = &vm->plane;
    const uint8_t *map = MEM(vm, vm->map), *flags = MEM(vm, vm->flags);
    const uint8_t *sheet = MEM(vm, vm->sheet);
    const uint16_t *xl = p.xl;
    const int32_t mw = vm->map_w, mh = vm->map_h, w = p.s.w;
    const int64_t never = (int64_t)1 << 50, farz = (int64_t)ws->far << 16;
    const uint8_t fogc = p.d->dpal[v->fog_colour];
    uint32_t hit = 0, steps = 0, drawn = 0;
    for (int32_t x = 0; x < SCREEN_W; x++)
        v->depth[x] = 0x7fffffff;
    /* half a column's width over focal, 0.32: a column's offset is then a
     * multiply */
    int64_t half_inv = ((int64_t)1 << 47) / v->focal;
    for (int32_t x = p.cx0; x < p.cx1; x++) {
        /* the ray: the view's direction plus the column's way along the
         * screen, right being (-dy, dx); its length along the view is 1,
         * so distances along it are distances along the view */
        int64_t off = ((int64_t)(2 * x + 1 - SCREEN_W) * half_inv) >> 16;
        int64_t rx = v->dx + ((-(int64_t)v->dy * off) >> 16);
        int64_t ry = v->dy + (((int64_t)v->dx * off) >> 16);
        int32_t mx = v->x >> 19, my = v->y >> 19;
        int32_t sx = rx < 0 ? -1 : 1, sy = ry < 0 ? -1 : 1;
        int64_t ax = rx < 0 ? -rx : rx, ay = ry < 0 ? -ry : ry;
        /* the distance between column edges and row edges, and to the
         * first of each, from 1 / the ray's slope on each axis (16.16, by a
         * 32-bit division) */
        int64_t ix = ax > 1 ? (int64_t)(0xffffffffu / (uint32_t)ax) : 0;
        int64_t iy = ay > 1 ? (int64_t)(0xffffffffu / (uint32_t)ay) : 0;
        int64_t ex = rx < 0 ? v->x - ((int64_t)mx << 19)
                            : ((int64_t)(mx + 1) << 19) - v->x;
        int64_t ey = ry < 0 ? v->y - ((int64_t)my << 19)
                            : ((int64_t)(my + 1) << 19) - v->y;
        int64_t ddx = ix ? ix * 8 : never, ddy = iy ? iy * 8 : never;
        int64_t tdx = ix ? (ex * ix) >> 16 : never;
        int64_t tdy = iy ? (ey * iy) >> 16 : never;
        int64_t d = 0;
        int side = 0, found = 0, door = 0;
        int32_t tx = 0;
        uint8_t m = 0;
        for (;;) {
            if (tdx < tdy) {
                d = tdx;
                tdx += ddx;
                mx += sx;
                side = 0;
            } else {
                d = tdy;
                tdy += ddy;
                my += sy;
                side = 1;
            }
            steps++;
            if (d > farz)
                break;
            if (mx < 0 || my < 0 || mx >= mw || my >= mh)
                continue;
            m = map[my * mw + mx];
            if (!(flags[m] & ws->mask))
                continue;
            int32_t open = ws->ndoors ? door_open(ws, mx, my) : -1;
            if (open < 0) {
                found = 1;
                break;
            }
            /* a door: met at its panel, or passed through its gap */
            int64_t hd;
            if (door_hit(v, ws, map, flags, mw, mx, my, rx, ry, d,
                         tdx < tdy ? tdx : tdy, open, &hd, &side, &tx)) {
                d = hd;
                found = door = 1;
                break;
            }
        }
        if (!found)
            continue;
        if (d < (1 << 12))
            d = 1 << 12;
        v->depth[x] = (int32_t)d;
        hit++;
        /* where along the face it met, as the sprite's column, the same
         * way round from either side */
        if (!door) {
            int64_t along = side == 0 ? v->y + ((ry * d) >> 16)
                                      : v->x + ((rx * d) >> 16);
            tx = (int32_t)((along >> 16) & 7);
            if ((side == 0 && rx < 0) || (side == 1 && ry > 0))
                tx = 7 - tx;
        }
        /* the wall's top and bottom rows at this distance */
        int64_t per = ((int64_t)v->focal << 16) / d;
        int32_t bottom = v->horizon - 1 +
                         (int32_t)(((int64_t)v->z * per) >> 32);
        int32_t top = v->horizon - 1 +
                      (int32_t)((((int64_t)v->z - ws->height) * per) >> 32);
        if (top >= bottom)
            continue;
        int32_t y0 = top < p.cy0 ? p.cy0 : top;
        int32_t y1 = bottom > p.cy1 ? p.cy1 : bottom;
        if (y0 >= y1)
            continue;
        /* eight rows of the sprite over the wall's height */
        uint32_t ts = (8u << 16) / (uint32_t)(bottom - top);
        uint32_t ty = (uint32_t)(y0 - top) * ts;
        const uint8_t *col = sheet + (m >> 4) * 8 * SHEET_W + (m & 15) * 8 + tx;
        const uint8_t *dark = side == 1 ? ws->dark : NULL;
        int32_t fog = core_fog_level(v, d);
        /* the column's eight texels as drawn, translated and darkened
         * here rather than at every pixel; WALL_CLEAR where see-through */
        uint16_t base[8];
        int clear = 0;
        for (int32_t r = 0; r < 8; r++) {
            uint8_t s = col[r * SHEET_W];
            uint32_t c = xl[s];
            if (c & XLAT_CLEAR)
                clear = 1;
            else if (dark && !(xl[dark[s]] & XLAT_CLEAR))
                c = xl[dark[s]];
            base[r] = (uint16_t)(c & XLAT_CLEAR ? WALL_CLEAR : (uint8_t)c);
        }
        uint8_t *px = p.s.px + y0 * w + x;
        if (!clear) {
            /* a table for each row of the dither, the same four times
             * where there is no fog */
            uint8_t tab[4][8];
            const uint8_t *rows[4];
            for (int32_t k = 0; k < (fog ? 4 : 1); k++)
                for (int32_t r = 0; r < 8; r++)
                    tab[k][r] = fog && bayer[k][x & 3] < fog
                                ? fogc : (uint8_t)base[r];
            for (int32_t k = 0; k < 4; k++)
                rows[k] = tab[fog ? (y0 + k) & 3 : 0];
            wall_column(px, w, y1 - y0, ty, ts, rows);
        } else
            for (int32_t y = y0; y < y1; y++, ty += ts, px += w) {
                uint32_t c = base[(ty >> 16) & 7];
                if (c == WALL_CLEAR)
                    continue;
                *px = fog && bayer[y & 3][x & 3] < fog ? fogc : (uint8_t)c;
            }
        drawn += (uint32_t)(y1 - y0);
        row_touched(&p, y0);
        row_touched(&p, y1 - 1);
    }
    v->depth_set = 1;
    /* measured on the ARM rig (drawprobe): the rays and each column's
     * set-up, its divisions most of it; the pixels about 8 cycles each */
    p.cost += (steps * 2 + hit * 40) * 4 / 3 + drawn * 3 / 16;
    finish(vm, &p);
    return hit;
}

/* ---- light --------------------------------------------------------------- *
 * Lights over a dark scene. Each pixel within a light's reach takes a
 * level from how near its centre it is, the nearest light's winning; the
 * level picks a colour table from the cart's ramp, darkest first, and a
 * pixel past every light takes the darkest. Within half its reach a light
 * leaves the scene as drawn. The steps between levels are
 * dithered, so a torch fades rather than banding. */

#define LIGHT_LEVELS 8
/* the most lights one call takes */
#define LIGHT_MAX 4096
/* a reach, in sixteenths, from which a light's middle is filled */
#define LIGHT_SPLIT (32 * 16)

/* measured on the ARM rig (drawprobe) and timed on the 5G, in units: a
 * colour table; the shade tables, when the ramp's length changes; a row
 * any light reaches, and each light's row; per 32 pixels stepped through
 * by a light, and per 32 mapped through their levels over darkening; and
 * per 16 of the clip, all darkened or mapped */
#define LIGHT_TABLE 25
#define LIGHT_SHADE 1200
#define LIGHT_ROW 16
#define LIGHT_LAMP 16
#define LIGHT_STEP32 11
#define LIGHT_MAP32 3
#define LIGHT_DARK16 4
/* each light given, and each within the clip's rows: its set-up, a 64-bit
 * division among it; and each row a light's edges are walked down */
#define LIGHT_EACH 2
#define LIGHT_SET 12
#define LIGHT_WALK 1

/* the units for the pixels stepped through and mapped, the rows lit and
 * the lights' rows */
static uint32_t light_units(uint32_t step, uint32_t map, uint32_t rows,
                            uint32_t lamprows)
{
    return (step * LIGHT_STEP32 + map * LIGHT_MAP32) / 32 + rows * LIGHT_ROW +
           lamprows * LIGHT_LAMP;
}

#ifdef ARCADE_ASM
void light_map_arm(uint8_t *px, const uint8_t *lit, int32_t n,
                   const uint8_t *lut, uint32_t levels);
void light_dark_arm(uint8_t *px, int32_t n, const uint8_t *lut);
void light_span_arm(uint8_t *row, int32_t n, uint32_t q, uint32_t dq,
                    uint32_t inv, const uint8_t *const *tab);
#endif

/* The level at a squared distance of i/256 of the reach's square, for the
 * dither b: 2 * levels * (1 - its root) in sixteenths, plus b, over 16 --
 * full light within half the reach, dithered steps across the outer half.
 * Kept for shade_levels levels, beside the tables rather than in the
 * game's state, which a resumed game brings back without them. */
static uint8_t shade[16][260];
static uint32_t shade_levels;

static void light_shade(uint32_t levels)
{
    int32_t root = 0;
    for (int32_t i = 0; i <= 256; i++) {
        /* the root of i * 256, walked up as i grows */
        while ((root + 1) * (root + 1) <= i * 256)
            root++;
        int32_t f = (int32_t)levels * (256 - root) / 8;
        /* (f + b) / 16 is f / 16 until b reaches 16 less f's remainder,
         * then one more; at most levels */
        int32_t k = f >> 4, split = 16 - (f & 15);
        uint8_t lo = (uint8_t)(k > (int32_t)levels ? (int32_t)levels : k);
        uint8_t up = (uint8_t)(k + 1 > (int32_t)levels ? (int32_t)levels
                                                       : k + 1);
        uint8_t *s = &shade[0][i], *mid = &shade[split][i];
        uint8_t *end = &shade[15][i];
        for (; s < mid; s += sizeof(shade[0]))
            *s = lo;
        for (; s <= end; s += sizeof(shade[0]))
            *s = up;
    }
}

/* n pixels of a light's row from row: each pixel's level from q, its
 * squared distance in 256ths of the reach's square with 23 bits of
 * fraction, through its dither's shade table, tab[0] the first pixel's;
 * the highest of the lights kept. q steps by dq, which grows by inv. */
static void light_span(uint8_t *row, int32_t n, uint32_t q, uint32_t dq,
                       uint32_t inv, const uint8_t *const *tab)
{
#ifdef ARCADE_ASM
    light_span_arm(row, n, q, dq, inv, tab);
#else
    for (int32_t x = 0; x < n; x++) {
        uint8_t k = tab[x & 3][q >> 23];
        if (k > row[x])
            row[x] = k;
        q += dq;
        dq += inv;
    }
#endif
}

/* pixels xa to xb of the row dy2 (sixteenths squared) from a light at lx:
 * q and dq at xa, with inv 2^40 over the reach's square; row8 the row's
 * shade tables by x & 3, twice over. dq, the step to the next pixel's
 * squared distance (32 * (dx + 8)) times inv over 2^9, is (dx + 8) * inv
 * over 16, from one 32 by 32 multiply. q and dq are both rounded up, so q
 * stepped is never below the true value: at a light's very centre it
 * would wrap below zero and index past the shade table. */
static void light_run(uint8_t *row, int32_t xa, int32_t xb, int32_t lx,
                      int32_t dy2, uint32_t inv, const uint8_t *const *row8)
{
    if (xa >= xb)
        return;
    int32_t dx = xa * 16 + 8 - lx;
    uint32_t dd = (uint32_t)(dx * dx + dy2);
    int32_t s = dx + 8;
    uint64_t m = (uint64_t)(uint32_t)(s < 0 ? -s : s) * inv;
    uint32_t dq = s < 0 ? -(uint32_t)(m >> 4) : (uint32_t)((m + 15) >> 4);
    light_span(row + xa, xb - xa,
               (uint32_t)(((uint64_t)dd * inv + 511) >> 9), dq, inv,
               row8 + (xa & 3));
}

/* lit zeroed from a to b, rounded out to whole words: a few in place,
 * more by memset */
static inline __attribute__((always_inline))
void light_zero(uint8_t *lit, int32_t a, int32_t b)
{
    uint32_t *z = (uint32_t *)(void *)(lit + (a & ~3));
    uint32_t *e = (uint32_t *)(void *)(lit + ((b + 3) & ~3));
    if (e - z > 8)
        memset(z, 0, (size_t)((uint8_t *)e - (uint8_t *)z));
    else
        while (z < e)
            *z++ = 0;
}

/* n pixels through the darkest table */
static void light_dark(uint8_t *px, int32_t n, const uint8_t *dark)
{
    int32_t x = 0;
#ifdef ARCADE_ASM
    for (; x < n && ((uintptr_t)(px + x) & 3); x++)
        px[x] = dark[px[x]];
    if (n - x >= 4) {
        light_dark_arm(px + x, (n - x) & ~3, dark);
        x += (n - x) & ~3;
    }
#endif
    for (; x < n; x++)
        px[x] = dark[px[x]];
}

/* n pixels each through its level's table; the brightest are left be.
 * Four at a time where the levels are all dark or all bright. */
static void light_map(uint8_t *px, const uint8_t *row, int32_t n,
                      const uint8_t (*lut)[256], uint32_t levels)
{
    int32_t x = 0;
#ifdef ARCADE_ASM
    if (!(((uintptr_t)px | (uintptr_t)row) & 3)) {
        x = n & ~3;
        light_map_arm(px, row, x, lut[0], levels);
    }
#endif
    for (; x + 4 <= n; x += 4) {
        uint32_t l0 = row[x], l1 = row[x + 1], l2 = row[x + 2];
        uint32_t l3 = row[x + 3];
        /* all four darkest; else all four at the top level (an AND keeps
         * every bit of levels only where each is levels), left alone */
        if ((l0 | l1 | l2 | l3) == 0) {
            px[x] = lut[0][px[x]];
            px[x + 1] = lut[0][px[x + 1]];
            px[x + 2] = lut[0][px[x + 2]];
            px[x + 3] = lut[0][px[x + 3]];
        } else if ((l0 & l1 & l2 & l3) != levels) {
            for (int k = 0; k < 4; k++)
                if (row[x + k] < levels)
                    px[x + k] = lut[row[x + k]][px[x + k]];
        }
    }
    for (; x < n; x++)
        if (row[x] < levels)
            px[x] = lut[row[x]][px[x]];
}

/* A light as the rows are drawn: its centre in sixteenths of a pixel, so a
 * squared distance fits 32 bits and steps along a row by adding; its
 * reach's square, a quarter of that, and 2^40 over it; its rows; the
 * pixels of the last row whose centres are within the reach (l to r) and
 * within half of it (il to ir), end exclusive; and the next light to start
 * on its first row, plus one, or 0 */
struct lamp
{
    int32_t lx, ly, r2, quarter, y0, y1;
    uint32_t inv;
    int32_t l, r, il, ir;
    int32_t next;
};

/* whether pixel x's centre, dy2 (sixteenths squared) above or below the
 * light's, is within lim of it */
static inline __attribute__((always_inline))
int lamp_in(const struct lamp *m, int32_t x, int32_t dy2, int32_t lim)
{
    int32_t dx = x * 16 + 8 - m->lx;
    return dx * dx + dy2 <= lim;
}

/* The pixels of a row within lim of the light: l to r, moved from the
 * last row's a pixel at a time, as a row's edges move little from the
 * last's. The pixel holding the centre is the nearest, so none is within
 * if it is not. Returns 0 for none. */
static int lamp_row(const struct lamp *m, int32_t dy2, int32_t lim,
                    int32_t *l, int32_t *r)
{
    int32_t c = m->lx >> 4;
    if (!lamp_in(m, c, dy2, lim))
        return 0;
    int32_t a = *l > c ? c : *l, b = *r - 1 < c ? c : *r - 1;
    if (lamp_in(m, a, dy2, lim))
        while (lamp_in(m, a - 1, dy2, lim))
            a--;
    else
        while (!lamp_in(m, a, dy2, lim))
            a++;
    if (lamp_in(m, b, dy2, lim))
        while (lamp_in(m, b + 1, dy2, lim))
            b++;
    else
        while (!lamp_in(m, b, dy2, lim))
            b--;
    *l = a;
    *r = b + 1;
    return 1;
}

/* light.draw(x, y, r, n, ramp): n lights, each centre (x, y) and reach r,
 * as numbers or arrays; ramp a list of 1 to 8 colour tables, each an
 * array or Uint8Array taking a colour to its darker one (colours past its
 * end kept). Over the target, within the clip, after the scene is drawn.
 * A row at a time: its lights' levels into one row of levels, then the
 * pixels they reach through their levels' tables and the rest through the
 * darkest. */
NATIVE(light_draw)
{
    (void)argc; (void)out;
    struct column cx, cy, cr;
    if (!column_of(vm, a[0], &cx) || !column_of(vm, a[1], &cy) ||
        !column_of(vm, a[2], &cr) || !is_num(a[3]))
        return vm_fail(vm, "light.draw() needs x, y and r (numbers or "
                       "arrays), then how many lights", "", "");
    if (a[4].tag != T_ARR || ARR(vm, a[4])->len < 1 ||
        ARR(vm, a[4])->len > LIGHT_LEVELS)
        return vm_fail(vm, "light.draw() needs a ramp: a list of 1 to 8 "
                       "colour tables, darkest first", "", "");
    int32_t n = floor_int(a[3]);
    if (n < 0)
        n = 0;
    if (n > LIGHT_MAX)
        return vm_fail(vm, "light.draw() takes at most 4096 lights", "", "");
    if ((uint32_t)n > cx.len || (uint32_t)n > cy.len || (uint32_t)n > cr.len)
        return vm_fail(vm, "light.draw() has fewer x, y or r than lights", "",
                       "");
    uint32_t levels = ARR(vm, a[4])->len;
    /* the tables, each the colours as they are from cart[k] up: only those
     * a cart's table covers are written each call */
    static uint8_t lut[LIGHT_LEVELS][256];
    static uint16_t cart[LIGHT_LEVELS] = { 256, 256, 256, 256,
                                           256, 256, 256, 256 };
    for (uint32_t k = 0; k < levels; k++) {
        struct column t;
        if (!column_of(vm, BUF(vm, ARR(vm, a[4])->buf)->v[k], &t) ||
            t.tag == T_INT)
            return vm_fail(vm, "light.draw()'s ramp holds colour tables: "
                           "arrays or Uint8Arrays", "", "");
        uint32_t m = t.len < 256 ? t.len : 256;
        if (t.tag == T_U8)
            memcpy(lut[k], t.p, m);
        else
            for (uint32_t c = 0; c < m; c++)
                lut[k][c] = (uint8_t)column_at(&t, c, (int32_t)c);
        for (uint32_t c = m; c < cart[k]; c++)
            lut[k][c] = (uint8_t)c;
        cart[k] = (uint16_t)m;
    }
    struct pen p;
    begin(vm, &p, 0, 0);
    int32_t w = p.cx1 - p.cx0, h = p.cy1 - p.cy0;
    if (w <= 0 || h <= 0) {
        finish(vm, &p);
        return 1;
    }
    uint32_t cost = levels * LIGHT_TABLE;
    if (shade_levels != levels) {
        shade_levels = levels;
        light_shade(levels);
        cost += LIGHT_SHADE;
    }
    /* the lamps; those lit on the row, by number; the first to start on
     * each row, plus one; and a row of levels indexed by x */
    uint32_t lamps_at = (uint32_t)n * sizeof(struct lamp);
    uint32_t lit_at = lamps_at + (uint32_t)(n + h) * 4;
    uint8_t *mem = scratch(vm, lit_at + (uint32_t)p.cx1 + 4);
    if (!mem)
        return 0;
    /* scratch may have collected: the surface is found again */
    begin(vm, &p, 0, 0);
    struct lamp *lamps = (struct lamp *)(void *)mem;
    int32_t *on = (int32_t *)(void *)(mem + lamps_at);
    int32_t *starts = on + n;
    uint8_t *lit = mem + lit_at;
    int32_t nl = 0, non = 0;
    uint32_t reach = 0;
    for (int32_t i = 0; i < n; i++) {
        struct lamp *m = &lamps[nl];
        int64_t lx = column_fix(&cx, (uint32_t)i);
        int64_t ly = column_fix(&cy, (uint32_t)i);
        if (vm->d.cam_moved) {
            turn_fix(&vm->d, &lx, &ly);
            /* a light turned far off cannot reach the clip */
            if (lx > (1 << 30) || lx < -(1 << 30) || ly > (1 << 30) ||
                ly < -(1 << 30))
                continue;
        }
        m->lx = (int32_t)(lx >> 12) + (int32_t)(p.ox * 16);
        m->ly = (int32_t)(ly >> 12) + (int32_t)(p.oy * 16);
        int32_t r = column_fix(&cr, (uint32_t)i) >> 12;
        if (r < 17)
            continue;                   /* under a pixel */
        if (r > 16 * 512)
            r = 16 * 512;
        m->r2 = r * r;
        m->quarter = m->r2 >> 2;
        m->inv = (uint32_t)(((uint64_t)256 << 32) / (uint32_t)m->r2);
        m->y0 = (m->ly - r) >> 4;
        m->y1 = ((m->ly + r) >> 4) + 1;
        if (m->y0 < p.cy0) m->y0 = p.cy0;
        if (m->y1 > p.cy1) m->y1 = p.cy1;
        if (m->y0 >= m->y1)
            continue;
        m->l = m->il = m->r = m->ir = m->lx >> 4;
        m->next = starts[m->y0 - p.cy0];
        starts[m->y0 - p.cy0] = ++nl;
        reach += (uint32_t)r >> 5;      /* its first row's edges walked out */
    }
    cost += (uint32_t)n * LIGHT_EACH + (uint32_t)nl * LIGHT_SET + reach;
    /* the pixels stepped through and mapped, the rows lit and the
     * lights' rows, since the last spend */
    uint32_t step = 0, mapped = 0, rows = 0, lamprows = 0, walked = 0;
    for (int32_t y = p.cy0; y < p.cy1; y++) {
        /* the row's shade tables by x & 3, twice over, once a light
         * reaches it */
        const uint8_t *row8[8];
        row8[0] = NULL;
        /* lo to hi: the levels kept, whole words within the clip */
        int32_t lo = p.cx1, hi = p.cx1;
        for (int32_t i = starts[y - p.cy0]; i; i = lamps[i - 1].next)
            on[non++] = i - 1;
        for (int32_t k = 0; k < non; k++) {
            struct lamp *m = &lamps[on[k]];
            if (y >= m->y1) {
                on[k--] = on[--non];
                continue;
            }
            int32_t dy = y * 16 + 8 - m->ly, dy2 = dy * dy;
            /* its edges walked whether or not the row reaches the clip */
            walked++;
            if (!lamp_row(m, dy2, m->r2, &m->l, &m->r))
                continue;
            int32_t xa = m->l < p.cx0 ? p.cx0 : m->l;
            int32_t xb = m->r > p.cx1 ? p.cx1 : m->r;
            if (xa >= xb)
                continue;
            lamprows++;
            if (!row8[0])
                for (int j = 0; j < 8; j++)
                    row8[j] = shade[bayer[y & 3][j & 3]];
            /* within half the reach, full light: filled rather than
             * stepped through where that saves, a reach of 32 or more */
            int32_t ia = xb, ib = xb;
            if (m->r2 >= LIGHT_SPLIT * LIGHT_SPLIT &&
                lamp_row(m, dy2, m->quarter, &m->il, &m->ir)) {
                ia = m->il < xa ? xa : m->il;
                ib = m->ir > xb ? xb : m->ir;
                if (ia >= ib)
                    ia = ib = xb;
            }
            int32_t wa = xa & ~3, wb = (xb + 3) & ~3;
            if (wa < p.cx0) wa = p.cx0;
            if (wb > p.cx1) wb = p.cx1;
            if (lo >= hi) {
                light_zero(lit, wa, wb);
                lo = wa, hi = wb;
            } else {
                if (wa < lo) {
                    light_zero(lit, wa, lo);
                    lo = wa;
                }
                if (wb > hi) {
                    light_zero(lit, hi, wb);
                    hi = wb;
                }
            }
            light_run(lit, xa, ia, m->lx, dy2, m->inv, row8);
            if (ia < ib) {
                memset(lit + ia, (int)levels, (size_t)(ib - ia));
                light_run(lit, ib, xb, m->lx, dy2, m->inv, row8);
            }
            step += (uint32_t)(xb - xa - (ib - ia));
        }
        uint8_t *px = p.s.px + y * p.s.w;
        if (lo >= hi)
            light_dark(px + p.cx0, w, lut[0]);
        else {
            light_dark(px + p.cx0, lo - p.cx0, lut[0]);
            light_map(px + lo, lit + lo, hi - lo,
                      (const uint8_t (*)[256])lut, levels);
            light_dark(px + hi, p.cx1 - hi, lut[0]);
            mapped += (uint32_t)(hi - lo);
            rows++;
        }
        row_touched(&p, y);
        if (step + mapped + walked * 32 > 65536) {
            if (!vm_spend(vm, light_units(step, mapped, rows, lamprows) +
                              walked * LIGHT_WALK))
                return 0;
            step = mapped = rows = lamprows = walked = 0;
        }
    }
    p.count = (uint32_t)(w * h);
    p.per256 = LIGHT_DARK16 * 16;
    p.cost += cost + light_units(step, mapped, rows, lamprows) +
              walked * LIGHT_WALK;
    finish(vm, &p);
    return 1;
}

/* ---- the host's view of the screen ----------------------------------- */

const struct arcade_screen *arcade_screen(struct vm *vm)
{
    static struct arcade_screen view;
    view.px = MEM(vm, vm->screen);
    view.width = SCREEN_W;
    view.height = SCREEN_H;
    view.dirty_lo = vm->d.dirty_lo;
    view.dirty_hi = vm->d.dirty_hi;
    view.rgb = vm->d.shown;
    view.palette_changed = vm->d.palette_changed;
    return &view;
}

void arcade_presented(struct vm *vm)
{
    vm->d.dirty_lo = SCREEN_H;
    vm->d.dirty_hi = -1;
    vm->d.palette_changed = 0;
}
