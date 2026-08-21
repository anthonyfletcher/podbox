/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * The deck's drawing toolkit: gradients, anti-aliased shapes, vector
 * numerals and the text helpers the cards are assembled from.
 *
 * Everything here blends against the background ANALYTICALLY. The card's
 * background is a vertical gradient described by five colours, so the colour
 * under any pixel is a function of its row -- no framebuffer read, no
 * read-modify-write, and anti-aliasing that costs one blend per pixel. That
 * is the reason the deck can afford smooth shapes on a 75 MHz PortalPlayer,
 * and the reason a bitmap background would be a rewrite rather than a swap:
 * every routine below takes a theme and asks it what is underneath.
 *
 * The drawing knows nothing about cards or statistics: it is handed
 * coordinates and colours and puts pixels down. The animation section at the
 * end is the exception, and owns the wheel -- how much movement turns a card,
 * and when that is worth abandoning an animation for. It lives with the
 * animations because they are what reads the keypad between frames, and the
 * count has to be the same one the deck loop navigates by.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <string-extra.h>
#include "config.h"
#include "kernel.h"
#include "rbpaths.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "pv_paint.h"

#define PV_W LCD_WIDTH
#define PV_H LCD_HEIGHT

/* Sub-pixel resolution for the anti-aliased shapes. Coverage is derived from
 * the distance to a segment measured in these units, so 16 buys a sixteenth
 * of a pixel of precision -- past what the eye resolves at this size. */
#define SUBPX 16

/* The deck's own font. Bigger and better proportioned than the system fixed
 * font, which is what the numerals are set against. */
#define PV_BODY_FONT_PATH ROCKBOX_DIR "/fonts/15-Adobe-Helvetica.fnt"

static int body_font = FONT_SYSFIXED;
static bool font_loaded;

void pv_paint_init(void)
{
    int id;

    if (font_loaded)
        return;

    /* A missing font is not a failure worth reporting: the system font is
     * uglier and everything still lays out. */
    id = font_load(PV_BODY_FONT_PATH);
    if (id >= 0)
    {
        body_font = id;
        font_loaded = true;
    }
}

void pv_paint_done(void)
{
    if (font_loaded)
    {
        font_unload(body_font);
        font_loaded = false;
    }
    body_font = FONT_SYSFIXED;
}

int pv_body_font(void)
{
    return body_font;
}

/* ---------------------------------------------------------------- colour */

