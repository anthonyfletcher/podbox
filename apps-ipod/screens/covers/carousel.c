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
 * The cover-flow engine shared by album_covers.c and artist_portraits.c:
 * slide cache, 3D projection, scrolling, input loop and worker thread.
 * Each screen supplies a model.
 *
 * There is no floating point here. Positions and angles are PFreal, a signed
 * fixed-point long with PFREAL_SHIFT (10) fractional bits -- so PFREAL_ONE is
 * 1.0, and the fmul/fdiv/fsin/fcos helpers below are the arithmetic. Angles
 * are 0..IANGLE_MAX (1024) for a full turn, not radians or degrees, so they
 * mask instead of wrapping.
 *
 * Slides are decoded on a worker thread into a fixed set of cache slots.
 * Which slot holds which slide is tracked by the lla_* helpers -- a doubly
 * linked list whose "pointers" are array indices rather than addresses, so
 * the whole structure survives being moved and costs no allocation. Reading
 * the cache requires buf_ctx_lock().
 *
 * Parts, in order:
 *   - fixed-point and geometry constants, module state, theme colours
 *   - config load/save, and the database availability check
 *   - fixed-point arithmetic and trig
 *   - pixel row output helpers for the three source formats
 *   - scrolling text lines
 *   - the slide cache: the .pfraw format, the worker thread, and the
 *     index-linked-list that tracks slot use
 *   - the projection: offsets, per-slide render, and the full-screen compose
 *   - animation: stepping between slides and settling
 *   - init and cleanup, then the input loop and carousel_run() entry point
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
#include "system/app_buffer.h"           /* app_get_buffer() -- see init() */
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
#include "usb.h"                     /* SYS_USB_CONNECTED handling */
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
#include "widgets/menu.h"             /* MENUITEM_STRINGLIST, do_menu */
#include "draw/bmp.h"              /* read_bmp_file */
#include "draw/jpeg_load.h"        /* read_jpeg_file */
#include "power.h"
#include "backlight.h"        /* backlight_set_timeout(_plugged) */
#include "cpu.h"
#include "skin/skin_engine.h"  /* skin_inhibit_flush */
#include "skin/skin_albumart_color.h" /* dynamic_colors_resolve */
#include "skin/statusbar_skinned.h" /* sb_set_persistent_title */
#include "album_covers.h"
#include "carousel.h"     /* shared carousel engine interface (pf_idx, model, ...) */

/******************************* Globals ***********************************/
static fb_data *lcd_fb;

#define PF_PREV ACTION_STD_PREV
#define PF_PREV_REPEAT ACTION_STD_PREVREPEAT
#define PF_NEXT ACTION_STD_NEXT
#define PF_NEXT_REPEAT ACTION_STD_NEXTREPEAT
#define PF_SELECT ACTION_STD_OK
#define PF_BACK ACTION_STD_CANCEL
#define PF_MENU ACTION_STD_MENU
#define PF_WPS ACTION_TREE_WPS
#define PF_JMP ACTION_LISTTREE_PGDOWN
#define PF_JMP_PREV ACTION_LISTTREE_PGUP

#define PF_QUIT (LAST_ACTION_PLACEHOLDER + 1)
#define PF_TRACKLIST (LAST_ACTION_PLACEHOLDER + 2)
#define PF_SORTING_NEXT (LAST_ACTION_PLACEHOLDER + 3)
#define PF_SORTING_PREV (LAST_ACTION_PLACEHOLDER + 4)

/* Both this fork's targets are CONFIG_KEYPAD == IPOD_4G_PAD (confirmed via
 * firmware/export/config/ipod6g.h and ipodvideo.h), so only that keypad's
 * button mapping is kept -- every other target's mapping the original
 * cross-target plugin supported has been dropped rather than left as dead
 * conditional code. */
const struct button_mapping pf_context_album_scroll[] =
{
    {PF_JMP_PREV,     BUTTON_LEFT,                BUTTON_NONE},
    {PF_JMP_PREV,     BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_NONE},
    {PF_JMP,          BUTTON_RIGHT,               BUTTON_NONE},
    {PF_JMP,          BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_NONE},
    {ACTION_NONE,     BUTTON_LEFT|BUTTON_REL,     BUTTON_LEFT},
    {ACTION_NONE,     BUTTON_RIGHT|BUTTON_REL,    BUTTON_RIGHT},
    {ACTION_NONE,     BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_LEFT},
    {ACTION_NONE,     BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_RIGHT},
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_PLUGIN|1)
};

const struct button_mapping pf_context_buttons[] =
{
    {PF_MENU,         BUTTON_MENU|BUTTON_REPEAT,  BUTTON_MENU},
    {PF_QUIT,         BUTTON_MENU|BUTTON_REL,     BUTTON_MENU},
    {PF_SORTING_NEXT, BUTTON_SELECT|BUTTON_MENU,  BUTTON_NONE},
    {PF_SORTING_PREV, BUTTON_SELECT|BUTTON_PLAY,  BUTTON_NONE},
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_TREE)
};
const struct button_mapping *pf_contexts[] =
{
    pf_context_album_scroll,
    pf_context_buttons
};

#define LCD_BUF lcd_fb
#define G_BRIGHT(y) LCD_RGBPACK(y,y,y)
#define BUFFER_WIDTH LCD_WIDTH
/* pix_t, the index structs, the scroll-line enum, the SUCCESS/ERROR_* codes and
 * struct carousel_model now live in carousel.h (shared with artist_portraits.c). */

static pix_t pf_bg_color;     /* theme background, replaces G_BRIGHT(0) */
pix_t pf_fg_color;     /* theme foreground, replaces G_BRIGHT(255) */
static pix_t pf_lss_color;    /* selector start color for gradient */
static pix_t pf_lse_color;    /* selector end color for gradient */
static pix_t pf_lst_color;    /* selector text color */

/* Pre-extracted RGB565 channel masks for fade_color() performance */
static unsigned int pf_bg_rb;  /* pf_bg_color & 0xf81f */
static unsigned int pf_bg_g;   /* pf_bg_color & 0x7e0 */

static void pf_update_dynamic_colors(void)
{
    /* The setting picks which of the theme's two colours fills the screen;
     * the captions always take the other. */
    bool bg_from_fg = global_settings.album_covers_background
                        == CAROUSEL_BG_FOREGROUND;
    int fill = bg_from_fg ? global_settings.fg_color : global_settings.bg_color;
    int text = bg_from_fg ? global_settings.bg_color : global_settings.fg_color;

    pf_bg_color = (pix_t)dynamic_colors_resolve(fill);
    pf_fg_color = (pix_t)dynamic_colors_resolve(text);
    pf_lss_color = (pix_t)dynamic_colors_resolve(global_settings.lss_color);
    pf_lse_color = (pix_t)dynamic_colors_resolve(global_settings.lse_color);
    pf_lst_color = (pix_t)dynamic_colors_resolve(global_settings.lst_color);
    pf_bg_rb = pf_bg_color & 0xf81f;
    pf_bg_g  = pf_bg_color & 0x7e0;
}

/* for fixed-point arithmetic, we need minimum 32-bit long
   long long (64-bit) might be useful for multiplication and division */
#define PFREAL_SHIFT 10
#define PFREAL_ONE (1 << PFREAL_SHIFT)
#define PFREAL_HALF (PFREAL_ONE >> 1)

#define IANGLE_MAX 1024
#define IANGLE_MASK 1023

#define DISPLAY_HEIGHT (LCD_HEIGHT * 2 / 3)
#define DISPLAY_WIDTH MAX((LCD_HEIGHT * LCD_PIXEL_ASPECT_HEIGHT / \
    LCD_PIXEL_ASPECT_WIDTH / 2), (LCD_WIDTH * 2 / 5))
#define CAM_DIST MAX(MIN(LCD_HEIGHT,LCD_WIDTH),120)
#define CAM_DIST_R (CAM_DIST << PFREAL_SHIFT)
#define DISPLAY_LEFT_R (PFREAL_HALF - LCD_WIDTH * PFREAL_HALF)
#define MAXSLIDE_LEFT_R (PFREAL_HALF - DISPLAY_WIDTH * PFREAL_HALF)


/* Caption lines are inset from the screen edges by this much. A caption long
 * enough to scroll has to overhang its box to reveal its tail, so the inset
 * is a clip region (see carousel_text_begin()) and not just a starting
 * offset: without it a scrolling album name runs right into the bezel. */
#define PF_TEXT_MARGIN 20
#define PF_TEXT_WIDTH  (LCD_WIDTH - 2 * PF_TEXT_MARGIN)

#define MAX_SLIDES_COUNT 10
/* Number of side slides rendered per side (3 visible + 1 animation buffer).
 * Was a user-configurable field in the original plugin's config struct, but
 * was never actually exposed in any settings menu, so it's now a fixed
 * constant. */
#define ALBUM_COVERS_NUM_SLIDES 4

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
#define THREAD_STACK_SIZE DEFAULT_STACK_SIZE + 0x200

#define EV_EXIT 9999
#define EV_WAKEUP 1337

/* Ordered Bayer dithering for 24-bit to RGB565 conversion */
static const unsigned char pf_dither_table[16] =
    {   0,192, 48,240, 12,204, 60,252,  3,195, 51,243, 15,207, 63,255 };
#define PF_DITHERY(y) (pf_dither_table[(y) & 15] & 0xAA)
#define PF_DITHERX(x) (pf_dither_table[(x) & 15])
#define PF_DITHERXDY(x,dy) (PF_DITHERX(x) ^ dy)

/* some magic numbers for cache_version. */

/* current version for cover cache */
#define CONFIG_FILE ROCKBOX_DIR "/album_covers.cfg"

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

static struct albumart_t aa_cache;
struct pf_config_t pf_cfg;

/** below we allocate the memory we want to use **/

static pix_t *buffer; /* for now it always points to the lcd framebuffer */
int pf_height;           /* viewport height (LCD_HEIGHT minus status bar) */
static int pf_half_height;      /* rows of drawable (post text-margin) height above center */
static int pf_lower_half;       /* rows of drawable (post text-margin) height below center */
static int pf_draw_y_shift;     /* rows skipped below pf_vp_y for a TOP text caption's margin */
/* Album name font: global_settings.bold_font_file loaded via font_load(),
 * or the real UI font if none is configured. Provided by font_get_ui_bold()
 * (settings.c), which owns the font's lifecycle -- this screen must not unload
 * it. Set once in init(). */
int pf_bold_font;
static int pf_vp_y;             /* viewport y-offset (status bar height) */
static struct viewport pf_vp;   /* PF rendering viewport (below status bar) */
static struct frame_buffer_t pf_framebuffer; /* bypass backdrop in clear_viewport */
static bool pf_show_statusbar;  /* this session's album_covers_statusbar */
/* Theme state left pushed for cleanup() to pop. cleanup() also runs after a
 * failed init(), so it cannot assume there is one. */
static bool pf_theme_pushed;

static struct slide_data center_slide;
static struct slide_data left_slides[MAX_SLIDES_COUNT];
static struct slide_data right_slides[MAX_SLIDES_COUNT];
static int slide_frame;
static int step;
static int target;
static int fade;
/* Always 0: nothing sets it any more, but render_all_slides()'s fade
 * arithmetic still reads it, so it stays as a no-op term. */
static int extra_fade;
int center_index = 0; /* index of the slide that is in the center */
static int itilt;
static PFreal offsetX;
static PFreal auto_slide_spacing;
static PFreal offsetY;
static int number_of_slides;

static struct pf_slide_cache pf_sldcache;

/* use long for aligning */
unsigned long thread_stack[THREAD_STACK_SIZE / sizeof(long)];
/* queue (as array) for scheduling load_surface */

static int empty_slide_hid;

/* Index of the "coverflow" size in the shared albumart cache (art_sizes.h),
 * resolved once in init(); -1 if unavailable (falls back to the local pfraw
 * cache). */
static int pf_cover_size_idx = -1;

unsigned int thread_id;
struct event_queue thread_q;

static struct buflib_context buf_ctx;

