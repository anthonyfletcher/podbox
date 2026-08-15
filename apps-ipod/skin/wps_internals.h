/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/skin_engine/wps_internals.h
 * Copyright (C) 2007 Nicolas Pennequin
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Internal types shared across the skin engine: wps_data, the element and
 * viewport structures, and the token enum. Not for use outside skin/.
 ****************************************************************************/

 /* This stuff is for the wps engine only.. anyone caught using this outside
  * of apps/gui/wps_engine will be shot on site! */

#ifndef _WPS_ENGINE_INTERNALS_
#define _WPS_ENGINE_INTERNALS_

#include "tag_table.h"
#include "skin_parser.h"
#include "core_alloc.h"
#include "kernel.h"
#include "draw/img_filter.h"

struct wps_data;

struct skin_stats {
    size_t buflib_handles;
    size_t browser_size;
    size_t images_size;
};

int skin_get_num_skins(void);
struct skin_stats *skin_get_stats(int number, int screen);
#define skin_clear_stats(stats) memset(stats, 0, sizeof(struct skin_stats))
bool skin_backdrop_get_debug(int index, char **path, int *ref_count, size_t *size);

/*
 * setup up the skin-data from a format-buffer (isfile = false)
 * or from a skinfile (isfile = true)
 */
bool skin_data_load(enum screen_type screen, struct wps_data *wps_data,
                    const char *buf, bool isfile, struct skin_stats *stats);

/* Timeout unit expressed in HZ. In WPS, all timeouts are given in seconds
   (possibly with a decimal fraction) but stored as integer values.
   E.g. 2.5 is stored as 25. This means 25 tenth of a second, i.e. 25 units.
*/
#define TIMEOUT_UNIT (HZ/10) /* I.e. 0.1 sec */
#define DEFAULT_SUBLINE_TIME_MULTIPLIER 20 /* In TIMEOUT_UNIT's */

/* TODO: sort this mess out */

#include "draw/screen_access.h"
#include "statusbar.h"
#include "metadata.h"

#define TOKEN_VALUE_ONLY 0x0DEADC0D

/* wps_data*/
struct wps_token {
    union {
        char c;
        unsigned short i;
        long l;
        OFFSETTYPE(void*) data;
        void *xdata;
    } value;

    /* An enum skin_token_type id, or one of the out-of-enum ids in
     * custom_tokens.h. Widened from the enum so both sets fit; matches the
     * unsigned short that struct tag_info carries the id in. */
    unsigned short type;
    /* Whether the tag (e.g. track name or the album) refers the
       current or the next song (false=current, true=next) */
    bool next;
};

struct wps_subline_timeout {
    unsigned long next_tick;
    unsigned short hide;
    unsigned short show;
};

char* get_dir(char* buf, int buf_size, const char* path, int level);

struct skin_token_list {
    OFFSETTYPE(struct wps_token *) token;
    OFFSETTYPE(struct skin_token_list *) next;
};

struct gui_img {
    int16_t x;                  /* x-pos */
    int16_t y;                  /* y-pos */
    int16_t num_subimages;      /* number of sub-images */
    int16_t subimage_height;    /* height of each sub-image */
    struct bitmap bm;
    int buflib_handle;
    OFFSETTYPE(char*) label;
    int display;
    bool loaded;            /* load state */
    bool using_preloaded_icons; /* using the icon system instead of a bmp */
    bool is_9_segment;
    bool dither;
};

struct image_display {
    OFFSETTYPE(char*) label;
    OFFSETTYPE(struct wps_token*) token; /* the token to get the subimage number from */
    int16_t subimage;
    int16_t offset; /* offset into the bitmap strip to start */
};

struct progressbar {
    enum skin_token_type type;
    bool  follow_lang_direction;
    bool horizontal;
    char setting_offset;
    /* regular pb */
    int16_t x;
    /* >=0: explicitly set in the tag -> y-coord within the viewport
       <0 : not set in the tag -> negated 1-based line number within
            the viewport. y-coord will be computed based on the font height */
    int16_t y;
    int16_t width;
    int16_t height;

    OFFSETTYPE(struct gui_img *) image;
    bool invert_fill_direction;
    bool nofill;
    bool noborder;
    bool nobar;
    OFFSETTYPE(struct gui_img *) slider;

