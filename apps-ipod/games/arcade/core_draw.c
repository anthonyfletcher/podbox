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
 *   arguments      coordinates, colours, flags
 *   pens           one call's clip, colours and pattern; spans; the charge
 *   shapes         circles and ovals as spans, filled or outlined
 *   lines          line, and tline, which steps the same way
 *   triangles, rectangles, text
 *   sprites        spr, sspr, sget/sset, fget/fset, images
 *   the map
 *   state          camera, clip, colour, cursor, palettes, target
 *   the host's view of the screen
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"
#include "games/arcade/core_font.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)
#define FAR (1 << 20)

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
}

static void reshow(struct draw *d)
{
    for (int i = 0; i < 256; i++)
        d->shown[i] = d->rgb[d->spal[i]];
    d->palette_changed = 1;
}

static void full_clip(struct vm *vm);

int core_draw_init(struct vm *vm)
{
    struct draw *d = &vm->d;
    if (!(vm->screen = perm_alloc(vm, SCREEN_W * SCREEN_H)) ||
        !(vm->sheet = perm_alloc(vm, SHEET_W * SHEET_H)) ||
        !(vm->flags = perm_alloc(vm, 256)) ||
        !(vm->map = perm_alloc(vm, MAP_W * MAP_H)))
        return 0;
    memset(d, 0, sizeof(*d));
    d->target = mk(T_INT, 0);
    d->pen = 6;
    memcpy(d->rgb, default_rgb, sizeof(default_rgb));
    reset_palettes(d);
    reshow(d);
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

/* Argument i as a whole number, def when missing or null. */
static int whole(struct vm *vm, struct value *a, int argc, int i, int32_t def,
                 const char *fn, int32_t *out)
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
static int colour(struct vm *vm, struct value *a, int argc, int i,
                  const char *fn, uint32_t *c)
{
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

struct surf
{
    uint8_t *px;
    int32_t w, h;
    int is_screen;
};

static int surf_of(struct vm *vm, struct value t, struct surf *s,
                   const char *fn)
{
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
    uint32_t count;
    int32_t lo, hi;
    const struct draw *d;
};

static void begin(struct vm *vm, struct pen *p, uint32_t c, int patterned)
{
    struct draw *d = &vm->d;
    surf_of(vm, d->target, &p->s, "draw");
    p->cx0 = d->clip_x0 < 0 ? 0 : d->clip_x0;
    p->cy0 = d->clip_y0 < 0 ? 0 : d->clip_y0;
    p->cx1 = d->clip_x1 > p->s.w ? p->s.w : d->clip_x1;
    p->cy1 = d->clip_y1 > p->s.h ? p->s.h : d->clip_y1;
    p->ox = -(int64_t)d->cam_x;
    p->oy = -(int64_t)d->cam_y;
    p->c = d->dpal[c & 0xff];
    p->pat = patterned ? d->fill : 0;
    p->alt_on = p->pat && ((c >> 8) & 0xff);
    p->alt = d->dpal[(c >> 8) & 0xff];
    p->count = 0;
    p->lo = 0x7fffffff;
    p->hi = -1;
    p->d = d;
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
    vm_charge(vm, p->count / PIXELS_PER_UNIT);
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
    if (!p->pat)
        memset(row + x0, p->c, (size_t)(x1 - x0 + 1));
    else {
        int shift = 12 - ((int)(y & 3) << 2);
        for (int64_t x = x0; x <= x1; x++) {
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

static int64_t floor_div(int64_t n, int64_t d)
{
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
    for (int64_t y = y0; y <= y1; y++) {
        int64_t l, r, ul, ur, dl, dr;
        if (!shape_row(s, y, &l, &r))
            continue;
        int64_t sy = y + p->oy;
        if (!outline) {
            span(p, sy, l + p->ox, r + p->ox);
            continue;
        }
        int up = shape_row(s, y - 1, &ul, &ur);
        int dn = shape_row(s, y + 1, &dl, &dr);
        if (!up || !dn) {
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

static void walk_line(struct pen *p, int64_t x0, int64_t y0, int64_t x1,
                      int64_t y1, plot_fn plot, void *ctx)
{
    x0 = clampc(x0) + p->ox;
    y0 = clampc(y0) + p->oy;
    x1 = clampc(x1) + p->ox;
    y1 = clampc(y1) + p->oy;
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
        for (i = i0; i <= i1; i++, step_next(&st))
            plot(p, ctx, x0 + i * sx, y0 + st.q, i);
    } else {
        int sy = dy < 0 ? -1 : 1;
        if (!visible(y0, sy, ady, p->cy0, p->cy1, &i0, &i1))
            return;
        struct stepper st;
        step_init(&st, i0, dx, ady);
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
    walk_line(&p, x0, y0, x1, y1, plot_dot, NULL);
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
    struct tline_ctx *t = ctx;
    int64_t mx = t->mx + i * t->mdx, my = t->my + i * t->mdy;
    int64_t cx = mx >> 16, cy = my >> 16;
    if (cx < 0 || cy < 0 || cx >= MAP_W || cy >= MAP_H)
        return;
    uint8_t *map = MEM(t->vm, t->vm->map), *sheet = MEM(t->vm, t->vm->sheet);
    uint32_t n = map[cy * MAP_W + cx];
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

static int real(struct vm *vm, struct value *a, int argc, int i, int64_t def,
                const char *fn, int64_t *out)
{
    if (i >= argc || a[i].tag == T_NULL) {
        *out = def;
        return 1;
    }
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
    walk_line(&p, x0, y0, x1, y1, plot_map, &t);
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
        walk_line(&p, v[0], v[1], v[2], v[3], plot_dot, NULL);
        walk_line(&p, v[2], v[3], v[4], v[5], plot_dot, NULL);
        walk_line(&p, v[4], v[5], v[0], v[1], plot_dot, NULL);
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
    struct pen p;
    begin(vm, &p, c, 1);
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
    vm_charge(vm, (uint32_t)(s.w * s.h) / PIXELS_PER_UNIT);
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
        if (draw && g) {
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
    const uint8_t *transparent = p->d->transparent, *dpal = p->d->dpal;
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
                    uint8_t c = sp[u];
                    if (transparent[c >> 3] & (1 << (c & 7)))
                        continue;
                    drow[u] = dpal[c];
                    n++;
                }
            } else {
                const uint8_t *sp = srow + (int32_t)sx + (int32_t)w - 1;
                for (int32_t u = ua; u < ub; u++) {
                    uint8_t c = sp[-u];
                    if (transparent[c >> 3] & (1 << (c & 7)))
                        continue;
                    drow[u] = dpal[c];
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

    int32_t whole = (int32_t)(w / dw), part = (int32_t)(w % dw);
    int32_t tx0 = (int32_t)(u0 * w / dw), rem0 = (int32_t)(u0 * w % dw);
    int32_t W = (int32_t)w, DW = (int32_t)dw, SX = (int32_t)sx;
    for (int64_t v = v0; v < v1; v++) {
        int32_t ty = (int32_t)(v * h / dh);
        int32_t yy = (int32_t)sy + (fy ? (int32_t)h - 1 - ty : ty);
        if (yy < 0 || yy >= sh_img)
            continue;
        const uint8_t *srow = src + yy * sw_img;
        uint8_t *drow = p->s.px + ((int32_t)dy + (int32_t)v) * stride
                        + (int32_t)dx;
        int32_t tx = tx0, rem = rem0;
        uint32_t n = 0;
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
            uint8_t c = srow[xx];
            if (transparent[c >> 3] & (1 << (c & 7)))
                continue;
            drow[u] = dpal[c];
            n++;
        }
        if (n) {
            p->count += n;
            row_touched(p, (int32_t)(dy + v));
        }
    }
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
    struct pen p;
    begin(vm, &p, 0, 0);
    copy_rect(&p, MEM(vm, vm->sheet), SHEET_W, SHEET_H, (n & 15) * 8,
              (n >> 4) * 8, w, h, x, y, w, h, flag(vm, a, argc, 5),
              flag(vm, a, argc, 6));
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
    vm_charge(vm, (uint32_t)(w * h) / PIXELS_PER_UNIT);
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
    WHOLE(4, MAP_W, cw, "map");
    WHOLE(5, MAP_H, ch, "map");
    WHOLE(6, 0, layers, "map");
    uint8_t *map = MEM(vm, vm->map), *flags = MEM(vm, vm->flags);
    struct pen p;
    begin(vm, &p, 0, 0);
    int64_t j0 = cy < 0 ? -(int64_t)cy : 0, i0 = cx < 0 ? -(int64_t)cx : 0;
    int64_t j1 = (int64_t)MAP_H - cy < ch ? (int64_t)MAP_H - cy : ch;
    int64_t i1 = (int64_t)MAP_W - cx < cw ? (int64_t)MAP_W - cx : cw;
    for (int64_t j = j0; j < j1; j++) {
        int64_t py = (int64_t)sy + j * 8 + p.oy;
        if (py + 8 <= p.cy0 || py >= p.cy1)
            continue;
        for (int64_t i = i0; i < i1; i++) {
            int64_t px = (int64_t)sx + i * 8 + p.ox;
            if (px + 8 <= p.cx0 || px >= p.cx1)
                continue;
            uint8_t n = map[(cy + j) * MAP_W + cx + i];
            if (!n || (layers && (flags[n] & layers) != layers))
                continue;
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
    *out = mk(T_INT, (uint32_t)x < MAP_W && (uint32_t)y < MAP_H
              ? map[y * MAP_W + x] : 0);
    return 1;
}

NATIVE(mset)
{
    (void)out;
    WHOLE(0, 0, x, "mset");
    WHOLE(1, 0, y, "mset");
    WHOLE(2, 0, v, "mset");
    uint8_t *map = MEM(vm, vm->map);
    if ((uint32_t)x < MAP_W && (uint32_t)y < MAP_H)
        map[y * MAP_W + x] = (uint8_t)v;
    return 1;
}

NATIVE(mapw)
{
    (void)vm; (void)a; (void)argc;
    *out = mk(T_INT, MAP_W);
    return 1;
}

NATIVE(maph)
{
    (void)vm; (void)a; (void)argc;
    *out = mk(T_INT, MAP_H);
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
    } else
        vm->d.dpal[c0 & 0xff] = (uint8_t)c1;
    return 1;
}

NATIVE(palt)
{
    (void)out;
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
