/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.c (colour parsing)
 * GNU General Public License (version 2+)
 *
 * Colour arithmetic, and parsing colours from text.
 *
 * Parts, in order:
 *   - hex_to_rgb(), blending, HSV, contrast and the HSV contrast fit
 *   - tone and OKLab: perceptual lightness, and moving a colour to a tone
 *     with its hue kept
 *   - parse_color(), and the table of palette words skins write
 ****************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"
#include "system.h"                 /* ARRAYLEN */
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

/* ---------------------------------------------------------------------- *
 * Tone and OKLab                                                         *
 * ---------------------------------------------------------------------- */

/* OKLab in integers, Q16: lightness 0..65536, a and b signed. The matrices
 * are Ottosson's, scaled by 2^14 and rounded so each row still sums to what
 * it did.
 *
 * OKLab takes sRGB's own curve, below; tone takes rel_luminance()'s gamma of
 * 2.0, so a bound is met on the same terms color_contrast() measures. Trap:
 * putting the 2.0 shortcut into OKLab as well moves chroma enough to change
 * the vivid colour picked on one album in seven. */
#define OK_ONE 65536

/* sRGB's transfer curve, 8-bit value to linear light in Q16 */
static const uint16_t srgb_linear[256] =
{
        0,    20,    40,    60,    80,    99,   119,   139,   159,   179,
      199,   219,   241,   264,   288,   313,   340,   367,   396,   427,
      458,   491,   526,   562,   599,   637,   677,   718,   761,   805,
      851,   898,   947,   997,  1048,  1101,  1156,  1212,  1270,  1330,
     1391,  1453,  1517,  1583,  1651,  1720,  1791,  1863,  1937,  2013,
     2090,  2170,  2250,  2333,  2418,  2504,  2592,  2681,  2773,  2866,
     2961,  3058,  3157,  3258,  3360,  3464,  3570,  3678,  3788,  3900,
     4014,  4129,  4247,  4366,  4488,  4611,  4736,  4864,  4993,  5124,
     5257,  5392,  5530,  5669,  5810,  5953,  6099,  6246,  6395,  6547,
     6701,  6856,  7014,  7174,  7336,  7500,  7666,  7834,  8004,  8177,
     8352,  8529,  8708,  8889,  9072,  9258,  9446,  9636,  9828, 10022,
    10219, 10418, 10619, 10822, 11028, 11236, 11446, 11658, 11873, 12090,
    12309, 12531, 12754, 12981, 13209, 13440, 13673, 13909, 14147, 14387,
    14629, 14874, 15122, 15372, 15624, 15878, 16135, 16394, 16656, 16920,
    17187, 17456, 17727, 18001, 18278, 18556, 18838, 19121, 19408, 19696,
    19988, 20281, 20578, 20876, 21178, 21481, 21788, 22096, 22408, 22722,
    23038, 23357, 23679, 24003, 24329, 24659, 24991, 25325, 25662, 26002,
    26344, 26689, 27036, 27387, 27739, 28095, 28453, 28813, 29177, 29543,
    29911, 30283, 30657, 31033, 31413, 31795, 32180, 32567, 32957, 33350,
    33746, 34144, 34545, 34949, 35355, 35765, 36177, 36591, 37009, 37429,
    37852, 38278, 38707, 39138, 39572, 40009, 40449, 40892, 41337, 41786,
    42237, 42691, 43147, 43607, 44069, 44534, 45003, 45474, 45947, 46424,
    46904, 47386, 47871, 48360, 48851, 49345, 49842, 50342, 50844, 51350,
    51859, 52370, 52884, 53402, 53922, 54445, 54972, 55501, 56033, 56568,
    57106, 57647, 58191, 58738, 59288, 59841, 60397, 60956, 61518, 62083,
    62651, 63222, 63796, 64373, 64953, 65535,
};

struct oklab { int32_t l, a, b; };

static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;

    while (bit > x)
        bit >>= 2;
    while (bit)
    {
        if (x >= r + bit)
        {
            x -= r + bit;
            r = (r >> 1) + bit;
        }
        else
            r >>= 1;
        bit >>= 2;
    }
    return r;
}

static uint32_t icbrt64(uint64_t x)
{
    uint64_t y = 0;
    int s;

    for (s = 63; s >= 0; s -= 3)
    {
        uint64_t b;

        y <<= 1;
        b = 3 * y * (y + 1) + 1;
        if ((x >> s) >= b)
        {
            x -= b << s;
            y++;
        }
    }
    return (uint32_t)y;
}

