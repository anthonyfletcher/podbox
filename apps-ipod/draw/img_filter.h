/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to img_filter.c: compiling a filter chain into an executable
 * form, and running it over a block of native pixels.
 ****************************************************************************/

#ifndef _IMG_FILTER_H_
#define _IMG_FILTER_H_

#include <stdbool.h>
#include "config.h"
#include "lcd.h"

/* A chain is filter names joined by '+', each optionally carrying an amount:
 *
 *      invert            brightness-30            reduce3+invert
 *
 * No whitespace. A positive amount is written without a sign, because '+' is
 * the separator. An unrecognised name is an error rather than a filter that
 * does nothing, so a typo fails where the chain is compiled.
 *
 * The whole set exists: invert, brightness, lighter, darker, contrast,
 * reduce, bw, saturate, hue, dither, pixellate, blur.
 */

/* Filter classes: what one output pixel needs to see. The class decides the
 * mechanism, the stage the filter runs in, and which callers accept it.
 *
 * The spatial class splits in two because the tier rule is structural rather
 * than a cost cutoff: a caller working on cached art accepts anything that
 * rewrites the pixels it was handed, at the size it was handed. `pixellate`
 * does that; `blur` is a resize and needs a destination of its own. */
#define IMG_CLASS_LEVELS    (1u << 0)   /* own channels, independently  */
#define IMG_CLASS_COLOUR    (1u << 1)   /* own channels, combined       */
#define IMG_CLASS_SCREEN    (1u << 2)   /* own value and its (x,y)      */
#define IMG_CLASS_SPATIAL   (1u << 3)   /* neighbours, in place         */
#define IMG_CLASS_RESIZE    (1u << 4)   /* neighbours, own destination  */

/* The two tiers, as an `allowed_classes` argument. */
#define IMG_CLASSES_INPLACE (IMG_CLASS_LEVELS | IMG_CLASS_COLOUR | \
                             IMG_CLASS_SCREEN | IMG_CLASS_SPATIAL)
#define IMG_CLASSES_ALL     (IMG_CLASSES_INPLACE | IMG_CLASS_RESIZE)

#define IMG_FILTER_MAX_CHAIN 8
#define IMG_FILTER_ERR_MAX   48

/* One filter as written. Kept after compiling so an adaptive chain can refold
 * its table once the image's mean luminance is known; `def` indexes the
 * filter table in img_filter.c and means nothing outside it. */
struct img_filter_step
{
    unsigned char def;
    bool          adaptive;     /* amount comes from the image, not the spec */
    short         amount;       /* as written, before the filter's sign */
};

/* One channel entry per level: 32 red, 64 green, 32 blue -- 128 bytes on
 * RGB565. A whole chain of levels filters folds into this one table, so
 * `invert+brightness20+reduce4` costs exactly what `invert` alone costs. */
#define IMG_FILTER_LUT_SIZE (LCD_MAX_RED + LCD_MAX_GREEN + LCD_MAX_BLUE + 3)

struct img_filter
{
    /* Which stages have any work in them, as IMG_CLASS_* bits. An empty stage
     * is skipped, so a chain pays per stage used rather than per filter
     * named. Zero means the whole chain is a no-op. */
    unsigned stages;

    unsigned char lut[IMG_FILTER_LUT_SIZE];

    /* Per channel, the longest run of input levels the table sends to one
     * output -- how coarse it is, and so how far the dither stage has to
     * jitter a value to break the run up. 1 means there is nothing to
     * dither. */
    unsigned char lut_span[3];

    /* The colour stage, row-major: out[o] = sum(matrix[o*3+i] * in[i]) >> 8.
     * A whole chain of colour filters folds into this one matrix the way a
     * chain of levels filters folds into the table above. The coefficients
     * are pre-scaled for the display's field widths, so they act on 5- and
     * 6-bit channel values directly rather than on 8-bit ones. */
    short matrix[9];

    /* The folded matrix sends every pixel to a grey, so the colour stage can
     * work out one luminance and derive the other two channels from it. */
    bool colour_neutral;

    /* The folded table is exactly the channel complement, so the levels stage
     * can XOR the packed pixel instead of reading the table three times. */
    bool levels_complement;

    /* Some filter takes its amount from the image, so the table is a
     * placeholder until it is refolded per image. */
    bool has_adaptive;

