/***************************************************************************
 * Original code from RockBox
 * was: apps/settings.h
 * Copyright (C) 2002 by Stuart Martin
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * The global_settings and global_status structures -- the entire persisted
 * state of the device -- plus the settings API.
 ****************************************************************************/

#ifndef __SETTINGS_H__
#define __SETTINGS_H__

#include <stdbool.h>
#include <stddef.h>
#include "inttypes.h"
#include "config.h"
#include "audiohw.h" /* for the AUDIOHW_* defines */
#include "skin/statusbar.h" /* for the statusbar values */
#include "screens/playback/quick_screen.h"
#include "button.h"
#include "audio.h"
#include "dsp_proc_settings.h"

struct opt_items {
    unsigned const char* string;
    int32_t voice_id;
};

/** Setting values defines **/
#define MAX_FILENAME 32
#define MAX_PATHNAME 80
#define MAX_PATHLIST (MAX_PATHNAME*2)

/* The values are assigned to the enums so that they correspond to */
/* setting values in settings_list.c                               */

/* Shared by all bookmark parameters */
enum {
    BOOKMARK_NO = 0,
    BOOKMARK_YES = 1,
};

/* Auto create bookmark */
enum {
    BOOKMARK_ASK = 2,
    BOOKMARK_RECENT_ONLY_YES = 3,
    BOOKMARK_RECENT_ONLY_ASK = 4,
};

/* Most recent bookmark */
enum {
    BOOKMARK_ONE_PER_PLAYLIST = 2,
    BOOKMARK_ONE_PER_TRACK = 3,
};

/* global_settings.dialog_colors -- what a modal dialog does about colour.
 *
 * OFF and ON are the two the setting has always had, in the order it wrote
 * them, so an existing .cfg keeps its meaning. AUTO is the default, so a theme
 * that says nothing about dialogs gets the derived scheme rather than the flat
 * one. */
enum {
    DIALOG_COLORS_OFF = 0,   /* every colour inherited from the theme        */
    DIALOG_COLORS_ON = 1,    /* the nine colours named in the .cfg           */
    DIALOG_COLORS_AUTO = 2,  /* derived from the theme's, or the album's     */
};

enum
{
    TRIG_MODE_OFF = 0,
    TRIG_MODE_NOREARM,
    TRIG_MODE_REARM
};

enum
{
    TRIG_TYPE_STOP = 0,
    TRIG_TYPE_PAUSE,
    TRIG_TYPE_NEW_FILE
};

enum {
    PLAYLIST_VIEWER_ENTRY_SHOW_FILE_NAME = 0,
    PLAYLIST_VIEWER_ENTRY_SHOW_FULL_PATH = 1,
    PLAYLIST_VIEWER_ENTRY_SHOW_ID3_TITLE_AND_ALBUM = 2,
    PLAYLIST_VIEWER_ENTRY_SHOW_ID3_TITLE = 3
};

enum {
    CROSSFADE_ENABLE_OFF = 0,
    CROSSFADE_ENABLE_AUTOSKIP,
    CROSSFADE_ENABLE_MANSKIP,
    CROSSFADE_ENABLE_SHUFFLE,
    CROSSFADE_ENABLE_SHUFFLE_OR_MANSKIP,
    CROSSFADE_ENABLE_ALWAYS,
};

enum {
    FOLDER_ADVANCE_OFF = 0,
    FOLDER_ADVANCE_NEXT,
    FOLDER_ADVANCE_RANDOM,
};

/* repeat mode options */
enum
{
    REPEAT_OFF = 0,
    REPEAT_ALL,
    REPEAT_ONE,
    REPEAT_SHUFFLE,
    REPEAT_AB,
    NUM_REPEAT_MODES
};

