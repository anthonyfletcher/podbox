/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.c (time formatting and sleep timer)
 * Copyright (C) 2002 by Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * Formats durations and clock values for display and speech, including the
 * auto-ranging formatter and the sleep timer's text.
 ****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lang.h"
#include "timefuncs.h"
#include "powermgmt.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "speech/language.h"
#include "format_time.h"

/*  time_split_units()
    split time values depending on base unit
    unit_idx: UNIT_HOUR, UNIT_MIN, UNIT_SEC, UNIT_MS
    abs_value: absolute time value
    units_in: array of unsigned ints with UNIT_IDX_TIME_COUNT fields
*/
unsigned int time_split_units(int unit_idx, unsigned long abs_val,
                              unsigned long (*units_in)[UNIT_IDX_TIME_COUNT])
{
    unsigned int base_idx = UNIT_IDX_HR;
    unsigned long hours;
    unsigned long minutes  = 0;
    unsigned long seconds  = 0;
    unsigned long millisec = 0;

    switch (unit_idx & UNIT_IDX_MASK) /*Mask off upper bits*/
    {
            case UNIT_MS:
                base_idx = UNIT_IDX_MS;
                millisec = abs_val;
                abs_val  = abs_val  /  1000U;
                millisec = millisec - (1000U * abs_val);
                /* fallthrough and calculate the rest of the units */
            case UNIT_SEC:
                if (base_idx == UNIT_IDX_HR)
                    base_idx = UNIT_IDX_SEC;
                seconds  = abs_val;
                abs_val  = abs_val  / 60U;
                seconds  = seconds - (60U * abs_val);
                /* fallthrough and calculate the rest of the units */
            case UNIT_MIN:
                if (base_idx == UNIT_IDX_HR)
                    base_idx = UNIT_IDX_MIN;
                minutes  = abs_val;
                abs_val  = abs_val / 60U;
                minutes  = minutes -(60U * abs_val);
                /* fallthrough and calculate the rest of the units */
            case UNIT_HOUR:
            default:
                hours    = abs_val;
                break;
    }

    (*units_in)[UNIT_IDX_HR]  = hours;
    (*units_in)[UNIT_IDX_MIN] = minutes;
    (*units_in)[UNIT_IDX_SEC] = seconds;
    (*units_in)[UNIT_IDX_MS]  = millisec;

    return base_idx;
}

/* format_time_auto - return an auto ranged time string;
   buffer:  needs to be at least 25 characters for full range

   unit_idx: specifies lowest or base index of the value
   add | UNIT_LOCK_ to keep place holder of units that would normally be
   discarded.. For instance, UNIT_LOCK_HR would keep the hours place, ex: string
   00:10:10 (0 HRS 10 MINS 10 SECONDS) normally it would return as 10:10
   add | UNIT_TRIM_ZERO to supress leading zero on the largest unit

   value: should be passed in the same form as unit_idx

   supress_unit: may be set to true and in this case the
   hr, min, sec, ms identifiers will be left off the resulting string but
   since right to left languages are handled it is advisable to leave units
   as an indication of the text direction
*/

