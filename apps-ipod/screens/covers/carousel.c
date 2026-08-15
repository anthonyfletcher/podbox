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
#include "carousel.h"     /* the shared engine: carousel_idx, the model */

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

/* --- The flat view ---------------------------------------------------------
 * Covers lie face-on, all the same size and all on the same line: two piles
 * squared off against the screen edges, and the current cover on top in the
 * middle. The piles are flat -- every card in one sits exactly on the one
 * below, so only the top of each is ever visible, and there is no stack to see.
 *
 * A card at cx = 0 occupies the middle DISPLAY_WIDTH columns, so a pile at
 * -PF_PILE_CX runs off the left edge with its inner strip underneath the centre
 * card. That overlap is what makes the centre card read as lying *on* the pile;
 * with the two exactly abutting, the boundary is ambiguous.
 *
 * Timing is the flat view's own (see flat_pace()). The 3D view's ease curve
 * spends most of a flip crawling through its last tenth, which reads as a
 * rotation settling and as a stall for a card being dealt; and a fixed duration
 * is wrong in the other direction, since it caps how fast the user can get
 * through the covers. The flat view runs at a constant rate per cover, set from
 * how fast the user is actually scrolling: PF_FLAT_FLIP_MS when they are taking
 * their time, down to PF_FLAT_MIN_MS, below which there is no animation left to
 * see and the covers just change in the middle. Both scale with the scroll
 * speed setting. */
#define PF_PILE_OVERLAP 16
#define PF_PILE_CX      ((DISPLAY_WIDTH - PF_PILE_OVERLAP) * PFREAL_ONE)
#define PF_FLAT_FLIP_MS 250
/* A frame costs 11-15 ms here, so this is about four of them -- the fewest a
 * card can be caught at and still read as travelling rather than jumping. */
#define PF_FLAT_MIN_MS  60

/* The deal: the cover on top slides off to its pile, and the top of the other
 * pile slides into the middle behind it, PF_DEAL_LEAD of a flip later.
 *
 * That lead is the whole of it, and it is a narrow window. A cover at rest lies
 * on top of both piles, overlapping each by PF_PILE_OVERLAP, so the two moving
 * covers start out overlapping by that much -- and the lead is what pulls them
 * apart. Too much and the background shows between them, which is glaring, and
 * on the 5G makes the tearing far worse (a large area changing colour rather
 * than shifting). Too little and they travel locked together, which reads as
 * the whole row sliding rather than one card being dealt.
 *
 * At 0.11 the pair stays about two pixels overlapped for the middle of the
 * flip: no background, and the moment they swap over in the stacking order --
 * which they must, since the arriving cover ends up on top of the one it
 * started underneath -- has almost nothing to swap, so it cannot be seen. */
#define PF_DEAL_LEAD (PFREAL_ONE * 11 / 100)
#define PF_DEAL_SPAN (PFREAL_ONE - PF_DEAL_LEAD)

/* How far a flip may stretch as the user stops, as a multiple of the pace they
 * were scrolling at -- see flat_pace(). */
#define PF_FLAT_SETTLE 3

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

/* Timing overlay, off. Set to 1 to draw a two-line readout over the covers:
 * frames a second, microseconds inside the render, inside the per-slide yields
 * and inside the flush, this screen's own share of wall clock, slides fetched
 * by the loader, then the clear and slide loops timed apart, nanoseconds of
 * slide loop per pixel written, and the CPU clock with its boost count.
 *
 * Worth keeping, because none of that is guessable. Measured on the 5G it
 * turned out the flush is ~6% of wall clock while the render is ~94%, which is
 * the opposite of what this screen's cost was assumed to be. Two rules if it
 * is switched back on: the yields must stay separated out, since USEC_TIMER is
 * wall clock and would otherwise bill the codec's time to the render; and
 * ns-per-pixel means nothing without the clock beside it, being ~43 cycles at
 * 80 MHz and ~16 at 30. */
#define PF_SHOW_STATS 0

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
    PFreal cy;      /* downward shift; the flat view's pile offset, 0 in 3D */
    PFreal distance;
    int alpha;      /* flat view only -- the 3D view derives its own per frame */
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
static int pf_height;    /* viewport height (LCD_HEIGHT minus status bar) */
static int pf_half_height;      /* rows of drawable (post text-margin) height above center */
static int pf_lower_half;       /* rows of drawable (post text-margin) height below center */
static int pf_draw_y_shift;     /* rows skipped below pf_vp_y for a TOP text caption's margin */
static int pf_caption_band;     /* rows the covers give up to the caption */
/* The caption's own rows, measured from the top of the viewport. Not the same
 * as the band: the band is a reservation, and carousel_caption_layout() centres
 * the text in the space that opens up, which starts a few rows above where the
 * band does. Sized for a two-line caption, which contains the one-line case --
 * both are centred on the same point, so the taller block covers the shorter. */
static int pf_text_y, pf_text_h;
/* Rows the last render actually wrote to, again from the top of the viewport.
 * A caption can only be repainted on its own if the covers stayed clear of it,
 * and the covers are the wrong shape to answer that from the layout alone: art
 * keeps its aspect ratio, so how tall a cover comes out is a property of the
 * album, not of the screen. */
static int pf_slides_y0, pf_slides_y1;
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
/* Wall clock for the flip, see update_scroll_animation(). The scroll and
 * transition speed settings are calibrated as a per-frame step at this rate. */
#define CAROUSEL_NOMINAL_FPS 30
/* Most frames a second the screen will draw. The animation is advanced by
 * elapsed time, not per frame, so this changes how many intermediate positions
 * a flip is shown at and not how long the flip takes. Above it there is little
 * left to see and a great deal to pay: a frame here costs 11-15 ms, so an
 * uncapped loop spends essentially all the CPU it takes from playback drawing
 * positions that are replaced before the panel has finished showing them.
 *
 * Equal to CAROUSEL_NOMINAL_FPS by coincidence rather than by rule -- that one
 * calibrates the scroll and transition speed settings, so welding the two
 * together would make retuning this rescale those. HZ/30 is 3 ticks, i.e. 33
 * frames a second, tick granularity being what it is. */
#define PF_MAX_FPS       30
#define PF_FRAME_TICKS   MAX(HZ / PF_MAX_FPS, 1)
static long anim_last_tick;
static int anim_frac;
/* Always 0: nothing sets it any more, but render_all_slides()'s fade
 * arithmetic still reads it, so it stays as a no-op term. */
static int extra_fade;
int center_index = 0; /* index of the slide that is in the center */
static int itilt;
static PFreal offsetX;
static PFreal auto_slide_spacing;
static int number_of_slides;
/* Flat view pacing: when the user last asked for a cover and how far apart they
 * have been asking, plus the two per-flip decisions those feed -- whether this
 * cover is going past too quickly to be worth dealing, and whether any card is
 * currently off its resting place. See flat_pace(). */
static long pf_flat_last_input;
static int  pf_flat_interval;
static bool pf_flat_snap;
static bool pf_flat_moving;
/* Whether the arriving cover has taken the top of the stack from the one it is
 * replacing. See flat_animate(). */
static bool pf_flat_handover;

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

struct db_summary_t carousel_idx;

static bool thread_is_running;
static bool wants_to_quit;

/* Bumped by the loader thread each time a slide near enough to be on screen
 * gets its surface. The main loop draws a frame only when the picture has
 * actually changed, and a cover arriving is one of the ways it changes -- the
 * one nothing else would tell it about. Slides loaded further out are read long
 * before they are looked at and would only ask for frames showing nothing new.
 */
static volatile unsigned int pf_slides_loaded;

#if PF_SHOW_STATS
/* Every slide the loader fetches, near the centre or not -- pf_slides_loaded
 * counts only the ones worth a frame, and it is the far ones that cost the
 * disk. */
static volatile unsigned int pf_loads_total;
/* Wall clock handed to other threads by render_slide()'s per-slide yield(),
 * accumulated across one frame. Timing the render without separating this out
 * measures the codec and the slide loader and calls the result "drawing". */
static unsigned long pf_yield_us;
/* The clear and the slide loops timed apart, and the pixels the slide loops
 * actually wrote. Time per pixel is the number that matters: the loops are
 * maybe fifteen cycles of arithmetic each, so anything far above that is the
 * memory system, not the code. */
static unsigned long pf_clear_us;
static unsigned long pf_slides_us;
static unsigned long pf_px;
#endif

/*
    Prevent picture loading thread from allocating
    buflib memory while the main thread may be
    performing buffer-shifting operations.
*/
static struct mutex buf_ctx_mutex;
static bool buf_ctx_locked;

static struct pf_scroll_line_info scroll_line_info;
static struct pf_scroll_line scroll_lines[PF_MAX_SCROLL_LINES];

/* What the caption currently on screen was built for. Forgotten alongside the
 * scroll lines in init_scroll_lines() -- see carousel_caption_changed(). */
static int  caption_index;
static int  caption_variant;
static bool caption_valid;

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

