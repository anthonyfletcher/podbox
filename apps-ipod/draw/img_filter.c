/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The artwork filter engine: a chain of named filters, folded into as few
 * passes over the pixels as the filters allow.
 ****************************************************************************/

/* Regions:
 *   the filter table    -- every name, its class, and what amount it takes
 *   channel operations  -- one filter's effect on one channel level
 *   colour matrices     -- one filter's effect on the three together
 *   folding             -- each stage, into one table or one matrix
 *   the parser          -- img_filter_compile()
 *   one pixel at a time -- each stage's arithmetic, written once
 *   the passes          -- the loops around it, and img_filter_apply()
 *
 * Two things decide the shape of all of it.
 *
 * A filter's class says what one output pixel needs to see, and filters of
 * the same class compose: every levels filter is a function on one channel
 * level, so a chain of them is the composition of those functions and folds
 * into a single 128-byte table when the chain is compiled; every colour
 * filter is a 3x3 matrix, so a chain of those folds into one matrix by
 * matrix multiplication. A chain pays for the stages it uses, not the
 * filters it names.
 *
 * The pixel work stays in the display's own field widths -- 5, 6 and 5 bits
 * on RGB565 -- and never expands a channel to 8 bits. Unpacking to 8 bits
 * costs a bit replication that repacking throws straight back away: 27 ARM
 * instructions a pixel against 17 for the same filter written on the fields
 * where they lie. The _LCD macro variants are what stay in field values, and
 * they carry the byte-swapped case for free.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "gcc_extensions.h"          /* FORCE_INLINE */
#include "fixedpoint.h"
#include "kernel.h"              /* yield() between bands */
#include "draw/img_filter.h"

#ifndef ARRAYLEN
#define ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* Longer than any filter name, so an over-long word cannot match one. */
#define NAME_MAX_LEN 16

/* Every channel at full: the complement mask for an inverting chain. */
#define FIELD_MASK \
    LCD_RGBPACK_LCD(LCD_MAX_RED, LCD_MAX_GREEN, LCD_MAX_BLUE)

/* ---------------------------------------------------------------------- *
 * The filter table                                                       *
 * ---------------------------------------------------------------------- */

enum channel_op_id
{
    OP_INVERT,
    OP_BRIGHTNESS,
    OP_GAIN,
    OP_CONTRAST,
    OP_REDUCE,
    OP_SATURATE,
    OP_HUE,
    OP_DITHER,      /* no arithmetic of its own; see "the passes" */
    OP_PIXELLATE,
    OP_BLUR,
};

/* What a filter does with the amount written after its name. */
enum amount_kind
{
    AMT_NONE,       /* an amount is an error                              */
    AMT_REQUIRED,   /* the filter means nothing without one               */
    AMT_DEFAULT,    /* absent means `def`                                 */
    AMT_ADAPTIVE,   /* absent means "derive it from the image"            */
    AMT_DERIVED,    /* an amount is an error: always from the image       */
};

static const struct filter_def
{
    const char   *name;
    unsigned      cls;          /* IMG_CLASS_*                            */
    unsigned char op;
    signed char   sign;         /* applied to the amount: darker darkens  */
    unsigned char amt;          /* enum amount_kind                       */
    short         min, max;     /* the amount's range, once signed        */
    short         def;
} filters[] =
{
#define LV IMG_CLASS_LEVELS                     /* so the columns fit */
#define CO IMG_CLASS_COLOUR
#define SC IMG_CLASS_SCREEN
#define SP IMG_CLASS_SPATIAL
#define RS IMG_CLASS_RESIZE
    { "invert",     LV, OP_INVERT,      1, AMT_NONE,        0,   0,   0 },
    { "brightness", LV, OP_BRIGHTNESS,  1, AMT_REQUIRED, -100, 100,   0 },
    /* Scaling, not sliding: these three exist to move a picture out of the
     * way of text without bleaching it, which is what OP_GAIN is for. So
     * `lighter50` is half as bright again rather than half way to white, and
     * only `darker100`, a scale of zero, reaches an end of the scale. */
    { "lighter",    LV, OP_GAIN,        1, AMT_ADAPTIVE,    0, 300,   0 },
    { "darker",     LV, OP_GAIN,       -1, AMT_ADAPTIVE,    0, 100,   0 },
    /* Brightness again, but neither the amount nor the direction is knowable
     * from the spec: both come from the picture and the colour it has to stay
     * clear of. sign and the range are unread -- adaptive_amount() returns an
     * amount already signed, and the parser range-checks only what was
     * written, which here is nothing. */
    { "scrim",      LV, OP_GAIN,        1, AMT_DERIVED,     0,   0,   0 },
    { "contrast",   LV, OP_CONTRAST,    1, AMT_REQUIRED, -100, 100,   0 },
    { "reduce",     LV, OP_REDUCE,      1, AMT_DEFAULT,     2, 256,   4 },
    /* bw is saturate with the sign turned round: bw100 is saturate-100.
     * Two names because bw is what people write. */
    { "bw",         CO, OP_SATURATE,   -1, AMT_DEFAULT,     0, 100, 100 },
    { "saturate",   CO, OP_SATURATE,    1, AMT_REQUIRED, -100, 100,   0 },
    { "hue",        CO, OP_HUE,         1, AMT_REQUIRED,    0, 359,   0 },
    { "dither",     SC, OP_DITHER,      1, AMT_NONE,        0,   0,   0 },
    { "pixellate",  SP, OP_PIXELLATE,   1, AMT_DEFAULT,     2,  64,   8 },
    /* The amount is the box window on the decimated picture, so 4 is about
     * sixteen output pixels at the divisor an ordinary art box gets, and
     * doubling it doubles the softness. The range stops at BLUR_MAX_TAPS
     * rather than running past it: an amount that cannot move the window is
     * better refused than silently ignored. */
    { "blur",       RS, OP_BLUR,        1, AMT_DEFAULT,     1,  16,   4 },
#undef RS
#undef SP
#undef SC
#undef CO
#undef LV
};

/* Where each channel's levels sit in the table, and how many it has. */
static const struct { unsigned short offset, max; } channels[3] =
{
    { 0,                              LCD_MAX_RED   },
    { LCD_MAX_RED + 1,                LCD_MAX_GREEN },
    { LCD_MAX_RED + LCD_MAX_GREEN + 2, LCD_MAX_BLUE },
};

/* ---------------------------------------------------------------------- *
 * Channel operations                                                     *
 * ---------------------------------------------------------------------- */

/* These run once per level when a chain is compiled -- 128 calls for a whole
 * image -- so they are written for clarity and exactness, not for speed. */

static unsigned clamp_level(int v, unsigned max)
{
    if (v < 0)
        return 0;
    if ((unsigned)v > max)
        return max;
    return (unsigned)v;
}

/* `v` and `max` are both the field scaled by IMG_FILTER_LUT_ONE. Every op
 * below is a ratio of the one to the other, so scaling both leaves the
 * arithmetic untouched -- which is what lets a chain fold without rounding to
 * a level between each pair of filters. OP_REDUCE is the exception and says
 * why. */
