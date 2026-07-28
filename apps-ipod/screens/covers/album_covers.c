/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/pictureflow/pictureflow.c
 * Copyright (C) 2007 Jonas Hurrelmann (j@outpo.st)
 * Copyright (C) 2007 Nicolas Pennequin
 * Copyright (C) 2007 Ariya Hidayat (ariya@kde.org) (original Qt Version)
 * GNU General Public License (version 2+)
 *
 * Original code: http://code.google.com/p/pictureflow/
 *
 * Cover-flow album browser built on carousel.c. Supplies the slide model
 * -- album list, art loading and its disk cache, sort order -- and what
 * happens on select.
 *
 * carousel.c owns the rendering and the input loop; this file only answers
 * its questions. The link is album_model at the bottom, a struct of function
 * pointers the carousel calls to ask how many slides there are, what a slide
 * looks like, and what to do when one is chosen. artist_portraits.c is the
 * other implementation of the same model.
 *
 * The album list is built once by scanning tagcache and written to an index
 * file, so subsequent opens read the index instead of rescanning. Slide
 * images are cached to disk as .pfraw (pre-scaled raw pixels) for the same
 * reason -- decoding album art per frame would be far too slow.
 *
 * Parts, in order:
 *   - the album index: building it from tagcache, sorting, and the on-disk form
 *   - locating cover art for an album, and rendering it into the slide cache
 *   - the text drawn under the slides
 *   - the model callbacks: counts, names, sort order, select and menu
 *   - album_model itself, and album_covers() which runs the carousel with it
 ****************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"          /* MIN/MAX/ALIGN_BUFFER/ALIGN_DOWN */
#include "rbpaths.h"
#include "input/action.h"           /* get_action/get_custom_action/button_mapping */
#include "draw/screen_access.h"    /* FOR_NB_SCREENS, screens[] */
#include "kernel.h"           /* threads, mutex, queue, current_tick */
#include "core_alloc.h"       /* buflib types for buf_ctx (see init()) */
#include "database/tagcache.h"
#include "playlist/playlist.h"
#include "playlist/catalog.h"
#include "settings/settings.h"
#include "lang.h"
#include "widgets/splash.h"
#include "draw/viewport.h"
#include "system/activity.h"
#include "system/app_util.h"
#include "system/shutdown.h"
#include "system/strutil.h"
#include "screens/context_menu.h"           /* context_menu_show_playlist_cat/menu */
#include "metadata/albumart.h"         /* find_albumart / search_albumart_files */
#include "metadata/art_cache.h"   /* shared database-driven thumbnail cache */
#include "metadata.h"         /* struct mp3entry, get_metadata */
#include "dir.h"
#include "file.h"
#include "widgets/yesno.h"            /* gui_syncyesno_run */
#include "root_menu.h"
#include "screens/browse/browser.h"
#include "screens/browse/browser_db.h"
#include "screens/playback/track_info.h"          /* browse_id3 */
#include "audio.h"            /* audio_status, audio_current_track */
#include "lcd.h"
#include "font.h"
#include "draw/icon_bitmaps.h"
#include "widgets/menu.h"             /* do_menu */
#include "screens/settings/exported_settings.h" /* album_covers_menu (shared settings menu) */
#include "draw/bmp.h"              /* read_bmp_file */
#include "draw/jpeg_load.h"        /* read_jpeg_file */
#include "power.h"
#include "powermgmt.h"        /* reset_poweroff_timer */
#include "backlight.h"        /* backlight_set_timeout(_plugged) */
#include "cpu.h"
#include "skin/skin_engine.h"  /* skin_inhibit_flush */
#include "skin/skin_albumart_color.h" /* dynamic_colors_resolve */
#include "skin/statusbar_skinned.h" /* sb_set_persistent_title */
#include "album_covers.h"
#include "carousel.h"     /* shared carousel engine interface (pf_idx, model, ...) */

/** Globals **/

/* The engine's shared constants -- PFreal, the slide cache size and the
 * on-disk cache paths -- come from carousel.h. Only what is specific to the
 * album model is defined here. */

/* Not theme-controlled: layout is fixed/proportional (see init()) and
 * colours come from the theme's normal fg/bg + the dynamic (album-art
 * derived) colour scheme, same as everywhere else.
 *
 * It cannot be themed further while it draws the way it does. This screen
 * repaints its own pixels every frame, straight to the framebuffer, outside
 * the SBS's render cycle -- so anything routed through the skin engine at
 * runtime fights it: colours arrive inherited from ambient, SBS viewport
 * state is non-deterministic, skin_update()-rendered text races this
 * screen's own lcd_update(), and %Vd()'d content left "shown" by whatever
 * screen preceded this one bleeds through. Themeable layout means giving up
 * the per-frame raw redraw first. */
/* The album index file, and the four-byte magic at its start. */
/* Holds the artist name blob as well as the albums -- artist_portraits.c
 * reads pf_idx.artist_names/artist_index straight out of it -- so the name
 * is deliberately not album-specific. Regenerated if absent. */
#define ALBUM_INDEX CACHE_PREFIX "/carousel.idx"
#define INDEX_HDR "PFID"

/** structs we use */
struct albumart_t {
    struct bitmap input_bmp;
    char pfraw_file[MAX_PATH];
    char file[MAX_PATH];
    int idx;
    int slides;
    int inspected;
    void * buf;
    size_t buf_sz;
};

struct slide_data {
    int slide_index;
    int angle;
    PFreal cx;
    PFreal cy;
    PFreal distance;
    int cached_slot; /* last cache slot this slide resolved to (see surface()) */
};

struct slide_cache {
    int index;      /* index of the cached slide */
    int hid;        /* handle ID of the cached slide */
    short next; /* "next" slide, with LRU last */
    short prev; /* "previous" slide */
};

struct rect {
    int left;
    int right;
    int top;
    int bottom;
};

struct load_slide_event_data {
    int slide_index;
    int cache_index;
};

struct pf_slide_cache
{
    struct slide_cache cache[SLIDE_CACHE_SIZE];
    int free;
    int used;
    int left_idx;
    int right_idx;
    int center_idx;
};

