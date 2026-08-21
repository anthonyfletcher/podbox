/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/lib/mul_id3.c
 * Copyright (C) 2023 Christian Soffke
 *
 * Ported from apps/plugins/lib/mul_id3.c to the core; used only by the
 * core Properties screen (viewers/properties.c).
 * GNU General Public License (version 2+)
 *
 * Aggregates tags across many files: walks a directory collecting counts,
 * sizes and durations for the properties screen.
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "system.h"          /* ARRAYLEN */
#include "kernel.h"          /* current_tick, yield, HZ, TIME_AFTER */
#include "lang.h"            /* LANG_*, str */
#include "input/action.h"          /* get_action */
#include "system/shutdown.h" /* default_event_handler */
#include "widgets/splash.h"          /* splashf, splash_progress */
#include "dir.h"             /* opendir/readdir/closedir, dir_get_info */
#include "files/filetypes.h"       /* filetype_get_attr, FILE_ATTR_* */
#include "draw/viewport.h"        /* viewport_set_defaults */
#include "draw/screen_access.h"   /* screens[], FOR_NB_SCREENS */
#include "mul_id3.h"

struct multiple_tracks_id3 {
    unsigned long long filesize;
    unsigned long long length;
    unsigned long frequency;
    unsigned int artist_hash;
    unsigned int composer_hash;
    unsigned int albumartist_hash;
    unsigned int grouping_hash;
    unsigned int comment_hash;
    unsigned int album_hash;
    unsigned int genre_hash;
    unsigned int codectype;
    unsigned int bitrate;
    int year;
    bool vbr;
};

static struct multiple_tracks_id3 mul_id3;

static const int32_t units[] =
{
    LANG_BYTE,
    LANG_KIBIBYTE,
    LANG_MEBIBYTE,
    LANG_GIBIBYTE
};

/* Calculate modified FNV hash of string
 * has good avalanche behaviour and uniform distribution
 * see http://home.comcast.net/~bretm/hash/ */
static unsigned int mfnv(char *str)
{
    const unsigned int p = 16777619;
    unsigned int hash = 0x811C9DC5; /* 2166136261 */

    if (!str)
        return 0;

    while(*str)
        hash = (hash ^ *str++) * p;
    hash += hash << 13;
    hash ^= hash >> 7;
    hash += hash << 3;
    hash ^= hash >> 17;
    hash += hash << 5;
    return hash;
}

static void init_mul_id3(void)
{
    mul_id3.artist_hash = 0;
    mul_id3.album_hash = 0;
    mul_id3.genre_hash = 0;
    mul_id3.composer_hash = 0;
    mul_id3.albumartist_hash = 0;
    mul_id3.grouping_hash = 0;
    mul_id3.comment_hash = 0;
    mul_id3.codectype = 0;
    mul_id3.vbr = false;
    mul_id3.bitrate = 0;
    mul_id3.frequency = 0;
    mul_id3.length = 0;
    mul_id3.filesize = 0;
    mul_id3.year = 0;
}

void collect_id3(struct mp3entry *id3, bool is_first_track)
{
    if (is_first_track)
    {
        init_mul_id3();
        mul_id3.artist_hash = mfnv(id3->artist);
        mul_id3.album_hash = mfnv(id3->album);
        mul_id3.genre_hash = mfnv(id3->genre_string);
        mul_id3.composer_hash = mfnv(id3->composer);
        mul_id3.albumartist_hash = mfnv(id3->albumartist);
        mul_id3.grouping_hash = mfnv(id3->grouping);
        mul_id3.comment_hash = mfnv(id3->comment);
        mul_id3.codectype = id3->codectype;
        mul_id3.vbr = id3->vbr;
        mul_id3.bitrate = id3->bitrate;
        mul_id3.frequency = id3->frequency;
        mul_id3.year = id3->year;
    }
    else
    {
        if (mul_id3.artist_hash && (mfnv(id3->artist) != mul_id3.artist_hash))
            mul_id3.artist_hash = 0;
        if (mul_id3.album_hash && (mfnv(id3->album) != mul_id3.album_hash))
            mul_id3.album_hash = 0;
        if (mul_id3.genre_hash && (mfnv(id3->genre_string) != mul_id3.genre_hash))
            mul_id3.genre_hash = 0;
        if (mul_id3.composer_hash && (mfnv(id3->composer) != mul_id3.composer_hash))
            mul_id3.composer_hash = 0;
        if (mul_id3.albumartist_hash && (mfnv(id3->albumartist) !=
                                          mul_id3.albumartist_hash))
            mul_id3.albumartist_hash = 0;
        if (mul_id3.grouping_hash && (mfnv(id3->grouping) != mul_id3.grouping_hash))
            mul_id3.grouping_hash = 0;
        if (mul_id3.comment_hash && (mfnv(id3->comment) != mul_id3.comment_hash))
            mul_id3.comment_hash = 0;

        if (mul_id3.codectype && (id3->codectype != mul_id3.codectype))
            mul_id3.codectype = AFMT_UNKNOWN;
        if (mul_id3.bitrate && (id3->bitrate != mul_id3.bitrate ||
                                 id3->vbr != mul_id3.vbr))
            mul_id3.bitrate = 0;
        if (mul_id3.frequency && (id3->frequency != mul_id3.frequency))
            mul_id3.frequency = 0;
        if (mul_id3.year && (id3->year != mul_id3.year))
            mul_id3.year = 0;
    }
    mul_id3.length += id3->length;
    mul_id3.filesize += id3->filesize;
}

