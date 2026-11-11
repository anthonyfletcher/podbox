/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade core: colours as 0xRRGGBB, for building a palette at run time.
 *
 * playback.colors() hands over the colours the player is showing: the
 * album's, under dynamic colours, or the theme's. The color.* calls make
 * shades and accents from any colour, the way the player derives its own:
 * tone is CIELAB lightness from a gamma-2 luminance, so two tones say how
 * far apart two colours read; toned() and turn() move a colour through
 * OKLab, keeping its hue or its lightness, and losing chroma only where no
 * RGB colour has it.
 *
 * Parts: luminance and tone; OKLab; the natives.
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"
#include "games/arcade/arcade.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)
#define RET_INT(v) (*out = mk(T_INT, (int32_t)(v)), 1)

#define CR(c) ((int32_t)((c) >> 16) & 0xff)
#define CG(c) ((int32_t)((c) >> 8) & 0xff)
#define CB(c) ((int32_t)(c) & 0xff)
#define RGB(r, g, b) ((uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))

/* ---- luminance and tone -------------------------------------------------- */

/* WCAG luminance with sRGB taken as gamma 2, 0..65025 */
static int32_t luminance(uint32_t c)
{
    return (CR(c) * CR(c) * 2126 + CG(c) * CG(c) * 7152 +
            CB(c) * CB(c) * 722) / 10000;
}

static uint32_t icbrt64(uint64_t x)
{
    uint64_t y = 0;
    for (int s = 63; s >= 0; s -= 3) {
        y <<= 1;
        uint64_t b = 3 * y * (y + 1) + 1;
        if ((x >> s) >= b) {
            x -= b << s;
            y++;
        }
    }
    return (uint32_t)y;
}

/* CIELAB L*, 0..100, of luminance y */
static int32_t tone_of(int32_t y)
{
    if (y * 24389 <= 65025 * 216)
        return (y * 24389 / 27 + 32512) / 65025;
    /* the cube root of y / 65025, in 16.16 */
    uint32_t r = icbrt64(((uint64_t)y << 48) / 65025);
    return (int32_t)((116 * (int64_t)r + 32768) >> 16) - 16;
}

/* the luminance at a tone: L*'s inverse */
static int32_t tone_luminance(int32_t tone)
{
    if (tone <= 8)
        return tone * 65025 * 27 / 24389;
    return (int32_t)((int64_t)(tone + 16) * (tone + 16) * (tone + 16) *
                     65025 / 1560896);
}

/* ---- OKLab, in 16.16 ----------------------------------------------------- */

#define OK_ONE 65536

