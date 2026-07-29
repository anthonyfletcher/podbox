/***************************************************************************
 * Original code from RockBox
 * was: apps/screens.c (track info screen)
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * The Track Info screen: lists an mp3entry's tags, speaks them on request,
 * and opens any single field full-screen via the text box.
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string-extra.h>
#include <ctype.h>
#include "config.h"
#include "lang.h"
#include "timefuncs.h"
#include "metadata.h"
#include "kernel.h"
#include "usb.h"
#include "audio.h"
#include "sound.h"
#include "replaygain.h"
#include "input/action.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "widgets/list.h"
#include "playlist/playlist.h"
#include "system/activity.h"
#include "system/app_util.h"
#include "system/format_time.h"
#include "system/shutdown.h"
#include "track_info.h"

static const int id3_headers[]=
{
    LANG_TAGNAVI_ALL_TRACKS,
    LANG_ID3_TITLE,
    LANG_ID3_ARTIST,
    LANG_ID3_COMPOSER,
    LANG_ID3_ALBUM,
    LANG_ID3_ALBUMARTIST,
    LANG_ID3_GROUPING,
    LANG_ID3_DISCNUM,
    LANG_ID3_TRACKNUM,
    LANG_ID3_COMMENT,
    LANG_ID3_GENRE,
    LANG_ID3_YEAR,
    LANG_ID3_LENGTH,
    LANG_ID3_PLAYLIST,
    LANG_FORMAT,
    LANG_ID3_BITRATE,
    LANG_ID3_FREQUENCY,
    LANG_ID3_TRACK_GAIN,
    LANG_ALBUM_GAIN,
    LANG_FILESIZE,
    LANG_ID3_PATH,
    LANG_DATE,
    LANG_TIME,
};

struct id3view_info {
    struct mp3entry* id3;
    struct tm *modified;
    int track_ct;
    int count;
    struct playlist_info *playlist;
    int playlist_display_index;
    int playlist_amount;
    int info_id[ARRAYLEN(id3_headers)];
};

/* Spell out a buffer, but when successive digits are encountered, say
   the whole number. Useful for some ID3 tags that usually contain a
   number but are in fact free-form. */
static void say_number_and_spell(char *buf, bool year_style)
{
    char *ptr = buf;
    while(*ptr) {
        if(isdigit(*ptr)) {
            /* parse the number */
            int n = atoi(ptr);
            /* skip over digits to rest of string */
            while(isdigit(*++ptr));
            /* say the number */
            if(year_style)
                talk_value(n, UNIT_DATEYEAR, true);
            else talk_number(n, true);
        }else{
            /* Spell a sequence of non-digits */
            char tmp, *start = ptr;
            while(*++ptr && !isdigit(*ptr));
            /* temporarily truncate the string here */
            tmp = *ptr;
            *ptr = '\0';
            talk_spell(start, true);
            *ptr = tmp; /* restore string */
        }
    }
}

/* Say a replaygain ID3 value from its text form */
static void say_gain(char *buf)
{
    /* Expected form is "-5.74 dB". We'll try to parse out the number
       until the dot, say it (forcing the + sign), then say dot and
       spell the following numbers, and then say the decibel unit. */
    char *ptr = buf;
    if(*ptr == '-' || *ptr == '+')
        /* skip sign */
        ++ptr;
    /* See if we can parse out a number. */
    if(isdigit(*ptr)) {
        char tmp;
        /* skip successive digits */
        while(isdigit(*++ptr));
        /* temporarily truncate the string here */
        tmp = *ptr;
        *ptr = '\0';
        /* parse out the number we just skipped */
        talk_value(atoi(buf), UNIT_SIGNED, true); /* say the number with sign */
        *ptr = tmp; /* restore the string */
        if(*ptr == '.') {
            /* found the dot, get fractional part */
            buf = ptr;
            while (isdigit(*++ptr));
            while (*--ptr == '0');
            if (ptr > buf) {
                tmp = *++ptr;
                *ptr = '\0';
                talk_id(LANG_POINT, true);
                while (*++buf == '0')
                    talk_id(VOICE_ZERO, true);
                talk_number(atoi(buf), true);
                *ptr = tmp;
            }
            ptr = buf;
            while (isdigit(*++ptr));
        }
        buf = ptr;
        if(strlen(buf) >2 && !strcmp(buf+strlen(buf)-2, "dB")) {
            /* String does end with "dB" */
            /* point to that "dB" */
            ptr = buf+strlen(buf)-2;
            /* backup any spaces */
            while (ptr >buf && ptr[-1] == ' ')
                --ptr;
            if (ptr > buf)
                talk_spell(buf, true);
            else talk_id(VOICE_DB, true); /* say the dB unit */
        }else /* doesn't end with dB, just spell everything after the
                 number of dot. */
            talk_spell(buf, true);
    }else /* we didn't find a number, just spell everything */
        talk_spell(buf, true);
}