static unsigned channel_op(unsigned op, int amount, unsigned v, unsigned max)
{
    switch (op)
    {
    case OP_INVERT:
        return max - v;

    case OP_BRIGHTNESS:
        /* Toward white or toward black, by a proportion of the distance
         * remaining, so neither end can be pushed past itself. */
        if (amount >= 0)
            return clamp_level((int)v + ((int)(max - v) * amount + 50) / 100,
                               max);
        return clamp_level((int)v - ((int)v * -amount + 50) / 100, max);

    case OP_GAIN:
        /* Every channel by the same factor, so the ratio between them -- which
         * is what colour is -- survives the change in brightness. -50 is half
         * as bright, +100 twice.
         *
         * OP_BRIGHTNESS is not this. Its two arms are not mirrors: darkening
         * scales, but lightening slides toward white, which converges the
         * channels and takes the chroma with them. Measured over ten covers,
         * a scrim written that way costs five to ten times the saturation the
         * scaling one does. Brightness keeps that shape because a theme
         * asking for brightness by name is asking to move toward an end of
         * the scale; a scrim is asking to stay out of the way. */
        return clamp_level(((int)v * (100 + amount) + 50) / 100, max);

    case OP_CONTRAST:
    {
        /* Scale the distance from mid-grey by (100+a)/(100-a): +50 trebles
         * it, -50 thirds it, -100 flattens the channel to mid-grey. Doubled
         * throughout so mid-grey is exact on a field with an odd number of
         * levels. */
        const int num = 100 + amount, den = 100 - amount;
        int d = 2 * (int)v - (int)max;

        if (den <= 0)
            return d > 0 ? max : 0;             /* amount 100: a threshold */
        d = d * num / den;
        return clamp_level((d + (int)max + 1) / 2, max);
    }

    case OP_REDUCE:
    {
        /* `amount` levels spread across the field, so the ends land exactly
         * on 0 and max whatever the field is worth. More levels than the
         * field holds is not an error, just nothing to do.
         *
         * Trap: that ceiling counts levels the display can show, so it comes
         * off the unscaled field. Against `max` it is never reached, and
         * `reduce256` on a five-bit channel quantises 32 levels onto a
         * 256-step ladder none of whose steps it lands on -- an identity that
         * arrives about 3% dark. */
        unsigned n = (unsigned)amount, q;
        const unsigned levels = (max >> IMG_FILTER_LUT_FRAC) + 1;

        if (n > levels)
            n = levels;
        if (n < 2)
            n = 2;
        q = (v * (n - 1) + max / 2) / max;
        return (q * max + (n - 1) / 2) / (n - 1);
    }
    }

    return v;
}

/* ---------------------------------------------------------------------- *
 * Colour matrices                                                        *
 * ---------------------------------------------------------------------- */

/* A colour filter mixes the channels, so it cannot be a table. It is a 3x3
 * matrix instead, built here on *normalised* channels -- 0 to 1 for all
 * three, whatever the field is worth -- and rescaled for the real field
 * widths only at the end of folding. Keeping those apart is what lets the
 * matrices multiply together.
 *
 * MQ fractional bits throughout, with 64-bit products, so a chain that
 * multiplies several matrices together loses nothing. All of this runs once
 * per chain. */
#define MQ         14                   /* what fp14_sin/fp14_cos return */
#define MONE       (1 << MQ)
#define ROOT_THIRD 9459                 /* sqrt(1/3) */

static int32_t mq_mul(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> MQ);
}

/* The 77/150/29 luminance weights skin_albumart_color.c uses, at MQ. They
 * sum to exactly MONE, which is what stops a greyscale conversion shifting
 * the overall brightness of the image. */
static const int32_t lum_weight[3] =
    { 77 << (MQ - 8), 150 << (MQ - 8), 29 << (MQ - 8) };

/* Pull each channel toward the pixel's own luminance, or away from it. At
 * s == MONE the pixel is untouched, at 0 it collapses to grey, at 2*MONE the
 * distance doubles. Greyscale is not a special case but this at s == 0,
 * which is why bw and saturate are one operation with two spellings. */
static void mat_saturate(int32_t *m, int32_t s)
{
    for (int o = 0; o < 3; o++)
        for (int i = 0; i < 3; i++)
        {
            int32_t ident = (o == i) ? MONE : 0;
            m[o*3 + i] = lum_weight[i] + mq_mul(s, ident - lum_weight[i]);
        }
}

/* Turn the pixel about the grey axis: a rotation of the (r,g,b) vector about
 * (1,1,1), which is what a hue rotation is. The HSV round trip in
 * draw/color.c would answer the same question per pixel, with a divide and
 * several branches; this answers it once per chain. */
static void mat_hue(int32_t *m, int degrees)
{
    const int32_t cos_a = fp14_cos(degrees);
    const int32_t sin_a = fp14_sin(degrees);
    const int32_t mid   = (MONE - cos_a) / 3;
    const int32_t skew  = mq_mul(ROOT_THIRD, sin_a);
    const int32_t dia   = cos_a + mid;

    m[0] = dia;         m[1] = mid - skew;  m[2] = mid + skew;
    m[3] = mid + skew;  m[4] = dia;         m[5] = mid - skew;
    m[6] = mid - skew;  m[7] = mid + skew;  m[8] = dia;
}

/* out = step * out, so the matrix written first is the one applied first. */
static void mat_compose(int32_t *out, const int32_t *step)
{
    int32_t r[9];

    for (int o = 0; o < 3; o++)
        for (int i = 0; i < 3; i++)
            r[o*3 + i] = mq_mul(step[o*3 + 0], out[0*3 + i])
                       + mq_mul(step[o*3 + 1], out[1*3 + i])
                       + mq_mul(step[o*3 + 2], out[2*3 + i]);
    memcpy(out, r, sizeof r);
}

/* Rescale a normalised coefficient for the fields it will really act on, and
 * drop it to the Q8 the per-pixel loop wants. Green holds six bits where red
 * and blue hold five, so a coefficient reading green is worth half as much,
 * and one writing green twice as much.
 *
 * By the bit widths, not by the ratio of the maximum values -- 63/31 is the
 * more faithful number and it is the wrong one. A greyscale matrix has three
 * equal rows, and only an exact factor of two keeps the green field's answer
 * within half a step of the other two once all three have been rounded. At
 * 63/31 the three land independently, and a neutral grey comes out a whole
 * five-bit step green at some levels. */
static short scale_coeff(int32_t v, int out_bits, int in_bits)
{
    const int shift = MQ - 8 - (out_bits - in_bits);
    int32_t t = (v + (1 << (shift - 1))) >> shift;

    if (t >  32767) t =  32767;
    if (t < -32768) t = -32768;
    return (short)t;
}

/* ---------------------------------------------------------------------- *
 * Folding                                                                *
 * ---------------------------------------------------------------------- */

/* The mean luminance a fixed-target adaptive filter leaves behind, the gap
 * `scrim` keeps between a picture and the text over it, and how far either may
 * scale a picture to get there, as a percentage of what it was.
 *
 * A constant amount is wrong in both directions across a real library: 40%
 * is not enough to read white text over a white sleeve and turns a dark one
 * to mud.
 *
 * The two bounds are not symmetrical because the two directions are not.
 * Scaling down cannot overflow, so the floor is only there to stop a cover
 * being crushed to black, and 31% is the 22/32 the design note settled on.
 * Scaling up runs into the top of the fields, and the three channels arrive
 * there at different gains: past about 2x a dark cover grows flat white areas
 * with a fringe on them, measured over ten covers off the player. Reaching
 * the target matters less than that -- a backdrop that is merely lighter
 * still carries the text. */
#define ADAPT_LUM_MAX  255              /* the scale a mean is measured on */
#define ADAPT_TARGET   80
#define ADAPT_GAP      (ADAPT_LUM_MAX - ADAPT_TARGET)
#define ADAPT_FLOOR    31
#define ADAPT_CEIL     200

/* How far to scale a picture of this mean to land it on `target`, as an amount
 * for OP_GAIN. One expression for both directions, which is what scaling buys
 * over sliding toward an end of the scale: the factor is simply the ratio of
 * where the picture should be to where it is.
 *
 * The division truncates, so a picture lands on the target or a shade past it
 * and never short of it -- the safe side when the point is text staying
 * legible. */
