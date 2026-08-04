/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/skin_engine/skin_tokens.c
 * Copyright (C) 2002-2007 Björn Stenberg
 * Copyright (C) 2007-2008 Nicolas Pennequin
 * GNU General Public License (version 2+)
 *
 * Resolves each skin tag to a value -- track title, elapsed time, battery,
 * and the rest. The bridge between the skin language and the running
 * system.
 *
 * Effectively one very large switch over the token enum. get_token_value()
 * is called during every repaint, once per tag on screen, so the cases are
 * written to read existing state rather than compute or block.
 *
 * A case returns a string, and optionally writes an integer through intval
 * for tags usable as a conditional or a bar value. Which integer depends on
 * how the tag was invoked: when limit == TOKEN_VALUE_ONLY it is the raw
 * value, otherwise it is a 1-based option number, because skin conditionals
 * count their branches from 1 (see skin_display.c). Returning NULL means the
 * tag has nothing to show, which the renderer treats differently from an
 * empty string.
 *
 * Parts, in order:
 *   - helpers for formatting values into the shared numeric buffer
 *   - track metadata and playback position tags
 *   - battery, disk, time and system state tags
 *   - playlist, database and file tags
 *   - get_token_value(): the dispatch every repaint goes through
 ****************************************************************************/
#include "font.h"
#include <stdio.h>
#include "string-extra.h"
#include <stdlib.h>
#include "kernel.h"
#include "input/action.h"
#include "system.h"
#include "settings/settings.h"
#include "settings/settings_list.h"
#include "rbunicode.h"
#include "diacritic.h"
#include "timefuncs.h"
#include "audio/play_status.h"
#include "power.h"
#include "powermgmt.h"
#include "sound.h"
#include "debug.h"
#include "metadata/cuesheet.h"
#include "replaygain.h"
#include "core_alloc.h"
#include "crc32.h"
#include "widgets/splash.h"
#include "file.h"
#if (CONFIG_STORAGE & STORAGE_ATA)
#include "ata.h" /* ata_disk_isssd() */
#endif
#include "audio/ab_repeat.h"
#include "lang.h"
#include "system/activity.h"
#include "system/format_time.h"
#include "system/volume.h"
#include "led.h"
#include "audio/peak_meter.h"
/* Image stuff */
#include "metadata/albumart.h"
#include "metadata/art_cache.h"
#include "playlist/playlist.h"
#include "audio/playback.h"
#include "tdspeed.h"
#include "draw/viewport.h"
#include "database/tagcache.h"
#include "database/db_summary.h"      /* db_summary_is_busy -- the %lb indicator */

#include "wps_internals.h"
#include "custom_tokens.h"
#include "skin_engine.h"
#include "statusbar_skinned.h"
#include "root_menu.h"
#include "speech/language.h"
#include "usb.h"
#include "widgets/list.h"
#include "screens/browse/browser.h"  /* browser_showing_art -- %La in chrome */
#include "widgets/option_select.h"
#include "screens/playback/wps.h"

#define NOINLINE __attribute__ ((noinline))

static const char* get_codectype(const struct mp3entry* id3)
{
    if (id3 && id3->codectype < AFMT_NUM_CODECS) {
        return get_codec_string(id3->codectype);
    } else {
        return NULL;
    }
}

/* Extract a part from a path.
 *
 * buf      - buffer extract part to.
 * buf_size - size of buffer.
 * path     - path to extract from.
 * level    - what to extract. 0 is file name, 1 is parent of file, 2 is
 *            parent of parent, etc.
 *
 * path does not need to be absolute, ignores multiple slashes
 * Returns buf if the desired level was found, NULL otherwise.
 */
char* get_dir(char* buf, int buf_size, const char* path, int level)
{
    const char* sep;
    const char* last_sep;
    int len;
    buf[0] = '\0';
    sep = path + strlen(path);
    last_sep = sep;

    while (sep > path)
    {
        if ('/' == *(--sep))
        {
            if (!level)
                break;

            last_sep = sep - 1;
            if (last_sep < path || *last_sep != '/') /* ignore multiple separators */
                level--;
        }
    }

    if (level || (last_sep <= sep)) /* level was not found */
        return NULL;

    if (sep == path && *sep != '/') /* for paths without leading separator */
        sep = path - 1;

    len = MIN((last_sep - sep), buf_size - 1);
    strmemccpy(buf, sep + 1, len + 1);
    return buf;
}

/* A helper to determine the enum value for pitch/speed.

   When there are two choices (i.e. boolean), return 1 if the value is
   different from normal value and 2 if the value is the same as the
   normal value.  E.g. "%?Sp<%Sp>" would show the pitch only when
   playing at a modified pitch.

   When there are more than two choices (i.e. enum), the left half of
   the choices are to show 0..normal range, and the right half of the
   choices are to show values over that.  The last entry is used when
   it is set to the normal setting, following the rockbox convention
   to use the last entry for special values.

   E.g.

   2 items: %?Sp<0..99 or 101..infinity|100>
   3 items: %?Sp<0..99|101..infinity|100>
   4 items: %?Sp<0..49|50..99|101..infinity|100>
   5 items: %?Sp<0..49|50..99|101..149|150..infinity|100>
   6 items: %?Sp<0..33|34..66|67..99|101..133|134..infinity|100>
   7 items: %?Sp<0..33|34..66|67..99|101..133|134..167|167..infinity|100>
*/
static int pitch_speed_enum(int range, int32_t val, int32_t normval)
{
    int center;
    int n;

    if (range < 3)
        return (val == normval) + 1;
    if (val == normval)
        return range;
    center = range / 2;
    n = (center * val) / normval + 1;
    return (range <= n) ? (range - 1) : n;
}

/* Whole-playlist progress by time (%pX). if track count exceeds MAX_TRACKS
 * the tag falls back to position-based progress.
 * The cache is keyed on the track count and a crc of the
 * first, middle and last filenames, so a playlist change, same-length swap,
 * or reshuffle causes a reload of the length data. */
#define PLPCT_MAX_TRACKS 500

static struct {
    uint32_t sig;           /* signature of the playlist to detect changed playlist or reshuffle*/
    int amount;
    bool nomem;             /* allocation failed -> position fallback */
    bool enabled;           /* tag was parsed */
    uint16_t track_mins[PLPCT_MAX_TRACKS];
    unsigned long total_min;
} plpct = { .amount = -1, .enabled = false };

static uint32_t plpct_sig(void)
{
    struct playlist_info *pl = playlist_get_current();
    /* changes on any playlist swap or reshuffle */
    return pl->seed ^ pl->first_index ^ pl->amount ^ pl->last_insert_pos ^ pl->dirlen
           ^ pl->created_tick ^ pl->max_playlist_size ^ pl->last_shuffled_start;
}

/* Equal-weight estimate (every track the same length)
     scaled by 1000 as %pX is percent-only. */
static void plpct_estimate(const struct mp3entry *id3, unsigned long elapsed_ms,
                           int index, int amount,
                           unsigned long *elapsed_s, unsigned long *total_s)
{
    unsigned long frac = 0;  /* 0..1000 through the current track */
    if (id3->length)
        frac = MIN(1000UL, 1000UL * elapsed_ms / id3->length);
    *elapsed_s = (unsigned long)(index - 1) * 1000 + frac;
    *total_s   = (unsigned long)amount * 1000;
}

void wps_playlist_percent_enable(void)
{
    plpct.enabled = true;
}