/* linear light back to an 8-bit value: the nearest entry in the curve */
static int srgb_encode(int32_t lin)
{
    int lo = 0, hi = 255;

    while (lo < hi)
    {
        int mid = (lo + hi + 1) / 2;

        if (srgb_linear[mid] <= lin)
            lo = mid;
        else
            hi = mid - 1;
    }
    if (lo < 255 && srgb_linear[lo + 1] - lin < lin - srgb_linear[lo])
        lo++;
    return lo;
}

static void oklab_from(unsigned c, struct oklab *o)
{
    int32_t r = srgb_linear[RGB_UNPACK_RED(c)];
    int32_t g = srgb_linear[RGB_UNPACK_GREEN(c)];
    int32_t b = srgb_linear[RGB_UNPACK_BLUE(c)];
    int32_t l = (6754 * r + 8787 * g + 843 * b) >> 14;
    int32_t m = (3472 * r + 11152 * g + 1760 * b) >> 14;
    int32_t s = (1447 * r + 4616 * g + 10321 * b) >> 14;

    /* cube roots of Q16 values, still Q16 */
    l = icbrt64((uint64_t)l << 32);
    m = icbrt64((uint64_t)m << 32);
    s = icbrt64((uint64_t)s << 32);
    o->l = (int32_t)((3448LL * l + 13003LL * m - 67LL * s) >> 14);
    o->a = (int32_t)((32407LL * l - 39790LL * m + 7383LL * s) >> 14);
    o->b = (int32_t)((424LL * l + 12825LL * m - 13249LL * s) >> 14);
}

/* false when the colour is outside what the display can show */
static bool oklab_to(const struct oklab *o, unsigned *out)
{
    int64_t l_ = o->l + ((6494 * o->a + 3536 * o->b) >> 14);
    int64_t m_ = o->l - ((1730 * o->a + 1046 * o->b) >> 14);
    int64_t s_ = o->l - ((1466 * o->a + 21159 * o->b) >> 14);
    int64_t l = (l_ * l_ >> 16) * l_ >> 16;
    int64_t m = (m_ * m_ >> 16) * m_ >> 16;
    int64_t s = (s_ * s_ >> 16) * s_ >> 16;
    int64_t lin[3];
    int ch[3];
    int i;

    lin[0] = (66793 * l - 54193 * m + 3784 * s) >> 14;
    lin[1] = (-20782 * l + 42758 * m - 5592 * s) >> 14;
    lin[2] = (-69 * l - 11525 * m + 27978 * s) >> 14;
    for (i = 0; i < 3; i++)
    {
        /* a step either side is rounding, not a colour out of range */
        if (lin[i] < -64 || lin[i] > OK_ONE + 64)
            return false;
        if (lin[i] < 0)
            lin[i] = 0;
        if (lin[i] > OK_ONE)
            lin[i] = OK_ONE;
        ch[i] = srgb_encode((int32_t)lin[i]);
    }
    *out = LCD_RGBPACK(ch[0], ch[1], ch[2]);
    return true;
}

/* o at lightness l, keeping its hue and as much of its chroma as the display
 * can show there. */
static unsigned oklab_at(const struct oklab *o, int32_t l)
{
    struct oklab t = { l, o->a, o->b };
    unsigned c;
    int lo = 0, hi = 256, i;

    if (oklab_to(&t, &c))
        return c;
    for (i = 0; i < 8; i++)
    {
        int mid = (lo + hi) / 2;

        t.a = o->a * mid / 256;
        t.b = o->b * mid / 256;
        if (oklab_to(&t, &c))
            lo = mid;
        else
            hi = mid;
    }
    t.a = o->a * lo / 256;
    t.b = o->b * lo / 256;
    if (!oklab_to(&t, &c))
        c = l >= OK_ONE / 2 ? LCD_RGBPACK(255, 255, 255) : LCD_RGBPACK(0, 0, 0);
    return c;
}

/* The luminance at a tone, on rel_luminance()'s 0..65025 scale: CIELAB's
 * inverse, Y = ((L* + 16) / 116)^3 above its linear toe. */
static int32_t tone_luminance(int tone)
{
    if (tone <= 8)
        return tone * 65025 * 27 / 24389;
    return (int32_t)((int64_t)(tone + 16) * (tone + 16) * (tone + 16)
                     * 65025 / 1560896);
}

