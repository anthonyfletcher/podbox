/***************************************************************************
 * Original code from RockPod
 * was: apps/gui/skin_engine/skin_albumart_color.c
 * Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Extracts a dominant colour palette from the current album art and
 * exposes it to skins as dynamic colours.
 *
 * Extraction runs once per track, not per frame: the art is sampled into
 * coarse colour buckets, the most populous are picked as the palette, then
 * adjusted for contrast so text drawn in them stays readable against the
 * background. Skins read the result through the dynamic-colour tags.
 *
 * Once per boot as well, where there is no track to extract from: the palette
 * is taken from the folder the last session left a resume point in, so the
 * first screen comes up in the colours the device was left wearing.
 *
 * The six colours a theme names are matched by value and mapped by role. Every
 * other colour a skin spells out is carried over instead: measured against the
 * theme's own pair, and rebuilt in the same relationship to the album's. So a
 * skin needs no syntax to take part -- a plain hex accent moves with the music
 * like the foreground and background do. The syntax is for opting out: a
 * colour written '!rrggbb' arrives flagged COLOR_FIXED and is left alone.
 *
 * The palette changes in one step, not over a fade -- see apply_colors() for
 * why a fade cannot be made to look right here.
 *
 * The module also owns the other once-per-art-change job, running a skin's
 * %Cl filter chain over the art -- not because filtering has anything to do
 * with colour, but because both need the same answer to "has the art changed,
 * and has it finished buffering yet?", and two state machines answering that
 * separately would drift apart. Extraction always goes first: the palette
 * describes the album, not the theme's treatment of it.
 *
 * Parts, in order:
 *   - luminance, contrast and blending helpers
 *   - pick_accent(), pick_vivid() and extract_colors(): sampling the bitmap
 *     and choosing the palette
 *   - the %Cl filter chain, applied in place once per art change
 *   - track-change hook, the boot seed, theme colour save/restore, and the
 *     once-per-render check that drives both jobs
 *   - carrying a skin's own colours over to the new palette
 *   - the accessors skins resolve their colour tags through, palette words
 *     included, which also report whether a change is recent enough to need
 *     another repaint
 ****************************************************************************/

#include "config.h"


#include <string.h>
#include <stdlib.h>                  /* abs */
#include "lcd.h"
#include "settings/settings.h"
#include "kernel.h"
#include "audio/playback.h"
#include "audio/buffering.h"
#include "system/appevents.h"
#include "core_alloc.h"
#include "file.h"
#include "string-extra.h"
#include "metadata/art_cache.h"      /* the palette's own source */
#include "playlist/playlist.h"       /* playlist_is_from_artist -- Auto's rule */
#include "draw/bmp.h"                /* struct bitmap, FORMAT_NATIVE */
#include "draw/color.h"              /* blending, HSV and contrast helpers */
#include "draw/img_filter.h"
#include "skin_engine.h"             /* SKINNABLE_SCREENS_COUNT */
#include "wps_internals.h"           /* struct skin_albumart */
#include "skin_albumart_color.h"

/* How long after a colour change the screens are asked to keep repainting.
 * Long enough for the slowest of them -- the status bar, on its own throttle --
 * to come round once. Not a fade: every repaint in the window draws the final
 * colour. */
#define AA_SETTLE_TICKS   (HZ / 5)   /* 200ms */
#define HISTOGRAM_BUCKETS 4096

/* How many pixels the histogram wants to see. The cached thumbnail below is
 * exactly this many, so it is counted whole; the fallback path reads a skin's
 * slot bitmap, which can be full-screen, and strides down to about this.
 *
 * Not fewer, because the bucket count is the floor: at one sample per bucket
 * the winning bucket is decided by counts in single figures, and the
 * saturation weight below multiplies that noise rather than damping it. Three
 * passes over 16384 pixels is a couple of milliseconds, once per track. */
#define SAMPLE_TARGET     16384      /* one whole 128px thumbnail */
#define MIN_RATIO         600        /* target contrast ratio x100 (6:1) */
#define SATURATION_BASE   8          /* base score for unsaturated colors */
#define NO_ART_TIMEOUT    HZ         /* 1s timeout before concluding no art */

/* How far off the theme's own background-to-foreground line a colour has to
 * sit, as a per-channel distance, before it is treated as an accent in its own
 * right rather than as a tint mixed from the pair. */
#define AXIS_TOLERANCE    24

/* Saturation below which a theme background counts as having no hue at all. */
#define CHROMA_MIN        32

/* Contrast a carried-over accent aims for against the new background, in
 * hundredths: WCAG's 3:1 for interface components rather than the 4.5:1 it
 * asks of body text.
 *
 * An accent has to be told apart from its background, which is what 3:1 is the
 * threshold for. Demanding 4.5:1 is what a mid-luminance album cannot give: no
 * vivid colour reaches it there, only something near black or near white, so
 * the stricter bar reliably destroys the colour it was meant to protect. */
#define ACCENT_MIN_RATIO  300

/* Contrast the accent aims for against the artwork itself, in hundredths. The
 * same 3:1, and for the same reason: this is text laid over a picture rather
 * than body text on a flat fill. */
#define ART_MIN_RATIO     300

/* How far a carried tint has to stay from the accent, in hundredths, so that
 * it still reads as a dimmed version of it rather than as the same colour.
 * Every album in the sample keeps more separation than this on the theme's own
 * pair, so it is a floor reached only where a narrow album pair leaves no
 * choice, not a figure anything is aimed at. */
#define TINT_MIN_SEPARATION 115

/* Transformed colours remembered between palette changes. A skin uses a
 * handful, so this sits well clear of what one asks for. */
#define XFORM_CACHE_SIZE  16

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

struct dynamic_colors_cache {
    unsigned int dominant;       /* bg color */
    unsigned int accent;         /* fg color */
    unsigned int found_dominant; /* the pair as extracted, before */
    unsigned int found_accent;   /*   Dynamic Colors Background turns it */
    unsigned int vivid;          /* the album's most colourful colour */
    bool has_vivid;              /* the album has one: see pick_vivid() */
    int orientation;             /* the DYNAMIC_BG_* those two were turned by */
    unsigned int theme_fg;       /* saved original theme fg */
    unsigned int theme_bg;       /* saved original theme bg */
    unsigned int theme_sep;      /* saved list separator color */
    long change_tick;            /* when the colours last changed */
    long track_change_tick;      /* when TRACK_CHANGE fired */
    bool valid;                  /* have valid AA colors */
    bool was_enabled;            /* track setting state for toggle detection */
    bool needs_full_update;      /* set on a change, for a full redraw */
    bool needs_screen_clear;     /* set on a change, to clear bg gaps */
};

static struct dynamic_colors_cache cache;
static volatile bool needs_extraction;
static uint16_t histogram[HISTOGRAM_BUCKETS];

/* Colours the theme named that are neither its foreground nor its background,
 * and what they became under the current palette. */
static struct
{
    unsigned int in;
    unsigned int out;
} xform_cache[XFORM_CACHE_SIZE];
static int xform_entries;
static int xform_rotation;
static bool xform_rotation_valid;

/* What each palette word came to, by its index in the word table, and which
 * source that was for: a tone bound is a search, too dear to repeat every
 * time a skin draws. */
enum { WORD_UNKNOWN, WORD_FROM_PALETTE, WORD_FROM_THEME };
static unsigned int word_out[COLOR_WORDS_MAX];
static unsigned char word_from[COLOR_WORDS_MAX];

/* Every transformed colour is derived from the palette and the theme, so both
 * changing invalidates the lot. */
static void forget_transforms(void)
{
    xform_entries = 0;
    xform_rotation_valid = false;
    memset(word_from, WORD_UNKNOWN, sizeof word_from);
}

static int compute_luminance(int r8, int g8, int b8)
{
    return (r8 * 77 + g8 * 150 + b8 * 29) >> 8;
}

static int color_luminance(unsigned int c)
{
    return compute_luminance(RGB_UNPACK_RED(c), RGB_UNPACK_GREEN(c),
                             RGB_UNPACK_BLUE(c));
}

/* WCAG relative luminance, 0..LUM_MAX.
 *
 * sRGB is linearised with a gamma of 2.0 rather than the exact piecewise 2.4
 * curve: a multiply instead of a lookup table, and well inside the tolerance of
 * a pass/fail threshold. */
#define LUM_MAX  65025          /* 255 * 255 */
#define LUM_5PCT  3251          /* the 0.05 term in the WCAG ratio */