void wps_playlist_percent_prepare(void)
{
    if (!plpct.enabled)
        return;

    plpct.nomem = true;

    int amount = playlist_amount();
    plpct.sig = plpct_sig();
    plpct.amount = amount;
    if (amount <= 0 || amount > PLPCT_MAX_TRACKS) /* too many tracks abort */
    {
        return;
    }

    struct playlist_track_info info;

    struct mp3entry *tmp = get_temp_mp3entry(NULL); /* shared scratch */

    if (!tmp)
        return;
#if (CONFIG_STORAGE & STORAGE_ATA) /* Harddrive estimate length based on filesize */
    unsigned int last_afmt = AFMT_UNKNOWN;
    uint32_t last_bps = 0;
#endif
    for (int n = 0; n < amount; n++)
    {
        unsigned long secs = 0;
        uint16_t mins = 0;
        if (playlist_get_track_info(NULL, n, &info) < 0) /* couldn't get track filename abort */
            goto abort;

        int slot = info.display_index - 1;
        if (slot < 0 || slot >= amount)
            goto abort;

#if (CONFIG_STORAGE & STORAGE_ATA) /* Harddrive */
        int fd = open(info.filename, O_RDONLY);
        if (fd >= 0)
        {
            unsigned int afmt = probe_file_format(info.filename);
            off_t size = filesize(fd);

            if (size > 0)
            {
                if (afmt != last_afmt || last_bps == 0
                    || amount <= 50 || (amount <= 250 && ata_disk_isssd()))
                    /* TODO tune this for harddisk devices */
                {
                    if (get_metadata_afmt(tmp, fd, info.filename, afmt,
                        METADATA_EXCLUDE_ID3_PATH | METADATA_EXCLUDE_NORMALIZE))
                    {
                        secs = tmp->length / 1000;
                        last_bps = (uint32_t)((uint64_t)size * 1000 / tmp->length);
                        last_afmt = afmt;
                    }
                }
                else /* file size only -- no per-track metadata parse */
                {
                    secs = (unsigned long)((uint64_t)size / last_bps);
                }
                mins = MIN(MAX(1, (secs + 30) / 60), 65535ul);/* minutes, rounded to nearest */
            }
            close(fd);
        }
#else
        if (get_metadata_ex(tmp, -1, info.filename,
                            METADATA_EXCLUDE_ID3_PATH | METADATA_EXCLUDE_NORMALIZE))
        {
            secs = tmp->length / 1000;
            mins = MIN(MAX(1, (secs + 30) / 60), 65535ul);/* minutes, rounded to nearest */
        }
#endif
        /* unreadable tracks count as zero length */
        plpct.track_mins[slot] = mins;
    }

    plpct.total_min = 0;
    for (int t = 0; t < plpct.amount; t++)
        plpct.total_min += plpct.track_mins[t];
    plpct.nomem = false;
abort:
    get_temp_mp3entry(tmp); /* release scratch */
}

bool wps_get_playlist_percent(struct mp3entry *id3, unsigned long elapsed_ms,
                              unsigned long *elapsed_s, unsigned long *total_s)
{
    int amount = playlist_amount();
    int index = playlist_get_display_index();  /* 1-based */

    if (!id3 || amount <= 0 || index < 1 || index > amount)
        return false;

    uint32_t sig = plpct_sig();
    if (amount != plpct.amount || sig != plpct.sig)
    {
        splashf(0, ID2P(LANG_WAIT));
        wps_playlist_percent_enable();
        wps_playlist_percent_prepare();
    }
    /* error parsing playlist or more than MAX_TRACKS fall back to
       position-based progress rather than blank */
    if (plpct.nomem)
    {
        plpct_estimate(id3, elapsed_ms, index, amount, elapsed_s, total_s);
        return true;
    }

    unsigned long before_min = 0;
    for (int t = 0; t < index - 1; t++)
        before_min += plpct.track_mins[t];

    *elapsed_s = before_min * 60 + elapsed_ms / 1000;
    *total_s = plpct.total_min * 60;
    if (*elapsed_s > *total_s)
        *elapsed_s = *total_s;
    return true;
}


const char *get_cuesheetid3_token(struct wps_token *token, struct mp3entry *id3,
                                  int offset_tracks, char *buf, int buf_size)
{
    struct cuesheet *cue = id3?id3->cuesheet:NULL;
    if (!cue || !cue->curr_track)
        return NULL;

    struct cue_track_info *track = cue->curr_track;
    if (offset_tracks)
    {
        if (cue->curr_track_idx+offset_tracks < cue->track_count)
            track+=offset_tracks;
        else
            return NULL;
    }
    switch (token->type)
    {
        case SKIN_TOKEN_METADATA_ARTIST:
            return *track->performer ? track->performer : NULL;
        case SKIN_TOKEN_METADATA_COMPOSER:
            return *track->songwriter ? track->songwriter : NULL;
        case SKIN_TOKEN_METADATA_ALBUM:
            return *cue->title ? cue->title : NULL;
        case SKIN_TOKEN_METADATA_ALBUM_ARTIST:
            return *cue->performer ? cue->performer : NULL;
        case SKIN_TOKEN_METADATA_TRACK_TITLE:
            return *track->title ? track->title : NULL;
        case SKIN_TOKEN_METADATA_TRACK_NUMBER:
            snprintf(buf, buf_size, "%d/%d",
                     cue->curr_track_idx+offset_tracks+1, cue->track_count);
            return buf;
        default:
            return NULL;
    }
    return NULL;
}

static const char* get_filename_token(struct wps_token *token, char* filename,
                               char *buf, int buf_size)
{
    if (filename)
    {
        switch (token->type)
        {
            case SKIN_TOKEN_FILE_PATH:
                return filename;
            case SKIN_TOKEN_FILE_NAME:
                if (get_dir(buf, buf_size, filename, 0)) {
                    /* Remove extension */
                    char* sep = strrchr(buf, '.');
                    if (NULL != sep) {
                        *sep = 0;
                    }
                    return buf;
                }
                return NULL;
            case SKIN_TOKEN_FILE_NAME_WITH_EXTENSION:
                return get_dir(buf, buf_size, filename, 0);
            case SKIN_TOKEN_FILE_DIRECTORY:
                return get_dir(buf, buf_size, filename, token->value.i);
            default:
                return NULL;
        }
    }
    return NULL;
}