struct pf_scroll_line_info {
    long ticks;         /* number of ticks between each move */
    long delay;         /* number of ticks to delay starting scrolling */
    int step;           /* pixels to move */
    long next_scroll;   /* tick of the next move */
};

struct pf_scroll_line {
    int width;          /* width of the string */
    int offset;         /* x coordinate of the string */
    int step;           /* 0 if scroll is disabled. otherwise, pixels to move */
    long start_tick;    /* tick when to start scrolling */
};

struct pfraw_header {
    int32_t width;          /* bmap width in pixels */
    int32_t height;         /* bmap height in pixels */
};

static struct tagcache_search tcs;

/* Scratch id3 used to resolve the currently playing/selected track into an
 * album index (id3_get_index()) and to look up album art (retrieve_id3()).
 */
static struct mp3entry id3;

enum ePFS{ePFS_ARTIST = 0, ePFS_ALBUM};

/*
 * States: pf_idle <-> pf_scrolling (browsing covers); SELECT on a cover
 * jumps straight into the core database's track list for that album (see
 * browser_db_enter_album_tracks_on_next_load(), called from
 * album_covers_loop()) instead of an in-house zoom animation and track-list
 * screen -- there are no separate cover-zoom/track-list browsing states
 * anymore, unlike the original plugin.
 */
enum pf_states {
    pf_idle = 0,
    pf_scrolling
};
static void draw_progressbar(int step, int count, char *msg);

/* struct carousel_model (the engine<->data seam) is declared in carousel.h. */

static int  album_build_index(void);
static int  album_count(void);
static bool get_slide_dir(const int slide_index, char *dir, int dirlen);
static bool album_legacy_art(int index, char *path, int len);
static int  album_enter(int index);
static int  jmp_idx_prev(void);
static int  jmp_idx_next(void);
static void draw_album_text(void);
static void album_sort_next(void);
static void album_sort_prev(void);
static void set_initial_slide(const char *selected_file);
static int  album_on_menu(void);
static void album_prepare(void);

static const struct carousel_model album_model = {
    .build_index = album_build_index,
    .count       = album_count,
    .slide_art   = get_slide_dir,
    .legacy_art  = album_legacy_art,
    .enter       = album_enter,
    .jump_prev   = jmp_idx_prev,
    .jump_next   = jmp_idx_next,
    .draw_text   = draw_album_text,
    .sort_next   = album_sort_next,
    .sort_prev   = album_sort_prev,
    .set_initial = set_initial_slide,
    .on_menu     = album_on_menu,
    .prepare     = album_prepare,
    .has_pfraw_cache = true,
    .title       = "Album Covers",
};

/* Minimal stand-in for the plugin-only apps/plugins/lib/configfile.c (which
 * can't be reused as-is -- it derives its file path via
 * plugin_get_current_filename(), a genuinely plugin-lifecycle-only call).
 * This is deliberately bare-bones (a single fixed path, no version-gated
 * migration) since it's a transitional bridge -- real settings persistence
 * moves to global_settings/settings_list.c shortly after this lands. Reuses
 * the exact same line-parsing core functions (read_line/settings_parseline,
 * apps/misc.h) the plugin-lib version itself wraps via the plugin API's
 * function-pointer indirection. */

struct configdata
{
    int type;
    int min;
    int max;    /* enum: number of values */
    union {
        int *int_p;
        bool *bool_p;
    };
    const char *name;
    const char * const *values; /* enum only */
};


static bool progress_cancel(int step, int count, char *msg)
{
    const struct text_message prompt = {
            (const char*[]) {"Quit?", "Progress will be lost"}, 2};

    int action = get_action(CONTEXT_STD,TIMEOUT_NOBLOCK);
    if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
    {
        if (gui_syncyesno_run(&prompt, NULL, NULL) == YESNO_YES)
            return true;
        lcd_clear_display();
    }

    if (count)
        draw_progressbar(step, count, msg);

    return false;
}

static int compare_albums (const void *a_v, const void *b_v)
{
    uint32_t artist_a = ((struct album_data *)a_v)->artist_idx;
    uint32_t artist_b = ((struct album_data *)b_v)->artist_idx;

    uint32_t album_a = ((struct album_data *)a_v)->name_idx;
    uint32_t album_b = ((struct album_data *)b_v)->name_idx;

    int year_a = ((struct album_data *)a_v)->year;
    int year_b = ((struct album_data *)b_v)->year;

    switch (global_settings.album_covers_sort_albums_by)
    {
        case SORT_BY_ARTIST_AND_NAME:
            if (artist_a - artist_b == 0)
                return (int)(album_a - album_b);
            break;
        case SORT_BY_ARTIST_AND_YEAR:
            if (artist_a - artist_b == 0)
            {
                if (global_settings.album_covers_year_sort_order == ASCENDING)
                    return year_a - year_b;
                else
                    return year_b - year_a;
            }
            break;
        case SORT_BY_YEAR:
            if (year_a - year_b != 0)
            {
                if (global_settings.album_covers_year_sort_order == ASCENDING)
                    return year_a - year_b;
                else
                    return year_b - year_a;
            }
            break;
        case SORT_BY_NAME:
            if (album_a - album_b != 0)
                return (int)(album_a - album_b);
            break;
    }

    return (int)(artist_a - artist_b);
}

static int compare_album_artists (const void *a_v, const void *b_v)
{
    uint32_t a = ((struct album_data *)a_v)->artist_idx;
    uint32_t b = ((struct album_data *)b_v)->artist_idx;
    return (int)(a - b);
}

static void write_album_index(int idx, int name_idx,
                              long album_seek, int artist_idx, long artist_seek)
{
    pf_idx.album_index[idx].name_idx = name_idx;
    pf_idx.album_index[idx].seek = album_seek;
    pf_idx.album_index[idx].artist_idx = artist_idx;
    pf_idx.album_index[idx].artist_seek = artist_seek;
    pf_idx.album_index[idx].year = 0;
}

static inline void write_album_entry(struct tagcache_search *tcs,
                                     int name_idx, unsigned int len)
{
    write_album_index(-pf_idx.album_ct, name_idx, tcs->result_seek, 0, -1);
    pf_idx.album_len += len;
    pf_idx.album_ct++;

    if (pf_idx.album_untagged_seek == -1 && strcmp(UNTAGGED, tcs->result) == 0)
    {
        pf_idx.album_untagged_idx = name_idx;
        pf_idx.album_untagged_seek = tcs->result_seek;
    }
}

