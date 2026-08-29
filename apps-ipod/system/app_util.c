/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.c (cross-cutting UI helpers) + apps/screens.c (charging_splash)
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * Cross-cutting UI helpers that belong to no single screen: colour
 * parsing, bitmap loading into buflib, keyclick and system sounds, dynamic
 * value formatting, and small confirmation prompts.
 ****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "lcd.h"
#include "lang.h"
#include "file.h"
#include "sound.h"
#include "core_alloc.h"
#include "debug.h"
#include "piezo.h"
#include "buflib.h"
#include "input/action.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"
#include "widgets/list.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "draw/bmp.h"
#include "playlist/playlist.h"
#include "metadata/book_resume.h"
#include "app_util.h"

/* units used with output_dyn_value */
const unsigned char * const byte_units[] =
{
    ID2P(LANG_BYTE),
    ID2P(LANG_KIBIBYTE),
    ID2P(LANG_MEBIBYTE),
    ID2P(LANG_GIBIBYTE)
};

const unsigned char * const * const kibyte_units = &byte_units[1];


char *output_dyn_value(char *buf,
                       int buf_size,
                       int64_t value,
                       const unsigned char * const *units,
                       unsigned int unit_count,
                       bool binary_scale)
{
    unsigned int scale = binary_scale ? 1024 : 1000;
    unsigned int fraction = 0;
    unsigned int unit_no = 0;
    uint64_t value_abs = (value < 0) ? -value : value;
    char tbuf[5];
    int value2;

    while (value_abs >= scale && unit_no < (unit_count - 1))
    {
        fraction = value_abs % scale;
        value_abs /= scale;
        unit_no++;
    }

    value2 = (value < 0) ? -value_abs : value_abs; /* preserve sign */
    fraction = (fraction * 1000 / scale) / 10;

    if (value_abs >= 100 || fraction >= 100 || !unit_no)
        tbuf[0] = '\0';
    else if (value_abs >= 10)
        snprintf(tbuf, sizeof(tbuf), "%01u", fraction / 10);
    else
        snprintf(tbuf, sizeof(tbuf), "%02u", fraction);

    if (buf)
    {
        if (*tbuf)
            snprintf(buf, buf_size, "%d%s%s%s", value2, str(LANG_POINT),
                     tbuf, P2STR(units[unit_no]));
        else
            snprintf(buf, buf_size, "%d%s", value2, P2STR(units[unit_no]));
    }
    else
    {
        talk_fractional(tbuf, value2, P2ID(units[unit_no]));
    }
    return buf;
}

bool warn_on_pl_erase(void)
{
    /* Every path that replaces the current playlist asks here first, which
     * makes this the one place a book being listened to is left behind
     * without a pause or a stop to record it. Saving is silent unless a book
     * really is playing, and a cancelled erase saves a position that is still
     * the right one. */
    book_resume_save();

    if (global_status.resume_index != -1 &&
        global_settings.warnon_erase_dynplaylist &&
        !global_settings.party_mode &&
        playlist_modified(NULL))
    {
        static const char *lines[] =
            {ID2P(LANG_WARN_ERASEDYNPLAYLIST_PROMPT)};
        static const struct text_message message={lines, 1};

        if (gui_syncyesno_run(&message, NULL, NULL) == YESNO_YES)
            return true;
        else
        {
            splash(HZ, ID2P(LANG_CANCEL));
            return false;
        }
    }
    else
        return true;
}

bool show_search_progress(bool init, int display_count, int current, int total)
{
    static long last_tick, talked_tick;

    /* Don't show splashes for 1/2 second after starting search */
    if (init)
    {
        last_tick = current_tick + HZ/2;
        talked_tick = 0;
        return true;
    }

    /* Update progress every 1/10 of a second */
    if (TIME_AFTER(current_tick, last_tick + HZ/10))
    {
        if (total != current)
            /* (voiced) */
            splash_progress(current, total, str(LANG_PLAYLIST_SEARCH_MSG),
                            display_count, str(LANG_OFF_ABORT));
        else
        {
            if (global_settings.talk_menu &&
                TIME_AFTER(current_tick, talked_tick + (HZ * 5)))
            {
                talked_tick = current_tick;
                talk_number(display_count, false);
                talk_id(LANG_PLAYLIST_SEARCH_MSG, true);
            }
            /* (voiced above) */
            splashf(0, str(LANG_PLAYLIST_SEARCH_MSG),
                    display_count, str(LANG_OFF_ABORT));
        }

        if (action_userabort(TIMEOUT_NOBLOCK))
            return false;
        last_tick = current_tick;
        yield();
    }

    return true;
}

