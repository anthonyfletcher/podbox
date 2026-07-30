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

unsigned color_hue_rotate(unsigned base, int degrees, int sat, int val)
{
    int r = RGB_UNPACK_RED(base);
    int g = RGB_UNPACK_GREEN(base);
    int b = RGB_UNPACK_BLUE(base);
    int max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int delta = max - min;
    int h, region, rem, p, q, t;

    /* base's hue in degrees. A grey has no hue to turn, so it yields 0 (red):
     * a predictable colour rather than another grey, which is the one answer
     * that would be useless to a caller asking for an accent. */
    if (delta == 0)
        h = 0;
    else if (max == r)
        h = (60 * (g - b)) / delta;
    else if (max == g)
        h = 120 + (60 * (b - r)) / delta;
    else
        h = 240 + (60 * (r - g)) / delta;

    h = (h + degrees) % 360;
    if (h < 0)
        h += 360;

    /* Back to RGB at the caller's saturation and brightness rather than
     * base's. A theme background is usually dark and unsaturated and has
     * neither to spare -- keeping them would give a colour too close to the
     * one we were asked to move away from. */
    region = h / 60;
    rem    = ((h - region * 60) * 255) / 60;
    p = (val * (255 - sat)) / 255;
    q = (val * (255 - (sat * rem) / 255)) / 255;
    t = (val * (255 - (sat * (255 - rem)) / 255)) / 255;

    switch (region)
    {
        case 0:  r = val; g = t;   b = p;   break;
        case 1:  r = q;   g = val; b = p;   break;
        case 2:  r = p;   g = val; b = t;   break;
        case 3:  r = p;   g = q;   b = val; break;
        case 4:  r = t;   g = p;   b = val; break;
        default: r = val; g = p;   b = q;   break;
    }

    return LCD_RGBPACK(r, g, b);
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

/* '0'-'3' are ASCII 0x30 to 0x33 */
#define is0123(x) (((x) & 0xfc) == 0x30)
bool parse_color(enum screen_type screen, char *text, int *value)
{
    (void)text; (void)value; /* silence warnings on mono bitmap */
    (void)screen;

    if (screens[screen].depth > 2)
    {
        if (hex_to_rgb(text, value) < 0)
            return false;
        else
            return true;
    }

    return false;
}