struct pf_index_t pf_idx;

static bool thread_is_running;
static bool wants_to_quit;

/*
    Prevent picture loading thread from allocating
    buflib memory while the main thread may be
    performing buffer-shifting operations.
*/
static struct mutex buf_ctx_mutex;
static bool buf_ctx_locked;

static struct pf_scroll_line_info scroll_line_info;
static struct pf_scroll_line scroll_lines[PF_MAX_SCROLL_LINES];

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

static int pf_state;

/** code */
static bool free_slide_prio(int prio);
bool load_new_slide(void);
int load_surface(int);
static void free_all_slide_prio(int prio);

/* The artist-portraits model lives in artist_portraits.c (over the same
 * carousel engine). The active model is selected by the entry point
 * (album_covers / artist_portraits) before init() runs. */
static const struct carousel_model *model = NULL;

static inline void buf_ctx_lock(void)
{
    mutex_lock(&buf_ctx_mutex);
    buf_ctx_locked = true;
}

static inline void buf_ctx_unlock(void)
{
    mutex_unlock(&buf_ctx_mutex);
    buf_ctx_locked = false;
}

/* Minimal stand-in for the plugin-only apps/plugins/lib/configfile.c (which
 * can't be reused as-is -- it derives its file path via
 * plugin_get_current_filename(), a genuinely plugin-lifecycle-only call).
 * This is deliberately bare-bones (a single fixed path, no version-gated
 * migration) since it's a transitional bridge -- real settings persistence
 * moves to global_settings/settings_list.c shortly after this lands. Reuses
 * the exact same line-parsing core functions (read_line/settings_parseline,
 * apps/misc.h) the plugin-lib version itself wraps via the plugin API's
 * function-pointer indirection. */
#define TYPE_INT  1
#define TYPE_ENUM 2
#define TYPE_BOOL 4

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

static void config_load(const char *filename, const struct configdata *cfg,
                        int num_items)
{
    int fd;
    int i, j;
    char *name;
    char *val;
    char buf[128];
    int tmp;

    fd = open(filename, O_RDONLY);
    if (fd < 0)
        return;

    while (read_line(fd, buf, sizeof(buf)) > 0)
    {
        settings_parseline(buf, &name, &val);
        for (i = 0; i < num_items; i++)
        {
            if (strcmp(cfg[i].name, name))
                continue;
            switch (cfg[i].type)
            {
                case TYPE_INT:
                    tmp = atoi(val);
                    if (tmp >= cfg[i].min && tmp <= cfg[i].max)
                        *cfg[i].int_p = tmp;
                    break;
                case TYPE_BOOL:
                    *cfg[i].bool_p = (bool)atoi(val);
                    break;
                case TYPE_ENUM:
                    for (j = 0; j < cfg[i].max; j++)
                    {
                        if (!strcmp(cfg[i].values[j], val))
                            *cfg[i].int_p = j;
                    }
                    break;
            }
            break;
        }
    }
    close(fd);
}

static void config_save(const char *filename, const struct configdata *cfg,
                        int num_items)
{
    int fd = creat(filename, 0666);
    int i;

    if (fd < 0)
        return;

    for (i = 0; i < num_items; i++)
    {
        switch (cfg[i].type)
        {
            case TYPE_INT:
                fdprintf(fd, "%s: %d\n", cfg[i].name, *cfg[i].int_p);
                break;
            case TYPE_BOOL:
                fdprintf(fd, "%s: %d\n", cfg[i].name, (int)*cfg[i].bool_p);
                break;
            case TYPE_ENUM:
                fdprintf(fd, "%s: %s\n", cfg[i].name,
                         cfg[i].values[*cfg[i].int_p]);
                break;
        }
    }
    close(fd);
}

static struct configdata config[] =
{
    { TYPE_INT, 0, 100, { .int_p = &pf_cfg.cache_version }, "cache version", NULL },
    { TYPE_BOOL, 0, 1, { .bool_p = &pf_cfg.update_albumart }, "update albumart", NULL },
    { TYPE_INT, 0, 999999, { .int_p = &pf_cfg.last_album }, "last album", NULL },
    { TYPE_INT, 0, 999999, { .int_p = &aa_cache.idx }, "art cache pos", NULL },
    { TYPE_INT, 0, 999999, { .int_p = &aa_cache.inspected }, "art cache inspected", NULL },
};

#define CONFIG_NUM_ITEMS (sizeof(config) / sizeof(struct configdata))

void pf_config_save(void)
{
    config_save(CONFIG_FILE, config, CONFIG_NUM_ITEMS);
}

static void pf_config_load(void)
{
    config_load(CONFIG_FILE, config, CONFIG_NUM_ITEMS);
}

static bool check_database(void)
{
    bool needwarn = true;
    int spin = 5;

    struct tagcache_stat *stat = tagcache_get_stat();

    while ( !(stat->initialized && stat->ready) )
    {
        if (--spin > 0)
        {
            sleep(HZ/5);
        }
        else if (needwarn)
        {
            /* Waiting on the database is activity, not an error -- show it in
             * the status bar rather than over the carousel. */
            needwarn = false;
            ui_set_working(true);
        }
        else
        {
            ui_set_working(false);
            return false;
        }

        yield();
        stat = tagcache_get_stat();
    }
    ui_set_working(false);
    return true;
}

static void config_set_defaults(struct pf_config_t *cfg)
{
     cfg->cache_version = CACHE_REBUILD;
     cfg->update_albumart = false;
     cfg->last_album = 0;
}

static inline PFreal fmul(PFreal a, PFreal b)
{
    return (a*b) >> PFREAL_SHIFT;
}

/**
 * This version preshifts each operand, which is useful when we know how many
 * of the least significant bits will be empty, or are worried about overflow
 * in a particular calculation
 */
static inline PFreal fmuln(PFreal a, PFreal b, int ps1, int ps2)
{
    return ((a >> ps1) * (b >> ps2)) >> (PFREAL_SHIFT - ps1 - ps2);
}

/* ARMv5+ has a clz instruction equivalent to our function.
 */
#if (defined(CPU_ARM) && (ARM_ARCH > 4))
static inline int clz(uint32_t v)
{
    return __builtin_clz(v);
}

/* Otherwise, use our clz, which can be inlined */
#else
static const char clz_lut[16] = { 4, 3, 2, 2, 1, 1, 1, 1,
                                  0, 0, 0, 0, 0, 0, 0, 0 };
/* This clz is based on the log2(n) implementation at
 * http://graphics.stanford.edu/~seander/bithacks.html#IntegerLogLookup
 * It is not any faster than the one above, but trades 16B in the lookup table
 * for a savings of 12B per each inlined call.
 */
static inline int clz(uint32_t v)
{
    int r = 28;
    if (v >= 0x10000)
    {
        v >>= 16;
        r -= 16;
    }
    if (v & 0xff00)
    {
        v >>= 8;
        r -= 8;
    }
    if (v & 0xf0)
    {
        v >>= 4;
        r -= 4;
    }
    return r + clz_lut[v];
}
#endif

/* Return the maximum possible left shift for a signed int32, without
 * overflow
 */
static inline int allowed_shift(int32_t val)
{
    uint32_t uval = val ^ (val >> 31);
    return clz(uval) - 1;
}

/* Calculate num/den, with the result shifted left by PFREAL_SHIFT, by shifting
 * num and den before dividing.
 */
static inline PFreal fdiv(PFreal num, PFreal den)
{
    int shift = allowed_shift(num);
    shift = MIN(PFREAL_SHIFT, shift);
    num <<= shift;
    den >>= PFREAL_SHIFT - shift;
    return num / den;
}

#define fmin(a,b) (((a) < (b)) ? (a) : (b))
#define fmax(a,b) (((a) > (b)) ? (a) : (b))
#define fabs(a) (a < 0 ? -a : a)
#define fbound(min,val,max) (fmax((min),fmin((max),(val))))


/* warning: regenerate the table if IANGLE_MAX and PFREAL_SHIFT are changed! */
static const short sin_tab[] = {
        0,   100,   200,   297,   392,   483,   569,   650,
      724,   792,   851,   903,   946,   980,  1004,  1019,
     1024,  1019,  1004,   980,   946,   903,   851,   792,
      724,   650,   569,   483,   392,   297,   200,   100,
        0,  -100,  -200,  -297,  -392,  -483,  -569,  -650,
     -724,  -792,  -851,  -903,  -946,  -980, -1004, -1019,
    -1024, -1019, -1004,  -980,  -946,  -903,  -851,  -792,
     -724,  -650,  -569,  -483,  -392,  -297,  -200,  -100,
        0
};

static inline PFreal fsin(int iangle)
{
    iangle &= IANGLE_MASK;

    int i = (iangle >> 4);
    PFreal p = sin_tab[i];
    PFreal q = sin_tab[(i+1)];
    PFreal g = (q - p);
    return p + g * (iangle-i*16)/16;
}

static inline PFreal fcos(int iangle)
{
    return fsin(iangle + (IANGLE_MAX >> 2));
}

static void output_row_8_transposed(uint32_t row, void * row_in,
                                       struct scaler_context *ctx)
{
    pix_t *dest = (pix_t*)ctx->bm->data + row;
    pix_t *end = dest + ctx->bm->height * ctx->bm->width;
    struct uint8_rgb *qp = (struct uint8_rgb*)row_in;
    unsigned r, g, b;
    int col = 0;
    uint8_t dy = PF_DITHERY(row);
    for (; dest < end; dest += ctx->bm->height, col++)
    {
        int delta = ctx->dither ? PF_DITHERXDY(col, dy) : 127;
        r = (31 * qp->red + (qp->red >> 3) + delta) >> 8;
        g = (63 * qp->green + (qp->green >> 2) + delta) >> 8;
        b = (31 * qp->blue + (qp->blue >> 3) + delta) >> 8;
        qp++;
        *dest = FB_RGBPACK_LCD(r, g, b);
    }
}

/* read_image_file() is called without FORMAT_TRANSPARENT so
 * it's safe to ignore alpha channel in the next two functions */
static void output_row_32_transposed(uint32_t row, void * row_in,
                                       struct scaler_context *ctx)
{
    pix_t *dest = (pix_t*)ctx->bm->data + row;
    pix_t *end = dest + ctx->bm->height * ctx->bm->width;
    struct uint32_argb *qp = (struct uint32_argb*)row_in;
    int r, g, b;
    int col = 0;
    uint8_t dy = PF_DITHERY(row);
    for (; dest < end; dest += ctx->bm->height, col++)
    {
        int delta = ctx->dither ? PF_DITHERXDY(col, dy) : 127;
        r = SC_OUT(qp->r, ctx);
        g = SC_OUT(qp->g, ctx);
        b = SC_OUT(qp->b, ctx);
        qp++;
        r = (31 * r + (r >> 3) + delta) >> 8;
        g = (63 * g + (g >> 2) + delta) >> 8;
        b = (31 * b + (b >> 3) + delta) >> 8;
        *dest = FB_RGBPACK_LCD(r, g, b);
    }
}

static void output_row_32_transposed_fromyuv(uint32_t row, void * row_in,
                                       struct scaler_context *ctx)
{
    pix_t *dest = (pix_t*)ctx->bm->data + row;
    pix_t *end = dest + ctx->bm->height * ctx->bm->width;
    struct uint32_argb *qp = (struct uint32_argb*)row_in;
    int col = 0;
    uint8_t dy = PF_DITHERY(row);
    for (; dest < end; dest += ctx->bm->height, col++)
    {
        unsigned r, g, b, y, u, v;
        int delta = ctx->dither ? PF_DITHERXDY(col, dy) : 127;
        y = SC_OUT(qp->b, ctx);
        u = SC_OUT(qp->g, ctx);
        v = SC_OUT(qp->r, ctx);
        qp++;
        yuv_to_rgb(y, u, v, &r, &g, &b);
        r = (31 * r + (r >> 3) + delta) >> 8;
        g = (63 * g + (g >> 2) + delta) >> 8;
        b = (31 * b + (b >> 3) + delta) >> 8;
        *dest = FB_RGBPACK_LCD(r, g, b);
    }
}

