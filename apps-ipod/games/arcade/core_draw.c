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
 *   sprites        spr, sspr, sget/sset, fget/fset, images
 *   the map
 *   state          camera, clip, colour, cursor, palettes, target
 *   memory         the engine's arrays as Uint8Arrays; copy and fill
 *   the host's view of the screen
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"
#include "games/arcade/core_font.h"

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
        for (int c = 0; c < 256; c++)
            d->xlat[c] = d->transparent[c >> 3] & (1 << (c & 7))
                         ? XLAT_CLEAR : d->dpal[c];
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
 * The chunks' layouts are tools/arcade/src/assets.js's. Each is checked
 * whole when the cart loads, so the natives below can trust them. */

static uint32_t le16(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
    return le16(p) | le16(p + 2) << 16;
}

static int damaged(struct vm *vm, const char *what)
{
    return vm_fail(vm, "this game file is damaged: ", what, "");
}

/* Palette n's colours and how many, or NULL */
static const uint8_t *palette_at(const struct vm *vm, uint32_t n,
                                 uint32_t *count)
{
    const uint8_t *p = vm->pals, *end = p + vm->pals_len;
    *count = 0;
    if (!p || vm->pals_len < 1 || n >= p[0])
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
        m->zlen = le32(p + 4);
        p += 8;
        if (!m->w || !m->h || m->w > MAP_MAX || m->h > MAP_MAX ||
            m->zlen > (uint32_t)(end - p))
            return damaged(vm, "maps");
        m->z = p;
        p += m->zlen;
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

/* Map n becomes the active one, as stored */
static int use_map(struct vm *vm, uint32_t n)
{
    const struct mapinfo *m = &vm->mapdir[n];
    uint32_t cells = (uint32_t)m->w * m->h;
    if (!lz4_decode(m->z, m->zlen, MEM(vm, vm->map), cells))
        return damaged(vm, "a map");
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
        le32(p + 4) > vm->shet_len - 8 ||
        vm->shet_len - 8 - le32(p + 4) != 256 ||
        !lz4_decode(p + 8, le32(p + 4), MEM(vm, vm->sheet), SHEET_W * SHEET_H))
        return damaged(vm, "the sheet");
    memcpy(MEM(vm, vm->flags), p + 8 + le32(p + 4), 256);
    return 1;
}

/* Each image as an img outside the heap, and the globals IMG.* name */
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
        for (uint32_t i = 0; i < n; i++) {
            if (end - p < 8)
                return damaged(vm, "images");
            uint32_t w = le16(p), h = le16(p + 2), zlen = le32(p + 4);
            p += 8;
            if (!w || !h || w > IMAGE_MAX || h > IMAGE_MAX ||
                zlen > (uint32_t)(end - p))
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

static int flag(struct vm *vm, struct value *a, int argc, int i)
{
    return i < argc && vm_truthy(vm, a[i]);
}

#define WHOLE(i, def, x, fn) int32_t x; \
    if (!whole(vm, a, argc, i, def, fn, &x)) return 0
#define COLOUR(i, c, fn) uint32_t c; \
    if (!colour(vm, a, argc, i, fn, &c)) return 0

/* ---- pens --------------------------------------------------------------- */

/* What a drawing call is charged, in units, from its cycles on the 5G: a
 * call's setup, then each row a shape works out, glyph or map tile, and
 * each pixel by its kind as units per 16 pixels -- the pixels drawn, or
 * for copies and tline the pixels read, which cost the same whether or
 * not they are transparent. */
#define COST_CALL      20
#define COST_COPY      25   /* copy_rect's setup, on top of the call's */
#define COST_ROW       2
#define COST_GLYPH     12
#define COST_BIG_GLYPH 25
#define COST_TILE      5
#define PER16_FILL     1
#define PER16_PATTERN  10
#define PER16_TILE     5
#define PER16_COPY     9
#define PER16_SCALED   16
#define PER16_DOT      18
#define PER16_TLINE    48

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
    uint16_t per16;                 /* units per 16 pixels */
    int32_t lo, hi;
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
    p->per16 = p->pat ? PER16_PATTERN : PER16_FILL;
    p->lo = 0x7fffffff;
    p->hi = -1;
    p->d = d;
    p->xl = xlat_of(d);
}

static void row_touched(struct pen *p, int32_t y)
{
    if (y < p->lo)
        p->lo = y;
    if (y > p->hi)
        p->hi = y;
}