/* All tokens which only need the info to return a value go in here */
const char *get_id3_token(struct wps_token *token, struct mp3entry *id3,
                          char *filename, char *buf, int buf_size, int limit, int *intval)
{
    struct wps_state *state = get_wps_state();
    if (id3)
    {
        unsigned long length = id3->length;
        unsigned long elapsed = id3->elapsed + state->ff_rewind_count;
        switch (token->type)
        {
            case SKIN_TOKEN_METADATA_ARTIST:
                return id3->artist;
            case SKIN_TOKEN_METADATA_COMPOSER:
                return id3->composer;
            case SKIN_TOKEN_METADATA_ALBUM:
                return id3->album;
            case SKIN_TOKEN_METADATA_ALBUM_ARTIST:
                return id3->albumartist;
            case SKIN_TOKEN_METADATA_GROUPING:
                return id3->grouping;
            case SKIN_TOKEN_METADATA_GENRE:
                return id3->genre_string;
            case SKIN_TOKEN_METADATA_DISC_NUMBER:
                if (id3->disc_string)
                    return id3->disc_string;
                if (id3->discnum) {
                    itoa_buf(buf, buf_size, id3->discnum);
                    return buf;
                }
                return NULL;
            case SKIN_TOKEN_METADATA_TRACK_NUMBER:
                if (id3->track_string)
                    return id3->track_string;
                if (id3->tracknum >= 0) {
                    itoa_buf(buf, buf_size, id3->tracknum);
                    return buf;
                }
                return NULL;
            case SKIN_TOKEN_METADATA_TRACK_TITLE:
                return id3->title;
            case SKIN_TOKEN_METADATA_VERSION:
                switch (id3->id3version)
                {
                    case ID3_VER_1_0:
                        return "1";
                    case ID3_VER_1_1:
                        return "1.1";
                    case ID3_VER_2_2:
                        return "2.2";
                    case ID3_VER_2_3:
                        return "2.3";
                    case ID3_VER_2_4:
                        return "2.4";
                    default:
                        break;
                }
                return NULL;
            case SKIN_TOKEN_METADATA_YEAR:
                if( id3->year_string )
                    return id3->year_string;
                if (id3->year) {
                    itoa_buf(buf, buf_size, id3->year);
                    return buf;
                }
                return NULL;
            case SKIN_TOKEN_METADATA_COMMENT:
                return id3->comment;
            case SKIN_TOKEN_FILE_BITRATE:
                if(id3->bitrate)
                    itoa_buf(buf, buf_size, id3->bitrate);
                else
                    return "?";
                return buf;
            case SKIN_TOKEN_TRACK_TIME_ELAPSED:
                if (intval && limit == TOKEN_VALUE_ONLY)
                    *intval = elapsed/1000;
                format_time(buf, buf_size, elapsed);
                return buf;

            case SKIN_TOKEN_TRACK_TIME_REMAINING:
                if (intval && limit == TOKEN_VALUE_ONLY)
                    *intval = (length - elapsed)/1000;
                format_time(buf, buf_size, length - elapsed);
                return buf;

            case SKIN_TOKEN_TRACK_LENGTH:
                if (intval && limit == TOKEN_VALUE_ONLY)
                    *intval = length/1000;
                format_time(buf, buf_size, length);
                return buf;

            case SKIN_TOKEN_TRACK_ELAPSED_PERCENT:
                if (length <= 0)
                    return NULL;

                if (intval)
                {
                    if (limit == TOKEN_VALUE_ONLY)
                        limit = 100; /* make it a percentage */
                    *intval = limit * elapsed / length + 1;
                }
                snprintf(buf, buf_size, "%lu", 100 * elapsed / length);
                return buf;

            case SKIN_TOKEN_PLAYLIST_ELAPSED_PERCENT:
            {
                unsigned long pl_elapsed, pl_total;
                if (!wps_get_playlist_percent(id3, elapsed,
                                              &pl_elapsed, &pl_total)
                    || pl_total == 0)
                    return NULL;

                /* Always yields a value for a sized playlist -- the
                 * equal-weight estimate until the scan finishes, then the
                 * time-weighted figure -- so it behaves like any other percent
                 * tag, and %?pX<...> is true whenever the tag applies. */
                if (intval && limit == TOKEN_VALUE_ONLY)
                    *intval = 100 * pl_elapsed / pl_total;
                snprintf(buf, buf_size, "%lu", 100 * pl_elapsed / pl_total);
                return buf;
            }

            case SKIN_TOKEN_TRACK_STARTING:
                {
                    unsigned long time = token->value.i * (HZ/TIMEOUT_UNIT);
                    if (elapsed < time)
                        return "starting";
                }
                return NULL;
            case SKIN_TOKEN_TRACK_ENDING:
                {
                    unsigned long time = token->value.i * (HZ/TIMEOUT_UNIT);
                    if (length - elapsed < time)
                        return "ending";
                }
                return NULL;

            case SKIN_TOKEN_FILE_CODEC:
                if (intval)
                {
                    if(id3->codectype == AFMT_UNKNOWN)
                        *intval = AFMT_NUM_CODECS;
                    else
                        *intval = id3->codectype;
                }
                return get_codectype(id3);

            case SKIN_TOKEN_FILE_FREQUENCY:
                itoa_buf(buf, buf_size, id3->frequency);
                return buf;
            case SKIN_TOKEN_FILE_FREQUENCY_KHZ:
                /* ignore remainders < 100, so 22050 Hz becomes just 22k */
                if ((id3->frequency % 1000) < 100)
                    itoa_buf(buf, buf_size, id3->frequency / 1000);
                else
                    snprintf(buf, buf_size, "%ld.%lu",
                            id3->frequency / 1000,
                            (id3->frequency % 1000) / 100);
                return buf;
            case SKIN_TOKEN_FILE_VBR:
                return (id3->vbr) ? "(avg)" : NULL;
            case SKIN_TOKEN_FILE_SIZE:
                itoa_buf(buf, buf_size, id3->filesize / 1024);
                return buf;

        case SKIN_TOKEN_DATABASE_PLAYCOUNT:
            if (intval)
                *intval = id3->playcount + 1;
            itoa_buf(buf, buf_size, id3->playcount);
            return buf;
        case SKIN_TOKEN_DATABASE_RATING:
            if (intval)
                *intval = id3->rating + 1;
            itoa_buf(buf, buf_size, id3->rating);
            return buf;
        case SKIN_TOKEN_DATABASE_AUTOSCORE:
            if (intval)
                *intval = id3->score + 1;
            itoa_buf(buf, buf_size, id3->score);
            return buf;

            default:
                return get_filename_token(token, id3->path, buf, buf_size);
        }
    }
    else /* id3 == NULL, handle the error based on the expected return type */
    {
        switch (token->type)
        {
            /* Most tokens expect NULL on error so leave that for the default case,
             * The ones that expect "0" need to be handled */
            case SKIN_TOKEN_FILE_FREQUENCY:
            case SKIN_TOKEN_FILE_FREQUENCY_KHZ:
            case SKIN_TOKEN_FILE_SIZE:
            case SKIN_TOKEN_DATABASE_PLAYCOUNT:
            case SKIN_TOKEN_DATABASE_RATING:
            case SKIN_TOKEN_DATABASE_AUTOSCORE:
                if (intval)
                    *intval = 0;
                return "0";
            default:
                return get_filename_token(token, filename, buf, buf_size);
        }
    }
    return buf;
}


static struct mp3entry* get_mp3entry_from_offset(int offset,
                                   struct mp3entry **freeid3, char **filename)
{
    struct mp3entry* pid3 = NULL;
    struct wps_state *state = get_wps_state();
    struct cuesheet *cue = state->id3 ? state->id3->cuesheet : NULL;
    const char *fname = NULL;

    if (cue && cue->curr_track_idx + offset < cue->track_count)
        pid3 = state->id3;
    else if (offset == 0)
        pid3 = state->id3;
    else if (offset == 1)
        pid3 = state->nid3;
    else if (audio_status() & AUDIO_STATUS_PLAY)
    {
        /* we had to get a temp id3 entry, fill freeid3 to free later */
        struct mp3entry *bufid3 = get_temp_mp3entry(NULL);
        *freeid3 = bufid3;
        fname = playlist_peek(offset, bufid3->path, sizeof(bufid3->path));
        *filename = (char*)fname;

        if (
            tagcache_fill_tags(bufid3, NULL) ||
            audio_peek_track(bufid3, offset)
        )
        {
            pid3 = bufid3;
        }
        else /* failed */
        {
            /* ensure *filename gets the path, audio_peek_track() cleared it */
            fname = playlist_peek(offset, bufid3->path, sizeof(bufid3->path));
            *filename = (char*)fname;
        }
    }

    return pid3;
}

/* Tokens which use current id3 go here */
static const char *try_id3_token(struct wps_token *token, int offset,
                                              char *buf, int buf_size,
                                              int limit, int *intval)
{
    const char *out_text = NULL;
    char *filename = NULL;
    struct mp3entry *id3, *freeid3 = NULL;

    /* if freeid3 is filled on return we need to free the id3 when finished */
    id3 = get_mp3entry_from_offset(token->next? 1: offset, &freeid3, &filename);

    if (token->type == SKIN_TOKEN_REPLAYGAIN)
    {
        int numeric_ret = -1;
        const char *numeric_buf = buf;
        int globtype = global_settings.replaygain_settings.type;
        int val;

        if (globtype == REPLAYGAIN_OFF)
            val = 1; /* off */
        else
        {
            int type = id3_get_replaygain_mode(id3);

            if (type < 0)
                val = 6;    /* no tag */
            else
                val = type + 2;

            if (globtype == REPLAYGAIN_SHUFFLE)
                val += 2;
        }

        numeric_ret = val;
        switch (val)
        {
            case 1:
            case 6:
                numeric_buf = "+0.00 dB";;
                break;
            /* due to above, coming here with !id3 shouldn't be possible */
            case 2:
            case 4:
                replaygain_itoa(buf, buf_size, id3->track_level);
                break;
            case 3:
            case 5:
                replaygain_itoa(buf, buf_size, id3->album_level);
                break;
        }

        if (intval)
        {
            *intval = numeric_ret;
        }
        out_text = numeric_buf;
        goto free_id3_outtext;
    }

    struct wps_state *state = get_wps_state();
    if (id3 && id3 == state->id3 && id3->cuesheet)
    {
        out_text = get_cuesheetid3_token(token, id3,
                                         token->next?1:offset, buf, buf_size);
        if (out_text)
            goto free_id3_outtext;
    }

    out_text = get_id3_token(token, id3, filename, buf, buf_size, limit, intval);

    if (out_text)
        goto free_id3_outtext;

    if (freeid3)
        get_temp_mp3entry(freeid3);
    return NULL;
free_id3_outtext:
    if (freeid3)
        get_temp_mp3entry(freeid3);
    return out_text;
}

/* Render one %sel argument to text. Keys and values may each be written as a
 * literal, a number, or a tag, so all three parameter shapes are accepted. */
static const char* eval_select_param(struct gui_wps *gwps, char *skinbuffer,
                                     struct skin_tag_parameter *param,
                                     int offset, char *buf, int buf_size)
{
    switch (param->type)
    {
        case STRING:
            return SKINOFFSETTOPTR(skinbuffer, param->data.text);
        case INTEGER:
        case DECIMAL:
        case PERCENT:
            itoa_buf(buf, buf_size, param->data.number);
            return buf;
        case CODE:
        {
            struct skin_element *element =
                    SKINOFFSETTOPTR(skinbuffer, param->data.code);
            if (!element) return NULL;
            return get_token_value(gwps,
                                   SKINOFFSETTOPTR(skinbuffer, element->data),
                                   offset, buf, buf_size, NULL);
        }
        default:
            return NULL;
    }
}