static unsigned int get_size(struct bitmap *bm)
{
    return bm->width * bm->height * sizeof(pix_t);
}

const struct custom_format format_transposed = {
    .output_row_8 = output_row_8_transposed,
    .output_row_32 = {
        output_row_32_transposed,
        output_row_32_transposed_fromyuv
    },
    .get_size = get_size
};

static const struct button_mapping* get_context_map(int context)
{
    /* action_code_lookup() ORs in CONTEXT_REMOTE for any button with
     * BUTTON_REMOTE bits set (true on this target, which supports an
     * accessory/headphone remote) -- without masking it here too,
     * pf_contexts[context & ~CONTEXT_PLUGIN] indexes ~2 billion entries
     * past this 2-element array on a remote button press. */
    context &= ~(CONTEXT_LOCKED | CONTEXT_REMOTE);
    return pf_contexts[context & ~CONTEXT_PLUGIN];
}

/* scrolling */
static void init_scroll_lines(void)
{
    int i;
    static const char scroll_tick_table[16] = {
     /* Hz values:
        1, 1.25, 1.55, 2, 2.5, 3.12, 4, 5, 6.25, 8.33, 10, 12.5, 16.7, 20, 25, 33 */
        100, 80, 64, 50, 40, 32, 25, 20, 16, 12, 10, 8, 6, 5, 4, 3
    };

    scroll_line_info.ticks = scroll_tick_table[global_settings.scroll_speed];
    scroll_line_info.step = global_settings.scroll_step;
    scroll_line_info.delay = global_settings.scroll_delay / (HZ / 10);
    scroll_line_info.next_scroll = current_tick;
    for (i = 0; i < PF_MAX_SCROLL_LINES; i++)
        scroll_lines[i].step = 0;
}

void set_scroll_line(const char *str, enum pf_scroll_line_type type)
{
    struct pf_scroll_line *s = &scroll_lines[type];
    s->width = lcd_getstringsize(str, NULL, NULL);
    s->step = 0;
    s->offset = 0;
    s->start_tick = current_tick + scroll_line_info.delay;
    /* Offsets are relative to the inset caption box, so 0 is PF_TEXT_MARGIN
     * in from the screen edge -- carousel_text_begin() supplies the origin. */
    if (PF_TEXT_WIDTH - s->width < 0)
        s->step = scroll_line_info.step;
    else
        s->offset = (PF_TEXT_WIDTH - s->width) / 2;
}

/* Clip and shift drawing to the caption box for the duration of one caption.
 * Callers restore with carousel_text_end(). Enter this before setting the
 * font or colour: both are properties of the current viewport, so anything
 * set beforehand is lost on the switch. */
struct viewport *carousel_text_begin(void)
{
    static struct viewport text_vp;

    text_vp = pf_vp;
    if (pf_vp.width > 2 * PF_TEXT_MARGIN)
    {
        text_vp.x = pf_vp.x + PF_TEXT_MARGIN;
        text_vp.width = pf_vp.width - 2 * PF_TEXT_MARGIN;
    }
    return lcd_set_viewport(&text_vp);
}

void carousel_text_end(struct viewport *saved)
{
    lcd_set_viewport(saved);
}

int get_scroll_line_offset(enum pf_scroll_line_type type)
{
    return scroll_lines[type].offset;
}

static void update_scroll_lines(void)
{
    int i;

    if (TIME_BEFORE(current_tick, scroll_line_info.next_scroll))
        return;

    scroll_line_info.next_scroll = current_tick + scroll_line_info.ticks;

    for (i = 0; i < PF_MAX_SCROLL_LINES; i++)
    {
        struct pf_scroll_line *s = &scroll_lines[i];
        if (s->step && TIME_BEFORE(s->start_tick, current_tick))
        {
            s->offset -= s->step;

            if (s->offset >= 0) {
                /* at beginning of line */
                s->offset = 0;
                s->step = scroll_line_info.step;
                s->start_tick = current_tick + scroll_line_info.delay * 2;
            }
            if (s->offset <= PF_TEXT_WIDTH - s->width) {
                /* at end of line */
                s->offset = PF_TEXT_WIDTH - s->width;
                s->step = -scroll_line_info.step;
                s->start_tick = current_tick + scroll_line_info.delay * 2;
            }
        }
    }
}


/**
 Save the given bitmap as filename in the pfraw format
 */
static bool save_pfraw(char* filename, struct bitmap *bm)
{
    struct pfraw_header bmph;
    bmph.width = bm->width;
    bmph.height = bm->height;
    int fh = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if( fh < 0 ) return false;
    write( fh, &bmph, sizeof( struct pfraw_header ) );
    write( fh, bm->data , sizeof( pix_t ) * bm->width *  bm->height );
    close( fh );
    return true;
}

/* Placeholder slide fill: a plain box, a touch off the dark theme background,
 * shown while a cover loads or when a folder has none. Deliberately not the
 * bold podboxnoart -- on a carousel of unloaded slides that was too loud. */
#define EMPTY_SLIDE_COLOR LCD_RGBPACK(0x12, 0x16, 0x20)

/**
  Create the placeholder slide, shown while loading or when no cover was found.
 */
static int create_empty_slide(bool force)
{
    if (!aa_cache.buf)
        return false;

    if ( force || ! file_exists( EMPTY_SLIDE ) )  {
        pix_t *px = (pix_t*)aa_cache.buf;
        int n = DISPLAY_WIDTH * DISPLAY_WIDTH;
        int i;

        for (i = 0; i < n; i++)
            px[i] = EMPTY_SLIDE_COLOR;

        aa_cache.input_bmp.width = DISPLAY_WIDTH;
        aa_cache.input_bmp.height = DISPLAY_WIDTH;
        aa_cache.input_bmp.format = FORMAT_NATIVE;
        aa_cache.input_bmp.data = (char*)aa_cache.buf;

        if (!save_pfraw(EMPTY_SLIDE, &aa_cache.input_bmp))
            return false;
    }

    return true;
}

/**
 Thread used for loading and preparing bitmaps in the background
 */
static void thread(void)
{
    /* SSD mode: poll more frequently since disk access is cheap */
    long sleep_time = (global_settings.storage_mode == 2)
                      ? HZ : 5 * HZ;
    struct queue_event ev;
    while (1) {
        queue_wait_w_tmo(&thread_q, &ev, sleep_time);
        switch (ev.id) {
            case EV_EXIT:
                return;
            case EV_WAKEUP:
                /* we just woke up */
                break;
            case SYS_USB_CONNECTED:
                /* This queue is in the broadcast list, so the storage handover
                 * counts an acknowledgement from it and will not give the host
                 * the disk until it arrives. Then park: loading a slide reads
                 * a disk that is no longer ours to read.
                 *
                 * Parked by hand rather than with usb_wait_for_disconnect(),
                 * which discards every event but the disconnect -- including
                 * the EV_EXIT that end_pf_thread() posts before waiting on
                 * this thread, which would then wait forever. */
                usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
                while (1) {
                    queue_wait(&thread_q, &ev);
                    if (ev.id == SYS_USB_DISCONNECTED)
                        break;
                    if (ev.id == EV_EXIT)
                        return;
                }
                continue;
        }

        if(ev.id != SYS_TIMEOUT) {
            bool usb_pending = false;
            while (1) {
                /* Drain the wakeups that piled up while we were loading, and
                 * keep going, rather than abandoning the run to fetch them.
                 *
                 * A wakeup means the centre slide moved. It does not mean this
                 * run is stale: load_new_slide() reads center_index afresh
                 * every call, so the next load already aims at the new
                 * neighbourhood -- going back through the queue to learn that
                 * changes nothing except when the loading resumes. It used to
                 * abandon the run on any pending event, which during a scroll
                 * meant roughly one slide loaded per centre change, against the
                 * 2 * ALBUM_COVERS_NUM_SLIDES + 1 a moving centre needs. The
                 * loader lost ground for as long as the animation ran and only
                 * caught up once it stopped, which is what "the art appears
                 * after the animation settles" was. */
                while (!queue_empty(&thread_q)) {
                    queue_wait_w_tmo(&thread_q, &ev, 0);
                    if (ev.id == EV_EXIT)
                        return;
                    if (ev.id == SYS_USB_CONNECTED) {
                        /* Put it back and stop the run: the acknowledgement
                         * belongs in one place, and the outer loop is it. */
                        queue_post(&thread_q, ev.id, ev.data);
                        usb_pending = true;
                        break;
                    }
                }
                if (usb_pending)
                    break;

                buf_ctx_lock();
                bool slide_loaded = load_new_slide();
                buf_ctx_unlock();
                if (!slide_loaded)
                    break;
                yield();
            }
        }
    }
}


/**
 End the thread by posting the EV_EXIT event
 */
static void end_pf_thread(void)
{
    if ( thread_is_running ) {
        queue_post(&thread_q, EV_EXIT, 0);
        thread_wait(thread_id);
        /* remove the thread's queue from the broadcast list */
        queue_delete(&thread_q);
        thread_is_running = false;
    }
}


/**
 Create the thread an setup the event queue
 */
static bool create_pf_thread(void)
{
    /* put the thread's queue in the bcast list */
    queue_init(&thread_q, true);
    if ((thread_id = create_thread(
                           thread,
                           thread_stack,
                           sizeof(thread_stack),
                            0,
                           "Picture load thread"
                               IF_PRIO(, PRIORITY_BUFFERING)
                               IF_COP(, CPU)
                                      )
        ) == 0) {
        return false;
    }
    thread_is_running = true;
    queue_post(&thread_q, EV_WAKEUP, 0);
    return true;
}


static void initialize_slide_cache(void)
{
    int i= 0;
    for (i = 0; i < SLIDE_CACHE_SIZE; i++) {
        pf_sldcache.cache[i].hid = 0;
        pf_sldcache.cache[i].index = 0;
        pf_sldcache.cache[i].next = i + 1;
        pf_sldcache.cache[i].prev = i - 1;
    }
    pf_sldcache.cache[0].prev = i - 1;
    pf_sldcache.cache[i - 1].next = 0;

    pf_sldcache.free = 0;
    pf_sldcache.used = -1;
    pf_sldcache.left_idx = -1;
    pf_sldcache.right_idx = -1;
    pf_sldcache.center_idx = -1;
}


/*
 * The following functions implement the linked-list-in-array used to manage
 * the LRU cache of slides, and the list of free cache slots.
 */

#define _SEEK_RIGHT_WHILE(start, cond) \
({ \
    int ind_, next_ = (start); \
    int i_ = 0; \
    do { \
        ind_ = next_; \
        next_ = pf_sldcache.cache[ind_].next; \
        i_++; \
    } while (next_ != pf_sldcache.used && (cond) && i_ < SLIDE_CACHE_SIZE); \
    if (i_ >= SLIDE_CACHE_SIZE) \
    /* TODO: Not supposed to happen */ \
        ind_ = -1; \
    ind_; \
})

#define _SEEK_LEFT_WHILE(start, cond) \
({ \
    int ind_, next_ = (start); \
    int i_ = 0; \
    do { \
        ind_ = next_; \
        next_ = pf_sldcache.cache[ind_].prev; \
        i_++; \
    } while (ind_ != pf_sldcache.used && (cond) && i_ < SLIDE_CACHE_SIZE); \
    if (i_ >= SLIDE_CACHE_SIZE) \
    /* TODO: Not supposed to happen */ \
        ind_ = -1; \
    ind_; \
})

/**
 Pop the given item from the linked list starting at *head, returning the next
 item, or -1 if the list is now empty.
*/
static inline int lla_pop_item (int *head, int i)
{
    int prev = pf_sldcache.cache[i].prev;
    int next = pf_sldcache.cache[i].next;
    if (i == next)
    {
        *head = -1;
        return -1;
    }
    else if (i == *head)
        *head = next;
    pf_sldcache.cache[next].prev = prev;
    pf_sldcache.cache[prev].next = next;
    return next;
}


