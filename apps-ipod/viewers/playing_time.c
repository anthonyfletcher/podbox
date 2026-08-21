/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/playing_time.c
 * Core "Playing Time" screen, ported from the playing_time plugin.
 * Entered as playing_time_screen(), from the Current Playlist context menu.
 * GNU General Public License (version 2+)
 *
 * Playing-time statistics for the current playlist: elapsed, remaining and
 * total time, track counts and sizes.
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include "string-extra.h"    /* strlcpy */
#include "system.h"          /* cpu_boost */
#include "kernel.h"          /* HZ, IS_SYSEVENT */
#include "lang.h"            /* LANG_*, str */
#include "speech/language.h"        /* lang_is_rtl */
#include "settings/settings.h"        /* global_settings, SINGLE_MODE_* */
#include "input/action.h"          /* get_action, action_userabort */
#include "widgets/splash.h"          /* splash, splashf, splash_progress */
#include "speech/talk.h"            /* talk_*, TALK_ID, TALK_IDARRAY, UNIT_* */
#include "system/app_util.h"
#include "system/format_time.h"
#include "system/shutdown.h"
#include "metadata.h"        /* struct mp3entry, get_metadata */
#include "playlist/playlist.h"        /* playlist_*, struct playlist_track_info */
#include "audio.h"           /* audio_status, audio_current_track */
#include "draw/viewport.h"        /* viewportmanager_theme_enable/undo */
#include "draw/screen_access.h"   /* FOR_NB_SCREENS */
#include "draw/icon.h"            /* Icon_NOICON, NOICON */
#include "widgets/list.h"        /* gui_synclist */
#include "skin/statusbar_skinned.h" /* sb_set_persistent_title */
#include "playing_time.h"

#define pt_talk_ids(enqueue, ids...) talk_idarray(TALK_IDARRAY(ids), enqueue)

/* byte_units[] / kibyte_units for output_dyn_value come from misc.h */

static const int menu_items[] = {
        LANG_REMAINING,
        LANG_ELAPSED,
        LANG_PLAYTIME_TRK_REMAINING,
        LANG_PLAYTIME_TRK_ELAPSED,
        LANG_PLAYTIME_TRACK,
        LANG_PLAYTIME_STORAGE,
        LANG_PLAYTIME_AVG_TRACK_SIZE,
        LANG_PLAYTIME_AVG_BITRATE,
};

enum ePT_SUM {
    /* Note: Order matters (voicing order of LANG_PLAYTIME_STORAGE) */
    ePT_TOTAL = 0,
    ePT_ELAPSED,
    ePT_REMAINING,

    ePT_COUNT
};

struct playing_time_info {
    char single_mode_tag[MAX_PATH]; /* Relevant tag when single mode enabled */
    char error_str[16];                  /* Error message to display to user */
    unsigned long long size[ePT_COUNT]; /*               File size of tracks */
    unsigned long long length[ePT_COUNT]; /*                Length of tracks */
    unsigned long curr_track_length[ePT_COUNT]; /*      Current track length */
    int curr_track_index;   /*  Index of currently playing track in playlist */
    int curr_display_index; /*      Display index of currently playing track */
    int actual_index; /*            Display index in actually counted tracks */
    int counted; /*                        Number of tracks already added up */
    int nb_tracks; /*                           Number of tracks in playlist */
    int error_count; /*    Number of tracks whose data couldn't be retrieved */
    bool remaining_only; /*                 Whether to ignore elapsed tracks */
};

static int32_t single_mode_lang(void)
{
    switch (global_settings.single_mode)
    {
        case SINGLE_MODE_ALBUM:
            return LANG_ID3_ALBUM;
        case SINGLE_MODE_ALBUM_ARTIST:
            return LANG_ID3_ALBUMARTIST;
        case SINGLE_MODE_ARTIST:
            return LANG_ID3_ARTIST;
        case SINGLE_MODE_COMPOSER:
            return LANG_ID3_COMPOSER;
        case SINGLE_MODE_GROUPING:
            return LANG_ID3_GROUPING;
        case SINGLE_MODE_GENRE:
            return LANG_ID3_GENRE;
        case SINGLE_MODE_TRACK:
            return LANG_TRACK;
    }
    return LANG_OFF;
}