static int32_t rel_luminance(int r8, int g8, int b8)
{
    int32_t r = (int32_t)r8 * r8;
    int32_t g = (int32_t)g8 * g8;
    int32_t b = (int32_t)b8 * b8;

    return (r * 2126 + g * 7152 + b * 722) / 10000;
}

/* Contrast between two relative luminances, in hundredths -- 450 is WCAG's
 * 4.5:1 for body text. A ratio, not a difference: the same gap between two dark
 * colours reads far weaker than between two light ones, which is exactly where
 * album art tends to sit. */
static int contrast_ratio(int32_t lum_a, int32_t lum_b)
{
    int32_t hi = MAX(lum_a, lum_b) + LUM_5PCT;
    int32_t lo = MIN(lum_a, lum_b) + LUM_5PCT;

    return (int)((hi * 100) / lo);
}

/* t: 0..256, 0 = fully c1, 256 = fully c2. The dialog chrome derives its own
 * secondary colours the same way, so the arithmetic lives in draw/color.c. */
static unsigned int lerp_color(unsigned int c1, unsigned int c2, int t)
{
    return color_blend(c1, c2, t);
}

/* Change to a new pair of colours, or back to the theme's, in one step.
 *
 * Trap: do not interpolate these over a fade. The screen is painted by several
 * independent schedules -- the skin's own refresh, the meters' animation
 * cycles, the status bar's throttle -- and an interpolated colour is only
 * coherent if every one of them samples it on the same tick, which they do not.
 * Elements drawn on different schedules then show different points along the
 * fade at once, so the status boxes and the glyphs framing the spectrum appear
 * to fade differently from everything around them.
 *
 * One step removes the intermediate values entirely: whenever each schedule
 * next paints, it paints the final colour, and the worst case is an element
 * being one frame late, which is not visible. */
static void apply_colors(unsigned int new_accent, unsigned int new_dominant,
                         bool to_defaults)
{
    cache.accent = new_accent;
    cache.dominant = new_dominant;
    cache.valid = !to_defaults;
    cache.change_tick = current_tick;
    cache.needs_full_update = true;
    cache.needs_screen_clear = true;
    /* The theme's own pair belongs to no album, so there is nothing for the
     * next boot to seed from. */
    if (to_defaults)
        global_status.resume_art_hash = 0;
    forget_transforms();
}

/* The extracted pair, as Dynamic Colors Background asks: as found, or turned so
 * the background is the lighter or the darker of the two. Contrast is
 * symmetric, so either way round keeps every guarantee extraction made about
 * the pair; what Light gives up is the dark-accent rescue in extract_colors(),
 * since the colour that then reads over the artwork is the darker one. */
static void apply_oriented(void)
{
    unsigned int black = LCD_RGBPACK(0, 0, 0);
    unsigned int accent = cache.found_accent;
    unsigned int dominant = cache.found_dominant;
    bool accent_lighter = color_contrast(accent, black) >
                          color_contrast(dominant, black);
    int orientation = global_settings.dynamic_colors_background;

    if ((orientation == DYNAMIC_BG_LIGHT && accent_lighter) ||
        (orientation == DYNAMIC_BG_DARK && !accent_lighter))
    {
        accent = cache.found_dominant;
        dominant = cache.found_accent;
    }

    cache.orientation = orientation;
    apply_colors(accent, dominant, false);
}

/* Mean full-precision colour of the sampled pixels a quantised bucket caught.
 * The histogram is 4 bits a channel, so the bucket index alone is not a colour
 * worth putting on screen. */
static void average_bucket(const fb_data *pixels, int total_pixels, int stride,
                           int bucket, int *out_r, int *out_g, int *out_b)
{
    long sum_r = 0, sum_g = 0, sum_b = 0;
    int count = 0;
    int i;

    for (i = 0; i < total_pixels; i += stride)
    {
        unsigned short px = (unsigned short)pixels[i];
        int r4 = (px >> 12) & 0xF;
        int g4 = (px >> 7) & 0xF;
        int b4 = (px >> 1) & 0xF;

        if (((r4 << 8) | (g4 << 4) | b4) == bucket)
        {
            sum_r += RGB_UNPACK_RED(px);
            sum_g += RGB_UNPACK_GREEN(px);
            sum_b += RGB_UNPACK_BLUE(px);
            count++;
        }
    }

    *out_r = count ? (int)(sum_r / count) : 0;
    *out_g = count ? (int)(sum_g / count) : 0;
    *out_b = count ? (int)(sum_b / count) : 0;
}

/* Hues for the accent, pooled rather than taken a bucket at a time.
 *
 * A soft gradient -- an iridescent disc, a sky -- spreads one hue over dozens
 * of neighbouring buckets with a handful of pixels each, so no single bucket
 * outscores a flat patch a fraction of its size. Hues are pooled into 30-degree
 * sectors and each sector is judged with its two neighbours. */
#define HUE_SECTORS    12
#define HUE_MIN_SPREAD 2        /* 4-bit steps between channels to be a hue */
#define HUE_MIN_SHARE  32       /* a run needs 1/32 of the samples */
#ifndef HUE_ACCENT_SAT
#define HUE_ACCENT_SAT 160      /* 0..255: the least an accent taken this way has */
#endif

/* The bucket's hue sector, or -1 for a grey, dark or near-white one.
 *
 * Grey is judged by the channels' spread in 4-bit steps, not by saturation: a
 * single step is quantisation, and near black it reads as 20% saturation or
 * more, which would make the specks of print on a pale cover count as a
 * colour. Two steps keeps a pale pastel such as fdd. */
static int bucket_sector(int i)
{
    int r4 = (i >> 8) & 0xF;
    int g4 = (i >> 4) & 0xF;
    int b4 = i & 0xF;
    int h, s, v;

    if ((r4 < 4 && g4 < 4 && b4 < 4) || (r4 > 13 && g4 > 13 && b4 > 13))
        return -1;
    if (MAX(MAX(r4, g4), b4) - MIN(MIN(r4, g4), b4) < HUE_MIN_SPREAD)
        return -1;

    color_get_hsv(LCD_RGBPACK(r4 * 17, g4 * 17, b4 * 17), &h, &s, &v);
    return h * HUE_SECTORS / 360;
}

/* The picture's strongest run of one hue other than the dominant's, as its
 * best-scored bucket, or -1 when no run covers HUE_MIN_SHARE of the picture.
 * A run centred within a sector of a coloured dominant does not count.
 * *run_count is the winning run's sample count. */
static int strongest_hue_bucket(int dom_bucket, unsigned int *run_count)
{
    static int8_t sector_of[HISTOGRAM_BUCKETS];   /* 4 KB; off the stack */
    unsigned int sector_count[HUE_SECTORS] = { 0 };
    unsigned int samples = 0;
    int dom_sector = -1;
    int best_run = -1;
    unsigned int best_run_count = 0;
    int i;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        samples += histogram[i];
        sector_of[i] = histogram[i] ? bucket_sector(i) : -1;
        if (sector_of[i] >= 0)
            sector_count[sector_of[i]] += histogram[i];
    }

    if (dom_bucket >= 0)
        dom_sector = sector_of[dom_bucket];

    for (i = 0; i < HUE_SECTORS; i++)
    {
        unsigned int run = sector_count[(i + HUE_SECTORS - 1) % HUE_SECTORS]
                         + sector_count[i]
                         + sector_count[(i + 1) % HUE_SECTORS];
        int apart = abs(i - dom_sector);

        if (dom_sector >= 0 && MIN(apart, HUE_SECTORS - apart) <= 1)
            continue;
        if (run > best_run_count)
        {
            best_run_count = run;
            best_run = i;
        }
    }

    *run_count = best_run_count;
    if (best_run < 0 || best_run_count * HUE_MIN_SHARE < samples)
        return -1;

    int bucket = -1;
    unsigned int bucket_score = 0;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        int d = abs(sector_of[i] - best_run);

        if (sector_of[i] < 0 || i == dom_bucket || MIN(d, HUE_SECTORS - d) > 1)
            continue;

        int r4 = (i >> 8) & 0xF;
        int g4 = (i >> 4) & 0xF;
        int b4 = i & 0xF;
        int max_c = MAX(MAX(r4, g4), b4);
        int min_c = MIN(MIN(r4, g4), b4);
        int sat = max_c > 0 ? ((max_c - min_c) * 15) / max_c : 0;
        unsigned int score = (unsigned int)histogram[i]
                           * (sat + SATURATION_BASE) / SATURATION_BASE;

        if (score > bucket_score)
        {
            bucket_score = score;
            bucket = i;
        }
    }

    return bucket;
}