/**
 Pop the head item from the list starting at *head, returning the index of the
 item, or -1 if the list is already empty.
*/
static inline int lla_pop_head (int *head)
{
    int i = *head;
    if (i != -1)
        lla_pop_item(head, i);
    return i;
}

/**
 Insert the item at index i before the one at index p.
*/
static inline void lla_insert (int i, int p)
{
    int next = p;
    int prev = pf_sldcache.cache[next].prev;
    pf_sldcache.cache[next].prev = i;
    pf_sldcache.cache[prev].next = i;
    pf_sldcache.cache[i].next = next;
    pf_sldcache.cache[i].prev = prev;
}


/**
 Insert the item at index i at the end of the list starting at *head.
*/
static inline void lla_insert_tail (int *head, int i)
{
    if (*head == -1)
    {
        *head = i;
        pf_sldcache.cache[i].next = i;
        pf_sldcache.cache[i].prev = i;
    } else
        lla_insert(i, *head);
}

/**
 Insert the item at index i before the one at index p.
*/
static inline void lla_insert_after(int i, int p)
{
    p = pf_sldcache.cache[p].next;
    lla_insert(i, p);
}


/**
 Insert the item at index i before the one at index p in the list starting at
 *head
*/
static inline void lla_insert_before(int *head, int i, int p)
{
    lla_insert(i, p);
    if (*head == p)
        *head = i;
}


/**
 Free the used slide at index i, and its buffer, and move it to the free
 slides list.
*/
static inline void free_slide(int i)
{
    if (pf_sldcache.cache[i].hid != empty_slide_hid)
        buflib_free(&buf_ctx, pf_sldcache.cache[i].hid);
    pf_sldcache.cache[i].index = -1;
    lla_pop_item(&pf_sldcache.used, i);
    lla_insert_tail(&pf_sldcache.free, i);
    if (pf_sldcache.used == -1)
    {
        pf_sldcache.right_idx = -1;
        pf_sldcache.left_idx = -1;
        pf_sldcache.center_idx = -1;
    }
}


/**
 Free one slide ranked above the given priority. If no such slide can be found,
 return false.
*/
static bool free_slide_prio(int prio)
{
    if (pf_sldcache.used == -1)
        return false;

    int i, prio_max;
    int l = pf_sldcache.used;
    int r = pf_sldcache.cache[pf_sldcache.used].prev;

    int prio_l = pf_sldcache.cache[l].index < center_index ?
           center_index - pf_sldcache.cache[l].index : 0;
    int prio_r = pf_sldcache.cache[r].index > center_index ?
           pf_sldcache.cache[r].index - center_index : 0;
    if (prio_l > prio_r)
    {
        i = l;
        prio_max = prio_l;
    } else {
        i = r;
        prio_max = prio_r;
    }
    if (prio_max > prio)
    {
        if (i == pf_sldcache.left_idx)
            pf_sldcache.left_idx = pf_sldcache.cache[i].next;
        if (i == pf_sldcache.right_idx)
            pf_sldcache.right_idx = pf_sldcache.cache[i].prev;
        free_slide(i);
        return true;
    } else
        return false;
}


/**
 Free all slides ranked above the given priority.
*/
static void free_all_slide_prio(int prio)
{
    while (free_slide_prio(prio))
    {;;}
}


/**
 Read the pfraw image given as filename and return the hid of the buffer
 */
static int read_pfraw(char* filename, int prio)
{
    struct pfraw_header bmph;
    int fh = open(filename, O_RDONLY);
    if( fh < 0 ) {
        /* pf_cfg.cache_version = CACHE_UPDATE; -- don't invalidate on missing pfraw */
        return empty_slide_hid;
    }

    /* A short read (truncated/corrupted cache file, e.g. from a power-off
     * mid-write) leaves bmph partially or fully uninitialized, and a
     * width/height larger than anything save_pfraw() could have written
     * (bounded by DISPLAY_WIDTH/DISPLAY_HEIGHT) means the file is corrupt
     * either way -- trusting either lets the size calculation below and
     * every later access via bm->width/height walk arbitrarily far past
     * this allocation. Treat both as "no cached image" rather than
     * propagating garbage dimensions. */
    if (read(fh, &bmph, sizeof(struct pfraw_header)) != sizeof(struct pfraw_header) ||
        bmph.width <= 0 || bmph.height <= 0 ||
        bmph.width > DISPLAY_WIDTH || bmph.height > DISPLAY_HEIGHT)
    {
        close(fh);
        return empty_slide_hid;
    }

    int size =  sizeof(struct dim) +
                sizeof( pix_t ) * bmph.width * bmph.height;

    int hid;
    do {
        hid = buflib_alloc(&buf_ctx, size);
    } while (hid < 0 && free_slide_prio(prio));

    if (hid < 0) {
        close( fh );
        return -1;
    }

    struct dim *bm = buflib_get_data(&buf_ctx, hid);

    bm->width = bmph.width;
    bm->height = bmph.height;
    pix_t *data = (pix_t*)(sizeof(struct dim) + (char *)bm);

    read( fh, data , sizeof( pix_t ) * bm->width * bm->height );
    close( fh );
    return hid;
}

/* Read a shared-cache thumbnail from an open fd (.aat: struct art_cache_header
 * followed by native pixels) into a buflib surface in the column-major layout
 * render_slide() expects. The fd stays the caller's to close.
 *
 * Takes an fd rather than a path because the caller has to open the file
 * anyway, and asking the cache whether it exists first would be a second
 * directory lookup for the same answer.
 *
 * Returns a buflib handle, empty_slide_hid on a corrupt file, or -1 on
 * allocation failure. */
static int read_aat(int fh, int prio)
{
    struct art_cache_header hdr;
    pix_t rowbuf[DISPLAY_WIDTH];
    int row, col, w, h, size, hid;

    if (read(fh, &hdr, sizeof(hdr)) != sizeof(hdr) ||
        hdr.magic != ART_CACHE_MAGIC ||
        hdr.version != ART_CACHE_FORMAT_VERSION ||
        hdr.width == 0 || hdr.height == 0 ||
        hdr.width > DISPLAY_WIDTH || hdr.height > DISPLAY_HEIGHT)
    {
        return empty_slide_hid;
    }

    w = hdr.width;
    h = hdr.height;
    size = sizeof(struct dim) + sizeof(pix_t) * w * h;

    do {
        hid = buflib_alloc(&buf_ctx, size);
    } while (hid < 0 && free_slide_prio(prio));

    if (hid < 0)
        return -1;

    struct dim *bm = buflib_get_data(&buf_ctx, hid);
    bm->width = w;
    bm->height = h;
    pix_t *dst = (pix_t*)(sizeof(struct dim) + (char *)bm);

    /* The "coverflow" size is stored column-major (art_sizes.h), which is the
     * order rendering wants, so it reads straight in. The cache falls back to
     * rows for a thumbnail it could not store transposed -- a non-square COVER
     * fit -- and that one is transposed a row at a time on the way in. */
    if (hdr.layout == AA_COLUMNS)
    {
        size_t bytes = sizeof(pix_t) * w * h;

        if (read(fh, dst, bytes) != (ssize_t)bytes)
        {
            buflib_free(&buf_ctx, hid);
            return empty_slide_hid;
        }
    }
    else
    {
        for (row = 0; row < h; row++)
        {
            if (read(fh, rowbuf, sizeof(pix_t) * w)
                != (ssize_t)(sizeof(pix_t) * w))
            {
                buflib_free(&buf_ctx, hid);
                return empty_slide_hid;
            }
            for (col = 0; col < w; col++)
                dst[col * h + row] = rowbuf[col];
        }
    }

    return hid;
}

/**
  Load the surface for the given slide_index into the cache at cache_index.
 */
static inline bool load_and_prepare_surface(const int slide_index,
                                            const int cache_index,
                                            const int prio)
{
    int hid = -1;
    bool got_shared = false;

    /* Prefer the shared, database-driven thumbnail cache. It is keyed by a hash
     * of the folder the tracks live in, which the album index resolved once
     * when it was built -- so this costs a file open, not a database search. */
    unsigned int art_hash = model->art_key(slide_index);
    if (pf_cover_size_idx >= 0 && art_hash != 0)
    {
        char aat_file[MAX_PATH];
        int fh;

        /* Opened rather than looked up: this runs while an animation is
         * scrolling, and asking whether the file exists before opening it is
         * two directory lookups where one will do. A folder the cache has no
         * art for simply fails to open and falls through below, which is also
         * what happens while a pass is still running and has not reached it --
         * the pass finishing drops these slides and loads them again (see
         * album_covers_loop), so real art replaces the empty slide when it
         * lands. */
        art_cache_thumb_path(art_hash, pf_cover_size_idx,
                             aat_file, sizeof(aat_file));
        fh = open(aat_file, O_RDONLY);
        if (fh >= 0)
        {
            hid = read_aat(fh, prio);
            close(fh);
            if (hid < 0)
                return false; /* allocation failure: retry later */
            got_shared = (hid != empty_slide_hid);
        }
    }

    /* Fall back to this screen's own pfraw cache when a shared thumbnail
     * isn't available yet (e.g. background generation hasn't reached it),
     * so nothing regresses versus before the shared cache existed. A model
     * without a legacy cache (returns false) just shows the empty slide. */
    if (!got_shared)
    {
        char pfraw_file[MAX_PATH];
        if (model->legacy_art(slide_index, pfraw_file, sizeof(pfraw_file)))
        {
            hid = read_pfraw(pfraw_file, prio);
            if (hid < 0)
                return false; /* allocation failure: retry later */
        }
        else
            hid = empty_slide_hid;
    }

    pf_sldcache.cache[cache_index].hid = hid;
    if (cache_index < SLIDE_CACHE_SIZE)
        pf_sldcache.cache[cache_index].index = slide_index;

    return true;
}