/* A numeric %ma operand: a literal int, or a tag rendered to text and atoi'd. */
static int eval_param_int(struct gui_wps *gwps, char *skinbuffer,
                          struct skin_tag_parameter *param,
                          int offset, char *buf, int buf_size)
{
    if (param->type == INTEGER)
        return param->data.number;
    const char *s = eval_select_param(gwps, skinbuffer, param, offset,
                                      buf, buf_size);
    return s ? atoi(s) : 0;
}

/* %sf(haystack, needle) -- 0-based character index of the first occurrence of
 * needle in haystack, or -1. NOINLINE for the haystack buffer (recursive). */
static int NOINLINE get_strfind_value(struct gui_wps *gwps,
                                      struct skin_element *element,
                                      int offset, char *buf, int buf_size)
{
    char *skinbuffer = get_skin_buffer(gwps->data);
    char haystack[MAX_PATH];
    const char *s, *needle, *hit;
    int cidx = 0;
    const unsigned char *c;
    ucschar_t ch;

    if (!element || !element->params) return -1;
    struct skin_tag_parameter *params =
            SKINOFFSETTOPTR(skinbuffer, element->params);

    /* Copy the haystack out: evaluating the needle reuses buf. */
    s = eval_select_param(gwps, skinbuffer, &params[0], offset, buf, buf_size);
    if (!s) return -1;
    strmemccpy(haystack, s, sizeof(haystack));
    needle = eval_select_param(gwps, skinbuffer, &params[1], offset,
                               buf, buf_size);
    if (!needle) return -1;
    hit = strstr(haystack, needle);
    if (!hit) return -1;

    for (c = (const unsigned char *)haystack; (const char *)c < hit; cidx++)
        c = utf8decode(c, &ch);
    return cidx;
}

/* %pd(n, text) -- text padded with trailing spaces or truncated to exactly n
 * characters. NOINLINE for the source buffer (recursive). */
static const char* NOINLINE get_pad_value(struct gui_wps *gwps,
                                          struct skin_element *element,
                                          int offset, char *buf, int buf_size)
{
    char *skinbuffer = get_skin_buffer(gwps->data);
    char src[MAX_PATH];
    const char *t;
    int n, len, bytes;

    if (!element || !element->params) return NULL;
    struct skin_tag_parameter *params =
            SKINOFFSETTOPTR(skinbuffer, element->params);

    n = params[0].data.number;
    if (n < 0) n = 0;
    t = eval_select_param(gwps, skinbuffer, &params[1], offset, buf, buf_size);
    strmemccpy(src, t ? t : "", sizeof(src));
    len = (int)utf8length((const unsigned char *)src);

    if (len >= n)                                   /* truncate to n chars */
    {
        bytes = utf8seek((const unsigned char *)src, n);
        if (bytes >= buf_size) bytes = buf_size - 1;
        memcpy(buf, src, bytes);
        buf[bytes] = '\0';
    }
    else                                            /* pad to n chars */
    {
        int pad = n - len;
        bytes = strlen(src);
        if (bytes + pad >= buf_size) pad = buf_size - 1 - bytes;
        if (pad < 0) pad = 0;
        memcpy(buf, src, bytes);
        memset(buf + bytes, ' ', pad);
        buf[bytes + pad] = '\0';
    }
    return buf;
}

/* %wr(n, text) -- return the nth (0-based) word-wrapped line of text, wrapped
 * to the current viewport width in its font. One forward pass per call: walk
 * the text accumulating pixel width (mirroring font_getstringnsize), remember
 * the last space, and break there on overflow -- never re-measuring prefixes.
 * NOINLINE to keep the two work buffers off get_token_value's recursive frame. */
static const char* NOINLINE get_wordwrap_token_value(struct gui_wps *gwps,
                                                     struct skin_element *element,
                                                     int offset, char *buf,
                                                     int buf_size)
{
    char *skinbuffer = get_skin_buffer(gwps->data);
    struct viewport *vp = *gwps->display->current_viewport;
    char source[MAX_PATH];
    const char *text;
    int target_line = 0, cur_line = 0;
    struct font *pf;
    const unsigned char *line_start;

    if (!element || !element->params) return NULL;
    struct skin_tag_parameter *params =
            SKINOFFSETTOPTR(skinbuffer, element->params);

    target_line = params[0].data.number;

    /* Copy the source out: evaluating a tag arg may hand back buf itself, which
     * we overwrite with the result, and we walk the text more than once. */
    text = eval_select_param(gwps, skinbuffer, &params[1], offset,
                             buf, buf_size);
    if (!text) return NULL;
    strmemccpy(source, text, sizeof(source));

    pf = font_get(vp->font);
    font_lock(vp->font, true);

    line_start = (const unsigned char *)source;
    for (;;)
    {
        const unsigned char *cursor = line_start;
        const unsigned char *last_space = NULL;   /* break just before this */
        const unsigned char *line_end, *next_start;
        ucschar_t ch;
        int width = 0;
        bool at_end = false;

        for (;;)
        {
            const unsigned char *prev = cursor;
            cursor = utf8decode(cursor, &ch);
            if (ch == 0)
            {
                line_end = next_start = prev;      /* text ends this line */
                at_end = true;
                break;
            }
            if (IS_DIACRITIC(ch))
                continue;
            if (ch == ' ')
                last_space = prev;
            width += font_get_width(pf, ch);
            if (width > vp->width)
            {
                if (last_space && last_space > line_start)
                {
                    line_end = last_space;         /* break at the space, */
                    next_start = last_space + 1;   /* which is dropped */
                }
                else if (prev > line_start)
                {
                    line_end = next_start = prev;  /* hard break before ch */
                }
                else
                {
                    line_end = next_start = cursor; /* lone too-wide glyph */
                }
                break;
            }
        }

        if (cur_line == target_line)
        {
            int len = line_end - line_start;
            font_lock(vp->font, false);
            if (len >= buf_size)
                len = buf_size - 1;
            memcpy(buf, line_start, len);
            buf[len] = '\0';
            return buf;
        }

        if (at_end)                 /* asked for a line past the wrapped text */
            break;
        cur_line++;
        line_start = next_start;
    }
    font_lock(vp->font, false);
    return NULL;
}

/* %sel(subject, key1, value1, ..., [default]) -- return the value paired with
 * the first key equal to the subject, else the trailing default, else nothing.
 * NOINLINE, like get_lif_token_value below, to keep the subject buffer out of
 * get_token_value()'s frame; that function recurses. */
static const char* NOINLINE get_select_token_value(struct gui_wps *gwps,
                                                   struct skin_element *element,
                                                   int offset, char *buf,
                                                   int buf_size)
{
    char *skinbuffer = get_skin_buffer(gwps->data);
    char subject[MAX_PATH];
    const char *val;
    int i;

    if (!element || !element->params) return NULL;
    struct skin_tag_parameter* params =
            SKINOFFSETTOPTR(skinbuffer, element->params);

    /* The subject needs storage of its own: evaluating each key below reuses
     * buf, which would otherwise overwrite it part-way through the search. */
    val = eval_select_param(gwps, skinbuffer, &params[0],
                            offset, buf, buf_size);
    if (!val) return NULL;
    strmemccpy(subject, val, sizeof(subject));

    /* Arguments after the subject pair up as key, value. */
    for (i = 1; i + 1 < element->params_count; i += 2)
    {
        val = eval_select_param(gwps, skinbuffer, &params[i],
                                offset, buf, buf_size);
        if (val && !strcmp(subject, val))
            return eval_select_param(gwps, skinbuffer, &params[i+1],
                                     offset, buf, buf_size);
    }

    /* An odd argument left over at the end is the default. */
    if (i < element->params_count)
        return eval_select_param(gwps, skinbuffer, &params[i],
                                 offset, buf, buf_size);
    return NULL;
}

/* Don't inline this; it was broken out of get_token_value to reduce stack
 * usage.
 */