/* single mode options */
enum {
    SINGLE_MODE_OFF = 0,
    SINGLE_MODE_TRACK,
    SINGLE_MODE_ALBUM,
    SINGLE_MODE_ALBUM_ARTIST,
    SINGLE_MODE_ARTIST,
    SINGLE_MODE_COMPOSER,
    SINGLE_MODE_GROUPING,
    SINGLE_MODE_GENRE,
    SINGLE_MODE_PLAYLIST,
};

enum
{
    QUEUE_HIDE = 0,
    QUEUE_SHOW_AT_TOPLEVEL,
    QUEUE_SHOW_IN_SUBMENU
};

enum
{
    BROWSER_DEFAULT_FILES = 0,
    BROWSER_DEFAULT_DB,
    BROWSER_DEFAULT_PL_CAT
};

enum
{
    AA_OFF = 0,
    AA_PREFER_EMBEDDED,
    AA_PREFER_IMAGE_FILE,
    AA_PREFER_CACHE
};

/* Which picture the now-playing screen shows. The artwork cache holds both,
 * keyed by folder, so this only chooses which folder to ask it about -- see
 * load_cached_albumart() in audio/playback.c. */
enum
{
    WPS_ART_ALBUM = 0,
    WPS_ART_ARTIST
};

enum
{
    TAGCACHE_RAM_OFF = 0,
    TAGCACHE_RAM_ON = 1,
    TAGCACHE_RAM_QUICK = 2
};

/* dir filter options */
/* Note: Any new filter modes need to be added before NUM_FILTER_MODES.
 *       Any new rockbox browse filter modes (accessible through the menu)
 *       must be added after NUM_FILTER_MODES. */
/* Only the entries before NUM_FILTER_MODES reach disk: they are the values of
 * the "show files" setting. Everything after it is a compile-time argument to
 * browse_folder_info, so renumbering that half is free. SHOW_RWPS, SHOW_FMS,
 * SHOW_RFMS, SHOW_RSBS and SHOW_FMR were dropped from there -- the remote-LCD
 * and tuner browse modes, neither of which this fork has. */
enum { SHOW_ALL, SHOW_SUPPORTED, SHOW_MUSIC, SHOW_PLAYLIST, SHOW_ID3DB,
       NUM_FILTER_MODES,
       SHOW_WPS, SHOW_SBS, SHOW_CFG,
       SHOW_LNG, SHOW_MOD, SHOW_FONT, SHOW_PLUGINS, SHOW_M3U};

/* file and dir sort options */
enum { SORT_ALPHA, SORT_DATE, SORT_DATE_REVERSED, SORT_TYPE, /* available as settings */
       SORT_ALPHA_REVERSED, SORT_TYPE_REVERSED, SORT_AS_FILE }; /* internal use only */
enum { SORT_INTERPRET_AS_DIGIT, SORT_INTERPRET_AS_NUMBER };

/* recursive dir insert options */
enum { RECURSE_OFF, RECURSE_ON, RECURSE_ASK };

/* show path types */
enum { SHOW_PATH_OFF = 0, SHOW_PATH_CURRENT, SHOW_PATH_FULL };

/* scrollbar visibility/position */
enum { SCROLLBAR_OFF = 0, SCROLLBAR_LEFT, SCROLLBAR_RIGHT };

/* autoresume settings */
enum { AUTORESUME_NEXTTRACK_NEVER = 0, AUTORESUME_NEXTTRACK_ALWAYS,
       AUTORESUME_NEXTTRACK_CUSTOM};

/* Alarm settings */
#ifdef HAVE_RTC_ALARM
enum {  ALARM_START_WPS = 0,
        ALARM_START_COUNT
    };
#endif /* HAVE_RTC_ALARM */

/* Keyclick stuff */

 /* Not really a setting but several files should stay synced */
#define KEYCLICK_DURATION 2

/** virtual pointer stuff.. move to another .h maybe? **/
/* These define "virtual pointers", which could either be a literal string,
   or a mean a string ID if the pointer is in a certain range.
   This helps to save space for menus and options.

   While using 0 as the base address is fastest/simplest,
   it could result in a nullptr when ID==0 which C compilers try to
   helpfully "optimize" away.

   NOTE:  VIRT_PTR must point at an *invalid* address in the target
          memory map!
*/