static void write_artist_entry(struct tagcache_search *tcs,
                               int name_idx, unsigned int len)
{
    pf_idx.artist_index[-pf_idx.artist_ct].name_idx = name_idx;
    pf_idx.artist_index[-pf_idx.artist_ct].seek = tcs->result_seek;
    pf_idx.artist_len += len;
    pf_idx.artist_ct++;
}

/* adds tagcache_search results into artist/album index */
static int get_tcs_search_res(int type, struct tagcache_search *tcs,
                              void **buf, size_t *bufsz)
{
    char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    int ret = SUCCESS;
    unsigned int l, name_idx = 0;
    void (*writefn)(struct tagcache_search *, int, unsigned int);
    int data_size;
    if (type == ePFS_ARTIST)
    {
        writefn = &write_artist_entry;
        data_size = sizeof(struct artist_data);
    }
    else
    {
        writefn = &write_album_entry;
        data_size = sizeof(struct album_data);
    }

    while (tagcache_get_next(tcs, tcs_buf, tcs_bufsz))
    {
        if (progress_cancel(0, 0, NULL))
        {
            ret = ERROR_USER_ABORT;
            break;
        }

        l = tcs->result_len;

        /* Check before subtracting -- *bufsz is unsigned, so subtracting
         * data_size (or l) once the real remaining space is smaller than
         * that would wrap to a huge value instead of going negative,
         * permanently defeating this check for the rest of the scan and
         * letting strcpy()/writefn() below walk past the end of
         * pf_idx.buf on any library large enough to fill it. */
        if ((size_t)data_size > *bufsz || l > *bufsz - data_size)
        {
            /* not enough memory */
            ret = ERROR_BUFFER_FULL;
            break;
        }

        *bufsz -= data_size;

        strcpy(*buf, tcs->result);

        *bufsz -= l;
        *buf = l + (char *)*buf;

        writefn(tcs, name_idx, l);

        name_idx += l;
    }
    tagcache_search_finish(tcs);
    return ret;
}

#define STR_STEP_INDEXING_UNTAGGED "1/5 Find " UNTAGGED
#define STR_STEP_ASSIGNING_ALBUMS "2/5 Find Albums"
#define STR_STEP_ASSIGNING_ALBUM_YEAR "3/5 Check Album Year"
#define STR_STEP_REMOVING_DUPLICATES "4/5 Remove Duplicates"

/*adds <untagged> albums/artist to existing album index */
static int create_album_untagged(struct tagcache_search *tcs, size_t *bufsz)
{
    static char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    int ret = SUCCESS;
    int album_count = pf_idx.album_ct; /* store existing count */
    int total_count = pf_idx.album_ct + pf_idx.artist_ct * 2;
    long seek;
    int last, final, retry;
    int i, j;
    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, total_count, STR_STEP_INDEXING_UNTAGGED);

    /* search tagcache for all <untagged> albums & save the albumartist seek pos */
    if (tagcache_search(tcs, tag_albumartist))
    {
        tagcache_search_add_filter(tcs, tag_album, pf_idx.album_untagged_seek);

        while (tagcache_get_next(tcs, tcs_buf, tcs_bufsz))
        {
            if (progress_cancel(pf_idx.album_ct, total_count, STR_STEP_INDEXING_UNTAGGED))
            {
                tagcache_search_finish(tcs);
                return ERROR_USER_ABORT;
            }

            if (tcs->result_seek ==
                pf_idx.album_index[-(pf_idx.album_ct - 1)].artist_seek)
                continue;

            if (sizeof(struct album_data) > *bufsz)
            {
                /* not enough memory */
                ret = ERROR_BUFFER_FULL;
                break;
            }

            *bufsz -= sizeof(struct album_data);
            write_album_index(-pf_idx.album_ct, pf_idx.album_untagged_idx,
                               pf_idx.album_untagged_seek, -1, tcs->result_seek);

            pf_idx.album_ct++;
        }
        tagcache_search_finish(tcs);

        if (ret == SUCCESS) {
            draw_progressbar(0, pf_idx.album_ct, STR_STEP_INDEXING_UNTAGGED);

            last = 0;
            final = pf_idx.artist_ct;
            retry = 0;

            /* map the artist_seek position to the artist name index */
            for (j = album_count; j < pf_idx.album_ct; j++)
            {
                if (progress_cancel(j, pf_idx.album_ct, STR_STEP_INDEXING_UNTAGGED))
                    return ERROR_USER_ABORT;

                seek = pf_idx.album_index[-j].artist_seek;

    retry_artist_lookup:
                retry++;
                for (i = last; i < final; i++)
                {
                    if (seek == pf_idx.artist_index[i].seek)
                    {
                        int idx = pf_idx.artist_index[i].name_idx;
                        pf_idx.album_index[-j].artist_idx = idx;
                        last = i; /* last match, start here next loop */
                        final = pf_idx.artist_ct;
                        retry = 0;
                        break;
                    }
                }
                if (retry > 0 && retry < 2)
                {
                    /* no match start back at beginning */
                    final = last;
                    last = 0;
                    goto retry_artist_lookup;
                }
            }
        }
    }

    return ret;
}

/* Create an index of all artists from the database */
int build_artist_index(struct tagcache_search *tcs,
                                 void **buf, size_t *bufsz)
{
    int i, res = SUCCESS;
    struct artist_data* tmp_artist;

    /* artist index starts at end of buf it will be rearranged when finalized */
    pf_idx.artist_index = ((struct artist_data *)(*bufsz + (char *) *buf)) - 1;
    pf_idx.artist_ct = 0;
    pf_idx.artist_len = 0;
    /* artist names starts at beginning of buf */
    pf_idx.artist_names = *buf;

    tagcache_search(tcs, tag_albumartist);
    res = get_tcs_search_res(ePFS_ARTIST, tcs, &(*buf), bufsz);
    tagcache_search_finish(tcs);
    if (res < SUCCESS)
        return res;

    /* finalize the artist index */
    ALIGN_BUFFER(*buf, *bufsz, alignof(struct artist_data));
    tmp_artist = (struct artist_data*)*buf;
    for (i = pf_idx.artist_ct - 1; i >= 0; i--)
        tmp_artist[i] = pf_idx.artist_index[-i];

    pf_idx.artist_index = tmp_artist;
    /* move buf ptr to end of artist_index */
    *buf = pf_idx.artist_index + pf_idx.artist_ct;

    if (res == SUCCESS)
    {
        if (pf_idx.artist_ct > 0)
            res = pf_idx.artist_ct;
        else
            res = ERROR_NO_ALBUMS;
    }

    return res;
}

