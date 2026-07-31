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
#include "database/db_index.h" /* the album/artist list this model shows */

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

/* struct carousel_model (the engine<->data seam) is declared in carousel.h. */

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
    .build_index = db_index_build,
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

static void album_covers_rebuild_cache(void)
{
    pf_cfg.update_albumart = false;
    pf_cfg.cache_version = CACHE_REBUILD;
    remove(EMPTY_SLIDE);
    db_index_invalidate();  /* so the background pass rebuilds too */
    pf_config_save();
}

static void album_covers_update_cache(void)
{
    pf_cfg.update_albumart = true;
    pf_cfg.cache_version = CACHE_REBUILD;
    remove(EMPTY_SLIDE);
    db_index_invalidate();  /* so the background pass rebuilds too */
    pf_config_save();
}

/* bg_task.request: the carousel drives itself, so this is a task only in the
 * sense that the menu can reach it the same way as the others.
 *
 * It is not one background pass but two things going stale together: the
 * carousel's own on-disk state above, which nothing else touches, and the
 * album index, which really is a bg_task and is told separately. Pointing a
 * menu at that index alone would leave the first half undone. */
static void album_covers_request(bool rebuild)
{
    if (rebuild)
        album_covers_rebuild_cache();
    else
        album_covers_update_cache();
}

struct bg_task album_covers_task =
{
    .request = album_covers_request,
};

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