#define VIRT_SIZE 0xFFFF /* more than enough for our string ID range */
#if defined(CPU_S5L87XX)
/* 256K IRAM at 0x0 */
#define VIRT_PTR ((unsigned char*)0x40000)
#elif defined(CPU_PP)
/* up to 64MB of DRAM at 0x0 */
#define VIRT_PTR ((unsigned char*)0x4000000)
#else
/* offset from 0x0 slightly */
#define VIRT_PTR ((unsigned char*)sizeof(char*))
#endif

/* form a "virtual pointer" out of a language ID */
#define ID2P(id) (VIRT_PTR + id)

/* resolve a pointer which could be a virtualized ID or a literal */
#define P2STR(p) (char *)((p>=VIRT_PTR && p<VIRT_PTR+VIRT_SIZE) ? str(p-VIRT_PTR) : p)

/* get the string ID from a virtual pointer, -1 if not virtual */
#define P2ID(p) ((p>=VIRT_PTR && p<VIRT_PTR+VIRT_SIZE) ? p-VIRT_PTR : -1)

/* !defined(HAVE_LCD_COLOR) implies HAVE_LCD_CONTRAST with default 40.
   Explicitly define HAVE_LCD_CONTRAST in config file for newer ports for
   simplicity. */



/** function prototypes **/
void reset_runtime(void);
void update_runtime(void);
void zero_runtime(void);
void settings_load(void) INIT_ATTR;
bool settings_load_config(const char* file, bool apply);

void status_save(bool force);
int settings_save(void);

/* defines for the options paramater */
enum {
    SETTINGS_SAVE_CHANGED = 0,
    SETTINGS_SAVE_ALL,
    SETTINGS_SAVE_THEME,
    SETTINGS_SAVE_SOUND,
    SETTINGS_SAVE_EQPRESET,
    SETTINGS_SAVE_RESUMEINFO,
};
bool settings_save_config(int options);

struct settings_list;
struct filename_setting;
void reset_setting(const struct settings_list *setting, void *var);
void settings_reset(void);
void sound_settings_apply(void);

/* call this after loading a .wps/.rwps or other skin files, so that the
 * skin buffer is reset properly
 */
void settings_apply_skins(void);

void settings_apply(bool read_disk);
void settings_apply_pm_range(void);

/* Shared "bold UI font" -- the theme's configured bold font (settings
 * 'font bold' / global_settings.bold_font_file), loaded once by settings_apply()
 * and reloaded on theme change. Returns the loaded font id, or the regular UI
 * font id if no bold font is configured (or it failed to load), so callers can
 * use it unconditionally. Do NOT font_unload() the returned id -- settings_apply
 * owns its lifecycle. */
int font_get_ui_bold(void);
void settings_display(void);

enum optiontype { RB_INT, RB_BOOL };

const struct settings_list* find_setting(const void* variable);
const struct settings_list* find_setting_by_cfgname(const char* name);
bool cfg_int_to_string(const struct settings_list *setting, int val, char* buf, int buf_len);
bool cfg_string_to_int(const struct settings_list *setting, int* out, const char* str);
void cfg_to_string(const struct settings_list *setting, char* buf, int buf_len);
bool string_to_cfg(const char *name, char* value, bool *theme_changed);

bool copy_filename_setting(char *buf, size_t buflen, const char *input,
                           const struct filename_setting *fs);
bool set_bool_options(const char* string, const bool* variable,
                      const char* yes_str, int yes_voice,
                      const char* no_str, int no_voice,
                      void (*function)(bool));

bool set_bool(const char* string, const bool* variable);
bool set_int(const unsigned char* string, const char* unit, int voice_unit,
             const int* variable,
             void (*function)(int), int step, int min, int max,
             const char* (*formatter)(char*, size_t, int, const char*) );