/* Play a standard sound */

int confirm_delete_yesno(const char *name, const char *title)
{
    const char *lines[] = { ID2P(LANG_REALLY_DELETE), name };
    const char *yes_lines[] = { ID2P(LANG_DELETING), name };
    const struct text_message message = { lines, 2 };
    const struct text_message yes_message = { yes_lines, 2 };
    return gui_syncyesno_run_w_title(title, &message, &yes_message, NULL);
}


/* only used in USB HID and set_time screen */
int clamp_value_wrap(int value, int max, int min)
{
    if (value > max)
        return min;
    if (value < min)
        return max;
    return value;
}

/* core_load_bmp opens bitmp filename and allocates space for it
*  you must set bm->data with the result from core_get_data(handle)
*  you must also call core_free(handle) when finished with the bitmap
*  returns handle, ALOC_ERR(0) on failure
* ** Extended error info truth table **
*  [ Handle ][buf_reqd]
*  [  > 0   ][  > 0   ] buf_reqd indicates how many bytes were used
*  [ALOC_ERR][  > 0   ] buf_reqd indicates how many bytes are needed
*  [ALOC_ERR][READ_ERR] there was an error reading the file or it is empty
*/
int core_load_bmp(const char * filename, struct bitmap *bm, const int bmformat,
                  ssize_t *buf_reqd, struct buflib_callbacks *ops)
{
    ssize_t buf_size;
    ssize_t size_read = 0;
    int handle = CLB_ALOC_ERR;

    int fd = open(filename, O_RDONLY);
    *buf_reqd = CLB_READ_ERR;

    if (fd < 0) /* Exit if file opening failed */
    {
        DEBUGF("read_bmp_file: can't open '%s', rc: %d\n", filename, fd);
        return CLB_ALOC_ERR;
    }

    buf_size = read_bmp_fd(fd, bm, 0, bmformat|FORMAT_RETURN_SIZE, NULL);

    if (buf_size > 0)
    {
        handle = core_alloc_ex(buf_size, ops);
        if (handle > 0)
        {
            bm->data = core_get_data_pinned(handle);
            lseek(fd, 0, SEEK_SET); /* reset to beginning of file */
            size_read = read_bmp_fd(fd, bm, buf_size, bmformat, NULL);

            if (size_read > 0) /* free unused alpha channel, if any */
            {
                core_shrink(handle, bm->data, size_read);
                *buf_reqd = size_read;
            }

            core_put_data_pinned(bm->data);
            bm->data = NULL; /* do this to force a crash later if the
                                    caller doesnt call core_get_data() */
        }
        else
            *buf_reqd = buf_size; /* couldn't allocate pass bytes needed */

        if (size_read <= 0)
        {
            /* error reading file */
            core_free(handle); /* core_free() ignores free handles (<= 0) */
            handle = CLB_ALOC_ERR;
        }
    }

    close(fd);
    return handle;
}

void clear_screen_buffer(bool update)
{
    struct viewport vp;
    struct viewport *last_vp;
    FOR_NB_SCREENS(i)
    {
        struct screen * screen = &screens[i];
        viewport_set_defaults(&vp, screen->screen_type);
        last_vp = screen->set_viewport(&vp);
        screen->clear_viewport();
        if (update) {
            screen->update_viewport();
        }
        screen->set_viewport(last_vp);
    }
}


void charging_splash(void)
{
    splash(2*HZ, str(LANG_BATTERY_CHARGE));
    button_clear_queue();
}
