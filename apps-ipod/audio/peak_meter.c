/***************************************************************************
 * Original code from RockBox
 * was: apps/recorder/peakmeter.c
 * Copyright (C) 2002 by Philipp Pertermann
 * GNU General Public License (version 2+)
 *
 * Audio peak meter. Samples the PCM peaks, converts to dB with a
 * logarithmic scale, and draws the bar with peak-hold and clip indication.
 ****************************************************************************/
#include "config.h"
#include "thread.h"
#include "kernel.h"
#include "settings/settings.h"
#include "storage.h"
#include "lcd.h"
#include "draw/scrollbar.h"
#include "string.h"
#include "button.h"
#include "system.h"
#include "font.h"
#include "draw/icon_bitmaps.h"
#include "lang.h"
#include "peak_meter.h"
#include "audio.h"
#include "draw/screen_access.h"
#include "backlight.h"
#include "input/action.h"

#include "pcm.h"
#include "pcm_mixer.h"


static bool pm_playback = true; /* selects between playback and recording peaks */

static struct meter_scales scales[NB_SCREENS];

/* Current values and cumulation */
static int pm_cur_left;        /* current values (last peak_meter_peek) */
static int pm_cur_right;
static int pm_max_left;        /* maximum values between peak meter draws */
static int pm_max_right;

/* Clip hold */
static bool pm_clip_left = false;  /* when true a clip has occurred */
static bool pm_clip_right = false;
static long pm_clip_timeout_l;     /* clip hold timeouts */
static long pm_clip_timeout_r;

/* Temporarily en- / disables peak meter. This is especially for external
   applications to detect if the peak_meter is in use and needs drawing at all */
static bool peak_meter_enabled = true;
void peak_meter_enable(bool enable)
{
    peak_meter_enabled = enable;
}

/** Parameters **/
/* Range */
static unsigned short peak_meter_range_min;  /* minimum of range in samples */
static unsigned short peak_meter_range_max;  /* maximum of range in samples */
static unsigned short pm_range;       /* range width in samples */
static bool pm_use_dbfs = true;       /* true if peakmeter displays dBfs */
static bool level_check;              /* true if peeked at peakmeter before drawing */
static unsigned short pm_db_min = 0;      /* minimum of range in 1/100 dB */
static unsigned short pm_db_max = 9000;   /* maximum of range in 1/100 dB */
static unsigned short pm_db_range = 9000; /* range width in 1/100 dB */
/* Timing behaviour */
static int pm_peak_hold     = HZ / 5;  /* peak hold timeout ticks */
static int pm_peak_release  = 8;       /* peak release in units per read */
static int pm_clip_hold     = HZ * 60; /* clip hold timeout ticks */
static bool pm_clip_eternal = false;   /* true if clip timeout is disabled */


/* debug only */
#ifdef PM_DEBUG
static int peek_calls = 0;

#define PEEKS_PER_DRAW_SIZE 40
static unsigned int peeks_per_redraw[PEEKS_PER_DRAW_SIZE];

#define TICKS_PER_DRAW_SIZE 20
static unsigned int ticks_per_redraw[TICKS_PER_DRAW_SIZE];
#endif


static void peak_meter_draw(struct screen *display, struct meter_scales *meter_scales,
                            int x, int y, int width, int height);

/* precalculated peak values that represent magical
   dBfs values. Used to draw the scale */
static const short db_scale_src_values[DB_SCALE_SRC_VALUES_SIZE] = {
    32736, /*   0 db */
    22752, /* - 3 db */
    16640, /* - 6 db */
    11648, /* - 9 db */
    8320,  /* -12 db */
    4364,  /* -18 db */
    2064,  /* -24 db */
    1194,  /* -30 db */
    363,   /* -40 db */
    101,   /* -50 db */
    34,    /* -60 db */
    0,     /* -inf   */
};

static int db_scale_count = DB_SCALE_SRC_VALUES_SIZE;