static const char* NOINLINE get_lif_token_value(struct gui_wps *gwps,
                                                struct logical_if *lif,
                                                int offset, char *buf,
                                                int buf_size)
{
    int a = lif->num_options;
    int b;
    bool number_set = true;
    struct wps_token *liftoken = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), lif->token);
    const char* out_text = get_token_value(gwps, liftoken, offset, buf, buf_size, &a);
    if (a == -1 && liftoken->type != SKIN_TOKEN_VOLUME)
    {
        a = (out_text && *out_text) ? 1 : 0;
        number_set = false;
    }
    switch (lif->operand.type)
    {
        case STRING:
        {
            char *cmp = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), lif->operand.data.text);
            if (out_text == NULL)
                return NULL;
            a = strcmp(out_text, cmp);
            b = 0;
            break;
        }
        case INTEGER:
            if (!number_set && out_text && ((*out_text >= '0' && *out_text <= '9') || (*out_text == '-' && out_text[1] >= '0' && out_text[1] <= '9')))
                a = atoi(out_text);
            /* fall through */
        case PERCENT:
        case DECIMAL:
            b = lif->operand.data.number;
            break;
        case CODE:
        {
            char temp_buf[MAX_PATH];
            const char *outb;
            struct skin_element *element = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), lif->operand.data.code);
            if (!element) return NULL;
            struct wps_token *token = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), element->data);
            b = lif->num_options;

            outb = get_token_value(gwps, token, offset, temp_buf,
                                   sizeof(temp_buf), &b);

            if (b == -1 && liftoken->type != SKIN_TOKEN_VOLUME)
            {
                if (!out_text || !outb)
                    return (lif->op == IF_EQUALS) ? NULL : "neq";
                bool equal = strcmp(out_text, outb) == 0;
                if (lif->op == IF_EQUALS)
                    return equal ? "eq" : NULL;
                else if (lif->op == IF_NOTEQUALS)
                    return !equal ? "neq" : NULL;
                else
                    b = (outb && *outb) ? 1 : 0;
            }
        }
        break;
        case DEFAULT:
            b = -1;
            break;
    }

    switch (lif->op)
    {
        case IF_EQUALS:
            return a == b ? "eq" : NULL;
        case IF_NOTEQUALS:
            return a != b ? "neq" : NULL;
        case IF_LESSTHAN:
            return a < b ? "lt" : NULL;
        case IF_LESSTHAN_EQ:
            return a <= b ? "lte" : NULL;
        case IF_GREATERTHAN:
            return a > b ? "gt" : NULL;
        case IF_GREATERTHAN_EQ:
            return a >= b ? "gte" : NULL;
    }
    return NULL;
}

/* RTC tokens go here */
static const char* get_rtc_token_value(struct wps_token *token,
                                                char *buf, int buf_size,
                                                int *intval)
{
    int numeric_ret = -1;
    const char *numeric_buf = buf;
    struct tm* tm = get_time();

    if (!valid_time(tm))
        return NULL;

    switch (token->type)
    {
        default:
            return "?";


        case SKIN_TOKEN_RTC_12HOUR_CFG:
            itoa_buf(buf, buf_size, global_settings.timeformat);
            numeric_ret = global_settings.timeformat + 1;
            break;
        case SKIN_TOKEN_RTC_DAY_OF_MONTH:
            /* d: day of month (01..31) */
            snprintf(buf, buf_size, "%02d", tm->tm_mday);
            numeric_ret = tm->tm_mday - 1;
            break;

        case SKIN_TOKEN_RTC_DAY_OF_MONTH_BLANK_PADDED:
            /* e: day of month, blank padded ( 1..31) */
            snprintf(buf, buf_size, "%2d", tm->tm_mday);
            numeric_ret = tm->tm_mday - 1;
            break;

        case SKIN_TOKEN_RTC_HOUR_24_ZERO_PADDED:
            /* H: hour (00..23) */
            numeric_ret = tm->tm_hour;
            snprintf(buf, buf_size, "%02d", numeric_ret);
            break;

        case SKIN_TOKEN_RTC_HOUR_24:
            /* k: hour ( 0..23) */
            numeric_ret = tm->tm_hour;
            snprintf(buf, buf_size, "%2d", numeric_ret);
            break;

        case SKIN_TOKEN_RTC_HOUR_12_ZERO_PADDED:
            /* I: hour (01..12) */
            numeric_ret = (tm->tm_hour % 12 == 0) ? 12 : tm->tm_hour % 12;
            snprintf(buf, buf_size, "%02d", numeric_ret);
            break;

        case SKIN_TOKEN_RTC_HOUR_12:
            /* l: hour ( 1..12) */
            numeric_ret = (tm->tm_hour % 12 == 0) ? 12 : tm->tm_hour % 12;
            snprintf(buf, buf_size, "%2d", numeric_ret);
            break;

        case SKIN_TOKEN_RTC_MONTH:
            /* m: month (01..12) */
            numeric_ret = tm->tm_mon + 1;
            snprintf(buf, buf_size, "%02d", numeric_ret);
            break;

        case SKIN_TOKEN_RTC_MINUTE:
            /* M: minute (00..59) */
            numeric_ret = tm->tm_min;
            snprintf(buf, buf_size, "%02d", numeric_ret);
            break;

        case SKIN_TOKEN_RTC_SECOND:
            /* S: second (00..59) */
            numeric_ret = tm->tm_sec;
            snprintf(buf, buf_size, "%02d", numeric_ret);
            break;

        case SKIN_TOKEN_RTC_YEAR_2_DIGITS:
            /* y: last two digits of year (00..99) */
            numeric_ret = tm->tm_year % 100;
            snprintf(buf, buf_size, "%02d", numeric_ret);
            break;

        case SKIN_TOKEN_RTC_YEAR_4_DIGITS:
            /* Y: year (1970...) */
            numeric_ret = tm->tm_year + 1900;
            snprintf(buf, buf_size, "%04d", numeric_ret);
            break;

        case SKIN_TOKEN_RTC_AM_PM_UPPER:
            /* p: upper case AM or PM indicator */
            numeric_ret = tm->tm_hour/12 == 0 ? 0 : 1;
            numeric_buf = numeric_ret == 0 ? "AM" : "PM";
            break;

        case SKIN_TOKEN_RTC_AM_PM_LOWER:
            /* P: lower case am or pm indicator */
            numeric_ret= tm->tm_hour/12 == 0 ? 0 : 1;
            numeric_buf = numeric_ret == 0 ? "am" : "pm";
            break;

        case SKIN_TOKEN_RTC_WEEKDAY_NAME:
            /* a: abbreviated weekday name (Sun..Sat) */
            return str(LANG_WEEKDAY_SUNDAY + tm->tm_wday);

        case SKIN_TOKEN_RTC_MONTH_NAME:
            /* b: abbreviated month name (Jan..Dec) */
            return str(LANG_MONTH_JANUARY + tm->tm_mon);

        case SKIN_TOKEN_RTC_DAY_OF_WEEK_START_MON:
            /* u: day of week (1..7); 1 is Monday */
            snprintf(buf, buf_size, "%1d", tm->tm_wday + 1);
            numeric_ret = (tm->tm_wday == 0) ? 7 : tm->tm_wday;
            break;

        case SKIN_TOKEN_RTC_DAY_OF_WEEK_START_SUN:
            /* w: day of week (0..6); 0 is Sunday */
            snprintf(buf, buf_size, "%1d", tm->tm_wday);
            numeric_ret = tm->tm_wday + 1;
            break;
        } /* switch */

        if (intval)
        {
            *intval = numeric_ret;
        }
        return numeric_buf;
}

static const char* get_qs_token_value(struct wps_token *token, char *buf, int buf_size)
{
    enum quickscreen_item item;
    bool data_token = true;

    switch(token->type)
    {
        case SKIN_TOKEN_TOP_QUICKSETTING_NAME:
            data_token = false;
            /*fall-through*/
        case SKIN_TOKEN_TOP_QUICKSETTING_VALUE:
            item = QUICKSCREEN_TOP;
            break;

        case SKIN_TOKEN_RIGHT_QUICKSETTING_NAME:
            data_token = false;
            /*fall-through*/
        case SKIN_TOKEN_RIGHT_QUICKSETTING_VALUE:
            item = QUICKSCREEN_RIGHT;
            break;

        case SKIN_TOKEN_BOTTOM_QUICKSETTING_NAME:
            data_token = false;
            /*fall-through*/
        case SKIN_TOKEN_BOTTOM_QUICKSETTING_VALUE:
            item = QUICKSCREEN_BOTTOM;
            break;

        case SKIN_TOKEN_LEFT_QUICKSETTING_NAME:
            data_token = false;
            /*fall-through*/
        case SKIN_TOKEN_LEFT_QUICKSETTING_VALUE:
            item = QUICKSCREEN_LEFT;
            break;

        default:
            return NULL;
    }
    const struct settings_list *qs_setting = global_settings.qs_items[item];

    if (qs_setting == NULL)
        return "ERR";

    if (data_token)
        return option_get_valuestring(qs_setting, buf, buf_size,
                                      option_value_as_int(qs_setting));

    return P2STR(ID2P(qs_setting->lang_id));
}