static void finish(struct vm *vm, struct pen *p)
{
    if (p->s.is_screen && p->hi >= 0) {
        if (p->lo < vm->d.dirty_lo)
            vm->d.dirty_lo = (int16_t)p->lo;
        if (p->hi > vm->d.dirty_hi)
            vm->d.dirty_hi = (int16_t)p->hi;
    }
    vm_charge(vm, p->cost + p->count * p->per16 / 16);
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
    row_touched(p, (int32_t)y);
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
    row_touched(p, y);
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
    row_touched(p, yi);
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
    struct pen p;
    begin(vm, &p, c, 1);
    p.cost += 8;
    p.per16 = PER16_DOT;
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
    row_touched(p, (int32_t)y);
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
    p.per16 = 0;                    /* each pixel walked is charged */
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
    struct pen p;
    begin(vm, &p, c, 1);
    if (outline) {
        run_fn run = p.pat ? NULL : run_dots;
        p.per16 = PER16_DOT;
        walk_line(&p, v[0], v[1], v[2], v[3], plot_dot, run, NULL);
        walk_line(&p, v[2], v[3], v[4], v[5], plot_dot, run, NULL);
        walk_line(&p, v[4], v[5], v[0], v[1], plot_dot, run, NULL);
        finish(vm, &p);
        return 1;
    }
    if (triangle_near(&p, v)) {
        finish(vm, &p);
        return 1;
    }
    int64_t ymin = v[1], ymax = v[1];
    for (int i = 3; i < 6; i += 2) {
        if (v[i] < ymin) ymin = v[i];
        if (v[i] > ymax) ymax = v[i];
    }
    if (ymin + p.oy < p.cy0)
        ymin = p.cy0 - p.oy;
    if (ymax + p.oy >= p.cy1)
        ymax = p.cy1 - 1 - p.oy;
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
            span(&p, y + p.oy, l + p.ox, r + p.ox);
    }
    finish(vm, &p);
    return 1;
}

NATIVE(tri) { (void)out; return triangle(vm, a, argc, 1, "tri"); }
NATIVE(trifill) { (void)out; return triangle(vm, a, argc, 0, "trifill"); }

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

static int rectangle(struct vm *vm, struct value *a, int argc, int outline,
                     const char *fn)
{
    int64_t b[4];
    if (!box(vm, a, argc, fn, b, FAR))
        return 0;
    COLOUR(4, c, fn);
    struct pen p;
    begin(vm, &p, c, 1);
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
    vm_charge(vm, COST_CALL + (uint32_t)(s.w * s.h) / BYTES_PER_UNIT);
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
    return vm->d.font == 1 ? 2 : 1;
}

/* A glyph wholly inside the clip, unpatterned, k pixels to a dot: the
 * pixels span() would draw, without a clipped call for each. */
static void glyph(struct pen *p, uint16_t g, int32_t x, int32_t y, int k)
{
    int32_t w = p->s.w;
    uint8_t ink = p->c, *row = p->s.px + y * w + x;
    int top = -1, bot = 0;
    uint32_t dots = 0;
    for (int r = 0; r < 5; r++, row += w * k) {
        uint32_t bits = (g >> (12 - r * 3)) & 7;
        if (!bits)
            continue;
        dots += (bits & 1) + ((bits >> 1) & 1) + (bits >> 2);
        if (top < 0)
            top = r;
        bot = r;
        if (k == 1) {
            if (bits & 4) row[0] = ink;
            if (bits & 2) row[1] = ink;
            if (bits & 1) row[2] = ink;
            continue;
        }
        for (int dy = 0; dy < k; dy++) {
            uint8_t *d = row + dy * w;
            for (int c = 0; c < 3; c++, d += k)
                if ((bits >> (2 - c)) & 1)
                    for (int dx = 0; dx < k; dx++)
                        d[dx] = ink;
        }
    }
    if (top >= 0) {
        p->count += dots * (uint32_t)(k * k);
        row_touched(p, y + top * k);
        row_touched(p, y + bot * k + k - 1);
    }
}

/* Draws text at (x, y) in camera space, or with draw 0 only measures it.
 * Returns the x after the last glyph; *widest is the longest line. */