/**
 * Calculates dB Value for the peak meter, uses peak value as input
 * @param int sample - The input value
 *                    Make sure that 0 <= value < SAMPLE_RANGE
 *
 * @return int - The 2 digit fixed point result of the euation
 *               20 * log (sample / SAMPLE_RANGE) + 90
 *               Output range is 0-9000 (that is 0.0 - 90.0 dB).
 *               Normally 0dB is full scale, here it is shifted +90dB.
 *               The calculation is based on the results of a linear
 *               approximation tool written specifically for this problem
 *               by Andreas Zwirtes (radhard@gmx.de). The result has an
 *               accurracy of better than 2%.
 *
 *               The search tree does a binary search on the input value.
 *
 *               Marked "NO_INLINE" because GCC really wants to inline it
 *               in some cases.
 *.
 *               Improved by Jvo Studer for errors < 0.2dB for critical
 *               range of -12dB to 0dB (78.0 to 90.0dB).
 */

static NO_INLINE int calc_db (int isample)
{
    /* Search tree:
     *
     *            ________2308_______
     *       ___115___             _12932_
     *    __24__   __534__     _6394_   _22450_
     *   _5_   d0 d1     d2   d3    d4 d5     d6
     *  d7 d8
     */
    static const int16_t keys[] =
    {
            0, /* 1-based array */
         2308,
          115,
        12932,
           24,
          534,
         6394,
        22450,
            5,
    };

    /* To ensure no overflow in the computations throughout the operational
       range, M_FRAC_BITS must be 20 or less. The full range of isample for
       20 fractional bits without overflowing int32_t is [-4, 90903]. */
    #define M_FRAC_BITS 20
    #define M_FIXED(m)  ((int32_t)(((int64_t)(m) << M_FRAC_BITS) / 100))

    static const struct {
        int16_t istart;
        int16_t n;
        int32_t m;
    } data[] =
    {
        { .istart =    24, .n = 2858, .m = M_FIXED( 1498) },
        { .istart =   114, .n = 4207, .m = M_FIXED(  319) },
        { .istart =   588, .n = 5583, .m = M_FIXED(   69) },
        { .istart =  2608, .n = 6832, .m = M_FIXED(   21) },
        { .istart =  7000, .n = 7682, .m = M_FIXED(    9) },
        { .istart = 13000, .n = 8219, .m = M_FIXED(    5) },
        { .istart = 22636, .n = 8697, .m = M_FIXED(    3) },
        { .istart =     1, .n =   98, .m = M_FIXED(34950) },
        { .istart =     5, .n = 1496, .m = M_FIXED( 7168) },
    };

    int i = 1;

    /* Runs 3 times if isample >= 24, else 4 */
    while (i < 9) {
        i = 2*i + (isample >= keys[i]);
    }

    i -= 9;

    return data[i].n + (data[i].m * (isample - data[i].istart) >> M_FRAC_BITS);
}

/**
 * A helper function for peak_meter_db2sample. Don't call it separately but
 * use peak_meter_db2sample. If one or both of min and max are outside the
 * range 0 <= min (or max) < 8961 the behaviour of this function is
 * undefined. It may not return.
 * @param int min - The minimum of the value range that is searched.
 * @param int max - The maximum of the value range that is searched.
 * @param int db  - The value in dBfs * (-100) for which the according
 *                  minimal peak sample is searched.
 * @return int - A linear volume value with 0 <= value < MAX_PEAK
 */
static int db_to_sample_bin_search(int min, int max, int db)
{
    int test = min + (max - min) / 2;

    if (min < max) {
        if (calc_db(test) < db) {
            test = db_to_sample_bin_search(test, max, db);
        } else {
            if (calc_db(test-1) > db) {
                test = db_to_sample_bin_search(min, test, db);
            }
        }
    }
    return test;
}

/**
 * Converts a value representing dBfs to a linear
 * scaled volume info as it is used by the MAS.
 * An incredibly inefficiant function which is
 * the vague inverse of calc_db. This really
 * should be replaced by something better soon.
 *
 * @param int db - A dBfs * 100 value with
 *                 -9000 < value <= 0
 * @return int - The return value is in the range of
 *               0 <= return value < MAX_PEAK
 */