/**
 Load the "next" slide that we can load, freeing old slides if needed, provided
 that they are further from center_index than the current slide
*/
bool load_new_slide(void)
{
    if (wants_to_quit)
        return false;

    int i = -1;

    if (pf_sldcache.center_idx != -1)
    {
        int next, prev;
        if (pf_sldcache.cache[pf_sldcache.center_idx].index != center_index)
        {
            if (pf_sldcache.cache[pf_sldcache.center_idx].index < center_index)
            {
                pf_sldcache.center_idx = _SEEK_RIGHT_WHILE(pf_sldcache.center_idx,
                                pf_sldcache.cache[next_].index <= center_index);
                if (pf_sldcache.center_idx == -1)
                    goto fatal_fail;

                prev = pf_sldcache.center_idx;
                next = pf_sldcache.cache[pf_sldcache.center_idx].next;
            }
            else
            {
                pf_sldcache.center_idx = _SEEK_LEFT_WHILE(pf_sldcache.center_idx,
                                pf_sldcache.cache[next_].index >= center_index);
                if (pf_sldcache.center_idx == -1)
                    goto fatal_fail;

                next = pf_sldcache.center_idx;
                prev = pf_sldcache.cache[pf_sldcache.center_idx].prev;
            }
            if (pf_sldcache.cache[pf_sldcache.center_idx].index != center_index)
            {
                if (pf_sldcache.free == -1)
                    free_slide_prio(0);

                i = lla_pop_head(&pf_sldcache.free);
                if (!load_and_prepare_surface(center_index, i, 0))
                    goto fail_and_refree;

                if (pf_sldcache.cache[next].index == -1)
                {
                    if (pf_sldcache.cache[prev].index == -1)
                        goto insert_first_slide;
                    else
                        next = pf_sldcache.cache[prev].next;
                }
                lla_insert(i, next);
                if (pf_sldcache.cache[i].index < pf_sldcache.cache[pf_sldcache.used].index)
                    pf_sldcache.used = i;

                pf_sldcache.center_idx = i;
                pf_sldcache.left_idx = i;
                pf_sldcache.right_idx = i;
                return true;
            }
        }
        int left, center, right;
        left = pf_sldcache.cache[pf_sldcache.left_idx].index;
        center = pf_sldcache.cache[pf_sldcache.center_idx].index;
        right = pf_sldcache.cache[pf_sldcache.right_idx].index;

        if (left > center)
            pf_sldcache.left_idx = pf_sldcache.center_idx;
        if (right < center)
            pf_sldcache.right_idx = pf_sldcache.center_idx;

        pf_sldcache.left_idx = _SEEK_LEFT_WHILE(pf_sldcache.left_idx,
            pf_sldcache.cache[ind_].index - 1 == pf_sldcache.cache[next_].index);

        pf_sldcache.right_idx = _SEEK_RIGHT_WHILE(pf_sldcache.right_idx,
            pf_sldcache.cache[ind_].index + 1 == pf_sldcache.cache[next_].index);
        if (pf_sldcache.right_idx == -1 || pf_sldcache.left_idx == -1)
            goto fatal_fail;


        /* update indices */
        left = pf_sldcache.cache[pf_sldcache.left_idx].index;
        center = pf_sldcache.cache[pf_sldcache.center_idx].index;
        right = pf_sldcache.cache[pf_sldcache.right_idx].index;

        int prio_l = center - left + 1;
        int prio_r = right - center + 1;
        if ((prio_l < prio_r
             || right >= number_of_slides - 1) && left > 0)
        {
            if (pf_sldcache.free == -1 && !free_slide_prio(prio_l))
            {
                return false;
            }

            i = lla_pop_head(&pf_sldcache.free);
            if (load_and_prepare_surface(left - 1, i, prio_l))
            {
                lla_insert_before(&pf_sldcache.used, i, pf_sldcache.left_idx);
                pf_sldcache.left_idx = i;
                return true;
            }
        } else if(right < number_of_slides - 1)
        {
            if (pf_sldcache.free == -1 && !free_slide_prio(prio_r))
            {
                return false;
            }

            i = lla_pop_head(&pf_sldcache.free);
            if (load_and_prepare_surface(right + 1, i, prio_r))
            {
                lla_insert_after(i, pf_sldcache.right_idx);
                pf_sldcache.right_idx = i;
                return true;
            }
        }
    } else {
        i = lla_pop_head(&pf_sldcache.free);
        if (load_and_prepare_surface(center_index, i, 0))
        {
insert_first_slide:
            pf_sldcache.cache[i].next = i;
            pf_sldcache.cache[i].prev = i;
            pf_sldcache.center_idx = i;
            pf_sldcache.left_idx = i;
            pf_sldcache.right_idx = i;
            pf_sldcache.used = i;
            return true;
        }
    }
fail_and_refree:
    if (i != -1)
    {
        lla_insert_tail(&pf_sldcache.free, i);
    }
    return false;
fatal_fail:
    free_all_slide_prio(0);
    initialize_slide_cache();
    return false;
}


/**
  Get a slide from the buffer
 */
static inline struct dim *get_slide(const int hid)
{
    if (!hid)
        return NULL;

    struct dim *bmp;

    bmp = buflib_get_data(&buf_ctx, hid);

    return bmp;
}


/**
 Return the requested surface for the given slide.

 The cache is a linked-list-in-array, so locating a slide_index is an O(n)
 walk of the used list. render_slide() calls this for every visible slide,
 every frame, and a slide normally stays in the same cache slot across frames.
 slide->cached_slot memoizes the slot found last time: a slot's index is unique
 within the used list, so if cache[slot].index still equals slide_index (and the
 slot holds a real handle) it is guaranteed to be the same slot, and we can skip
 the scan. This check reproduces the scan's result exactly on a hit and simply
 falls back to the full scan on a miss (evicted/reused/not-yet-resolved slot),
 so no explicit invalidation is needed. The bitmap data is always fetched fresh
 via get_slide() because buflib may relocate it between frames.
*/
static inline struct dim *surface(struct slide_data *slide)
{
    const int slide_index = slide->slide_index;
    if (slide_index < 0)
        return 0;
    if (slide_index >= number_of_slides)
        return 0;

    int slot = slide->cached_slot;
    if ((unsigned)slot < SLIDE_CACHE_SIZE &&
        pf_sldcache.cache[slot].index == slide_index &&
        pf_sldcache.cache[slot].hid != 0)
        return get_slide(pf_sldcache.cache[slot].hid);

    int i;
    if ((i = pf_sldcache.used ) != -1)
    {
        int j = 0;
        do {
            if (pf_sldcache.cache[i].index == slide_index) {
                slide->cached_slot = i;
                return get_slide(pf_sldcache.cache[i].hid);
            }
            i = pf_sldcache.cache[i].next;
            j++;
        } while (i != pf_sldcache.used && j < SLIDE_CACHE_SIZE);
    }
    return get_slide(empty_slide_hid);
}

/**
 adjust slides so that they are in "steady state" position
 */
static void reset_slides(void)
{
    center_slide.angle = 0;
    center_slide.cx = 0;
    center_slide.cy = 0;
    center_slide.distance = 0;
    center_slide.slide_index = center_index;

    int i;
    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        struct slide_data *si = &left_slides[i];
        si->angle = itilt;
        si->cx = -(offsetX + auto_slide_spacing * i);
        si->cy = offsetY;
        si->slide_index = center_index - 1 - i;
        si->distance = 0;
    }

    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        struct slide_data *si = &right_slides[i];
        si->angle = -itilt;
        si->cx = offsetX + auto_slide_spacing * i;
        si->cy = offsetY;
        si->slide_index = center_index + 1 + i;
        si->distance = 0;
    }
}


/**
 Updates look-up table and other stuff necessary for the rendering.
 Call this when the viewport size or slide dimension is changed.
 *
 * To calculate the offset that will provide the proper margin, we use the same
 * projection used to render the slides. The solution for xc, the slide center,
 * is:
 *                         xp * (zo + xs * sin(r))
 * xc = xp - xs * cos(r) + ───────────────────────
 *                                    z
 * TODO: support moving the side slides toward or away from the camera
 */
static void recalc_offsets(void)
{
    PFreal xs = PFREAL_HALF - DISPLAY_WIDTH * PFREAL_HALF;
    PFreal zo;
    PFreal xp = (DISPLAY_WIDTH * PFREAL_HALF - PFREAL_HALF +
                global_settings.album_covers_center_margin * PFREAL_ONE)
                - global_settings.album_covers_slide_tuck * PFREAL_ONE;
    PFreal cosr, sinr;

    itilt = (global_settings.album_covers_parallel_slides ? 55 : 70) * IANGLE_MAX / 360;
    cosr = fcos(-itilt);
    sinr = fsin(-itilt);
    zo = fmuln(MAXSLIDE_LEFT_R, sinr, PFREAL_SHIFT - 2, 0);
    offsetX = xp - fmul(xs, cosr) + fmuln(xp,
        zo + fmuln(xs, sinr, PFREAL_SHIFT - 2, 0), PFREAL_SHIFT - 2, 0)
        / CAM_DIST;
    offsetY = DISPLAY_WIDTH / 2 * (fsin(itilt) + PFREAL_ONE / 2);

    /* auto-compute side slide spacing for 3 visible slides per side.
     * We distribute 3 visible slides across 2 intervals from offsetX
     * to cx_last (the world-space cx where the last slide's far edge
     * reaches the screen border).
     */
    {
        PFreal cx_last;
        if (global_settings.album_covers_parallel_slides) {
            /* In parallel mode, each slide is rendered at cx=0 then
             * shifted by screen_cx = CAM_DIST*cx/(CAM_DIST_R+zo).
             * Find the canonical far-edge position (right bitmap edge
             * of a right slide rendered at cx=0), then invert the
             * shift to find the cx that reaches the screen border. */
            PFreal edge_r = fdiv(CAM_DIST * fmul(-xs, cosr),
                CAM_DIST_R + zo + fmul(-xs, sinr));
            PFreal target = -DISPLAY_LEFT_R - edge_r;
            /* Invert: cx = target * (CAM_DIST_R+zo) / CAM_DIST_R
             *            = target + target * zo / CAM_DIST_R */
            cx_last = target
                + fmuln(target, zo, PFREAL_SHIFT - 2, 0) / CAM_DIST;
        } else {
            cx_last = (-DISPLAY_LEFT_R) + fmul(xs, cosr);
        }
        PFreal span = cx_last - offsetX;
        if (span < PFREAL_ONE)
            span = PFREAL_ONE;
        auto_slide_spacing = span / 2;
    }
}

/* Re-apply the geometry settings (zoom / centre margin / slide tuck / tilt) to
 * the slide layout without rebuilding the index -- used after the in-screen
 * settings menu changes a purely visual setting. Preserves the current slide. */
void carousel_refresh(void)
{
    recalc_offsets();
    reset_slides();
}

/**
   Fade the given color toward the theme background color.
   a = 256 means fully opaque (original color), a = 0 means fully bg.
 */
static inline pix_t fade_color(pix_t c, unsigned a)
{
    unsigned int result;
    a = (a + 2) & 0x1fc;
    unsigned int inv_a = 0x100 - a;
    result = (((c & 0xf81f) * a + pf_bg_rb * inv_a)) & 0xf81f00;
    result |= (((c & 0x7e0) * a + pf_bg_g * inv_a)) & 0x7e000;
    result >>= 8;
    return result;

}

/**
 * Render a single slide
 * Where xc is the slide's horizontal offset from center, xs is the horizontal
 * on the slide from its center, zo is the slide's depth offset from the plane
 * of the display, r is the angle at which the slide is tilted, and xp is the
 * point on the display corresponding to xs on the slide, the projection
 * formulas are:
 *
 *      z * (xc + xs * cos(r))
 * xp = ──────────────────────
 *       z + zo + xs * sin(r)
 *
 *      z * (xc - xp) - xp * zo
 * xs = ────────────────────────
 *      xp * sin(r) - z * cos(r)
 *
 * We use the xp projection once, to find the left edge of the slide on the
 * display. From there, we use the xs reverse projection to find the horizontal
 * offset from the slide center of each column on the screen, until we reach
 * the right edge of the slide, or the screen. The reverse projection can be
 * optimized by saving the numerator and denominator of the fraction, which can
 * then be incremented by (z + zo) and sin(r) respectively.
 */

/* Clear only the PictureFlow viewport area (not the status bar).
 * Uses screen->clear_viewport() which fills with bg_pattern,
 * unlike lcd_fillrect(DRMODE_SOLID) which fills with fg_pattern. */
static void pf_clear_display(void)
{
    screens[SCREEN_MAIN].clear_viewport();
}