/* The accent against a given dominant: the best-scored bucket clearing
 * MIN_RATIO against it, brought up to that ratio if nothing clears it.
 *
 * Unless that bucket is a grey, or there is none, and one hue covers more of
 * the picture than it does. On a light or pastel cover every colour is too
 * close to the dominant to be text on it, so the only buckets clearing the bar
 * are a few specks of black print; the hue the cover is actually made of is
 * then darkened or lightened until it reads instead. A white title on a dark
 * cover keeps its white unless one hue covers more of the cover than it does.
 *
 * A dark hue must also read against the picture as a whole ('art_rl'), which
 * black print does and a mid-dark hue may not; failing that sends
 * extract_colors() into its dark-accent rescue, which turns the palette over. */
static void pick_accent(const fb_data *pixels, int total_pixels, int stride,
                        int dom_bucket, int32_t dom_rl, int dom_lum,
                        int32_t art_rl, int *out_r, int *out_g, int *out_b)
{
    int i;

    int accent_bucket = -1;
    int hue_bucket = -1;
    unsigned int accent_score = 0;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        if (histogram[i] == 0 || i == dom_bucket)
            continue;

        int r4 = (i >> 8) & 0xF;
        int g4 = (i >> 4) & 0xF;
        int b4 = i & 0xF;

        /* Approximate 8-bit values from 4-bit */
        int r8 = (r4 << 4) | r4;
        int g8 = (g4 << 4) | g4;
        int b8 = (b4 << 4) | b4;
        if (contrast_ratio(dom_rl, rel_luminance(r8, g8, b8)) >= MIN_RATIO)
        {
            /* Score by count weighted by saturation */
            int max_c = MAX(MAX(r4, g4), b4);
            int min_c = MIN(MIN(r4, g4), b4);
            int sat = max_c > 0 ? ((max_c - min_c) * 15) / max_c : 0;
            unsigned int score = (unsigned int)histogram[i]
                               * (sat + SATURATION_BASE) / SATURATION_BASE;

            if (score > accent_score)
            {
                accent_score = score;
                accent_bucket = i;
            }
        }
    }

    unsigned int run_count = 0;

    if (accent_bucket < 0 || bucket_sector(accent_bucket) < 0)
    {
        hue_bucket = strongest_hue_bucket(dom_bucket, &run_count);
        if (accent_bucket >= 0 && run_count <= histogram[accent_bucket])
            hue_bucket = -1;
    }

    int acc_r, acc_g, acc_b;
    if (hue_bucket >= 0)
    {
        int dom_r, dom_g, dom_b;
        unsigned int fitted;

        average_bucket(pixels, total_pixels, stride, dom_bucket,
                       &dom_r, &dom_g, &dom_b);
        average_bucket(pixels, total_pixels, stride, hue_bucket,
                       &acc_r, &acc_g, &acc_b);

        /* A pastel darkened to read comes out a dull brown-grey, where the
         * eye expects the colour the pastel is a pale version of. Only where
         * that still reads, though: a saturated colour cannot get as light
         * or as dark, and one the fit gives up on for black or white is
         * worse than the paler hue it replaced. */
        unsigned int dominant = LCD_RGBPACK(dom_r, dom_g, dom_b);
        unsigned int hued = LCD_RGBPACK(acc_r, acc_g, acc_b);
        unsigned int white = LCD_RGBPACK(255, 255, 255);
        unsigned int black = LCD_RGBPACK(0, 0, 0);
        int h, s, v;

        color_get_hsv(hued, &h, &s, &v);
        fitted = white;
        if (s < HUE_ACCENT_SAT)
            fitted = color_fit_contrast(color_from_hsv(h, HUE_ACCENT_SAT, v),
                                        dominant, MIN_RATIO);
        if (fitted == white || fitted == black)
            fitted = color_fit_contrast(hued, dominant, MIN_RATIO);

        /* A hue that cannot read at any brightness has lost to the accent
         * it was replacing, which is often an off-white or a near-black with
         * some warmth in it rather than the plain one the fit falls back to. */
        if ((fitted == white || fitted == black) && accent_bucket >= 0)
            hue_bucket = -1;
        acc_r = RGB_UNPACK_RED(fitted);
        acc_g = RGB_UNPACK_GREEN(fitted);
        acc_b = RGB_UNPACK_BLUE(fitted);

        if (compute_luminance(acc_r, acc_g, acc_b) < 128 &&
            contrast_ratio(art_rl, rel_luminance(acc_r, acc_g, acc_b))
                < ART_MIN_RATIO)
            hue_bucket = -1;
    }

    if (hue_bucket < 0 && accent_bucket >= 0)
    {
        average_bucket(pixels, total_pixels, stride, accent_bucket,
                       &acc_r, &acc_g, &acc_b);
    }
    else if (hue_bucket < 0)
    {
        /* Hard fallback: dark dominant -> white text, light -> black */
        if (dom_lum < 128)
        {
            acc_r = 255; acc_g = 255; acc_b = 255;
        }
        else
        {
            acc_r = 0; acc_g = 0; acc_b = 0;
        }
    }

    /* Readability enforcement. First try to keep the accent's hue by scaling
     * its channels toward a target luminance; then check the result, because
     * that scaling can fail silently -- each channel clamps at 255
     * independently, so a saturated one leaves the colour short of the target
     * and shifts its hue. If it is still short, legibility wins over hue. */
    if (contrast_ratio(dom_rl, rel_luminance(acc_r, acc_g, acc_b)) < MIN_RATIO)
    {
        int acc_lum = compute_luminance(acc_r, acc_g, acc_b);
        int target_lum = (dom_lum < 128) ? MIN(dom_lum + 128, 255)
                                         : MAX(dom_lum - 128, 0);

        if (acc_lum > 0)
        {
            int scale = (target_lum * 256) / acc_lum;
            acc_r = MIN((acc_r * scale) >> 8, 255);
            acc_g = MIN((acc_g * scale) >> 8, 255);
            acc_b = MIN((acc_b * scale) >> 8, 255);
        }
        else
        {
            acc_r = acc_g = acc_b = target_lum;
        }

        if (contrast_ratio(dom_rl, rel_luminance(acc_r, acc_g, acc_b)) < MIN_RATIO)
            acc_r = acc_g = acc_b = (dom_lum < 128) ? 255 : 0;
    }

    *out_r = acc_r;
    *out_g = acc_g;
    *out_b = acc_b;
}

/* The album's most colourful colour, for the `vivid` palette word: the bucket
 * scoring highest on count times OKLab chroma squared, among those at least
 * VIVID_CHROMA_MIN colourful whose hue -- the bucket's sector and both
 * neighbours, pooled as strongest_hue_bucket() pools them -- covers
 * 1/VIVID_SHARE of the picture. False when no bucket qualifies, as on a
 * black-and-white or sepia sleeve.
 *
 * Chroma, not HSV saturation: a dark brown is highly saturated in HSV, and
 * scored that way the wood and skin in a photograph win over the one red
 * jacket the eye goes to. */
#define VIVID_CHROMA_MIN 4588           /* 0.07 in color_chroma()'s units */
#define VIVID_SHARE      64

static bool pick_vivid(const fb_data *pixels, int total_pixels, int stride,
                       unsigned int *out)
{
    unsigned int sector_count[HUE_SECTORS] = { 0 };
    unsigned int samples = 0, best_score = 0;
    int best = -1, i, r, g, b;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        int sector = histogram[i] ? bucket_sector(i) : -1;

        samples += histogram[i];
        if (sector >= 0)
            sector_count[sector] += histogram[i];
    }

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        int sector = histogram[i] ? bucket_sector(i) : -1;
        unsigned int run, score;
        int chroma;

        if (sector < 0)
            continue;
        run = sector_count[(sector + HUE_SECTORS - 1) % HUE_SECTORS]
            + sector_count[sector]
            + sector_count[(sector + 1) % HUE_SECTORS];
        if (run * VIVID_SHARE < samples)
            continue;
        chroma = color_chroma(LCD_RGBPACK(((i >> 8) & 0xF) * 17,
                                          ((i >> 4) & 0xF) * 17,
                                          (i & 0xF) * 17));
        if (chroma < VIVID_CHROMA_MIN)
            continue;
        chroma >>= 6;     /* the square times a count of 16384 fits 32 bits */
        score = histogram[i] * (unsigned int)(chroma * chroma);
        if (score > best_score)
        {
            best_score = score;
            best = i;
        }
    }

    if (best < 0)
        return false;
    average_bucket(pixels, total_pixels, stride, best, &r, &g, &b);
    *out = LCD_RGBPACK(r, g, b);
    return true;
}