static int assign_album_year(void)
{
    char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, pf_idx.album_ct, STR_STEP_ASSIGNING_ALBUM_YEAR);
    for (int album_idx = 0; album_idx < pf_idx.album_ct; album_idx++)
    {
        /* Prevent idle poweroff */
        reset_poweroff_timer();

        if (progress_cancel(album_idx, pf_idx.album_ct, STR_STEP_ASSIGNING_ALBUM_YEAR))
            return ERROR_USER_ABORT;

        int album_year = 0;

        if (tagcache_search(&tcs, tag_year))
        {
            tagcache_search_add_filter(&tcs, tag_album,
                                       pf_idx.album_index[album_idx].seek);

            if (pf_idx.album_index[album_idx].artist_idx >= 0)
                tagcache_search_add_filter(&tcs, tag_albumartist,
                    pf_idx.album_index[album_idx].artist_seek);

            while (tagcache_get_next(&tcs, tcs_buf, tcs_bufsz)) {
                int track_year = tagcache_get_numeric(&tcs, tag_year);
                if (track_year > album_year)
                    album_year = track_year;
            }
        }
        tagcache_search_finish(&tcs);

        pf_idx.album_index[album_idx].year = album_year;
    }
    return SUCCESS;
}

/**
  Create an index of all artists and albums from the database.
  Also store the artists and album names so we can access them later.
 */
static int create_album_index(void)
{
    static char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    void *buf = pf_idx.buf;
    size_t buf_size = pf_idx.buf_sz;

    struct album_data* tmp_album;

    int i, j, last, final, retry, res;

    ALIGN_BUFFER(buf, buf_size, sizeof(long));

    /* Artists */
    res = build_artist_index(&tcs, &buf, &buf_size);
    if (res < SUCCESS)
        return res;

    /* Albums */
    pf_idx.album_ct = 0;
    pf_idx.album_len =0;
    pf_idx.album_untagged_idx = -1;
    pf_idx.album_untagged_seek = -1;

    /* album_index starts at end of buf it will be rearranged when finalized */
    pf_idx.album_index = ((struct album_data *)(buf_size + (char *)buf)) - 1;
    /* album_names starts at the beginning of buf */
    pf_idx.album_names = buf;

    tagcache_search(&tcs, tag_album);
    res = get_tcs_search_res(ePFS_ALBUM, &tcs, &buf, &buf_size);
    tagcache_search_finish(&tcs);
    if (res < SUCCESS)
        return res;

    /* Build artist list for untagged albums */
    res = create_album_untagged(&tcs, &buf_size);

    if (res < SUCCESS)
        return res;

    /* finalize the album index */
    ALIGN_BUFFER(buf, buf_size, alignof(struct album_data));
    tmp_album = (struct album_data*)buf;
    for (i = pf_idx.album_ct - 1; i >= 0; i--)
        tmp_album[i] = pf_idx.album_index[-i];

    pf_idx.album_index = tmp_album;
    /* move buf ptr to end of album_index */
    buf = pf_idx.album_index + pf_idx.album_ct;

    /* Assign indices */
    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, pf_idx.album_ct, STR_STEP_ASSIGNING_ALBUMS);
    for (j = 0; j < pf_idx.album_ct; j++)
    {
        /* Prevent idle poweroff */
        reset_poweroff_timer();

        if (progress_cancel(j, pf_idx.album_ct, STR_STEP_ASSIGNING_ALBUMS))
            return ERROR_USER_ABORT;

        if (pf_idx.album_index[j].artist_seek >= 0) { continue; }

        tagcache_search(&tcs, tag_albumartist);
        tagcache_search_add_filter(&tcs, tag_album, pf_idx.album_index[j].seek);

        last = 0;
        final = pf_idx.artist_ct;
        retry = 0;
        if (tagcache_get_next(&tcs, tcs_buf, tcs_bufsz))
        {

retry_artist_lookup:
            retry++;
            for (i = last; i < final; i++)
            {
                if (tcs.result_seek == pf_idx.artist_index[i].seek)
                {
                    int idx = pf_idx.artist_index[i].name_idx;
                    pf_idx.album_index[j].artist_idx = idx;
                    pf_idx.album_index[j].artist_seek = tcs.result_seek;
                    last = i; /* last match, start here next loop */
                    final = pf_idx.artist_ct;
                    retry = 0;
                    break;
                }
            }
            if (retry > 0 && retry < 2)
            {
                /* no match start back at beginning */
                final = last;
                last = 0;
                goto retry_artist_lookup;
            }
        }
        tagcache_search_finish(&tcs);
    }

    res = assign_album_year();

    if (res < SUCCESS)
        return res;

    /* sort list order to find duplicates */
    qsort(pf_idx.album_index, pf_idx.album_ct,
              sizeof(struct album_data), compare_album_artists);

    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, pf_idx.album_ct, STR_STEP_REMOVING_DUPLICATES);
    /* mark duplicate albums for deletion */
    for (i = 0; i < pf_idx.album_ct - 1; i++) /* -1 don't check last entry */
    {
        /* Prevent idle poweroff */
        reset_poweroff_timer();

        if (progress_cancel(i, pf_idx.album_ct, STR_STEP_REMOVING_DUPLICATES))
            return ERROR_USER_ABORT;

        int idxi = pf_idx.album_index[i].artist_idx;
        int seeki = pf_idx.album_index[i].seek;

        for (j = i + 1; j < pf_idx.album_ct; j++)
        {
            if (idxi > 0 &&
            idxi == pf_idx.album_index[j].artist_idx &&
            seeki == pf_idx.album_index[j].seek)
            {
                pf_idx.album_index[j].artist_idx = -1;
            }
            else
            {
                i = j - 1;
                break;
            }
        }
    }

    /* now fix the album list order */
    qsort(pf_idx.album_index, pf_idx.album_ct,
              sizeof(struct album_data), compare_album_artists);

    /* remove any extra untagged albums
     * extra space is orphaned till restart */
    pf_idx.album_index += pf_idx.album_untagged_idx + 1;
    pf_idx.album_ct -= pf_idx.album_untagged_idx + 1;

    pf_idx.buf = buf;
    pf_idx.buf_sz = buf_size;
    pf_idx.artist_index = 0;

    qsort(pf_idx.album_index, pf_idx.album_ct,
                          sizeof(struct album_data), compare_albums);

    return (pf_idx.album_ct > 0) ? 0 : ERROR_NO_ALBUMS;
}