static int scrim_amount(int mean, int target)
{
    int a;

    if (mean <= 0)
        return 0;                       /* nothing to scale */
    if (target < 0)
        target = 0;                     /* a target past the end of the scale */

    a = target * 100 / mean - 100;
    if (a < ADAPT_FLOOR - 100)
        a = ADAPT_FLOOR - 100;
    else if (a > ADAPT_CEIL - 100)
        a = ADAPT_CEIL - 100;
    return a;
}

/* What an adaptive filter does to this picture, as a signed amount.
 *
 * `avoid` is the luminance the picture has to stay clear of -- what text over
 * it will be drawn in -- or -1 when the caller named none. `scrim` is the
 * only filter here that reads it, and the only one that can go either way: it
 * moves the picture away from `avoid` -- but only ever downward, and only as
 * far as it has to. `lighter` and `darker` each hold to a fixed target and
 * move one way, so a cover already past it is left alone rather than dragged
 * back.
 *
 * `scrim` stands down entirely for dark text rather than lightening to meet
 * it, because dark text is itself evidence that the artwork is already light.
 * A theme's text colour on this build is the palette's accent, and
 * skin_albumart_color.c derives that to contrast with the artwork's dominant
 * colour -- it goes dark only when the dominant is light. So a picture facing
 * dark text has already been contrasted against, and lifting it again is the
 * same compensation twice: it costs the artwork its colour for nothing, since
 * raising a picture's black point drives every channel toward the top of its
 * field and the chroma goes with them. A theme that wants a lift anyway can
 * still name `lighter`.
 *
 * Darkening works from the ninth decile rather than the mean. Text is read
 * against the brightest band it crosses, and on a real sleeve the two are far
 * apart -- one averaging 132 has a ninth decile of 199, so aiming at the mean
 * leaves the band under the title half as bright again as it was asked to
 * be. */
static int adaptive_amount(const struct filter_def *d,
                           const struct img_levels *lv, int avoid)
{
    bool darken;
    int a;

    if (d->amt == AMT_DERIVED)
    {
        /* Nothing named to avoid: fall back to what `darker` assumes, which
         * is light text -- the common case, and the one a theme gets by
         * default. */
        if (avoid < 0)
            avoid = ADAPT_LUM_MAX;

        if (2 * avoid < ADAPT_LUM_MAX)
            return 0;                   /* dark text: nothing to do */
        darken = true;
        a = scrim_amount(lv->bright, avoid - ADAPT_GAP);
    }
    else
    {
        darken = d->sign < 0;
        a = scrim_amount(darken ? lv->bright : lv->mean,
                         darken ? ADAPT_TARGET : ADAPT_GAP);
    }

    /* One way only, whichever way was chosen. The target is the least a
     * picture has to move, not where it belongs: one already clear of the
     * text is left as it is rather than dragged back toward it. */
    if (darken)
        return a < 0 ? a : 0;
    return a > 0 ? a : 0;
}

/* Compose every levels filter in the chain into one table, in the order the
 * chain wrote them, then decide what the result is worth running.
 *
 * `mean` is the picture's mean luminance, or -1 when no picture is in hand.
 * Without one an adaptive filter contributes nothing -- its amount is not
 * known until an image arrives -- so the table it leaves is a placeholder and
 * the stage stays claimed even when the placeholder is the identity. */
static void fold_levels(struct img_filter *f,
                        const struct img_levels *lv, int avoid)
{
    bool identity = true, complement = true, has_levels = false;
    bool fractional = false;

    /* Whether the chain names a levels filter at all. The stage bit is
     * dropped below when the folded table turns out to do nothing, so it
     * cannot be read back on a later fold -- this can. */
    for (int i = 0; i < f->nsteps && !has_levels; i++)
        has_levels = filters[f->steps[i].def].cls == IMG_CLASS_LEVELS;

    for (int c = 0; c < 3; c++)
    {
        unsigned short *t = f->lut + channels[c].offset;
        const unsigned max = channels[c].max;

        /* The whole fold runs on the scaled field, so no filter's output is
         * rounded to a level before the next one reads it. */
        const unsigned smax = max << IMG_FILTER_LUT_FRAC;

        for (unsigned v = 0; v <= max; v++)
            t[v] = (unsigned short)(v << IMG_FILTER_LUT_FRAC);

        for (int i = 0; i < f->nsteps; i++)
        {
            const struct filter_def *d = &filters[f->steps[i].def];
            int amount;

            if (d->cls != IMG_CLASS_LEVELS)
                continue;
            if (f->steps[i].adaptive)
            {
                if (!lv)
                    continue;
                amount = adaptive_amount(d, lv, avoid);
            }
            else
                amount = d->sign * f->steps[i].amount;

            for (unsigned v = 0; v <= max; v++)
                t[v] = (unsigned short)channel_op(d->op, amount, t[v], smax);
        }

        /* What the finished table is, in the three terms the passes ask
         * about: is it the identity, is it the complement, and has the dither
         * anything to work with -- a fraction to round, or a run to jitter
         * across. */
        unsigned run = 1, span = 1;

        for (unsigned v = 0; v <= max; v++)
        {
            if (t[v] != (v << IMG_FILTER_LUT_FRAC))
                identity = false;
            if (t[v] != smax - (v << IMG_FILTER_LUT_FRAC))
                complement = false;
            if (t[v] & (IMG_FILTER_LUT_ONE - 1))
                fractional = true;

            if (v > 0 && t[v] == t[v-1])
                run++;
            else
                run = 1;
            if (run > span)
                span = run;
        }
        f->lut_span[c] = (unsigned char)span;
    }

    f->levels_complement = complement;
    f->levels_fractional = fractional;

    /* A table that does nothing costs a pass to prove it, so the stage goes.
     * An unresolved adaptive one is the exception: its table is a placeholder
     * and says nothing yet about what the stage will be worth. Set as well as
     * cleared, so a picture that wants a scrim gets the stage back after one
     * that wanted none took it away. */
    if (has_levels && !(identity && (lv || !f->has_adaptive)))
        f->stages |= IMG_CLASS_LEVELS;
    else
        f->stages &= ~IMG_CLASS_LEVELS;
}

void img_filter_adapt(struct img_filter *f, const struct img_levels *lv,
                      int avoid)
{
    if (!f || !f->has_adaptive || !lv)
        return;
    if (avoid > ADAPT_LUM_MAX)
        avoid = ADAPT_LUM_MAX;          /* negative stays: it means "none" */

    fold_levels(f, lv, avoid);
}

/* The same for the colour stage: multiply every colour filter's matrix
 * together in the order the chain wrote them, then rescale once. */
static void fold_colour(struct img_filter *f)
{
    static const int chan_bits[3] =
        { LCD_RED_BITS, LCD_GREEN_BITS, LCD_BLUE_BITS };
    int32_t m[9] = { MONE, 0, 0,  0, MONE, 0,  0, 0, MONE };
    bool identity = true;

    for (int i = 0; i < f->nsteps; i++)
    {
        const struct filter_def *d = &filters[f->steps[i].def];
        int32_t step[9];
        int amount;

        if (d->cls != IMG_CLASS_COLOUR)
            continue;
        amount = d->sign * f->steps[i].amount;

        if (d->op == OP_HUE)
            mat_hue(step, amount);
        else
            mat_saturate(step, MONE + (MONE * amount) / 100);
        mat_compose(m, step);
    }

    /* Three identical rows means every pixel lands on a grey, whatever else
     * the chain did on the way -- a hue rotation of a greyscale image is
     * still greyscale, and the folded matrix says so. */
    f->colour_neutral = !memcmp(m, m + 3, 3 * sizeof m[0])
                     && !memcmp(m, m + 6, 3 * sizeof m[0]);

    for (int o = 0; o < 3; o++)
        for (int i = 0; i < 3; i++)
        {
            short c = scale_coeff(m[o*3 + i], chan_bits[o], chan_bits[i]);

            f->matrix[o*3 + i] = c;
            if (c != (o == i ? 256 : 0))
                identity = false;
        }

    if (identity)
        f->stages &= ~IMG_CLASS_COLOUR;
}