    OFFSETTYPE(struct gui_img *) backdrop;
    const struct settings_list *setting;
};

struct draw_rectangle {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
    unsigned start_colour;
    unsigned end_colour;
    /* 0..LCD_BLEND_OPAQUE; opaque unless the theme said otherwise, so every
     * existing %dr keeps taking the plain fill path. */
    uint8_t opacity;
    /* Corner radius, 0 for square corners. The coverage mask is cut to this
     * radius and this opacity when the tag is parsed, since neither can
     * change afterwards; see draw/round_rect.h. */
    uint8_t radius;
    OFFSETTYPE(unsigned char *) mask;
};

struct align_pos {
    char* left;
    char* center;
    char* right;
};

#define WPS_MAX_TOKENS      1150

enum wps_parse_error {
    PARSE_OK,
    PARSE_FAIL_UNCLOSED_COND,
    PARSE_FAIL_INVALID_CHAR,
    PARSE_FAIL_COND_SYNTAX_ERROR,
    PARSE_FAIL_COND_INVALID_PARAM,
    PARSE_FAIL_LIMITS_EXCEEDED,
};
struct gradient_config {
    unsigned start;
    unsigned end;
    unsigned text;
    int lines_count;
};

#define VP_DRAW_HIDEABLE    0x1
#define VP_DRAW_HIDDEN      0x2
#define VP_DRAW_WASHIDDEN   0x4
/* these are never drawn, nor cleared, i.e. just ignored */
#define VP_NEVER_VISIBLE    0x8
#define VP_DEFAULT_LABEL    -200
#define VP_DEFAULT_LABEL_STRING "|"
struct skin_viewport {
    struct viewport vp;   /* The LCD viewport struct */
    struct frame_buffer_t framebuf; /* holds reference to current framebuffer */
    OFFSETTYPE(char*) label;
    int16_t parsed_fontid;
    char hidden_flags;
    bool is_infovp;
    bool output_to_backdrop_buffer;
    bool fgbg_changed;
    struct gradient_config start_gradient;
    unsigned int dc_orig_fg; /* original parsed fg for dynamic colors */
    unsigned int dc_orig_bg; /* original parsed bg for dynamic colors */
};
struct viewport_colour {
    unsigned colour;
    bool is_default; /* true if parsed from `-` (theme default) */
};


struct playlistviewer {
    bool show_icons;
    int start_offset;
    OFFSETTYPE(struct skin_element *) line;
};



/* albumart definitions */
#define WPS_ALBUMART_ALIGN_RIGHT    1    /* x align:   right */
#define WPS_ALBUMART_ALIGN_CENTER   2    /* x/y align: center */
#define WPS_ALBUMART_ALIGN_LEFT     4    /* x align:   left */
#define WPS_ALBUMART_ALIGN_TOP      1    /* y align:   top */
#define WPS_ALBUMART_ALIGN_BOTTOM   4    /* y align:   bottom */

/* What one %Cd draws: which art, and optionally a window -- a rectangle of the
 * viewport to reveal instead of the whole art box.
 *
 * The window is in viewport coordinates, the same frame %Cl's x/y are in, and
 * the art stays anchored where %Cl put it. So the window opens onto the
 * composition rather than onto the bitmap: slide it and a different part of
 * the cover shows through, which is what lets one buffered art serve several
 * cut-outs without a second slot. w == 0 means no window. */
struct skin_albumart_draw {
    OFFSETTYPE(struct skin_albumart *) art;
    int16_t x, y, w, h;
};

struct skin_albumart {
    /* Album art support */
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;

    unsigned char xalign; /* WPS_ALBUMART_ALIGN_LEFT, _CENTER, _RIGHT */
    unsigned char yalign; /* WPS_ALBUMART_ALIGN_TOP, _CENTER, _BOTTOM */

    /* %Cl's ninth parameter: the name a %Cd calls this art by. NULL when the
     * skin gave none, which is the one a bare %Cd draws. */
    OFFSETTYPE(const char *) label;

    /* Where playback buffers this art. Claimed by dimension, so two %Cl
     * asking for the same size -- in one skin or in two -- share a slot and
     * the second costs the audio buffer nothing. -1 if none was free. */
    int slot;

    int draw_handle;

