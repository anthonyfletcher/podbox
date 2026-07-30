/***************************************************************************
 * Original code from RockPod
 * was: apps/gui/skin_engine/skin_albumart_color.c
 * Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Extracts a dominant colour palette from the current album art and
 * exposes it to skins as dynamic colours, with fading between tracks.
 *
 * Extraction runs once per track, not per frame: the art is sampled into
 * coarse colour buckets, the most populous are picked as the palette, then
 * adjusted for contrast so text drawn in them stays readable against the
 * background. Skins read the result through the dynamic-colour tags.
 *
 * The fade between tracks is not animated by a timer. start_fade() records
 * the old and new palettes and a start tick; every colour lookup during the
 * fade interpolates between them by however far current_tick has got. So the
 * fade advances only as fast as the screen is repainted, and asking for a
 * colour is what makes it progress.
 *
 * Parts, in order:
 *   - luminance, interpolation and fade progress helpers
 *   - extract_colors(): sampling the bitmap and choosing the palette
 *   - track-change hook and theme colour save/restore
 *   - the accessors skins resolve their colour tags through, which also
 *     report whether a fade is still in flight and needs another repaint
 ****************************************************************************/

#include "config.h"


#include <string.h>
#include "lcd.h"
#include "settings/settings.h"
#include "kernel.h"
#include "audio.h"
#include "audio/playback.h"
#include "audio/buffering.h"
#include "system/appevents.h"
#include "draw/color.h"              /* color_blend */
#include "skin_albumart_color.h"

#define AA_FADE_DURATION  (HZ / 4)   /* 250ms */
#define HISTOGRAM_BUCKETS 4096
#define SAMPLE_STRIDE     4          /* sample every 4th pixel */
#define MIN_RATIO         600        /* target contrast ratio x100 (6:1) */
#define SATURATION_BASE   8          /* base score for unsaturated colors */
#define NO_ART_TIMEOUT    HZ         /* 1s timeout before concluding no art */

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

struct dynamic_colors_cache {
    unsigned int dominant;       /* target bg color */
    unsigned int accent;         /* target fg color */
    unsigned int prev_dominant;  /* fade-start bg */
    unsigned int prev_accent;    /* fade-start fg */
    unsigned int theme_fg;       /* saved original theme fg */
    unsigned int theme_bg;       /* saved original theme bg */
    unsigned int theme_lss;      /* saved selector start color */
    unsigned int theme_lse;      /* saved selector end color */
    unsigned int theme_lst;      /* saved selector text color */
    unsigned int theme_sep;      /* saved list separator color */
    long fade_start_tick;
    long track_change_tick;      /* when TRACK_CHANGE fired */
    bool valid;                  /* have valid AA colors */
    bool fading;                 /* fade in progress */
    bool fading_out;             /* fading to defaults after disable/stop */
    bool was_enabled;            /* track setting state for toggle detection */
    bool needs_full_update;      /* set when fade completes for full redraw */
    bool needs_screen_clear;     /* set when fade completes to clear bg gaps */
};

static struct dynamic_colors_cache cache;
static volatile bool needs_extraction;
static uint16_t histogram[HISTOGRAM_BUCKETS];