/* ---------------------------------------------------------------------- *
 * The parser                                                             *
 * ---------------------------------------------------------------------- */

static bool fail(struct img_filter *f, const char *name, const char *reason)
{
    if (name)
        snprintf(f->error, sizeof f->error, "%s: %s", name, reason);
    else
        snprintf(f->error, sizeof f->error, "%s", reason);
    /* Leave nothing a caller that ignores the return value could act on. */
    f->stages = 0;
    f->nsteps = 0;
    f->has_adaptive = false;
    return false;
}

bool img_filter_compile(const char *spec, unsigned allowed,
                        struct img_filter *out)
{
    const char *p = spec;

    memset(out, 0, sizeof *out);
    if (!p || !*p)
        return true;

    while (*p)
    {
        char name[NAME_MAX_LEN];
        const struct filter_def *d = NULL;
        unsigned idx = 0;
        size_t len = 0;
        bool have_amount = false, negative = false, adaptive = false;
        int amount = 0;

        while (isalpha((unsigned char)*p) && len < sizeof name - 1)
            name[len++] = *p++;
        name[len] = '\0';

        if (len == 0)
            return fail(out, NULL, "expected a filter name");
        if (isalpha((unsigned char)*p))         /* longer than any name */
            return fail(out, name, "unknown filter");

        /* Only '-' can introduce an amount: '+' is the chain separator, so a
         * positive amount is written bare. */
        if (*p == '-')
        {
            negative = true;
            p++;
        }
        while (isdigit((unsigned char)*p))
        {
            if (amount < 100000)
                amount = amount * 10 + (*p - '0');
            have_amount = true;
            p++;
        }
        if (negative)
        {
            if (!have_amount)
                return fail(out, name, "expected an amount after '-'");
            amount = -amount;
        }

        if (*p == '+')
        {
            p++;
            if (!*p)
                return fail(out, NULL, "chain ends with '+'");
        }
        else if (*p)
            return fail(out, name, "unexpected text after the filter");

        for (unsigned i = 0; i < ARRAYLEN(filters); i++)
            if (!strcmp(name, filters[i].name))
            {
                d = &filters[i];
                idx = i;
                break;
            }

        if (!d)
            return fail(out, name, "unknown filter");
        if (!(d->cls & allowed))
            return fail(out, name, "not permitted here");

        switch (d->amt)
        {
        case AMT_NONE:
            if (have_amount)
                return fail(out, name, "takes no amount");
            break;
        case AMT_REQUIRED:
            if (!have_amount)
                return fail(out, name, "needs an amount");
            break;
        case AMT_DEFAULT:
            if (!have_amount)
                amount = d->def;
            break;
        case AMT_ADAPTIVE:
            adaptive = !have_amount;
            break;
        case AMT_DERIVED:
            if (have_amount)
                return fail(out, name, "takes no amount");
            adaptive = true;
            break;
        }

        /* Refused rather than compiled and left inert. `lighter` and `darker`
         * are still available here with an amount written, which is why the
         * two are told apart -- one is a fixable spelling, the other is a
         * filter this caller cannot offer at all. */
        if (adaptive && !(allowed & IMG_ALLOW_ADAPTIVE))
            return fail(out, name, d->amt == AMT_ADAPTIVE
                                     ? "needs an amount here"
                                     : "not permitted here");

        if (!adaptive && d->amt != AMT_NONE &&
            (amount < d->min || amount > d->max))
            return fail(out, name, "amount out of range");

        if (out->nsteps == IMG_FILTER_MAX_CHAIN)
            return fail(out, NULL, "too many filters in the chain");

        out->steps[out->nsteps].def      = idx;
        out->steps[out->nsteps].adaptive = adaptive;
        out->steps[out->nsteps].amount   = (short)amount;
        out->nsteps++;
        out->stages |= d->cls;
        out->has_adaptive |= adaptive;
    }

    if (out->stages & IMG_CLASS_COLOUR)
        fold_colour(out);
    if (out->stages & IMG_CLASS_LEVELS)
        fold_levels(out, NULL, -1);

    /* The spatial filters do not fold -- two blurs, or two block sizes, in
     * one chain is not a thing anyone means -- so the larger wins. */
    for (int i = 0; i < out->nsteps; i++)
    {
        const struct filter_def *d = &filters[out->steps[i].def];
        const short amount = out->steps[i].amount;

        if (d->op == OP_BLUR && amount > (short)out->blur_radius)
            out->blur_radius = (unsigned char)amount;
        if (d->op == OP_PIXELLATE && amount > (short)out->pixel_block)
            out->pixel_block = (unsigned char)amount;
    }

    /* Dither is not a pass of its own -- it is the rounding done by the
     * colour and levels stages, and by the blur's upscale (see "the
     * passes"). With none of the three left there is nothing for it to
     * round, so the chain really is empty. */
    if (!(out->stages & (IMG_CLASS_COLOUR | IMG_CLASS_LEVELS
                                          | IMG_CLASS_RESIZE)))
        out->stages &= ~IMG_CLASS_SCREEN;
    return true;
}

/* ---------------------------------------------------------------------- *
 * The passes                                                             *
 * ---------------------------------------------------------------------- */

#define CLAMP_FIELD(v, max) \
    ((v) < 0 ? 0u : ((unsigned)(v) > (max) ? (unsigned)(max) : (unsigned)(v)))

/* Dither has no pass of its own, and cannot have one.
 *
 * It is a quantisation step, and a quantisation step is only worth anything
 * where the precision is about to be lost. By the time a stage has written
 * its pixel back the value is already five or six bits and the detail the
 * dither would have spread out is gone; a pass after that could only add
 * noise. So `dither` compiles into the two stages that quantise -- which is
 * the same fusion §5.4 of the design note describes for the blurred case,
 * where the matrix, the table and the dither all apply to the pixel on its
 * way out.
 *
 * Both reach it the same way, because both carry eight fractional bits into a
 * shift: the Bayer cell replaces the fixed rounding constant. Exact, and
 * free.
 *
 * Trap: the fraction has to survive the fold to be worth rounding. Round each
 * filter's output to a level before the next one runs and the table arrives
 * here exact, with nothing left for a cell to move -- the dither then costs a
 * pass and changes almost no pixel. Over 470 covers that difference is a
 * banding figure of 1.23 against 0.14, and on the worst cover the exact table
 * dithers to something worse than not dithering at all.
 *
 * Rounding the output is not the whole of it, because it cannot break a run:
 * where the table sends several inputs to one value, every one of them rounds
 * the same way. That is `reduce`, which is asked for by name, and there the
 * value is also jittered on the way *in*, across `lut_span` levels. A gain
 * never needs it -- it compresses, but each input still lands on its own
 * fraction, so the span is 1 and BAYER_JITTER returns zero. Doing both to a
 * gain anyway is measurably worse than doing neither: 1.16 banding, and the
 * worst grain of any option tried.
 */
static const unsigned char bayer8[64] =
{
     0, 32,  8, 40,  2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37,
    63, 31, 55, 23, 61, 29, 53, 21
};

/* A Bayer cell as a rounding constant: 2..254 against the fixed 128, so the
 * mean is unchanged and the image does not drift lighter or darker. */
#define BAYER_ROUND(b)  (((b) << 2) + 2)

/* A Bayer cell as a signed jitter across `span` input levels, zero-mean. A
 * span of 1 gives zero, which is what keeps this off every table that has no
 * run in it without a test. */
#define BAYER_JITTER(b, span)  ((((b) * (span)) >> 6) - ((span) >> 1))

