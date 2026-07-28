/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.h (time formatting and sleep timer)
 * Copyright (C) 2002 by Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * Interface to format_time.c and the unit index flags it takes.
 ****************************************************************************/

#ifndef _FORMAT_TIME_H_
#define _FORMAT_TIME_H_

#include <stdbool.h>
#include <stddef.h>

/* units used with format_time_auto, and by widgets/option_select.c */
extern const unsigned char * const unit_strings_core[];

/* format_time_auto */
enum e_fmt_time_auto_idx
{
    UNIT_IDX_HR = 0,
    UNIT_IDX_MIN,
    UNIT_IDX_SEC,
    UNIT_IDX_MS,
    UNIT_IDX_TIME_COUNT,
};
#define UNIT_IDX_MASK       0x01FFU /*Return only Unit_IDX*/
#define UNIT_TRIM_ZERO      0x0200U /*Don't show leading zero on max_idx*/
#define UNIT_LOCK_HR        0x0400U /*Don't Auto Range below this field*/
#define UNIT_LOCK_MIN       0x0800U /*Don't Auto Range below this field*/
#define UNIT_LOCK_SEC       0x1000U /*Don't Auto Range below this field*/

/*  time_split_units()
    split time values depending on base unit
    unit_idx: UNIT_HOUR, UNIT_MIN, UNIT_SEC, UNIT_MS
    abs_value: absolute time value
    units_in: array of unsigned ints with IDX_TIME_COUNT fields
*/
unsigned int time_split_units(int unit_idx, unsigned long abs_val,
                        unsigned long (*units_in)[UNIT_IDX_TIME_COUNT]);

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
                                  int unit_idx, bool supress_unit);

/* Format time into buf.
 *
 * buf      - buffer to format to.
 * buf_size - size of buffer.
 * t        - time to format, in milliseconds.
 */
void format_time(char* buf, int buf_size, long t);

const char* format_sleeptimer(char* buffer, size_t buffer_size,
                              int value, const char* unit);

/* A string representation of either whether a sleep timer will be started or
   canceled, and how long it will be or how long is remaining in brackets */
char* string_sleeptimer(char *buffer, size_t buffer_len);
int toggle_sleeptimer(void);
void talk_sleeptimer(int custom_duration);

void talk_timedate(void);


#endif /* _FORMAT_TIME_H_ */