/* use this one if you need to create a lang from the value (i.e with TALK_ID()) */
bool set_int_ex(const unsigned char* string, const char* unit, int voice_unit,
             const int* variable,
             void (*function)(int), int step, int min, int max,
             const char* (*formatter)(char*, size_t, int, const char*),
             int32_t (*get_talk_id)(int, int));

void set_file(const char* filename, char* setting);

bool set_option(const char* string, const void* variable, enum optiontype type,
                const struct opt_items* options, int numoptions, void (*function)(int));

const char* setting_get_cfgvals(const struct settings_list *setting);

/** global_settings and global_status struct definitions **/

struct system_status
{
    int volume;     /* audio output volume in decibels range depends on the dac */
    int resume_index;  /* index in playlist (-1 for no active resume) */
    uint32_t resume_crc32; /* crc32 of the name of the file */
    uint32_t resume_elapsed; /* elapsed time in last file */
    uint32_t resume_offset; /* byte offset in mp3 file */
    /* Write-only, and not persisted: firmware/sound.c and
       lib/rbcodec/dsp/tdspeed.c store the value they applied here. Both mirror
       upstream, so the fields have to stay even though this fork has no pitch
       screen to set or restore them. */
    int32_t resume_pitch;
    int32_t resume_speed;
    int runtime;       /* current runtime since last charge */
    int topruntime;    /* top known runtime */
    int dircache_size;      /* directory cache structure last size, 22 bits */
    /* int, not a narrower type: the SYSTEM_STATUS macro flags these F_T_INT,
       so settings.c loads and saves them through an int pointer. A 1-byte
       member leaves the other three to alignment padding, which is what
       reaches .resume.cfg. */
    int last_screen;
    int  viewer_icon_count;
    int last_volume_change; /* tick the last volume change happened. skins use this */
    int font_id[NB_SCREENS]; /* font id of the settings font for each screen */

    int resume_modified; /* playlist is modified (=> warn before erase) */
};

struct user_settings
{
    /* audio settings */
    int balance;    /* stereo balance: -100 - +100 -100=left  0=bal +100=right  */
    int bass;       /* bass boost/cut in decibels                               */
    int treble;     /* treble boost/cut in decibels                             */
    int channel_config; /* Stereo, Mono, Custom, Mono left, Mono right, Karaoke */
    int stereo_width; /* 0-255% */

    int bass_cutoff;
    int treble_cutoff;

    /* Crossfade */
    int crossfade;     /* Enable crossfade (0=off, 1=shuffle, 2=trackskip,
                                            3=shuff&trackskip, 4=always) */
    int crossfade_fade_in_delay;      /* Fade in delay (0-15s)             */
    int crossfade_fade_out_delay;     /* Fade out delay (0-15s)            */
    int crossfade_fade_in_duration;   /* Fade in duration (0-15s)          */
    int crossfade_fade_out_duration;  /* Fade out duration (0-15s)         */
    int crossfade_fade_out_mixmode;   /* Fade out mode (0=crossfade,1=mix) */

    /* Replaygain */
    struct replaygain_settings replaygain_settings;

    /* Crossfeed */
    int crossfeed;                              /* crossfeed type */
    unsigned int crossfeed_direct_gain;         /* dB x 10 */
    unsigned int crossfeed_cross_gain;          /* dB x 10 */
    unsigned int crossfeed_hf_attenuation;      /* dB x 10 */
    unsigned int crossfeed_hf_cutoff;           /* Frequency in Hz */

    /* EQ */
    bool eq_enabled;            /* Enable equalizer */
    unsigned int eq_precut;     /* dB */
    struct eq_band_setting eq_band_settings[EQ_NUM_BANDS]; /* for each band */

    /* Misc. swcodec */
    int  beep;              /* system beep volume when changing tracks etc. */
    int  keyclick;          /* keyclick volume */
    int  keyclick_repeats;  /* keyclick on repeats */
    bool dithering_enabled;