static void render_slide(struct slide_data *slide, const int alpha)
{
    struct dim *bmp = surface(slide);
    if (!bmp) {
        return;
    }
    if (slide->angle > 255 || slide->angle < -255)
        return;
    pix_t *src = (pix_t*)(sizeof(struct dim) + (char *)bmp);

    const int sw = bmp->width;
    const int sh = bmp->height;
    const PFreal slide_left = -sw * PFREAL_HALF + PFREAL_HALF;
    const int w = LCD_WIDTH;

    PFreal cosr = fcos(slide->angle);
    PFreal sinr = fsin(slide->angle);
    PFreal zo = PFREAL_ONE * slide->distance
        - fmuln(MAXSLIDE_LEFT_R, fabs(sinr), PFREAL_SHIFT - 2, 0);

    /* For parallel rendering, project the slide as if cx=0 (canonical tilt),
     * then shift horizontally to the true screen position. */
    PFreal screen_cx = (global_settings.album_covers_parallel_slides && slide->angle != 0)
        ? fdiv(CAM_DIST * slide->cx, CAM_DIST_R + zo) : 0;
    PFreal render_cx = (screen_cx != 0) ? 0 : slide->cx;

    PFreal xs = slide_left, xsnum, xsnumi, xsden, xsdeni;
    PFreal xp = fdiv(CAM_DIST * (render_cx + fmul(xs, cosr)),
        (CAM_DIST_R + zo + fmul(xs,sinr)));

    xp += screen_cx;
    int xi = (fmax(DISPLAY_LEFT_R, xp) - DISPLAY_LEFT_R + PFREAL_ONE - 1)
        >> PFREAL_SHIFT;
    xp = DISPLAY_LEFT_R + xi * PFREAL_ONE;
    if (xi >= w) {
        return;
    }
    PFreal xp_local = xp - screen_cx;
    xsnum = CAM_DIST * (render_cx - xp_local)
        - fmuln(xp_local, zo, PFREAL_SHIFT - 2, 0);
    xsden = fmuln(xp_local, sinr, PFREAL_SHIFT - 2, 0) - CAM_DIST * cosr;
    xs = fdiv(xsnum, xsden);

    xsnumi = -CAM_DIST_R - zo;
    xsdeni = sinr;
    int x;
    int dy = PFREAL_ONE;
    const int half_height = pf_half_height;
    const int lower_half = pf_lower_half;
    const bool perspective = (zo != 0 || slide->angle != 0);
    /* sh (the decoded cover's own height, after FORMAT_KEEP_ASPECT scaling
     * into the DISPLAY_WIDTH x DISPLAY_HEIGHT box) is rarely equal to
     * pf_height -- e.g. square album art is width-limited by DISPLAY_WIDTH,
     * so sh ends up well short of DISPLAY_HEIGHT, let alone this theme's own
     * pf_height. vertical_offset centers whatever the real sh is within the
     * viewport: positive when the image is shorter than the viewport (equal
     * blank margin top and bottom), negative when it's taller (equal crop
     * top and bottom). Without this, the split was implicitly anchored to
     * the viewport's own half_height regardless of sh, which only matched
     * the original plugin's fixed full-screen numbers by coincidence (see
     * the removed pf_display_offs) -- any other sh left the image sitting
     * top-anchored, with a blank gap only at the bottom. */
    const int vertical_offset = ((half_height + lower_half) - sh) / 2;
    const int p_start_upper = (half_height - 1 - vertical_offset) * PFREAL_ONE;
    const int p_start_lower = (half_height - vertical_offset) * PFREAL_ONE;
    for (x = xi; x < w; x++) {
        int column = (unsigned)(xs - slide_left) >> PFREAL_SHIFT;
        if (column >= sw)
            break;
        if (perspective) {
            dy = (CAM_DIST_R + zo + fmul(xs, sinr)) / CAM_DIST;
        }

        const pix_t *ptr = &src[column * sh];

#define PIXELSTEP_Y   BUFFER_WIDTH
#define LCDADDR(x, y) (&buffer[(y)*BUFFER_WIDTH + (x)])

        int p = p_start_upper;
        int plim = MAX(0, p - (half_height-1) * dy);
        pix_t *pixel = LCDADDR(x, half_height-1 );

        if (alpha == 256) {
            while (p >= plim) {
                *pixel = ptr[((unsigned)p) >> PFREAL_SHIFT];
                p -= dy;
                pixel -= PIXELSTEP_Y;
            }
        } else {
            while (p >= plim) {
                *pixel = fade_color(ptr[((unsigned)p) >> PFREAL_SHIFT], alpha);
                p -= dy;
                pixel -= PIXELSTEP_Y;
            }
        }
        p = p_start_lower;
        plim = MIN(sh * PFREAL_ONE, p + lower_half * dy);
        pixel = LCDADDR(x, half_height );

        if (alpha == 256) {
            while (p < plim) {
                *pixel = ptr[((unsigned)p) >> PFREAL_SHIFT];
                p += dy;
                pixel += PIXELSTEP_Y;
            }
        } else {
            while (p < plim) {
                *pixel = fade_color(ptr[((unsigned)p) >> PFREAL_SHIFT], alpha);
                p += dy;
                pixel += PIXELSTEP_Y;
            }
        }

        if (perspective)
        {
            xsnum += xsnumi;
            xsden += xsdeni;
            xs = fdiv(xsnum, xsden);
        } else
            xs += PFREAL_ONE;

    }
    /* let the music play... */
    yield();
    return;
}

void set_current_slide(const int slide_index)
{
    int old_center_index = center_index;
    step = 0;
    center_index = fbound(0, slide_index, number_of_slides - 1);
    if (old_center_index != center_index)
    {
        queue_remove_from_head(&thread_q, EV_WAKEUP);
        queue_post(&thread_q, EV_WAKEUP, 0);
    }
    target = center_index;
    slide_frame = center_index << 16;
    reset_slides();
}

static void return_to_idle_state(void)
{
    if (pf_state == pf_scrolling)
        set_current_slide(target);
    pf_state = pf_idle;
}

/* Set right before jumping to an album's track list (PF_SELECT below),
 * consumed here the next time Album covers opens. Needed because Album
 * covers doesn't resume an existing session when the user backs out of
 * that track list -- root_menu.c's GO_TO_PREVIOUS handling re-enters via a
 * fresh album_covers(NULL) call, same as any other new entry, and
 * selected_file is NULL either way -- so without this, the branch below
 * would use its normal "audio_status() ? now-playing : last_album"
 * fresh-entry heuristic and land on whatever's currently playing instead
 * of the cover the user just came from. That heuristic makes sense when
 * actually entering fresh (from the main menu, a WPS shortcut, etc., where
 * seeing "the current album" first is reasonable) but not when coming
 * straight back from browsing this exact cover's own tracks. */
bool pf_resume_last_album = false;
/* The index to resume to, captured separately from pf_cfg.last_album:
 * init() calls pf_config_load() (reloading pf_cfg from its on-disk file)
 * before set_initial_slide() runs, which clobbers whatever was just
 * assigned to pf_cfg.last_album in the PF_SELECT handler below with
 * whatever index was last saved to disk -- silently resuming the wrong
 * album (observed as always landing back on the first one). This is a
 * purely transient, in-memory signal that must never round-trip through
 * the persisted config. */
int pf_resume_album_index;

/* True once every slide's art has been inspected (the background art cache is
 * fully populated for this index). The album model gates its "please wait"
 * splashes on this rather than reaching into aa_cache directly. */
bool carousel_cache_ready(void)
{
    return aa_cache.inspected >= pf_idx.album_ct;
}

/* carousel_settle: settle an in-progress scroll animation to idle. */
void carousel_settle(void)
{
    return_to_idle_state();
}

/* carousel_reload: restart the render pipeline over the current index buffer.
 * The loader thread is stopped first (so `compare`, if given, can re-sort the
 * album index without racing it), then the slide cache is rebuilt and the
 * loader restarted. */
void carousel_reload(int (*compare)(const void *, const void *))
{
    end_pf_thread(); /* stop loading of covers */

    if (compare)
        qsort(pf_idx.album_index, pf_idx.album_ct,
              sizeof(struct album_data), compare);

    /* Empty cache and restart cover loading thread */
    buflib_init(&buf_ctx, (void *)pf_idx.buf, pf_idx.buf_sz);
    empty_slide_hid = read_pfraw(EMPTY_SLIDE, 0);
    initialize_slide_cache();
    create_pf_thread();
}

/**
  Start the animation for changing slides
 */
static void start_animation(void)
{
    step = (target < center_slide.slide_index) ? -1 : 1;
    pf_state = pf_scrolling;
}

static void update_scroll_animation(void);

static void show_previous_slide(void)
{
    if (step == 0) {
        if (center_index > 0) {
            target = center_index - 1;
            start_animation();
        }
    } else if ( step > 0 ) {
        target = center_index;
        step = (target <= center_slide.slide_index) ? -1 : 1;
        if (step < 0)
            update_scroll_animation();
    } else {
        target = fmax(0, center_index - 2);
    }
}

static void show_next_slide(void)
{
    if (step == 0) {
        if (center_index < number_of_slides - 1) {
            target = center_index + 1;
            start_animation();
        }
    } else if ( step < 0 ) {
        target = center_index;
        step = (target < center_slide.slide_index) ? -1 : 1;
        if (step > 0)
            update_scroll_animation();
    } else {
        target = fmin(center_index + 2, number_of_slides - 1);
    }
}

static void render_all_slides(void)
{
    lcd_set_background(pf_bg_color);
    /* TODO: Optimizes this by e.g. invalidating rects */
    pf_clear_display();

    int nleft = ALBUM_COVERS_NUM_SLIDES;
    int nright = ALBUM_COVERS_NUM_SLIDES;

    int alpha;
    int index;
    if (step == 0) {
        /* no animation, boring plain rendering */
        for (index = nleft - 2; index >= 0; index--) {
            alpha = (index < nleft - 2) ? 256 : 128;
            alpha -= extra_fade;
            if (alpha > 0 )
                render_slide(&left_slides[index], alpha);
        }
        for (index = nright - 2; index >= 0; index--) {
            alpha = (index < nright - 2) ? 256 : 128;
            alpha -= extra_fade;
            if (alpha > 0 )
                render_slide(&right_slides[index], alpha);
        }
    } else {
        /* the first and last slide must fade in/fade out */

        /* Check if the transitioning slide will be re-rendered later for
         * z-order correction.  If so, skip its first render to avoid
         * drawing an entire slide that gets immediately overwritten. */
        bool skip_right_0 = false, skip_left_0 = false;
        if (step > 0) {
            PFreal cd = (center_slide.cx >= 0) ? center_slide.cx
                                                : -center_slide.cx;
            PFreal td = (right_slides[0].cx >= 0) ? right_slides[0].cx
                                                    : -right_slides[0].cx;
            skip_right_0 = (td < cd);
        } else if (step < 0) {
            PFreal cd = (center_slide.cx >= 0) ? center_slide.cx
                                                : -center_slide.cx;
            PFreal td = (left_slides[0].cx >= 0) ? left_slides[0].cx
                                                   : -left_slides[0].cx;
            skip_left_0 = (td < cd);
        }

        /* if step<0 and nleft==1, left_slides[0] is fading in  */
        alpha = ((step > 0) ? 0 : ((nleft == 1) ? 256 : 128)) - fade / 2;
        for (index = nleft - 1; index >= 0; index--) {
            if (index == 0 && skip_left_0) {
                alpha += 128;
                if (alpha > 256) alpha = 256;
                continue;
            }
            if (alpha > 0)
                render_slide(&left_slides[index], alpha);
            alpha += 128;
            if (alpha > 256) alpha = 256;
        }
        /* if step>0 and nright==1, right_slides[0] is fading in  */
        alpha = ((step > 0) ? ((nright == 1) ? 128 : 0) : -64) + fade / 2;
        for (index = nright - 1; index >= 0; index--) {
            if (index == 0 && skip_right_0) {
                alpha += 128;
                if (alpha > 256) alpha = 256;
                continue;
            }
            if (alpha > 0)
                render_slide(&right_slides[index], alpha);
            alpha += 128;
            if (alpha > 256) alpha = 256;
        }
    }
    alpha = 256;
    if (step != 0 && ALBUM_COVERS_NUM_SLIDES <= 2) /* fading out center slide */
        alpha = (step > 0) ? 256 - fade / 2 : 128 + fade / 2;
    render_slide(&center_slide, alpha);
    /* During animation, re-render the transitioning slide on top once
     * it is closer to screen center than the outgoing center slide.
     * This gives a smooth z-order transition instead of a sudden flip. */
    if (step > 0) {
        PFreal cd = (center_slide.cx >= 0) ? center_slide.cx : -center_slide.cx;
        PFreal td = (right_slides[0].cx >= 0) ? right_slides[0].cx
                                               : -right_slides[0].cx;
        if (td < cd)
            render_slide(&right_slides[0], 256);
    } else if (step < 0) {
        PFreal cd = (center_slide.cx >= 0) ? center_slide.cx : -center_slide.cx;
        PFreal td = (left_slides[0].cx >= 0) ? left_slides[0].cx
                                              : -left_slides[0].cx;
        if (td < cd)
            render_slide(&left_slides[0], 256);
    }
}