/* sRGB's transfer curve, 8-bit value to linear light in 16.16 */
static const uint16_t srgb_linear[256] = {
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

/* linear light back to an 8-bit value: the nearest entry in the curve */
static int32_t srgb_encode(int32_t lin)
{
    int lo = 0, hi = 255;
    while (lo < hi) {
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

static void oklab_from(uint32_t c, struct oklab *o)
{
    int32_t r = srgb_linear[CR(c)], g = srgb_linear[CG(c)];
    int32_t b = srgb_linear[CB(c)];
    int32_t l = (6754 * r + 8787 * g + 843 * b) >> 14;
    int32_t m = (3472 * r + 11152 * g + 1760 * b) >> 14;
    int32_t s = (1447 * r + 4616 * g + 10321 * b) >> 14;
    l = (int32_t)icbrt64((uint64_t)l << 32);
    m = (int32_t)icbrt64((uint64_t)m << 32);
    s = (int32_t)icbrt64((uint64_t)s << 32);
    o->l = (int32_t)((3448LL * l + 13003LL * m - 67LL * s) >> 14);
    o->a = (int32_t)((32407LL * l - 39790LL * m + 7383LL * s) >> 14);
    o->b = (int32_t)((424LL * l + 12825LL * m - 13249LL * s) >> 14);
}

/* 0 when no RGB colour is that one */
static int oklab_to(const struct oklab *o, uint32_t *out)
{
    int64_t l_ = o->l + ((6494LL * o->a + 3536LL * o->b) >> 14);
    int64_t m_ = o->l - ((1730LL * o->a + 1046LL * o->b) >> 14);
    int64_t s_ = o->l - ((1466LL * o->a + 21159LL * o->b) >> 14);
    int64_t l = (l_ * l_ >> 16) * l_ >> 16;
    int64_t m = (m_ * m_ >> 16) * m_ >> 16;
    int64_t s = (s_ * s_ >> 16) * s_ >> 16;
    int64_t lin[3] = { (66793 * l - 54193 * m + 3784 * s) >> 14,
                       (-20782 * l + 42758 * m - 5592 * s) >> 14,
                       (-69 * l - 11525 * m + 27978 * s) >> 14 };
    int32_t ch[3];
    for (int i = 0; i < 3; i++) {
        /* a step either side is rounding, not a colour out of range */
        if (lin[i] < -64 || lin[i] > OK_ONE + 64)
            return 0;
        lin[i] = lin[i] < 0 ? 0 : lin[i] > OK_ONE ? OK_ONE : lin[i];
        ch[i] = srgb_encode((int32_t)lin[i]);
    }
    *out = RGB(ch[0], ch[1], ch[2]);
    return 1;
}

/* o at lightness l, its hue kept, and as much of its chroma as some RGB
 * colour has there */
static uint32_t oklab_at(const struct oklab *o, int32_t l)
{
    struct oklab t = { l, o->a, o->b };
    uint32_t c;
    if (oklab_to(&t, &c))
        return c;
    int lo = 0, hi = 256;
    for (int i = 0; i < 8; i++) {
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
        c = l >= OK_ONE / 2 ? 0xffffff : 0;
    return c;
}

/* o's hue at a tone, 1 to 99: lightness against luminance rises at a fixed
 * hue, so halve toward it. Twelve halvings leave a step of 1/4096, far
 * finer than a tone. */
static uint32_t oklab_toned(const struct oklab *o, int32_t tone)
{
    int32_t want = tone_luminance(tone), lo = 0, hi = OK_ONE;
    for (int i = 0; i < 12; i++) {
        int32_t mid = (lo + hi) / 2;
        if (luminance(oklab_at(o, mid)) >= want)
            hi = mid;
        else
            lo = mid;
    }
    uint32_t below = oklab_at(o, lo), above = oklab_at(o, hi);
    return want - luminance(below) < luminance(above) - want ? below : above;
}

/* ---- the natives --------------------------------------------------------- */

static int colour_arg(struct vm *vm, struct value *a, int i, const char *fn,
                      uint32_t *c)
{
    if (!is_num(a[i]))
        return vm_fail(vm, fn, "() needs a colour, 0xRRGGBB, not a ",
                       vm_typename(a[i].tag));
    *c = (uint32_t)floor_int(a[i]) & 0xffffff;
    return 1;
}

#define COLOUR(i, x, fn) uint32_t x = 0; \
    if (!colour_arg(vm, a, i, fn, &x)) return 0
#define NUM(i, fn) if (!is_num(a[i])) \
    return vm_fail(vm, fn, "() needs a number, not a ", vm_typename(a[i].tag))

/* playback.colors(out): the background, the text and the most vivid
 * colour the player is showing, into an Int32Array of 3; whether they are
 * the album's */
NATIVE(playback_colors)
{
    (void)argc;
    if (a[0].tag != T_I32 || I32(vm, a[0])->len < 3)
        return vm_fail(vm, "playback.colors() needs an Int32Array of 3",
                       "", "");
    uint32_t c[3] = { 0x000000, 0xffffff, 0xffffff };
    const struct arcade_tracks *t = vm->host ? vm->host->tracks : NULL;
    int album = t && t->colours ? t->colours(vm->host->ctx, c) : 0;
    int32_t *d = I32(vm, a[0])->d;
    for (int i = 0; i < 3; i++)
        d[i] = (int32_t)(c[i] & 0xffffff);
    vm_charge(vm, 20);
    return RET_INT(album ? 1 : 0);
}

/* color.mix(a, b, t): a toward b, channel by channel; t from 0 to 1.0 */
NATIVE(color_mix)
{
    (void)argc;
    COLOUR(0, c1, "color.mix");
    COLOUR(1, c2, "color.mix");
    NUM(2, "color.mix");
    int64_t t = scaled(a[2]);
    t = t < 0 ? 0 : t > 65536 ? 65536 : t;
    int32_t r = CR(c1) + (int32_t)(((CR(c2) - CR(c1)) * t + 32768) >> 16);
    int32_t g = CG(c1) + (int32_t)(((CG(c2) - CG(c1)) * t + 32768) >> 16);
    int32_t b = CB(c1) + (int32_t)(((CB(c2) - CB(c1)) * t + 32768) >> 16);
    vm_charge(vm, 4);
    return RET_INT(RGB(r, g, b));
}

/* color.tone(c): how light c reads, 0 black to 100 white */
NATIVE(color_tone)
{
    (void)argc;
    COLOUR(0, c, "color.tone");
    vm_charge(vm, 46);
    return RET_INT(tone_of(luminance(c)));
}

/* color.toned(c, tone): c at that tone, its hue kept */
NATIVE(color_toned)
{
    (void)argc;
    COLOUR(0, c, "color.toned");
    NUM(1, "color.toned");
    int32_t tone = floor_int(a[1]);
    tone = tone < 0 ? 0 : tone > 100 ? 100 : tone;
    if (tone == 0 || tone == 100)
        return RET_INT(tone ? 0xffffff : 0);
    struct oklab o;
    oklab_from(c, &o);
    vm_charge(vm, 950);
    return RET_INT(oklab_toned(&o, tone));
}

/* color.turn(c, t): c's hue turned by t turns, its lightness kept */
NATIVE(color_turn)
{
    (void)argc;
    COLOUR(0, c, "color.turn");
    NUM(1, "color.turn");
    uint32_t t = (uint32_t)scaled(a[1]);
    int64_t cs = core_sine(t + 0x4000), sn = core_sine(t);
    struct oklab o, r;
    oklab_from(c, &o);
    /* a grey has no hue to turn */
    if ((int64_t)o.a * o.a + (int64_t)o.b * o.b < 64 * 64)
        return RET_INT(c);
    r.l = o.l;
    r.a = (int32_t)((o.a * cs - o.b * sn) >> 16);
    r.b = (int32_t)((o.a * sn + o.b * cs) >> 16);
    int32_t tone = tone_of(luminance(c));
    vm_charge(vm, 1455);
    if (tone <= 0 || tone >= 100)
        return RET_INT(c);
    return RET_INT(oklab_toned(&r, tone));
}

/* color.contrast(a, b): the WCAG ratio between them, 1.0 to 21.0: 4.5 is
 * what text wants against its background */
NATIVE(color_contrast)
{
    (void)argc;
    COLOUR(0, c1, "color.contrast");
    COLOUR(1, c2, "color.contrast");
    int64_t la = luminance(c1) + 3251, lb = luminance(c2) + 3251;
    int64_t hi = la > lb ? la : lb, lo = la < lb ? la : lb;
    vm_charge(vm, 16);
    *out = mk(T_FIX, wrap32((hi << 16) / lo));
    return 1;
}
