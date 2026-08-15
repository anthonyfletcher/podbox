/* pfgeom -- checks the carousel's occlusion cull, on the host.
 *
 * apps-ipod/screens/covers/carousel.c skips drawing the parts of a cover that
 * a nearer one already hides. That is a claim about projection geometry, and
 * the cost of getting it wrong is a stripe of stale framebuffer that no later
 * pass repairs -- on one album shape, at one point in one scroll, on hardware.
 *
 * So the claim is checked rather than argued about. This mirrors the carousel's
 * projection, its cull and its draw order, renders each frame twice -- once
 * with the cull and once without -- and compares which slide ended up on top of
 * every pixel. The two must agree exactly. It also counts the pixels saved,
 * which is the only reason the cull exists.
 *
 * See tools/pfgeom/README.md. Everything below the CAROUSEL MIRROR banner is a
 * copy of carousel.c and has to be updated with it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))

/* ======================= CAROUSEL MIRROR ==============================
 * Kept identical to apps-ipod/screens/covers/carousel.c.
 * ====================================================================== */

typedef int32_t PFreal;

#define LCD_WIDTH  320
#define LCD_HEIGHT 240

#define PFREAL_SHIFT 10
#define PFREAL_ONE (1 << PFREAL_SHIFT)
#define PFREAL_HALF (PFREAL_ONE >> 1)
#define IANGLE_MAX 1024
#define IANGLE_MASK 1023

#define DISPLAY_HEIGHT (LCD_HEIGHT * 2 / 3)
#define DISPLAY_WIDTH MAX((LCD_HEIGHT / 2), (LCD_WIDTH * 2 / 5))
#define CAM_DIST MAX(MIN(LCD_HEIGHT,LCD_WIDTH),120)
#define CAM_DIST_R (CAM_DIST << PFREAL_SHIFT)
#define DISPLAY_LEFT_R (PFREAL_HALF - LCD_WIDTH * PFREAL_HALF)
#define MAXSLIDE_LEFT_R (PFREAL_HALF - DISPLAY_WIDTH * PFREAL_HALF)

#define ALBUM_COVERS_NUM_SLIDES 4
#define MAX_SLIDES_COUNT 10

#define PF_CULL_MARGIN 1
#define PF_CULL_MIRROR(v) (LCD_WIDTH - 1 - (v))
#define PF_ROW_MARGIN 2

/* the settings the geometry reads */
static int set_center_margin = 0;       /* album covers center margin, 0..80 */
static int set_slide_tuck    = 32;      /* album covers slide tuck,    0..64 */
static int set_parallel      = 1;       /* album covers parallel slides       */

/* the viewport split, from init() */
static int pf_half_height, pf_lower_half;

/* A slide carries its own bitmap size here; on the device that comes from
 * surface(), and covers keep their aspect, so it genuinely varies. */
struct slide_data { int angle; PFreal cx, cy; PFreal distance; int sw, sh; };
struct dim { int width, height; };

static struct slide_data left_slides[MAX_SLIDES_COUNT];
static struct slide_data right_slides[MAX_SLIDES_COUNT];
static struct slide_data center_slide;
static PFreal offsetX, auto_slide_spacing;
static int itilt;
static int fade, step;

static inline PFreal fmul(PFreal a, PFreal b) { return (a*b) >> PFREAL_SHIFT; }
static inline PFreal fmuln(PFreal a, PFreal b, int ps1, int ps2)
{ return ((a >> ps1) * (b >> ps2)) >> (PFREAL_SHIFT - ps1 - ps2); }