static void update_scroll_animation(void)
{
    if (step == 0)
        return;

    int speed = 16384;
    int i;

    /* deaccelerate when approaching the target */
    const int max = 2 * 65536;

    int fi = slide_frame;
    fi -= (target << 16);
    if (fi < 0)
        fi = -fi;
    fi = fmin(fi, max);

    int ia = IANGLE_MAX * (fi - max / 2) / (max * 2);
    int accel = 16384 * (PFREAL_ONE + fsin(ia)) / PFREAL_ONE;
    speed = 512 * global_settings.album_covers_transition_speed / 100
          + accel * global_settings.album_covers_scroll_speed / 100;

    /* This advances slide_frame by a fixed amount per loop iteration,
     * not per unit of wall-clock time (the original plugin's own
     * update_scroll_animation() worked exactly the same way, so this
     * isn't a regression) -- meaning the same logical animation plays
     * out in fewer, larger jumps whenever the loop itself runs slower,
     * which is exactly what happens now that playback stays active
     * and competes with this screen for CPU time (see
     * app_claim_buffer()'s comment in init()). A true fix would make
     * this frame-rate independent (scale by measured elapsed ticks
     * rather than a flat per-iteration constant); this is the cheaper,
     * targeted version of that: a flat multiplier specifically while
     * something is actually competing for the CPU, rather than always.
     * The 3/2 factor is a guess, not a measurement -- there's no way
     * to test actual frame timing from here, so treat this as a
     * starting point to tune against how it actually feels on
     * hardware, not a calibrated value. */
    if (audio_status() & AUDIO_STATUS_PLAY)
        speed = speed * 3 / 2;

    slide_frame += speed * step;

    int index = slide_frame >> 16;
    int pos = slide_frame & 0xffff;
    int neg = 65536 - pos;
    int tick = (step < 0) ? neg : pos;
    PFreal ftick = (tick * PFREAL_ONE) >> 16;

    /* the leftmost and rightmost slide must fade away */
    fade = pos / 256;

    if (step < 0)
        index++;
    if (center_index != index) {
        center_index = index;
        queue_post(&thread_q, EV_WAKEUP, 0);
        slide_frame = index << 16;
        /* Recalculate for the snapped slide_frame. Without this, the values
         * describe the frame we just left: fade drives the whole alpha ramp
         * and ftick the slide geometry, so carrying either across a crossing
         * shows as a one-frame flash.
         *
         * slide_frame now sits exactly on the boundary, so progress through
         * the gap is zero -- and that is true whichever way we are moving.
         * Deriving the backward case from pos = 65535 instead put it at the
         * far end of the gap it had just finished: fade came out 255 rather
         * than 0, lifting the entire right-hand ramp a half-step so the
         * outermost right slide appeared at alpha 63 when it should not have
         * been drawn, and ftick came out a full gap rather than none. One
         * frame of both, on every backward crossing. */
        pos = 0;
        tick = 0;
        ftick = 0;
        fade = 0;
        center_slide.slide_index = center_index;
        for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++)
            left_slides[i].slide_index = center_index - 1 - i;
        for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++)
            right_slides[i].slide_index = center_index + 1 + i;
    }

    center_slide.angle = (step * tick * itilt) >> 16;
    center_slide.cx = -step * fmul(offsetX, ftick);
    center_slide.cy = fmul(offsetY, ftick);

    if (center_index == target) {
        reset_slides();
        pf_state = pf_idle;
        slide_frame = center_index << 16;
        step = 0;
        fade = 256;
        return;
    }

    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        struct slide_data *si = &left_slides[i];
        si->angle = itilt;
        si->cx =
            -(offsetX + auto_slide_spacing * i + step
                        * fmul(auto_slide_spacing, ftick));
        si->cy = offsetY;
    }

    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        struct slide_data *si = &right_slides[i];
        si->angle = -itilt;
        si->cx =
            offsetX + auto_slide_spacing * i - step
                      * fmul(auto_slide_spacing, ftick);
        si->cy = offsetY;
    }

    if (step > 0) {
        PFreal ftick = (neg * PFREAL_ONE) >> 16;
        right_slides[0].angle = -(neg * itilt) >> 16;
        right_slides[0].cx = fmul(offsetX, ftick);
        right_slides[0].cy = fmul(offsetY, ftick);
    } else {
        PFreal ftick = (pos * PFREAL_ONE) >> 16;
        left_slides[0].angle = (pos * itilt) >> 16;
        left_slides[0].cx = -fmul(offsetX, ftick);
        left_slides[0].cy = fmul(offsetY, ftick);
    }

    /* must change direction ? */
    if (target < index)
        if (step > 0)
            step = -1;
    if (target > index)
        if (step < 0)
            step = 1;
}

static void cleanup(void)
{
    wants_to_quit = true;
    if (buf_ctx_locked)
        buf_ctx_unlock();

    /* Give the theme back. Whatever comes next draws itself, so no forced
     * redraw from here. */
    if (pf_theme_pushed)
    {
        FOR_NB_SCREENS(i)
            viewportmanager_theme_undo(i, false);
        pf_theme_pushed = false;
    }

    cpu_boost(false);
    end_pf_thread();

    /* Turn on backlight timeout (revert to settings) -- inlined equivalent
     * of apps/plugins/lib/helper.c's backlight_use_settings(), which isn't
     * linkable from core code. */
    backlight_set_timeout(global_settings.backlight_timeout);
    backlight_set_timeout_plugged(global_settings.backlight_timeout_plugged);

    /* Nothing to free: pf_idx.buf points into the shared app buffer (see
     * init()), not a buflib handle. But that buffer was claimed for this
     * screen's lifetime, so hand it back -- otherwise the next screen to want
     * it panics. The bold album-name font is the shared font_get_ui_bold() --
     * owned by settings.c, not unloaded here. */
    app_release_buffer("album covers");
}

enum {
    PF_SORT_ALBUMS_BY,
    PF_GOTO_LAST_ALBUM,
    PF_GOTO_WPS,
    PF_REBUILD_CACHE,
    PF_UPDATE_CACHE,
    PF_MENU_QUIT,
};

static void error_wait(const char *message)
{
    splashf(0, "%s. Press any button to continue.", message);
    while (get_action(CONTEXT_STD, 1) == ACTION_NONE)
        yield();
    sleep(2 * HZ);
}

static bool init(void)
{
    int ret = SUCCESS;
    void *buf;
    size_t buf_size;

    cpu_boost(true); /* revert in cleanup */

    wants_to_quit = false;

    /* must appear before config load */
    memset(&aa_cache, 0, sizeof(struct albumart_t));

    config_set_defaults(&pf_cfg); /* must appear before pf_config_load */
    pf_config_load();

    /* Hiding the status bar means disabling the theme for as long as this
     * screen is up -- nothing less stops the SBS drawing into our area.
     * cleanup() pops it. Showing it needs only a temporary push, so that
     * viewport_set_defaults() reads the theme whatever the ambient state. */
    pf_show_statusbar = global_settings.album_covers_statusbar;
    pf_theme_pushed = true;
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, pf_show_statusbar, NULL);
    viewport_set_defaults(&pf_vp, SCREEN_MAIN);
    if (pf_show_statusbar)
    {
        FOR_NB_SCREENS(i)
            viewportmanager_theme_undo(i, false);
        pf_theme_pushed = false;
    }

    pf_vp.buffer = &pf_framebuffer;
    pf_vp.x = 0;
    pf_vp.width = LCD_WIDTH;
    /* Down to the bottom edge, for the same reason x and width are overridden
     * above: viewport_set_defaults() hands back the theme's %Vi, which is
     * sized for a list, and this screen fills what it is given.
     *
     * Trap: %Vi is whichever %VI the SBS last selected, so a theme that picks
     * none for this activity leaves the previous screen's in force and the
     * height follows where you came from. LCD_HEIGHT - y is what the skin
     * engine itself uses for a viewport with no height (parse_viewport()). */
    pf_vp.height = LCD_HEIGHT - pf_vp.y;

    pf_vp_y = pf_vp.y;
    pf_height = pf_vp.height;

    /* Reserve real, empty vertical space for draw_album_text()'s overlay,
     * rather than just biasing where the up/down split falls: since
     * pf_lower_half is *defined* as pf_height - pf_half_height, upper and
     * lower always summed to exactly pf_height no matter how the split was
     * biased (a "+8"-style offset, tried first, only shifts which rows go
     * above vs below center -- it never actually shrinks the total drawn
     * area, so the art always ran the full height of the viewport
     * regardless). Actually keeping clear of the text means genuinely
     * reducing the drawable height by a margin sized to match
     * draw_album_text()'s own geometry, and -- for a *top*-anchored
     * caption -- also shifting the render origin down so the reserved
     * strip is skipped rather than just repositioning within it.
     *
     * Margin sizes mirror draw_album_text()'s own Y math exactly:
     * TOP captions span up to 1.75 * char_height (name+artist) or
     * 1.5 * char_height (name only) from the top; BOTTOM captions start
     * at (pf_height - 2.25 * char_height) regardless of whether the artist
     * line is also shown (it's added below the album line, within the same
     * reserved strip, not beyond it). 2 * char_height / 2.25 * char_height
     * cover both variants of each with a little safety margin. */
    {
        int char_height = font_get(screens[SCREEN_MAIN].getuifont())->height;
        int text_margin;
        bool text_at_top;
        int draw_height;

        switch (global_settings.album_covers_show_album_name)
        {
            case ALBUM_NAME_HIDE:
                text_margin = 0;
                text_at_top = false;
                break;
            case ALBUM_NAME_TOP:
            case ALBUM_AND_ARTIST_TOP:
                text_margin = char_height * 2;
                text_at_top = true;
                break;
            case ALBUM_NAME_BOTTOM:
            case ALBUM_AND_ARTIST_BOTTOM:
            default:
                /* The lift is part of the strip: the text moved up, so the
                 * slides have to stop that much higher or they run under it. */
                text_margin = PF_CAPTION_STRIP(char_height) + PF_CAPTION_LIFT;
                text_at_top = false;
                break;
        }

        draw_height = pf_height - text_margin;
        if (draw_height < 0)
            draw_height = 0;

        pf_half_height = draw_height / 2;
        pf_lower_half = draw_height - pf_half_height;
        pf_draw_y_shift = text_at_top ? text_margin : 0;
    }

    pf_update_dynamic_colors();

    /* app_claim_buffer() -- the same region the original plugin used in its
     * PF_PLAYBACK_CAPABLE branch (pictureflow.c's init(), guarded by
     * PLUGIN_BUFFER_SIZE > 0x10000: true for both of this fork's targets,
     * ipod6g at 3 MiB and ipodvideo at 512 KiB, per firmware/export/config/
     * *.h) -- NOT core_alloc_maximum()/plugin_get_audio_buffer(), which was
     * tried here first and reverted: those hand back the largest free block
     * of the *audio* buflib pool, which requires the audio system to free
     * its own buffer first, stopping playback (and, with it, the dynamic
     * colour scheme that depends on a current track).
     *
     * pluginbuf[] (see app_buffer.c) is a plain static array -- a fixed,
     * always-resident region completely separate from the audio buffer
     * pool, reserved for whichever plugin is currently loaded, or, when
     * none is (current_plugin_handle is NULL -- true here, since this is
     * core-linked code, never a loaded plugin), returned to the caller in
     * full. It costs nothing new: this RAM was already permanently
     * reserved for plugin use, sitting idle whenever no plugin is loaded;
     * Album covers is simply borrowing it back the same way the original
     * plugin did. Not a buflib handle, so nothing to free in cleanup(). */
    buf = app_claim_buffer(&buf_size, "album covers");
    if (!buf)
    {
        error_wait("Not enough memory");
        return false;
    }

    /* store buffer pointers and sizes */
    pf_idx.buf = buf;
    pf_idx.buf_sz = buf_size;

    lcd_setfont(screens[SCREEN_MAIN].getuifont());

    /* Album-name font: the shared bold UI font. settings.c loads it once and
     * owns its lifecycle (so we must not unload it), and it already falls back
     * to the real configured UI font when the theme has no bold font -- so this
     * can be used unconditionally. Using the shared id (not the FONT_UI
     * constant) also sidesteps FONT_UI's fallback slot-search, which could
     * otherwise resolve to some other theme font in a higher slot. */
    pf_bold_font = font_get_ui_bold();

    if (!dir_exists(CACHE_PREFIX))
    {
        if (mkdir(CACHE_PREFIX) < 0)
        {
            error_wait("Could not create directory " CACHE_PREFIX);
            return false;
        }
    }

    mutex_init(&buf_ctx_mutex);

    init_scroll_lines();

    ret = model->build_index();

    if (ret == ERROR_BUFFER_FULL)
    {
        error_wait("Not enough memory for album names");
        return false;
    }
    else if (ret == ERROR_NO_ALBUMS)
    {
        error_wait("No albums found. Please enable database");
        return false;
    }
    else if (ret == ERROR_USER_ABORT)
        return false;

    number_of_slides = model->count();

    /* Phase 3 v2: Cover Flow no longer generates its own thumbnails -- the
     * background album-art cache (art_cache.c) does. Mark inspection
     * complete so the idle-loop generator never runs and the navigation
     * "wait for cache" splashes don't appear; slides come from the shared
     * .aat cache (with the old pfraw / empty slide as fallback). */
    aa_cache.inspected = model->count();

    /* Reserve the album-art scratch buffer. Both models need it: create_empty_slide()
     * builds the placeholder into aa_cache.buf, and (album only) the pfraw
     * generator caches thumbnails there. */
    size_t aa_min = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(pix_t);
    size_t aa_bufsz = ALIGN_DOWN(MAX(aa_min * 3, pf_idx.buf_sz / 8),
                                 sizeof(long));
    if (aa_bufsz < aa_min)
    {
        error_wait("Not enough memory for album art cache");
        return false;
    }

    ALIGN_BUFFER(pf_idx.buf, pf_idx.buf_sz, sizeof(long));
    aa_cache.buf = (char*) pf_idx.buf;
    aa_cache.buf_sz = aa_bufsz;

    pf_idx.buf += aa_bufsz;
    pf_idx.buf_sz -= aa_bufsz;

    pf_cover_size_idx = art_cache_size_index("coverflow");

    buflib_init(&buf_ctx, (void *)pf_idx.buf, pf_idx.buf_sz);
    initialize_slide_cache();

    if (!create_empty_slide(model->has_pfraw_cache &&
                            pf_cfg.cache_version != CACHE_VERSION))
    {
        if (model->has_pfraw_cache)
        {
            pf_cfg.cache_version = CACHE_REBUILD;
            pf_config_save();
        }
        error_wait("Could not load the empty slide");
        return false;
    }

    /* Model-specific one-off setup after the slide cache is up (album covers
     * generates its pfraw fallback thumbnails here; artist portraits has none). */
    if (model->prepare)
        model->prepare();

    if ((empty_slide_hid = read_pfraw(EMPTY_SLIDE, 0)) < 0)
    {
        error_wait("Unable to load empty slide image");
        return false;
    }

    if (!create_pf_thread())
    {
        error_wait("Cannot create thread!");
        return false;
    }

    buffer = LCD_BUF;
    buffer += (pf_vp_y + pf_draw_y_shift) * BUFFER_WIDTH; /* offset below
        status bar, plus a TOP text caption's reserved margin, if any (see
        pf_draw_y_shift's assignment above) */

    pf_state = pf_idle;

    slide_frame = 0;
    step = 0;
    target = 0;
    fade = 256;

    recalc_offsets();
    reset_slides();

    lcd_set_drawmode(DRMODE_FG);

    return true;
}