    /* misc options */



    int  pause_rewind; /* time in s to rewind when pausing */
    int  unplug_mode; /* pause on headphone unplug */
    bool unplug_autoresume; /* disable auto-resume if no phones */

    const struct settings_list *qs_items[QUICKSCREEN_ITEM_COUNT];

    int timeformat;    /* time format: 0=24 hour clock, 1=12 hour clock */

    int disk_spindown; /* time until disk spindown, in seconds (0=off) */
    int buffer_margin; /* audio buffer watermark margin, in seconds */
    int storage_mode;  /* 0=auto, 1=HDD, 2=SSD */

    int dirfilter;     /* 0=display all, 1=only supported, 2=only music,
                          3=dirs+playlists, 4=ID3 database */
    int show_filename_ext; /* show filename extensions in file browser?
                              0 = no, 1 = yes, 2 = only unknown 0 */
    int default_codepage;   /* set default codepage for tag conversion */
    bool hold_lr_for_scroll_in_list; /* hold L/R scrolls the list left/right */
    bool play_selected; /* Plays selected file even in shuffle mode */
    int single_mode;    /* single mode - stop after every track, album, album artist,
                           artist, composer, work, or genre */
    bool party_mode;    /* party mode - unstoppable music */
    bool cuesheet;
    bool car_adapter_mode; /* 0=off 1=on */
    int car_adapter_mode_delay; /* delay before resume,  in seconds*/
    int start_in_screen;
    int wps_select_action;
    int ff_rewind_min_step; /* FF/Rewind minimum step size */
    int ff_rewind_accel; /* FF/Rewind acceleration (in seconds per doubling) */

    int peak_meter_release;   /* units per read out */
    int peak_meter_hold;      /* hold time for peak meter in 1/100 s */
    int peak_meter_clip_hold; /* hold time for clips */
    bool peak_meter_dbfs;     /* show linear or dbfs values */
    int peak_meter_min; /* range minimum */
    int peak_meter_max; /* range maximum */

    unsigned char wps_file[MAX_FILENAME+1];  /* last wps */
    unsigned char sbs_file[MAX_FILENAME+1];  /* last statusbar skin */
    unsigned char lang_file[MAX_FILENAME+1]; /* last language */
    unsigned char playlist_catalog_dir[MAX_PATHNAME+1];
    int skip_length; /* skip length */
    int max_files_in_dir; /* Max entries in directory (file browser) */
    int max_files_in_playlist; /* Max entries in playlist */
    int volume_type;   /* how volume is displayed: 0=graphic, 1=percent */
    int battery_display; /* how battery is displayed: 0=graphic, 1=percent */
    bool show_icons;   /* 0=hide 1=show */
    bool show_debug_menu; /* show the Debug entry in the System menu */
    int statusbar;    /* STATUSBAR_* enum values */

    int scrollbar;    /* SCROLLBAR_* enum values */
    int scrollbar_width;

    int list_separator_height; /* -1=auto (== 1 currently), 0=disabled, X=height in pixels */
    int list_separator_color;
    /* goto current song when exiting WPS */
    bool browse_current; /* 1=goto current song,
                            0=goto previous location */
    bool scroll_paginated; /* 0=dont 1=do */
    bool list_wraparound;  /* wrap around to opposite end of list when scrolling */
    int  list_order;       /* order for numeric lists (ascending or descending) */
    int  scroll_speed;     /* long texts scrolling speed: 1-30 */
    int  bidir_limit;      /* bidir scroll length limit */
    int  scroll_delay;     /* delay (in 1/10s) before starting scroll */
    int  scroll_step;      /* pixels to advance per update */

    /* auto bookmark settings */
    int autoloadbookmark;   /* auto load option: 0=off, 1=ask, 2=on */
    int autocreatebookmark; /* auto create option: 0=off, 1=ask, 2=on */
    bool autoupdatebookmark;/* auto update option */
    int usemrb;             /* use MRB list: 0=No, 1=Yes, 2=One per playlist,
                                             3=One per playlist and track */