static const char clz_lut[16] = { 4,3,2,2,1,1,1,1,0,0,0,0,0,0,0,0 };
static inline int clz(uint32_t v)
{
    int r = 28;
    if (v >= 0x10000) { v >>= 16; r -= 16; }
    if (v & 0xff00)   { v >>= 8;  r -= 8;  }
    if (v & 0xf0)     { v >>= 4;  r -= 4;  }
    return r + clz_lut[v];
}
static inline int allowed_shift(int32_t val)
{ uint32_t uval = val ^ (val >> 31); return clz(uval) - 1; }
static inline PFreal fdiv(PFreal num, PFreal den)
{
    int shift = allowed_shift(num);
    shift = MIN(PFREAL_SHIFT, shift);
    num <<= shift;
    den >>= PFREAL_SHIFT - shift;
    return num / den;
}
#define fmin(a,b) (((a) < (b)) ? (a) : (b))
#define fmax(a,b) (((a) > (b)) ? (a) : (b))
#define fabsr(a) ((a) < 0 ? -(a) : (a))

static const short sin_tab[] = {
        0,   100,   200,   297,   392,   483,   569,   650,
      724,   792,   851,   903,   946,   980,  1004,  1019,
     1024,  1019,  1004,   980,   946,   903,   851,   792,
      724,   650,   569,   483,   392,   297,   200,   100,
        0,  -100,  -200,  -297,  -392,  -483,  -569,  -650,
     -724,  -792,  -851,  -903,  -946,  -980, -1004, -1019,
    -1024, -1019, -1004,  -980,  -946,  -903,  -851,  -792,
     -724,  -650,  -569,  -483,  -392,  -297,  -200,  -100,
        0
};

static inline PFreal fsin(int iangle)
{
    iangle &= IANGLE_MASK;
    int i = (iangle >> 4);
    PFreal p = sin_tab[i], q = sin_tab[i+1], g = (q - p);
    return p + g * (iangle - i*16)/16;
}
static inline PFreal fcos(int iangle) { return fsin(iangle + (IANGLE_MAX >> 2)); }

static struct dim slide_dim;
static struct dim *surface(struct slide_data *s)
{
    slide_dim.width = s->sw;
    slide_dim.height = s->sh;
    return &slide_dim;
}

static void recalc_offsets(void)
{
    PFreal xs = PFREAL_HALF - DISPLAY_WIDTH * PFREAL_HALF;
    PFreal zo;
    PFreal xp = (DISPLAY_WIDTH * PFREAL_HALF - PFREAL_HALF +
                set_center_margin * PFREAL_ONE) - set_slide_tuck * PFREAL_ONE;
    PFreal cosr, sinr;

    itilt = (set_parallel ? 55 : 70) * IANGLE_MAX / 360;
    cosr = fcos(-itilt);
    sinr = fsin(-itilt);
    zo = fmuln(MAXSLIDE_LEFT_R, sinr, PFREAL_SHIFT - 2, 0);
    offsetX = xp - fmul(xs, cosr) + fmuln(xp,
        zo + fmuln(xs, sinr, PFREAL_SHIFT - 2, 0), PFREAL_SHIFT - 2, 0) / CAM_DIST;
    {
        PFreal cx_last;
        if (set_parallel) {
            PFreal edge_r = fdiv(CAM_DIST * fmul(-xs, cosr),
                CAM_DIST_R + zo + fmul(-xs, sinr));
            PFreal target = -DISPLAY_LEFT_R - edge_r;
            cx_last = target + fmuln(target, zo, PFREAL_SHIFT - 2, 0) / CAM_DIST;
        } else
            cx_last = (-DISPLAY_LEFT_R) + fmul(xs, cosr);
        PFreal span = cx_last - offsetX;
        if (span < PFREAL_ONE) span = PFREAL_ONE;
        auto_slide_spacing = span / 2;
    }
}

static void coverflow_idle(void)
{
    int i;
    center_slide.angle = 0; center_slide.cx = 0; center_slide.cy = 0;
    center_slide.distance = 0;
    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        left_slides[i].angle = itilt;
        left_slides[i].cx = -(offsetX + auto_slide_spacing * i);
        left_slides[i].cy = 0; left_slides[i].distance = 0;
        right_slides[i].angle = -itilt;
        right_slides[i].cx = offsetX + auto_slide_spacing * i;
        right_slides[i].cy = 0; right_slides[i].distance = 0;
    }
}