/* carousel_reinit: full teardown + rebuild of the engine. */
bool carousel_reinit(void)
{
    cleanup();
    return init();
}

static int album_covers_loop(void)
{
    int ret;
    int button;
    bool art_was_building = art_cache_is_busy();

    while (true) {
        /* An artwork pass finished while the carousel was open. Slides loaded
         * while it ran may have got the placeholder because their thumbnail had
         * not been generated yet, so drop the cached surfaces and let the
         * loader thread fetch them again -- now they exist. Everything goes,
         * the centre slide included: it is the one being looked at, and the
         * loader reloads it first. */
        bool art_building = art_cache_is_busy();
        if (art_was_building && !art_building)
        {
            buf_ctx_lock();
            free_all_slide_prio(-1);
            buf_ctx_unlock();
            queue_remove_from_head(&thread_q, EV_WAKEUP);
            queue_post(&thread_q, EV_WAKEUP, 0);
        }
        art_was_building = art_building;

        /* Get input first. The SBS renders during get_custom_action() and
         * writes into the framebuffer (including decorative viewports that
         * overlap our area). Inhibit the SBS's lcd_update() so it doesn't
         * push that content to the display -- our own lcd_update() after
         * rendering will push both the SBS status bar and our content
         * atomically, avoiding one-frame flicker of theme artifacts. */

        /* Idle-frame-rate backoff. This loop fully re-renders the coverflow
         * and flushes the shared framebuffer (status bar included) on every
         * wake-up, so individual frames can't be skipped -- the SBS stamps
         * overlapping viewports into our area and relies on our lcd_update()
         * to reach the LCD. What we can do is wake up less often when nothing
         * is changing, which avoids the expensive per-frame redraw (a full
         * viewport clear + recomposite of every slide) while the screen sits
         * idle. Only back off when it is genuinely static: not animating a
         * scroll, no caption text scrolling, cover cache fully built, no
         * dynamic-colour fade in progress, and playback stopped (while
         * playing, the status bar's time updates every second and a track
         * change can start a colour fade, both of which want the normal
         * rate). A button press interrupts the wait immediately, so
         * responsiveness is unaffected. */
        bool caption_scrolling = scroll_lines[PF_SCROLL_TRACK].step
                              || scroll_lines[PF_SCROLL_ALBUM].step
                              || scroll_lines[PF_SCROLL_ARTIST].step;
        bool quiescent = pf_state != pf_scrolling
                      && !caption_scrolling
                      && aa_cache.inspected >= pf_idx.album_ct
                      && !dynamic_colors_needs_repaint()
                      && !(audio_status() & AUDIO_STATUS_PLAY);
        int timeout = (pf_state == pf_scrolling) ? 0
                    : (quiescent ? HZ/2 : HZ/16);

        skin_inhibit_flush(true);
        button = get_custom_action(CONTEXT_PLUGIN, timeout, get_context_map);
        skin_inhibit_flush(false);

        /* SBS rendering in get_custom_action resets the viewport to default,
         * and also leaves the global draw mode at whatever the skin's own
         * text/menu elements last used (typically DRMODE_SOLID, an opaque
         * background block) -- restore both, or draw_album_text()'s
         * lcd_putsxy() below inherits that solid mode and paints an opaque
         * block behind the album/artist text instead of drawing
         * transparently over the coverflow's own background. */
        lcd_set_viewport(&pf_vp);
        lcd_set_drawmode(DRMODE_FG);

        pf_update_dynamic_colors();
        update_scroll_lines();

        if (pf_state == pf_scrolling)
            update_scroll_animation();
        render_all_slides();
        model->draw_text();

        /* Copy offscreen buffer to LCD and give time to other threads */
        lcd_update();
        yield();

        switch (button) {
        case PF_QUIT:
            return GO_TO_PREVIOUS;
        case PF_WPS:
            return GO_TO_WPS;
        case PF_BACK:
            /* Album covers is a first-class main-menu screen now, not just
             * a WPS browsing mode (the old plugin's assumption, which is
             * why this unconditionally went to the WPS) -- BACK should
             * return to whichever screen it was actually opened from, same
             * as everywhere else in the firmware. */
            return GO_TO_PREVIOUS;
        case PF_MENU:
            /* The in-screen menu is model-specific: both models open the shared
             * Carousel settings menu but act on different settings afterwards.
             * Optional (on_menu may be NULL). It returns a GO_TO_* to exit, or
             * a CAROUSEL_MENU_* sentinel to stay. */
            if (!model->on_menu)
                break;
            ret = model->on_menu();
            if (ret >= 0)
                return ret;   /* a GO_TO_* screen code */
            /* Handled in place -- restore the carousel's own status bar/viewport
             * after the menu overlay, and (on a rebuild) its colours. */
            sb_set_persistent_title(model->title, Icon_NOICON, SCREEN_MAIN);
            lcd_set_viewport(&pf_vp);
            if (ret == CAROUSEL_MENU_RELOADED)
            {
                lcd_set_background(pf_bg_color);
                lcd_set_foreground(pf_fg_color);
            }
            lcd_set_drawmode(DRMODE_FG);
            break;

        case PF_NEXT:
        case PF_NEXT_REPEAT:
            show_next_slide();
            break;

        case PF_PREV:
        case PF_PREV_REPEAT:
            show_previous_slide();
            break;

        case PF_SORTING_NEXT:
            model->sort_next();
            break;
        case PF_SORTING_PREV:
            model->sort_prev();
            break;
        case PF_JMP:
        {
            int new_idx = model->jump_next();
            if (new_idx != center_index)
            {
                pf_state = pf_idle;
                set_current_slide(new_idx);
            }
            break;
        }
        case PF_JMP_PREV:
        {
            int new_idx = model->jump_prev();
            if (new_idx != center_index)
            {
                pf_state = pf_idle;
                set_current_slide(new_idx);
            }
            break;
        }
        case PF_TRACKLIST:
        case PF_SELECT:
        {
            /* Settle the animation onto the slide the user is looking at, so
             * center_index is the one they picked, then let the model drill in. */
            if (pf_state == pf_scrolling)
                set_current_slide(target);
            return model->enter(center_index);
        }
        default:
            if (default_event_handler(button) == SYS_USB_CONNECTED)
                return GO_TO_ROOT;
            break;
        }
    }
}

/* Run the coverflow carousel over the given model. Both public entry points
 * (album_covers / artist_portraits) are thin wrappers that select the model. */
int carousel_run(const struct carousel_model *m, const char *selected_file)
{
    int ret;

    model = m;

    /* Self-managed (rather than left to root_menu.c's load_screen(), which
     * only wraps the main-menu dispatch path) since this is also reachable
     * directly from apps/gui/wps.c (the "coverflow" WPS select-action and
     * the custom STOP-opens-coverflow behavior), bypassing that dispatcher
     * entirely. This is what makes %cs report ACTIVITY_ALBUMCOVERS
     * correctly regardless of which entry path was used. */
    push_current_activity(ACTIVITY_ALBUMCOVERS);

    if (!check_database())
    {
        pop_current_activity();
        return GO_TO_PREVIOUS;
    }

    /* render_slide() writes pixels directly into lcd_fb, and pf_framebuffer
     * (assigned to pf_vp.buffer in init()) needs the real frame_buffer_t
     * contents -- both dropped silently during the plugin-to-core port,
     * leaving every direct pixel write going to a NULL pointer (lcd_fb's
     * BSS default) instead of the real framebuffer. Matches plugin_start()'s
     * original setup exactly. */
    {
        struct viewport *vp_main = lcd_set_viewport(NULL);
        lcd_fb = vp_main->buffer->fb_ptr;
        pf_framebuffer = *vp_main->buffer;
    }

    if (!init())
    {
        cleanup();
        pop_current_activity();
        return GO_TO_PREVIOUS;
    }

    /* Set here, before the loop starts, so the status bar carries this
     * screen's title from the moment it opens rather than whatever the
     * previous screen left behind. The PF_MENU case in album_covers_loop()
     * re-sets it on the way back from the in-screen menu. */
    sb_set_persistent_title(model->title, Icon_NOICON, SCREEN_MAIN);

    /* Jump to selected_file's album if one was passed (e.g. context_menu_show.c's
     * "Album covers" context-menu item on a specific track), otherwise the
     * currently-playing track's album, otherwise wherever was last viewed --
     * matching the old plugin's plugin_start() behavior exactly. */
    model->set_initial(selected_file);

    ret = album_covers_loop();

    cleanup();

    if (ret == GO_TO_WPS || ret == GO_TO_PREVIOUS_MUSIC || ret == GO_TO_PLUGIN)
        pop_current_activity_without_refresh();
    else
        pop_current_activity();

    return ret;
}