const char *format_time_auto(char *buffer, int buf_len, long value,
                                  int unit_idx, bool supress_unit)
{
    const char * const sign        = &"-"[value < 0 ? 0 : 1];
    bool               is_rtl      = lang_is_rtl();
    char               timebuf[25]; /* -2147483648:00:00.00\0 */
    int                len, left_offset;
    unsigned char      base_idx, max_idx;

    unsigned long  units_in[UNIT_IDX_TIME_COUNT];
    unsigned char  fwidth[UNIT_IDX_TIME_COUNT] =
                   {
                        [UNIT_IDX_HR]  = 0, /* hr is variable length */
                        [UNIT_IDX_MIN] = 2,
                        [UNIT_IDX_SEC] = 2,
                        [UNIT_IDX_MS]  = 3,
                   }; /* {0,2,2,3}; Field Widths */
    unsigned char  offsets[UNIT_IDX_TIME_COUNT] =
                   {
                        [UNIT_IDX_HR]  = 10,/* ?:59:59.999 Std offsets    */
                        [UNIT_IDX_MIN] = 7, /*0?:+1:+4.+7 need calculated */
                        [UNIT_IDX_SEC] = 4,/* 999.59:59:0  RTL offsets    */
                        [UNIT_IDX_MS]  = 0,/* 0  .4 :7 :10 won't change   */
                   }; /* {10,7,4,0}; Offsets */
    static const uint16_t unitlock[UNIT_IDX_TIME_COUNT] =
                   {
                        [UNIT_IDX_HR]  = UNIT_LOCK_HR,
                        [UNIT_IDX_MIN] = UNIT_LOCK_MIN,
                        [UNIT_IDX_SEC] = UNIT_LOCK_SEC,
                        [UNIT_IDX_MS]  = 0,
                   }; /* unitlock */
    static const uint16_t units[UNIT_IDX_TIME_COUNT] =
                   {
                        [UNIT_IDX_HR]  = UNIT_HOUR,
                        [UNIT_IDX_MIN] = UNIT_MIN,
                        [UNIT_IDX_SEC] = UNIT_SEC,
                        [UNIT_IDX_MS]  = UNIT_MS,
                   }; /* units */


    base_idx = time_split_units(unit_idx, labs(value), &units_in);

    if (units_in[UNIT_IDX_HR] || (unit_idx & unitlock[UNIT_IDX_HR]))
        max_idx = UNIT_IDX_HR;
    else if (units_in[UNIT_IDX_MIN] || (unit_idx & unitlock[UNIT_IDX_MIN]))
        max_idx = UNIT_IDX_MIN;
    else if (units_in[UNIT_IDX_SEC] || (unit_idx & unitlock[UNIT_IDX_SEC]))
        max_idx = UNIT_IDX_SEC;
    else if (units_in[UNIT_IDX_MS])
        max_idx = UNIT_IDX_MS;
    else /* value is 0 */
        max_idx = base_idx;

    if (!is_rtl)
    {
        len = snprintf(timebuf, sizeof(timebuf),
                       "%02lu:%02lu:%02lu.%03lu",
                       units_in[UNIT_IDX_HR],
                       units_in[UNIT_IDX_MIN],
                       units_in[UNIT_IDX_SEC],
                       units_in[UNIT_IDX_MS]);

        fwidth[UNIT_IDX_HR]   = len - offsets[UNIT_IDX_HR];

        /* calculate offsets of the other fields based on length of previous */
        offsets[UNIT_IDX_MS]  = fwidth[UNIT_IDX_HR] + offsets[UNIT_IDX_MIN];
        offsets[UNIT_IDX_SEC] = fwidth[UNIT_IDX_HR] + offsets[UNIT_IDX_SEC];
        offsets[UNIT_IDX_MIN] = fwidth[UNIT_IDX_HR] + 1;
        offsets[UNIT_IDX_HR]  = 0;

        timebuf[offsets[base_idx] + fwidth[base_idx]] = '\0';

        left_offset  = -(offsets[max_idx]);
        left_offset += strlcpy(buffer, sign, buf_len);

        /* trim leading zero on the max_idx */
        if ((unit_idx & UNIT_TRIM_ZERO) == UNIT_TRIM_ZERO &&
            timebuf[offsets[max_idx]] == '0' && fwidth[max_idx] > 1)
        {
            offsets[max_idx]++;
        }

        strlcat(buffer, &timebuf[offsets[max_idx]], buf_len);

        if (!supress_unit)
        {
            strlcat(buffer, " ", buf_len);
            strlcat(buffer, unit_strings_core[units[max_idx]], buf_len);
        }
    }
    else /*RTL Languages*/
    {
        len = snprintf(timebuf, sizeof(timebuf),
                       "%03lu.%02lu:%02lu:%02lu",
                       units_in[UNIT_IDX_MS],
                       units_in[UNIT_IDX_SEC],
                       units_in[UNIT_IDX_MIN],
                       units_in[UNIT_IDX_HR]);

        fwidth[UNIT_IDX_HR] = len - offsets[UNIT_IDX_HR];

        left_offset = -(offsets[base_idx]);

        /* trim leading zero on the max_idx */
        if ((unit_idx & UNIT_TRIM_ZERO) == UNIT_TRIM_ZERO &&
            timebuf[offsets[max_idx]] == '0' && fwidth[max_idx] > 1)
        {
            timebuf[offsets[max_idx]] = timebuf[offsets[max_idx]+1];
            fwidth[max_idx]--;
        }

        timebuf[offsets[max_idx] + fwidth[max_idx]] = '\0';

        if (!supress_unit)
        {
            strmemccpy(buffer, unit_strings_core[units[max_idx]], buf_len);
            left_offset += strlcat(buffer, " ", buf_len);
            strlcat(buffer, &timebuf[offsets[base_idx]], buf_len);
        }
        else
            strmemccpy(buffer, &timebuf[offsets[base_idx]], buf_len);

        strlcat(buffer, sign, buf_len);
    }

    return buffer;
}