    bool dircache;          /* enable directory cache */
    int tagcache_ram;        /* load tagcache to ram: 1=on, 2=quick (ignore dircache) */
    bool tagcache_scan_on_eject; /* rescan after a USB session that wrote? */
    bool tagcache_scan_on_startup; /* also check for changes at boot? */
    bool tagcache_autocommit;    /* finish an interrupted commit without asking */
    bool autoresume_enable;   /* enable auto-resume feature? */
    int autoresume_automatic; /* resume next track? 0=never, 1=always,
                                 2=custom */
    unsigned char autoresume_paths[MAX_PATHLIST+1]; /* colon-separated list */
    bool runtimedb;           /* runtime database active? */
    unsigned char tagcache_scan_paths[MAX_PATHLIST+1];
    unsigned char tagcache_db_path[MAX_PATHNAME+1];

    unsigned char backdrop_file[MAX_PATHNAME+1];  /* backdrop bitmap file */

    int bg_color; /* background color native format */
    int fg_color; /* foreground color native format */
    int lss_color; /* background color for the selector or start color for the gradient */
    int lse_color; /* end color for the selector gradient */
    int lst_color; /* color of the text for the selector */
    unsigned char colors_file[MAX_FILENAME+1];
    bool dynamic_colors; /* auto-color from album art */
    int wps_art_source;  /* WPS_ART_*: album cover or artist portrait */

    /* Modal dialog chrome (apps/gui/dialog.h). Applied in settings_apply() via
     * dialog_set_default_style(). The metrics always apply; what the colours do
     * depends on dialog_colors, one of DIALOG_COLORS_*. */
    int dialog_box_border_width;
    int dialog_box_margin;
    int dialog_btn_border_width;
    int dialog_btn_border_radius;
    int dialog_colors;
    int dialog_box_fg;
    int dialog_box_bg;
    int dialog_box_border;
    int dialog_btn_fg;
    int dialog_btn_bg;
    int dialog_btn_border;
    int dialog_btn_fg_sel;      /* the selected button */
    int dialog_btn_bg_sel;
    int dialog_btn_border_sel;

    /* Corner radius of the progress bar drawn by draw/progress_bar.c. */
    int progress_bar_radius;

    /* apps/gui/album_covers.c settings -- see enum show_album_name_values /
     * sort_albums_by_values / year_sort_order_values in album_covers.h for
     * what the choice settings' integer values mean. */
    bool debug_log_tagcache;   /* append tag-database progress to a file */
    bool debug_log_artcache;   /* append art-cache progress to a file */
    int  album_covers_center_margin;
    int  album_covers_slide_tuck;
    bool album_covers_parallel_slides;
    int  album_covers_scroll_speed;
    int  album_covers_transition_speed;
    int  album_covers_show_album_name;
    int  album_covers_sort_albums_by;
    int  album_covers_sort_artists_by; /* SORT_ARTISTS_*: portraits ordering */
    int  album_covers_year_sort_order;
    bool album_covers_show_year;
    int  album_covers_background; /* CAROUSEL_BG_*: the colour it fills with */
    bool album_covers_statusbar;  /* show it, or take the screen and cover it */
    /* Album covers in the database browser (tall rows + the skin's %La tag). On
     * by default; a theme sets it off in its .cfg for the stock/fast list. Off
     * also means faster scrolling (no cover decode). */
    bool db_albumart;
    /* Artist art (a photo in <artist>/folder.jpg) on artist browse rows, same
     * mechanism as db_albumart. On by default. */
    bool db_artistart;
    /* Uniform row height when album or artist art is on -- a little above the
     * cover size so the square fits. Ignored when both are off. */
    int  db_art_row_height;
    /* Art cache generation: derive the smaller thumbnails from the largest one
     * instead of decoding the source image once per size. Roughly a third of the
     * work for a slightly softer small thumbnail. Off by default. */
    bool art_cache_fast_build;

