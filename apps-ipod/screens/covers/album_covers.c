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
#include <stdlib.h>            /* qsort */
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
#include "database/db_summary.h"   /* the album/artist index, and playing one */
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
#include "backlight.h"        /* backlight_set_timeout(_plugged) */
#include "cpu.h"
#include "skin/skin_engine.h"  /* skin_inhibit_flush */
#include "skin/skin_albumart_color.h" /* dynamic_colors_resolve */
#include "skin/statusbar_skinned.h" /* sb_set_persistent_title */
#include "album_covers.h"
#include "carousel.h"     /* the shared engine: carousel_idx, the model */

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
 * reads carousel_idx.artist_names/artist_index straight out of it -- so the
 * name is deliberately not album-specific. Regenerated if absent. */

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
 * album_covers_loop()). There is no in-house cover-zoom or track-list
 * browsing state; the database browser is the track list.
 */
enum pf_states {
    pf_idle = 0,
    pf_scrolling
};

/* struct carousel_model (the engine<->data seam) is declared in carousel.h. */

static int  album_count(void);
static unsigned int album_art_key(int slide_index);
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
static int  album_build_index(void);

static const struct carousel_model album_model = {
    .build_index = album_build_index,
    .count       = album_count,
    .art_key     = album_art_key,
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

/**
 Return a pointer to the album name of the given slide_index
 */
static char* get_album_name(const int slide_index)
{
    char *name = carousel_idx.album_names
               + carousel_idx.album_index[slide_index].name_idx;
    return name;
}

/**
 Return a pointer to the album name of the given slide_index
 */
static char* get_album_name_idx(const int slide_index, int *idx)
{
    *idx = carousel_idx.album_index[slide_index].name_idx;
    char *name = carousel_idx.album_names
               + carousel_idx.album_index[slide_index].name_idx;
    return name;
}

/**
 Return a pointer to the album artist of the given slide_index
 */
static char* get_album_artist(const int slide_index)
{
    if (slide_index < carousel_idx.album_ct && slide_index >= 0){
        int idx = carousel_idx.album_index[slide_index].artist_idx;
        if (idx >= 0 && idx < (int) carousel_idx.artist_len) {
            char *name = carousel_idx.artist_names + idx;
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

    /* Step back to the first slide of a run.
     *
     * Two cases, and the same three lines cover both. Standing part-way into a
     * run, the slide before us shares our key, so we keep it and walk to that
     * run's start. Standing on a run's first slide, the one before belongs to
     * the previous run, so we adopt *its* key and walk to that run's start
     * instead. Either way the answer is the first slide holding `current`.
     *
     * Upstream (pictureflow) writes this as a `for` loop whose body always
     * returns, i.e. a disguised `if`. Unwound here, and in artist_jump_prev(),
     * which is the same walk over the artist list. */
    if (global_settings.album_covers_sort_albums_by == SORT_BY_YEAR)
    {
        int current_year = carousel_idx.album_index[center_index].year;
        int i = center_index - 1;

        if (i > 0)
        {
            if (carousel_idx.album_index[i].year != current_year)
                current_year = carousel_idx.album_index[i].year;
            while (i > 0
                   && carousel_idx.album_index[i - 1].year == current_year)
                i--;
            return i;
        }
    }
    else
    {
        bool by_artist = global_settings.album_covers_sort_albums_by != SORT_BY_NAME;
        char *current_selection = get_slide_name(center_index, by_artist);
        int i = center_index - 1;

        if (i > 0)
        {
            if (strncmp(get_slide_name(i, by_artist), current_selection, 1))
                current_selection = get_slide_name(i, by_artist);
            while (i > 0 && strncmp(get_slide_name(i - 1, by_artist),
                                    current_selection, 1) == 0)
                i--;
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
        int current_year = carousel_idx.album_index[center_index].year;
        for (int i = center_index + 1; i < carousel_idx.album_ct; i++ )
            if(carousel_idx.album_index[i].year != current_year)
                return i;
    }
    else
    {
        bool by_artist = global_settings.album_covers_sort_albums_by != SORT_BY_NAME;
        char *current_selection = get_slide_name(center_index, by_artist);
        for (int i = center_index + 1; i < carousel_idx.album_ct; i++ )
            if(strncmp(get_slide_name(i, by_artist), current_selection, 1))
                return i;
    }
    return carousel_idx.album_ct - 1;
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

        for (i = 0; i < carousel_idx.album_ct; i++ )
        {
            album_idx = carousel_idx.album_index[i].name_idx;
            artist_idx = carousel_idx.album_index[i].artist_idx;

            if(!strcmp(carousel_idx.album_names + album_idx, current_album) &&
                !strcasecmp(carousel_idx.artist_names + artist_idx,
                            current_artist))
                return i;
        }

    }
    splash(HZ * 2, "Album not found");
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


/* carousel_model.art_key for the album model: the shared cache's key for the
 * folder this album's tracks live in, resolved when the index was built. */
static unsigned int album_art_key(int slide_index)
{
    return carousel_idx.album_index[slide_index].art_hash;
}

/* The order the slides appear in, from this screen's two sort settings.
 *
 * Here rather than in the index builder because it is a display order and
 * nothing else: it reads settings this screen owns, and no other reader of the
 * index wants it. The charts rank the same albums their own way, and Random
 * album picks by number. */
static int compare_albums(const void *a_v, const void *b_v)
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

/* carousel_model.build_index for the album model: the whole index, into the
 * engine's own struct and the buffer it has already claimed, then into this
 * screen's display order.
 *
 * The database module builds into whichever index it is handed and leaves it
 * in its own order; choosing that it is this one, and how the slides are
 * arranged, is the model's business. */
static int album_build_index(void)
{
    int res = db_summary_build_into(&carousel_idx, carousel_idx.buf,
                                    carousel_idx.buf_sz);

    if (res >= SUCCESS)
        qsort(carousel_idx.album_index, carousel_idx.album_ct,
              sizeof(struct album_data), compare_albums);

    return res;
}

/* carousel_model.count for the album model. */
static int album_count(void)
{
    return carousel_idx.album_ct;
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
    for (i = 0; i < carousel_idx.album_ct; i++ )
    {
        album_idx = carousel_idx.album_index[i].name_idx;
        artist_idx = carousel_idx.album_index[i].artist_idx;

        if(hash_album == mfnv(carousel_idx.album_names + album_idx) &&
           hash_artist == mfnv(carousel_idx.artist_names + artist_idx))
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
    db_summary_invalidate();  /* so the background pass rebuilds too */
    pf_config_save();
}

static void album_covers_update_cache(void)
{
    pf_cfg.update_albumart = true;
    pf_cfg.cache_version = CACHE_REBUILD;
    remove(EMPTY_SLIDE);
    db_summary_invalidate();  /* so the background pass rebuilds too */
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

/* Drawn directly inside pf_vp -- the same viewport the coverflow itself uses,
 * not a separate panel -- overlaid near the top or bottom of that area, in
 * pf_fg_color, so the now-playing dynamic colour fade applies here as it does
 * everywhere else. Always center_index: there is no mid-scroll transition to
 * track. The album name uses pf_bold_font, a genuinely separate bold font file
 * where the theme configured one (global_settings.bold_font_file, settings.h)
 * -- not a double-drawn faux bold, which does not look right. */
static void draw_album_text(void)
{
    char album_and_year[MAX_PATH];
    char *albumtxt, *artisttxt;
    int album_idx = 0;
    struct pf_caption cap;
    int albumtxt_x, albumtxt_y, artisttxt_x;
    bool show_artist;

    if (global_settings.album_covers_show_album_name == ALBUM_NAME_HIDE)
        return;

    show_artist = (global_settings.album_covers_show_album_name == ALBUM_AND_ARTIST_TOP
                || global_settings.album_covers_show_album_name == ALBUM_AND_ARTIST_BOTTOM);

    albumtxt = get_album_name_idx(center_index, &album_idx);
    if (global_settings.album_covers_show_year
        && carousel_idx.album_index[center_index].year > 0)
    {
        snprintf(album_and_year, sizeof(album_and_year), "%s \xe2\x80\x93 %d",
                  albumtxt, carousel_idx.album_index[center_index].year);
    }
    else
        snprintf(album_and_year, sizeof(album_and_year), "%s", albumtxt);

    struct viewport *saved_vp = carousel_text_begin();
    lcd_set_foreground(pf_fg_color);

    /* The year is the caption's other input: turning it on or off changes the
     * string without moving the selection. */
    bool album_changed = carousel_caption_changed(
                             center_index, global_settings.album_covers_show_year);

    /* Switched to pf_bold_font (plain FONT_UI, a no-op, if no bold font is
     * configured -- see its declaration) *before* set_scroll_line() below:
     * that measures the string's pixel width against whatever font is
     * currently active, to center/scroll it, so it has to see the album
     * name's *actual* rendering font, not FONT_UI, or a bold font with a
     * different average glyph width would end up mis-centered. */
    lcd_setfont(pf_bold_font);
    if (album_changed)
        set_scroll_line(album_and_year, PF_SCROLL_ALBUM);

    /* The engine measures both fonts and centres the block for us, so top and
     * bottom captions need no separate arithmetic here. */
    carousel_caption_layout(show_artist, &cap);
    albumtxt_y = cap.y1;

    albumtxt_x = get_scroll_line_offset(PF_SCROLL_ALBUM);

    if (show_artist)
    {
        if (carousel_idx.album_index[center_index].seek
            != carousel_idx.album_untagged_seek)
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
        lcd_putsxy(artisttxt_x, cap.y2, artisttxt);
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

/* carousel_model.enter for the album model: either drill into the selected
 * album's track list or play it, per "On album select". Identifies the album by
 * its own tagcache seek, not by name -- browser_db.c and db_summary.c both
 * filter directly on this, so there's no name/sort matching involved (the
 * earlier, much more fragile design that this replaced).
 *
 * Playing does not happen here. It needs the app buffer to put the album in
 * order, and this screen is still holding a claim on it -- so the album is
 * recorded and album_covers() plays it once carousel_run() has returned. */
static int album_enter(int index)
{
    int album_idx = 0;
    char *album = get_album_name_idx(index, &album_idx);
    long album_seek = carousel_idx.album_index[index].seek;

    pf_cfg.last_album = index;
    pf_resume_album_index = index;
    pf_resume_last_album = true;

    if (global_settings.album_covers_on_select == ON_SELECT_PLAY_ALBUM)
    {
        db_summary_play_album_on_exit(&carousel_idx.album_index[index]);
        return CAROUSEL_PLAY_ALBUM;
    }

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
    bool old_statusbar = global_settings.album_covers_statusbar;
    int old_filter[CAROUSEL_FILTER_SLOTS];
    char old_chain[CAROUSEL_FILTER_MAX];

    memcpy(old_filter, global_settings.album_covers_filter, sizeof(old_filter));
    strmemccpy(old_chain, global_settings.album_covers_filter_chain,
               sizeof(old_chain));

    if (carousel_settings_menu() == MENU_ATTACHED_USB)
        return GO_TO_ROOT;

    /* A cache rebuild/update (those items bump cache_version), a caption-layout
     * change or a status-bar change needs a full rebuild -- init() recomputes
     * the text margin and the viewport and, when the cache was invalidated,
     * regenerates the index. */
    if (pf_cfg.cache_version != old_cache_ver
        || global_settings.album_covers_show_album_name != old_show_name
        || global_settings.album_covers_statusbar != old_statusbar)
    {
        if (!reinit())
            return GO_TO_PREVIOUS;
    }

    /* A sort-order change must be applied explicitly: reinit()'s normal path
     * reloads the cached index in its saved order, so it wouldn't re-sort. */
    if (global_settings.album_covers_sort_albums_by != old_sort
        || global_settings.album_covers_year_sort_order != old_year_order)
        sort_albums(global_settings.album_covers_sort_albums_by, true);

    /* A treatment reaches a slide only as it is loaded, so the ones already
     * decoded have to go -- otherwise the new look arrives a screen at a time
     * as the user scrolls past them. Both halves are compared: the written-out
     * chain overrides the three slots, so either changing changes the picture.
     */
    if (memcmp(old_filter, global_settings.album_covers_filter,
               sizeof(old_filter))
        || strcmp(old_chain, global_settings.album_covers_filter_chain))
        carousel_drop_slides();

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
    int ret = carousel_run(&album_model, selected_file);

    /* "On album select" = Play album. Deliberately here and not in
     * album_enter(): carousel_run()'s cleanup() has released the app buffer by
     * now, and putting the album in track order needs it. Doing it here rather
     * than in root_menu.c also means every way into this screen gets it -- the
     * WPS select action and the playlist viewer call this directly, and would
     * not know what to do with a sentinel escaping to them. */
    if (ret == CAROUSEL_PLAY_ALBUM)
        ret = db_summary_play_pending() > 0 ? GO_TO_WPS : GO_TO_PREVIOUS;

    return ret;
}