static void coverflow_animate(int s, int tick, int neg, PFreal ftick)
{
    int i;
    center_slide.angle = (s * tick * itilt) >> 16;
    center_slide.cx = -s * fmul(offsetX, ftick);
    center_slide.cy = 0;
    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        left_slides[i].angle = itilt;
        left_slides[i].cx = -(offsetX + auto_slide_spacing * i
                              + s * fmul(auto_slide_spacing, ftick));
        left_slides[i].cy = 0;
        right_slides[i].angle = -itilt;
        right_slides[i].cx = offsetX + auto_slide_spacing * i
                              - s * fmul(auto_slide_spacing, ftick);
        right_slides[i].cy = 0;
    }
    PFreal fneg = (neg * PFREAL_ONE) >> 16;
    if (s > 0) {
        right_slides[0].angle = -(neg * itilt) >> 16;
        right_slides[0].cx = fmul(offsetX, fneg);
    } else {
        left_slides[0].angle = (neg * itilt) >> 16;
        left_slides[0].cx = -fmul(offsetX, fneg);
    }
}

static int slide_x_range(struct slide_data *slide, int *x0, int *x1)
{
    struct dim *bmp = surface(slide);
    PFreal cosr, sinr, zo, screen_cx, render_cx, xs, xp;
    int sw, lo, hi;

    if (!bmp || slide->angle > 255 || slide->angle < -255)
        return 0;

    sw = bmp->width;
    cosr = fcos(slide->angle);
    sinr = fsin(slide->angle);
    zo = PFREAL_ONE * slide->distance
       - fmuln(MAXSLIDE_LEFT_R, fabsr(sinr), PFREAL_SHIFT - 2, 0);
    screen_cx = (set_parallel && slide->angle != 0)
        ? fdiv(CAM_DIST * slide->cx, CAM_DIST_R + zo) : 0;
    render_cx = (screen_cx != 0) ? 0 : slide->cx;

    xs = -sw * PFREAL_HALF + PFREAL_HALF;
    xp = fdiv(CAM_DIST * (render_cx + fmul(xs, cosr)),
              CAM_DIST_R + zo + fmul(xs, sinr)) + screen_cx;
    lo = (fmax(DISPLAY_LEFT_R, xp) - DISPLAY_LEFT_R + PFREAL_ONE - 1)
         >> PFREAL_SHIFT;

    xs = sw * PFREAL_HALF;
    xp = fdiv(CAM_DIST * (render_cx + fmul(xs, cosr)),
              CAM_DIST_R + zo + fmul(xs, sinr)) + screen_cx;
    hi = (xp - DISPLAY_LEFT_R + PFREAL_ONE - 1) >> PFREAL_SHIFT;

    if (lo < 0) lo = 0;
    if (hi > LCD_WIDTH) hi = LCD_WIDTH;
    *x0 = lo; *x1 = hi;
    return lo < hi;
}

static int slide_dy_at(struct slide_data *slide, int x)
{
    struct dim *bmp = surface(slide);
    PFreal cosr, sinr, zo, screen_cx, render_cx, xp_local, xsnum, xsden, xs;
    PFreal slide_left;

    if (!bmp)
        return 0;

    cosr = fcos(slide->angle);
    sinr = fsin(slide->angle);
    zo = PFREAL_ONE * slide->distance
       - fmuln(MAXSLIDE_LEFT_R, fabsr(sinr), PFREAL_SHIFT - 2, 0);
    screen_cx = (set_parallel && slide->angle != 0)
        ? fdiv(CAM_DIST * slide->cx, CAM_DIST_R + zo) : 0;
    render_cx = (screen_cx != 0) ? 0 : slide->cx;

    xp_local = (DISPLAY_LEFT_R + x * PFREAL_ONE) - screen_cx;
    xsnum = CAM_DIST * (render_cx - xp_local)
          - fmuln(xp_local, zo, PFREAL_SHIFT - 2, 0);
    xsden = fmuln(xp_local, sinr, PFREAL_SHIFT - 2, 0) - CAM_DIST * cosr;
    xs = fdiv(xsnum, xsden);

    slide_left = -bmp->width * PFREAL_HALF + PFREAL_HALF;
    if (xs < slide_left)
        xs = slide_left;

    return (CAM_DIST_R + zo + fmul(xs, sinr)) / CAM_DIST;
}