    int browser_default;        /* Default browser when accessed from WPS */

    /* playlist/playback settings */
    int  repeat_mode; /* 0=off 1=repeat all 2=repeat one 3=shuffle 4=ab */
    int  next_folder; /* move to next folder */
    bool constrain_next_folder; /* whether next_folder is constrained to
                                   directories within start_directory */
    int  recursive_dir_insert; /* should directories be inserted recursively */
    bool fade_on_stop; /* fade on pause/unpause/stop */
    bool playlist_shuffle;
    bool warnon_erase_dynplaylist; /* warn when erasing dynamic playlist */
    bool keep_current_track_on_replace_playlist;
    bool show_shuffled_adding_options; /* whether to display options for adding shuffled tracks to dynamic playlist */
    int show_queue_options; /* how and whether to display options to queue tracks */
    int album_art; /* switch off album art display or choose preferred source */
    bool rewind_across_tracks;

    /* playlist viewer settings */
    bool playlist_viewer_icons; /* display icons on viewer */
    bool playlist_viewer_indices; /* display playlist indices on viewer */
    int playlist_viewer_track_display; /* how to display tracks in viewer */

    /* voice UI settings */
    bool talk_menu; /* enable voice UI */
    int talk_dir; /* voiced directories mode: 0=off 1=number 2=spell */
    bool talk_dir_clip; /* use directory .talk clips */
    int talk_file; /* voice file mode: 0=off, 1=number, 2=spell */
    bool talk_file_clip; /* use file .talk clips */
    bool talk_filetype; /* say file type */
    bool talk_battery_level;
    int  talk_mixer_amp; /* Relative volume of voices, MIX_AMP_MPUTE->MIX_AMP_UNITY */

    /* file browser sorting */
    bool sort_case; /* dir sort order: 0=case insensitive, 1=sensitive */
    int sort_dir;   /* 0=alpha, 1=date (old first), 2=date (new first) */
    int sort_file;  /* 0=alpha, 1=date, 2=date (new first), 3=type */
    int sort_playlists; /* in playlist catalog 0=alpha, 1=date, 2=date (new first) */
    int interpret_numbers; /* true=strnatcmp, false=strcmp */

    /* power settings */
    int poweroff;   /* idle power off timer */
#if BATTERY_CAPACITY_INC > 0
    int battery_capacity; /* in mAh */
#endif
    int usb_charging;
    /* device settings */