static char* single_mode_id3_tag(struct mp3entry *id3)
{
    switch (global_settings.single_mode)
    {
        case SINGLE_MODE_ALBUM:
            return id3->album;
        case SINGLE_MODE_ALBUM_ARTIST:
            return id3->albumartist;
        case SINGLE_MODE_ARTIST:
            return id3->artist;
        case SINGLE_MODE_COMPOSER:
            return id3->composer;
        case SINGLE_MODE_GROUPING:
            return id3->grouping;
        case SINGLE_MODE_GENRE:
            return id3->genre_string;
    }
    return NULL;
}

static char* get_percent_str(long percents)
{
    static char val[10];
    snprintf(val, sizeof(val), str(LANG_PERCENT_FORMAT), percents);
    return val;
}

static inline void prepare_time_string(char *buf, size_t buffer_len,
                                       long elapsed_pct, const char *timestr1,
                                       const char *timestr2)
{
    if (lang_is_rtl())
        snprintf(buf, buffer_len, "%s %s / %s",
                 get_percent_str(elapsed_pct), timestr2, timestr1);
    else
        snprintf(buf, buffer_len, "%s / %s %s",
                 timestr1, timestr2, get_percent_str(elapsed_pct));
}

/* list callback for playing_time screen */
static const char * pt_get_or_speak_info(int selected_item, void * data,
                                         char *buf, size_t buffer_len,
                                         bool say_it)
{
    long elapsed_pct; /* percentage of duration elapsed */
    struct playing_time_info *pti = (struct playing_time_info *)data;
    int info_no = selected_item/2;
    const int menu_name_id = menu_items[info_no];

    /* header */
    if (!say_it && !(selected_item % 2))
        return str(menu_name_id);

    /* data */
    switch(info_no) {
    case 0: { /* playlist remaining time */
        char timestr[25];
        format_time_auto(timestr, sizeof(timestr),
                         pti->length[ePT_REMAINING], UNIT_SEC, false);
        snprintf(buf, buffer_len, "%s", timestr);

        if (say_it)
            pt_talk_ids(false, menu_name_id,
                        TALK_ID(pti->length[ePT_REMAINING], UNIT_TIME));
        break;
    }
    case 1: { /* elapsed and total time */
        char timestr1[25], timestr2[25];
        format_time_auto(timestr1, sizeof(timestr1),
                         pti->length[ePT_ELAPSED], UNIT_SEC, true);

        format_time_auto(timestr2, sizeof(timestr2),
                         pti->length[ePT_TOTAL], UNIT_SEC, true);

        if (pti->length[ePT_TOTAL] == 0)
            elapsed_pct = 0;
        else if (pti->length[ePT_TOTAL] <= 0xFFFFFF)
        {
            elapsed_pct = (pti->length[ePT_ELAPSED] * 100
                           / pti->length[ePT_TOTAL]);
        }
        else /* sacrifice some precision to avoid overflow */
        {
            elapsed_pct = (pti->length[ePT_ELAPSED] >> 7) * 100
                          / (pti->length[ePT_TOTAL] >> 7);
        }
        prepare_time_string(buf, buffer_len, elapsed_pct, timestr1, timestr2);

        if (say_it)
            pt_talk_ids(false, menu_name_id,
                        TALK_ID(pti->length[ePT_ELAPSED], UNIT_TIME),
                        VOICE_OF,
                        TALK_ID(pti->length[ePT_TOTAL], UNIT_TIME),
                        VOICE_PAUSE,
                        TALK_ID(elapsed_pct, UNIT_PERCENT));
        break;
    }
    case 2: { /* track remaining time */
        char timestr[25];
        format_time_auto(timestr, sizeof(timestr),
                         pti->curr_track_length[ePT_REMAINING], UNIT_SEC, false);
        snprintf(buf, buffer_len, "%s", timestr);

        if (say_it)
            pt_talk_ids(false, menu_name_id,
                        TALK_ID(pti->curr_track_length[ePT_REMAINING], UNIT_TIME));
        break;
    }
    case 3: { /* track elapsed and duration */
        char timestr1[25], timestr2[25];

        format_time_auto(timestr1, sizeof(timestr1),
                         pti->curr_track_length[ePT_ELAPSED], UNIT_SEC, true);
        format_time_auto(timestr2, sizeof(timestr2),
                         pti->curr_track_length[ePT_TOTAL], UNIT_SEC, true);

        if (pti->curr_track_length[ePT_TOTAL] == 0)
            elapsed_pct = 0;
        else if (pti->curr_track_length[ePT_TOTAL] <= 0xFFFFFF)
        {
            elapsed_pct = (pti->curr_track_length[ePT_ELAPSED] * 100
                           / pti->curr_track_length[ePT_TOTAL]);
        }
        else /* sacrifice some precision to avoid overflow */
        {
            elapsed_pct = (pti->curr_track_length[ePT_ELAPSED] >> 7) * 100
                          / (pti->curr_track_length[ePT_TOTAL] >> 7);
        }
        prepare_time_string(buf, buffer_len, elapsed_pct, timestr1, timestr2);

        if (say_it)
            pt_talk_ids(false, menu_name_id,
                        TALK_ID(pti->curr_track_length[ePT_ELAPSED], UNIT_TIME),
                        VOICE_OF,
                        TALK_ID(pti->curr_track_length[ePT_TOTAL], UNIT_TIME),
                        VOICE_PAUSE,
                        TALK_ID(elapsed_pct, UNIT_PERCENT));
        break;
    }
    case 4: { /* track index */
        int track_pct = pti->actual_index * 100 / pti->counted;

        if (lang_is_rtl())
            snprintf(buf, buffer_len, "%s %d / %d", get_percent_str(track_pct),
                     pti->counted, pti->actual_index);
        else
            snprintf(buf, buffer_len, "%d / %d %s", pti->actual_index,
                     pti->counted, get_percent_str(track_pct));

        if (say_it)
            pt_talk_ids(false, menu_name_id,
                        TALK_ID(pti->actual_index, UNIT_INT),
                        VOICE_OF,
                        TALK_ID(pti->counted, UNIT_INT),
                        VOICE_PAUSE,
                        TALK_ID(track_pct, UNIT_PERCENT));
        break;
    }
    case 5: { /* storage size */
        int i;
        char kbstr[ePT_COUNT][20];

        for (i = 0; i < ePT_COUNT; i++) {
            output_dyn_value(kbstr[i], sizeof(kbstr[i]),
                             pti->size[i], kibyte_units, 3, true);
        }
        snprintf(buf, buffer_len, "%s (%s / %s)", kbstr[ePT_TOTAL],
                 kbstr[ePT_ELAPSED], kbstr[ePT_REMAINING]);

        if (say_it) {
            int32_t voice_ids[ePT_COUNT];
            voice_ids[ePT_TOTAL] = menu_name_id;
            voice_ids[ePT_ELAPSED] = VOICE_PLAYTIME_DONE;
            voice_ids[ePT_REMAINING] = LANG_REMAINING;

            for (i = 0; i < ePT_COUNT; i++)
            {
                pt_talk_ids(i > 0, VOICE_PAUSE, voice_ids[i]);
                output_dyn_value(NULL, 0, pti->size[i], kibyte_units, 3, true);
            }
        }
        break;
    }
    case 6: { /* Average track file size */
        char sizestr[20];
        long avg_track_size = pti->size[ePT_TOTAL] / pti->counted;
        output_dyn_value(sizestr, sizeof(sizestr), avg_track_size, kibyte_units, 3, true);
        snprintf(buf, buffer_len, "%s", sizestr);

        if (say_it) {
            talk_id(menu_name_id, false);
            output_dyn_value(NULL, 0, avg_track_size, kibyte_units, 3, true);
        }
        break;
    }
    case 7: { /* Average bitrate */
        /* Convert power of 2 kilobytes to power of 10 kilobits */
        long avg_bitrate = (pti->size[ePT_TOTAL] / pti->length[ePT_TOTAL]
                            * 1024 * 8 / 1000);
        snprintf(buf, buffer_len, "%ld  kbps", avg_bitrate);

        if (say_it)
            pt_talk_ids(false, menu_name_id,
                        TALK_ID(avg_bitrate, UNIT_KBIT));
        break;
    }
    }
    return buf;
}