int peak_meter_db2sample(int db)
{
    int retval = 0;

    /* what is the maximum pseudo db value */
    int max_peak_db = calc_db(MAX_PEAK - 1);

    /* range check: db value to big */
    if (max_peak_db + db < 0) {
        retval = 0;
    }

    /* range check: db value too small */
    else if (max_peak_db + db >= max_peak_db) {
        retval = MAX_PEAK -1;
    }

    /* value in range: find the matching linear value */
    else {
        retval = db_to_sample_bin_search(0, MAX_PEAK, max_peak_db + db);

        /* as this is a dirty function anyway, we want to adjust the
           full scale hit manually to avoid users complaining that when
           they adjust maximum for 0 dBfs and display it in percent it
           shows 99%. That is due to precision loss and this is the
           optical fix */
    }

    return retval;
}

/**
 * Set the min value for restriction of the value range.
 * @param int newmin - depending whether dBfs is used
 * newmin is a value in dBfs * 100 or in linear percent values.
 * for dBfs: -9000 < newmin <= 0
 * for linear: 0 <= newmin <= 100
 */
static void peak_meter_set_min(int newmin)
{
    if (pm_use_dbfs) {
        peak_meter_range_min = peak_meter_db2sample(newmin);

    } else {
        if (newmin < peak_meter_range_max) {
            peak_meter_range_min = newmin * MAX_PEAK / 100;
        }
    }

    pm_range = peak_meter_range_max - peak_meter_range_min;

    /* Avoid division by zero. */
    if (pm_range == 0) {
        pm_range = 1;
    }

    pm_db_min = calc_db(peak_meter_range_min);
    pm_db_range = pm_db_max - pm_db_min;

    FOR_NB_SCREENS(i)
        scales[i].db_scale_valid = false;
}

/**
 * Returns the minimum value of the range the meter
 * displays. If the scale is set to dBfs it returns
 * dBfs values * 100 or linear percent values.
 * @return: using dBfs : -9000 < value <= 0
 *          using linear scale: 0 <= value <= 100
 */
int peak_meter_get_min(void)
{
    int retval = 0;
    if (pm_use_dbfs) {
        retval = calc_db(peak_meter_range_min) - calc_db(MAX_PEAK - 1);
    } else {
        retval = peak_meter_range_min * 100 / MAX_PEAK;
    }
    return retval;
}

/**
 * Set the max value for restriction of the value range.
 * @param int newmax - depending wether dBfs is used
 * newmax is a value in dBfs * 100 or in linear percent values.
 * for dBfs: -9000 < newmax <= 0
 * for linear: 0 <= newmax <= 100
 */
static void peak_meter_set_max(int newmax)
{
    if (pm_use_dbfs) {
        peak_meter_range_max = peak_meter_db2sample(newmax);
    } else {
        if (newmax > peak_meter_range_min) {
            peak_meter_range_max = newmax * MAX_PEAK / 100;
        }
    }

    pm_range = peak_meter_range_max - peak_meter_range_min;

    /* Avoid division by zero. */
    if (pm_range == 0) {
        pm_range = 1;
    }

    pm_db_max = calc_db(peak_meter_range_max);
    pm_db_range = pm_db_max - pm_db_min;

    FOR_NB_SCREENS(i)
        scales[i].db_scale_valid = false;
}

/**
 * Returns the minimum value of the range the meter
 * displays. If the scale is set to dBfs it returns
 * dBfs values * 100 or linear percent values
 * @return: using dBfs : -9000 < value <= 0
 *          using linear scale: 0 <= value <= 100
 */
int peak_meter_get_max(void)
{
    int retval = 0;
    if (pm_use_dbfs) {
        retval = calc_db(peak_meter_range_max) - calc_db(MAX_PEAK - 1);
    } else {
        retval = peak_meter_range_max * 100 / MAX_PEAK;
    }
    return retval;
}

/**
 * Returns whether the meter is currently displaying dBfs or percent values.
 * @return bool - true if the meter is displaying dBfs
                  false if the meter is displaying percent values.
 */