/*Saves the album index into a binary file to be recovered the
 next time PictureFlow is launched*/

static int save_album_index(void){
    int fd = creat(ALBUM_INDEX,0666);

    struct pf_index_t data;
    memcpy(&data, &pf_idx, sizeof(struct pf_index_t));

    if(fd >= 0)
    {
        memcpy(&data.header, INDEX_HDR, sizeof(pf_idx.header));

        write(fd, &data, sizeof(struct pf_index_t));

        write(fd, data.artist_names, data.artist_len);
        write(fd, data.album_names, data.album_len);

        write(fd, data.album_index, data.album_ct * sizeof(struct album_data));

        close(fd);
        return 0;
    }
    return -1;
}

/* reads data from save file to buffer */
static inline int read2buf(int fildes, void *buf, size_t nbyte){
    int nread;
    nread = read(fildes, buf, nbyte);
    if (nread < (int)nbyte)
        return 0;

    return nread;
}

/*Loads the album_index information stored in the hard drive*/
static int load_album_index(void){

    int i, fr = open(ALBUM_INDEX, O_RDONLY);
    struct pf_index_t data;

    void *bufstart = pf_idx.buf;
    unsigned int bufstart_sz = pf_idx.buf_sz;

    void* buf = pf_idx.buf;
    size_t buf_size = pf_idx.buf_sz;

    unsigned int name_sz, album_idx_sz;
    int album_idx, artist_idx;

    if (fr >= 0){
        const unsigned long fsize = filesize(fr);
        if (fsize > sizeof(data))
        {
            if (read(fr, &data, sizeof(data)) == sizeof(data) &&
                memcmp(&(data.header), INDEX_HDR, sizeof(data.header)) == 0)
            {
                name_sz = data.artist_len + data.album_len;
                album_idx_sz = data.album_ct * sizeof(struct album_data);

                if (name_sz + album_idx_sz > bufstart_sz)
                    goto failure;

                /* lseek(fr, sizeof(data) + 1, SEEK_SET); */
                /* artist names */
                if (read2buf(fr, buf, data.artist_len) == 0)
                    goto failure;

                data.artist_names = buf;
                buf = (char *)buf + data.artist_len;
                buf_size -= data.artist_len;

                /* album names */
                if (read2buf(fr, buf, data.album_len) == 0)
                    goto failure;

                data.album_names = buf;
                buf = (char *)buf + data.album_len;
                buf_size -= data.album_len;

                /* index of album names */
                ALIGN_BUFFER(buf, buf_size, alignof(struct album_data));
                if (read2buf(fr, buf, album_idx_sz) == 0)
                    goto failure;

                data.album_index = buf;
                buf = (char *)buf + album_idx_sz;
                buf_size -= album_idx_sz;

                close(fr);

                /* sanity check loaded data */
                for (i = 0; i < data.album_ct; i++)
                {
                    album_idx = data.album_index[i].name_idx;
                    artist_idx = data.album_index[i].artist_idx;
                    if (album_idx >= (int) data.album_len ||
                        artist_idx >= (int) data.artist_len)
                    {
                        goto failure;
                    }
                }

                memcpy(&pf_idx, &data, sizeof(struct pf_index_t));
                pf_idx.buf = buf;
                pf_idx.buf_sz = buf_size;

                qsort(pf_idx.album_index, pf_idx.album_ct,
                          sizeof(struct album_data), compare_albums);

                return 0;
            }
        }
    }

failure:
    splash(HZ/2, "Failed to load index");
    if (fr >= 0)
        close(fr);

    pf_idx.buf = bufstart;
    pf_idx.buf_sz = bufstart_sz;
    pf_idx.artist_ct = 0;
    pf_idx.album_ct = 0;
    return -1;

}

/* carousel_model.build_index for the album model: reuse the on-disk index when
 * it matches the current cache version, otherwise rebuild it (and persist it).
 * Returns SUCCESS or one of the ERROR_* codes. */
static int album_build_index(void)
{
    int ret = SUCCESS;

    /* Scan will trigger when no file is found or the option was activated */
    if ((pf_cfg.cache_version != CACHE_VERSION) || (load_album_index() < 0))
    {
        ui_set_working(true);   /* show the "working" LED while (re)building */
        ret = create_album_index();
        ui_set_working(false);

        if (ret == 0)
        {
            pf_cfg.cache_version = CACHE_REBUILD;
            pf_config_save();
            if (save_album_index() < 0)
                splash(HZ, "Could not write index");
        }
    }
    return ret;
}

/**
 Return a pointer to the album name of the given slide_index
 */
static char* get_album_name(const int slide_index)
{
    char *name = pf_idx.album_names + pf_idx.album_index[slide_index].name_idx;
    return name;
}

/**
 Return a pointer to the album name of the given slide_index
 */
static char* get_album_name_idx(const int slide_index, int *idx)
{
    *idx = pf_idx.album_index[slide_index].name_idx;
    char *name = pf_idx.album_names + pf_idx.album_index[slide_index].name_idx;
    return name;
}

/**
 Return a pointer to the album artist of the given slide_index
 */
static char* get_album_artist(const int slide_index)
{
    if (slide_index < pf_idx.album_ct && slide_index >= 0){
        int idx = pf_idx.album_index[slide_index].artist_idx;
        if (idx >= 0 && idx < (int) pf_idx.artist_len) {
            char *name = pf_idx.artist_names + idx;
            return name;
        }
    }
    return "?";
}


