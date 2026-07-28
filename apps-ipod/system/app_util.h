/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.h (cross-cutting UI helpers)
 * Copyright (C) 2002 by Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * Interface to app_util.c.
 ****************************************************************************/

#ifndef _APP_UTIL_H_
#define _APP_UTIL_H_

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>
#include "config.h"
#include "draw/screen_access.h"

extern const unsigned char * const byte_units[];
extern const unsigned char * const * const kibyte_units;
/* Format a large-range value for output, using the appropriate unit so that
 * the displayed value is in the range 1 <= display < 1000 (1024 for "binary"
 * units) if possible, and 3 significant digits are shown. If a buffer is
 * given, the result is snprintf()'d into that buffer, otherwise the result is
 * voiced.*/
char *output_dyn_value(char *buf,
                       int buf_size,
                       int64_t value,
                       const unsigned char * const *units,
                       unsigned int unit_count,
                       bool binary_scale);

/* Ask the user if they really want to erase the current dynamic playlist
 * returns true if the playlist should be replaced */
bool warn_on_pl_erase(void);

bool show_search_progress(bool init, int count, int current, int total);

/* Note: Don't rely on title being visible. It is not
         displayed on Android, or if SBS has no title. */
int confirm_delete_yesno(const char *name, const char *title);

/* only used in USB HID and set_time screen */
int clamp_value_wrap(int value, int max, int min);



enum core_load_bmp_error
{
    CLB_ALOC_ERR = 0,
    CLB_READ_ERR = -1,
};
struct buflib_callbacks;
int core_load_bmp(const char *filename, struct bitmap *bm, const int bmformat,
                  ssize_t *buf_reqd, struct buflib_callbacks *ops);

/* clear the lcd output buffer, if update is true the cleared buffer
 * will be written to the lcd */
void clear_screen_buffer(bool update);


/* was: apps/screens.h */
/* Briefly show the "battery charge" splash and drain the button queue. */
void charging_splash(void);

#endif /* _APP_UTIL_H_ */