    int  cursor_style; /* style of the selection cursor */
    int  screen_scroll_step;
    int  show_path_in_browser; /* 0=off, 1=current directory, 2=full path */
    bool offset_out_of_view;
    bool disable_mainmenu_scrolling;
    unsigned char icon_file[MAX_FILENAME+1];
    unsigned char viewers_icon_file[MAX_FILENAME+1];
    unsigned char font_file[MAX_FILENAME+1]; /* last font */
    /* Optional bold companion to font_file -- empty ("") means none is
     * configured, in which case anything that wants a bold look just
     * falls back to FONT_UI (font_file) unchanged. Currently only read by
     * apps/gui/album_covers.c, for the album name; not a general theme
     * concept like font_file, just a per-feature opt-in. */
    unsigned char bold_font_file[MAX_FILENAME+1];
    int glyphs_to_cache; /* default font allocation size in glyphs */
    /* Core text viewer (apps/text_viewer). */
    int text_viewer_colour_mode;  /* 0 theme, 1 inverted, 2 black-on-white,
                                     3 white-on-black */
    bool text_viewer_margin;      /* inset the page by a small border */
    int text_viewer_line_spacing; /* extra pixels between lines */
    unsigned char text_viewer_font_file[MAX_FILENAME+1]; /* "" = UI font */
    bool text_viewer_page_number; /* show the page number at the foot */
    /* Synchronised lyrics viewer (apps-ipod/viewers/lyric_viewer). */
    int lyric_colour_mode;        /* as text_viewer_colour_mode */
    unsigned char lyric_font_file[MAX_FILENAME+1]; /* "" = UI font */
    int lyric_line_spacing;       /* extra pixels between rows */
    int lyric_align;              /* 0 left, 1 centre, 2 right */
    int lyric_prev_opacity;       /* per cent, the line above */
    int lyric_next_opacity;       /* per cent, the line below */
    int lyric_anim;               /* 0 off, else the slide in ms */
    bool lyric_highlight;         /* light words as they are sung */
    bool lyric_backlight;         /* hold the backlight on */
    unsigned char kbd_file[MAX_FILENAME+1];  /* last keyboard */
    int  backlight_timeout;  /* backlight off timeout:  -1=never,
                                0=always, or time in seconds */
    bool caption_backlight; /* turn on backlight at end and start of track */
    bool bl_filter_first_keypress;   /* filter first keypress when dark? */
    int backlight_timeout_plugged;
    bool bl_selective_actions; /* backlight disable on some actions */
    int  bl_selective_actions_mask;/* mask of actions that will not enable backlight */
    int backlight_on_button_hold; /* what to do with backlight when hold
                                     switch is on */
    int lcd_sleep_after_backlight_off; /* when to put lcd to sleep after backlight
                                          has turned off:  -1=never, 0=always,
                                          or time in seconds */

#if defined(HAVE_BACKLIGHT_FADING_INT_SETTING)
    int backlight_fade_in;  /* backlight fade in timing: 0..3 */
    int backlight_fade_out; /* backlight fade in timing: 0..7 */
#endif
    int brightness;



    int serial_bitrate; /* 0=auto 1=9600 2=19200 3=38400 4=57600 */
    bool accessory_supply; /* 0=off 1=on, accessory power supply for iPod */
    bool lineout_active;

    bool prevent_skip;


    /* If values are just added to the end, no need to bump plugin API
       version. */
    /* new stuff to be added at the end */

    bool usb_hid;
    int usb_keypad_mode;

#ifdef USB_ENABLE_AUDIO
    int usb_audio;
#endif

    unsigned char ui_vp_config[64]; /* viewport string for the lists */

    struct compressor_settings compressor_settings;

    int sleeptimer_duration; /* In minutes; 0=off */
    bool sleeptimer_on_startup;
    bool keypress_restarts_sleeptimer;

    bool show_shutdown_message; /* toggle whether display lights up and displays message
                                when shutting down */

    /* hotkey and WPS context menu assignments - several hotkey_action values
       (see context_menu.h) packed into one int with the HK_CTX_* macros.
       Item 0 is the hotkey button; for context_wps items 1..4 are the
       configurable rows at the bottom of the WPS context menu. */
    int context_wps;
    int hotkey_tree;

    /* When resuming playback (after a stop), rewind this number of seconds */
    int resume_rewind;





    bool keyclick_hardware; /* hardware piezo keyclick */

    char start_directory[MAX_PATHNAME+1];
    /* Has the root been customized from the .cfg file? false = no, true = loaded from cfg */
    bool root_menu_customized;
    bool shortcuts_replaces_qs;

    int play_frequency; /* core audio output frequency selection */
    int volume_limit; /* maximum volume limit */

    int volume_adjust_mode;
    int volume_adjust_norm_steps;

    int surround_enabled;
    int surround_balance;
    int surround_fx1;
    int surround_fx2;
    bool surround_method2;
    int surround_mix;

    int pbe;
    int pbe_precut;

    int afr_enabled;

    int usb_mode;
    bool clear_settings_on_hold;
    int playback_log; /* 0=off, 1=generic (ROCKBOX_DIR/playback.log),
                         2=Audioscrobbler (/.scrobbler.log) */
};

/* global settings */
extern struct user_settings global_settings;
/* global status */
extern struct system_status global_status;

#endif /* __SETTINGS_H__ */
