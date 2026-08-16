/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.c (colour parsing)
 * GNU General Public License (version 2+)
 *
 * Parses colours from text: "#rrggbb" hex into a native pixel value, and the
 * named/hex forms skins and theme files use.
 ****************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"
#include "lcd.h"
#include "settings/settings.h"
#include "draw/screen_access.h"
#include "color.h"

/*
 * Helper function to convert a string of 6 hex digits to a native colour
 */
static int hex2dec(int c)
{
    return  (((c) >= '0' && ((c) <= '9')) ? (c) - '0' :
                                            (toupper(c)) - 'A' + 10);
}

int hex_to_rgb(const char* hex, int* color)
{
    int red, green, blue;
    int i = 0;

    while ((i < 6) && (isxdigit(hex[i])))
        i++;

    if (i < 6)
        return -1;

    red = (hex2dec(hex[0]) << 4) | hex2dec(hex[1]);
    green = (hex2dec(hex[2]) << 4) | hex2dec(hex[3]);
    blue = (hex2dec(hex[4]) << 4) | hex2dec(hex[5]);

    *color = LCD_RGBPACK(red,green,blue);

    return 0;
}

unsigned color_blend(unsigned c1, unsigned c2, int t)
{
    int r1 = RGB_UNPACK_RED(c1);
    int g1 = RGB_UNPACK_GREEN(c1);
    int b1 = RGB_UNPACK_BLUE(c1);
    int r2 = RGB_UNPACK_RED(c2);
    int g2 = RGB_UNPACK_GREEN(c2);
    int b2 = RGB_UNPACK_BLUE(c2);

    int r = r1 + (((r2 - r1) * t) >> 8);
    int g = g1 + (((g2 - g1) * t) >> 8);
    int b = b1 + (((b2 - b1) * t) >> 8);

    return LCD_RGBPACK(r, g, b);
}

void color_get_hsv(unsigned c, int *h, int *s, int *v)
{
    int r = RGB_UNPACK_RED(c);
    int g = RGB_UNPACK_GREEN(c);
    int b = RGB_UNPACK_BLUE(c);
    int max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int delta = max - min;

    /* A grey has no hue, so it yields 0 (red): a predictable answer rather
     * than another grey, which is the one result that would be useless to a
     * caller asking where to turn to. */
    if (delta == 0)
        *h = 0;
    else if (max == r)
        *h = (60 * (g - b)) / delta;
    else if (max == g)
        *h = 120 + (60 * (b - r)) / delta;
    else
        *h = 240 + (60 * (r - g)) / delta;

    if (*h < 0)
        *h += 360;

    *s = max ? (delta * 255) / max : 0;
    *v = max;
}

unsigned color_from_hsv(int h, int s, int v)
{
    int region, rem, p, q, t, r, g, b;

    h %= 360;
    if (h < 0)
        h += 360;

    region = h / 60;
    rem    = ((h - region * 60) * 255) / 60;
    p = (v * (255 - s)) / 255;
    q = (v * (255 - (s * rem) / 255)) / 255;
    t = (v * (255 - (s * (255 - rem)) / 255)) / 255;

    switch (region)
    {
        case 0:  r = v; g = t; b = p; break;
        case 1:  r = q; g = v; b = p; break;
        case 2:  r = p; g = v; b = t; break;
        case 3:  r = p; g = q; b = v; break;
        case 4:  r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }

    return LCD_RGBPACK(r, g, b);
}

unsigned color_hue_rotate(unsigned base, int degrees, int sat, int val)
{
    int h, s, v;

    color_get_hsv(base, &h, &s, &v);

    /* Back to RGB at the caller's saturation and brightness rather than
     * base's. A theme background is usually dark and unsaturated and has
     * neither to spare -- keeping them would give a colour too close to the
     * one we were asked to move away from. */
    return color_from_hsv(h + degrees, sat, val);
}

/* WCAG relative luminance. sRGB is linearised with a gamma of 2.0 rather than
 * the exact piecewise curve -- a multiply instead of a lookup table, and well
 * inside the tolerance of a pass/fail threshold. */
static int32_t rel_luminance(unsigned c)
{
    int32_t r = RGB_UNPACK_RED(c);
    int32_t g = RGB_UNPACK_GREEN(c);
    int32_t b = RGB_UNPACK_BLUE(c);

    return (r * r * 2126 + g * g * 7152 + b * b * 722) / 10000;
}

int color_contrast(unsigned c1, unsigned c2)
{
    int32_t la = rel_luminance(c1);
    int32_t lb = rel_luminance(c2);
    int32_t hi = (la > lb ? la : lb) + 3251;   /* the 0.05 of the WCAG ratio */
    int32_t lo = (la < lb ? la : lb) + 3251;

    return (int)((hi * 100) / lo);
}

/* Steps of brightness taken while searching, and the most that will be tried
 * before giving up on the hue and going to plain black or white. 16 steps of
 * 16 reaches either end of the 0..255 range from anywhere in it. */
#define FIT_STEP  16
#define FIT_TRIES 16

unsigned color_fit_contrast(unsigned c, unsigned against, int target)
{
    unsigned white = LCD_RGBPACK(255, 255, 255);
    unsigned black = LCD_RGBPACK(0, 0, 0);
    bool lighten;
    int h, s, v, step, i;

    if (color_contrast(c, against) >= target)
        return c;

    color_get_hsv(c, &h, &s, &v);

    /* Only brightness moves: the hue is the caller's design and the saturation
     * is what makes it read as a colour rather than a tint.
     *
     * Both ways are tried at each distance, nearest first, so the answer is the
     * one that changes the colour least. Searching one way only -- toward
     * whichever extreme has more contrast in it -- takes a vivid colour all the
     * way to near-black when a step or two the other way would have done, and a
     * near-black accent is not an accent. */
    for (step = FIT_STEP; step <= FIT_STEP * FIT_TRIES; step += FIT_STEP)
    {
        for (i = 0; i < 2; i++)
        {
            int cv = i ? v - step : v + step;
            unsigned cand;

            /* clamped rather than skipped, so the ends of the range are
             * themselves tried: a colour that only works at full brightness
             * would otherwise be stepped straight past and lose its hue to the
             * fallback below */
            if (cv < 0)
                cv = 0;
            if (cv > 255)
                cv = 255;

            cand = color_from_hsv(h, s, cv);
            if (color_contrast(cand, against) >= target)
                return cand;
        }
    }

    lighten = color_contrast(white, against) > color_contrast(black, against);

    /* The hue cannot reach the target at any brightness -- a saturated one on
     * a mid-luminance field is the usual case, since it runs out of range
     * before it runs out of contrast. Legibility wins over hue.
     *
     * A mid-luminance background may leave even this short of the target: no
     * colour whatever reaches 4.5:1 against some of them. This is then the
     * most readable answer that exists, not a passing one. */
    return lighten ? white : black;
}

/* '0'-'3' are ASCII 0x30 to 0x33 */
#define is0123(x) (((x) & 0xfc) == 0x30)
bool parse_color(enum screen_type screen, char *text, int *value)
{
    (void)text; (void)value; /* silence warnings on mono bitmap */
    (void)screen;

    if (screens[screen].depth > 2)
    {
        bool fixed = false;

        /* '!' before the digits pins the colour: the album palette carries
         * every other literal over, and this is how a skin says not to. */
        if (*text == '!')
        {
            fixed = true;
            text++;
        }

        if (hex_to_rgb(text, value) < 0)
            return false;

        if (fixed)
            *value |= COLOR_FIXED;
        return true;
    }

    return false;
}