static void extract_colors(const struct bitmap *bmp)
{
    if (!bmp->data || bmp->width <= 0 || bmp->height <= 0)
        return;

    fb_data *pixels = (fb_data *)bmp->data;
    int width = bmp->width;
    int height = bmp->height;
    int total_pixels = width * height;
    int stride = MAX(total_pixels / SAMPLE_TARGET, 1);
    long sum_lum = 0;
    int nsamples = 0;
    int i;

    memset(histogram, 0, sizeof(histogram));

    /* Pass 1: build quantized histogram */
    for (i = 0; i < total_pixels; i += stride)
    {
        unsigned short px = (unsigned short)pixels[i];
        int r4 = (px >> 12) & 0xF;
        int g4 = (px >> 7) & 0xF;
        int b4 = (px >> 1) & 0xF;
        int bucket = (r4 << 8) | (g4 << 4) | b4;
        if (histogram[bucket] < UINT16_MAX)
            histogram[bucket]++;
        sum_lum += r4 * 77 + g4 * 150 + b4 * 29;
        nsamples++;
    }

    /* The picture's own mean luminance, on the 0..255 scale that
     * compute_luminance() uses. The running total weights 4-bit channels, and
     * 17 is what widens a 4-bit channel to 8. */
    int art_lum = nsamples ? (int)(((sum_lum / nsamples) * 17) >> 8) : 0;
    int32_t art_rl = rel_luminance(art_lum, art_lum, art_lum);

    /* Find dominant bucket (skip near-black/near-white, prefer saturated) */
    int best_bucket = -1;
    unsigned int best_score = 0;
    int fallback_bucket = -1;
    uint16_t fallback_count = 0;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        if (histogram[i] == 0)
            continue;

        int r4 = (i >> 8) & 0xF;
        int g4 = (i >> 4) & 0xF;
        int b4 = i & 0xF;

        /* Track unfiltered best as fallback */
        if (histogram[i] > fallback_count)
        {
            fallback_count = histogram[i];
            fallback_bucket = i;
        }

        /* Skip near-black and near-white */
        if (r4 < 2 && g4 < 2 && b4 < 2)
            continue;
        if (r4 > 13 && g4 > 13 && b4 > 13)
            continue;
        /* Skip very dark colors regardless of hue */
        int lum4 = (r4 * 5 + g4 * 9 + b4 * 2) >> 4;
        if (lum4 < 2)
            continue;

        /* Score by count weighted by saturation (vibrant colors preferred) */
        int max_c = MAX(MAX(r4, g4), b4);
        int min_c = MIN(MIN(r4, g4), b4);
        int sat = max_c > 0 ? ((max_c - min_c) * 15) / max_c : 0;
        unsigned int score = (unsigned int)histogram[i]
                           * (sat + SATURATION_BASE) / SATURATION_BASE;

        if (score > best_score)
        {
            best_score = score;
            best_bucket = i;
        }
    }

    if (best_bucket < 0)
        best_bucket = fallback_bucket;
    if (best_bucket < 0)
        return; /* empty image? */

    /* Pass 2: average full-precision RGB for dominant bucket */
    int dom_r, dom_g, dom_b;

    average_bucket(pixels, total_pixels, stride, best_bucket,
                   &dom_r, &dom_g, &dom_b);
    unsigned int dominant = LCD_RGBPACK(dom_r, dom_g, dom_b);
    int dom_lum = compute_luminance(dom_r, dom_g, dom_b);
    int32_t dom_rl = rel_luminance(dom_r, dom_g, dom_b);

    int acc_r, acc_g, acc_b;

    pick_accent(pixels, total_pixels, stride, best_bucket, dom_rl, dom_lum,
                art_rl, &acc_r, &acc_g, &acc_b);
    unsigned int accent = LCD_RGBPACK(acc_r, acc_g, acc_b);

    /* The dominant is not the only thing the accent is read against: a theme
     * that paints over the artwork puts the accent straight on the picture,
     * and the pair chosen above says nothing about that. The accent is picked
     * by count, so a cover that is mostly one flat expanse hands its own
     * background back as the text colour.
     *
     * Only a dark accent needs catching. A light one over light art is the
     * `scrim` filter's job -- it darkens the picture to make room -- but it
     * stands down below half scale, reading a dark accent as evidence that
     * the artwork is already light, which is what a dark expanse disproves.
     * The replacement settles for ACCENT_MIN_RATIO against the dominant
     * rather than MIN_RATIO, because nothing clears 6:1 above a mid-luminance
     * dominant: the pair that produced a dark accent is the pair with no
     * light answer.
     *
     * A dominant too bright for even white to clear that bar -- a light logo
     * on a black sleeve, since the dominant search skips near-black -- is
     * replaced instead, by the picture's most populous bucket no brighter than
     * its mean. Trap: searching the old dominant for a rescue there finds only
     * a mid-grey between it and the art, and a panel blended from the two
     * lands on that grey. */
    bool dark_unread = compute_luminance(acc_r, acc_g, acc_b) < 128 &&
        contrast_ratio(art_rl, rel_luminance(acc_r, acc_g, acc_b))
            < ART_MIN_RATIO;

    if (dark_unread && contrast_ratio(dom_rl, rel_luminance(255, 255, 255))
                           < ACCENT_MIN_RATIO)
    {
        int dark_bucket = -1;
        uint16_t dark_count = 0;

        for (i = 0; i < HISTOGRAM_BUCKETS; i++)
        {
            int r4 = (i >> 8) & 0xF;
            int g4 = (i >> 4) & 0xF;
            int b4 = i & 0xF;

            if (histogram[i] > dark_count &&
                compute_luminance(r4 * 17, g4 * 17, b4 * 17) <= art_lum)
            {
                dark_count = histogram[i];
                dark_bucket = i;
            }
        }

        if (dark_bucket >= 0)
        {
            average_bucket(pixels, total_pixels, stride, dark_bucket,
                           &dom_r, &dom_g, &dom_b);
            dominant = LCD_RGBPACK(dom_r, dom_g, dom_b);
            dom_lum = compute_luminance(dom_r, dom_g, dom_b);
            dom_rl = rel_luminance(dom_r, dom_g, dom_b);
            pick_accent(pixels, total_pixels, stride, dark_bucket, dom_rl,
                        dom_lum, art_rl, &acc_r, &acc_g, &acc_b);
            accent = LCD_RGBPACK(acc_r, acc_g, acc_b);
        }
    }
    else if (dark_unread)
    {
        int rescue_bucket = -1;
        unsigned int rescue_score = 0;

        for (i = 0; i < HISTOGRAM_BUCKETS; i++)
        {
            if (histogram[i] == 0 || i == best_bucket)
                continue;

            int r4 = (i >> 8) & 0xF;
            int g4 = (i >> 4) & 0xF;
            int b4 = i & 0xF;
            int r8 = (r4 << 4) | r4;
            int g8 = (g4 << 4) | g4;
            int b8 = (b4 << 4) | b4;
            int32_t cand_rl = rel_luminance(r8, g8, b8);

            if (contrast_ratio(dom_rl, cand_rl) < ACCENT_MIN_RATIO)
                continue;
            if (contrast_ratio(art_rl, cand_rl) < ART_MIN_RATIO)
                continue;

            int max_c = MAX(MAX(r4, g4), b4);
            int min_c = MIN(MIN(r4, g4), b4);
            int sat = max_c > 0 ? ((max_c - min_c) * 15) / max_c : 0;
            unsigned int score = (unsigned int)histogram[i]
                               * (sat + SATURATION_BASE) / SATURATION_BASE;

            if (score > rescue_score)
            {
                rescue_score = score;
                rescue_bucket = i;
            }
        }

        if (rescue_bucket >= 0)
        {
            average_bucket(pixels, total_pixels, stride, rescue_bucket,
                           &acc_r, &acc_g, &acc_b);
            accent = LCD_RGBPACK(acc_r, acc_g, acc_b);
        }
        else if (contrast_ratio(art_rl, rel_luminance(255, 255, 255))
                     >= ART_MIN_RATIO)
        {
            accent = LCD_RGBPACK(255, 255, 255);
        }
    }

    cache.found_accent = accent;
    cache.found_dominant = dominant;
    cache.has_vivid = pick_vivid(pixels, total_pixels, stride, &cache.vivid);
    apply_oriented();
}