unsigned color_tone_bound(unsigned c, int tone, int bound)
{
    int32_t want = tone_luminance(tone);
    struct oklab o;
    int32_t lo, hi;
    unsigned out;
    int i;

    if (bound == 0 ||
        (bound > 0 ? rel_luminance(c) >= want : rel_luminance(c) <= want))
        return c;

    /* Lightness against luminance is monotonic at a fixed hue, so halve the
     * gap between the colour's own lightness and the end it is moving
     * toward, which always meets the bound. */
    oklab_from(c, &o);
    lo = o.l;
    hi = bound > 0 ? OK_ONE : 0;
    for (i = 0; i < 16; i++)
    {
        int32_t mid = (lo + hi) / 2;
        int32_t y = rel_luminance(oklab_at(&o, mid));

        if (bound > 0 ? y >= want : y <= want)
            hi = mid;
        else
            lo = mid;
    }
    out = oklab_at(&o, hi);
    if (bound > 0 ? rel_luminance(out) < want : rel_luminance(out) > want)
        out = bound > 0 ? LCD_RGBPACK(255, 255, 255) : LCD_RGBPACK(0, 0, 0);
    return out;
}

int color_chroma(unsigned c)
{
    struct oklab o;

    oklab_from(c, &o);
    return isqrt32((uint32_t)(o.a * o.a + o.b * o.b));
}

/* ---------------------------------------------------------------------- *
 * Parsing                                                                *
 * ---------------------------------------------------------------------- */

static struct color_word words[COLOR_WORDS_MAX];
static int nwords;

const struct color_word *color_word(unsigned c)
{
    if (!(c & COLOR_WORD) || COLOR_WORD_INDEX(c) >= (unsigned)nwords)
        return NULL;
    return &words[COLOR_WORD_INDEX(c)];
}

void color_words_reset(void)
{
    nwords = 0;
}

/* A number of at most `max`, at least one digit, advancing *p past it. */
static bool parse_amount(const char **p, int max, int *out)
{
    int n = 0;

    if (!isdigit((unsigned char)**p))
        return false;
    while (isdigit((unsigned char)**p))
    {
        n = n * 10 + (*(*p)++ - '0');
        if (n > max)
            return false;
    }
    *out = n;
    return true;
}

/* `accent`, `dominant` or `vivid`, then optionally `>NN` or `<NN`, `.NN`
 * and `:rrggbb`, in that order. Identical words share one table entry, so a
 * skin pays for the distinct colours it names, not for each use. */
static bool parse_word(const char *text, int *value)
{
    static const char *const names[] = { "accent", "dominant", "vivid" };
    struct color_word w;
    const char *p = NULL;
    int n, i;

    memset(&w, 0, sizeof w);
    for (i = 0; i < (int)ARRAYLEN(names); i++)
    {
        size_t len = strlen(names[i]);

        if (strncasecmp(text, names[i], len) == 0 &&
            strchr("<>.:", text[len]))          /* also matches the '\0' */
        {
            w.kind = i;
            p = text + len;
            break;
        }
    }
    if (!p)
        return false;

    if (*p == '>' || *p == '<')
    {
        w.bound = *p++ == '>' ? 1 : -1;
        if (!parse_amount(&p, 100, &n))
            return false;
        w.tone = n;
    }
    if (*p == '.')
    {
        p++;
        if (!parse_amount(&p, 100, &n))
            return false;
        w.shade = n + 1;
    }
    if (*p == ':')
    {
        int fallback;

        if (hex_to_rgb(p + 1, &fallback) < 0 || p[7] != '\0')
            return false;
        w.has_fallback = true;
        w.fallback = fallback;
        p += 7;
    }
    if (*p != '\0')
        return false;

    for (i = 0; i < nwords; i++)
        if (!memcmp(&words[i], &w, sizeof w))
            break;
    if (i == nwords)
    {
        if (nwords == COLOR_WORDS_MAX)
            return false;
        words[nwords++] = w;
    }
    *value = COLOR_WORD | i;
    return true;
}

bool parse_color(enum screen_type screen, char *text, int *value)
{
    (void)text; (void)value; /* silence warnings on mono bitmap */
    (void)screen;

    if (screens[screen].depth > 2)
    {
        bool fixed = false;

        if (isalpha((unsigned char)*text) && parse_word(text, value))
            return true;

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