/* The carousel's own settings file, kept apart from global_settings because
 * they are the engine's state rather than anything the settings menus present.
 *
 * Deliberately bare-bones: one fixed path and no version-gated migration, so
 * an unreadable or outdated file falls back to config_set_defaults() rather
 * than being converted. The line parsing is the core's own
 * (read_line/settings_parseline). */
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

    /* The captions go with them. Everything above has just been forgotten, so
     * a model still believing its caption is current would skip the
     * set_scroll_line() that rebuilds it. */
    caption_valid = false;
}

/* See carousel.h. Records what it was asked about, so a model calls it once
 * per draw and uses the answer for every line of its caption. */
bool carousel_caption_changed(int index, int variant)
{
    if (caption_valid && caption_index == index && caption_variant == variant)
        return false;

    caption_index = index;
    caption_variant = variant;
    caption_valid = true;
    return true;
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

/* True when a line actually moved, so the caller knows a frame is owed. A line
 * parked at either end waiting out its delay does not count -- it is showing
 * the same pixels as the frame before. */
static bool update_scroll_lines(void)
{
    int i;
    bool moved = false;

    if (TIME_BEFORE(current_tick, scroll_line_info.next_scroll))
        return false;

    scroll_line_info.next_scroll = current_tick + scroll_line_info.ticks;

    for (i = 0; i < PF_MAX_SCROLL_LINES; i++)
    {
        struct pf_scroll_line *s = &scroll_lines[i];
        if (s->step && TIME_BEFORE(s->start_tick, current_tick))
        {
            s->offset -= s->step;
            moved = true;

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

    return moved;
}


/**
 Save the given bitmap as filename in the pfraw format
 */
static bool save_pfraw(char* filename, struct bitmap *bm)
{
    struct pfraw_header bmph;
    size_t pixbytes = sizeof( pix_t ) * bm->width * bm->height;
    bool ok;
    bmph.width = bm->width;
    bmph.height = bm->height;
    int fh = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if( fh < 0 ) return false;

    /* Both writes checked, and the file removed if either falls short. A
     * truncated cache file keeps its valid header, so read_pfraw() accepts it
     * and renders whatever the short body left behind -- and being in the
     * cache, it is re-read on every open until something deletes it. */
    ok = (write(fh, &bmph, sizeof(struct pfraw_header))
              == (ssize_t)sizeof(struct pfraw_header))
      && (write(fh, bm->data, pixbytes) == (ssize_t)pixbytes);
    close( fh );

    if (!ok)
        remove(filename);

    return ok;
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
    size_t pixbytes = sizeof( pix_t ) * bm->width * bm->height;

    /* The header was checked above for exactly this reason; the body needs the
     * same. A short read here leaves the tail of a freshly allocated slide
     * holding whatever the buffer last contained, which renders as garbage. */
    if (read(fh, data, pixbytes) != (ssize_t)pixbytes)
    {
        close( fh );
        buflib_free(&buf_ctx, hid);
        return empty_slide_hid;
    }

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

    /* Height and width that way round: the slide is stored column-major, so a
     * "row" of this buffer is a column of the picture. Only the dither cares
     * -- it is the one filter that reads a pixel's position -- and a Bayer
     * grid turned on its side is still a Bayer grid. */
    art_filter_apply(dst, h, w);

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

    if (slide_index >= center_index - ALBUM_COVERS_NUM_SLIDES &&
        slide_index <= center_index + ALBUM_COVERS_NUM_SLIDES)
        pf_slides_loaded++;

#if PF_SHOW_STATS
    pf_loads_total++;
#endif

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

static bool flat_view(void)
{
    return global_settings.album_covers_view_mode == CAROUSEL_VIEW_FLAT;
}

/* The alpha a cover sitting on a pile is drawn at, the setting being how far
 * toward the background it is taken. Note this is a dimming, not a
 * transparency: render_slide() blends toward the background colour, so a pile
 * cover drawn over another one still hides it, exactly as an undimmed one
 * would. Which is what the piles want -- covers, further back. */
static int pile_alpha(void)
{
    return 256 - 256 * global_settings.album_covers_pile_fade / 100;
}

/* How far below the middle cover a pile sits. */
static PFreal pile_offset(void)
{
    return global_settings.album_covers_pile_offset * PFREAL_ONE;
}

/* Ease in and out: 3u^2 - 2u^3, for `u` in 0..PFREAL_ONE.
 *
 * Used for the drop onto a pile and the lift back off it, while the sideways
 * travel stays linear. A card crossing at a steady rate while its height eases
 * away and back is how one behaves being dealt; easing both would read as the
 * whole move slowing down in the middle, which is the 3D view's ease curve and
 * the thing the flat view was given a constant rate to get away from. */
static PFreal smoothstep(PFreal u)
{
    PFreal uu;

    if (u <= 0)
        return 0;
    if (u >= PFREAL_ONE)
        return PFREAL_ONE;

    uu = (u * u) >> PFREAL_SHIFT;
    return (uu * (3 * PFREAL_ONE - 2 * u)) >> PFREAL_SHIFT;
}

/* A duration in ticks, scaled by the scroll speed setting (200% is the
 * default, and what the PF_FLAT_*_MS figures are quoted at). */
static int flat_ticks(int ms)
{
    int t = ms * HZ / 1000 * 200 / global_settings.album_covers_scroll_speed;
    return MAX(t, 1);
}

/* Ticks this cover should take, so the deal keeps up with the user rather than
 * lagging behind them: how far apart their scroll actions have been coming,
 * stretched as the input dries up so the last cover of a gesture settles rather
 * than merely stopping.
 *
 * The stretch is a multiple of their own pace, not a fixed duration. A fixed one
 * is what makes a fast flick end in a full-length deal -- covers going by at
 * 30ms apiece and then one taking 250ms, which reads as a stumble rather than a
 * stop. Everything the flat view does is timed off the wheel; this is the part
 * that is easiest to get wrong, because a settle that is too quick to see is
 * the obvious failure and a settle that is out of proportion is not. */
static int flat_pace(void)
{
    int base = MAX(pf_flat_interval, 1);
    int since = current_tick - pf_flat_last_input;
    int pace = MAX(base, MIN(since, base * PF_FLAT_SETTLE));

    return MIN(pace, flat_ticks(PF_FLAT_FLIP_MS));
}

/* Ticks the flip in progress should take. Covers being snapped rather than
 * dealt keep to the gesture's own rate and ignore the settling term above: they
 * have no animation to land, so stretching one only holds a still picture on
 * the screen, and an even cadence is the whole of what a snapped scroll has to
 * look at. Handing them back to the 3D view's ease curve, which was the first
 * try, gives the opposite -- each cover decelerating almost to a halt and then
 * changing without warning. */
static int flat_rate(void)
{
    if (pf_flat_snap)
        return MAX(pf_flat_interval, 1);
    return MAX(flat_pace(), flat_ticks(PF_FLAT_MIN_MS));
}

/* Every scroll action, whether or not it is one the engine can act on yet --
 * the ones it discards past its two-cover lookahead still say how fast the
 * user is going. */
static void flat_note_scroll(void)
{
    int full = flat_ticks(PF_FLAT_FLIP_MS);
    int gap = current_tick - pf_flat_last_input;

    pf_flat_last_input = current_tick;
    if (gap >= full)
        pf_flat_interval = full;    /* a fresh gesture: forget the old pace */
    else
        pf_flat_interval = (pf_flat_interval + gap) / 2;
}

static void coverflow_idle(void)
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
        si->cy = 0;
        si->slide_index = center_index - 1 - i;
        si->distance = 0;
    }

    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        struct slide_data *si = &right_slides[i];
        si->angle = -itilt;
        si->cx = offsetX + auto_slide_spacing * i;
        si->cy = 0;
        si->slide_index = center_index + 1 + i;
        si->distance = 0;
    }
}

/* Every card on its resting place: one cover in the middle, and the rest
 * squared up in the two piles, each exactly on top of the one below it. */
static void flat_idle(void)
{
    int pile = pile_alpha();
    PFreal drop = pile_offset();

    center_slide.angle = 0;
    center_slide.cx = 0;
    center_slide.cy = 0;
    center_slide.distance = 0;
    center_slide.alpha = 256;
    center_slide.slide_index = center_index;

    int i;
    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        struct slide_data *l = &left_slides[i];
        struct slide_data *r = &right_slides[i];

        l->angle = r->angle = 0;
        l->distance = r->distance = 0;
        l->alpha = r->alpha = pile;
        l->cy = r->cy = drop;
        l->cx = -PF_PILE_CX;
        r->cx = PF_PILE_CX;
        l->slide_index = center_index - 1 - i;
        r->slide_index = center_index + 1 + i;
    }

    pf_flat_moving = false;
}

/**
 adjust slides so that they are in "steady state" position
 */