unsigned pv_blend(unsigned a, unsigned b, int t)
{
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int it = 256 - t;
    int r  = (ar * it + br * t) >> 8;
    int g  = (ag * it + bg * t) >> 8;
    int bl = (ab * it + bb * t) >> 8;

    return (unsigned)((r << 11) | (g << 5) | bl);
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

unsigned pv_grad_at(const struct pv_theme *th, int y)
{
    int t = (clampi(y, 0, PV_H - 1) * 256) / (PV_H - 1);
    return pv_blend(th->bg0, th->bg1, t);
}

void pv_fill(const struct pv_theme *th)
{
    lcd_set_drawmode(DRMODE_SOLID);
    for (int y = 0; y < PV_H; y++)
    {
        lcd_set_foreground(pv_grad_at(th, y));
        lcd_hline(0, PV_W - 1, y);
    }
}

void pv_band(const struct pv_theme *th, int y0, int h)
{
    lcd_set_drawmode(DRMODE_SOLID);
    for (int y = y0; y < y0 + h; y++)
    {
        if (y < 0 || y >= PV_H)
            continue;
        lcd_set_foreground(pv_grad_at(th, y));
        lcd_hline(0, PV_W - 1, y);
    }
}

/* ---------------------------------------------------------------- shapes */

static int isqrtl(long v)
{
    long x, y;

    if (v <= 0)
        return 0;

    /* Newton, from a deliberately poor starting guess: converges in a handful
     * of iterations and needs no table. */
    x = v;
    y = (x + 1) >> 1;
    while (y < x)
    {
        x = y;
        y = (x + v / x) >> 1;
    }
    return (int)x;
}

/* Squared distance from a point to a segment, all in sub-pixel units.
 *
 * The axis-aligned cases are separated out because they need no projection
 * and therefore no division, and because they are nearly all the work: every
 * stroke of a seven-segment digit is one or the other, as is every bar and
 * underline. ARM has no divide instruction, so each division in here is a
 * library call -- taking them out of the numeral inner loop is worth more
 * than it looks. */
static long seg_dist2(long px, long py, long x1, long y1, long x2, long y2)
{
    long vx, vy, wx, wy, c1, c2, projx, projy, dx, dy, t;

    if (y1 == y2)                       /* horizontal */
    {
        if (x1 > x2) { t = x1; x1 = x2; x2 = t; }
        dx = (px < x1) ? x1 - px : (px > x2) ? px - x2 : 0;
        dy = py - y1;
        return dx * dx + dy * dy;
    }
    if (x1 == x2)                       /* vertical */
    {
        if (y1 > y2) { t = y1; y1 = y2; y2 = t; }
        dy = (py < y1) ? y1 - py : (py > y2) ? py - y2 : 0;
        dx = px - x1;
        return dx * dx + dy * dy;
    }

    vx = x2 - x1; vy = y2 - y1;
    wx = px - x1; wy = py - y1;
    c1 = vx * wx + vy * wy;

    if (c1 <= 0)
    {
        dx = px - x1;
        dy = py - y1;
        return dx * dx + dy * dy;
    }

    c2 = vx * vx + vy * vy;
    if (c2 <= c1)
    {
        dx = px - x2;
        dy = py - y2;
        return dx * dx + dy * dy;
    }

    projx = x1 + (long)(((long long)vx * c1) / c2);
    projy = y1 + (long)(((long long)vy * c1) / c2);
    dx = px - projx;
    dy = py - projy;
    return dx * dx + dy * dy;
}

/* Coverage of a pixel whose nearest edge is d2 (SQUARED, sub-pixel units)
 * from a shape of half-width hw: fully inside, fully outside, or a linear
 * ramp across the one pixel between.
 *
 * Taking a squared distance rather than a distance is the point. Only pixels
 * actually straddling the edge need the square root, and they are a thin
 * outline around a shape that is mostly interior and exterior -- so the
 * Newton iteration, and the several software divisions it costs, runs for a
 * small fraction of the pixels. */
static int coverage_d2(long hw, long d2)
{
    long inner = hw - SUBPX / 2;
    long outer = hw + SUBPX / 2;
    int d;

    if (inner > 0 && d2 <= inner * inner)
        return 256;
    if (d2 >= outer * outer)
        return 0;

    d = isqrtl(d2);
    return (((int)(hw - d) + SUBPX / 2) * 256) / SUBPX;
}

void pv_disc(const struct pv_theme *th, int cx, int cy, int r,
             unsigned col, int alpha)
{
    long rU = (long)r * SUBPX;

    lcd_set_drawmode(DRMODE_SOLID);
    for (int yy = -r - 1; yy <= r + 1; yy++)
    {
        int ay = cy + yy;
        unsigned bg, full;
        long dyU;
        int run;

        if (ay < 0 || ay >= PV_H)
            continue;

        bg  = pv_grad_at(th, ay);
        dyU = (long)yy * SUBPX;

        /* Fully-covered pixels in a row all end up the same colour, because
         * both the background and the fill depend only on the row. So they
         * are drawn as one span rather than one call per pixel -- which for
         * a large disc is nearly all of it. */
        full = pv_blend(bg, col, (256 * alpha) >> 8);
        run = -1;

        for (int xx = -r - 1; xx <= r + 1; xx++)
        {
            long dxU = (long)xx * SUBPX;
            int cov = coverage_d2(rU, dxU * dxU + dyU * dyU);

            cov = (cov * alpha) >> 8;

            if (cov >= 255)
            {
                if (run < 0)
                    run = cx + xx;
                continue;
            }

            if (run >= 0)
            {
                lcd_set_foreground(full);
                lcd_hline(run, cx + xx - 1, ay);
                run = -1;
            }
            if (cov <= 0)
                continue;

            lcd_set_foreground(pv_blend(bg, col, cov));
            lcd_drawpixel(cx + xx, ay);
        }

        if (run >= 0)
        {
            lcd_set_foreground(full);
            lcd_hline(run, cx + r + 1, ay);
        }
    }
}

void pv_capsule(const struct pv_theme *th, int x1, int y1, int x2, int y2,
                int hw, unsigned col)
{
    int minx = (x1 < x2 ? x1 : x2) - hw - 1;
    int maxx = (x1 > x2 ? x1 : x2) + hw + 1;
    int miny = (y1 < y2 ? y1 : y2) - hw - 1;
    int maxy = (y1 > y2 ? y1 : y2) + hw + 1;
    long X1 = (long)x1 * SUBPX, Y1 = (long)y1 * SUBPX;
    long X2 = (long)x2 * SUBPX, Y2 = (long)y2 * SUBPX;
    long hwU = (long)hw * SUBPX;

    lcd_set_drawmode(DRMODE_SOLID);
    for (int ay = miny; ay <= maxy; ay++)
    {
        unsigned bg;
        long pyU;
        int run = -1;

        if (ay < 0 || ay >= PV_H)
            continue;

        bg   = pv_grad_at(th, ay);
        pyU  = (long)ay * SUBPX + SUBPX / 2;

        for (int ax = minx; ax <= maxx; ax++)
        {
            long pxU = (long)ax * SUBPX + SUBPX / 2;
            int cov = coverage_d2(hwU, seg_dist2(pxU, pyU, X1, Y1, X2, Y2));

            if (cov >= 255)
            {
                if (run < 0)
                    run = ax;
                continue;
            }

            if (run >= 0)
            {
                lcd_set_foreground(col);
                lcd_hline(run, ax - 1, ay);
                run = -1;
            }
            if (cov <= 0)
                continue;

            lcd_set_foreground(pv_blend(bg, col, cov));
            lcd_drawpixel(ax, ay);
        }

        if (run >= 0)
        {
            lcd_set_foreground(col);
            lcd_hline(run, maxx, ay);
        }
    }
}

/* -------------------------------------------------------- vector numerals */

/* Segment bits, the usual seven-segment order: a=1 b=2 c=4 d=8 e=16 f=32 g=64 */
static const unsigned char seg_digit[10] =
{
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

/* One glyph, as up to eight capsule segments rendered together: the distance
 * field is the minimum across all of them, so segments meeting at a corner
 * join smoothly instead of overdrawing each other's anti-aliasing. */
static void draw_glyph(const struct pv_theme *th, int gx, int gy, char c,
                       int cw, int ch, int sw)
{
    long X1[8], Y1[8], X2[8], Y2[8];
    int ns = 0;
    int hw = sw / 2;
    long hwU, inner2;
    int wlim;

    if (hw < 1)
        hw = 1;
    hwU = (long)hw * SUBPX;

#define SEG(a, b, cc, d) do { \
        X1[ns] = (long)(a) * SUBPX;  Y1[ns] = (long)(b) * SUBPX; \
        X2[ns] = (long)(cc) * SUBPX; Y2[ns] = (long)(d) * SUBPX; \
        ns++; \
    } while (0)

    if (c >= '0' && c <= '9')
    {
        unsigned char seg = seg_digit[c - '0'];
        int xL = hw, xR = cw - hw;
        int yT = hw, yM = ch / 2, yB = ch - hw;
        int gp = hw;    /* gap at each end, so segments do not touch */

        if (seg & 0x01) SEG(xL + gp, yT,      xR - gp, yT);
        if (seg & 0x02) SEG(xR,      yT + gp, xR,      yM - gp);
        if (seg & 0x04) SEG(xR,      yM + gp, xR,      yB - gp);
        if (seg & 0x08) SEG(xL + gp, yB,      xR - gp, yB);
        if (seg & 0x10) SEG(xL,      yM + gp, xL,      yB - gp);
        if (seg & 0x20) SEG(xL,      yT + gp, xL,      yM - gp);
        if (seg & 0x40) SEG(xL + gp, yM,      xR - gp, yM);
    }
    else if (c == ',')
    {
        int x = hw * 2;
        SEG(x, ch - hw, x - hw, ch - hw + sw);
    }
    else if (c == ':')
    {
        int x = hw;
        SEG(x, ch / 3, x, ch / 3);
        SEG(x, 2 * ch / 3, x, 2 * ch / 3);
    }
    else
    {
        return;
    }
#undef SEG

    /* Inside this, a pixel is solid whatever the other segments say -- which
     * lets the per-segment search stop early instead of measuring all eight. */
    inner2 = (hwU - SUBPX / 2 > 0) ? (hwU - SUBPX / 2) * (hwU - SUBPX / 2) : 0;

    wlim = cw + hw;
    for (int py = 0; py < ch + sw; py++)
    {
        int ay = gy + py;
        unsigned bg, fg;
        int gt, run = -1;
        long pyU;

        if (ay < 0 || ay >= PV_H)
            continue;

        bg  = pv_grad_at(th, ay);
        /* The numerals carry their own vertical gradient, independent of the
         * background's, which is what makes them read as metal rather than
         * as flat colour. */
        gt  = (ch > 1) ? clampi(py * 256 / (ch - 1), 0, 256) : 0;
        fg  = pv_blend(th->num0, th->num1, gt);
        pyU = (long)py * SUBPX + SUBPX / 2;

        for (int px = 0; px < wlim; px++)
        {
            long pxU = (long)px * SUBPX + SUBPX / 2;
            long best = 0x7fffffffL;
            int cov;

            for (int k = 0; k < ns; k++)
            {
                long dist = seg_dist2(pxU, pyU, X1[k], Y1[k], X2[k], Y2[k]);
                if (dist < best)
                {
                    best = dist;
                    if (best <= inner2)
                        break;    /* already solid; no nearer segment matters */
                }
            }

            cov = coverage_d2(hwU, best);

            if (cov >= 255)
            {
                if (run < 0)
                    run = gx + px;
                continue;
            }

            if (run >= 0)
            {
                lcd_set_foreground(fg);
                lcd_hline(run, gx + px - 1, ay);
                run = -1;
            }
            if (cov <= 0)
                continue;

            lcd_set_foreground(pv_blend(bg, fg, cov));
            lcd_drawpixel(gx + px, ay);
        }

        if (run >= 0)
        {
            lcd_set_foreground(fg);
            lcd_hline(run, gx + wlim - 1, ay);
        }
    }
}

/* Punctuation is narrower than a digit, so the string is not simply n*cw. */
static int char_adv(char c, int cw, int sw)
{
    if (c == ',' || c == ':')
        return sw * 3;
    return cw + sw;
}

int pv_number_width(const char *s, int cw, int sw)
{
    int w = 0;

    for (const char *p = s; *p; p++)
        w += char_adv(*p, cw, sw);
    if (w)
        w -= sw;
    return w;
}

void pv_number(const struct pv_theme *th, int cx, int oy, const char *s,
               int cw, int ch, int sw)
{
    int x = cx - pv_number_width(s, cw, sw) / 2;

    for (const char *p = s; *p; p++)
    {
        draw_glyph(th, x, oy, *p, cw, ch, sw);
        x += char_adv(*p, cw, sw);
    }
}

void pv_percent(const struct pv_theme *th, int cx, int cy, int sz)
{
    unsigned col = th->num1;
    int r = sz / 6;
    int hw = sz / 14;
    int x0, y0, x1, y1;

    if (r < 2)
        r = 2;
    if (hw < 2)
        hw = 2;

    x0 = cx - sz / 2;
    y0 = cy - sz / 2;
    x1 = cx + sz / 2;
    y1 = cy + sz / 2;

    pv_capsule(th, x0 + r, y1 - r, x1 - r, y0 + r, hw, col);
    pv_disc(th, x0 + r, y0 + r, r, col, 256);
    pv_disc(th, x1 - r, y1 - r, r, col, 256);
}

/* A crown: three spikes on a rounded base, a bead on each tip and a jewel in
 * the middle.
 *
 * Filled by scanline rather than assembled from capsules, because a crown is
 * a silhouette and an outline of one reads as a zigzag. Each row is the union
 * of up to three spans -- one per spike, merging into a single span below the
 * valleys -- and coverage is how much of a pixel those spans cover, so the
 * sloping edges anti-alias against the gradient the way every other shape
 * here does.
 *
 * The rounded bottom corners are part of that span rather than a capsule laid
 * over it. A second shape drawn across the first would blend its own edge
 * against the gradient over pixels the body has already filled, leaving a
 * dark arc inside a corner that is supposed to be solid.
 */
void pv_crown(const struct pv_theme *th, int cx, int cy, int w,
              unsigned col, unsigned hi)
{
    int h    = w * 66 / 100;
    int half = w / 2;
    int cr   = h / 8;                   /* how round the bottom corners are */
    int tipr = w / 20;
    int x0 = cx - half, x1 = cx + half;
    int ytop = cy - h / 2;
    int ybot = cy + h / 2;
    int yval, dy;
    long X0 = (long)x0 * SUBPX, X1 = (long)x1 * SUBPX;
    long CX = (long)cx * SUBPX;
    long reach = CX - X0;               /* how far a spike spreads, each side */
    long CRU;

    if (tipr < 2)
        tipr = 2;
    if (cr < 3)
        cr = 3;
    CRU = (long)cr * SUBPX;

    /* The spikes take the upper part; what is left below the valleys is solid
     * across, and is the base. */
    yval = ytop + (ybot - ytop) * 62 / 100;
    dy = yval - ytop;
    if (dy < 1)
        dy = 1;

    lcd_set_drawmode(DRMODE_SOLID);

    for (int ay = ytop; ay <= ybot; ay++)
    {
        long sa[3], sb[3];
        long spread, below;
        unsigned bg;
        int n, run = -1;

        if (ay < 0 || ay >= PV_H)
            continue;

        bg = pv_grad_at(th, ay);

        spread = (long)(ay - ytop) * SUBPX * reach / (2 * (long)dy * SUBPX);

        /* The outermost edges reach half a pixel past the tip columns, which
         * is where the outside of those pixels is -- everything here is in
         * pv_disc()'s units, where a coordinate is a pixel's centre. */
        if (2 * spread >= reach)
        {
            sa[0] = X0 - SUBPX / 2; sb[0] = X1 + SUBPX / 2;
            n = 1;
        }
        else
        {
            sa[0] = X0 - SUBPX / 2; sb[0] = X0 + spread;
            sa[1] = CX - spread;    sb[1] = CX + spread;
            sa[2] = X1 - spread;    sb[2] = X1 + SUBPX / 2;
            n = 3;
        }

        below = (long)(ay - (ybot - cr)) * SUBPX;
        if (n == 1 && below > 0)
        {
            long inset;

            if (below > CRU)
                below = CRU;
            inset = CRU - isqrtl(CRU * CRU - below * below);
            sa[0] += inset;
            sb[0] -= inset;
        }

        for (int px = x0 - 1; px <= x1 + 1; px++)
        {
            long pa = (long)px * SUBPX - SUBPX / 2, pb = pa + SUBPX;
            long covered = 0;
            int cov;

            for (int i = 0; i < n; i++)
            {
                long lo = sa[i] > pa ? sa[i] : pa;
                long up = sb[i] < pb ? sb[i] : pb;

                if (up > lo)
                    covered += up - lo;
            }
            cov = (int)(covered * 256 / SUBPX);

            /* Whole pixels in a row are all the same colour, so they go down
             * as one span the way pv_disc() does it. */
            if (cov >= 255)
            {
                if (run < 0)
                    run = px;
                continue;
            }
            if (run >= 0)
            {
                lcd_set_foreground(col);
                lcd_hline(run, px - 1, ay);
                run = -1;
            }
            if (cov <= 0)
                continue;

            lcd_set_foreground(pv_blend(bg, col, cov));
            lcd_drawpixel(px, ay);
        }

        if (run >= 0)
        {
            lcd_set_foreground(col);
            lcd_hline(run, x1 + 1, ay);
        }
    }

    pv_disc(th, x0, ytop, tipr, hi, 256);
    pv_disc(th, cx, ytop, tipr, hi, 256);
    pv_disc(th, x1, ytop, tipr, hi, 256);
}

/* ------------------------------------------------------------------ text */

void pv_text_centre(int y, const char *s, unsigned colour)
{
    int w, h;

    lcd_setfont(body_font);
    lcd_set_drawmode(DRMODE_FG);
    lcd_getstringsize(s, &w, &h);
    lcd_set_foreground(colour);
    lcd_putsxy((PV_W - w) / 2, y, s);
}

void pv_text_centre2(int y, const char *a, unsigned ca,
                     const char *b, unsigned cb)
{
    int wa, wb, h, x;

    lcd_setfont(body_font);
    lcd_set_drawmode(DRMODE_FG);
    lcd_getstringsize(a, &wa, &h);
    lcd_getstringsize(b, &wb, &h);

    x = (PV_W - wa - wb) / 2;
    lcd_set_foreground(ca);
    lcd_putsxy(x, y, a);
    lcd_set_foreground(cb);
    lcd_putsxy(x + wa, y, b);
}

void pv_kicker(int y, const char *s, unsigned colour)
{
    const int sp = 2;       /* extra tracking between letters */
    char b[2] = { 0, 0 };
    int total = 0, cw, ch, x;

    lcd_setfont(body_font);
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(colour);

    /* Measured a character at a time, because that is how it is drawn -- the
     * spacing is what the effect is. */
    for (const char *p = s; *p; p++)
    {
        b[0] = *p;
        lcd_getstringsize(b, &cw, &ch);
        total += cw + sp;
    }
    total -= sp;

    x = (PV_W - total) / 2;
    for (const char *p = s; *p; p++)
    {
        b[0] = *p;
        lcd_getstringsize(b, &cw, &ch);
        lcd_putsxy(x, y, b);
        x += cw + sp;
    }
}

void pv_underline(const struct pv_theme *th, int y)
{
    pv_capsule(th, PV_W / 2 - 16, y, PV_W / 2 + 16, y, 2, th->accent);
}

void pv_fit_text(const char *s, int maxw, char *out, int outsz)
{
    int w, h, len;

    lcd_setfont(body_font);
    strlcpy(out, s, outsz);
    lcd_getstringsize(out, &w, &h);
    if (w <= maxw)
        return;

    /* Shorten until there is room for the two dots as well as the text. */
    len = strlen(out);
    while (len > 2)
    {
        len--;
        out[len] = '\0';
        lcd_getstringsize(out, &w, &h);
        if (w <= maxw - 10)
            break;
    }
    if (len >= 2)
    {
        out[len - 1] = '.';
        out[len]     = '.';
        out[len + 1] = '\0';
    }
}

void pv_commafmt(long v, char *out, size_t outsz)
{
    char tmp[24];
    int len, o = 0;

    snprintf(tmp, sizeof(tmp), "%ld", v < 0 ? -v : v);
    len = strlen(tmp);

    if (v < 0 && o < (int)outsz - 1)
        out[o++] = '-';
    for (int i = 0; i < len; i++)
    {
        if (i && (len - i) % 3 == 0 && o < (int)outsz - 1)
            out[o++] = ',';
        if (o < (int)outsz - 1)
            out[o++] = tmp[i];
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------ furniture */

void pv_page_dots(const struct pv_theme *th, int idx, int count)
{
    const int r = 2, gap = 7;
    int w, x, y = PV_H - 10;

    if (count <= 1)
        return;

    w = (count - 1) * gap;
    x = (PV_W - w) / 2;

    for (int i = 0; i < count; i++)
    {
        bool here = (i == idx);

        pv_disc(th, x + i * gap, y, r,
                here ? th->accent : PV_TEXT_LIGHT, here ? 256 : 96);
    }
}

/* ----------------------------------------------------------- animation */

static bool exporting;

void pv_set_export(bool on)
{
    exporting = on;
}

bool pv_exporting(void)
{
    return exporting;
}

/* Buttons that act the moment they arrive, whatever the wheel is doing. */
static bool immediate_button(int b)
{
    if (b == SYS_USB_CONNECTED)
        return true;

    b &= ~BUTTON_REPEAT;
    return b == BUTTON_LEFT || b == BUTTON_RIGHT
        || b == BUTTON_MENU || b == BUTTON_SELECT;
}

/* Which way a wheel event points, or 0 if it is not one.
 *
 * BUTTON_REPEAT is masked off: continuous wheel rotation posts
 * repeat-flagged scroll events, and those are navigation too. */
static int wheel_dir(int b)
{
    b &= ~BUTTON_REPEAT;
    if (b == BUTTON_SCROLL_FWD)
        return +1;
    if (b == BUTTON_SCROLL_BACK)
        return -1;
    return 0;
}

/* Wheel events needed to turn one card.
 *
 * The clickwheel reports a great many small movements, and a card is a whole
 * screenful rather than a list row -- at one card per event the smallest
 * touch skids through several. The pad's own left and right are exact and are
 * not damped. */
#define PV_WHEEL_PER_CARD 4

/* Movement asked for since the last card turned. Shared by the deck loop and
 * by the animations, so a wheel event is counted once wherever it is read. */
static int wheel;

/* A reversal starts again from this event rather than unwinding the count, so
 * turning back is as responsive as starting. */
static int wheel_plus(int dir)
{
    return (((wheel > 0) != (dir > 0)) ? 0 : wheel) + dir;
}

static bool wheel_at_card(int w)
{
    return w <= -PV_WHEEL_PER_CARD || w >= PV_WHEEL_PER_CARD;
}

int pv_nav_step(int button)
{
    int b = button & ~BUTTON_REPEAT;
    int dir;

    if (b == BUTTON_RIGHT)
        return +1;
    if (b == BUTTON_LEFT)
        return -1;

    dir = wheel_dir(button);
    if (dir == 0)
    {
        wheel = 0;
        return 0;
    }

    wheel = wheel_plus(dir);
    if (wheel_at_card(wheel))
    {
        wheel = 0;
        return dir;
    }
    return 0;
}

bool pv_nav_interrupt(int button)
{
    int dir = wheel_dir(button);

    if (dir == 0)
        return immediate_button(button);

    /* A wheel event abandons an animation only when it is the one that will
     * turn the card, and is then left for pv_nav_step() to count. The others
     * are counted here and dropped, since the deck loop never sees them. Trap:
     * letting them abandon the animation too kills a card's animation on three
     * wheel events in four and then navigates nowhere. */
    if (wheel_at_card(wheel_plus(dir)))
        return true;

    wheel = wheel_plus(dir);
    return false;
}

long pv_ease(long target, int fr, int frames)
{
    long inv = frames - fr;
    return target - (target * inv * inv) / ((long)frames * frames);
}

int pv_animate_count(const struct pv_theme *th, int cx, int oy,
                     int band_y, int band_h,
                     int cw, int ch, int sw, long target)
{
    const int frames = 16;
    char b[24];

    /* Straight to the end when exporting. */
    for (int fr = (pv_exporting() ? frames : 0); fr <= frames; fr++)
    {
        int btn;

        pv_commafmt(pv_ease(target, fr, frames), b, sizeof(b));
        pv_band(th, band_y, band_h);
        pv_number(th, cx, oy, b, cw, ch, sw);
        lcd_update_rect(0, band_y, PV_W, band_h);

        btn = button_get_w_tmo(HZ / 50);
        if (pv_nav_interrupt(btn))
        {
            pv_commafmt(target, b, sizeof(b));
            pv_band(th, band_y, band_h);
            pv_number(th, cx, oy, b, cw, ch, sw);
            lcd_update_rect(0, band_y, PV_W, band_h);
            return btn;
        }
    }
    return 0;
}

int pv_animate_percent(const struct pv_theme *th, int oy, int pct)
{
    const int cw = 32, ch = 54, sw = 9, pctsz = 34, frames = 14;
    int numcx = PV_W / 2 - (pctsz + 8) / 2;
    char b[16];
    int numw, ret = 0;

    snprintf(b, sizeof(b), "%d", pct);
    numw = pv_number_width(b, cw, sw);

    for (int fr = (pv_exporting() ? frames : 0); fr <= frames; fr++)
    {
        int btn;

        snprintf(b, sizeof(b), "%d", (int)pv_ease(pct, fr, frames));
        pv_band(th, oy - 2, ch + sw + 6);
        pv_number(th, numcx, oy, b, cw, ch, sw);
        pv_percent(th, numcx + numw / 2 + 8 + pctsz / 2, oy + ch - pctsz / 2,
                   pctsz);
        lcd_update_rect(0, oy - 2, PV_W, ch + sw + 6);

        btn = button_get_w_tmo(HZ / 50);
        if (pv_nav_interrupt(btn))
        {
            snprintf(b, sizeof(b), "%d", pct);
            pv_band(th, oy - 2, ch + sw + 6);
            pv_number(th, numcx, oy, b, cw, ch, sw);
            pv_percent(th, numcx + numw / 2 + 8 + pctsz / 2,
                       oy + ch - pctsz / 2, pctsz);
            lcd_update_rect(0, oy - 2, PV_W, ch + sw + 6);
            ret = btn;
            break;
        }
    }
    return ret;
}

void pv_present(int dir)
{
    const int step = 24;
    int nstrips;

    if (dir == 0)
    {
        lcd_update();
        return;
    }

    /* The frame is already drawn; only the order the columns reach the panel
     * differs, so a transition costs no rendering at all. */
    nstrips = (PV_W + step - 1) / step;
    for (int s = 0; s < nstrips; s++)
    {
        int i = (dir > 0) ? s : (nstrips - 1 - s);
        int x = i * step;
        int w = (x + step > PV_W) ? (PV_W - x) : step;

        lcd_update_rect(x, 0, w, PV_H);
        sleep(1);
    }
}