/* ---------------------------------------------------------------------- *
 * The palette's own source                                                *
 * ---------------------------------------------------------------------- */

/* Where the palette comes from, in order of preference: the art cache's own
 * thumbnail for the playing album, and failing that whatever bitmap a skin
 * happened to buffer into its album-art slot.
 *
 * Reading the cache is much the better answer, and not only because it needs
 * no slot:
 *
 *   - it works on a theme with no %Cl at all. Borrowing a skin's slot means
 *     dynamic colours silently do nothing on such a theme, which is a trap
 *     theme authors have had to know about and work around.
 *   - it is the same picture whatever theme is loaded, so an album gets the
 *     same colours from one theme to the next. Borrowing gives a palette
 *     taken from whatever size that theme's %Cl asked for.
 *   - it cannot be a filtered picture. A skin's slot can be: %Cl filters
 *     rewrite it in place, and a bw chain would turn every derived colour
 *     grey.
 *
 * The 128px "coverflow" thumbnail, not the 44px "list" one, and that is
 * measured rather than assumed. Downscaling averages small saturated regions
 * away, and the dominant colour is chosen with a bias toward saturation --
 * so a heavily reduced thumbnail leaves that bias nothing to work with.
 * Against the 300px thumbnail over 120 albums, the mean per-channel
 * difference in the colour chosen is 14 from the 128px and 25 from the 44px,
 * and the albums whose palette comes out markedly duller run 4% against 5%.
 *
 * The whole picture has to be resident because the extractor makes three
 * passes over it, so this is 32 KB that stays allocated. Reading it in bands
 * would need three reads of the file instead of one; area-averaging it down
 * on the way in was measured too, and gives back exactly the accuracy the
 * 128px was chosen for. */
#define PALETTE_MAX_DIM 128

static fb_data palette_px[PALETTE_MAX_DIM * PALETTE_MAX_DIM];
static int palette_size_idx = -2;      /* -2 not looked up, -1 no such size */

/* The folder last found to have no cached thumbnail. Without this, a track
 * whose album is not in the cache reopens a file that is not there on every
 * render pass while the timeout below runs down; with it, once per track. */
static unsigned int palette_miss;

/* The folder of a track path, hashed the way the cache keys on it: no
 * normalisation and no trailing slash. Anything else silently misses.
 *
 * `artist` rises one more level, which is where the cache keeps a portrait --
 * the same arithmetic load_cached_albumart() does, including refusing to rise
 * past the volume root, where truncating to "" would key on the whole volume. */
static unsigned int track_folder_hash(const char *path, bool artist)
{
    const char *slash;

    if (!path || !path[0])
        return 0;
    slash = strrchr(path, '/');
    if (!slash || slash == path)
        return 0;

    {
        char dir[MAX_PATH];
        size_t n = (size_t)(slash - path);

        if (n == 0 || n >= sizeof(dir))
            return 0;
        memcpy(dir, path, n);
        dir[n] = '\0';

        if (artist)
        {
            char *psep = strrchr(dir, '/');

            if (!psep || psep == dir)
                return 0;
            *psep = '\0';
        }
        return art_cache_dir_hash(dir);
    }
}

/* The edge of the thumbnails the palette is taken from, or 0 if this build
 * caches no such size or caches it larger than the buffer above. */
static int palette_dim(void)
{
    int dim;

    if (palette_size_idx == -2)
        palette_size_idx = art_cache_size_index("coverflow");
    if (palette_size_idx < 0)
        return 0;

    dim = art_cache_size_dim(palette_size_idx);
    return (dim > 0 && dim <= PALETTE_MAX_DIM) ? dim : 0;
}

/* Read one folder's cached thumbnail and take the palette from it. False means
 * there is none to read: no database yet, a folder the caching pass has not
 * reached, or one with no art at all. */
static bool palette_from_folder(unsigned int hash, int dim)
{
    struct art_cache_header hdr;
    struct bitmap bmp;
    char aat[MAX_PATH];
    size_t bytes;
    int fd;

    if (hash == 0)
        return false;

    /* Ask where the thumbnail would be and open it, rather than asking
     * whether it exists first -- the open is the existence test. A folder
     * with no art has no file here, which is what keeps the shared "no art"
     * placeholder from becoming somebody's palette. */
    art_cache_thumb_path(hash, palette_size_idx, aat, sizeof(aat));
    fd = open(aat, O_RDONLY);
    if (fd < 0)
        return false;

    bytes = (size_t)dim * dim * sizeof(fb_data);
    if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        hdr.magic != ART_CACHE_MAGIC ||
        hdr.version != ART_CACHE_FORMAT_VERSION ||
        hdr.width != dim || hdr.height != dim ||
        read(fd, palette_px, bytes) != (ssize_t)bytes)
    {
        close(fd);
        return false;
    }
    close(fd);

    /* Pixel order is not checked because a histogram does not care: this
     * size is stored column-major for the carousel, and every pixel is
     * counted whichever way round they sit. */
    bmp.width = dim;
    bmp.height = dim;
    bmp.format = FORMAT_NATIVE;
    bmp.data = (unsigned char *)palette_px;
    extract_colors(&bmp);
    /* Remembered for the next boot, which has a resume point but no playing
     * track to derive a folder from. */
    global_status.resume_art_hash = hash;
    return true;
}

/* The other source: whatever bitmap the skin buffered into its album-art slot.
 * Second choice, because it is the theme's picture at the theme's size rather
 * than the album's own, but it is all there is until the caching pass arrives.
 *
 * Pinned across the read. extract_colors() makes three passes, and the bitmap
 * lives in the audio buffer where it is movable -- so anything that allocated
 * between those passes could have it sampling two different pictures and
 * producing a palette belonging to neither. Pinning forbids the move; the handle
 * can still be *removed*, which is what the bufgetdata() test catches.
 *
 * False for art the chain has already rewritten: %Cl rewrites in place, and a
 * palette taken from one describes the theme's treatment of the album rather
 * than the album -- a bw chain would turn every derived colour grey. */
static bool art_filtered(int handle);                 /* with the filter chain */

static bool palette_from_slot(int handle)
{
    struct bitmap *bmp;
    bool got;

    if (art_filtered(handle))
        return false;
    if (!buf_pin_handle(handle, true))
        return false;

    got = bufgetdata(handle, 0, (void *)&bmp) > 0;
    if (got)
        extract_colors(bmp);

    buf_pin_handle(handle, false);
    return got;
}

/* Extract from the cached thumbnail, asking about the folder the now-playing
 * screen is showing -- and the caller falls back to the skin's slot if there is
 * nothing there.
 *
 * The cache holds portraits as well as covers, keyed by folder either way, so
 * every setting can be served from here; only the folder differs. The rule is
 * load_cached_albumart()'s, and the asymmetry in it is deliberate there: Auto
 * asks for a portrait only for a playlist built from an artist browse and falls
 * back to the cover, while the explicit Artist setting does not fall back,
 * because a missing portrait is meant to be visible rather than papered over. */
static bool palette_from_cache(void)
{
    unsigned int cand[2];
    char path[MAX_PATH];
    struct mp3entry *id3;
    unsigned int miss_key;
    int n = 0, i, dim;

    dim = palette_dim();
    if (dim == 0)
        return false;

    /* audio_current_track() hands back the engine's live entry, not a copy,
     * so take what is needed out of it before doing anything else. */
    id3 = audio_current_track();
    if (!id3 || !id3->path[0])
        return false;
    strmemccpy(path, id3->path, sizeof(path));

    if (global_settings.wps_art_source == WPS_ART_ARTIST)
        cand[n++] = track_folder_hash(path, true);
    else
    {
        if (global_settings.wps_art_source == WPS_ART_AUTO &&
            playlist_is_from_artist())
        {
            cand[n++] = track_folder_hash(path, true);
        }
        cand[n++] = track_folder_hash(path, false);
    }

    /* The negative cache is keyed on the album folder whichever candidate was
     * tried, because that is the one hash every setting can compute: it only has
     * to name this track's folder set, and it is read back before any of them are
     * opened. Set only when they all failed, or a fallback would be skipped the
     * moment the portrait was missing. */
    miss_key = track_folder_hash(path, false);
    if (miss_key != 0 && miss_key == palette_miss)
        return false;

    for (i = 0; i < n; i++)
        if (palette_from_folder(cand[i], dim))
            return true;

    palette_miss = miss_key;
    return false;
}