bool peak_meter_get_use_dbfs(void)
{
    return pm_use_dbfs;
}

/**
 * Specifies whether the values displayed are scaled
 * as dBfs or as linear percent values.
 * @param use - set to true for dBfs,
 *              set to false for linear scaling in percent
 */
void peak_meter_set_use_dbfs(bool use)
{
    pm_use_dbfs = use;
    FOR_NB_SCREENS(i)
        scales[i].db_scale_valid = false;
}

/**
 * Initialize the range of the meter. Only values
 * that are in the range of [range_min ... range_max]
 * are displayed.
 * @param bool dbfs - set to true for dBfs,
 *                    set to false for linear scaling in percent
 * @param int range_min - Specifies the lower value of the range.
 *                        Pass a value dBfs * 100 when dbfs is set to true.
 *                        Pass a percent value when dbfs is set to false.
 * @param int range_max - Specifies the upper value of the range.
 *                        Pass a value dBfs * 100 when dbfs is set to true.
 *                        Pass a percent value when dbfs is set to false.
 */
void peak_meter_init_range( bool dbfs, int range_min, int range_max)
{
    pm_use_dbfs = dbfs;
    peak_meter_set_min(range_min);
    peak_meter_set_max(range_max);
}

/**
 * Initialize the peak meter with all relevant values concerning times.
 * @param int release - Set the maximum amount of pixels the meter is allowed
 *                      to decrease with each redraw
 * @param int hold_ms - Select the time in ms for the time the peak indicator
 *                      is reset after a peak occurred.
 * @param int clip_hold_sec - Select the time in seconds for the time the peak
 *                            indicator is reset after a peak occurred.
 */
void peak_meter_init_times(int release, int hold_ms, int clip_hold_sec)
{
    pm_peak_hold = hold_ms/(1000UL/HZ); /* convert ms to ticks */
    pm_peak_release = release;
    pm_clip_hold = HZ * clip_hold_sec;
}


/**
 * Set the source of the peak meter to playback or to
 * record.
 * @param: bool playback - If true playback peak meter is used.
 *         If false recording peak meter is used.
 */
void peak_meter_playback(bool playback)
{
    pm_playback = playback;
    /* reset the scales just in case recording and playback
       use different viewport sizes. Normally we should be checking viewport
       sizes every time but this will do for now */
    FOR_NB_SCREENS(i)
            scales[i].db_scale_valid = false;
}


/**
 * Reads peak values from the MAS, and detects clips. The
 * values are stored in pm_max_left pm_max_right for later
 * evauluation. Consecutive calls to peak_meter_peek detect
 * that ocurred. This function could be used by a thread for
 * busy reading the MAS.
 */
void peak_meter_peek(void)
{
    int left, right;
   /* read current values */
    if (pm_playback)
    {
        static struct pcm_peaks chan_peaks; /* *MUST* be static */
        mixer_channel_calculate_peaks(PCM_MIXER_CHAN_PLAYBACK,
                                      &chan_peaks);
        pm_cur_left = chan_peaks.left;
        pm_cur_right = chan_peaks.right;
    }
    left  = pm_cur_left;
    right = pm_cur_right;

    /* check for clips
       An clip is assumed when two consecutive readouts
       of the volume are at full scale. This is proven
       to be inaccurate in both ways: it may detect clips
       when no clip occurred and it may fail to detect
       a real clip. For software codecs, the peak is already
       the max of a bunch of samples, so use one max value
       or you fail to detect clipping! */
    if (left == MAX_PEAK - 1) {
        pm_clip_left = true;
        pm_clip_timeout_l = current_tick + pm_clip_hold;
    }

    if (right == MAX_PEAK - 1) {
        pm_clip_right = true;
        pm_clip_timeout_r = current_tick + pm_clip_hold;
    }


    /* peaks are searched -> we have to find the maximum. When
       many calls of peak_meter_peek the maximum value will be
       stored in pm_max_xxx. This maximum is reset by the
       functions peak_meter_read_x. */
    pm_max_left = MAX(pm_max_left, left);
    pm_max_right = MAX(pm_max_right, right);

    /* check levels next time peakmeter drawn */
    level_check = true;
#ifdef PM_DEBUG
    peek_calls++;
#endif
}