/* ---------------------------------------------------------------------- *
 * One pixel at a time                                                    *
 * ---------------------------------------------------------------------- */

/* Each stage's arithmetic lives here once, so the dithered and plain loops
 * below differ only in where the rounding comes from. FORCE_INLINE, not a
 * plain `inline`: the build is -Os, where GCC declines to inline anything
 * with two callers, and it turned levels_pixel into a real call with its six
 * arguments pushed on the stack -- 11 instructions a pixel became a function
 * call per pixel. */

/* Greyscale, the shape §6 of the design note settles on: one luminance, with
 * the other two channels derived from it rather than computed on their own.
 *
 * Green holds a bit more than red and blue, so computing it independently
 * lets it land on the step below the neutral one -- half a five-bit step of
 * magenta, invisible by itself and plainly visible once `reduce` posterises
 * it into a solid patch. Deriving green from the same y cannot do that.
 *
 * The shifts are the 5-to-6-bit relation inverted, written from the field
 * widths so a display with different ones still gets a neutral answer. */
static FORCE_INLINE unsigned grey_pixel(unsigned p, int m0, int m1, int m2,
                                        int rnd)
{
    int y = (m0 * (int)RGB_UNPACK_RED_LCD(p)
           + m1 * (int)RGB_UNPACK_GREEN_LCD(p)
           + m2 * (int)RGB_UNPACK_BLUE_LCD(p) + rnd) >> 8;
    unsigned v = CLAMP_FIELD(y, LCD_MAX_RED);
    unsigned g = (v << (LCD_GREEN_BITS - LCD_RED_BITS))
               | (v >> (2 * LCD_RED_BITS - LCD_GREEN_BITS));

    return LCD_RGBPACK_LCD(v, g, v);
}

static FORCE_INLINE unsigned matrix_pixel(unsigned p, const short *m, int rnd)
{
    int r = (int)RGB_UNPACK_RED_LCD(p);
    int g = (int)RGB_UNPACK_GREEN_LCD(p);
    int b = (int)RGB_UNPACK_BLUE_LCD(p);
    /* Rounded rather than truncated. Three truncations bias the whole image
     * down by half a level each, and it is what would otherwise leave white
     * one step short of white. */
    int nr = (m[0]*r + m[1]*g + m[2]*b + rnd) >> 8;
    int ng = (m[3]*r + m[4]*g + m[5]*b + rnd) >> 8;
    int nb = (m[6]*r + m[7]*g + m[8]*b + rnd) >> 8;

    /* Clamped because these really do leave the field: a saturation boost
     * pushes a vivid colour past the end of its channel, and a hue rotation
     * can drive one below zero. */
    return LCD_RGBPACK_LCD(CLAMP_FIELD(nr, LCD_MAX_RED),
                           CLAMP_FIELD(ng, LCD_MAX_GREEN),
                           CLAMP_FIELD(nb, LCD_MAX_BLUE));
}

/* `rnd` is the constant the table's fraction is rounded with -- 128 for
 * round-to-nearest, a Bayer cell for the dithered loops. `jr`/`jg`/`jb` move
 * the value along the table on the way in, which only a table with a run in
 * it needs; BAYER_JITTER of a span of 1 is zero, so the ordinary case pays an
 * add rather than a branch. The table is already clamped to the field, so
 * nothing needs clamping on the way out: the largest entry plus the largest
 * cell still rounds down to `max`. */
static FORCE_INLINE unsigned levels_pixel(unsigned p, const unsigned short *lr,
                                    const unsigned short *lg,
                                    const unsigned short *lb, int rnd,
                                    int jr, int jg, int jb)
{
    int ir = (int)RGB_UNPACK_RED_LCD(p)   + jr;
    int ig = (int)RGB_UNPACK_GREEN_LCD(p) + jg;
    int ib = (int)RGB_UNPACK_BLUE_LCD(p)  + jb;
    unsigned r = lr[CLAMP_FIELD(ir, LCD_MAX_RED)]   + rnd;
    unsigned g = lg[CLAMP_FIELD(ig, LCD_MAX_GREEN)] + rnd;
    unsigned b = lb[CLAMP_FIELD(ib, LCD_MAX_BLUE)]  + rnd;

    return LCD_RGBPACK_LCD(r >> IMG_FILTER_LUT_FRAC,
                           g >> IMG_FILTER_LUT_FRAC,
                           b >> IMG_FILTER_LUT_FRAC);
}

/* ---------------------------------------------------------------------- *
 * The passes                                                             *
 * ---------------------------------------------------------------------- */

/* Plain and dithered are separate loops rather than one loop with a test,
 * because these run tens of thousands of times per image and the build is
 * -Os, which does not lift a loop-invariant branch out for itself. */

static void greyscale_pass(fb_data *px, size_t n, const short *m)
{
    /* Coefficients into locals before the loop. fb_data and the matrix are
     * both 16 bits wide, so the compiler cannot prove that storing a pixel
     * leaves the matrix alone; left in memory they are reloaded on every
     * pixel. Three of them fit in registers and this loop then has no memory
     * traffic but the pixels -- 22 ARM instructions each, against 53 for the
     * general form below. */
    const int m0 = m[0], m1 = m[1], m2 = m[2];

    while (n--)
    {
        unsigned p = FB_UNPACK_SCALAR_LCD(*px);
        *px++ = FB_SCALARPACK_LCD(grey_pixel(p, m0, m1, m2, 128));
    }
}

static void greyscale_dither_pass(fb_data *px, int w, int h, const short *m)
{
    const int m0 = m[0], m1 = m[1], m2 = m[2];

    for (int y = 0; y < h; y++)
    {
        const unsigned char *brow = bayer8 + ((y & 7) << 3);

        for (int x = 0; x < w; x++, px++)
        {
            unsigned p = FB_UNPACK_SCALAR_LCD(*px);
            *px = FB_SCALARPACK_LCD(
                      grey_pixel(p, m0, m1, m2, BAYER_ROUND(brow[x & 7])));
        }
    }
}

/* The general case. Nine coefficients, three channels, a pointer and a
 * counter do not fit in the register file, so unlike greyscale_pass this
 * loop reloads coefficients whatever is done to it -- hoisting them into
 * locals only moves the reloads from the matrix to the stack. Measured at
 * 53 ARM instructions a pixel either way. */
static void colour_pass(fb_data *px, size_t n, const short *m)
{
    while (n--)
    {
        unsigned p = FB_UNPACK_SCALAR_LCD(*px);
        *px++ = FB_SCALARPACK_LCD(matrix_pixel(p, m, 128));
    }
}

static void colour_dither_pass(fb_data *px, int w, int h, const short *m)
{
    for (int y = 0; y < h; y++)
    {
        const unsigned char *brow = bayer8 + ((y & 7) << 3);

        for (int x = 0; x < w; x++, px++)
        {
            unsigned p = FB_UNPACK_SCALAR_LCD(*px);
            *px = FB_SCALARPACK_LCD(
                      matrix_pixel(p, m, BAYER_ROUND(brow[x & 7])));
        }
    }
}

static void levels_pass(fb_data *px, size_t n, const struct img_filter *f)
{
    const unsigned short *lr = f->lut + channels[0].offset;
    const unsigned short *lg = f->lut + channels[1].offset;
    const unsigned short *lb = f->lut + channels[2].offset;

    if (f->levels_complement)
    {
        /* The fields are contiguous, so one XOR complements all three. */
        while (n--)
        {
            unsigned p = FB_UNPACK_SCALAR_LCD(*px);
            *px++ = FB_SCALARPACK_LCD(p ^ FIELD_MASK);
        }
        return;
    }

    while (n--)
    {
        unsigned p = FB_UNPACK_SCALAR_LCD(*px);
        *px++ = FB_SCALARPACK_LCD(
                    levels_pixel(p, lr, lg, lb, IMG_FILTER_LUT_ONE / 2,
                                 0, 0, 0));
    }
}