/* the every-column version, which is what the sampled one is checked against */
static int exhaustive;

static int slide_covers(struct slide_data *near, struct slide_data *far,
                        int a, int b)
{
    struct dim n, f;
    int mid = a + (b - a) / 2, x;

    n = *surface(near); f = *surface(far);

    if (b < a)
        return 0;
    if (n.height != f.height || near->cy != far->cy)
        return 0;
    if (near->angle == 0 && near->distance == 0)
        return 1;
    if (exhaustive) {
        for (x = a; x <= b; x++)
            if (slide_dy_at(near, x) > slide_dy_at(far, x)) return 0;
        return 1;
    }
    return slide_dy_at(near, a)   <= slide_dy_at(far, a)
        && slide_dy_at(near, mid) <= slide_dy_at(far, mid)
        && slide_dy_at(near, b)   <= slide_dy_at(far, b);
}

static void cull_side(struct slide_data *side, const int *alpha,
                      int center_alpha, int *edge_out, int right)
{
    struct slide_data *cover[ALBUM_COVERS_NUM_SLIDES + 1];
    int cover_lo[ALBUM_COVERS_NUM_SLIDES + 1];
    int cover_hi[ALBUM_COVERS_NUM_SLIDES + 1];
    int n_cover = 0;
    int i, k, x0, x1, lo, hi;
    int edge = 0, span = LCD_WIDTH;

    if (center_alpha > 0 && slide_x_range(&center_slide, &x0, &x1))
    {
        lo = (right ? x0 : LCD_WIDTH - x1) + PF_CULL_MARGIN;
        hi = (right ? x1 : LCD_WIDTH - x0) - PF_CULL_MARGIN;
        if (hi > lo)
        {
            cover[0] = &center_slide;
            cover_lo[0] = lo;
            cover_hi[0] = hi;
            n_cover = 1;
            span = lo;
            edge = hi;
        }
    }

    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++)
    {
        edge_out[i] = right ? 0 : LCD_WIDTH;

        if (alpha[i] <= 0)
            continue;
        if (!slide_x_range(&side[i], &x0, &x1))
            continue;
        lo = right ? x0 : LCD_WIDTH - x1;
        hi = right ? x1 : LCD_WIDTH - x0;

        if (n_cover && lo - PF_CULL_MARGIN >= span)
        {
            int reached = lo - PF_CULL_MARGIN;

            for (k = 0; k < n_cover; k++)
            {
                int last;

                if (cover_hi[k] <= reached)
                    continue;
                if (cover_lo[k] > reached)
                    break;

                last = MIN(cover_hi[k], MIN(hi, edge)) - 1;
                if (last < reached)
                    break;
                if (!slide_covers(cover[k], &side[i],
                                  right ? reached : PF_CULL_MIRROR(last),
                                  right ? last : PF_CULL_MIRROR(reached)))
                    break;
                reached = last + 1;
            }
            if (reached > lo)
                edge_out[i] = right ? reached : LCD_WIDTH - reached;
        }

        lo += PF_CULL_MARGIN;
        hi -= PF_CULL_MARGIN;
        if (hi <= lo)
            continue;

        if (lo > edge)
        {
            n_cover = 0;
            span = lo;
            edge = hi;
        }
        else if (hi > edge)
        {
            edge = hi;
            span = MIN(span, lo);
        }
        else
            continue;

        cover[n_cover] = &side[i];
        cover_lo[n_cover] = lo;
        cover_hi[n_cover] = hi;
        n_cover++;
    }
}