/**
 * Reads out the peak volume of the left channel.
 * @return int - The maximum value that has been detected
 * since the last call of peak_meter_read_l. The value
 * is in the range 0 <= value < MAX_PEAK.
 */
static int peak_meter_read_l(void)
{
    /* pm_max_left contains the maximum of all peak values that were read
       by peak_meter_peek since the last call of peak_meter_read_l */
    int retval;

    retval = pm_max_left;

#ifdef PM_DEBUG
    peek_calls = 0;
#endif
    /* reset pm_max_left so that subsequent calls of peak_meter_peek don't
       get fooled by an old maximum value */
    pm_max_left = pm_cur_left;

    return retval;
}

/**
 * Reads out the peak volume of the right channel.
 * @return int - The maximum value that has been detected
 * since the last call of peak_meter_read_l. The value
 * is in the range 0 <= value < MAX_PEAK.
 */
static int peak_meter_read_r(void)
{
    /* peak_meter_r contains the maximum of all peak values that were read
       by peak_meter_peek since the last call of peak_meter_read_r */
    int retval;

    retval = pm_max_right;

#ifdef PM_DEBUG
    peek_calls = 0;
#endif
    /* reset pm_max_right so that subsequent calls of peak_meter_peek don't
       get fooled by an old maximum value */
    pm_max_right = pm_cur_right;

    return retval;
}


/**
 * Reset the detected clips. This method is for
 * use by the user interface.
 * @param int unused - This parameter was added to
 * make the function compatible with set_int
 */
void peak_meter_set_clip_hold(int time)
{
    pm_clip_left = false;
    pm_clip_right = false;
    pm_clip_eternal = (time > 0) ? false : true;
}

/**
 * Scales a peak value as read from the MAS to the range of meterwidth.
 * The scaling is performed according to the scaling method (dBfs / linear)
 * and the range (peak_meter_range_min .. peak_meter_range_max).
 * @param unsigned short val - The volume value. Range: 0 <= val < MAX_PEAK
 * @param int meterwidht - The widht of the meter in pixel
 * @return unsigned short - A value 0 <= return value <= meterwidth
 */
unsigned short peak_meter_scale_value(unsigned short val, int meterwidth)
{
    int retval;

    if (val <= peak_meter_range_min) {
        return 0;
    }

    if (val >= peak_meter_range_max) {
        return meterwidth;
    }

    retval = val;

    /* different scaling is used for dBfs and linear percent */
    if (pm_use_dbfs) {

        /* scale the samples dBfs */
        retval  = (calc_db(retval) - pm_db_min) * meterwidth / pm_db_range;
    }

    /* Scale for linear percent display */
    else
    {
        /* scale the samples */
        retval  = ((retval - peak_meter_range_min) * meterwidth)
                  / pm_range;
    }
    return retval;
}
void peak_meter_screen(struct screen *display, int x, int y, int height)
{
    peak_meter_draw(display, &scales[display->screen_type], x, y,
                        display->getwidth() - x, height);
}

/* sets *left and *right to the current *unscaled* values */
void peak_meter_current_vals(int *left, int *right)
{
    static int left_level = 0, right_level = 0;
    if (level_check){
        /* only read the volume info from MAS if peek since last read*/
        left_level  = peak_meter_read_l();
        right_level = peak_meter_read_r();
        level_check = false;
    }
    *left = left_level;
    *right = right_level;
}

/**
 * Draws a peak meter in the specified size at the specified position.
 * @param int x - The x coordinate.
 *                Make sure that 0 <= x and x + width < display->getwidth()
 * @param int y - The y coordinate.
 *                Make sure that 0 <= y and y + height < display->getheight()
 * @param int width - The width of the peak meter. Note that for display
 *                    of clips a 3 pixel wide area is used ->
 *                    width > 3
 * @param int height - The height of the peak meter. height > 3
 */