static void reset_slides(void)
{
    if (flat_view())
        flat_idle();
    else
        coverflow_idle();
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

/* Nominal cover height. Covers are scaled into DISPLAY_WIDTH x DISPLAY_HEIGHT
 * keeping their aspect, so a square one -- nearly all of them -- comes out
 * width-limited at this height. The caption is placed against this rather than
 * against the cover currently on screen, so it stays put when a cover of some
 * other shape scrolls past. */
#define PF_NOMINAL_ART_H  MIN(DISPLAY_WIDTH, DISPLAY_HEIGHT)

/* Least clear air above and below the caption. It only bounds how tight the
 * band may get -- the block is centred in whatever space the band actually
 * opens up, which is more than this. */
#define PF_CAPTION_PAD    6

/* Second line's offset from the first line's box top: clear air of half the
 * album font's ascent, measured from that line's baseline.
 *
 * Both halves of that are load-bearing. Measured from the baseline rather than
 * the box bottom, because a box carries a descent running anywhere from a
 * fifth to nearly a third of its height depending on the font, and spacing off
 * the bottom hands that variation straight to the gap.
 *
 * Keyed to the *album* font, not the artist font or the larger of the two,
 * because the gap reads as belonging to the line above it. Keying it to the
 * larger font was tried and is wrong on real themes: Themify_2 sets an 18px
 * bold album line under a 25px regular artist line, and sizing the gap by the
 * 25px font made it visibly too wide, while BONES -- 17px for both lines --
 * looked right. Half the album ascent gives 6px and 7px respectively, which
 * matches how they actually look.
 *
 * Half an ascent also reliably exceeds a descent (ascent runs ~70% of the box,
 * descent ~28%), so a descending album name still clears the line below. */
static int caption_line2(void)
{
    return font_get(pf_bold_font)->ascent * 3 / 2;
}

static int caption_block(bool two_lines)
{
    if (!two_lines)
        return (int)font_get(pf_bold_font)->height;
    return caption_line2()
         + (int)font_get(screens[SCREEN_MAIN].getuifont())->height;
}

void carousel_caption_layout(bool two_lines, struct pf_caption *out)
{
    int block = caption_block(two_lines);
    int art_top = (pf_height - pf_caption_band - PF_NOMINAL_ART_H) / 2;
    int space, top;

    if (art_top < 0)        /* cover taller than the drawable area: it is
                             * clipped to that area, so it starts at the top */
        art_top = 0;
    art_top += pf_draw_y_shift;

    if (pf_draw_y_shift)    /* caption above the covers */
    {
        top = 0;
        space = art_top;
    }
    else
    {
        top = art_top + PF_NOMINAL_ART_H;
        space = pf_height - top;
    }

    out->y1 = (space > block) ? top + (space - block) / 2 : top;
    out->y2 = out->y1 + caption_line2();
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
/* The part of a fade that depends only on the slide's alpha and the background,
 * lifted out of the pixel loop. Both background terms are whole multiplies, and
 * leaving them where they were -- inside a function called per pixel -- had the
 * loop doing four multiplies and two global loads for every pixel where two
 * multiplies do. The compiler does not lift them itself at -Os. */
struct pf_fade
{
    unsigned int a;     /* alpha, rounded to the step the blend works in */
    unsigned int rb;    /* background red/blue contribution, pre-multiplied */
    unsigned int g;     /* background green contribution */
};

static inline void fade_prepare(struct pf_fade *f, unsigned int a)
{
    a = (a + 2) & 0x1fc;
    f->a  = a;
    f->rb = pf_bg_rb * (0x100 - a);
    f->g  = pf_bg_g  * (0x100 - a);
}

static inline pix_t fade_color(pix_t c, unsigned int fa, unsigned int frb,
                               unsigned int fg)
{
    unsigned int result;

    result  = ((c & 0xf81f) * fa + frb) & 0xf81f00;
    result |= ((c & 0x7e0)  * fa + fg)  & 0x7e000;
    return result >> 8;
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

/* The same, for `h` rows starting `y` rows down the viewport. Used to wipe the
 * caption on its own, when that is all that has moved.
 *
 * The viewport is static because lcd_set_viewport() keeps the pointer rather
 * than a copy, so one on the stack would be dangling by the time anything drew
 * through it. */
static void pf_clear_band(int y, int h)
{
    static struct viewport band_vp;
    struct viewport *saved;

    if (h <= 0)
        return;

    band_vp = pf_vp;
    band_vp.y = pf_vp.y + y;
    band_vp.height = h;
    saved = lcd_set_viewport(&band_vp);
    screens[SCREEN_MAIN].clear_viewport();
    lcd_set_viewport(saved);
}

/* The screen columns a slide would occupy, [*x0, *x1), clipped to the display.
 * False if it lands entirely off-screen.
 *
 * Same projection as render_slide()'s, run on the slide's two vertical edges
 * instead of every column -- a handful of divisions rather than a walk. Used to
 * work out what hides what before anything is drawn. */
static bool slide_x_range(struct slide_data *slide, int *x0, int *x1)
{
    struct dim *bmp = surface(slide);
    PFreal cosr, sinr, zo, screen_cx, render_cx, xs, xp;
    int sw, lo, hi;

    if (!bmp || slide->angle > 255 || slide->angle < -255)
        return false;

    sw = bmp->width;
    cosr = fcos(slide->angle);
    sinr = fsin(slide->angle);
    zo = PFREAL_ONE * slide->distance
       - fmuln(MAXSLIDE_LEFT_R, fabs(sinr), PFREAL_SHIFT - 2, 0);
    screen_cx = (global_settings.album_covers_parallel_slides && slide->angle != 0)
        ? fdiv(CAM_DIST * slide->cx, CAM_DIST_R + zo) : 0;
    render_cx = (screen_cx != 0) ? 0 : slide->cx;

    xs = -sw * PFREAL_HALF + PFREAL_HALF;        /* left edge */
    xp = fdiv(CAM_DIST * (render_cx + fmul(xs, cosr)),
              CAM_DIST_R + zo + fmul(xs, sinr)) + screen_cx;
    lo = (fmax(DISPLAY_LEFT_R, xp) - DISPLAY_LEFT_R + PFREAL_ONE - 1)
         >> PFREAL_SHIFT;

    xs = sw * PFREAL_HALF;                       /* right edge */
    xp = fdiv(CAM_DIST * (render_cx + fmul(xs, cosr)),
              CAM_DIST_R + zo + fmul(xs, sinr)) + screen_cx;
    hi = (xp - DISPLAY_LEFT_R + PFREAL_ONE - 1) >> PFREAL_SHIFT;

    if (lo < 0)
        lo = 0;
    if (hi > LCD_WIDTH)
        hi = LCD_WIDTH;
    *x0 = lo;
    *x1 = hi;
    return lo < hi;
}

/* Draw only columns in [x_from, x_to). The projection still has to walk every
 * column -- its state is carried forward one step at a time -- but a culled
 * column costs two divisions instead of a column of pixels. */
static void render_slide_clipped(struct slide_data *slide, const int alpha,
                                 int x_from, int x_to)
{
    struct dim *bmp = surface(slide);
    if (!bmp) {
        return;
    }
    if (slide->angle > 255 || slide->angle < -255)
        return;
    /* Wholly hidden. Worth its own test: the walk below still costs two
     * divisions a column even when every one of them is skipped, and the cull
     * takes slides out entirely often enough for that to add up. */
    if (x_from >= x_to)
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
    /* cy shifts the image down within that centred position -- the flat view's
     * pile offset, and zero in the 3D view. Clamped, because the lower loop
     * reads its source row unsigned: a start below row 0 would wrap to a huge
     * index and read far outside the bitmap rather than clipping. The upper
     * loop needs no clamp; a negative start simply fails its first test. */
    int voff = ((half_height + lower_half) - sh) / 2;
    if (slide->cy > 0)
        voff += slide->cy >> PFREAL_SHIFT;
    if (voff > half_height)
        voff = half_height;
    const int vertical_offset = voff;
    const int p_start_upper = (half_height - 1 - vertical_offset) * PFREAL_ONE;
    const int p_start_lower = (half_height - vertical_offset) * PFREAL_ONE;

    /* The rows this slide can reach, for the partial-frame test in
     * album_covers_loop(). Both loops start at the middle row and step by dy,
     * and dy is never less than PFREAL_ONE, so the slide is at its tallest when
     * it is drawn face-on and occupies exactly its own sh rows from
     * vertical_offset -- clipped to the drawable area. That is an upper bound
     * whatever the tilt, so it can be had here rather than counted per column. */
    {
        int y0 = MAX(0, vertical_offset);
        int y1 = MIN(half_height + lower_half, vertical_offset + sh);

        if (y0 + pf_draw_y_shift < pf_slides_y0)
            pf_slides_y0 = y0 + pf_draw_y_shift;
        if (y1 + pf_draw_y_shift > pf_slides_y1)
            pf_slides_y1 = y1 + pf_draw_y_shift;
    }
    /* Once per slide, not once per pixel -- see struct pf_fade. */
    /* Not named `fade`: that is the module-level animation ramp this function
     * has no business shadowing. */
    struct pf_fade fd;
    fade_prepare(&fd, alpha);
    /* Into plain locals so they land in registers rather than being reloaded
     * through the struct on every pixel. */
    const unsigned int fa = fd.a, frb = fd.rb, fg = fd.g;

    /* Walked a column at a time, which is the order the source is stored in
     * (art_sizes.h keeps the coverflow thumbnails column-major for exactly
     * this) and keeps p, dy and both pointers in registers.
     *
     * Trap for anyone eyeing the 640-byte stride on the stores: turning this
     * inside out to write along rows was tried and is ~20% *slower*. The
     * per-column state has to move from registers into arrays, and the four
     * extra array accesses a pixel cost more than the stores save. This loop is
     * limited by its arithmetic, not by its access pattern. */
    for (x = xi; x < w; x++) {
        /* Rounding in the reverse projection above can leave xs a fraction
         * below the slide's left edge. The cast is unsigned, so that would
         * wrap to a huge column and break out of the loop, truncating the
         * slide mid-render rather than drawing its first column. */
        if (xs < slide_left)
            xs = slide_left;
        int column = (unsigned)(xs - slide_left) >> PFREAL_SHIFT;
        if (column >= sw)
            break;
        if (perspective) {
            dy = (CAM_DIST_R + zo + fmul(xs, sinr)) / CAM_DIST;
        }

        if (x < x_from || x >= x_to)
            goto next_column;   /* hidden: keep the projection, skip the pixels */

        const pix_t *ptr = &src[column * sh];

#define PIXELSTEP_Y   BUFFER_WIDTH
#define LCDADDR(x, y) (&buffer[(y)*BUFFER_WIDTH + (x)])

        int p = p_start_upper;
        int plim = MAX(0, p - (half_height-1) * dy);
        pix_t *pixel = LCDADDR(x, half_height-1 );
#if PF_SHOW_STATS
        const pix_t *px_from = pixel;
#endif

        if (alpha == 256) {
            while (p >= plim) {
                *pixel = ptr[((unsigned)p) >> PFREAL_SHIFT];
                p -= dy;
                pixel -= PIXELSTEP_Y;
            }
        } else {
            while (p >= plim) {
                *pixel = fade_color(ptr[((unsigned)p) >> PFREAL_SHIFT],
                                    fa, frb, fg);
                p -= dy;
                pixel -= PIXELSTEP_Y;
            }
        }
#if PF_SHOW_STATS
        pf_px += (px_from - pixel) / PIXELSTEP_Y;
#endif
        p = p_start_lower;
        plim = MIN(sh * PFREAL_ONE, p + lower_half * dy);
        pixel = LCDADDR(x, half_height );
#if PF_SHOW_STATS
        px_from = pixel;
#endif

        if (alpha == 256) {
            while (p < plim) {
                *pixel = ptr[((unsigned)p) >> PFREAL_SHIFT];
                p += dy;
                pixel += PIXELSTEP_Y;
            }
        } else {
            while (p < plim) {
                *pixel = fade_color(ptr[((unsigned)p) >> PFREAL_SHIFT],
                                    fa, frb, fg);
                p += dy;
                pixel += PIXELSTEP_Y;
            }
        }

#if PF_SHOW_STATS
        pf_px += (pixel - px_from) / PIXELSTEP_Y;
#endif

next_column:
        if (perspective)
        {
            xsnum += xsnumi;
            xsden += xsdeni;
            xs = fdiv(xsnum, xsden);
        } else
            xs += PFREAL_ONE;

    }
    /* let the music play... */
#if PF_SHOW_STATS
    {
        unsigned long y0 = USEC_TIMER;
        yield();
        pf_yield_us += USEC_TIMER - y0;
    }
#else
    yield();
#endif
    return;
}

static void render_slide(struct slide_data *slide, const int alpha)
{
    render_slide_clipped(slide, alpha, 0, LCD_WIDTH);
}

/* How many source rows one screen row covers at screen column x -- the render
 * loop's `dy`, worked out for a single column instead of walked to.
 *
 * This is the whole of the "does this slide hide that one" question: both loops
 * in render_slide_clipped() start at the middle row and step by dy, so the
 * number of rows a column draws falls as dy rises, and two slides sharing a
 * vertical offset are ordered by dy alone. */
static int slide_dy_at(struct slide_data *slide, int x)
{
    struct dim *bmp = surface(slide);
    PFreal cosr, sinr, zo, screen_cx, render_cx, xp_local, xsnum, xsden, xs;
    PFreal slide_left;

    if (!bmp)
        return 0;

    cosr = fcos(slide->angle);
    sinr = fsin(slide->angle);
    zo = PFREAL_ONE * slide->distance
       - fmuln(MAXSLIDE_LEFT_R, fabs(sinr), PFREAL_SHIFT - 2, 0);
    screen_cx = (global_settings.album_covers_parallel_slides && slide->angle != 0)
        ? fdiv(CAM_DIST * slide->cx, CAM_DIST_R + zo) : 0;
    render_cx = (screen_cx != 0) ? 0 : slide->cx;

    xp_local = (DISPLAY_LEFT_R + x * PFREAL_ONE) - screen_cx;
    xsnum = CAM_DIST * (render_cx - xp_local)
          - fmuln(xp_local, zo, PFREAL_SHIFT - 2, 0);
    xsden = fmuln(xp_local, sinr, PFREAL_SHIFT - 2, 0) - CAM_DIST * cosr;
    xs = fdiv(xsnum, xsden);

    slide_left = -bmp->width * PFREAL_HALF + PFREAL_HALF;
    if (xs < slide_left)        /* the render clamps before it takes dy */
        xs = slide_left;

    return (CAM_DIST_R + zo + fmul(xs, sinr)) / CAM_DIST;
}

/* Whether `near`, drawn over `far`, really hides it across screen columns
 * [a, b]. Every drawn pixel is opaque -- fade_color() blends toward
 * pf_bg_color and never reads the framebuffer -- so what it hides is wasted
 * work rather than part of a blend.
 *
 * Both slides must share a vertical offset, which is what the height and cy
 * tests are: voff is worked out from those two alone, and without a shared one
 * the two are not even anchored to the same row.
 *
 * Given that, `near` is the taller of the two wherever its dy is the lower.
 * An untilted slide in the plane of the display -- the centre slide at rest --
 * takes that outright: its dy is exactly PFREAL_ONE and no slide's dy is ever
 * less, because zo grows with the tilt at the same rate the xs term can
 * subtract. Everything else is sampled: dy is a smooth function of the screen
 * column for both slides, so the two ends and the middle of the overlap settle
 * it. Getting this wrong leaves a stripe of whatever was in the framebuffer
 * before, which no later pass repairs, so tools/pfgeom checks the sampling
 * against a full every-column comparison rather than it being argued about. */
static bool slide_covers(struct slide_data *near, struct slide_data *far,
                         int a, int b)
{
    struct dim *n = surface(near), *f = surface(far);
    int mid = a + (b - a) / 2;

    if (b < a || !n || !f)
        return false;
    if (n->height != f->height || near->cy != far->cy)
        return false;
    if (near->angle == 0 && near->distance == 0)
        return true;
    return slide_dy_at(near, a)   <= slide_dy_at(far, a)
        && slide_dy_at(near, mid) <= slide_dy_at(far, mid)
        && slide_dy_at(near, b)   <= slide_dy_at(far, b);
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
    return aa_cache.inspected >= carousel_idx.album_ct;
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
        qsort(carousel_idx.album_index, carousel_idx.album_ct,
              sizeof(struct album_data), compare);

    /* Empty cache and restart cover loading thread */
    buflib_init(&buf_ctx, (void *)carousel_idx.buf, carousel_idx.buf_sz);
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
    anim_last_tick = current_tick;
    anim_frac = 0;
    pf_flat_snap = flat_pace() < flat_ticks(PF_FLAT_MIN_MS);
}

static void update_scroll_animation(void);

static void show_previous_slide(void)
{
    flat_note_scroll();
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
    flat_note_scroll();
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

/* A flat pile hides everything but its top card, so only four covers are ever
 * worth drawing, and the order they go in is fixed rather than sorted. Both
 * moving covers sit above both piles throughout -- a cover at rest lies on the
 * piles it overlaps, and neither of these is at rest -- so the only question is
 * which of the two is above the other, and that is the handover. */
/* Piles taken all the way to the background have nothing left to draw, and
 * drawing them anyway would paint background over what is behind them. */
static void flat_draw(struct slide_data *slide)
{
    if (slide->alpha > 0)
        render_slide(slide, slide->alpha);
}

static void flat_render(void)
{
    struct slide_data *pile_near, *pile_next, *arriving;

    if (!pf_flat_moving) {
        flat_draw(&left_slides[0]);
        flat_draw(&right_slides[0]);
        flat_draw(&center_slide);   /* overlaps both, so it goes last */
        return;
    }

    pile_near = (step > 0) ? &left_slides[0]  : &right_slides[0];
    pile_next = (step > 0) ? &right_slides[1] : &left_slides[1];
    arriving  = (step > 0) ? &right_slides[0] : &left_slides[0];

    flat_draw(pile_near);
    flat_draw(pile_next);
    if (pf_flat_handover) {
        flat_draw(&center_slide);
        flat_draw(arriving);
    } else {
        flat_draw(arriving);
        flat_draw(&center_slide);
    }
}

/* Columns a side slide need not draw because slides already drawn over it cover
 * them -- its neighbours nearer the centre, and the centre slide itself.
 *
 * One side at a time, working outwards, which is the reverse of the order they
 * are drawn in: index 0 is painted last and so sits on top of index 1, and the
 * centre slide is painted after all of them. `cover` collects the slides above
 * this one, in the order they cover the screen, and the run they make between
 * them is [span, edge).
 *
 * For each slide, the walk starts at its own inner edge and asks each member of
 * that run in turn whether it hides this slide. It stops at the first gap or
 * the first member that cannot be shown to -- and culls up to whatever it
 * reached, so one member that does not carry costs only its own stretch rather
 * than the whole claim. That matters because the runs are genuinely mixed:
 * coverflow_animate() gives index 0 an angle of its own on whichever side the
 * scroll is coming from, and the centre slide tilts throughout.
 *
 * Everything is trimmed by PF_CULL_MARGIN before being relied on, because
 * slide_x_range() solves for the slide's edges directly while the render walks
 * columns one step at a time, and the two can land a column apart.
 *
 * Left and right are the same walk with the columns mirrored, so the code below
 * reads as the right-hand side throughout and the caller unmirrors the answer. */
#define PF_CULL_MARGIN 1
#define PF_CULL_MIRROR(v) (LCD_WIDTH - 1 - (v))

static void cull_side(struct slide_data *side, const int *alpha,
                      int center_alpha, int *edge_out, bool right)
{
    struct slide_data *cover[ALBUM_COVERS_NUM_SLIDES + 1];
    int cover_lo[ALBUM_COVERS_NUM_SLIDES + 1];
    int cover_hi[ALBUM_COVERS_NUM_SLIDES + 1];
    int n_cover = 0;
    int i, k, x0, x1, lo, hi;
    int edge = 0, span = LCD_WIDTH;

    if (center_alpha > 0 && slide_x_range(&center_slide, &x0, &x1))
    {
        lo = (right ? x0 : LCD_WIDTH - x1) + PF_CULL_MARGIN;
        hi = (right ? x1 : LCD_WIDTH - x0) - PF_CULL_MARGIN;
        if (hi > lo)
        {
            cover[0] = &center_slide;
            cover_lo[0] = lo;
            cover_hi[0] = hi;
            n_cover = 1;
            span = lo;
            edge = hi;
        }
    }

    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++)
    {
        edge_out[i] = right ? 0 : LCD_WIDTH;

        if (alpha[i] <= 0)              /* not drawn: nothing to cull, and it
                                         * covers nothing either */
            continue;
        if (!slide_x_range(&side[i], &x0, &x1))
            continue;
        lo = right ? x0 : LCD_WIDTH - x1;
        hi = right ? x1 : LCD_WIDTH - x0;

        if (n_cover && lo - PF_CULL_MARGIN >= span)
        {
            int reached = lo - PF_CULL_MARGIN;

            for (k = 0; k < n_cover; k++)
            {
                int last;

                if (cover_hi[k] <= reached)     /* behind the walk already */
                    continue;
                if (cover_lo[k] > reached)      /* a gap: nothing beyond it */
                    break;

                last = MIN(cover_hi[k], MIN(hi, edge)) - 1;
                if (last < reached)
                    break;
                if (!slide_covers(cover[k], &side[i],
                                  right ? reached : PF_CULL_MIRROR(last),
                                  right ? last : PF_CULL_MIRROR(reached)))
                    break;
                reached = last + 1;
            }
            if (reached > lo)
                edge_out[i] = right ? reached : LCD_WIDTH - reached;
        }

        lo += PF_CULL_MARGIN;
        hi -= PF_CULL_MARGIN;
        if (hi <= lo)
            continue;

        if (lo > edge)                  /* a gap: this starts a fresh run */
        {
            n_cover = 0;
            span = lo;
            edge = hi;
        }
        else if (hi > edge)             /* carries the run further out */
        {
            edge = hi;
            span = MIN(span, lo);
        }
        else
            continue;                   /* wholly inside the run already there */

        cover[n_cover] = &side[i];
        cover_lo[n_cover] = lo;
        cover_hi[n_cover] = hi;
        n_cover++;
    }
}

static void coverflow_render(void)
{
    const int nleft = ALBUM_COVERS_NUM_SLIDES;
    const int nright = ALBUM_COVERS_NUM_SLIDES;
    /* Worked out before anything is drawn, because the cull needs to know which
     * slides are on screen at all: one that is faded out covers nothing, and
     * one drawn last covers everything under it. */
    int left_alpha[MAX_SLIDES_COUNT], right_alpha[MAX_SLIDES_COUNT];
    int left_to[MAX_SLIDES_COUNT], right_from[MAX_SLIDES_COUNT];
    int center_alpha = 256;
    /* The transitioning slide is drawn again on top once it is closer to screen
     * centre than the outgoing centre slide, which gives a smooth z-order
     * handover instead of a sudden flip. Its first render is then skipped
     * rather than drawing a whole slide that is immediately overwritten. */
    bool redraw_left_0 = false, redraw_right_0 = false;
    int alpha, index;

    for (index = 0; index < ALBUM_COVERS_NUM_SLIDES; index++)
        left_alpha[index] = right_alpha[index] = 0;

    if (step == 0) {
        /* no animation, boring plain rendering */
        for (index = nleft - 2; index >= 0; index--) {
            alpha = ((index < nleft - 2) ? 256 : 128) - extra_fade;
            if (alpha > 0)
                left_alpha[index] = right_alpha[index] = alpha;
        }
    } else {
        /* the first and last slide must fade in/fade out */
        PFreal cd = (center_slide.cx >= 0) ? center_slide.cx : -center_slide.cx;
        PFreal td;

        if (step > 0) {
            td = (right_slides[0].cx >= 0) ? right_slides[0].cx
                                           : -right_slides[0].cx;
            redraw_right_0 = (td < cd);
        } else {
            td = (left_slides[0].cx >= 0) ? left_slides[0].cx
                                          : -left_slides[0].cx;
            redraw_left_0 = (td < cd);
        }

        /* if step<0 and nleft==1, left_slides[0] is fading in  */
        alpha = ((step > 0) ? 0 : ((nleft == 1) ? 256 : 128)) - fade / 2;
        for (index = nleft - 1; index >= 0; index--) {
            if (alpha > 0 && !(index == 0 && redraw_left_0))
                left_alpha[index] = alpha;
            alpha += 128;
            if (alpha > 256) alpha = 256;
        }
        /* if step>0 and nright==1, right_slides[0] is fading in  */
        /* -128, not -64. Both keep the ramp continuous across a crossing --
         * that only constrains the difference between one index and the next --
         * but only -128 also meets the resting state this side starts and ends
         * a backwards scroll in. A scroll opens with fade at 256, so the ramp
         * has to reproduce the step==0 alphas above at that value: index 2 at
         * 128 needs X + 256 == 128. At -64 it came out 192 instead, and index 3
         * came out at 64 where at rest it is not drawn at all -- so a cover
         * appeared at the right edge, brightened its neighbour, and faded away
         * again, once per scroll. The other three side/direction combinations
         * were already exact; this was the only one adrift. */
        alpha = ((step > 0) ? ((nright == 1) ? 128 : 0) : -128) + fade / 2;
        for (index = nright - 1; index >= 0; index--) {
            if (alpha > 0 && !(index == 0 && redraw_right_0))
                right_alpha[index] = alpha;
            alpha += 128;
            if (alpha > 256) alpha = 256;
        }

        if (ALBUM_COVERS_NUM_SLIDES <= 2)   /* fading out center slide */
            center_alpha = (step > 0) ? 256 - fade / 2 : 128 + fade / 2;
    }

    cull_side(left_slides, left_alpha, center_alpha, left_to, false);
    cull_side(right_slides, right_alpha, center_alpha, right_from, true);

    for (index = nleft - 1; index >= 0; index--)
        if (left_alpha[index] > 0)
            render_slide_clipped(&left_slides[index], left_alpha[index],
                                 0, left_to[index]);
    for (index = nright - 1; index >= 0; index--)
        if (right_alpha[index] > 0)
            render_slide_clipped(&right_slides[index], right_alpha[index],
                                 right_from[index], LCD_WIDTH);

    render_slide(&center_slide, center_alpha);

    if (redraw_right_0)
        render_slide(&right_slides[0], 256);
    else if (redraw_left_0)
        render_slide(&left_slides[0], 256);
}

static void render_all_slides(void)
{
#if PF_SHOW_STATS
    unsigned long t0, y0;
#endif
    lcd_set_background(pf_bg_color);
    /* Reset before the slides run, not after: each one folds the rows it can
     * reach into this, and the answer describes the frame about to be drawn. */
    pf_slides_y0 = pf_height;
    pf_slides_y1 = 0;
#if PF_SHOW_STATS
    t0 = USEC_TIMER;
#endif
    pf_clear_display();
#if PF_SHOW_STATS
    pf_clear_us = USEC_TIMER - t0;
    pf_px = 0;
    t0 = USEC_TIMER;
    y0 = pf_yield_us;
#endif

    if (flat_view())
        flat_render();
    else
        coverflow_render();
#if PF_SHOW_STATS
    /* The per-slide yields land in here, and belong to whoever ran. */
    pf_slides_us = (USEC_TIMER - t0) - (pf_yield_us - y0);
#endif
}

/* The two layouts, mid-flip. `s` is the direction (the scroll's step), and
 * ftick how far through the gap between two covers this frame has got: 0 at the
 * start, PFREAL_ONE at the end. Everything above the call -- the timing, the
 * boundary snap that reassigns slide_index, the loader wake-ups -- is layout
 * independent, and stays in one copy in update_scroll_animation(). */
static void coverflow_animate(int s, int tick, int neg, PFreal ftick)
{
    int i;

    center_slide.angle = (s * tick * itilt) >> 16;
    center_slide.cx = -s * fmul(offsetX, ftick);
    center_slide.cy = 0;

    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        struct slide_data *si = &left_slides[i];
        si->angle = itilt;
        si->cx =
            -(offsetX + auto_slide_spacing * i + s
                        * fmul(auto_slide_spacing, ftick));
        si->cy = 0;
    }

    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        struct slide_data *si = &right_slides[i];
        si->angle = -itilt;
        si->cx =
            offsetX + auto_slide_spacing * i - s
                      * fmul(auto_slide_spacing, ftick);
        si->cy = 0;
    }

    PFreal fneg = (neg * PFREAL_ONE) >> 16;
    if (s > 0) {
        right_slides[0].angle = -(neg * itilt) >> 16;
        right_slides[0].cx = fmul(offsetX, fneg);
    } else {
        left_slides[0].angle = (neg * itilt) >> 16;
        left_slides[0].cx = -fmul(offsetX, fneg);
    }
}

static void flat_animate(int s, PFreal ftick)
{
    struct slide_data *arriving;
    PFreal off, in;

    /* Going past too quickly to be worth animating: a card would get two or
     * three frames to cross the screen and be sampled at arbitrary points along
     * the way. The 3D view blurs that into motion with its tilt and its fade;
     * flat covers have neither, so it strobes instead. Leave the piles at rest
     * and let the covers simply change in the middle, which is what you want
     * when hunting for one anyway.
     *
     * The scroll then stops dead rather than settling, and that is deliberate.
     * Replaying the missed move afterwards was tried: it has to bring back the
     * cover that was on top, which a snapped scroll had already taken out of
     * the middle, so it flickers. Holding a cover back so the last one could be
     * dealt for real does not work either -- the engine only ever aims two
     * ahead, so holding one back leaves a single gap and every cover ends up
     * dealt. Stopping dead is the honest end to a gesture that was never
     * showing an animation in the first place. */
    if (pf_flat_snap) {
        flat_idle();
        return;
    }

    /* Everything squared up, then move the two cards that are not. */
    flat_idle();

    off = MIN(ftick * PFREAL_ONE / PF_DEAL_SPAN, PFREAL_ONE);
    in = (ftick <= PF_DEAL_LEAD)
       ? 0 : MIN((ftick - PF_DEAL_LEAD) * PFREAL_ONE / PF_DEAL_SPAN, PFREAL_ONE);

    arriving = (s > 0) ? &right_slides[0] : &left_slides[0];

    center_slide.cx = -s * fmul(PF_PILE_CX, off);
    arriving->cx = s * fmul(PF_PILE_CX, PFREAL_ONE - in);

    /* Each takes the dimming and the height of where it is going, over the move
     * that takes it there: the one leaving sinks into its pile, the one
     * arriving comes up to full and rises to the middle as it lands. */
    {
        int pile = pile_alpha();
        int span = 256 - pile;
        PFreal drop = pile_offset();

        center_slide.alpha = 256 - ((span * off) >> PFREAL_SHIFT);
        arriving->alpha = pile + ((span * in) >> PFREAL_SHIFT);

        center_slide.cy = fmul(drop, smoothstep(off));
        arriving->cy = drop - fmul(drop, smoothstep(in));
    }

    /* They start out overlapped with the cover on top over the pile, and end
     * overlapped the other way round, so the swap goes in the middle where they
     * are barely touching. */
    pf_flat_handover = ftick >= PFREAL_HALF;
    pf_flat_moving = true;
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

    /* The flat view deals two covers per flip, one after the other, so it needs
     * long enough for both moves to read -- and the ease curve above spends
     * most of a flip crawling through its last tenth, which is a rotation
     * settling in the 3D view and a stall in this one. Constant rate instead,
     * at the pace the user is scrolling: see flat_rate(). */
    if (flat_view())
        speed = 65536 * HZ / (CAROUSEL_NOMINAL_FPS * flat_rate());

    /* Advance by elapsed time, not per call. speed is calibrated as a
     * per-frame step at CAROUSEL_NOMINAL_FPS; a slower frame then takes a
     * bigger step instead of making the flip take longer. Frame cost varies
     * with the settings, and playback competes for the CPU (see
     * app_claim_buffer()'s comment in init()), so a per-call advance cost
     * twice over: fewer frames per second, and a flip that stretched in
     * proportion because it still needed the same number of frames.
     *
     * The remainder is carried because HZ is 100 -- a ~30ms frame is about
     * three ticks, and truncating each one would jitter the step size by a
     * third. */
    long now = current_tick;
    int dt = now - anim_last_tick;
    anim_last_tick = now;
    if (dt < 0)
        dt = 0;
    else if (dt > HZ / 5)
        dt = HZ / 5;        /* a stall must not teleport the flip */

    int num = speed * dt * CAROUSEL_NOMINAL_FPS + anim_frac;
    int adv = num / HZ;
    anim_frac = num - adv * HZ;

    /* Clamping dt bounds the time step, not the distance. speed reaches
     * 133120 and target is never more than two slides away, so one long
     * frame could sail past it: the direction test at the end of this
     * function would throw the flip into reverse, and center_index would
     * meanwhile leave 0..number_of_slides-1, which album_covers.c's
     * draw_album_text() uses to index carousel_idx.album_index[] without a
     * range
     * check. Land on the target exactly instead. */
    int remain = (step < 0) ? slide_frame - (target << 16)
                            : (target << 16) - slide_frame;
    if (remain < 0)
        remain = 0;
    if (adv > remain)
        adv = remain;

    slide_frame += adv * step;

    /* Which slide is in the centre, and how far this flip has got towards
     * the next one. A boundary is shared by two gaps, so the answer depends
     * on the direction: moving right, C<<16 starts the gap above C; moving
     * left it ends the gap below C, and the centre is still C. Rounding
     * away from the direction of travel resolves both, and matters because
     * a frame can land exactly on a boundary -- the advance above is zero
     * for two frames inside the same 10ms tick, and is clamped to land on
     * the target.
     *
     * tick is progress through the gap, 0 at the start, and neg is what
     * remains; both are measured along the direction of travel, so the
     * geometry below reads the same either way. */
    int index = (step < 0) ? ((slide_frame + 0xffff) >> 16)
                           : (slide_frame >> 16);

    if (center_index != index) {
        center_index = index;
        /* Decided per cover, at the crossing, where every card is on its
         * resting place and the choice costs nothing to make. */
        pf_flat_snap = flat_pace() < flat_ticks(PF_FLAT_MIN_MS);
        queue_post(&thread_q, EV_WAKEUP, 0);
        slide_frame = index << 16;
        center_slide.slide_index = center_index;
        for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++)
            left_slides[i].slide_index = center_index - 1 - i;
        for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++)
            right_slides[i].slide_index = center_index + 1 + i;
    }

    /* Derived after the snap, so nothing can describe the gap we just left:
     * fade drives the whole alpha ramp and ftick the slide geometry, and
     * carrying either across a crossing shows as a one-frame flash. */
    int tick = (step < 0) ? ((-slide_frame) & 0xffff)
                          : (slide_frame & 0xffff);
    int neg = 65536 - tick;
    PFreal ftick = (tick * PFREAL_ONE) >> 16;

    /* the leftmost and rightmost slide must fade away */
    fade = ((step < 0) ? neg : tick) / 256;

    if (center_index == target) {
        reset_slides();
        pf_state = pf_idle;
        slide_frame = center_index << 16;
        step = 0;
        fade = 256;
        return;
    }

    if (flat_view())
        flat_animate(step, ftick);
    else
        coverflow_animate(step, tick, neg, ftick);

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

    /* Nothing to free: carousel_idx.buf points into the shared app buffer (see
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

    /* Album-name font: the shared bold UI font. settings.c loads it once and
     * owns its lifecycle (so we must not unload it), and it already falls back
     * to the real configured UI font when the theme has no bold font -- so this
     * can be used unconditionally. Using the shared id (not the FONT_UI
     * constant) also sidesteps FONT_UI's fallback slot-search, which could
     * otherwise resolve to some other theme font in a higher slot.
     *
     * Set before the caption strip below, which measures it. */
    pf_bold_font = font_get_ui_bold();

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
     * The band is always sized for a two-line caption, whichever mode is set,
     * so the covers do not change size between the album and artist screens --
     * carousel_caption_layout() centres whatever it is actually given in the
     * space this opens up. */
    {
        bool text_at_top;
        int draw_height;

        switch (global_settings.album_covers_show_album_name)
        {
            case ALBUM_NAME_HIDE:
                pf_caption_band = 0;
                text_at_top = false;
                break;
            case ALBUM_NAME_TOP:
            case ALBUM_AND_ARTIST_TOP:
                pf_caption_band = caption_block(true) + 2 * PF_CAPTION_PAD;
                text_at_top = true;
                break;
            case ALBUM_NAME_BOTTOM:
            case ALBUM_AND_ARTIST_BOTTOM:
            default:
                pf_caption_band = caption_block(true) + 2 * PF_CAPTION_PAD;
                text_at_top = false;
                break;
        }

        draw_height = pf_height - pf_caption_band;
        if (draw_height < 0)
            draw_height = 0;

        pf_half_height = draw_height / 2;
        pf_lower_half = draw_height - pf_half_height;
        pf_draw_y_shift = text_at_top ? pf_caption_band : 0;

        /* Where the caption itself lands, so a frame can repaint that and
         * nothing else. Asked for two lines because that block contains the
         * one-line one: both are centred on the same point. */
        if (pf_caption_band > 0)
        {
            struct pf_caption cap;

            carousel_caption_layout(true, &cap);
            pf_text_y = MAX(0, cap.y1);
            pf_text_h = MIN(pf_height, cap.y1 + caption_block(true)) - pf_text_y;
            if (pf_text_h < 0)
                pf_text_h = 0;
        }
        else
        {
            pf_text_y = 0;
            pf_text_h = 0;
        }

        /* Nothing has been drawn yet, so claim the lot: the first frame is a
         * full one regardless, and this must never read as "the covers are
         * clear of the caption" before a render has said so. */
        pf_slides_y0 = 0;
        pf_slides_y1 = pf_height;
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

    /* No failure to check for: the region is a static array, so the call either
     * returns it or panics because someone else is holding it. */

    /* store buffer pointers and sizes */
    carousel_idx.buf = buf;
    carousel_idx.buf_sz = buf_size;

    lcd_setfont(screens[SCREEN_MAIN].getuifont());

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

    /* Thumbnails come from the background album-art cache (art_cache.c), not
     * from here. Marking inspection complete up front keeps the idle-loop
     * generator from running and the navigation "wait for cache" splashes
     * from appearing; slides are read from the shared .aat cache, falling
     * back to a pfraw or the empty slide. */
    aa_cache.inspected = model->count();

    /* Reserve the album-art scratch buffer. Both models need it: create_empty_slide()
     * builds the placeholder into aa_cache.buf, and (album only) the pfraw
     * generator caches thumbnails there. */
    size_t aa_min = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(pix_t);
    size_t aa_bufsz = ALIGN_DOWN(MAX(aa_min * 3, carousel_idx.buf_sz / 8),
                                 sizeof(long));
    if (aa_bufsz < aa_min)
    {
        error_wait("Not enough memory for album art cache");
        return false;
    }

    ALIGN_BUFFER(carousel_idx.buf, carousel_idx.buf_sz, sizeof(long));
    aa_cache.buf = (char*) carousel_idx.buf;
    aa_cache.buf_sz = aa_bufsz;

    carousel_idx.buf += aa_bufsz;
    carousel_idx.buf_sz -= aa_bufsz;

    pf_cover_size_idx = art_cache_size_index("coverflow");

    buflib_init(&buf_ctx, (void *)carousel_idx.buf, carousel_idx.buf_sz);
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

#if PF_SHOW_STATS
/* One second's worth of frames, reduced to a line drawn over the covers.
 *
 * The line reads "<n>fps r<us> f<us> b<pct>% l<n>": the frame rate actually
 * achieved, the average microseconds inside the render and inside the flush,
 * the share of wall clock those two accounted for, and how many slides the
 * loader fetched. Between them those separate the three candidates, which is
 * the whole point of it:
 *
 *   fps at the cap                -> this loop is keeping up; look elsewhere
 *   fps low, busy high            -> saturated by its own drawing; transfer or
 *                                    draw less, and f vs r says which
 *   fps low, busy low             -> starved, not busy. The time is going to
 *                                    other threads, and l says whether the
 *                                    slide loader is the one taking it.
 *
 * Read the averages knowing that not every frame draws covers: a caption-only
 * frame counts here, contributes nothing to clr or sld, and pulls both down.
 * Scroll, where every frame is a full one, is the reading to compare against.
 */
static struct
{
    unsigned long render;       /* usec accumulated over the window */
    unsigned long yielded;
    unsigned long flush;
    int frames;
    unsigned long clear;
    unsigned long slides;
    unsigned long px;
    unsigned int loads;         /* pf_loads_total when the window opened */
    long window_start;
    char line[48];
    char line2[48];
} pf_stats;

static void pf_stats_frame(unsigned long render_us, unsigned long yield_us,
                           unsigned long flush_us)
{
    long now = current_tick;
    long elapsed;

    pf_stats.render += render_us;
    pf_stats.yielded += yield_us;
    pf_stats.flush  += flush_us;
    pf_stats.clear  += pf_clear_us;
    pf_stats.slides += pf_slides_us;
    pf_stats.px     += pf_px;
    pf_stats.frames++;

    elapsed = now - pf_stats.window_start;
    if (elapsed < HZ || pf_stats.frames < 1)
        return;

    /* Wall clock the window covered, in the same units as the totals. `busy` is
     * this screen's own share of it -- the yielded time belongs to whichever
     * thread ran, not to us. */
    unsigned long span = (unsigned long)elapsed * (1000000 / HZ);
    unsigned long busy = pf_stats.render + pf_stats.flush;

    snprintf(pf_stats.line, sizeof(pf_stats.line),
             "%ldfps r%lu y%lu f%lu b%lu%% l%u%s",
             (long)pf_stats.frames * HZ / elapsed,
             pf_stats.render / pf_stats.frames,
             pf_stats.yielded / pf_stats.frames,
             pf_stats.flush / pf_stats.frames,
             span ? busy * 100 / span : 0,
             pf_loads_total - pf_stats.loads,
             art_cache_is_busy() ? " ART" : "");

    /* nspx is the whole point: nanoseconds of slide-loop time per pixel those
     * loops wrote. Target-independent, so it means the same on both. */
    /* The clock the above was measured at, because ns/px means opposite things
     * at 30 MHz and 80 MHz -- healthy arithmetic in one reading, stalled memory
     * in the other -- and cpu_boost() being *called* is not evidence that it is
     * in effect. */
    snprintf(pf_stats.line2, sizeof(pf_stats.line2),
             "clr%lu sld%lu ns%lu %ldM b%d",
             pf_stats.clear / pf_stats.frames,
             pf_stats.slides / pf_stats.frames,
             pf_stats.px ? pf_stats.slides * 1000 / pf_stats.px : 0,
             FREQ / 1000000, get_cpu_boost_counter());

    pf_stats.render = 0;
    pf_stats.yielded = 0;
    pf_stats.flush = 0;
    pf_stats.clear = 0;
    pf_stats.slides = 0;
    pf_stats.px = 0;
    pf_stats.frames = 0;
    pf_stats.loads = pf_loads_total;
    pf_stats.window_start = now;
}

static void pf_stats_draw(void)
{
    if (!pf_stats.line[0])
        return;

    lcd_setfont(FONT_SYSFIXED);
    lcd_set_foreground(pf_fg_color);
    lcd_putsxy(2, 2, pf_stats.line);
    lcd_putsxy(2, 2 + font_get(FONT_SYSFIXED)->height, pf_stats.line2);
    lcd_setfont(screens[SCREEN_MAIN].getuifont());
}
#endif /* PF_SHOW_STATS */

static int album_covers_loop(void)
{
    int ret;
    int button;
    bool art_was_building = art_cache_is_busy();
    /* What the screen owes, split so that the two things that change on their
     * own account can be repaired on their own account. The covers are the
     * expensive half by a wide margin, and while a track plays most frames want
     * nothing to do with them: the status bar ticks its clock about seven times
     * a second, and a caption too long for its box steps sideways at the scroll
     * rate. Both used to cost a full viewport clear, a re-render of every slide
     * and a full-screen flush.
     *
     * All three are sticky: the frame rate cap can defer a frame to a later
     * time round the loop, and a reason to draw must survive until one draws. */
    bool art_owed = true;
    bool caption_owed = true;
    bool full_flush_owed = false;
    /* Far enough back that the first frame is due immediately. */
    long last_frame_tick = current_tick - PF_FRAME_TICKS;
    unsigned int seen_slides_loaded = pf_slides_loaded;

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
            art_owed = true;   /* every slide on screen just went away */
        }
        art_was_building = art_building;

        /* Get input first. The SBS renders during get_custom_action() and
         * writes into the framebuffer (including decorative viewports that
         * overlap our area). Inhibit the SBS's lcd_update() so it doesn't
         * push that content to the display -- our own lcd_update() after
         * rendering will push both the SBS status bar and our content
         * atomically, avoiding one-frame flicker of theme artifacts. */

        /* Idle-wake-rate backoff: how long to wait for a button before looking
         * around again. Waking finds nothing to draw most of the time (see the
         * frame test below) and costs almost nothing, so this only decides how
         * promptly the screen notices a change, not how much work it does.
         * Back off to twice a second when it is genuinely static: not animating
         * a scroll, no caption text scrolling, cover cache fully built, no
         * dynamic-colour fade in progress, and playback stopped (while playing,
         * the status bar's time updates every second and a track change can
         * start a colour fade, both of which want the normal rate). A button
         * press interrupts the wait immediately, so responsiveness is
         * unaffected. */
        bool caption_scrolling = scroll_lines[PF_SCROLL_TRACK].step
                              || scroll_lines[PF_SCROLL_ALBUM].step
                              || scroll_lines[PF_SCROLL_ARTIST].step;
        bool quiescent = pf_state != pf_scrolling
                      && !caption_scrolling
                      && aa_cache.inspected >= carousel_idx.album_ct
                      && !dynamic_colors_needs_repaint()
                      && !(audio_status() & AUDIO_STATUS_PLAY);
        int timeout;

        if (pf_state == pf_scrolling || art_owed || caption_owed)
        {
            /* There is a frame coming, so wait out the rest of its interval
             * rather than going round again to find it still not due. Waiting
             * zero -- which is what an animating carousel used to do -- turns
             * the cap into a spin, and burns exactly the CPU the cap exists to
             * hand back. */
            /* Zero when the frame is already due or overdue: the poll returns
             * at once and it is drawn. Waiting a tick there -- which is what
             * this did at first -- throws 10 ms away on every frame that
             * overruns its own interval, which on the 5G is all of them. */
            long due_in = PF_FRAME_TICKS - (current_tick - last_frame_tick);
            timeout = (due_in < 0) ? 0 : (int)due_in;
        }
        else
            timeout = quiescent ? HZ/2 : HZ/16;

        skin_inhibit_flush(true);
        button = get_custom_action(CONTEXT_PLUGIN, timeout, get_context_map);
        skin_inhibit_flush(false);

        /* What the status bar drew during that call, and where. Taking the
         * rectangles takes responsibility for flushing them, which is this
         * screen's job anyway -- the inhibit above stopped the skin engine from
         * doing its own.
         *
         * A theme's status bar is normally confined to the strip above this
         * screen's viewport, and then nothing it drew has anything to do with
         * the covers: the rectangle goes straight to the display and no frame
         * is owed at all. That is the whole point of asking where rather than
         * whether -- while a track plays the clock alone used to cost seven
         * full frames a second.
         *
         * A theme is free to paint over the covers as well, though, and then
         * the framebuffer no longer matches the display: the covers have to be
         * drawn again, and the flush has to carry the rows above the viewport
         * with them, so it becomes an ordinary full frame. */
        {
            struct skin_dirty_rect dirty[SKIN_MAX_DIRTY_RECTS];
            int n = skin_take_dirty_rects(SCREEN_MAIN, dirty);
            int i;

            for (i = 0; i < n; i++)
            {
                if (dirty[i].y < pf_vp_y + pf_height
                    && dirty[i].y + dirty[i].h > pf_vp_y)
                {
                    art_owed = true;
                    full_flush_owed = true;
                }
                else
                    lcd_update_rect(dirty[i].x, dirty[i].y,
                                    dirty[i].w, dirty[i].h);
            }
        }

        /* Runs every time round, not just on the frames that draw: it is what
         * notices that a caption is due to move. */
        if (update_scroll_lines())
            caption_owed = true;

        if (pf_state == pf_scrolling)
            art_owed = true;

        if (seen_slides_loaded != pf_slides_loaded)
        {
            seen_slides_loaded = pf_slides_loaded;
            art_owed = true;
        }

        /* The caption is drawn in the resolved foreground colour, so a fade
         * moves it as much as it moves the covers. */
        if (dynamic_colors_needs_repaint())
            art_owed = true;

        /* Repainting the caption on its own means wiping its rows first, and
         * that is only safe where no cover reached into them. Whether one did
         * is a property of the album on screen rather than of the layout --
         * art keeps its aspect ratio -- so it is measured, by the render that
         * last ran. Where they do meet, the covers have to be redrawn too. */
        if (caption_owed && !art_owed
            && (pf_text_h <= 0
                || (pf_text_y < pf_slides_y1
                    && pf_text_y + pf_text_h > pf_slides_y0)))
            art_owed = true;

        /* Redrawing the covers wipes the whole viewport, caption included. */
        if (art_owed)
            caption_owed = true;

        /* Draw only what would come out different, and no more often than
         * PF_MAX_FPS. A frame with the covers in it is a full viewport clear, a
         * re-render of every slide and a flush of the screen, which is far and
         * away the most expensive thing this loop does -- and idle frames used
         * to pay all of it to arrive at pixels identical to the ones already
         * there. Playback was what paid for them, since the wait above
         * deliberately keeps its normal rate while a track is running.
         *
         * Timed from the start of the frame, so the cap is a rate and not a
         * gap: a frame that overruns its own interval simply makes the next one
         * due immediately, rather than having its cost added to the wait. */
        if ((art_owed || caption_owed)
            && current_tick - last_frame_tick >= PF_FRAME_TICKS)
        {
            bool drew_art = art_owed;

            art_owed = false;
            caption_owed = false;
            last_frame_tick = current_tick;
#if PF_SHOW_STATS
            unsigned long t_start = USEC_TIMER, t_flush;
            pf_yield_us = 0;
#endif

            /* SBS rendering in get_custom_action resets the viewport to
             * default, and also leaves the global draw mode at whatever the
             * skin's own text/menu elements last used (typically DRMODE_SOLID,
             * an opaque background block) -- restore both, or
             * draw_album_text()'s lcd_putsxy() below inherits that solid mode
             * and paints an opaque block behind the album/artist text instead
             * of drawing transparently over the coverflow's own background. */
            lcd_set_viewport(&pf_vp);
            lcd_set_drawmode(DRMODE_FG);

            pf_update_dynamic_colors();

            if (drew_art)
            {
                if (pf_state == pf_scrolling)
                    update_scroll_animation();
                render_all_slides();
            }
            else
            {
                /* Only the caption moved. Its rows were not drawn into by the
                 * covers -- checked above -- so wiping them disturbs nothing
                 * else, and the background is the same colour the full clear
                 * would have used. */
                lcd_set_background(pf_bg_color);
                pf_clear_band(pf_text_y, pf_text_h);
            }
            model->draw_text();
#if PF_SHOW_STATS
            /* Sits over the covers, so it only has anywhere to go on a frame
             * that cleared them. */
            if (drew_art)
                pf_stats_draw();
            t_flush = USEC_TIMER;
#endif

            /* Copy offscreen buffer to LCD and give time to other threads.
             * Everything below pf_vp_y is this screen's own; the rows above it
             * only need sending when the status bar redrew into them. Full
             * width in every case, which is the single-burst path in both
             * targets' drivers -- a narrower rect goes line by line. */
            if (full_flush_owed)
            {
                lcd_update();
                full_flush_owed = false;
            }
            else if (drew_art)
                lcd_update_rect(0, pf_vp_y, LCD_WIDTH, pf_height);
            else
                lcd_update_rect(0, pf_vp_y + pf_text_y, LCD_WIDTH, pf_text_h);
#if PF_SHOW_STATS
            pf_stats_frame((t_flush - t_start) - pf_yield_us, pf_yield_us,
                           USEC_TIMER - t_flush);
#endif
            yield();
        }

        /* Whatever the button below is about to change, the frame showing it
         * has not been drawn yet -- this one went out before the switch. */
        if (button != ACTION_NONE)
            art_owed = true;

        switch (button) {
        case PF_QUIT:
            return GO_TO_PREVIOUS;
        case PF_WPS:
            return GO_TO_WPS;
        case PF_BACK:
            /* Whichever screen this was opened from, as anywhere else in the
             * firmware. Not the WPS: this is a main-menu screen in its own
             * right, reachable without anything playing. */
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

    /* Jump to selected_file's album if one was passed (e.g. context_menu.c's
     * "Album covers" context-menu item on a specific track), otherwise the
     * currently-playing track's album, otherwise wherever was last viewed. */
    model->set_initial(selected_file);

    ret = album_covers_loop();

    cleanup();

    /* CAROUSEL_PLAY_ALBUM belongs with these: album_covers() turns it into
     * GO_TO_WPS, so the screen underneath is about to be replaced either way
     * and a refresh on the way past would only flash it. */
    if (ret == GO_TO_WPS || ret == GO_TO_PREVIOUS_MUSIC || ret == GO_TO_PLUGIN
        || ret == CAROUSEL_PLAY_ALBUM)
        pop_current_activity_without_refresh();
    else
        pop_current_activity();

    return ret;
}