static int slide_voff(int sh, PFreal cy)
{
    int voff = ((pf_half_height + pf_lower_half) - sh) / 2;

    if (cy > 0)
        voff += cy >> PFREAL_SHIFT;
    if (voff > pf_half_height)
        voff = pf_half_height;
    return voff;
}

static void slide_rows(struct slide_data *slide, int *y0, int *y1)
{
    struct dim *bmp = surface(slide);
    int voff, sh;

    if (!bmp)
    {
        *y0 = pf_half_height + pf_lower_half;
        *y1 = 0;
        return;
    }
    sh = bmp->height;
    voff = slide_voff(sh, slide->cy);
    *y0 = MAX(0, voff - PF_ROW_MARGIN);
    *y1 = MIN(pf_half_height + pf_lower_half, voff + sh + PF_ROW_MARGIN);
}

/* carousel.c's cover_rows() without the union against the previous frame --
 * what the clear and the flush are sized from. */
static void cover_rows(int *y0, int *y1)
{
    int lo, hi, a, b, i;

    slide_rows(&center_slide, &lo, &hi);
    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        slide_rows(&left_slides[i], &a, &b);
        lo = MIN(lo, a); hi = MAX(hi, b);
        slide_rows(&right_slides[i], &a, &b);
        lo = MIN(lo, a); hi = MAX(hi, b);
    }
    *y0 = lo; *y1 = hi;
}

/* ======================= END CAROUSEL MIRROR =========================== */

/* Stands in for the framebuffer: which slide wrote each pixel last. Two renders
 * of the same frame have to agree on this everywhere -- the topmost writer is
 * what decides the colour, so anything else is a visible artefact. */
#define FBH 256
static short fb_top[LCD_WIDTH * FBH];
static short fb_ref[LCD_WIDTH * FBH];
static int fb_mark_id;

/* render_slide_clipped()'s column walk, counting and stamping instead of
 * blending. Everything that decides *which* pixels are written is copied; the
 * blend itself is not, because the cull cannot change a colour, only whether a
 * pixel is reached. */
static long render_slide_clipped(struct slide_data *slide, int x_from, int x_to)
{
    struct dim *bmp = surface(slide);
    long px = 0;

    if (!bmp || slide->angle > 255 || slide->angle < -255)
        return 0;
    if (x_from >= x_to)
        return 0;

    const int sw = bmp->width, sh = bmp->height;
    const PFreal slide_left = -sw * PFREAL_HALF + PFREAL_HALF;
    const int w = LCD_WIDTH;

    PFreal cosr = fcos(slide->angle);
    PFreal sinr = fsin(slide->angle);
    PFreal zo = PFREAL_ONE * slide->distance
              - fmuln(MAXSLIDE_LEFT_R, fabsr(sinr), PFREAL_SHIFT - 2, 0);
    PFreal screen_cx = (set_parallel && slide->angle != 0)
        ? fdiv(CAM_DIST * slide->cx, CAM_DIST_R + zo) : 0;
    PFreal render_cx = (screen_cx != 0) ? 0 : slide->cx;

    PFreal xs = slide_left, xsnum, xsnumi, xsden, xsdeni;
    PFreal xp = fdiv(CAM_DIST * (render_cx + fmul(xs, cosr)),
                     (CAM_DIST_R + zo + fmul(xs, sinr)));
    xp += screen_cx;
    int xi = (fmax(DISPLAY_LEFT_R, xp) - DISPLAY_LEFT_R + PFREAL_ONE - 1)
             >> PFREAL_SHIFT;
    xp = DISPLAY_LEFT_R + xi * PFREAL_ONE;
    if (xi >= w)
        return 0;
    PFreal xp_local = xp - screen_cx;
    xsnum = CAM_DIST * (render_cx - xp_local)
          - fmuln(xp_local, zo, PFREAL_SHIFT - 2, 0);
    xsden = fmuln(xp_local, sinr, PFREAL_SHIFT - 2, 0) - CAM_DIST * cosr;
    xs = fdiv(xsnum, xsden);
    xsnumi = -CAM_DIST_R - zo;
    xsdeni = sinr;

    int x, dy = PFREAL_ONE;
    const int half_height = pf_half_height, lower_half = pf_lower_half;
    const int perspective = (zo != 0 || slide->angle != 0);
    int voff = ((half_height + lower_half) - sh) / 2;
    if (slide->cy > 0) voff += slide->cy >> PFREAL_SHIFT;
    if (voff > half_height) voff = half_height;
    const int p_start_upper = (half_height - 1 - voff) * PFREAL_ONE;
    const int p_start_lower = (half_height - voff) * PFREAL_ONE;

    for (x = xi; x < w; x++) {
        if (xs < slide_left) xs = slide_left;
        int column = (unsigned)(xs - slide_left) >> PFREAL_SHIFT;
        if (column >= sw) break;
        if (perspective) dy = (CAM_DIST_R + zo + fmul(xs, sinr)) / CAM_DIST;

        if (x >= x_from && x < x_to) {
            int p = p_start_upper;
            int plim = MAX(0, p - (half_height - 1) * dy);
            int y = half_height - 1;
            while (p >= plim) {
                px++;
                if (y >= 0 && y < FBH) fb_top[y*LCD_WIDTH + x] = fb_mark_id;
                p -= dy; y--;
            }
            p = p_start_lower;
            plim = MIN(sh * PFREAL_ONE, p + lower_half * dy);
            y = half_height;
            while (p < plim) {
                px++;
                if (y >= 0 && y < FBH) fb_top[y*LCD_WIDTH + x] = fb_mark_id;
                p += dy; y++;
            }
        }
        if (perspective) { xsnum += xsnumi; xsden += xsdeni; xs = fdiv(xsnum, xsden); }
        else xs += PFREAL_ONE;
    }
    return px;
}