/* One row per stat: "Label: Value". pt_get_or_speak_info() returns the value
 * for an odd index (the data branch); prepend the header for a single line. */
static const char * pt_get_info(int selected_item, void * data,
                                char *buffer, size_t buffer_len)
{
    char value[64];
    pt_get_or_speak_info(selected_item * 2 + 1, data, value, sizeof(value), false);
    snprintf(buffer, buffer_len, "%s: %s",
             str(menu_items[selected_item]), value);
    return buffer;
}

static int pt_speak_info(int selected_item, void * data)
{
    static char buffer[MAX_PATH];
    pt_get_or_speak_info(selected_item * 2, data, buffer, sizeof(buffer), true);
    return 0;
}

static bool pt_display_stats(struct playing_time_info *pti)
{
    struct simplelist_info info;
    simplelist_info_init(&info,
        *pti->single_mode_tag ? str(single_mode_lang()) : str(LANG_PLAYLIST),
        pti->remaining_only ? 1 : 8, pti);
    info.get_name = pt_get_info;
    if (global_settings.talk_menu)
        info.get_talk = pt_speak_info;
    return simplelist_show_list(&info);   /* true == USB connected */
}

static const char *pt_options_name(int selected_item, void * data,
                                   char *buf, size_t buf_size)
{
    (void) data;
    (void) buf;
    (void) buf_size;
    return selected_item == 0 ? str(LANG_ALL) :
           selected_item == 1 ? str(LANG_REMAINING) :
           str(single_mode_lang());
}