static void peak_meter_draw(struct screen *display, struct meter_scales *scales,
                         int x, int y, int width, int height)
{
    int left_level = 0, right_level = 0;
    int left = 0, right = 0;
    int meterwidth = width - 3;
    int i, delta;
    static long peak_release_tick = 0;

#ifdef PM_DEBUG
    static long pm_tick = 0;
    int tmp = peek_calls;
#endif

    /* if disabled only draw the peak meter */
    if (peak_meter_enabled) {


        peak_meter_current_vals(&left_level, &right_level);

        /* scale the samples dBfs */
        left  = peak_meter_scale_value(left_level, meterwidth);
        right = peak_meter_scale_value(right_level, meterwidth);

         /*if the scale has changed -> recalculate the scale
           (The scale becomes invalid when the range changed.) */
        if (!scales->db_scale_valid){

            if (pm_use_dbfs) {
                db_scale_count = DB_SCALE_SRC_VALUES_SIZE;
                for (i = 0; i < db_scale_count; i++){
                    /* find the real x-coords for predefined interesting
                       dBfs values. These only are recalculated when the
                       scaling of the meter changed. */
                        scales->db_scale_lcd_coord[i] =
                            peak_meter_scale_value(
                                db_scale_src_values[i],
                                meterwidth - 1);
                }
            }

            /* when scaling linear we simly make 10% steps */
            else {
                db_scale_count = 10;
                for (i = 0; i < db_scale_count; i++) {
                    scales->db_scale_lcd_coord[i] =
                        (i * (MAX_PEAK / 10) - peak_meter_range_min) *
                        meterwidth / pm_range;
                }
            }

            /* mark scale valid to avoid recalculating dBfs values
               of the scale. */
            scales->db_scale_valid = true;
        }

        /* apply release */
        delta = current_tick - peak_release_tick;
        peak_release_tick = current_tick;
        left  = MAX(left , scales->last_left  - delta * pm_peak_release);
        right = MAX(right, scales->last_right - delta * pm_peak_release);

        /* reset max values after timeout */
        if (TIME_AFTER(current_tick, scales->pm_peak_timeout_l)){
            scales->pm_peak_left = 0;
        }

        if (TIME_AFTER(current_tick, scales->pm_peak_timeout_r)){
            scales->pm_peak_right = 0;
        }

        if (!pm_clip_eternal) {
            if (pm_clip_left &&
                TIME_AFTER(current_tick, pm_clip_timeout_l)){
                pm_clip_left = false;
            }

            if (pm_clip_right &&
                TIME_AFTER(current_tick, pm_clip_timeout_r)){
                pm_clip_right = false;
            }
        }

        /* check for new max values */
        if (left > scales->pm_peak_left) {
            scales->pm_peak_left = left - 1;
            scales->pm_peak_timeout_l = current_tick + pm_peak_hold;
        }

        if (right > scales->pm_peak_right) {
            scales->pm_peak_right = right - 1;
            scales->pm_peak_timeout_r = current_tick + pm_peak_hold;
        }
    }

    /* draw the peak meter */
    display->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
    display->fillrect(x, y, width, height);
    display->set_drawmode(DRMODE_SOLID);

    /* draw left */
    display->fillrect (x, y, left, height / 2 - 2 );
    if (scales->pm_peak_left > 0) {
        display->vline(x + scales->pm_peak_left, y, y + height / 2 - 2 );
    }
    if (pm_clip_left) {
        display->fillrect(x + meterwidth, y, 3, height / 2 - 1);
    }

    /* draw right */
    display->fillrect(x, y + height / 2 + 1, right, height / 2 - 2);
    if (scales->pm_peak_right > 0) {
        display->vline( x + scales->pm_peak_right, y + height / 2, y + height - 2);
    }
    if (pm_clip_right) {
        display->fillrect(x + meterwidth, y + height / 2, 3, height / 2 - 1);
    }

    /* draw scale end */
    display->vline(x + meterwidth, y, y + height - 2);

    /* draw dots for scale marks */
    for (i = 0; i < db_scale_count; i++) {
        /* The x-coordinates of interesting scale mark points
           have been calculated before */
        display->drawpixel(x + scales->db_scale_lcd_coord[i],
                           y + height / 2 - 1);
    }


#ifdef PM_DEBUG
    /* display a bar to show how many calls to peak_meter_peek
       have ocurred since the last display */
    display->set_drawmode(DRMODE_COMPLEMENT);
    display->fillrect(x, y, tmp, 3);

    if (tmp < PEEKS_PER_DRAW_SIZE) {
        peeks_per_redraw[tmp]++;
    }

    tmp = current_tick - pm_tick;
    if (tmp < TICKS_PER_DRAW_SIZE ){
        ticks_per_redraw[tmp] ++;
    }

    /* display a bar to show how many ticks have passed since
       the last redraw */
    display->fillrect(x, y + height / 2, current_tick - pm_tick, 2);
    pm_tick = current_tick;
#endif

    scales->last_left = left;
    scales->last_right = right;

    display->set_drawmode(DRMODE_SOLID);
}