static char* get_slide_name(const int slide_index, bool artist)
{
    if (artist)
        return get_album_artist(slide_index);

    return get_album_name(slide_index);
}

static int jmp_idx_prev(void)
{
    if (!carousel_cache_ready())
    {
        splash(HZ*2, str(LANG_WAIT_FOR_CACHE));
        return center_index;
    }

    if (global_settings.album_covers_sort_albums_by == SORT_BY_YEAR)
    {
        int current_year = pf_idx.album_index[center_index].year;

        for (int i = center_index - 1; i > 0; i-- )
        {
            if(pf_idx.album_index[i].year != current_year)
                current_year = pf_idx.album_index[i].year;
            while (i > 0)
            {
                if (pf_idx.album_index[i-1].year != current_year)
                    break;
                i--;
            }
            return i;
        }
    }
    else
    {
        bool by_artist = global_settings.album_covers_sort_albums_by != SORT_BY_NAME;
        char *current_selection = get_slide_name(center_index, by_artist);

        for (int i = center_index - 1; i > 0; i-- )
        {
            if(strncmp(get_slide_name(i, by_artist), current_selection, 1))
                current_selection = get_slide_name(i, by_artist);
            while (i > 0)
            {
                if (strncmp(get_slide_name(i-1, by_artist), current_selection, 1))
                    break;
                i--;
            }
            return i;
        }
    }

    return 0;
}

static int jmp_idx_next(void)
{
    if (!carousel_cache_ready())
    {
        splash(HZ*2, str(LANG_WAIT_FOR_CACHE));
        return center_index;
    }

    if (global_settings.album_covers_sort_albums_by == SORT_BY_YEAR)
    {
        int current_year = pf_idx.album_index[center_index].year;
        for (int i = center_index + 1; i < pf_idx.album_ct; i++ )
            if(pf_idx.album_index[i].year != current_year)
                return i;
    }
    else
    {
        bool by_artist = global_settings.album_covers_sort_albums_by != SORT_BY_NAME;
        char *current_selection = get_slide_name(center_index, by_artist);
        for (int i = center_index + 1; i < pf_idx.album_ct; i++ )
            if(strncmp(get_slide_name(i, by_artist), current_selection, 1))
                return i;
    }
    return pf_idx.album_ct - 1;
}

static int id3_get_index(struct mp3entry *id3)
{
    char* current_artist = UNTAGGED;
    char* current_album  = UNTAGGED;

    if(id3)
    {
        /* we could be looking for the artist in either field */
        if(id3->albumartist)
            current_artist = id3->albumartist;
        else if(id3->artist)
            current_artist = id3->artist;

        if (id3->album && strlen(id3->album) > 0)
            current_album = id3->album;

        /* splashf(1000, "%s, %s", current_album, current_artist); */

        int i;
        int album_idx, artist_idx;

        for (i = 0; i < pf_idx.album_ct; i++ )
        {
            album_idx = pf_idx.album_index[i].name_idx;
            artist_idx = pf_idx.album_index[i].artist_idx;

            if(!strcmp(pf_idx.album_names + album_idx, current_album) &&
                !strcasecmp(pf_idx.artist_names + artist_idx, current_artist))
                return i;
        }

    }
    splash(HZ/2, "Album Not Found!");
    return pf_cfg.last_album;
}

bool retrieve_id3(struct mp3entry *id3, const char* file)
{
    if (tagcache_fill_tags(id3, file))
    {
        strlcpy(id3->path, file, sizeof(id3->path));
        return true;
    }

    return get_metadata(id3, -1, file);
}

/**
  Draw a progress popup using the shared firmware progress widget, so
  index-building looks like every other long-running operation (e.g. the
  Album art cache builder) instead of a bespoke full-screen bar.
 */
/* Index building shows the theme's generic "working" indicator (the %lw
 * virtual LED / %la spinner, gated on ui_working()) instead of a covering
 * progress splash. Forcing a status-bar refresh here advances the spinner as
 * the build progresses. The step/count/msg args are unused, kept because the
 * callback signature is shared with the other progress reporters. */
static void draw_progressbar(int step, int count, char *msg)
{
    (void)step; (void)count; (void)msg;
    sb_skin_update(SCREEN_MAIN, true);
    skin_flush_dirty();
}

/* Calculate modified FNV hash of string
 * has good avalanche behaviour and uniform distribution
 * see http://home.comcast.net/~bretm/hash/ */