static int pt_options_speak(int selected_item, void * data)
{
    (void) data;
    talk_id(selected_item == 0 ? LANG_ALL :
            selected_item == 1 ? LANG_REMAINING :
            single_mode_lang(), false);
    return 0;
}

static int pt_options(struct playing_time_info *pti)
{
    struct simplelist_info info;
    simplelist_info_init(&info, str(LANG_PLAYING_TIME),
                         *pti->single_mode_tag ? 3 : 2, pti);
    info.get_name = pt_options_name;
    if (global_settings.talk_menu)
        info.get_talk = pt_options_speak;

    if (simplelist_show_list(&info))
        return 1;                   /* USB */
    if (info.selection < 0)
        return 0;                   /* cancelled */

    if (info.selection < 2)
        *pti->single_mode_tag = 0;
    if (info.selection == 1)
        pti->remaining_only = true;
    return -1;                      /* proceed to the stats screen */
}

static void pt_store_converted_totals(struct playing_time_info *pti)
{
    /* convert units from ms to s */
    pti->length[ePT_ELAPSED] /= 1000;
    pti->length[ePT_REMAINING] /= 1000;
    pti->curr_track_length[ePT_ELAPSED] /= 1000;
    pti->curr_track_length[ePT_REMAINING] /= 1000;
    /* convert units from Bytes to KiB  */
    pti->size[ePT_ELAPSED] >>= 10;
    pti->size[ePT_REMAINING] >>= 10;

    pti->length[ePT_TOTAL] = pti->length[ePT_ELAPSED] + pti->length[ePT_REMAINING];
    pti->curr_track_length[ePT_TOTAL] = pti->curr_track_length[ePT_ELAPSED]
                                       + pti->curr_track_length[ePT_REMAINING];
    pti->size[ePT_TOTAL] = pti->size[ePT_ELAPSED] + pti->size[ePT_REMAINING];
}

static int pt_add_track(int i, enum ePT_SUM section, struct playing_time_info *pti)
{
    static struct mp3entry id3;
    static struct playlist_track_info pl_track;
    int progress_total = pti->remaining_only ?
                         (pti->nb_tracks - pti->curr_display_index) + 1 :
                         pti->nb_tracks;

    /* (voiced) */
    splash_progress(pti->counted, progress_total, "%s (%s)",
                    str(LANG_WAIT), str(LANG_OFF_ABORT));

    if (action_userabort(TIMEOUT_NOBLOCK))
        return -1;
    else if (playlist_get_track_info(NULL, i, &pl_track) < 0
             || !get_metadata(&id3, -1, pl_track.filename))
    {
        pti->error_count++;
        return -2;
    }
    else if(*pti->single_mode_tag &&    /* single mode tag doesn't match */
            strcmp(pti->single_mode_tag, single_mode_id3_tag(&id3) ?: ""))
        return 1;

    pti->length[section] += id3.length;
    pti->size[section] += id3.filesize;
    pti->counted++;
    return 0;
}