static void levels_dither_pass(fb_data *px, int w, int h,
                               const struct img_filter *f)
{
    const unsigned short *lr = f->lut + channels[0].offset;
    const unsigned short *lg = f->lut + channels[1].offset;
    const unsigned short *lb = f->lut + channels[2].offset;
    const int sr = f->lut_span[0], sg = f->lut_span[1], sb = f->lut_span[2];

    for (int y = 0; y < h; y++)
    {
        const unsigned char *brow = bayer8 + ((y & 7) << 3);

        for (int x = 0; x < w; x++, px++)
        {
            const int d = brow[x & 7];
            unsigned p = FB_UNPACK_SCALAR_LCD(*px);

            *px = FB_SCALARPACK_LCD(
                      levels_pixel(p, lr, lg, lb, BAYER_ROUND(d),
                                   BAYER_JITTER(d, sr), BAYER_JITTER(d, sg),
                                   BAYER_JITTER(d, sb)));
        }
    }
}

/* Flatten the picture into blocks: read a block, average it, write it back
 * solid. In place, one pass, no scratch -- which is what puts it on the
 * cached-art tier while blur, which is a resize, stays off it. Blocks do not
 * overlap, so writing one cannot disturb one not yet read; there is no
 * ordering constraint and no second buffer.
 *
 * Unpacked accumulation, like the blur's decimate(). */

/* Dividing the three sums by the block's pixel count is what a small block
 * costs nearly all of: neither target has a divider, so each division is a
 * call, and at the shortest block that is three of them for every four pixels
 * -- four fifths of the pass, and four times what a whole `bw` costs. One
 * reciprocal per band brings a block down to three multiplies.
 *
 * Exact, not an approximation. Rounding the reciprocal up makes
 * (v * recip) >> MEAN_SHIFT equal v / n for every v short of a bound, and
 * MEAN_MAX_N is where that bound bites: a channel sums at most 63 a pixel, so
 * the condition is 63n(n-1) < 2^MEAN_SHIFT. The shift is as large as the
 * 32-bit product allows, which is what puts the cap as high as it is.
 *
 * Past the cap a block is 23 pixels square or more, and three divisions spread
 * over 529 pixels do not show. Those divide, and so does the one block a band
 * has clipped by the right edge -- along with the whole of a band clipped by
 * the bottom. */
#define MEAN_SHIFT  24
#define MEAN_MAX_N  512

static unsigned mean_recip(unsigned n)
{
    return n <= MEAN_MAX_N ? ((1u << MEAN_SHIFT) + n - 1) / n : 0;
}

static void pixellate_pass(fb_data *px, int w, int h, int block)
{
    for (int y = 0; y < h; y += block)
    {
        const int bh = MIN(block, h - y);
        /* Every block of this band but the last holds the same count. */
        const unsigned full_n = (unsigned)block * bh;
        const unsigned full_recip = mean_recip(full_n);

        for (int x = 0; x < w; x += block)
        {
            const int bw = MIN(block, w - x);
            unsigned r = 0, g = 0, b = 0;
            unsigned n = (unsigned)bw * bh;
            unsigned recip = n == full_n ? full_recip : 0;
            unsigned flat;

            for (int j = 0; j < bh; j++)
            {
                const fb_data *row = px + (size_t)(y + j) * w + x;

                for (int i = 0; i < bw; i++)
                {
                    unsigned p = FB_UNPACK_SCALAR_LCD(row[i]);

                    r += RGB_UNPACK_RED_LCD(p);
                    g += RGB_UNPACK_GREEN_LCD(p);
                    b += RGB_UNPACK_BLUE_LCD(p);
                }
            }

            flat = recip ? LCD_RGBPACK_LCD((r * recip) >> MEAN_SHIFT,
                                           (g * recip) >> MEAN_SHIFT,
                                           (b * recip) >> MEAN_SHIFT)
                         : LCD_RGBPACK_LCD(r / n, g / n, b / n);
            for (int j = 0; j < bh; j++)
            {
                fb_data *row = px + (size_t)(y + j) * w + x;

                for (int i = 0; i < bw; i++)
                    row[i] = FB_SCALARPACK_LCD(flat);
            }
        }
    }
}

/* Is there anything for the dither to do to the levels stage? Either half of
 * it will serve: a fraction to round, or a run to jitter across. A table with
 * neither is exact at every level, and the dithered loop would write the same
 * pixels the plain one does. */
static bool levels_worth_dithering(const struct img_filter *f)
{
    return f->levels_fractional
        || f->lut_span[0] > 1 || f->lut_span[1] > 1 || f->lut_span[2] > 1;
}

/* The 77/150/29 luminance weights skin_albumart_color.c extracts a palette
 * with, rescaled so they act on field values directly and a pixel costs three
 * multiplies rather than three divisions into 8-bit channels. The +128 is the
 * rounding that brings white out at exactly 255. */
#define LUM_W(w, m)  ((255 * (w) + (m) / 2) / (m))
#define LUM_RED      LUM_W(77,  LCD_MAX_RED)
#define LUM_GREEN    LUM_W(150, LCD_MAX_GREEN)
#define LUM_BLUE     LUM_W(29,  LCD_MAX_BLUE)

/* Four luminance levels to a bucket: enough to place a percentile within a
 * step nothing can see, and 64 counters of stack rather than 256.
 *
 * Trap: the counters have to be as wide as the pixel count. A 16-bit one
 * saturates at 65535, which is below the ninth decile of a full-screen
 * picture -- the walk below then never reaches `want`, falls out of the loop
 * and leaves `bright` at zero, so a flat white cover reports as having no
 * bright band and gets no scrim at all. */
#define LEVEL_BUCKETS 64
#define LEVEL_SHIFT   2

void img_filter_measure(const fb_data *px, int w, int h,
                        struct img_levels *out)
{
    uint32_t hist[LEVEL_BUCKETS];
    uint32_t total = 0, n, seen = 0, want;

    if (!out)
        return;
    out->mean = out->bright = 0;
    if (!px || w <= 0 || h <= 0)
        return;

    memset(hist, 0, sizeof hist);
    n = (uint32_t)w * h;

    for (uint32_t i = 0; i < n; i++)
    {
        unsigned p = FB_UNPACK_SCALAR_LCD(px[i]);
        unsigned lum = (RGB_UNPACK_RED_LCD(p)   * LUM_RED
                      + RGB_UNPACK_GREEN_LCD(p) * LUM_GREEN
                      + RGB_UNPACK_BLUE_LCD(p)  * LUM_BLUE + 128) >> 8;

        total += lum;
        hist[lum >> LEVEL_SHIFT]++;
    }

    out->mean = (short)(total / n);

    /* The top of the bucket the ninth decile falls in, so `bright` is never
     * an under-estimate of what the text has to be read against. */
    want = n - n / 10;
    for (int b = 0; b < LEVEL_BUCKETS; b++)
        if ((seen += hist[b]) >= want)
        {
            out->bright = (short)(((b + 1) << LEVEL_SHIFT) - 1);
            break;
        }
}