static int compute_luminance(int r8, int g8, int b8)
{
    return (r8 * 77 + g8 * 150 + b8 * 29) >> 8;
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

static int fade_progress(void)
{
    long elapsed = current_tick - cache.fade_start_tick;
    if (elapsed <= 0)
        return 0;
    if (elapsed >= AA_FADE_DURATION)
        return 256;
    return (int)((elapsed * 256) / AA_FADE_DURATION);
}

static void start_fade(unsigned int new_accent, unsigned int new_dominant,
                       bool to_defaults)
{
    /* Capture current effective colors as fade start */
    if (cache.fading || cache.fading_out)
    {
        int p = fade_progress();
        if (cache.fading_out)
        {
            cache.prev_accent = lerp_color(cache.prev_accent,
                                           cache.theme_fg, p);
            cache.prev_dominant = lerp_color(cache.prev_dominant,
                                             cache.theme_bg, p);
        }
        else
        {
            cache.prev_accent = lerp_color(cache.prev_accent,
                                           cache.accent, p);
            cache.prev_dominant = lerp_color(cache.prev_dominant,
                                             cache.dominant, p);
        }
    }
    else if (cache.valid)
    {
        cache.prev_accent = cache.accent;
        cache.prev_dominant = cache.dominant;
    }
    else
    {
        cache.prev_accent = cache.theme_fg;
        cache.prev_dominant = cache.theme_bg;
    }

    cache.accent = new_accent;
    cache.dominant = new_dominant;
    cache.fade_start_tick = current_tick;
    cache.fading = !to_defaults;
    cache.fading_out = to_defaults;
    cache.valid = true;
}

static void extract_colors(const struct bitmap *bmp)
{
    if (!bmp->data || bmp->width <= 0 || bmp->height <= 0)
        return;

    fb_data *pixels = (fb_data *)bmp->data;
    int width = bmp->width;
    int height = bmp->height;
    int total_pixels = width * height;
    int i;

    memset(histogram, 0, sizeof(histogram));

    /* Pass 1: build quantized histogram */
    for (i = 0; i < total_pixels; i += SAMPLE_STRIDE)
    {
        unsigned short px = (unsigned short)pixels[i];
        int r4 = (px >> 12) & 0xF;
        int g4 = (px >> 7) & 0xF;
        int b4 = (px >> 1) & 0xF;
        int bucket = (r4 << 8) | (g4 << 4) | b4;
        if (histogram[bucket] < UINT16_MAX)
            histogram[bucket]++;
    }

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
    long sum_r = 0, sum_g = 0, sum_b = 0;
    int count = 0;

    for (i = 0; i < total_pixels; i += SAMPLE_STRIDE)
    {
        unsigned short px = (unsigned short)pixels[i];
        int r4 = (px >> 12) & 0xF;
        int g4 = (px >> 7) & 0xF;
        int b4 = (px >> 1) & 0xF;
        int bucket = (r4 << 8) | (g4 << 4) | b4;

        if (bucket == best_bucket)
        {
            sum_r += RGB_UNPACK_RED(px);
            sum_g += RGB_UNPACK_GREEN(px);
            sum_b += RGB_UNPACK_BLUE(px);
            count++;
        }
    }

    int dom_r = count ? (int)(sum_r / count) : 0;
    int dom_g = count ? (int)(sum_g / count) : 0;
    int dom_b = count ? (int)(sum_b / count) : 0;
    unsigned int dominant = LCD_RGBPACK(dom_r, dom_g, dom_b);
    int dom_lum = compute_luminance(dom_r, dom_g, dom_b);
    int32_t dom_rl = rel_luminance(dom_r, dom_g, dom_b);

    /* Find accent: best-scored bucket with sufficient contrast */
    int accent_bucket = -1;
    unsigned int accent_score = 0;

    for (i = 0; i < HISTOGRAM_BUCKETS; i++)
    {
        if (histogram[i] == 0 || i == best_bucket)
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

    int acc_r, acc_g, acc_b;
    unsigned int accent;
    if (accent_bucket >= 0)
    {
        /* Average full-precision RGB for accent bucket */
        sum_r = sum_g = sum_b = 0;
        count = 0;
        for (i = 0; i < total_pixels; i += SAMPLE_STRIDE)
        {
            unsigned short px = (unsigned short)pixels[i];
            int r4 = (px >> 12) & 0xF;
            int g4 = (px >> 7) & 0xF;
            int b4 = (px >> 1) & 0xF;
            int bucket = (r4 << 8) | (g4 << 4) | b4;

            if (bucket == accent_bucket)
            {
                sum_r += RGB_UNPACK_RED(px);
                sum_g += RGB_UNPACK_GREEN(px);
                sum_b += RGB_UNPACK_BLUE(px);
                count++;
            }
        }
        acc_r = count ? (int)(sum_r / count) : 0;
        acc_g = count ? (int)(sum_g / count) : 0;
        acc_b = count ? (int)(sum_b / count) : 0;
        accent = LCD_RGBPACK(acc_r, acc_g, acc_b);
    }
    else
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
        accent = LCD_RGBPACK(acc_r, acc_g, acc_b);
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

        accent = LCD_RGBPACK(acc_r, acc_g, acc_b);
    }

    start_fade(accent, dominant, false);
}

static void track_change_cb(unsigned short id, void *param)
{
    (void)param;
    needs_extraction = true;
    if (id == PLAYBACK_EVENT_TRACK_CHANGE)
        cache.track_change_tick = current_tick;
}

static void save_all_theme_colors(void)
{
    cache.theme_fg  = global_settings.fg_color;
    cache.theme_bg  = global_settings.bg_color;
    cache.theme_lss = global_settings.lss_color;
    cache.theme_lse = global_settings.lse_color;
    cache.theme_lst = global_settings.lst_color;
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

void dynamic_colors_save_theme(void)
{
    save_all_theme_colors();
    /* Invalidate cached colors — they were for the old theme */
    cache.valid = false;
    cache.fading = false;
    cache.fading_out = false;
    cache.needs_screen_clear = false;
    needs_extraction = true;
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

    /* Fade back to the theme when there is no longer a track to take colours
     * from -- which is not the same as playback having stopped. A stopped
     * track is still the one on the playing screen, and its colours belong
     * with it; only an empty path means there is nothing left to match.
     *
     * Pausing never reached here anyway: PLAY_PAUSED is
     * AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE, so the PLAY bit stays set.
     *
     * The status test is kept in front as a guard, not as the condition. It is
     * cheap and false whenever anything is playing, which keeps
     * audio_current_track() -- a mutex and a metadata fetch -- off the render
     * path except while stopped. */
    if (cache.valid && !cache.fading_out &&
        global_settings.dynamic_colors &&
        !(audio_status() & AUDIO_STATUS_PLAY) &&
        audio_current_track()->path[0] == '\0')
    {
        start_fade(cache.theme_fg, cache.theme_bg, true);
    }

    /* Detect setting toggle */
    bool enabled = global_settings.dynamic_colors;
    if (!enabled && cache.was_enabled && cache.valid && !cache.fading_out)
    {
        /* Setting just turned off — start fade to defaults */
        start_fade(cache.theme_fg, cache.theme_bg, true);
    }
    if (enabled && !cache.was_enabled)
    {
        /* Setting just turned on — try extraction */
        needs_extraction = true;
    }
    cache.was_enabled = enabled;

    if (!needs_extraction)
        return;
    if (!enabled)
    {
        needs_extraction = false;
        return;
    }
    if (aa_slot < 0)
    {
        /* No known AA slot yet — can't extract */
        return;
    }

    int handle = playback_current_aa_hid(aa_slot);
    if (handle >= 0)
    {
        struct bitmap *bmp;
        if (bufgetdata(handle, 0, (void *)&bmp) > 0)
            extract_colors(bmp);
        needs_extraction = false;
    }
    else
    {
        /* Art not available yet — check timeout */
        long elapsed = current_tick - cache.track_change_tick;
        if (elapsed > NO_ART_TIMEOUT)
        {
            /* No art for this track — fade to defaults */
            if (cache.valid && !cache.fading_out)
                start_fade(cache.theme_fg, cache.theme_bg, true);
            needs_extraction = false;
        }
        /* else: keep trying on next render */
    }
}

/* Map a color to its dynamic equivalent using pre-computed effective colors.
 * Matches against all 6 saved theme colors with role-based fallbacks. */
static unsigned int resolve_mapped(unsigned int original,
                                   unsigned int eff_accent,
                                   unsigned int eff_dominant)
{
    /* Primary colors: always mapped */
    if (original == cache.theme_fg)
        return eff_accent;
    if (original == cache.theme_bg)
        return eff_dominant;

    /* Selector bar: default to accent (eye-catching highlight) */
    if (original == cache.theme_lss)
        return (cache.theme_lss == cache.theme_fg) ? eff_accent :
               (cache.theme_lss == cache.theme_bg) ? eff_dominant : eff_accent;

    /* Selector text: should contrast with bar, default to dominant */
    if (original == cache.theme_lst)
        return (cache.theme_lst == cache.theme_bg) ? eff_dominant :
               (cache.theme_lst == cache.theme_fg) ? eff_accent : eff_dominant;

    /* Selector gradient end: blend accent toward dominant */
    if (original == cache.theme_lse)
        return (cache.theme_lse == cache.theme_fg) ? eff_accent :
               (cache.theme_lse == cache.theme_bg) ? eff_dominant :
               lerp_color(eff_accent, eff_dominant, 64);

    /* List separator: subtle line, blend dominant toward accent */
    if (original == cache.theme_sep)
        return lerp_color(eff_dominant, eff_accent, 64);

    return original;
}

unsigned int dynamic_colors_resolve(unsigned int original)
{
    /* Fade-out continues even after setting is toggled off */
    if (cache.fading_out)
    {
        int p = fade_progress();
        if (p >= 256)
        {
            cache.fading_out = false;
            cache.valid = false;
            cache.needs_full_update = true;
            cache.needs_screen_clear = true;
            return original;
        }
        return resolve_mapped(original,
            lerp_color(cache.prev_accent, cache.theme_fg, p),
            lerp_color(cache.prev_dominant, cache.theme_bg, p));
    }

    if (!global_settings.dynamic_colors || !cache.valid)
        return original;

    if (cache.fading)
    {
        int p = fade_progress();
        if (p >= 256)
        {
            cache.fading = false;
            cache.needs_full_update = true;
            cache.needs_screen_clear = true;
        }
        else
            return resolve_mapped(original,
                lerp_color(cache.prev_accent, cache.accent, p),
                lerp_color(cache.prev_dominant, cache.dominant, p));
    }

    return resolve_mapped(original, cache.accent, cache.dominant);
}

bool dynamic_colors_fading(void)
{
    return cache.fading || cache.fading_out;
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