/* ---------------------------------------------------------------------- *
 * The %Cl filter chain                                                    *
 * ---------------------------------------------------------------------- */

/* The chain rewrites the album art slot's own copy of the bitmap, so it has
 * to run exactly once per buffer -- twice would darken a cover twice, or
 * invert it back.
 *
 * The guard is therefore the set of art buffers already rewritten, keyed by
 * handle. A handle is the buffer, which is what makes it the right key: slots
 * dedupe by dimension, so a status bar and a WPS asking for the same size
 * share one bitmap and one entry covers both. The sharing has a consequence
 * this cannot fix, and it is worth knowing: two skins on one slot both show
 * whichever chain ran first. Declaring different sizes separates them.
 *
 * A set rather than one entry per slot because art outlives the track it was
 * loaded for. Playback keeps a handle for the album it is in the middle of and
 * hands the same one back for every track of it, so a guard cleared on a track
 * change would filter one buffer once per skip -- and a slot alternating
 * between folder art and embedded art needs both remembered at once.
 *
 * A handle id alone would not do, because buflib reissues them. What makes it
 * do is skin_albumart_art_opened(): playback calls it for every art bitmap it
 * loads, so an id that comes back meaning a different picture is struck off
 * before it can become current.
 *
 * Filtering in place cannot be undone, which shows up in one place: change
 * theme mid-album and the art keeps the outgoing theme's treatment until the
 * next album. Striking the entries instead would be worse -- the new chain
 * would compose on top of the old one's output rather than replace it.
 *
 * AA_FILTER_SLOTS is one above SKINNABLE_SCREENS_COUNT because that is the
 * largest playback.c's MAX_MULTIPLE_AA can be (it grows by one for USB iAP).
 * The set is sized for the buffer, not the slots: every buffered track keeps
 * its own cover, so a skip back lands on a handle filtered many skips ago.
 * One that has fallen out of the set is filtered a second time. */
#define AA_FILTER_SLOTS (SKINNABLE_SCREENS_COUNT + 1)
#define AA_FILTERED_MAX 128

/* Zero is the empty entry; buflib issues no such handle. */
static volatile int filtered_art[AA_FILTERED_MAX];
static int filtered_next;

/* Which run of art the guards in struct skin_albumart refer to. Bumped
 * whenever art is loaded, which is what makes a repeated handle id harmless:
 * a guard stamped with an older count is stale however well its handle
 * matches. */
static unsigned filter_gen = 1;

unsigned skin_albumart_gen(void)
{
    return filter_gen;
}

static bool art_filtered(int handle)
{
    if (handle <= 0)
        return false;

    for (int i = 0; i < AA_FILTERED_MAX; i++)
        if (filtered_art[i] == handle)
            return true;
    return false;
}

static void remember_filtered(int handle)
{
    if (handle <= 0 || art_filtered(handle))
        return;

    filtered_art[filtered_next] = handle;
    if (++filtered_next == AA_FILTERED_MAX)
        filtered_next = 0;
}

void skin_albumart_art_opened(int handle)
{
    if (handle <= 0)
        return;

    for (int i = 0; i < AA_FILTERED_MAX; i++)
        if (filtered_art[i] == handle)
            filtered_art[i] = 0;

    /* The blurring tier renders into a destination of its own, so nothing
     * there is rewritten twice and its guard asks a different question: is
     * what I rendered still the cover on screen? Counting loads rather than
     * track changes is what stops it re-rendering the same art once per track
     * of an album.
     *
     * Never 0, which is what a skin's art carries before it has rendered
     * anything -- so a freshly parsed %Cl cannot match the current run. */
    if (++filter_gen == 0)
        filter_gen = 1;
}

/* Fit a source of sw x sh inside a bw x bh box, keeping its aspect -- the
 * same shape FORMAT_KEEP_ASPECT gives the unblurred path, so the blurred
 * image lands where the crisp one would have. */
static void fit_inside(int sw, int sh, int bw, int bh, int *dw, int *dh)
{
    *dw = bw;
    *dh = sh * bw / sw;
    if (*dh > bh)
    {
        *dh = bh;
        *dw = sw * bh / sh;
    }
    if (*dw < 1) *dw = 1;
    if (*dh < 1) *dh = 1;
}

/* A blurring chain cannot work in place, so it renders into the skin's own
 * buffer instead, and leaves the buffered art alone. That is why this one
 * keeps its guard on the skin rather than on the slot: rendering the same
 * art twice into two different destinations is harmless, and two skins
 * sharing a slot each need their own copy. It is also why the colour
 * extractor is not locked out here -- the art it reads is untouched. */
static void render_filtered(struct skin_albumart *aa, int handle,
                            struct bitmap *bmp)
{
    fb_data *dst;
    int dw, dh;

    if (aa->filter_handle <= 0)
        return;

    fit_inside(bmp->width, bmp->height, aa->width, aa->height, &dw, &dh);

    /* Pinned across the render: this one does have to hold still, because
     * unlike the in-place pass it writes for as long as it reads. */
    core_pin(aa->filter_handle);
    dst = core_get_data(aa->filter_handle);
    img_filter_render(dst, dw, dh, (const fb_data *)bmp->data,
                      bmp->width, bmp->height, &aa->filter);
    core_unpin(aa->filter_handle);

    aa->filtered_width = (short)dw;
    aa->filtered_height = (short)dh;
    aa->filtered_art = handle;
    aa->filtered_gen = filter_gen;
}

void skin_albumart_filter(int aa_slot, struct skin_albumart *aa)
{
    struct img_filter *filter = aa ? &aa->filter : NULL;
    const bool resizes = filter && (filter->stages & IMG_CLASS_RESIZE);
    struct bitmap *bmp;
    int handle, avoid = -1;

    /* An adaptive chain that folded to nothing for the last picture still has
     * work to do for the next one, so its stages are not the test for whether
     * there is anything to run. */
    if (!filter || (!filter->stages && !filter->has_adaptive))
        return;
    if (aa_slot < 0 || aa_slot >= AA_FILTER_SLOTS)
        return;

    handle = playback_current_aa_hid(aa_slot);
    if (handle < 0)
        return;                                  /* not buffered yet */

    /* What text over this art will be drawn in: the album's accent when
     * dynamic colours are on and have a palette, the theme's own foreground
     * otherwise. Reading the setting through the palette rather than the
     * accent directly is what makes both cases one line. */
    if (filter->has_adaptive)
    {
        avoid = color_luminance(
                    dynamic_colors_resolve(global_settings.fg_color));

        /* A scrim is cut for one text colour, and the palette can move
         * without the art changing -- on a dynamic-colours toggle, or when
         * extraction lands later than the first render. Render it again.
         *
         * Only the blurring tier can do that: it renders into a destination
         * of its own from a source nothing has touched, so it can be run
         * twice. An in-place chain has already rewritten the buffered pixels
         * and a second pass would darken them twice over, so that tier holds
         * the scrim it has until the art is buffered afresh. */
        if (resizes && avoid != aa->filter_avoid)
            aa->filtered_art = -1;
        aa->filter_avoid = (short)avoid;
    }

    if (resizes ? (aa->filtered_art == handle
                   && aa->filtered_gen == filter_gen)
                : art_filtered(handle))
        return;                                  /* already done */

    /* The bitmap lives in the audio buffer and is movable, and this reads it
     * unpinned -- the same bargain extract_colors() makes. Nothing between
     * bufgetdata() and the last read may yield, allocate or touch the disk.
     * Neither of the two calls below does any of the three.
     *
     * Width and height rather than the drawing stride: a levels filter is
     * per-pixel and cares only that it covers the whole buffer. A positional
     * one (dither) would need the real row length. */
    if (bufgetdata(handle, 0, (void *)&bmp) <= 0)
        return;

    /* An adaptive chain names no amount and is not fit to run until it has
     * seen the picture, so its table is folded here, once per buffered image.
     * The levels come off the art as buffered -- which is the unfiltered
     * picture, and for a blurring chain is already the decimated one. */
    if (filter->has_adaptive)
    {
        struct img_levels lv;

        img_filter_measure((const fb_data *)bmp->data,
                           bmp->width, bmp->height, &lv);
        img_filter_adapt(filter, &lv, avoid);
    }

    if (resizes)
    {
        render_filtered(aa, handle, bmp);
    }
    else
    {
        img_filter_apply((fb_data *)bmp->data, bmp->width, bmp->height,
                         filter);
        remember_filtered(handle);
    }
}