void img_filter_apply(fb_data *px, int w, int h, const struct img_filter *f)
{
    size_t n;
    bool dither;

    if (!px || !f || !f->stages || w <= 0 || h <= 0)
        return;
    n = (size_t)w * h;
    dither = (f->stages & IMG_CLASS_SCREEN) != 0;

    /* Canonical order -- spatial, colour, levels, dither -- not the order the
     * chain named them in. Fixing it between stages is what lets the filters
     * within a stage fold together. Dither is last in that list and is not a
     * pass: it is folded into the two colour and levels loops, for the reason
     * given above the Bayer table. */
    if (f->stages & IMG_CLASS_SPATIAL)
        pixellate_pass(px, w, h, f->pixel_block);

    if (f->stages & IMG_CLASS_COLOUR)
    {
        if (f->colour_neutral)
        {
            if (dither)
                greyscale_dither_pass(px, w, h, f->matrix);
            else
                greyscale_pass(px, n, f->matrix);
        }
        else if (dither)
            colour_dither_pass(px, w, h, f->matrix);
        else
            colour_pass(px, n, f->matrix);
    }

    if (f->stages & IMG_CLASS_LEVELS)
    {
        if (dither && !f->levels_complement && levels_worth_dithering(f))
            levels_dither_pass(px, w, h, f);
        else
            levels_pass(px, n, f);
    }
}

/* Enough pixels per band to keep the per-band overhead irrelevant, few enough
 * that no band holds the CPU for long. */
#define IMG_FILTER_BAND_PIXELS 4096

void img_filter_apply_banded(fb_data *px, int w, int h,
                             const struct img_filter *f)
{
    int rows;

    if (!f || !f->stages || w <= 0 || h <= 0)
        return;

    rows = IMG_FILTER_BAND_PIXELS / w;
    if (rows < 1)
        rows = 1;

    for (int y = 0; y < h; y += rows)
    {
        int band = MIN(rows, h - y);

        img_filter_apply(px + (size_t)y * w, w, band, f);
        if (y + band < h)
            yield();
    }
}

/* ---------------------------------------------------------------------- *
 * The blur                                                               *
 * ---------------------------------------------------------------------- */

/* A running-sum box blur costs the same whatever its radius, so radius is
 * free and only the pixel count is not. Blurring at full size is still the
 * wrong shape: many passes over many pixels to make something with almost no
 * detail left in it. So the picture is decimated first, blurred small, and
 * scaled back up:
 *
 *      decimate  ->  small box blur  ->  bilinear upscale, everything fused
 *
 * The decimation is the reason a blurred chain is the *cheapest* thing here
 * rather than the dearest: the blur itself runs over a thousand pixels
 * instead of sixty thousand, and the rest of the chain rides out on the
 * upscale's write.
 *
 * Nothing is cropped -- the whole picture is decimated, so the result does
 * not depend on the geometry the caller chose. */

/* Spread a packed pixel so all three channels sit in one word with room to
 * add. Blue lands at bits 0-4, red at 11-15, green at 21-26; the gaps are
 * what let a sum of up to 32 of them stay separable, and what let a shift
 * divide all three at once. */
#define SMASK       0x07E0F81FU
#define SPREAD(p)   ((((uint32_t)(p)) | (((uint32_t)(p)) << 16)) & SMASK)
#define COLLAPSE(s) ((uint16_t)(((s) | ((s) >> 16)) & 0xFFFFU))

/* One in each channel's field, so d * SPREAD_UNIT adds d to all three. */
#define SPREAD_UNIT 0x00200801U

/* Weights stay 5-bit (0..32). A 6-bit weight overflows green into nothing.
 *
 * `d` is a 5-bit ordered-dither cell, and it is not a refinement -- without it
 * the upscale blocks up. The shift throws away five fractional bits, and a
 * blurred picture is precisely the case where they carry the whole signal:
 * neighbouring source pixels differ by one level, so every interpolated value
 * between them truncates back to the left-hand one and each source pixel
 * becomes a flat div-wide block with the entire step at its edge. Dithering
 * the fraction instead spreads that step across the block. Zero-mean, so the
 * picture does not drift, and it is the same trick the colour stage uses on
 * its own shift.
 *
 * The dither is the last thing this word has room for: green's term alone
 * reaches 63<<26, and with d at 31 in all three fields the sum peaks at
 * 4,294,966,271 -- 1024 short of overflowing. Widening any field, or the
 * weight, needs a wider accumulator first. */
#define LERP(a, b, w, d) \
    ((((a) * (32u - (w)) + (b) * (w) + (d) * SPREAD_UNIT) >> 5) & SMASK)

/* The box window is what the chain's amount selects: `blur4` is a four-pixel
 * window on the decimated picture, and since that picture is `div` times
 * smaller than the finished one, the softness it produces is four times `div`
 * output pixels. Doubling the amount doubles the softness.
 *
 * The floor is not a rounding convenience. Below two taps the box stops
 * running and the upscale's facet lattice comes back -- the faint diamond grid
 * bilinear leaves at source-pixel boundaries, because it is continuous and its
 * gradient is not. Two taps is the smallest window that takes it off.
 *
 * Sixteen at the top so the amount keeps meaning something to the end of its
 * range rather than saturating in the middle of it. */
#define BLUR_MIN_TAPS 2
#define BLUR_MAX_TAPS 16
#define BLUR_PASSES   2
#define BLUR_MAX_DIV  16

/* The decimated picture the blur works on. There is no allocator down here,
 * so this is a fixed static and a chain asking for less decimation than it
 * allows gets more.
 *
 * Square, at the screen's longer side over four, so a destination of any size
 * up to the whole screen reaches a divisor of 4. Sizing it for a divisor of 8
 * instead saves 10 KB and costs the feature its point: a 240x240 panel is then
 * built from a 30x30 source whatever radius it asked for, which is a colour
 * wash rather than a blurred cover, and the radius stops meaning anything. */
#define BLUR_MAX_W ((MAX(LCD_WIDTH, LCD_HEIGHT) + 3) / 4)
#define BLUR_MAX_H BLUR_MAX_W

static fb_data blur_buf[BLUR_MAX_W * BLUR_MAX_H];

int img_filter_source_divisor(const struct img_filter *f, int w, int h)
{
    int d = 1;

    if (!f || !(f->stages & IMG_CLASS_RESIZE) || w <= 0 || h <= 0)
        return 1;

    /* As little as the working buffer will accept, so the picture keeps every
     * pixel there is room for.
     *
     * Trap: the amount must not come into it. It sets the window, which is the
     * thing it is named for, and deriving the divisor from it as well decides
     * how blurred a picture is twice over -- with the buffer overriding the
     * answer at any useful size. At 240x240 that clamps every amount from 1 to
     * 4 onto the same divisor, and three quarters of the range does nothing.
     *
     * A power of two, which keeps the upscale's weights an exact table. */
    while (d < BLUR_MAX_DIV
           && ((w + d - 1) / d > BLUR_MAX_W || (h + d - 1) / d > BLUR_MAX_H))
        d <<= 1;
    return d;
}

/* Area-average src down to cw x ch. Unpacked rather than spread: the block
 * can be any shape, so the count is not a power of two and a shift will not
 * divide it. It runs once over the source, once per picture. */
static void decimate(fb_data *dst, int cw, int ch,
                     const fb_data *src, int sw, int sh)
{
    for (int oy = 0; oy < ch; oy++)
    {
        const int y0 = oy * sh / ch;
        const int y1 = MAX((oy + 1) * sh / ch, y0 + 1);

        for (int ox = 0; ox < cw; ox++)
        {
            const int x0 = ox * sw / cw;
            const int x1 = MAX((ox + 1) * sw / cw, x0 + 1);
            unsigned r = 0, g = 0, b = 0, n = 0;

            for (int y = y0; y < y1 && y < sh; y++)
            {
                const fb_data *row = src + (size_t)y * sw;

                for (int x = x0; x < x1 && x < sw; x++)
                {
                    unsigned p = FB_UNPACK_SCALAR_LCD(row[x]);

                    r += RGB_UNPACK_RED_LCD(p);
                    g += RGB_UNPACK_GREEN_LCD(p);
                    b += RGB_UNPACK_BLUE_LCD(p);
                    n++;
                }
            }
            if (n == 0)
                n = 1;
            dst[(size_t)oy * cw + ox] =
                FB_SCALARPACK_LCD(LCD_RGBPACK_LCD(r / n, g / n, b / n));
        }
    }
}