static int64_t text(struct pen *p, const char *s, uint32_t n, int64_t x,
                    int64_t y, int k, int draw, int64_t *widest, int *lines)
{
    int64_t cx = x;
    *widest = 0;
    *lines = 1;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)s[i];
        if (ch == '\n') {
            if (cx - x > *widest)
                *widest = cx - x;
            cx = x;
            y += 6 * k;
            (*lines)++;
            continue;
        }
        if (ch >= 0x80 && ch < 0xc0)
            continue;               /* the rest of a UTF-8 character */
        if (ch < 32 || ch > 126)
            ch = ch >= 0x80 ? '?' : ' ';
        uint16_t g = font_small[ch - 32];
        if (draw && g)
            p->cost += k == 1 ? COST_GLYPH : COST_BIG_GLYPH;
        if (draw && g && !p->pat && cx + p->ox >= p->cx0 &&
            y + p->oy >= p->cy0 && cx + p->ox + 3 * k <= p->cx1 &&
            y + p->oy + 5 * k <= p->cy1) {
            glyph(p, g, (int32_t)(cx + p->ox), (int32_t)(y + p->oy), k);
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

/* print's and textw's text. Static, because the UI thread's stack is
 * small; there is only ever one engine. */
static char text_buf[1024];

/* print(str), print(str, c), print(str, x, y) or print(str, x, y, c);
 * str arrives as pieces. Returns the x after the last glyph. */
NATIVE(print)
{
    char *buf = text_buf;
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
    int64_t end = text(&p, buf, n, x, y, k, 1, &widest, &lines);
    finish(vm, &p);
    vm->d.cur_x = x;
    vm->d.cur_y = (int32_t)clampc((int64_t)y + lines * 6 * k);
    *out = mk(T_INT, (int32_t)clampc(end));
    return 1;
}

NATIVE(textw)
{
    char *buf = text_buf;
    uint32_t n = gather(vm, a, argc, buf, sizeof(text_buf));
    int64_t widest;
    int lines;
    text(NULL, buf, n, 0, 0, scale_of(vm), 0, &widest, &lines);
    *out = mk(T_INT, (int32_t)widest);
    return 1;
}

NATIVE(font)
{
    (void)out;
    WHOLE(0, 0, f, "font");
    if (f < 0 || f > 2)
        return vm_fail(vm, "font() needs FONT.SMALL, FONT.BIG or FONT.SYS",
                       "", "");
    /* FONT.SYS is the player's own font, drawn by the shell; until it is
     * wired, it falls back to the small font. */
    vm->d.font = (uint8_t)(f == 2 ? 0 : f);
    return 1;
}

/* ---- sprites and images ------------------------------------------------ */

/* Copies a w x h rectangle of src, scaled to dw x dh and flipped, to
 * (dx, dy) in camera space, skipping transparent colours. Coordinates are
 * clamped first, so the loops run in 32 bits: the 5G's ARM7 does 64-bit
 * arithmetic slowly. Unscaled -- every sprite, tile and blit -- is a
 * straight copy; scaled steps the source column by the whole and the
 * remainder of w / dw, which gives the same columns as dividing. */
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
        if (v1 > v0)
            p->cost += (uint32_t)((v1 - v0) * (u1 - u0)) * PER16_COPY / 16;
        int32_t ua = (int32_t)u0, ub = (int32_t)u1;
        for (int32_t v = (int32_t)v0; v < (int32_t)v1; v++) {
            int32_t yy = (int32_t)sy + (fy ? (int32_t)h - 1 - v : v);
            if (yy < 0 || yy >= sh_img)
                continue;
            const uint8_t *srow = src + yy * sw_img;
            uint8_t *drow = p->s.px + ((int32_t)dy + v) * stride + (int32_t)dx;
            uint32_t n = 0;
            if (!fx) {
                const uint8_t *sp = srow + (int32_t)sx;
                for (int32_t u = ua; u < ub; u++) {
                    uint32_t c = xl[sp[u]];
                    if (c & XLAT_CLEAR)
                        continue;
                    drow[u] = (uint8_t)c;
                    n++;
                }
            } else {
                const uint8_t *sp = srow + (int32_t)sx + (int32_t)w - 1;
                for (int32_t u = ua; u < ub; u++) {
                    uint32_t c = xl[sp[-u]];
                    if (c & XLAT_CLEAR)
                        continue;
                    drow[u] = (uint8_t)c;
                    n++;
                }
            }
            if (n) {
                p->count += n;
                row_touched(p, (int32_t)dy + v);
            }
        }
        return;
    }

    if (u1 > u0 && v1 > v0)
        p->cost += (uint32_t)((v1 - v0) * (u1 - u0)) * PER16_SCALED / 16;
    int32_t whole = (int32_t)(w / dw), part = (int32_t)(w % dw);
    int32_t tx0 = (int32_t)(u0 * w / dw), rem0 = (int32_t)(u0 * w % dw);
    int32_t W = (int32_t)w, DW = (int32_t)dw, SX = (int32_t)sx;
    /* The source row steps as the column does: v * h / dh, divided once */
    int32_t hwhole = (int32_t)(h / dh), hpart = (int32_t)(h % dh);
    int32_t ty = (int32_t)(v0 * h / dh), trem = (int32_t)(v0 * h % dh);
    int32_t DH = (int32_t)dh;
    int inside = SX >= 0 && W <= sw_img - SX;
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
            /* Every source column is in the image: no test per pixel */
            const uint8_t *sp = srow + SX + (fx ? W - 1 : 0);
            int32_t dir = fx ? -1 : 1;
            for (int32_t u = (int32_t)u0; u < (int32_t)u1; u++) {
                uint32_t c = xl[sp[dir * tx]];
                tx += whole;
                rem += part;
                if (rem >= DW) {
                    rem -= DW;
                    tx++;
                }
                if (!(c & XLAT_CLEAR)) {
                    drow[u] = (uint8_t)c;
                    n++;
                }
            }
            if (n) {
                p->count += n;
                row_touched(p, (int32_t)(dy + v));
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
            row_touched(p, (int32_t)(dy + v));
        }
    }
}