static void track_change_cb(unsigned short id, void *param)
{
    (void)param;
    /* Playback that is not listening -- the Music Quiz's clips -- leaves the
     * palette alone. Asked for here, the extraction would run once the quiz
     * gives the screen back, with playback stopped: either a clip's cover
     * would become the palette, and the one the next boot comes up in, or,
     * finding no track, the theme's colours would come back. */
    if (audio_is_unrecorded())
        return;
    needs_extraction = true;
    /* Nothing is said to the filter here. A track change is not an art change:
     * skin_albumart_art_opened() is, and it is the load that calls it. */
    palette_miss = 0;
    if (id == PLAYBACK_EVENT_TRACK_CHANGE)
        cache.track_change_tick = current_tick;
}

static void save_all_theme_colors(void)
{
    cache.theme_fg  = global_settings.fg_color;
    cache.theme_bg  = global_settings.bg_color;
    cache.theme_sep = global_settings.list_separator_color;
}

void dynamic_colors_init(void)
{
    static bool events_registered = false;

    memset(&cache, 0, sizeof(cache));
    save_all_theme_colors();
    cache.was_enabled = global_settings.dynamic_colors;
    needs_extraction = false;

    if (!events_registered)
    {
        add_event(PLAYBACK_EVENT_TRACK_CHANGE, track_change_cb);
        add_event(PLAYBACK_EVENT_CUR_TRACK_READY, track_change_cb);
        events_registered = true;
    }
}

/* Come up in the colours the device was left in, before anything is drawn.
 *
 * The palette normally arrives with a track, and at boot there is no track --
 * only a resume point, which names a file but not the folder the palette is
 * derived from. That folder's hash is stored beside it, so the same cached
 * thumbnail is read again here and the same palette comes out.
 *
 * Boot only, and before the first paint: the markers a palette change usually
 * raises are wound back afterwards, because they exist to make screens already
 * on display catch up and there are none. Leaving them is what would make the
 * boot screen visibly recolour rather than simply appear. */
void dynamic_colors_seed_resume(void)
{
    int dim = palette_dim();

    if (!global_settings.dynamic_colors ||
        global_status.resume_index == -1 ||
        global_status.resume_art_hash == 0 || dim == 0)
        return;

    if (!palette_from_folder(global_status.resume_art_hash, dim))
        return;

    cache.needs_full_update = false;
    cache.needs_screen_clear = false;
    cache.change_tick = current_tick - AA_SETTLE_TICKS;
}

void dynamic_colors_save_theme(void)
{
    save_all_theme_colors();
    /* Invalidate cached colors — they were for the old theme */
    cache.valid = false;
    cache.needs_screen_clear = false;
    needs_extraction = true;
    forget_transforms();
}

void dynamic_colors_check_extraction(int aa_slot)
{
    /* Remember valid AA slots from skin_render calls so list_draw
     * can trigger extraction with aa_slot = -1 (use last known) */
    static int last_aa_slot = -1;
    if (aa_slot >= 0)
        last_aa_slot = aa_slot;
    else
        aa_slot = last_aa_slot;

    /* Stopping playback deliberately does not put the theme's colours back.
     * The palette outlives the track it came from, and is replaced only by
     * another one: a new track's art, a track that has none (the timeout
     * below), the setting going off, or a theme change.
     *
     * Do not add a revert here. A stop frees the buffered art, and
     * playback_current_aa_hid() reports none once stopped, so cache.accent
     * and cache.dominant are the last copy of the palette anywhere -- a
     * revert would discard it with no way to derive it again. */

    /* Detect setting toggle */
    bool enabled = global_settings.dynamic_colors;
    if (!enabled && cache.was_enabled && cache.valid)
    {
        /* Setting just turned off — start fade to defaults */
        apply_colors(cache.theme_fg, cache.theme_bg, true);
    }
    if (enabled && !cache.was_enabled)
    {
        /* Setting just turned on — try extraction */
        needs_extraction = true;
    }
    cache.was_enabled = enabled;

    /* Dynamic Colors Background changed: turn the pair already found, which
     * needs no art and so works while stopped too. */
    if (enabled && cache.valid &&
        cache.orientation != global_settings.dynamic_colors_background)
        apply_oriented();

    if (!needs_extraction)
        return;
    if (!enabled)
    {
        needs_extraction = false;
        return;
    }
    /* The cache first. It needs no album-art slot, so this works on a theme
     * that declares no %Cl -- which is why there is no "give up without a
     * slot" test here any more. */
    if (palette_from_cache())
    {
        needs_extraction = false;
        return;
    }

    int handle = aa_slot >= 0 ? playback_current_aa_hid(aa_slot) : -1;
    if (handle >= 0)
    {
        if (palette_from_slot(handle))
        {
            /* A skin's own picture, which the next boot cannot reach. */
            global_status.resume_art_hash = 0;
            needs_extraction = false;
            return;
        }
        /* Nothing came of the slot: a filter has rewritten it, or the handle
         * went away mid-read. Keep the palette we have -- but keep asking,
         * because the caching pass may still reach this album and the cache
         * above is the better source anyway. Bounded by the same timeout, so a
         * slot that stays filtered stops costing a retry every frame.
         *
         * Trap: clearing the flag regardless makes one bad moment permanent.
         * The first pass after a track change then decides the palette, and
         * nothing asks again. */
        if (current_tick - cache.track_change_tick > NO_ART_TIMEOUT)
            needs_extraction = false;
    }
    else
    {
        /* Neither a cached thumbnail nor a buffered bitmap. Art may still be
         * on its way, so keep trying until the timeout. */
        long elapsed = current_tick - cache.track_change_tick;
        if (elapsed > NO_ART_TIMEOUT)
        {
            /* No art for this track — fade to defaults */
            if (cache.valid)
                apply_colors(cache.theme_fg, cache.theme_bg, true);
            needs_extraction = false;
        }
        /* else: keep trying on next render */
    }
}

/* Where c sits along the theme's background-to-foreground line, 0..256, and
 * how far off that line it lies.
 *
 * Position is taken from luminance alone, which is what the line is ordered by;
 * the distance is then measured against the colour that position would give, so
 * a tint of the pair comes back near zero however dark or light it is, and
 * anything carrying a hue of its own does not. */
static int axis_position(unsigned int c, int *distance)
{
    int lum_bg = color_luminance(cache.theme_bg);
    int lum_fg = color_luminance(cache.theme_fg);
    int lum_c  = color_luminance(c);
    unsigned int on_axis;
    int t, dr, dg, db;

    if (lum_fg == lum_bg)
        t = 0;
    else
    {
        t = ((lum_c - lum_bg) * 256) / (lum_fg - lum_bg);
        t = MIN(MAX(t, 0), 256);
    }

    on_axis = color_blend(cache.theme_bg, cache.theme_fg, t);

    dr = RGB_UNPACK_RED(c)   - RGB_UNPACK_RED(on_axis);
    dg = RGB_UNPACK_GREEN(c) - RGB_UNPACK_GREEN(on_axis);
    db = RGB_UNPACK_BLUE(c)  - RGB_UNPACK_BLUE(on_axis);

    *distance = MAX(MAX(MAX(dr, -dr), MAX(dg, -dg)), MAX(db, -db));
    return t;
}

/* One turn of the colour wheel, applied to every accent the theme uses.
 *
 * The whole palette turns rigidly rather than each colour being placed on its
 * own, so a skin using several accents keeps their relationships to each other
 * as well as to the background. What a theme author picked was a relationship
 * to their background, not a wavelength: an orange chosen against navy is
 * "170 degrees round from the background", and reproducing the angle against a
 * green album gives a magenta that works there, where reproducing the orange
 * gives orange on green. */
static int transform_rotation(void)
{
    int dom_h, dom_s, dom_v;
    int bg_h, bg_s, bg_v;

    if (xform_rotation_valid)
        return xform_rotation;

    color_get_hsv(cache.dominant, &dom_h, &dom_s, &dom_v);
    color_get_hsv(cache.theme_bg, &bg_h, &bg_s, &bg_v);

    /* A theme with no hue in its background -- white on black, and most themes
     * that are not built round a colour -- has no orientation to carry over.
     * Anchor it to red, which is what an achromatic colour reports, and aim at
     * where a derived accent would sit against the album instead. Its accents
     * then land somewhere that works against the art, and still hold their
     * angles to one another. */
    if (bg_s < CHROMA_MIN)
    {
        bg_h = 0;
        dom_h += COLOR_ACCENT_ROTATE;
    }

    xform_rotation = dom_h - bg_h;
    xform_rotation_valid = true;
    return xform_rotation;
}