/* One box pass along a run of n pixels `step` apart, in place. The line
 * buffer carries the run with both ends extended by the tap radius, which is
 * how the edges clamp without a bounds test in the inner loop.
 *
 * `half` rounds the divide to nearest. Truncating instead loses half a level
 * a pass, and there are four of them -- two passes, each horizontal then
 * vertical -- so the blurred picture came out two levels darker than the art
 * it was made from, on every channel. */
static void box_run(fb_data *p, int n, int step, uint32_t *line,
                    int taps, int shift)
{
    const uint32_t half = SPREAD_UNIT << (shift - 1);
    const int radius = taps / 2;
    const uint32_t first = SPREAD(p[0]);
    const uint32_t last  = SPREAD(p[(size_t)(n - 1) * step]);
    uint32_t sum = 0;

    for (int i = 0; i < radius; i++)
    {
        line[i] = first;
        line[radius + n + i] = last;
    }
    for (int i = 0; i < n; i++)
        line[radius + i] = SPREAD(p[(size_t)i * step]);

    for (int i = 0; i < taps; i++)
        sum += line[i];
    for (int i = 0; i < n; i++)
    {
        p[(size_t)i * step] = COLLAPSE(((sum + half) >> shift) & SMASK);
        sum += line[i + taps] - line[i];
    }
}

/* `want` is the window the chain asked for, in pixels of this picture. It is
 * capped at the picture's own width as well as at BLUR_MAX_TAPS: a window
 * wider than what it is averaging has nothing left to say, and every pixel
 * would come back the same colour. */
static void box_blur(fb_data *p, int cw, int ch, int want)
{
    uint32_t line[MAX(BLUR_MAX_W, BLUR_MAX_H) + BLUR_MAX_TAPS];
    int taps = BLUR_MIN_TAPS, shift = 1;

    want = MIN(want, MIN(cw, ch));

    /* Nearest power of two, so a chain that asks for six taps gets eight
     * rather than four, and the shift stays exact. */
    while (taps < BLUR_MAX_TAPS && want >= taps + taps / 2)
    {
        taps <<= 1;
        shift++;
    }

    for (int pass = 0; pass < BLUR_PASSES; pass++)
    {
        for (int y = 0; y < ch; y++)
            box_run(p + (size_t)y * cw, cw, 1, line, taps, shift);
        for (int x = 0; x < cw; x++)
            box_run(p + x, ch, cw, line, taps, shift);
    }
}

/* Everything the chain still has to do to one finished pixel, in the same
 * canonical order img_filter_apply() runs its passes in. The two must agree;
 * filterproto's "fused equals stepwise" check is what holds them together. */
static FORCE_INLINE unsigned finish_pixel(unsigned p,
                                          const struct img_filter *f,
                                          int rnd, int jr, int jg, int jb)
{
    if (f->stages & IMG_CLASS_COLOUR)
        p = f->colour_neutral
              ? grey_pixel(p, f->matrix[0], f->matrix[1], f->matrix[2], rnd)
              : matrix_pixel(p, f->matrix, rnd);

    if (f->stages & IMG_CLASS_LEVELS)
        p = f->levels_complement
              ? (p ^ FIELD_MASK)
              : levels_pixel(p, f->lut + channels[0].offset,
                             f->lut + channels[1].offset,
                             f->lut + channels[2].offset, rnd, jr, jg, jb);
    return p;
}

/* Fixed-factor separable bilinear, fused: one line of blended source pixels
 * at a time, no full-size intermediate. The two-pass form with a real
 * intermediate does the same number of blends and wants a hundred times the
 * memory.
 *
 * Linear weights, not smoothstep: smoothstep costs the same and cures the
 * faint lattice bilinear leaves at the source-pixel boundaries, but flattens
 * the middle of each source pixel into blockiness instead. The lattice is
 * cured by a blur before the upscale, which is what box_blur() already is. */
static void upscale(fb_data *dst, int dw, int dh, const fb_data *small,
                    int cw, int ch, int div, const struct img_filter *f)
{
    const bool dither = (f->stages & IMG_CLASS_SCREEN) != 0;
    const int sr = f->lut_span[0], sg = f->lut_span[1], sb = f->lut_span[2];
    uint32_t line[BLUR_MAX_W + 1];
    unsigned wt[BLUR_MAX_DIV];
    int sy = 0, fy = 0;

    for (int k = 0; k < div; k++)
        wt[k] = (unsigned)(k * 32 / div);

    for (int y = 0; y < dh; y++)
    {
        const fb_data *r0 = small + (size_t)sy * cw;
        const fb_data *r1 = (sy + 1 < ch) ? r0 + cw : r0;
        const unsigned char *brow = bayer8 + ((y & 7) << 3);
        /* A different cell for the vertical blend than the horizontal one
         * below, or the two roundings agree and the pattern reappears as
         * diagonal structure. The vertical one is indexed by source column,
         * because that is the grid it quantises on. */
        const unsigned char *vrow = bayer8 + (((y + 4) & 7) << 3);
        fb_data *o = dst + (size_t)y * dw;
        int x = 0;

        for (int i = 0; i < cw; i++)
            line[i] = LERP(SPREAD(r0[i]), SPREAD(r1[i]), wt[fy],
                           vrow[i & 7] >> 1);
        line[cw] = line[cw - 1];

        for (int i = 0; i < cw && x < dw; i++)
        {
            const uint32_t a = line[i], b = line[i + 1];

            for (int k = 0; k < div && x < dw; k++, x++)
            {
                unsigned p = COLLAPSE(LERP(a, b, wt[k], brow[x & 7] >> 1));
                int d = dither ? brow[x & 7] : 0;

                o[x] = FB_SCALARPACK_LCD(
                    finish_pixel(p, f, dither ? BAYER_ROUND(d) : 128,
                                 dither ? BAYER_JITTER(d, sr) : 0,
                                 dither ? BAYER_JITTER(d, sg) : 0,
                                 dither ? BAYER_JITTER(d, sb) : 0));
            }
        }

        if (++fy == div)
        {
            fy = 0;
            sy++;
        }
    }
}

void img_filter_render(fb_data *dst, int dw, int dh,
                       const fb_data *src, int sw, int sh,
                       const struct img_filter *f)
{
    int div, cw, ch;

    if (!dst || !src || !f || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;

    div = img_filter_source_divisor(f, dw, dh);
    cw = (dw + div - 1) / div;
    ch = (dh + div - 1) / div;

    if (!(f->stages & IMG_CLASS_RESIZE)
        || cw > BLUR_MAX_W || ch > BLUR_MAX_H)
    {
        /* No blur asked for, or a destination the working buffer cannot
         * serve: scale straight into it and run the ordinary passes. */
        decimate(dst, dw, dh, src, sw, sh);
        img_filter_apply(dst, dw, dh, f);
        return;
    }

    decimate(blur_buf, cw, ch, src, sw, sh);
    box_blur(blur_buf, cw, ch, f->blur_radius);

    if (f->stages & IMG_CLASS_SPATIAL)
    {
        /* Blocks want hard edges, so they cannot be part of a per-pixel
         * write, and canonical order puts them before the colour and levels
         * stages anyway. Both say the same thing: unfuse. Upscale plain,
         * block the result, then run the rest as ordinary passes. */
        struct img_filter resize_only = *f;

        resize_only.stages = IMG_CLASS_RESIZE;
        upscale(dst, dw, dh, blur_buf, cw, ch, div, &resize_only);
        img_filter_apply(dst, dw, dh, f);
    }
    else
        upscale(dst, dw, dh, blur_buf, cw, ch, div, f);
}