/* Return the tags value as text. buf should be used as temp storage if needed.

   intval is used with conditionals/enums: when this function is called,
   intval should contain the number of options in the conditional/enum.
   When this function returns, intval is -1 if the tag is non numeric or,
   if the tag is numeric, *intval is the enum case we want to go to (between 1
   and the original value of *intval, inclusive).
   When not treating a conditional/enum, intval should be NULL.
*/
const char *get_token_value(struct gui_wps *gwps,
                           struct wps_token *token, int offset,
                           char *buf, int buf_size,
                           int *intval)
{
    int numeric_ret = -1;
    const char *numeric_buf = "?";

    if (!gwps)
        return NULL;
    if (!token)
        return NULL;

    struct wps_data *data = gwps->data;
    struct wps_state *state = get_wps_state();

    const char *out_text = NULL;

    if (!data || !state)
        return NULL;

    int limit = 1;
    if (intval)
    {
        limit = *intval;
        *intval = -1;
    }

    switch (token->type)
    {
        case SKIN_TOKEN_LOGICAL_IF:
        {
            struct logical_if *lif = SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data);
            return get_lif_token_value(gwps, lif, offset, buf, buf_size);
        }
        break;
        case SKIN_TOKEN_LOGICAL_AND: /*fall-through*/
        case SKIN_TOKEN_LOGICAL_OR:
        {
            int i = 0, truecount = 0;
            char *skinbuffer = get_skin_buffer(data);
            struct skin_element *element =
                    SKINOFFSETTOPTR(skinbuffer, token->value.data);
            if (!element || !element->params) return NULL;
            struct skin_tag_parameter* params =
                    SKINOFFSETTOPTR(skinbuffer, element->params);
            struct skin_tag_parameter* thistag;
            for (i=0; i<element->params_count; i++)
            {
                thistag = &params[i];
                struct skin_element *tokenelement =
                        SKINOFFSETTOPTR(skinbuffer, thistag->data.code);
                if (!tokenelement) return NULL;
                out_text  =  get_token_value(gwps,
                        SKINOFFSETTOPTR(skinbuffer, tokenelement->data),
                        offset, buf, buf_size, intval);
                if (out_text && *out_text)
                    truecount++;
                else if (token->type == SKIN_TOKEN_LOGICAL_AND)
                    return NULL;
            }
            return truecount ? "true" : NULL;
        }
        break;

        case SKIN_TOKEN_TEXT_WIDTH:
        {
            char *skinbuffer = get_skin_buffer(data);
            struct skin_element *el =
                    SKINOFFSETTOPTR(skinbuffer, token->value.data);
            int w = 0, h;
            if (!el || !el->params) return NULL;
            struct skin_tag_parameter *p =
                    SKINOFFSETTOPTR(skinbuffer, el->params);
            /* Measure in an explicit font when %tw(text, fontid) is given,
             * else in the current viewport's font. The explicit form lets a
             * caller measure in a font the current viewport does not use --
             * e.g. deciding a title's layout from $VD, which does not run in
             * the title's font. */
            int fontid = (*gwps->display->current_viewport)->font;
            if (el->params_count >= 2 && p[1].type == INTEGER)
                fontid = p[1].data.number;
            out_text = eval_select_param(gwps, skinbuffer, &p[0], offset,
                                         buf, buf_size);
            if (out_text)
                font_getstringsize(out_text, &w, &h, fontid);
            numeric_ret = w;
            itoa_buf(buf, buf_size, numeric_ret);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        }

        case SKIN_TOKEN_VIEWPORT_WIDTH:
            numeric_ret = (*gwps->display->current_viewport)->width;
            itoa_buf(buf, buf_size, numeric_ret);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;

        case SKIN_TOKEN_VIEWPORT_HEIGHT:
            numeric_ret = (*gwps->display->current_viewport)->height;
            itoa_buf(buf, buf_size, numeric_ret);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;

        case SKIN_TOKEN_SELECT:
            return get_select_token_value(gwps,
                    SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data),
                                          offset, buf, buf_size);

        case SKIN_TOKEN_WORD_WRAP:
            return get_wordwrap_token_value(gwps,
                    SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data),
                                            offset, buf, buf_size);

        case SKIN_TOKEN_MATH:
        {
            char *skinbuffer = get_skin_buffer(data);
            struct skin_element *el =
                    SKINOFFSETTOPTR(skinbuffer, token->value.data);
            if (!el || !el->params) return NULL;
            struct skin_tag_parameter *p =
                    SKINOFFSETTOPTR(skinbuffer, el->params);
            int a = eval_param_int(gwps, skinbuffer, &p[0], offset,
                                   buf, buf_size);
            const char *ops = eval_select_param(gwps, skinbuffer, &p[1],
                                                offset, buf, buf_size);
            char op = ops ? ops[0] : 0;       /* before p[2] reuses buf */
            int b = eval_param_int(gwps, skinbuffer, &p[2], offset,
                                   buf, buf_size);
            switch (op)
            {
                case '+': numeric_ret = a + b; break;
                case '-': numeric_ret = a - b; break;
                case '*': numeric_ret = a * b; break;
                case '/': numeric_ret = b ? a / b : 0; break;
                case '%': numeric_ret = b ? a % b : 0; break;
                default:  numeric_ret = 0; break;
            }
            itoa_buf(buf, buf_size, numeric_ret);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        }

        case SKIN_TOKEN_STRLEN:
        {
            char *skinbuffer = get_skin_buffer(data);
            struct skin_element *el =
                    SKINOFFSETTOPTR(skinbuffer, token->value.data);
            if (!el || !el->params) return NULL;
            struct skin_tag_parameter *p =
                    SKINOFFSETTOPTR(skinbuffer, el->params);
            const char *t = eval_select_param(gwps, skinbuffer, &p[0],
                                              offset, buf, buf_size);
            numeric_ret = t ? (int)utf8length((const unsigned char *)t) : 0;
            itoa_buf(buf, buf_size, numeric_ret);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        }

        case SKIN_TOKEN_STRFIND:
            numeric_ret = get_strfind_value(gwps,
                    SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data),
                                            offset, buf, buf_size);
            itoa_buf(buf, buf_size, numeric_ret);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;

        case SKIN_TOKEN_PAD:
            return get_pad_value(gwps,
                    SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data),
                                   offset, buf, buf_size);

        case SKIN_TOKEN_SUBSTRING:
        {
            struct substring *ss = SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data);
            if (!ss) return NULL;
            const char *token_val = get_token_value(gwps,
                            SKINOFFSETTOPTR(get_skin_buffer(data), ss->token), offset,
                                                    buf, buf_size, intval);
            if (token_val)
            {
                int start_byte, end_byte, byte_len;
                int utf8_len = utf8length(token_val);

                if (utf8_len < ss->start)
                    return NULL;

                if (ss->start < 0)
                    start_byte = utf8seek(token_val, ss->start + utf8_len);
                else
                    start_byte = utf8seek(token_val, ss->start);

                if (ss->length < 0 || (ss->start + ss->length) > utf8_len)
                    end_byte = strlen(token_val);
                else
                    end_byte = utf8seek(token_val, ss->start + ss->length);

                byte_len = end_byte - start_byte;

                if (token_val != buf)
                {
                    if (byte_len >= buf_size) /* the resulting string exceeds buf */
                        return &token_val[start_byte]; /* just return the string */
                    memcpy(buf, &token_val[start_byte], byte_len);
                }
                else
                    buf = &buf[start_byte];

                buf[byte_len] = '\0';
                if (ss->expect_number &&
                    intval && (buf[0] >= '0' && buf[0] <= '9'))
                {

                    numeric_ret = atoi(buf) + 1; /* so 0 is the first item */
                }
                numeric_buf = buf;
                goto gtv_ret_numeric_tag_info;
            }
            return NULL;
        }
        break;

        case SKIN_TOKEN_CHARACTER:
            if (token->value.c == '\n')
                return NULL;
            return &(token->value.c);

        case SKIN_TOKEN_STRING:
            return (char*)SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data);

        case SKIN_TOKEN_TRANSLATEDSTRING:
            return token->value.i < LANG_LAST_INDEX_IN_ARRAY ?
                                   (char*)P2STR(ID2P(token->value.i)) : "<ERR>";

        case SKIN_TOKEN_PLAYLIST_ENTRIES:
            numeric_ret = playlist_amount();
            itoa_buf(buf, buf_size, numeric_ret);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        case SKIN_TOKEN_LIST_TITLE_TEXT:
            return sb_get_title(gwps->display->screen_type);
        case SKIN_TOKEN_LIST_TITLE_ICON:
            numeric_ret = sb_get_icon(gwps->display->screen_type);
            itoa_buf(buf, buf_size, numeric_ret);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        case SKIN_TOKEN_LIST_ITEM_TEXT:
        {
            struct listitem *li = (struct listitem *)SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data);
            if (!li) return NULL;
            return skinlist_get_item_text(li->offset, li->wrap, buf, buf_size);
        }
        case SKIN_TOKEN_LIST_ITEM_ROW:
            numeric_ret = skinlist_get_item_row() + 1;
            itoa_buf(buf, buf_size, numeric_ret);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        case SKIN_TOKEN_LIST_ITEM_COLUMN:
            numeric_ret = skinlist_get_item_column() + 1;
            itoa_buf(buf, buf_size, numeric_ret);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        case SKIN_TOKEN_LIST_ITEM_NUMBER:
            numeric_ret = skinlist_get_item_number() + 1;
            itoa_buf(buf, buf_size, numeric_ret);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        case SKIN_TOKEN_LIST_ITEM_IS_SELECTED:
            return skinlist_is_selected_item()?"s":"";
        case SKIN_TOKEN_LIST_ITEM_ICON:
        {
            struct listitem *li = (struct listitem *)SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data);
            if (!li) return NULL;
            int icon = skinlist_get_item_icon(li->offset, li->wrap);
            numeric_ret = icon;
            itoa_buf(buf, buf_size, numeric_ret);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        }
        case SKIN_TOKEN_LIST_ITEM_ALBUMART:
        {
            /* value context (%?La): is this an album-art row? The draw context
             * for %La is handled separately in skin_render.c. */
            struct listitem *li = (struct listitem *)SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data);
            /* Outside a list row (e.g. status-bar chrome) there is no item to
             * test, so fall back to the browser's list-level flag: true while an
             * album/artist art level is on screen. Lets the theme react to art
             * lists from the SBS, where current_list is not yet the new level. */
            if (!li) return browser_showing_art() ? "a" : "";
            return skinlist_item_is_art_row(gwps->display->screen_type,
                                            li->offset, li->wrap) ? "a" : "";
        }
        case SKIN_TOKEN_LIST_NEEDS_SCROLLBAR:
            return skinlist_needs_scrollbar(gwps->display->screen_type) ? "s" : "";
        case SKIN_TOKEN_PLAYLIST_NAME:
            return playlist_name(NULL, buf, buf_size);

        case SKIN_TOKEN_PLAYLIST_POSITION:
            numeric_ret = playlist_get_display_index()+offset;
            itoa_buf(buf, buf_size, numeric_ret);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;

        case SKIN_TOKEN_PLAYLIST_PERCENT:
        {
            int playlist_amt = playlist_amount();
            int current_pos = playlist_get_display_index() + offset;
            int percentage = current_pos * 100 / playlist_amt;

            if (intval && limit != TOKEN_VALUE_ONLY)
            {
                numeric_ret = current_pos * limit / playlist_amt;
            }
            else
            {
                numeric_ret = percentage;
            }

            itoa_buf(buf, buf_size, percentage);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        }
        case SKIN_TOKEN_PLAYLIST_SHUFFLE:
            if ( global_settings.playlist_shuffle )
                return "s";
            else
                return NULL;
            break;

        case SKIN_TOKEN_VOLUME:
            format_sound_value_ex(buf, buf_size, SOUND_VOLUME,
                                  global_status.volume, true);

            if (intval)
            {
                int minvol = sound_min(SOUND_VOLUME);
                if (limit == TOKEN_VALUE_ONLY)
                {
                    numeric_ret = global_status.volume;
                }
                else if (global_status.volume == minvol)
                {
                    numeric_ret = 1;
                }
                else if (global_status.volume == 0)
                {
                    numeric_ret = limit - 1;
                }
                else if (global_status.volume > 0)
                {
                    numeric_ret = limit;
                }
                else
                {
                    numeric_ret = (limit-3) * (global_status.volume - minvol - 1)
                                / (-1 - minvol) + 2;
                }
            }
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        case SKIN_TOKEN_ALBUMART_FOUND:
            if (SKINOFFSETTOPTR(get_skin_buffer(data), data->albumart))
            {
                int handle = -1;
                handle = playback_current_aa_hid(data->playback_aa_slot);
                if (handle >= 0)
                    return "C";
            }
            return NULL;

        case SKIN_TOKEN_BATTERY_PERCENT:
        {
            int l = battery_level();

            if (intval)
            {
                if (limit == TOKEN_VALUE_ONLY)
                {
                    numeric_ret = l;
                }
                else
                {
                    limit = MAX(limit, 3);
                    if (l > -1) {
                        /* First enum is used for "unknown level",
                         * last enum is used for 100%.
                         */
                        numeric_ret = (limit - 2) * l / 100 + 2;
                    } else {
                        numeric_ret = 1;
                    }
                }
            }

            if (l > -1) {
                itoa_buf(buf, buf_size, l);
                numeric_buf = buf;
                goto gtv_ret_numeric_tag_info;
            } else {
                numeric_buf = "?";
                goto gtv_ret_numeric_tag_info;
            }
        }

        case SKIN_TOKEN_BATTERY_VOLTS:
        {
            int v = battery_voltage();
            if (v >= 0) {
                snprintf(buf, buf_size, "%d.%02d", v / 1000, (v % 1000) / 10);
                return buf;
            } else {
                return "?";
            }
        }

        case SKIN_TOKEN_BATTERY_TIME:
        {
            int t = battery_time();
            if (t >= 0)
                snprintf(buf, buf_size, "%dh %dm", t / 60, t % 60);
            else
                return "?h ?m";
            return buf;
        }

        case SKIN_TOKEN_BATTERY_CHARGER_CONNECTED:
        {
            if(charger_input_state==CHARGER)
                return "p";
            else
                return NULL;
        }
        case SKIN_TOKEN_BATTERY_CHARGING:
        {
            if (charge_state == CHARGING || charge_state == TOPOFF) {
                return "c";
            } else {
                return NULL;
            }
        }
        case SKIN_TOKEN_USB_INSERTED:
            if (usb_inserted())
                return "u";
            return NULL;
        case SKIN_TOKEN_BATTERY_SLEEPTIME:
        {
            if (get_sleep_timer() == 0)
                return NULL;
            else
            {
                format_time(buf, buf_size, get_sleep_timer() * 1000);
                return buf;
            }
        }

        case SKIN_TOKEN_PLAYBACK_STATUS:
        {
            int status = current_playmode();
            switch (status) {
            case STATUS_STOP:
                numeric_ret = 1;
                break;
            case STATUS_PLAY:
                numeric_ret = 2;
                break;
            default:
                numeric_ret = status + 1;
                break;
            }

            itoa_buf(buf, buf_size, numeric_ret-1);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        }

        case SKIN_TOKEN_REPEAT_MODE:
            itoa_buf(buf, buf_size, global_settings.repeat_mode);
            numeric_ret = global_settings.repeat_mode + 1;
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;

        case SKIN_TOKEN_RTC_PRESENT:
            return "c";
        /* peakmeter */
        case SKIN_TOKEN_PEAKMETER_LEFT:
        case SKIN_TOKEN_PEAKMETER_RIGHT:
        {
            int left, right, val;
            peak_meter_current_vals(&left, &right);
            val = token->type == SKIN_TOKEN_PEAKMETER_LEFT ?
                    left : right;
            val = peak_meter_scale_value(val, limit==1 ? MAX_PEAK : limit);
            numeric_ret = val;
            itoa_buf(buf, buf_size, numeric_ret);
            data->peak_meter_enabled = true;
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        }

        case SKIN_TOKEN_CROSSFADE:
            itoa_buf(buf, buf_size, global_settings.crossfade);
            numeric_ret = global_settings.crossfade + 1;
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;

        case SKIN_TOKEN_SOUND_PITCH:
        {
            int32_t pitch = sound_get_pitch();
            snprintf(buf, buf_size, "%ld.%ld",
                     pitch / PITCH_SPEED_PRECISION,
             (pitch  % PITCH_SPEED_PRECISION) / (PITCH_SPEED_PRECISION / 10));
            if (intval)
                numeric_ret = pitch_speed_enum(limit, pitch, PITCH_SPEED_PRECISION * 100);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        }

    case SKIN_TOKEN_SOUND_SPEED:
    {
        int32_t pitch = sound_get_pitch();
        int32_t speed;
        if (dsp_timestretch_available())
            speed = GET_SPEED(pitch, dsp_get_timestretch());
        else
            speed = pitch;
        snprintf(buf, buf_size, "%ld.%ld",
                     speed / PITCH_SPEED_PRECISION,
             (speed  % PITCH_SPEED_PRECISION) / (PITCH_SPEED_PRECISION / 10));
        if (intval)
            numeric_ret = pitch_speed_enum(limit, speed, PITCH_SPEED_PRECISION * 100);
        numeric_buf = buf;
        goto gtv_ret_numeric_tag_info;
    }

        case SKIN_TOKEN_MAIN_HOLD:
            if (button_hold())
                return "h";
            else
                return NULL;


        case SKIN_TOKEN_VLED_HDD:
            if(led_read(HZ/2))
                return "h";
            else
                return NULL;
        case SKIN_TOKEN_VLED_BUILDING:
        {
            /* database, album-index and/or thumbnail-cache background work in
             * progress. All three are passes the user did not ask for and
             * cannot see, and any of them can be the reason a screen that
             * needs the database is slow to open -- so they share one
             * indicator rather than each having their own. */
            bool building = false;
            if (tagcache_is_busy())
                building = true;
            if (db_summary_is_busy())
                building = true;
            if (art_cache_is_busy())
                building = true;
            return building ? "b" : NULL;
        }
        case SKIN_TOKEN_VLED_WORKING:
            /* generic busy flag for other long-running work; set via
             * ui_set_working() -- no built-in driver yet */
            return ui_working() ? "w" : NULL;
        case SKIN_TOKEN_LOADING_ANIM:
            /* Time-cycling frame index for an animated "busy" spinner. A theme
             * conditional (%?la<f0|f1|...|fN>) maps it to N glyphs; the value
             * changes ~10x/s so the spinner advances via ordinary value-change
             * refreshes, with no reliance on skin sub-image timers. The frame
             * count follows however many options the theme provides. Only ever
             * drawn while the busy notification viewport is visible. */
            if (intval)
            {
                int frames = (limit >= 2) ? limit : 2;
                *intval = (int)((current_tick / (HZ / 10)) % frames) + 1;
            }
            return "a";
        case SKIN_TOKEN_BUTTON_VOLUME:
            if (global_status.last_volume_change &&
                TIME_BEFORE(current_tick, global_status.last_volume_change +
                                          token->value.i))
                return "v";
            return NULL;
        case SKIN_TOKEN_LASTTOUCH:
            {
            }
            return NULL;
        case SKIN_TOKEN_HAVE_TOUCH:
            return NULL;

        case SKIN_TOKEN_TOP_QUICKSETTING_NAME:
        case SKIN_TOKEN_TOP_QUICKSETTING_VALUE:
        case SKIN_TOKEN_RIGHT_QUICKSETTING_NAME:
        case SKIN_TOKEN_RIGHT_QUICKSETTING_VALUE:
        case SKIN_TOKEN_BOTTOM_QUICKSETTING_NAME:
        case SKIN_TOKEN_BOTTOM_QUICKSETTING_VALUE:
        case SKIN_TOKEN_LEFT_QUICKSETTING_NAME:
        case SKIN_TOKEN_LEFT_QUICKSETTING_VALUE:
            return get_qs_token_value(token, buf, buf_size);

        case SKIN_TOKEN_SETTING:
        {
            const struct settings_list *s = token->value.xdata;
            if (intval)
            {
                /* Handle contionals */
                switch (s->flags&F_T_MASK)
                {
                    case F_T_INT:
                    case F_T_UINT:
                        if (s->flags&F_T_SOUND)
                        {
                            /* %?St|name|<min|min+1|...|max-1|max> */
                            int sound_setting = s->sound_setting->setting;
                            /* settings with decimals can't be used in conditionals */
                            if (sound_numdecimals(sound_setting) == 0)
                            {
                                numeric_ret = (*(int*)s->setting-sound_min(sound_setting))
                                      /sound_steps(sound_setting) + 1;
                            }
                            else
                                numeric_ret = -1;
                        }
                        else if (s->flags&F_RGB)
                            /* %?St|name|<#000000|#000001|...|#FFFFFF> */
                            /* shouldn't overflow since colors are stored
                             * on 16 bits ...
                             * but this is pretty useless anyway */
                            numeric_ret = *(int*)s->setting + 1;
                        else if (setting_get_cfgvals(s) == NULL)
                            /* %?St|name|<1st choice|2nd choice|...> */
                            numeric_ret = (*(int*)s->setting-s->int_setting->min)
                                      /s->int_setting->step + 1;
                        else
                            /* %?St|name|<1st choice|2nd choice|...> */
                            /* Not sure about this one. cfg_name/vals are
                             * indexed from 0 right? */
                            numeric_ret = *(int*)s->setting + 1;
                        break;
                    case F_T_BOOL:
                        /* %?St|name|<if true|if false> */
                        numeric_ret = *(bool*)s->setting?1:2;
                        break;
                    case F_T_CHARPTR:
                    case F_T_UCHARPTR:
                        /* %?St|name|<if non empty string|if empty>
                         * The string's emptyness discards the setting's
                         * prefix and suffix */
                        numeric_ret = ((char*)s->setting)[0]?1:2;
                        /* if there is a prefix we should ignore it here */
                        if (s->filename_setting->prefix)
                        {
                            numeric_buf = (char*)s->setting;
                            goto gtv_ret_numeric_tag_info;
                        }
                        break;
                    default:
                        /* This shouldn't happen ... but you never know */
                        numeric_ret = -1;
                        break;
                }
            }

            /* Special handlng for filenames because we dont want to show the prefix */
            if ((s->flags&F_T_MASK) == F_T_CHARPTR ||
                (s->flags&F_T_MASK) == F_T_UCHARPTR)
            {
                if (s->filename_setting->prefix)
                {
                    numeric_buf = (char*)s->setting;
                    goto gtv_ret_numeric_tag_info;
                }
            }
            cfg_to_string(s, buf, buf_size);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        }
        case SKIN_TOKEN_HAVE_TUNER:
            return NULL;
        case SKIN_TOKEN_HAVE_RECORDING:
            return NULL;


        case SKIN_TOKEN_CURRENT_SCREEN:
        {
            int curr_screen = get_current_activity();
            numeric_ret = curr_screen;
            itoa_buf(buf, buf_size, numeric_ret);
            numeric_buf = buf;
            goto gtv_ret_numeric_tag_info;
        }

        case SKIN_TOKEN_LANG_IS_RTL:
            return lang_is_rtl() ? "r" : NULL;

        case SKIN_TOKEN_VAR_GETVAL:
        {
            struct skin_var *var =
                SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data);
            if (intval)
                *intval = var->value;
            itoa_buf(buf, buf_size, var->value);
            return buf;
        }
        case SKIN_TOKEN_VAR_TIMEOUT:
        {
            struct skin_var_lastchange *lc =
                SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data);
            struct skin_var *var = SKINOFFSETTOPTR(get_skin_buffer(data), lc->var);

            /* 0xffff is the "never set" marker find_or_add_var() writes */
            if (var->last_changed != 0xffff &&
                TIME_BEFORE(current_tick, lc->timeout + var->last_changed))
                return "t";
            return NULL;
        }

        default:
        {
            /* if the token is an RTC one, update the time
               and do the necessary checks */
            if (token->type >= SKIN_TOKENS_RTC_BEGIN
                && token->type <= SKIN_TOKENS_RTC_END)
            {
                return get_rtc_token_value(token, buf, buf_size, intval);
            }
            /* Anything left must use the current id3 if not found returns NULL */
            return try_id3_token(token, offset, buf, buf_size, limit, intval);
        }
    } /* switch */

gtv_ret_numeric_tag_info:
    if (intval)
    {
        *intval = numeric_ret;
    }
    return numeric_buf;
}