    /* The window the %Cd that asked for this draw wants, or NULL for the
     * whole art box. Pending state like draw_handle: the %Cd sets both, the
     * viewport's wps_display_images() consumes them. One %Cd per art per
     * viewport, therefore -- a second overwrites the first, which is how a
     * repeated %Cd has always behaved. Put each window in its own viewport. */
    OFFSETTYPE(struct skin_albumart_draw *) draw_win;

    /* The chain %Cl's seventh parameter named, compiled at skin load. No
     * stages means the skin asked for no filtering. */
    struct img_filter filter;

    /* A chain that blurs cannot work in place, so it gets a destination of
     * its own: one buflib block the size of the bounding box, allocated at
     * skin load rather than at track change, since a core_alloc from a
     * screen shrinks the audio buffer and forces a rebuffer. -1 when the
     * chain does not need one, which is every chain but a blurring one.
     *
     * `filtered_art` is the art handle already rendered into it. Per skin
     * rather than per slot -- unlike the in-place case there is no danger in
     * two skins rendering the same art, and they each have their own
     * destination to render it into. */
    int filter_handle;
    int filtered_art;
    short filtered_width;      /* what is really in it: the source fitted */
    short filtered_height;     /* inside the box, so smaller than it      */

    /* Corner radius, 0 for square corners, with the coverage mask cut for it
     * when the skin is parsed. The mask depends only on the radius -- the two
     * planes carry their own strides -- so the one mask serves the art at
     * whatever size it turns out to be. See draw/round_rect.h. */
    uint8_t radius;
    OFFSETTYPE(unsigned char *) mask;
};

/* The art hanging off one node of wps_data->albumart. `buf` is the buffer the
 * offsets are relative to: get_skin_buffer(data) once the skin is loaded, the
 * parse buffer before that. Walk the chain with
 *
 *   for (node = SKINOFFSETTOPTR(buf, data->albumart); node;
 *        node = SKINOFFSETTOPTR(buf, node->next))
 */
static inline struct skin_albumart *skin_albumart_of(char *buf,
                                        const struct skin_token_list *node)
{
    struct wps_token *token = node ? SKINOFFSETTOPTR(buf, node->token) : NULL;
    return token ? SKINOFFSETTOPTR(buf, token->value.data) : NULL;
}

/* The art a %Cd with no label draws: the skin's first %Cl. */
static inline struct skin_albumart *skin_albumart_first(char *buf,
                                        OFFSETTYPE(struct skin_token_list *) l)
{
    return skin_albumart_of(buf, SKINOFFSETTOPTR(buf, l));
}


struct line {
    unsigned update_mode;
};

struct line_alternator {
    int current_line;
    unsigned long next_change_tick;
};

struct conditional {
    int last_value;
    OFFSETTYPE(struct wps_token *) token;
};

struct logical_if {
    OFFSETTYPE(struct wps_token *) token;
    enum {
        IF_EQUALS, /* == */
        IF_NOTEQUALS, /* != */
        IF_LESSTHAN, /* < */
        IF_LESSTHAN_EQ, /* <= */
        IF_GREATERTHAN, /* > */
        IF_GREATERTHAN_EQ /* >= */
    } op;
    struct skin_tag_parameter operand;
    int num_options;
};

struct substring {
    int16_t start;
    int16_t length;
    bool expect_number;
    OFFSETTYPE(struct wps_token *) token;
};

struct listitem {
    bool wrap;
    int16_t offset;
    /* %La only: corner radius for the row's cover, 0 for square, with the
     * mask cut for it at parse time. Unlike %Cl there is nothing to clamp it
     * against here -- a row's art is sized by its viewport at draw time -- so
     * a radius too large for the cover that turns up is dropped there. */
    uint8_t radius;
    OFFSETTYPE(unsigned char *) mask;
};

/* %Sb(bars[,align[,radius]]): how many bars to split the band table into,
 * whether they grow from the viewport floor or from its middle, and the
 * corner radius of each bar (0 for square). */
struct spectrum_bars {
    int16_t bars;
    int16_t radius;
    int16_t gap;        /* pixels between bars; 1 unless the skin said otherwise */
    bool center_aligned;
};

/* %wt(text[,align[,fallback]]): the text token to draw, an optional fallback
 * token used when text is empty (e.g. %fn when %it has no title), plus vertical
 * ('t'/'c'/'b') and horizontal ('l'/'c'/'r') alignment within the viewport. */