static const char * id3_get_or_speak_info(int selected_item, void* data,
                                          char *buffer, size_t buffer_len,
                                          bool say_it)
{
    struct id3view_info *info = (struct id3view_info*)data;
    struct mp3entry* id3 =info->id3;
    const unsigned char * const *unit;
    unsigned int unit_ct;
    unsigned long length;
    bool pl_modified;
    struct tm *tm = info->modified;
    int info_no=selected_item/2;
    if(!(selected_item%2))
    {/* header */
        if(say_it)
            talk_id(id3_headers[info->info_id[info_no]], false);
        snprintf(buffer, buffer_len, "%s",
                 str(id3_headers[info->info_id[info_no]]));
        return buffer;
    }
    else
    {/* data */

        char * val=NULL;
        switch(id3_headers[info->info_id[info_no]])
        {
            case LANG_TAGNAVI_ALL_TRACKS:
                if (info->track_ct <= 1)
                    return NULL;
                itoa_buf(buffer, buffer_len, info->track_ct);
                val = buffer;
                if(say_it)
                    talk_number(info->track_ct, true);
                break;
            case LANG_ID3_TITLE:
                val=id3->title;
                if(say_it && val)
                    talk_spell(val, true);
                break;
            case LANG_ID3_ARTIST:
                val=id3->artist;
                if(say_it && val)
                    talk_spell(val, true);
                break;
            case LANG_ID3_ALBUM:
                val=id3->album;
                if(say_it && val)
                    talk_spell(val, true);
                break;
            case LANG_ID3_ALBUMARTIST:
                val=id3->albumartist;
                if(say_it && val)
                    talk_spell(val, true);
                break;
            case LANG_ID3_GROUPING:
                val=id3->grouping;
                if(say_it && val)
                    talk_spell(val, true);
                break;
            case LANG_ID3_DISCNUM:
                if (id3->disc_string)
                {
                    val = id3->disc_string;
                    if(say_it)
                        say_number_and_spell(val, true);
                }
                else if (id3->discnum)
                {
                    itoa_buf(buffer, buffer_len, id3->discnum);
                    val = buffer;
                    if(say_it)
                        talk_number(id3->discnum, true);
                }
                break;
            case LANG_ID3_TRACKNUM:
                if (id3->track_string)
                {
                    val = id3->track_string;
                    if(say_it)
                        say_number_and_spell(val, true);
                }
                else if (id3->tracknum >= 0)
                {
                    itoa_buf(buffer, buffer_len, id3->tracknum);
                    val = buffer;
                    if(say_it)
                        talk_number(id3->tracknum, true);
                }
                break;
            case LANG_ID3_COMMENT:
                if (!id3->comment)
                    return NULL;


                val = id3->comment;
                if(say_it && val)
                    talk_spell(val, true);
                break;
            case LANG_ID3_GENRE:
                val = id3->genre_string;
                if(say_it && val)
                    talk_spell(val, true);
                break;
            case LANG_ID3_YEAR: {
                /* the voiced date format picks the style: 'y' reads 2020 as
                 * "two thousand twenty", 'Y' as "twenty twenty" */
                const char *format = str(LANG_VOICED_DATE_FORMAT);
                bool numericyear = strrchr(format, 'y');
                if (id3->year_string)
                {
                    val = id3->year_string;
                    if(say_it && val)
                        say_number_and_spell(val, !numericyear);
                }
                else if (id3->year)
                {
                    itoa_buf(buffer, buffer_len, id3->year);
                    val = buffer;
                    if(say_it) {
                        if (numericyear)
                            talk_number(id3->year, true);
                        else
                            talk_value(id3->year, UNIT_DATEYEAR, true);
                    }
                }
                break;
            }
            case LANG_ID3_LENGTH:
                length = info->track_ct > 1 ? id3->length : id3->length / 1000;

                format_time_auto(buffer, buffer_len,
                                 length, UNIT_SEC | UNIT_TRIM_ZERO, true);
                val=buffer;
                if(say_it)
                    talk_value(length, UNIT_TIME, true);
                break;
            case LANG_ID3_PLAYLIST:
                if (info->playlist_display_index == 0 || info->playlist_amount == 0 )
                    return NULL;

                pl_modified = playlist_modified(info->playlist);

                snprintf(buffer, buffer_len, "%d/%d%s",
                         info->playlist_display_index, info->playlist_amount,
                         pl_modified ? "* " :"  ");
                val = buffer;
                size_t prefix_len = strlen(buffer);
                buffer += prefix_len;
                buffer_len -= prefix_len;

                if (info->playlist)
                     playlist_name(info->playlist, buffer, buffer_len);
                else
                {
                    if (playlist_allow_dirplay(NULL))
                        strmemccpy(buffer, "(Folder)", buffer_len);
                    else if (playlist_dynamic_only())
                        strmemccpy(buffer, "(Dynamic)", buffer_len);
                    else
                        playlist_name(NULL, buffer, buffer_len);
                }

                if(say_it)
                {
                    talk_number(info->playlist_display_index, true);
                    talk_id(VOICE_OF, true);
                    talk_number(info->playlist_amount, true);

                    if (pl_modified)
                        talk_spell("Modified", true);
                    if (buffer) /* playlist name */
                        talk_spell(buffer, true);
                }
                break;
            case LANG_FORMAT:
                if (id3->codectype == AFMT_UNKNOWN && info->track_ct > 1)
                    return NULL;

                val = (char*) get_codec_string(id3->codectype);
                if(say_it)
                    talk_spell(val, true);
                break;
            case LANG_ID3_BITRATE:
                if (!id3->bitrate)
                    return NULL;
                snprintf(buffer, buffer_len, "%d kbps%s%s", id3->bitrate,
            id3->vbr ? " " : "",
            id3->vbr ? str(LANG_ID3_VBR) : (const unsigned char*) "");
                val=buffer;
                if(say_it)
                {
                    talk_value(id3->bitrate, UNIT_KBIT, true);
                    if(id3->vbr)
                        talk_id(LANG_ID3_VBR, true);
                }
                break;
            case LANG_ID3_FREQUENCY:
                if (!id3->frequency)
                    return NULL;
                snprintf(buffer, buffer_len, "%ld Hz", id3->frequency);
                val=buffer;
                if(say_it)
                    talk_value(id3->frequency, UNIT_HERTZ, true);
                break;
            case LANG_ID3_TRACK_GAIN:
                replaygain_itoa(buffer, buffer_len, id3->track_level);
                val=(id3->track_level) ? buffer : NULL; /* only show level!=0 */
                if(say_it && val)
                    say_gain(val);
                break;
            case LANG_ALBUM_GAIN:
                replaygain_itoa(buffer, buffer_len, id3->album_level);
                val=(id3->album_level) ? buffer : NULL; /* only show level!=0 */
                if(say_it && val)
                    say_gain(val);
                break;
            case LANG_ID3_PATH:
                val=id3->path;
                if(say_it && val)
                    talk_fullpath(val, true);
                break;
            case LANG_ID3_COMPOSER:
                val=id3->composer;
                if(say_it && val)
                    talk_spell(val, true);
                break;
            case LANG_FILESIZE: /* not LANG_ID3_FILESIZE because the string is shared */
                if (!id3->filesize)
                    return NULL;
                if (info->track_ct > 1)
                {
                    unit = kibyte_units;
                    unit_ct = 3;
                }
                else
                {
                    unit = byte_units;
                    unit_ct = 4;
                }
                output_dyn_value(buffer, buffer_len, id3->filesize, unit, unit_ct, true);
                val=buffer;
                if(say_it && val)
                    output_dyn_value(NULL, 0, id3->filesize, unit, unit_ct, true);
                break;
            case LANG_DATE:
                if (!tm)
                    return NULL;

                snprintf(buffer, buffer_len, "%04d/%02d/%02d",
                         tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);

                val = buffer;
                if (say_it)
                    talk_date(tm, true);
                break;
            case LANG_TIME:
                if (!tm)
                    return NULL;

                snprintf(buffer, buffer_len, "%02d:%02d:%02d",
                         tm->tm_hour, tm->tm_min, tm->tm_sec);

                val = buffer;
                if (say_it)
                    talk_time(tm, true);
                break;
        }
        if((!val || !*val) && say_it)
            talk_id(LANG_ID3_NO_INFO, true);
        return val && *val ? val : NULL;
    }
}