/* coverflow_render(), with the cull switchable off. */
static long render_frame(int with_cull)
{
    const int n = ALBUM_COVERS_NUM_SLIDES;
    int left_alpha[MAX_SLIDES_COUNT], right_alpha[MAX_SLIDES_COUNT];
    int left_to[MAX_SLIDES_COUNT], right_from[MAX_SLIDES_COUNT];
    int center_alpha = 256;
    int redraw_left_0 = 0, redraw_right_0 = 0;
    int alpha, index;
    long px = 0;

    for (index = 0; index < n; index++)
        left_alpha[index] = right_alpha[index] = 0;

    if (step == 0) {
        for (index = n - 2; index >= 0; index--) {
            alpha = (index < n - 2) ? 256 : 128;
            if (alpha > 0)
                left_alpha[index] = right_alpha[index] = alpha;
        }
    } else {
        PFreal cd = fabsr(center_slide.cx);

        if (step > 0) redraw_right_0 = (fabsr(right_slides[0].cx) < cd);
        else          redraw_left_0  = (fabsr(left_slides[0].cx) < cd);

        alpha = ((step > 0) ? 0 : ((n == 1) ? 256 : 128)) - fade / 2;
        for (index = n - 1; index >= 0; index--) {
            if (alpha > 0 && !(index == 0 && redraw_left_0))
                left_alpha[index] = alpha;
            alpha += 128;
            if (alpha > 256) alpha = 256;
        }
        alpha = ((step > 0) ? ((n == 1) ? 128 : 0) : -128) + fade / 2;
        for (index = n - 1; index >= 0; index--) {
            if (alpha > 0 && !(index == 0 && redraw_right_0))
                right_alpha[index] = alpha;
            alpha += 128;
            if (alpha > 256) alpha = 256;
        }
        if (n <= 2)
            center_alpha = (step > 0) ? 256 - fade / 2 : 128 + fade / 2;
    }

    if (with_cull) {
        cull_side(left_slides, left_alpha, center_alpha, left_to, 0);
        cull_side(right_slides, right_alpha, center_alpha, right_from, 1);
    } else {
        for (index = 0; index < n; index++)
            { left_to[index] = LCD_WIDTH; right_from[index] = 0; }
    }

    for (index = n - 1; index >= 0; index--)
        if (left_alpha[index] > 0) {
            fb_mark_id = 1 + index;
            px += render_slide_clipped(&left_slides[index], 0, left_to[index]);
        }
    for (index = n - 1; index >= 0; index--)
        if (right_alpha[index] > 0) {
            fb_mark_id = 11 + index;
            px += render_slide_clipped(&right_slides[index],
                                       right_from[index], LCD_WIDTH);
        }
    fb_mark_id = 21;
    px += render_slide_clipped(&center_slide, 0, LCD_WIDTH);

    if (redraw_right_0) {
        fb_mark_id = 11;
        px += render_slide_clipped(&right_slides[0], 0, LCD_WIDTH);
    } else if (redraw_left_0) {
        fb_mark_id = 1;
        px += render_slide_clipped(&left_slides[0], 0, LCD_WIDTH);
    }
    return px;
}