static unsigned int mfnv(char *str)
{
    const unsigned int p = 16777619;
    unsigned int hash = 0x811C9DC5; /* 2166136261; */

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


/* Resolve the album's folder (directory of its first track) for a slide, so
 * its thumbnail can be looked up in the folder-keyed shared cache. Runs on the
 * picture-load thread, so the tagcache search here doesn't stall rendering. */
static bool get_slide_dir(const int slide_index, char *dir, int dirlen)
{
    struct tagcache_search tcs_l;
    char tcs_buf[TAGCACHE_BUFSZ];
    bool ret = false;

    if (!tagcache_search(&tcs_l, tag_filename))
        return false;

    tagcache_search_add_filter(&tcs_l, tag_album,
                               pf_idx.album_index[slide_index].seek);
    tagcache_search_add_filter(&tcs_l, tag_albumartist,
                               pf_idx.album_index[slide_index].artist_seek);

    if (tagcache_get_next(&tcs_l, tcs_buf, sizeof(tcs_buf)))
    {
        const char *sep = strrchr(tcs_l.result, '/');
        int len = sep ? (int)(sep - tcs_l.result) : 0;
        if (len >= dirlen)
            len = dirlen - 1;
        if (len > 0)
        {
            memcpy(dir, tcs_l.result, len);
            dir[len] = 0;
            ret = true;
        }
    }
    tagcache_search_finish(&tcs_l);
    return ret;
}

/* carousel_model.count for the album model. */
static int album_count(void)
{
    return pf_idx.album_ct;
}

/* carousel_model.legacy_art for the album model: this screen's own per-album
 * pfraw cache, keyed by a hash of the album+artist names. Always available. */
static bool album_legacy_art(int index, char *path, int len)
{
    snprintf(path, len, CACHE_PREFIX "/%x%x.pfraw",
             mfnv(get_album_name(index)), mfnv(get_album_artist(index)));
    return true;
}

static void set_initial_slide(const char* selected_file)
{
    if (pf_resume_last_album)
    {
        pf_resume_last_album = false;
        set_current_slide(pf_resume_album_index);
        return;
    }

    if (selected_file)
        set_current_slide(retrieve_id3(&id3, selected_file) ?
                            id3_get_index(&id3) :
                            pf_cfg.last_album);
    else
        set_current_slide(audio_status() ?
                            id3_get_index(audio_current_track()) :
                            pf_cfg.last_album);

}

static void reselect(unsigned int hash_album, unsigned int hash_artist)
{
    int i, album_idx, artist_idx;
    for (i = 0; i < pf_idx.album_ct; i++ )
    {
        album_idx = pf_idx.album_index[i].name_idx;
        artist_idx = pf_idx.album_index[i].artist_idx;

        if(hash_album == mfnv(pf_idx.album_names + album_idx) &&
           hash_artist == mfnv(pf_idx.artist_names + artist_idx))
        {
            set_current_slide(i);
            pf_cfg.last_album = i;
            return;
        }
    }
    set_initial_slide(NULL);
}

static bool sort_albums(int new_sorting, bool from_settings)
{
    unsigned int hash_album, hash_artist;
    static const char* sort_options[] = {
        ID2P(LANG_ARTIST_PLUS_NAME),
        ID2P(LANG_ARTIST_PLUS_YEAR),
        ID2P(LANG_ID3_YEAR),
        ID2P(LANG_NAME)
    };

    /* Only change sorting once artwork has been inspected */
    if (!carousel_cache_ready())
    {
        splash(HZ*2, str(LANG_WAIT_FOR_CACHE));
        return false;
    }

    carousel_settle();

    global_settings.album_covers_sort_albums_by = new_sorting;
    if (!from_settings)
    {
        splash(HZ, sort_options[global_settings.album_covers_sort_albums_by]);
    }

    hash_album = mfnv(get_album_name(center_index));
    hash_artist = mfnv(get_album_artist(center_index));

    carousel_reload(compare_albums);

    reselect(hash_album, hash_artist); /* splash if not found */

    return true;
}

/* carousel_model.sort_next/sort_prev for the album model: cycle through the
 * album sort orders and re-sort. */
static void album_sort_next(void)
{
    sort_albums((global_settings.album_covers_sort_albums_by + 1)
                % SORT_VALUES_SIZE, false);
}

static void album_sort_prev(void)
{
    sort_albums((global_settings.album_covers_sort_albums_by + (SORT_VALUES_SIZE - 1))
                % SORT_VALUES_SIZE, false);
}

void album_covers_rebuild_cache(void)
{
    pf_cfg.update_albumart = false;
    pf_cfg.cache_version = CACHE_REBUILD;
    remove(EMPTY_SLIDE);
    pf_config_save();
}

void album_covers_update_cache(void)
{
    pf_cfg.update_albumart = true;
    pf_cfg.cache_version = CACHE_REBUILD;
    remove(EMPTY_SLIDE);
    pf_config_save();
}

/* Restored to (almost) exactly how the original plugin's draw_album_text()
 * worked: drawn directly inside pf_vp -- the same viewport the coverflow
 * itself uses, not a separate panel -- overlaid near the top or bottom of
 * that same area, using pf_fg_color (dynamic_colors_resolve()-derived, so
 * the now-playing dynamic colour fade applies here too, same as everywhere
 * else in the UI). Differences from the original: no text_crossfade
 * (removed, see B2) so this always uses center_index rather than tracking
 * a mid-scroll transition index; no show_statusbar variant (the status bar
 * is always theme-controlled now, never toggled off by this screen); the
 * album name uses pf_bold_font rather than always FONT_UI -- a genuinely
 * separate, actually-bold font file if the theme configured one (see
 * global_settings.bold_font_file's comment in settings.h), not faked. A
 * faux-bold (double-drawn, offset by a pixel) treatment was tried here
 * first and reverted -- didn't look right. */
static void draw_album_text(void)
{
    char album_and_year[MAX_PATH];
    char *albumtxt, *artisttxt;
    int album_idx = 0;
    int char_height;
    int albumtxt_x, albumtxt_y, artisttxt_x;
    bool show_artist;

    if (global_settings.album_covers_show_album_name == ALBUM_NAME_HIDE)
        return;

    show_artist = (global_settings.album_covers_show_album_name == ALBUM_AND_ARTIST_TOP
                || global_settings.album_covers_show_album_name == ALBUM_AND_ARTIST_BOTTOM);

    albumtxt = get_album_name_idx(center_index, &album_idx);
    if (global_settings.album_covers_show_year
        && pf_idx.album_index[center_index].year > 0)
    {
        snprintf(album_and_year, sizeof(album_and_year), "%s \xe2\x80\x93 %d",
                  albumtxt, pf_idx.album_index[center_index].year);
    }
    else
        snprintf(album_and_year, sizeof(album_and_year), "%s", albumtxt);

    struct viewport *saved_vp = carousel_text_begin();
    lcd_set_foreground(pf_fg_color);

    static int prev_albumtxt_index = -1;
    static bool prev_show_year = false;
    bool album_changed = (center_index != prev_albumtxt_index
                         || global_settings.album_covers_show_year != prev_show_year);

    /* Switched to pf_bold_font (plain FONT_UI, a no-op, if no bold font is
     * configured -- see its declaration) *before* set_scroll_line() below:
     * that measures the string's pixel width against whatever font is
     * currently active, to center/scroll it, so it has to see the album
     * name's *actual* rendering font, not FONT_UI, or a bold font with a
     * different average glyph width would end up mis-centered. */
    lcd_setfont(pf_bold_font);
    if (album_changed)
    {
        set_scroll_line(album_and_year, PF_SCROLL_ALBUM);
        prev_albumtxt_index = center_index;
        prev_show_year = global_settings.album_covers_show_year;
    }

    char_height = screens[SCREEN_MAIN].getcharheight();
    switch (global_settings.album_covers_show_album_name)
    {
        case ALBUM_AND_ARTIST_TOP:
            albumtxt_y = 0;
            break;
        case ALBUM_NAME_BOTTOM:
        case ALBUM_AND_ARTIST_BOTTOM:
            albumtxt_y = pf_height - (char_height * 9 / 4);
            break;
        case ALBUM_NAME_TOP:
        default:
            albumtxt_y = char_height / 2;
            break;
    }

    albumtxt_x = get_scroll_line_offset(PF_SCROLL_ALBUM);

    if (show_artist)
    {
        if (pf_idx.album_index[center_index].seek != pf_idx.album_untagged_seek)
            lcd_putsxy(albumtxt_x, albumtxt_y, album_and_year);
        /* Restored before the artist line: render_all_slides()/the FPS
         * overlay/etc all assume pf_vp's font is the real UI font, and the
         * artist name itself is never bold. screens[SCREEN_MAIN].getuifont(),
         * not the FONT_UI constant -- see init()'s comment on pf_bold_font. */
        lcd_setfont(screens[SCREEN_MAIN].getuifont());

        artisttxt = get_album_artist(center_index);
        if (album_changed)
            set_scroll_line(artisttxt, PF_SCROLL_ARTIST);
        artisttxt_x = get_scroll_line_offset(PF_SCROLL_ARTIST);
        lcd_putsxy(artisttxt_x, albumtxt_y + char_height * 3 / 4, artisttxt);
    }
    else
    {
        lcd_putsxy(albumtxt_x, albumtxt_y, album_and_year);
        lcd_setfont(screens[SCREEN_MAIN].getuifont());
    }
    carousel_text_end(saved_vp);
}

/* Rebuild the album carousel in place (after a settings change or cache
 * rebuild), restoring the same album across the rebuild by name hash. Only the
 * album model reaches this (via album_on_menu). */
static bool reinit(void)
{
    unsigned int hash_album, hash_artist;

    carousel_settle();

    hash_album = mfnv(get_album_name(center_index));
    hash_artist = mfnv(get_album_artist(center_index));

    if (carousel_reinit())
    {
        reselect(hash_album, hash_artist); /* splash if not found */
        return true;
    }
    return false;
}

/* carousel_model.enter for the album model: drill into the selected album's
 * track list. Identifies the album by its own tagcache seek, not by name --
 * browser_db.c filters directly on this, so there's no name/sort matching involved
 * (the earlier, much more fragile design that this replaced). */
static int album_enter(int index)
{
    int album_idx = 0;
    char *album = get_album_name_idx(index, &album_idx);
    long album_seek = pf_idx.album_index[index].seek;

    pf_cfg.last_album = index;
    pf_resume_album_index = index;
    pf_resume_last_album = true;
    browser_db_enter_album_tracks_on_next_load(album_seek, album);
    return GO_TO_ALBUM_COVERS_TRACKS;
}

/* Open the shared Carousel settings menu (the same one under General Settings)
 * over a running carousel, and hand back do_menu()'s raw result. Both models'
 * on_menu hooks go through this; what differs is only what each does with the
 * settings afterwards, so that part stays with the model.
 *
 * Exported (album_covers.h) so artist_portraits.c gets the same menu without
 * duplicating the theme/activity plumbing. */
int carousel_settings_menu(void)
{
    int selected = 0;
    int ret;

    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, true, NULL);
    /* do_menu() only auto-switches to a menu-styling activity for
     * ACTIVITY_PLUGIN; the carousel runs as ACTIVITY_ALBUMCOVERS, which the
     * theme's SBS likewise excludes from menu chrome -- switch it ourselves so
     * the menu is themed the same as it is from Settings. */
    push_current_activity(ACTIVITY_CONTEXTMENU);
    ret = do_menu(&album_covers_menu, &selected, NULL, false);
    pop_current_activity();
    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, false);

    return ret;
}