/* gui_synclist callback -- one row per field, "Label: Value". The odd index
 * selects id3_get_or_speak_info()'s value branch; the header is prepended. */
static const char* id3_get_name_cb(int selected_item, void* data,
                                   char *buffer, size_t buffer_len)
{
    struct id3view_info *info = (struct id3view_info*)data;
    char value[MAX_PATH];
    const char *val = id3_get_or_speak_info(selected_item * 2 + 1, data,
                                            value, sizeof(value), false);
    snprintf(buffer, buffer_len, "%s: %s",
             str(id3_headers[info->info_id[selected_item]]), val ? val : "");
    return buffer;
}

static int id3_speak_item(int selected_item, void* data)
{
    char buffer[MAX_PATH];
    /* say field name (even index) then value (odd index) for this row */
    id3_get_or_speak_info(selected_item * 2, data, buffer, MAX_PATH, true);
    id3_get_or_speak_info(selected_item * 2 + 1, data, buffer, MAX_PATH, true);
    return 0;
}

/* Note: If track_ct > 1, filesize value will be treated as
 * KiB (instead of Bytes), and length as s instead of ms.
 */
bool browse_id3_ex(struct mp3entry *id3, struct playlist_info *playlist,
                int playlist_display_index, int playlist_amount,
                struct tm *modified, int track_ct,
                int (*view_text)(const char *title, const char *text))
{
    struct gui_synclist id3_lists;
    int key;
    unsigned int i;
    struct id3view_info info;
    info.id3 = id3;
    info.modified = modified;
    info.track_ct = track_ct;
    info.playlist = playlist;
    info.playlist_amount = playlist_amount;
    bool ret = false;
    /* "Live" mode -- exit when playback stops and re-read on track change -- only
     * when we're actually showing the currently playing track's id3. The live
     * callers (wps.c, context_menu_show.c) pass audio_current_track(); static viewers
     * (properties, playlist viewer) pass their own id3. Deciding by identity is
     * robust; the old activity check treated any non-plugin caller as live,
     * which flashed-and-closed the core Properties -> Track Info view. */
    bool is_curr_track_info = (id3 != NULL && id3 == audio_current_track());
    /* Push unconditionally: the activity is what a skin branches on (%cs), so
     * every caller has to set it, not just the live ones. Tied to the test
     * above it left Track Info reporting whichever browser opened it -- 7 from
     * Files, 8 from Music -- and those pick different header viewports, so the
     * title drew at a different x depending on the route. Live mode keeps the
     * id3-identity test; only the activity moves. */
    push_current_activity(ACTIVITY_ID3SCREEN);
refresh_info:
    info.count = 0;
    info.playlist_display_index = playlist_display_index;
    for (i = 0; i < ARRAYLEN(id3_headers); i++)
    {
        char temp[8];
        info.info_id[i] = i;
        if (id3_get_or_speak_info((i*2)+1, &info, temp, 8, false) != NULL)
            info.info_id[info.count++] = i;
    }

    gui_synclist_init(&id3_lists, &id3_get_name_cb, &info, false, 1, NULL);
    if(global_settings.talk_menu)
        gui_synclist_set_voice_callback(&id3_lists, id3_speak_item);
    gui_synclist_set_nb_items(&id3_lists, info.count);
    gui_synclist_set_title(&id3_lists, str(LANG_TRACK_INFO), NOICON);
    gui_synclist_draw(&id3_lists);
    gui_synclist_speak_item(&id3_lists);
    while (true) {
        if(!list_do_action(CONTEXT_LIST,HZ/2, &id3_lists, &key)
           && key!=ACTION_NONE && key!=ACTION_UNKNOWN)
        {
            if (key == ACTION_STD_OK)
            {
                /* A row mid-scroll keeps animating under the text view. */
                gui_synclist_scroll_stop(&id3_lists);
                int header_id = id3_headers[info.info_id[id3_lists.selected_item]];
                char* title_and_text[2];
                title_and_text[0] = str(header_id);

                char buffer[MAX_PATH];
                title_and_text[1] = (char*)id3_get_or_speak_info(id3_lists.selected_item*2+1,&info, buffer, sizeof(buffer), false);

                if (view_text)
                {
                    FOR_NB_SCREENS(i)
                        viewportmanager_theme_enable(i, false, NULL);
                    view_text(title_and_text[0], title_and_text[1]);
                    FOR_NB_SCREENS(i)
                        viewportmanager_theme_undo(i, false);
                }
                gui_synclist_set_title(&id3_lists, str(LANG_TRACK_INFO), NOICON);
                gui_synclist_draw(&id3_lists);
                continue;
            }
            if (key == ACTION_STD_CANCEL)
            {
                ret = false;
                break;
            }
            else if (key == ACTION_STD_MENU ||
                        default_event_handler(key) == SYS_USB_CONNECTED)
            {
                ret =  true;
                break;
            }
        }
        else if (is_curr_track_info)
        {
            if (!audio_status())
            {
                ret = false;
                break;
            }
            else
            {
                playlist_display_index = playlist_get_display_index();
                if (playlist_display_index != info.playlist_display_index)
                    goto refresh_info;
            }
        }
    }
    FOR_NB_SCREENS(i)
        screens[i].scroll_stop(); /* when custom lists are used */

    pop_current_activity();
    return ret;
}

bool browse_id3(struct mp3entry *id3, int playlist_display_index, int playlist_amount,
                struct tm *modified, int track_ct,
                int (*view_text)(const char *title, const char *text))
{
    return browse_id3_ex(id3, NULL, playlist_display_index, playlist_amount,
                         modified, track_ct, view_text);
}