static void pose(int dir, int tick)
{
    step = dir;
    if (dir == 0) {
        fade = 256;
        coverflow_idle();
    } else {
        fade = ((dir < 0) ? (65536 - tick) : tick) / 256;
        coverflow_animate(dir, tick, 65536 - tick, (tick * PFREAL_ONE) >> 16);
    }
}

/* Rows the frame in fb_top actually wrote to. */
static void written_rows(int *y0, int *y1)
{
    int y, x, lo = -1, hi = -1;

    for (y = 0; y < FBH; y++)
        for (x = 0; x < LCD_WIDTH; x++)
            if (fb_top[y*LCD_WIDTH + x]) {
                if (lo < 0) lo = y;
                hi = y + 1;
                break;
            }
    *y0 = (lo < 0) ? 0 : lo;
    *y1 = (hi < 0) ? 0 : hi;
}

/* Rows the clear and the flush are sized from must contain every row the render
 * wrote, or a cover comes out with a band of it missing. */
static long long rows_short;
static int rows_over_max;
static long long rows_band, rows_view, rows_frames;

static void check_rows(void)
{
    int by0, by1, wy0, wy1;

    cover_rows(&by0, &by1);
    written_rows(&wy0, &wy1);

    if (wy1 > wy0 && (wy0 < by0 || wy1 > by1)) {
        int over = MAX(by0 - wy0, wy1 - by1);
        rows_short++;
        if (over > rows_over_max) rows_over_max = over;
    }
    rows_band += (by1 > by0) ? by1 - by0 : 0;
    rows_view += pf_half_height + pf_lower_half;
    rows_frames++;
}

/* One frame, both ways round. Returns pixels the cull saved; *bad gets the
 * count of pixels the two renders disagree on, which must be zero. */
static long check_frame(int dir, int tick, long *plain, long long *bad)
{
    long culled;
    int i;

    pose(dir, tick);
    memset(fb_top, 0, sizeof fb_top);
    *plain = render_frame(0);
    memcpy(fb_ref, fb_top, sizeof fb_top);

    pose(dir, tick);
    memset(fb_top, 0, sizeof fb_top);
    culled = render_frame(1);
    check_rows();

    for (i = 0; i < LCD_WIDTH * FBH; i++)
        if (fb_ref[i] != fb_top[i]) (*bad)++;

    return *plain - culled;
}