/* Format time into buf.
 *
 * buf      - buffer to format to.
 * buf_size - size of buffer.
 * t        - time to format, in milliseconds.
 */
void format_time(char* buf, int buf_size, long t)
{
    unsigned long units_in[UNIT_IDX_TIME_COUNT] = {0};
    time_split_units(UNIT_MS, labs(t), &units_in);
    int hashours = units_in[UNIT_IDX_HR] > 0;
    snprintf(buf, buf_size, "%.*s%.0lu%.*s%.*lu:%.2lu",
             t < 0, "-", units_in[UNIT_IDX_HR], hashours, ":",
             hashours+1, units_in[UNIT_IDX_MIN], units_in[UNIT_IDX_SEC]);
}

const char* format_sleeptimer(char* buffer, size_t buffer_size,
                              int value, const char* unit)
{
    (void) unit;
    int minutes, hours;

    if (value) {
        hours = value / 60;
        minutes = value - (hours * 60);
        snprintf(buffer, buffer_size, "%d:%02d", hours, minutes);
        return buffer;
    } else {
        return str(LANG_OFF);
    }
}

static int seconds_to_min(int secs)
{
    return (secs + 10) / 60;  /* round up for 50+ seconds */
}

char* string_sleeptimer(char *buffer, size_t buffer_len)
{
    int sec = get_sleep_timer();
    char timer_buf[10];

    snprintf(buffer, buffer_len, "%s (%s)",
             str(sec ? LANG_SLEEP_TIMER_CANCEL_CURRENT
                 : LANG_SLEEP_TIMER_START_CURRENT),
             format_sleeptimer(timer_buf, sizeof(timer_buf),
                sec ? seconds_to_min(sec)
                    : global_settings.sleeptimer_duration, NULL));
    return buffer;
}

/* If a sleep timer is running, cancel it, otherwise start one */
int toggle_sleeptimer(void)
{
    set_sleeptimer_duration(get_sleep_timer() ? 0
                    : global_settings.sleeptimer_duration);
    return 0;
}

void talk_sleeptimer(int custom_duration)
{
    int seconds = custom_duration < 0 ? get_sleep_timer() : custom_duration*60;
    long talk_ids[] = {
        custom_duration >= 0 ? LANG_SLEEP_TIMER :
        (seconds ? LANG_SLEEP_TIMER_CANCEL_CURRENT : LANG_SLEEP_TIMER_START_CURRENT),
        VOICE_PAUSE,
        custom_duration == 0 ? LANG_OFF :
        (seconds ? seconds_to_min(seconds)
            : global_settings.sleeptimer_duration) | UNIT_MIN << UNIT_SHIFT,
        TALK_FINAL_ID
    };
    talk_idarray(talk_ids, true);
}

void talk_timedate(void)
{
    struct tm *tm = get_time();
    if (!global_settings.talk_menu)
        return;
    talk_id(LANG_CURRENT_TIME, false);
    if (valid_time(tm))
    {
        talk_time(tm, true);
        talk_date(get_time(), true);
    }
    else
    {
        talk_id(LANG_UNKNOWN, true);
    }
}


/* units used with format_time_auto, option_select.c->option_get_valuestring() */
const unsigned char * const unit_strings_core[] =
{
    [UNIT_INT] = "",    [UNIT_MS]  = "ms",
    [UNIT_SEC] = "s",   [UNIT_MIN] = "min",
    [UNIT_HOUR]= "hr",  [UNIT_KHZ] = "kHz",
    [UNIT_DB]  = "dB",  [UNIT_PERCENT] = "%",
    [UNIT_MAH] = "mAh", [UNIT_PIXEL] = "px",
    [UNIT_PER_SEC] = "per sec",
    [UNIT_HERTZ] = "Hz",
    [UNIT_MB]  = "MB",  [UNIT_KBIT]  = "kb/s",
    [UNIT_PM_TICK] = "units/10ms",
};