static bool pt_add_remaining(struct playing_time_info *pti)
{
    int display_index = pti->curr_display_index + 1;
    for (int i = pti->curr_track_index + 1; display_index <= pti->nb_tracks; i++, display_index++)
    {
        if (i == pti->nb_tracks)
            i = 0;

        int ret = pt_add_track(i, ePT_REMAINING, pti);
        if (ret == 1)
            break;
        else if (ret == -1)
            return false;
    }
    return true;
}

static bool pt_add_elapsed(struct playing_time_info *pti)
{
    int display_index = pti->curr_display_index - 1;
    for (int i = pti->curr_track_index - 1; display_index > 0; i--, display_index--)
    {
        if (i < 0)
            i = pti->nb_tracks - 1;

        int ret = pt_add_track(i, ePT_ELAPSED, pti);
        if (ret == 1)
            break;
        else if (ret == -1)
            return false;
        else if (ret == 0)
            pti->actual_index++;
    }
    return true;
}

static bool pt_add_curr_track(struct playing_time_info *pti)
{
    struct mp3entry *curr_id3 = audio_current_track();
    playlist_get_resume_info(&pti->curr_track_index);

    if (pti->curr_track_index == -1 || !curr_id3)
        return false;

    pti->curr_display_index = playlist_get_display_index();
    pti->length[ePT_ELAPSED] = pti->curr_track_length[ePT_ELAPSED]
                             = curr_id3->elapsed;
    pti->length[ePT_REMAINING] = pti->curr_track_length[ePT_REMAINING]
                               = curr_id3->length - curr_id3->elapsed;
    pti->size[ePT_ELAPSED] = curr_id3->offset;
    pti->size[ePT_REMAINING] = curr_id3->filesize - curr_id3->offset;
    pti->actual_index = pti->counted = 1;
    strlcpy(pti->single_mode_tag, single_mode_id3_tag(curr_id3) ?: "",
            sizeof(pti->single_mode_tag));
    return true;
}

/* playing time screen: shows total and elapsed playlist duration and
   other stats */
static bool playing_time(void)
{
    struct playing_time_info pti;
    memset(&pti, 0, sizeof(struct playing_time_info));

    if (!pt_add_curr_track(&pti))
        return false;

    int opt = pt_options(&pti);
    if (opt > -1)
        return opt;

    cpu_boost(true);
    splash_progress_set_delay(HZ/2);
    pti.nb_tracks = playlist_amount();
    int success = (pti.remaining_only || pt_add_elapsed(&pti)) && pt_add_remaining(&pti);
    cpu_boost(false);
    if (!success)
        return false;
    if (pti.error_count > 0)
    {
        snprintf(pti.error_str, sizeof pti.error_str,
                 "%d %s", pti.error_count, str(LANG_TRACKS));
        if (global_settings.talk_menu)
        {
            talk_id(LANG_ERROR_FORMATSTR, false);
            talk_number(pti.error_count, true);
            talk_id(LANG_TRACKS, true);
            talk_force_enqueue_next();
        }
        /* (voiced above) */
        splashf(HZ, str(LANG_ERROR_FORMATSTR), pti.error_str);
    }

    pt_store_converted_totals(&pti);
    return pt_display_stats(&pti);
}

/* Core entry point: set up the themed status bar, run the screen. The USB
 * result is intentionally unused, matching the previous plugin_load() caller
 * which discarded it. */
void playing_time_screen(void)
{
    if (!audio_status())
    {
        splash(HZ * 2, "Nothing playing");
        return;
    }

    FOR_NB_SCREENS(i)
    {
        sb_set_persistent_title(str(LANG_PLAYING_TIME), Icon_NOICON, i);
        viewportmanager_theme_enable(i, true, NULL);
    }

    playing_time();

    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, false);
}