int main(int argc, char **argv)
{
    /* Cover shapes: covers are scaled into DISPLAY_WIDTH x DISPLAY_HEIGHT
     * keeping their aspect, so a square one comes out 128x128 and anything
     * else does not -- which is the case the cull's height test is for. */
    static const int SW_SET[] = { 128, 128, 128, 100,  80, 128 };
    static const int SH_SET[] = { 128,  64,  96, 128, 160,  40 };
    static const int TUCK[]   = { 0, 8, 32, 56 };          /* setting: 0..64 */
    static const int MARGIN[] = { 0, 12, 28 };             /* setting: 0..80 */
    static const int SBS[]    = { 0, 20, 40 };             /* status bar      */
    static const int BAND[]   = { 0, 30, 49, 70 };         /* caption band    */
    const int TICKS = 40;

    long long saved = 0, plain_total = 0, bad = 0;
    long long frames = 0, configs = 0;
    unsigned seed = 12345;
    int verbose = (argc > 1 && !strcmp(argv[1], "-v"));

    exhaustive = (argc > 1 && !strcmp(argv[1], "-x"));
    if (exhaustive)
        printf("comparing every column rather than sampling three\n");

    for (int par = 0; par <= 1; par++)
    for (unsigned t = 0; t < sizeof TUCK / sizeof *TUCK; t++)
    for (unsigned g = 0; g < sizeof MARGIN / sizeof *MARGIN; g++)
    for (unsigned s = 0; s < sizeof SBS / sizeof *SBS; s++)
    for (unsigned b = 0; b < sizeof BAND / sizeof *BAND; b++)
    for (int shapes = 0; shapes < 3; shapes++)
    {
        int draw_h = (LCD_HEIGHT - SBS[s]) - BAND[b];
        if (draw_h < 40)
            continue;

        set_parallel = par;
        set_slide_tuck = TUCK[t];
        set_center_margin = MARGIN[g];
        pf_half_height = draw_h / 2;
        pf_lower_half = draw_h - pf_half_height;
        recalc_offsets();

        for (int k = 0; k < ALBUM_COVERS_NUM_SLIDES; k++) {
            int il = 0, ir = 0;
            if (shapes == 1) il = ir = 1;               /* all the same, wide */
            if (shapes == 2) {                          /* a mixture */
                seed = seed * 1103515245u + 12345u; il = (seed >> 16) % 6;
                seed = seed * 1103515245u + 12345u; ir = (seed >> 16) % 6;
            }
            left_slides[k].sw  = SW_SET[il]; left_slides[k].sh  = SH_SET[il];
            right_slides[k].sw = SW_SET[ir]; right_slides[k].sh = SH_SET[ir];
        }
        {
            int ic = (shapes == 2) ? ((seed >> 8) % 6) : (shapes == 1);
            center_slide.sw = SW_SET[ic]; center_slide.sh = SH_SET[ic];
        }
        configs++;

        for (int dir = 1; dir >= -1; dir--) {
            int n = (dir == 0) ? 1 : TICKS;
            for (int k = 0; k < n; k++) {
                long plain, was_bad = bad;
                int tick = (dir == 0) ? 0 : (int)((long long)k * 65536 / TICKS);

                saved += check_frame(dir, tick, &plain, &bad);
                plain_total += plain;
                frames++;
                if (bad != was_bad || verbose)
                    printf("%s par=%d tuck=%d margin=%d sbs=%d band=%d "
                           "shapes=%d dir=%d tick=%d: %lld stale pixels\n",
                           bad != was_bad ? "ARTEFACT" : "ok", par, TUCK[t],
                           MARGIN[g], SBS[s], BAND[b], shapes, dir, tick,
                           bad - was_bad);
            }
        }
    }

    printf("\n%lld configurations, %lld frames\n", configs, frames);
    printf("unculled  %lld pixels/frame\n", plain_total / frames);
    printf("culled    %lld pixels/frame  (%lld%% less)\n",
           (plain_total - saved) / frames, saved * 100 / plain_total);
    printf("stale pixels: %lld%s\n", bad, bad ? "  *** FAILED ***" : "  (pass)");

    /* The clear and the flush are sized from cover_rows() rather than the whole
     * viewport, so that band has to contain everything the render wrote. */
    printf("\ncover band  %lld%% of the drawable height "
           "(what the clear and flush cost instead of all of it)\n",
           rows_band * 100 / rows_view);
    printf("frames written outside the band: %lld, worst overshoot %d rows%s\n",
           rows_short, rows_over_max,
           rows_short ? "  *** FAILED ***" : "  (pass)");

    return (bad || rows_short) ? 1 : 0;
}