/* carousel_model.on_menu: open the shared Carousel settings menu and apply
 * whatever changed on the way out. Returns a GO_TO_* to exit the carousel, or a
 * CAROUSEL_MENU_* sentinel to stay (the engine loop restores the display).
 *
 * (The old hand-rolled menu's navigation items are gone: Go-To-WPS is the Play
 * button, and Go-To-Last-Album was dropped.) */
static int album_on_menu(void)
{
    /* Snapshot the settings whose change needs more than a cheap layout redraw. */
    int old_sort       = global_settings.album_covers_sort_albums_by;
    int old_year_order = global_settings.album_covers_year_sort_order;
    int old_show_name  = global_settings.album_covers_show_album_name;
    int old_cache_ver  = pf_cfg.cache_version;

    if (carousel_settings_menu() == MENU_ATTACHED_USB)
        return GO_TO_ROOT;

    /* A cache rebuild/update (those items bump cache_version) or a caption-layout
     * change needs a full rebuild -- init() recomputes the text margin and, when
     * the cache was invalidated, regenerates the index. */
    if (pf_cfg.cache_version != old_cache_ver
        || global_settings.album_covers_show_album_name != old_show_name)
    {
        if (!reinit())
            return GO_TO_PREVIOUS;
    }

    /* A sort-order change must be applied explicitly: reinit()'s normal path
     * reloads the cached index in its saved order, so it wouldn't re-sort. */
    if (global_settings.album_covers_sort_albums_by != old_sort
        || global_settings.album_covers_year_sort_order != old_year_order)
        sort_albums(global_settings.album_covers_sort_albums_by, true);

    /* Re-apply the live geometry settings (zoom / margins / tilt). Cheap and
     * keeps the current slide; harmless after the above. */
    carousel_refresh();
    return CAROUSEL_MENU_RELOADED;
}

/* carousel_model.prepare: after init(), mark this cache format as current so
 * create_empty_slide()'s force-regenerate (gated on cache_version) only fires
 * on the first launch following a format change, not every launch. */
static void album_prepare(void)
{
    if (pf_cfg.cache_version != CACHE_VERSION)
    {
        pf_cfg.cache_version = CACHE_VERSION;
        pf_config_save();
    }
}

int album_covers(const char *selected_file)
{
    return carousel_run(&album_model, selected_file);
}