/* (!) Note unit conversion below
 *
 *     Use result only as input for browse_id3,
 *     with the track_ct parameter set to  > 1.
 */
void finalize_id3(struct mp3entry *id3)
{
    id3->path[0] = '\0';
    id3->title = NULL;
    if (!mul_id3.artist_hash)
        id3->artist = NULL;
    if (!mul_id3.album_hash)
        id3->album = NULL;
    if (!mul_id3.genre_hash)
        id3->genre_string = NULL;
    if (!mul_id3.composer_hash)
        id3->composer = NULL;
    if (!mul_id3.albumartist_hash)
        id3->albumartist = NULL;
    if (!mul_id3.grouping_hash)
        id3->grouping = NULL;
    if (!mul_id3.comment_hash)
        id3->comment = NULL;
    id3->disc_string = NULL;
    id3->track_string = NULL;
    id3->year_string = NULL;
    id3->year = mul_id3.year;
    mul_id3.length /= 1000;  /* convert from ms to s */
    mul_id3.filesize >>= 10; /* convert from B to KiB */
    id3->length = mul_id3.length > ULONG_MAX ? 0 : mul_id3.length;
    id3->filesize = mul_id3.filesize > INT_MAX ? 0 : mul_id3.filesize;
    id3->frequency = mul_id3.frequency;
    id3->bitrate = mul_id3.bitrate;
    id3->codectype = mul_id3.codectype;
    id3->vbr = mul_id3.vbr;
    id3->discnum = 0;
    id3->tracknum = -1;
    id3->track_level = 0;
    id3->album_level = 0;
}

unsigned long human_size(unsigned long long byte_count, int32_t *unit_lang_id)
{
    const size_t n = sizeof(units)/sizeof(units[0]);
    unsigned int i;

    /* margin set at 10K boundary: 10239 B +1 => 10 KB */
    for(i = 0; i < n-1 && byte_count >= 10*1024; i++)
        byte_count >>= 10; /* div by 1024 */

    *unit_lang_id = units[i];
    return (unsigned long)byte_count;
}

/* missing filetype attribute for images */
static const char *image_exts[] = {"bmp","jpg","jpe","jpeg","png","ppm"};
/* and videos */
static const char *video_exts[] = {"mpg","mpeg","mpv","m2v"};

/* Progress/throttle state for collect_dir_stats(). File-scoped so a fresh
 * top-level scan can zero it: as a plugin these were function statics reset by
 * BSS on each launch, but the core caller persists across invocations. */
static unsigned int cds_id3_count;
static unsigned long cds_last_displayed, cds_last_get_action;

void collect_dir_stats_reset(void)
{
    cds_id3_count = 0;
    cds_last_displayed = 0;
    cds_last_get_action = 0;
}

static void display_dir_stats_vp(struct dir_stats *stats, struct viewport *vp,
                                 struct screen *display)
{
    int32_t lang_size_unit;
    unsigned long display_size = human_size(stats->byte_count, &lang_size_unit);
    struct viewport *last_vp = display->set_viewport(vp);
    display->clear_viewport();
    display->putsf(0, 0, "Files: %d (%lu %s)", stats->file_count,
                   display_size, str(lang_size_unit));
    display->putsf(0, 1, "Audio: %d", stats->audio_file_count);
    if (stats->count_all)
    {
        display->putsf(0, 2, "Playlists: %d", stats->m3u_file_count);
        display->putsf(0, 3, "Images: %d", stats->img_file_count);
        display->putsf(0, 4, "Videos: %d", stats->vid_file_count);
        display->putsf(0, 5, "Directories: %d", stats->dir_count);
        display->putsf(0, 6, "Max files in Dir: %d", stats->max_files_in_dir);
    }
    else
        display->putsf(0, 2, "Directories: %d", stats->dir_count);