struct skin_textbox {
    OFFSETTYPE(struct wps_token *) token;
    OFFSETTYPE(struct wps_token *) fallback;
    char valign;
    char halign;
};

struct listitem_viewport_cfg {
    struct wps_data *data;
    OFFSETTYPE(char *)   label;
    int16_t     width;
    int16_t     height;
    int16_t     xmargin;
    int16_t     ymargin;
    bool    tile;
};

/* Skin variables: a named integer a skin can set (%vs), read back (%vg) and
 * test the age of (%vl), which is how a skin holds state of its own between
 * refreshes. Upstream gates these on HAVE_SKIN_VARIABLES, which it only defines
 * for touchscreen targets; here they are always available. */
struct skin_var {
    OFFSETTYPE(const char *) label;
    int value;
    long last_changed;
};
struct skin_var_lastchange {
    OFFSETTYPE(struct skin_var *) var;
    long timeout;
};
struct skin_var_changer {
    OFFSETTYPE(struct skin_var *) var;
    int newval;
    bool direct; /* true to make val=newval, false for val += newval */
    int max;
};


/* wps_data
   this struct holds all necessary data which describes the
   viewable content of a wps */
struct wps_data
{
    int buflib_handle;

    OFFSETTYPE(struct skin_element *) tree;
    OFFSETTYPE(struct skin_token_list *) images;
    OFFSETTYPE(struct skin_token_list *) skinvars;
    OFFSETTYPE(int16_t *) font_ids;
    int16_t font_count;
    int16_t backdrop_id;
    bool use_extra_framebuffer;

    /* Every %Cl the skin declared, in the order written. A %Cd draws one of
     * them by label, a bare %Cd the first. Several cost no more than one
     * unless they ask for different sizes -- see skin_albumart.slot. */
    OFFSETTYPE(struct skin_token_list *) albumart;


    bool peak_meter_enabled;
    bool spectrum_enabled;
    bool wps_sb_tag;
    bool show_sb_on_wps;
    bool wps_loaded;
};

static inline char* get_skin_buffer(struct wps_data* data)
{
    if (data->buflib_handle > 0)
        return core_get_data(data->buflib_handle);
    return NULL;
}

/* wps_data end */

/* gui_wps
   defines a wps with its data, state,
   and the screen on which the wps-content should be drawn */
struct gui_wps
{
    struct screen *display;
    struct wps_data *data;
};

/* gui_wps end */

void get_image_filename(const char *start, const char* bmpdir,
                                char *buf, int buf_size);
/***** wps_tokens.c ******/

const char *get_token_value(struct gui_wps *gwps,
                           struct wps_token *token, int offset,
                           char *buf, int buf_size,
                           int *intval);

/* Get the id3 fields from the cuesheet */
/* whole-playlist progress backing %pX (number and bar forms) */
bool wps_get_playlist_percent(struct mp3entry *id3, unsigned long elapsed_ms,
                              unsigned long *elapsed_s, unsigned long *total_s);
void wps_playlist_percent_enable(void);
void wps_playlist_percent_prepare(void);

const char *get_cuesheetid3_token(struct wps_token *token, struct mp3entry *id3,
                                  int offset_tracks, char *buf, int buf_size);
const char *get_id3_token(struct wps_token *token, struct mp3entry *id3,
                          char *filename, char *buf, int buf_size, int limit, int *intval);

enum skin_find_what {
    SKIN_FIND_VP = 0,
    SKIN_FIND_UIVP,
    SKIN_FIND_IMAGE,
    SKIN_FIND_ALBUMART,
    SKIN_VARIABLE,
};
void *skin_find_item(const char *label, enum skin_find_what what,
                     struct wps_data *data);

/* Skin parse tracing. The three call sites in skin_parser.c report which file
 * each viewport and image came from, and which lang phrase every %Sx()
 * resolved to. debug_wps itself is defined by the SDL backend and set from
 * the simulator's --debugwps; CheckWPS turns the same output on unconditionally.
 * Neither is a hardware build, so nothing here reaches one. */
#if defined(SIMULATOR) || defined(CHECKWPS)
#define DEBUG_SKIN_ENGINE
extern bool debug_wps;
#endif

#endif