    /* The blur window the chain asked for, in pixels of the decimated picture
     * the blur runs on, not of the finished one. 0 when it named no blur.
     * The finished picture is img_filter_source_divisor() times larger, so
     * that is the factor between this and the softness on screen. */
    unsigned char blur_radius;

    /* Block edge for `pixellate`, in pixels. 0 when the chain named none. */
    unsigned char pixel_block;

    unsigned char          nsteps;
    struct img_filter_step steps[IMG_FILTER_MAX_CHAIN];

    /* Why compiling failed, naming the filter. Empty on success. */
    char error[IMG_FILTER_ERR_MAX];
};

/* Parse and fold a chain. Returns false, with out->error set, on a bad spec
 * or one naming a filter the caller's tier does not permit. A NULL or empty
 * spec is not an error -- it compiles to a chain with no stages. */
bool img_filter_compile(const char *spec, unsigned allowed_classes,
                        struct img_filter *out);

/* How light a picture is, in the two ways a scrim cares about. Both 0..255 on
 * the weights the palette extractor uses.
 *
 * `bright` is the luminance nine tenths of the picture falls below. Text is
 * read against the brightest thing it crosses rather than against the
 * average, and on a real cover the two are far apart: a sleeve averaging 132
 * has a ninth decile of 199, so a scrim aimed at the mean leaves the band the
 * title sits on half as bright again as it was asked to be. */
struct img_levels
{
    short mean;
    short bright;
};

/* Measure a block of native pixels. Cheap enough to take off the source of a
 * blurred chain: a box blur moves neither figure much, so the small picture
 * answers for the large one. */
void img_filter_measure(const fb_data *px, int w, int h,
                        struct img_levels *out);

/* Resolve a chain's adaptive filters against one picture and refold its
 * table. `lighter`, `darker` and `scrim` ask for as much as this picture
 * needs and no more, which is not knowable until the picture is in hand -- so
 * a chain carrying one compiles with a placeholder table and is not fit to
 * run until this has been called for the image it will act on.
 *
 * Call it once per image, before the passes below. A chain with nothing
 * adaptive in it is left alone, so calling it always is safe and costs a
 * branch. Rebuilding the table is 128 operations against the tens of
 * thousands the passes do, and a picture needing no scrim at all drops the
 * levels stage outright.
 *
 *   lv     img_filter_measure() of the picture. NULL leaves the chain on its
 *          placeholder table, which is the identity.
 *   avoid  the luminance the picture has to stay clear of -- what text drawn
 *          over it will be, which on a themed screen is not a constant. Only
 *          `scrim` reads it, and it is the one filter that can lighten or
 *          darken depending on the answer. -1 for "nothing named", where
 *          `scrim` falls back to assuming light text.
 */
void img_filter_adapt(struct img_filter *f, const struct img_levels *lv,
                      int avoid);

/* Rewrite w*h native pixels in place. Stages run in canonical order --
 * spatial, colour, levels, dither -- whatever order the chain named them. */
void img_filter_apply(fb_data *px, int w, int h, const struct img_filter *f);

/* img_filter_apply() over a tall image, a band at a time, yielding between
 * bands. A browser thumbnail is one band and never yields; a 300px frame is
 * twenty-odd. Use this for anything the user might be waiting on. */
void img_filter_apply_banded(fb_data *px, int w, int h,
                             const struct img_filter *f);

/* How much smaller than the finished picture the source may be, for a chain
 * that blurs: the blur throws that detail away in its first step, so asking
 * for a source this size costs nothing and saves everything downstream from
 * carrying the full-size one. 1 for any chain that does not blur.
 *
 * The smallest power of two that fits the decimated picture in the engine's
 * fixed working buffer, so the source keeps every pixel there is room for.
 * The blur amount does not enter into it. A caller sizing a source and the
 * render must both ask, and get the same answer for the same w and h. */
int img_filter_source_divisor(const struct img_filter *f, int w, int h);

/* Render a chain that resizes: source and destination may differ in size, and
 * the source is left alone. Every stage the chain uses is applied on the way
 * out, so a blurred chain is one pass over the destination however many
 * filters it names.
 *
 * Not the entry point for a chain that only rewrites pixels -- that is
 * img_filter_apply(), which needs no destination of its own. */
void img_filter_render(fb_data *dst, int dw, int dh,
                       const fb_data *src, int sw, int sh,
                       const struct img_filter *f);

#endif /* _IMG_FILTER_H_ */