    display->update_viewport();
    display->set_viewport(last_vp);
}

void display_dir_stats(struct dir_stats *stats)
{
    struct viewport vps[NB_SCREENS];
    FOR_NB_SCREENS(i)
    {
        viewport_set_defaults(&vps[i], i);
        display_dir_stats_vp(stats, &vps[i], &screens[i]);
    }
}

/* Recursively scans directories in search of files
 * and informs the user of the progress.
 */
bool collect_dir_stats(struct dir_stats *stats, bool (*id3_cb)(const char*))
{
    bool result = true;
    unsigned int files_in_dir = 0;
    struct dirent* entry;
    int dirlen = strlen(stats->dirname);
    DIR* dir =  opendir(stats->dirname);
    if (!dir)
    {
        splashf(HZ * 3, "Could not open %s", stats->dirname);
        return false;
    }
    else if (!stats->dirname[1]) /* root dir */
        stats->dirname[0] = dirlen = 0;

    /* walk through the directory content */
    while(result && (0 != (entry = readdir(dir))))
    {
        struct dirinfo info = dir_get_info(dir, entry);
        if (info.attribute & ATTR_DIRECTORY)
        {
            if (!strcmp((char *)entry->d_name, ".") ||
                !strcmp((char *)entry->d_name, ".."))
                continue; /* skip these */

            snprintf(stats->dirname + dirlen, sizeof(stats->dirname) - dirlen,
                     "/%s", entry->d_name); /* append name to current directory */
            if (!id3_cb)
            {
                stats->dir_count++; /* new directory */
                if (current_tick - cds_last_displayed > (HZ/2))
                {
                    if (cds_last_displayed)
                        display_dir_stats(stats);
                    cds_last_displayed = current_tick;
                }
            }
            result = collect_dir_stats(stats, id3_cb); /* recursion */
        }
        else if (!id3_cb)
        {
            char *ptr;
            stats->file_count++; /* new file */
            files_in_dir++;
            stats->byte_count += info.size;

            int attr = filetype_get_attr(entry->d_name);
            if (attr == FILE_ATTR_AUDIO)
                stats->audio_file_count++;
            else if (attr == FILE_ATTR_M3U)
                stats->m3u_file_count++;
            /* image or video file attributes have to be compared manually */
            else if (stats->count_all &&
                     (ptr = strrchr(entry->d_name,'.')))
            {
                unsigned int i;
                ptr++;
                for(i = 0; i < ARRAYLEN(image_exts); i++)
                {
                    if(!strcasecmp(ptr, image_exts[i]))
                    {
                        stats->img_file_count++;
                        break;
                    }
                }
                if (i >= ARRAYLEN(image_exts)) {
                    for(i = 0; i < ARRAYLEN(video_exts); i++) {
                        if(!strcasecmp(ptr, video_exts[i])) {
                            stats->vid_file_count++;
                            break;
                        }
                    }
                }
            }
        }
        else if (filetype_get_attr(entry->d_name) == FILE_ATTR_AUDIO)
        {
            splash_progress(cds_id3_count++, stats->audio_file_count,
                            "%s (%s)",
                            str(LANG_WAIT), str(LANG_OFF_ABORT));
            snprintf(stats->dirname + dirlen, sizeof(stats->dirname) - dirlen,
                     "/%s", entry->d_name); /* append name to current directory */
            id3_cb(stats->dirname); /* allow metadata to be collected */
        }

        if (TIME_AFTER(current_tick, cds_last_get_action + HZ/8))
        {
            int action = get_action(CONTEXT_STD,TIMEOUT_NOBLOCK);

            /* USB has to be handled here rather than dropped: this poll owns
             * the button queue while the scan runs, and the storage handover
             * waits on the acknowledgement default_event_handler() sends. */
            if (default_event_handler(action) == SYS_USB_CONNECTED ||
                action == ACTION_STD_CANCEL)
            {
                stats->canceled = true;
                result = false;
            }
            cds_last_get_action = current_tick;
        }
        yield();
    }
    closedir(dir);
    if (stats->max_files_in_dir < files_in_dir)
        stats->max_files_in_dir = files_in_dir;
    return result;
}