int peak_meter_draw_get_btn(int action_context, int x[], int y[],
                            int height[], int nb_screens,
                            struct viewport vps[])
{
    int button = BUTTON_NONE;
    long next_refresh = current_tick;
    long next_big_refresh = current_tick + HZ / 10;
    int i;
    bool highperf = false;
    bool dopeek = true;

    while (TIME_BEFORE(current_tick, next_big_refresh)) {
        button = get_action(action_context, TIMEOUT_NOBLOCK);
        if (button != BUTTON_NONE) {
            break;
        }
        if (dopeek) {          /* Peek only once per refresh when disk is */
            peak_meter_peek(); /* spinning, but as often as possible */
            dopeek = highperf; /* otherwise. */
            yield();
        } else {
            sleep(0);          /* Sleep until end of current tick. */
        }
        if (TIME_AFTER(current_tick, next_refresh)) {
            for(i = 0; i < nb_screens; i++)
            {
                screens[i].set_viewport(&vps[i]);
                peak_meter_screen(&screens[i], x[i], y[i], height[i]);
                screens[i].update_viewport_rect(x[i], y[i],
                                                screens[i].getwidth() - x[i],
                                                height[i]);
            }
            next_refresh += HZ / PEAK_METER_FPS;
            dopeek = true;
        }
    }

    return button;
}

#ifdef PM_DEBUG
static void peak_meter_clear_histogram(void)
{
    int i = 0;
    for (i = 0; i < TICKS_PER_DRAW_SIZE; i++) {
        ticks_per_redraw[i] = (unsigned int)0;
    }

    for (i = 0; i < PEEKS_PER_DRAW_SIZE; i++) {
        peeks_per_redraw[i] = (unsigned int)0;
    }
}

bool peak_meter_histogram(void)
{
    int i;
    int btn = BUTTON_NONE;
    while ((btn & BUTTON_OFF) != BUTTON_OFF )
    {
        unsigned int max = 0;
        int y = 0;
        int x = 0;
        screens[0].clear_display();

        for (i = 0; i < PEEKS_PER_DRAW_SIZE; i++) {
            max = MAX(max, peeks_per_redraw[i]);
        }

        for (i = 0; i < PEEKS_PER_DRAW_SIZE; i++) {
            x = peeks_per_redraw[i] * (LCD_WIDTH - 1)/ max;
            screens[0].hline(0, x, y + i);
        }

        y = PEEKS_PER_DRAW_SIZE + 1;
        max = 0;

        for (i = 0; i < TICKS_PER_DRAW_SIZE; i++) {
            max = MAX(max, ticks_per_redraw[i]);
        }

        for (i = 0; i < TICKS_PER_DRAW_SIZE; i++) {
            x = ticks_per_redraw[i] * (LCD_WIDTH - 1)/ max;
            screens[0].hline(0, x, y + i);
        }
        screens[0].update();

        btn = button_get(true);
        if (btn == BUTTON_PLAY) {
            peak_meter_clear_histogram();
        }
    }
    return false;
}
#endif