/* An unflipped 8x8 tile of the sheet, landing wholly inside the clip at
 * (x, y) in surface coordinates: copy_rect's pixels, without its setup. */
static void tile8(struct pen *p, const uint8_t *src, int32_t x, int32_t y)
{
    p->cost += 64 * PER16_TILE / 16;
    const uint16_t *xl = p->xl;
    int32_t w = p->s.w;
    uint8_t *d = p->s.px + y * w + x;
    int top = -1, bot = 0;
    uint32_t total = 0;
    for (int r = 0; r < 8; r++, src += SHEET_W, d += w) {
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
        row_touched(p, y + top);
        row_touched(p, y + bot);
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
    if (n < 0 || n > 255)
        return 1;
    int fx = flag(vm, a, argc, 5), fy = flag(vm, a, argc, 6);
    struct pen p;
    int32_t tx, ty;
    begin(vm, &p, 0, 0);
    p.per16 = 0;                    /* tile8 and copy_rect charge */
    if (w == 8 && h == 8 && !fx && !fy && tile_inside(&p, x, y, &tx, &ty)) {
        tile8(&p, MEM(vm, vm->sheet) + (n >> 4) * 8 * SHEET_W + (n & 15) * 8,
              tx, ty);
    } else {
        copy_rect(&p, MEM(vm, vm->sheet), SHEET_W, SHEET_H, (n & 15) * 8,
                  (n >> 4) * 8, w, h, x, y, w, h, fx, fy);
    }
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
    struct pen p;
    begin(vm, &p, 0, 0);
    p.per16 = 0;                    /* copy_rect charges what it reads */
    copy_rect(&p, MEM(vm, vm->sheet), SHEET_W, SHEET_H, sx, sy, sw, sh,
              dx, dy, dw, dh, flag(vm, a, argc, 8), flag(vm, a, argc, 9));
    finish(vm, &p);
    return 1;
}

NATIVE(sget)
{
    WHOLE(0, 0, x, "sget");
    WHOLE(1, 0, y, "sget");
    uint8_t *sheet = MEM(vm, vm->sheet);
    *out = mk(T_INT, (uint32_t)x < SHEET_W && (uint32_t)y < SHEET_H
              ? sheet[y * SHEET_W + x] : 0);
    return 1;
}

NATIVE(sset)
{
    (void)out;
    WHOLE(0, 0, x, "sset");
    WHOLE(1, 0, y, "sset");
    WHOLE(2, vm->d.pen, c, "sset");
    uint8_t *sheet = MEM(vm, vm->sheet);
    if ((uint32_t)x < SHEET_W && (uint32_t)y < SHEET_H)
        sheet[y * SHEET_W + x] = (uint8_t)c;
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

/* fset(n, v) all eight flags; fset(n, f, v) flag f. */
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
    WHOLE(1, 0, f, "fset");
    if ((uint32_t)f < 8) {
        if (vm_truthy(vm, a[2]))
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
    if (w < 1 || h < 1 || w > 4096 || h > 4096)
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

/* blit(i, x, y, [fx], [fy]): all of image i, or of the screen or a
 * border. */
NATIVE(blit)
{
    (void)out;
    struct surf s;
    if (!surf_of(vm, a[0], &s, "blit"))
        return 0;
    WHOLE(1, 0, x, "blit");
    WHOLE(2, 0, y, "blit");
    struct pen p;
    begin(vm, &p, 0, 0);
    p.per16 = 0;                    /* copy_rect charges what it reads */
    copy_rect(&p, s.px, s.w, s.h, 0, 0, s.w, s.h, x, y, s.w, s.h,
              flag(vm, a, argc, 3), flag(vm, a, argc, 4));
    finish(vm, &p);
    return 1;
}

/* blitpart(i, sx, sy, w, h, x, y, [fx], [fy]) */
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
    struct pen p;
    begin(vm, &p, 0, 0);
    p.per16 = 0;                    /* copy_rect charges what it reads */
    copy_rect(&p, s.px, s.w, s.h, sx, sy, w, h, x, y, w, h,
              flag(vm, a, argc, 7), flag(vm, a, argc, 8));
    finish(vm, &p);
    return 1;
}

/* ---- the map ------------------------------------------------------------ */

/* map(celx, cely, sx, sy, celw, celh, [layers]): each cell's sprite, if
 * not 0 and its flags include all of layers. */
NATIVE(map)
{
    (void)out;
    WHOLE(0, 0, cx, "map");
    WHOLE(1, 0, cy, "map");
    WHOLE(2, 0, sx, "map");
    WHOLE(3, 0, sy, "map");
    WHOLE(4, vm->map_w, cw, "map");
    WHOLE(5, vm->map_h, ch, "map");
    WHOLE(6, 0, layers, "map");
    uint8_t *map = MEM(vm, vm->map), *flags = MEM(vm, vm->flags);
    struct pen p;
    begin(vm, &p, 0, 0);
    p.per16 = 0;                    /* tile8 and copy_rect charge */
    int64_t j0 = cy < 0 ? -(int64_t)cy : 0, i0 = cx < 0 ? -(int64_t)cx : 0;
    const int32_t mw = vm->map_w, mh = vm->map_h;
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
                          px, py);
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
                      (n & 15) * 8, tx, ty);
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

NATIVE(loadmap)
{
    (void)out;
    WHOLE(0, 0, n, "loadmap");
    if (n < 0 || (uint32_t)n >= vm->nmaps)
        return vm_fail(vm, "loadmap() needs one of the cart's maps, MAP.name",
                       "", "");
    if (!use_map(vm, (uint32_t)n))
        return 0;
    vm_charge(vm, COST_CALL + (uint32_t)vm->map_w * vm->map_h / BYTES_PER_UNIT);
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
                        : mk(T_INT, (int32_t)le32(p + 3));
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

NATIVE(camera)
{
    (void)out;
    WHOLE(0, 0, x, "camera");
    WHOLE(1, 0, y, "camera");
    vm->d.cam_x = x;
    vm->d.cam_y = y;
    return 1;
}

/* clip(x, y, w, h), in target pixels; clip() the whole target. */
NATIVE(clip)
{
    (void)out;
    if (argc == 0) {
        full_clip(vm);
        return 1;
    }
    WHOLE(0, 0, x, "clip");
    WHOLE(1, 0, y, "clip");
    WHOLE(2, 0, w, "clip");
    WHOLE(3, 0, h, "clip");
    vm->d.clip_x0 = x;
    vm->d.clip_y0 = y;
    vm->d.clip_x1 = (int32_t)clampc((int64_t)x + (w < 0 ? 0 : w));
    vm->d.clip_y1 = (int32_t)clampc((int64_t)y + (h < 0 ? 0 : h));
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
    case T_I32: *len = I32(vm, v)->len; *size = 4; return 1;
    case T_ARR: *len = ARR(vm, v)->len; *size = sizeof(struct value);
                return 1;
    }
    return bad(vm, fn, "() needs an array or a typed array, not a ", v);
}

static void *elems(struct vm *vm, struct value v)
{
    return v.tag == T_U8 ? (void *)U8A(vm, v)->d
         : v.tag == T_I32 ? (void *)I32(vm, v)->d
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
    else if (a[0].tag == T_I32) {
        int32_t *d = I32(vm, a[0])->d + i, x = to_int(v);
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