/* Remake a tint of the theme's own pair on the album's. This is what carries
 * the greys, the muted panel fills and the half-tone separators.
 *
 * Placed by the contrast it carried rather than at the same fraction along the
 * pair. A theme's pair spans whatever it spans -- black to white is 21:1 -- and
 * the album's can be a fraction of that, so a grey three quarters of the way
 * along the one is nowhere near as legible three quarters of the way along the
 * other. `t` is therefore a starting point, and the colour walks on toward the
 * accent until it carries the contrast it had against the theme's background,
 * or the accent's own bar, whichever asks for less. A deliberately quiet fill
 * asks for little and does not move.
 *
 * The walk stops short of the accent, because a tint has a job the accent
 * cannot do for it: a list's unselected rows read as unselected by being
 * dimmer than the selected one. A narrow album pair cannot give a tint both
 * the contrast and the separation, and arriving at the accent would quietly
 * spend all of the second on the first.
 *
 * Trap: this walks the segment between the two colours instead of calling
 * color_fit_contrast(), which moves brightness in either direction and off the
 * segment entirely. Asked for more contrast than the light end of a dark pair
 * can give, it answers with black -- which turns a dim row invisible rather
 * than dim. Staying between the pair cannot do that: the furthest it can go is
 * the accent, and it stops before even that. */
static unsigned int carry_on_axis(int t, int had)
{
    int target = MIN(had, MIN_RATIO);
    unsigned int out = color_blend(cache.dominant, cache.accent, t);

    while (t < 256 && color_contrast(out, cache.dominant) < target)
    {
        unsigned int next;
        int step = MIN(t + 4, 256);

        next = color_blend(cache.dominant, cache.accent, step);
        if (color_contrast(next, cache.accent) < TINT_MIN_SEPARATION)
            break;

        t = step;
        out = next;
    }

    return out;
}

/* Carry a colour the theme named, but which is neither its foreground nor its
 * background, onto the album's palette. */
static unsigned int transform_literal(unsigned int c)
{
    int t, distance, h, s, v;
    unsigned int out;

    t = axis_position(c, &distance);

    if (distance <= AXIS_TOLERANCE)
        return carry_on_axis(t, color_contrast(c, cache.theme_bg));

    color_get_hsv(c, &h, &s, &v);
    out = color_from_hsv(h + transform_rotation(), s, v);

    /* Ask for legibility, or for what it already had if that was less -- a
     * deliberately quiet colour stays quiet.
     *
     * Not for the contrast it had against the theme's background, which is the
     * obvious reading and is unreachable: a dark theme background allows ratios
     * no colour can reach against a mid-luminance album, so the target would
     * always fail and fall back to plain black or white, throwing away the hue
     * this exists to keep. */
    return color_fit_contrast(out, cache.dominant,
                              MIN(color_contrast(c, cache.theme_bg),
                                  ACCENT_MIN_RATIO));
}

static unsigned int transform_cached(unsigned int c)
{
    unsigned int out;
    int i;

    for (i = 0; i < xform_entries; i++)
        if (xform_cache[i].in == c)
            return xform_cache[i].out;

    out = transform_literal(c);

    /* A skin with more distinct colours than the table holds simply stops
     * caching, and pays the transform on every resolve of the rest: up to 64
     * blend steps with two contrast divides each for a colour on the album's
     * axis (carry_on_axis()). Grow the table before a skin needs more. */
    if (xform_entries < XFORM_CACHE_SIZE)
    {
        xform_cache[xform_entries].in  = c;
        xform_cache[xform_entries].out = out;
        xform_entries++;
    }

    return out;
}

/* Map a color to its dynamic equivalent using pre-computed effective colors.
 * The six colours the theme names are matched by value and mapped by role;
 * anything else is a colour a skin chose, and is carried over by
 * transform_cached(). */
static unsigned int resolve_mapped(unsigned int original,
                                   unsigned int eff_accent,
                                   unsigned int eff_dominant)
{
    /* Primary colors: always mapped */
    if (original == cache.theme_fg)
        return eff_accent;
    if (original == cache.theme_bg)
        return eff_dominant;

    /* List separator: a hairline meant to be barely there rather than a colour
     * in its own right, so it is placed next to the background instead of
     * being carried over on its own merits. */
    if (original == cache.theme_sep)
        return lerp_color(eff_dominant, eff_accent, 64);

    /* Everything else: the selector colours when the theme gave them values of
     * their own, and every colour a skin spells out. Carried over.
     *
     * The selector colours need no cases here. Written as the theme's own
     * foreground or background they arrive as those values and are answered
     * above; given a third colour they meant it, and it is carried over like
     * any other. Trap: forcing the bar to the accent instead collides whenever
     * the selector text is the theme foreground -- the common way to write a
     * theme, and the foreground maps to the accent too, so bar and text land on
     * one colour and the selected row goes blank. */
    return transform_cached(original);
}

/* What a palette word starts from, before its bound and shade.
 *
 * `vivid` on an album with no colourful colour falls back on the pair: the
 * lighter of the two for a `>` bound, the darker for a `<`, and the accent
 * with none, so a black-and-white sleeve keeps its own off-white or
 * near-black rather than one the bound would have to invent. */
static unsigned int word_base(const struct color_word *w)
{
    unsigned int black = LCD_RGBPACK(0, 0, 0);
    bool accent_lighter;

    if (w->kind == COLOR_WORD_ACCENT)
        return cache.accent;
    if (w->kind == COLOR_WORD_DOMINANT)
        return cache.dominant;
    if (cache.has_vivid)
        return cache.vivid;
    if (w->bound == 0)
        return cache.accent;
    accent_lighter = color_contrast(cache.accent, black) >
                     color_contrast(cache.dominant, black);
    return (w->bound > 0) == accent_lighter ? cache.accent : cache.dominant;
}

/* A palette word: the album's colour by role, then the tone bound, then the
 * shade. With no palette it is the fallback the skin wrote, as written, or
 * failing that the theme's foreground (its background, for `dominant`),
 * bounded and shaded the same way. */
static unsigned int resolve_word(unsigned int original, bool palette)
{
    const struct color_word *w = color_word(original);
    int i = COLOR_WORD_INDEX(original);
    unsigned char from = palette ? WORD_FROM_PALETTE : WORD_FROM_THEME;
    unsigned int out;

    if (!w)
        return original & ~COLOR_WORD;  /* not reached: parsed as a word */
    if (!palette && w->has_fallback)
        return w->fallback;
    if (word_from[i] == from)
        return word_out[i];

    if (palette)
        out = word_base(w);
    else if (w->kind == COLOR_WORD_DOMINANT)
        out = global_settings.bg_color;
    else
        out = global_settings.fg_color;

    out = color_tone_bound(out, w->tone, w->bound);
    if (w->shade)
        out = color_blend(LCD_RGBPACK(0, 0, 0), out,
                          ((w->shade - 1) * 256) / 100);

    word_out[i] = out;
    word_from[i] = from;
    return out;
}

unsigned int dynamic_colors_resolve(unsigned int original)
{
    bool palette = global_settings.dynamic_colors && cache.valid;

    /* Before the checks below, not after: the flag has to come off whether or
     * not there is a palette to resolve against, since this is the only place
     * that knows about it and the value goes on to the display from here. */
    if (original & COLOR_FIXED)
        return original & ~COLOR_FIXED;

    if (original & COLOR_WORD)
        return resolve_word(original, palette);

    if (!palette)
        return original;

    return resolve_mapped(original, cache.accent, cache.dominant);
}

bool dynamic_colors_needs_repaint(void)
{
    /* The window only has to outlast one refresh of the slowest screen that
     * polls this. Every repaint inside it draws the same final colour, so
     * there is nothing for those screens to disagree about. */
    return TIME_BEFORE(current_tick, cache.change_tick + AA_SETTLE_TICKS);
}

bool dynamic_colors_needs_full_update(void)
{
    if (cache.needs_full_update)
    {
        cache.needs_full_update = false;
        return true;
    }
    return false;
}

bool dynamic_colors_screen_clear_needed(void)
{
    if (cache.needs_screen_clear)
    {
        cache.needs_screen_clear = false;
        return true;
    }
    return false;
}

bool dynamic_colors_pending(void)
{
    return needs_extraction && global_settings.dynamic_colors;
}

