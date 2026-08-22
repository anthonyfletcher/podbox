/***************************************************************************
 * Original code from RockBox
 * was: apps/settings_list.c
 * Copyright (C) 2007 Jonathan Gordon
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * The settings table: one entry per setting giving its name, type, range,
 * default, config-file representation and voice clip. The single source of
 * truth for what a setting is.
 *
 * Almost the entire file is one array, settings[], built from the
 * *_SETTING macros defined at the top. Each macro fills a struct
 * settings_list, so reading an entry means reading the macro that made it --
 * the arguments are positional and there are a lot of them.
 *
 * Adding a setting means: a field in struct user_settings (settings.h), an
 * entry here, and a lang id. Nothing stores a setting's index -- lookups
 * scan the table, by cfg_name for config files and by the address of the
 * variable for find_setting() -- so entries can be reordered freely. What
 * order does affect is the order settings are written into config.cfg.
 *
 * Parts, in order:
 *   - the value-wrapper and *_SETTING macros that build each entry
 *   - table data shared by several settings (choice strings, ranges)
 *   - settings[]: the table itself, grouped roughly as the menus present it
 *   - lookup helpers over the table
 ****************************************************************************/

#include "config.h"
#include <stdbool.h>
#include "string-extra.h"
#include "system.h"
#include "storage.h"
#include "lang.h"
#include "speech/talk.h"
#include "lcd.h"
#include "scroll_engine.h"
#include "button.h"
#include "backlight.h"
#include "sound.h"
#include "settings.h"
#include "rbpaths.h"
#include "settings_list.h"
#include "usb.h"
#include "audio.h"
#include "power.h"
#include "powermgmt.h"
#include "kernel.h"
#include "system/volume.h"
#include "system/strutil.h"     /* skip_whitespace() */
#include "audio/playback.h"
#include "widgets/list.h"
#include "rbunicode.h"
#include "audio/peak_meter.h"
#include "screens/settings/eq_settings.h"
#include "iap.h"
#include "skin/statusbar.h"
#include "screens/context_menu.h"
#include "playlist/playlist.h"
#include "screens/browse/browser.h"
#include "database/db_summary.h"   /* db_summary_invalidate */
#include "root_menu.h"             /* root_menu_set_audiobooks_row */

#include "audio/voice_thread.h"


#define UNUSED {.RESERVED=NULL}
#define INT(a) {.int_ = a}
#define UINT(a) {.uint_ = a}
#define BOOL(a) {.bool_ = a}
#define CHARPTR(a) {.charptr = a}
#define UCHARPTR(a) {.ucharptr = a}
#define FUNCTYPE(a) {.func = a}
#define NODEFAULT INT(0)


/* in all the following macros the args are:
    - flags: bitwise | or the F_ bits in settings_list.h
    - var: pointer to the variable being changed (usually in global_settings)
    - lang_id: LANG_* id to display in menus and setting screens for the setting
    - default: the default value for the variable, set if settings are reset
    - name: the name of the setting in config files
    - cfg_vals: comma separated list of legal values to write to cfg files.
                The values correspond to the values 0,1,2,etc. of the setting.
                NULL if just the number itself should be written to the file.
                No spaces between the values and the commas!
    - cb: the callback used by the setting screen.
*/

/* Use for int settings which use the set_sound() function to set them */
#define SOUND_SETTING(flags,var,lang_id,name,setting)                      \
            {flags|F_T_INT|F_T_SOUND|F_SOUNDSETTING|F_ALLOW_ARBITRARY_VALS, &global_settings.var, \
                lang_id, NODEFAULT,name,                              \
                {.sound_setting=(struct sound_setting[]){{setting}}} }

/* Use for bool variables which don't use LANG_SET_BOOL_YES and LANG_SET_BOOL_NO
      or dont save as "off" or "on" in the cfg.
   cfgvals are comma separated values (without spaces after the comma!) to write
      for 'false' and 'true' (in this order)
   yes_id is the lang_id for the 'yes' (or 'on') option in the menu
   no_id is the lang_id for the 'no' (or 'off') option in the menu
 */
#define BOOL_SETTING(flags,var,lang_id,default,name,cfgvals,yes_id,no_id,cb)\
            {flags|F_BOOL_SETTING, &global_settings.var,                    \
                lang_id, BOOL(default),name,                                \
                {.bool_setting=(struct bool_setting[]){{cb,yes_id,no_id,cfgvals}}} }

/* bool setting which does use LANG_YES and _NO and save as "off,on" */
#define OFFON_SETTING(flags,var,lang_id,default,name,cb)                    \
            BOOL_SETTING(flags,var,lang_id,default,name,off_on,             \
                LANG_SET_BOOL_YES,LANG_SET_BOOL_NO,cb)

/*system_status int variable which is saved to resume.cfg */
#define SYSTEM_STATUS(flags,var,default,name)                \
            {flags|F_RESUMESETTING|F_T_INT, &global_status.var,-1, \
             INT(default), name, UNUSED}
/* system_status settings items will be saved to resume.cfg
   Use for int which use the set_sound() function to set them
   These items WILL be included in the users exported settings files
 */
#define SYSTEM_STATUS_SOUND(flags,var,lang_id,name,setting)                 \
            {flags|F_T_INT|F_T_SOUND|F_SOUNDSETTING|F_ALLOW_ARBITRARY_VALS| \
             F_RESUMESETTING, &global_status.var, lang_id, NODEFAULT,name,  \
                {.sound_setting=(struct sound_setting[]){{setting}}} }

/* setting which stores as a filename (or another string) in the .cfgvals
    The string must be a char array (which all of our string settings are),
    not just a char pointer.
    prefix: The absolute path to not save in the variable, ex /.rockbox/wps_file
    suffix: The file extention (usually...) e.g .wps_file
    If the prefix is set (not NULL), then the suffix must be set as well.
 */
#define TEXT_SETTING(flags,var,name,default,prefix,suffix)      \
            {flags|F_T_UCHARPTR, &global_settings.var,-1,           \
                CHARPTR(default),name,                              \
                {.filename_setting=                                 \
                    (struct filename_setting[]){                    \
                        {prefix,suffix,sizeof(global_settings.var)}}} }

#define DIRECTORY_SETTING(flags,var,lang_id,name,default) \
    {flags|F_DIRNAME|F_T_UCHARPTR, &global_settings.var, lang_id, \
     CHARPTR(default), name, \
     {.filename_setting=(struct filename_setting[]){ \
         {NULL, NULL, sizeof(global_settings.var)}}}}

/*  Used for settings which use the set_option() setting screen.
    The ... arg is a list of pointers to strings to display in the setting
    screen. These can either be literal strings, or ID2P(LANG_*) */
#define CHOICE_SETTING(flags,var,lang_id,default,name,cfg_vals,cb,count,...)   \
            {flags|F_CHOICE_SETTING|F_T_INT,  &global_settings.var, lang_id,   \
                INT(default), name,                                  \
                {.choice_setting = (struct choice_setting[]){                  \
                    {cb, count, cfg_vals, {.desc = (const unsigned char*[])    \
                        {__VA_ARGS__}}}}}}

/* Similar to above, except the strings to display are taken from cfg_vals,
   the ... arg is a list of ID's to talk for the strings, can use TALK_ID()'s */
#define STRINGCHOICE_SETTING(flags,var,lang_id,default,name,cfg_vals,          \
                                                                cb,count,...)  \
            {flags|F_CHOICE_SETTING|F_T_INT|F_CHOICETALKS,                     \
                &global_settings.var, lang_id,                                 \
                INT(default), name,                                            \
                {.choice_setting = (struct choice_setting[]){                  \
                    {cb, count, cfg_vals, {.talks = (const int[]){__VA_ARGS__}}}}}}

/*  for settings which use the set_int() setting screen.
    unit is the UNIT_ define to display/talk.
    the first one saves a string to the config file,
    the second one saves the variable value to the config file */
#define INT_SETTING(flags, var, lang_id, default, name,                 \
                    unit, min, max, step, formatter, get_talk_id, cb)   \
            {flags|F_INT_SETTING|F_T_INT, &global_settings.var,         \
                lang_id, INT(default), name,                            \
                 {.int_setting = (struct int_setting[]){                \
                    {cb, unit, step, min, max, formatter, get_talk_id}}}}
#define INT_SETTING_NOWRAP(flags, var, lang_id, default, name,             \
                    unit, min, max, step, formatter, get_talk_id, cb)      \
            {flags|F_INT_SETTING|F_T_INT|F_NO_WRAP, &global_settings.var,  \
                lang_id, INT(default), name,                               \
                 {.int_setting = (struct int_setting[]){                   \
                    {cb, unit, step, min, max, formatter, get_talk_id}}}}
#define TABLE_SETTING(flags, var, lang_id, default, name, cfg_vals, \
                      unit, formatter, get_talk_id, cb, count, ...) \
            {flags|F_TABLE_SETTING|F_T_INT, &global_settings.var,   \
                lang_id, INT(default), name,                        \
                {.table_setting = (struct table_setting[]) {        \
                    {cb, formatter, get_talk_id, unit, count,       \
                    cfg_vals, (const int[]){__VA_ARGS__}}}}}
#define TABLE_SETTING_LIST(flags, var, lang_id, default, name, cfg_vals, \
                      unit, formatter, get_talk_id, cb, count, list) \
            {flags|F_TABLE_SETTING|F_T_INT, &global_settings.var,   \
                lang_id, INT(default), name,                        \
                {.table_setting = (struct table_setting[]) {        \
                    {cb, formatter, get_talk_id, unit, count, cfg_vals, list}}}}
#define CUSTOM_SETTING(flags, var, lang_id, default, name,              \
                       load_from_cfg, write_to_cfg,                     \
                       is_change, set_default)                          \
            {flags|F_CUSTOM_SETTING|F_T_CUSTOM|F_BANFROMQS,             \
                &global_settings.var, lang_id,                          \
                {.custom = (void*)default}, name,                       \
            {.custom_setting = (struct custom_setting[]){               \
        {load_from_cfg, write_to_cfg, is_change, set_default}}}}

#define VIEWPORT_SETTING(var,name)      \
        TEXT_SETTING(F_THEMESETTING|F_NEEDAPPLY,var,name,"-", NULL, NULL)

/* some sets of values which are used more than once, to save memory */
static const char off[] = "off";
static const char off_on[] = "off,on";
static const char off_on_ask[] = "off,on,ask";
static const char off_number_spell[] = "off,number,spell";
static const int timeout_sec_common[] = {-1,0,1,2,3,4,5,6,7,8,9,10,15,20,25,30,
                                        45,60,90,120,180,240,300,600,900,1200,
                                        1500,1800,2700,3600,4500,5400,6300,7200};
#if defined(HAVE_BACKLIGHT_FADING_INT_SETTING)
static const int backlight_fade[] = {0,100,200,300,500,1000,2000,3000,5000,10000};
#endif

static const char graphic_numeric[] = "graphic,numeric";

/* Default theme settings: none of them.
 *
 * These are what a theme load resets to before reading the theme, so naming a
 * theme's files here would mean a theme that mentions no skin of its own came
 * up wearing the shipped theme's -- the same inherited-look fault the reset
 * exists to prevent, just from the binary instead of from whatever was loaded
 * last. The shipped look is named by themes/default-config.cfg, which is the
 * first-boot config, and by the theme's own .cfg.
 *
 * "-" is the established "none" value and is what upstream's failsafe theme
 * uses for its font. Reaching these at all means no config.cfg has been
 * applied -- a fresh flash, or a Reset Settings -- and the honest answer there
 * is a bare screen and the built-in font, not somebody's theme. */
#define DEFAULT_WPSNAME  "-"
#define DEFAULT_SBSNAME  "-"
#define DEFAULT_FMS_NAME "cabbiev2"

  #define DEFAULT_FONT_HEIGHT 15
  #define DEFAULT_FONTNAME "-"
#define DEFAULT_GLYPHS 250
#define MIN_GLYPHS 50
#define MAX_GLYPHS 65540

#ifndef DEFAULT_FONTNAME
/* ugly expansion needed */
#define _EXPAND2(x) #x
#define _EXPAND(x) _EXPAND2(x)
#define DEFAULT_FONTNAME _EXPAND(DEFAULT_FONT_HEIGHT) "-Adobe-Helvetica"
#endif

    #define DEFAULT_ICONSET "tango_icons.16x16"
    #define DEFAULT_VIEWERS_ICONSET "tango_icons_viewers.16x16"


/* What a colours reset falls back to: a fixed set, not the shipped theme's --
 * those are named in themes/default-config.cfg. */
#define DEFAULT_THEME_FOREGROUND LCD_RGBPACK(0xe1, 0xf0, 0xee)
#define DEFAULT_THEME_BACKGROUND LCD_RGBPACK(0x00, 0x0c, 0x21)
#define DEFAULT_THEME_SELECTOR_START LCD_RGBPACK(0xff, 0xeb, 0x9c)
#define DEFAULT_THEME_SELECTOR_END LCD_RGBPACK(0xb5, 0x8e, 0x00)
#define DEFAULT_THEME_SELECTOR_TEXT LCD_RGBPACK(0x00, 0x00, 0x00)
#define DEFAULT_THEME_SEPARATOR  LCD_RGBPACK(0x80, 0x80, 0x80)

/* No backdrop, so that a theme which says nothing about one gets the plain
 * background colour rather than whatever image the previous theme left behind.
 * Upstream defaults to cabbiev2.bmp; that file is not in this build's zip, so
 * naming it here would only ever resolve to a failed load anyway. */
#define DEFAULT_BACKDROP    "-"


#define DEFAULT_TAGCACHE_SCAN_PATHS "/"

#ifdef SIMULATOR
/* Never, in a sim. There is no battery to save and a blanked window looks
 * exactly like a crash -- which costs an afternoon the first time. */
#define DEFAULT_BACKLIGHT_TIMEOUT 0
#else
#define DEFAULT_BACKLIGHT_TIMEOUT 15
#endif

# if !defined(TARGET_USB_CHARGING_DEFAULT)
#  define TARGET_USB_CHARGING_DEFAULT USB_CHARGING_ENABLE
# endif


/*
 * Total buffer size due to this setting = max files in dir * 52 bytes
 * Keep this in mind when selecting the maximum - if the maximum is too
 * high it's possible rockbox could hit OOM and become unusable until
 * the config file is deleted manually.
 *
 * Note the FAT32 limit is 65534 files per directory, but this limit
 * also applies to the database browser so it makes sense to support
 * larger maximums.
 */
# define MAX_FILES_IN_DIR_DEFAULT   5000
# define MAX_FILES_IN_DIR_MAX       100000
# define MAX_FILES_IN_DIR_STEP      1000

# define SCROLLBAR_DEFAULT SCROLLBAR_LEFT


static const char* list_pad_formatter(char *buffer, size_t buffer_size,
                                    int val, const char *unit)
{
    switch (val)
    {
        case -1: return str(LANG_AUTO);
        case  0: return str(LANG_OFF);
        default: break;
    }
    snprintf(buffer, buffer_size, "%d %s", val, unit);
    return buffer;
}

static int32_t list_pad_getlang(int value, int unit)
{
    switch (value)
    {
        case -1: return LANG_AUTO;
        case  0: return LANG_OFF;
        default: return TALK_ID(value, unit);
    }
}

static const char* formatter_time_unit_0_is_off(char *buffer, size_t buffer_size,
                                    int val, const char *unit)
{
    (void) buffer_size;
    (void) unit;
    if (val == 0)
        return str(LANG_OFF);
    return buffer;
}

static int32_t getlang_time_unit_0_is_off(int value, int unit)
{
    if (value == 0)
        return LANG_OFF;
    else
        return talk_time_intervals(value, unit, false);
}

static const char* formatter_time_unit_0_is_always(char *buffer, size_t buffer_size,
                                    int val, const char *unit)
{
    (void) buffer_size;
    (void) unit;
    if (val == -1)
        return str(LANG_NEVER);
    else if (val == 0)
        return str(LANG_ALWAYS);
    return buffer;
}

static int32_t getlang_time_unit_0_is_always(int value, int unit)
{
    if (value == -1)
        return LANG_NEVER;
    else if (value == 0)
        return LANG_ALWAYS;
    else
        return talk_time_intervals(value, unit, false);
}

static const char* formatter_time_unit_0_is_skip_track(char *buffer,
                                size_t buffer_size, int val, const char *unit)
{
    (void)unit;
    (void)buffer_size;
    if (val == -1)
        return str(LANG_SKIP_OUTRO);
    else if (val == 0)
        return str(LANG_SKIP_TRACK);
    return buffer;
}

static int32_t getlang_time_unit_0_is_skip_track(int value, int unit)
{
    (void)unit;
    if (value == -1)
        return LANG_SKIP_OUTRO;
    else if (value == 0)
        return LANG_SKIP_TRACK;
    else
        return talk_time_intervals(value, unit, false);
}

static const char* formatter_time_unit_0_is_eternal(char *buffer,
                                size_t buffer_size, int val, const char *unit)
{
    (void) buffer_size;
    (void) unit;
    if (val == 0)
        return str(LANG_PM_ETERNAL);
    return buffer;
}
static int32_t getlang_time_unit_0_is_eternal(int value, int unit)
{
    if (value == 0)
        return LANG_PM_ETERNAL;
    else
        return talk_time_intervals(value, unit, false);
}


static const char* formatter_unit_0_is_off(char *buffer, size_t buffer_size,
                                    int val, const char *unit)
{
    if (val == 0)
        return str(LANG_OFF);
    else
        snprintf(buffer, buffer_size, "%d %s", val, unit);
    return buffer;
}

static int32_t getlang_unit_0_is_off(int value, int unit)
{
    if (value == 0)
        return LANG_OFF;
    else
        return TALK_ID(value,unit);
}

static void crossfeed_cross_set(int val)
{
   (void)val;
   dsp_set_crossfeed_cross_params(global_settings.crossfeed_cross_gain,
                                  global_settings.crossfeed_hf_attenuation,
                                  global_settings.crossfeed_hf_cutoff);
}

static void surround_set_factor(int val)
{
    (void)val;
    dsp_surround_set_cutoff(global_settings.surround_fx1, global_settings.surround_fx2);
}

static void compressor_set(int val)
{
    (void)val;
    dsp_set_compressor(&global_settings.compressor_settings);
}

static const char* db_format(char* buffer, size_t buffer_size, int value,
                      const char* unit)
{
    int v = abs(value);

    snprintf(buffer, buffer_size, "%s%d.%d %s", value < 0 ? "-" : "",
             v / 10, v % 10, unit);
    return buffer;
}

static int32_t get_dec_talkid(int value, int unit)
{
    return TALK_ID_DECIMAL(value, 1, unit);
}

static int32_t get_precut_talkid(int value, int unit)
{
    return TALK_ID_DECIMAL(-value, 1, unit);
}

struct eq_band_setting eq_defaults[EQ_NUM_BANDS] = {
    { EQ_FILTER_LOW_SHELF, 32, 7, 0 },
    { EQ_FILTER_PEAK, 64, 10, 0 },
    { EQ_FILTER_PEAK, 125, 10, 0 },
    { EQ_FILTER_PEAK, 250, 10, 0 },
    { EQ_FILTER_PEAK, 500, 10, 0 },
    { EQ_FILTER_PEAK, 1000, 10, 0 },
    { EQ_FILTER_PEAK, 2000, 10, 0 },
    { EQ_FILTER_PEAK, 4000, 10, 0 },
    { EQ_FILTER_PEAK, 8000, 10, 0 },
    { EQ_FILTER_HIGH_SHELF, 16000, 7, 0 },
};

/* Item 0 is the hotkey button; 1..4 are the configurable rows at the bottom
   of the WPS context menu. */
static const int wps_context_menu_default =
    HK_CTX_SET(0, HOTKEY_LYRICS) /* hotkey */
  | HK_CTX_SET(1, HOTKEY_SHOW_TRACK_INFO)
  | HK_CTX_SET(2, HOTKEY_DELETE)
  | HK_CTX_SET(3, HOTKEY_SHOW_IN_FILES)
  | HK_CTX_SET(4, HOTKEY_ALBUMART);

static const int tree_hotkey_default = HOTKEY_OFF;

/* "cutoff, q, gain" in the old format, "cutoff, q, gain, TYPE" in the new one.
 * A config written before the type existed carries no fourth field, and its
 * type comes from the setting name instead -- see the EQ_BAND_OLD entries
 * below. */
static void eq_load_from_cfg(void *setting, char *value, bool has_type)
{
    struct eq_band_setting *eq = setting;
    char *val_end, *end;

    val_end = value + strlen(value);

    /* cutoff/center */
    end = strchr(value, ',');
    if (!end) return;
    *end = '\0';
    eq->cutoff = atoi(value);

    /* q */
    value = end + 1;
    if (value > val_end) return;
    end = strchr(value, ',');
    if (!end) return;
    *end = '\0';
    eq->q = atoi(value);

    /* gain */
    value = end + 1;
    if (value > val_end) return;
    if (has_type)
    {
        end = strchr(value, ',');
        if (!end) return;
    }
    eq->gain = atoi(value);

    if (!has_type) return;

    /* type */
    value = end + 1;
    if (value > val_end) return;
    value = skip_whitespace(value);
    if (strcasecmp(value, "LOW_SHELF") == 0)
        eq->type = EQ_FILTER_LOW_SHELF;
    else if (strcasecmp(value, "PEAK") == 0)
        eq->type = EQ_FILTER_PEAK;
    else if (strcasecmp(value, "HIGH_SHELF") == 0)
        eq->type = EQ_FILTER_HIGH_SHELF;
}

static void eq_load_from_cfg_old_low_shelf(void *setting, char *value)
{
    eq_load_from_cfg(setting, value, false);
    ((struct eq_band_setting *)setting)->type = EQ_FILTER_LOW_SHELF;
}

static void eq_load_from_cfg_old_peak(void *setting, char *value)
{
    eq_load_from_cfg(setting, value, false);
    ((struct eq_band_setting *)setting)->type = EQ_FILTER_PEAK;
}

static void eq_load_from_cfg_old_high_shelf(void *setting, char *value)
{
    eq_load_from_cfg(setting, value, false);
    ((struct eq_band_setting *)setting)->type = EQ_FILTER_HIGH_SHELF;
}

static void eq_load_from_cfg_new(void *setting, char *value)
{
    eq_load_from_cfg(setting, value, true);
}

static char* eq_write_to_cfg(void *setting, char *buf, int buf_len)
{
    struct eq_band_setting *eq = setting;
    const char *type;

    switch (eq->type)
    {
    case EQ_FILTER_LOW_SHELF:
        type = "LOW_SHELF";
        break;
    case EQ_FILTER_HIGH_SHELF:
        type = "HIGH_SHELF";
        break;
    case EQ_FILTER_PEAK:
    default:
        type = "PEAK";
        break;
    }

    snprintf(buf, buf_len, "%d, %d, %d, %s", eq->cutoff, eq->q, eq->gain, type);
    return buf;
}

static bool eq_is_changed(void *setting, void *defaultval)
{
    struct eq_band_setting *eq = setting;

    return memcmp(eq, defaultval, sizeof(struct eq_band_setting));
}

static void eq_set_default(void* setting, void* defaultval)
{
    memcpy(setting, defaultval, sizeof(struct eq_band_setting));
}

static const char* formatter_freq_unit_0_is_auto(char *buffer, size_t buffer_size,
                                    int value, const char *unit)
{
    if (value == 0)
        return str(LANG_AUTO);
    else
        return db_format(buffer, buffer_size, value / 100, unit);
}

static int32_t getlang_freq_unit_0_is_auto(int value, int unit)
{
    if (value == 0) {
        return LANG_AUTO;
    } else {
        talk_value_decimal(value, unit, 3, false);
        return -1;
    }
}

static void playback_frequency_callback(int sample_rate_hz)
{
    audio_set_playback_frequency(sample_rate_hz);
}

static void albumart_callback(int mode)
{
    set_albumart_mode(mode);
}

/* Two things follow from this one, which is the point of it being one setting.
 *
 * The album and artist index is built once and saved, so it holds whatever the
 * setting said at the time. Nothing about the database has changed, and the
 * index's own staleness checks only watch that -- so without the invalidate
 * the carousel and Random album keep serving the old list until something else
 * forces a rebuild. */
static void segregate_audiobooks_callback(bool segregate)
{
    root_menu_set_audiobooks_row(segregate);
    db_summary_invalidate();
}

/* The buffered bitmap is chosen per track, so nothing changes on screen until
 * the artwork is fetched again. */
static void wps_art_source_callback(int mode)
{
    (void)mode;
    playback_update_aa_dims();
}

/* perform shuffle/unshuffle of the current playlist based on the boolean provided */
static void shuffle_playlist_callback(bool shuffle)
{
    struct playlist_info *playlist = playlist_get_current();
    if (playlist->started)
    {
        if ((audio_status() & AUDIO_STATUS_PLAY) == AUDIO_STATUS_PLAY)
        {
            replaygain_update();
            if (shuffle)
            {
                playlist_randomise(playlist, current_tick, true);
            }
            else
            {
                playlist_sort(playlist, true);
            }
        }
    }
}

static void repeat_mode_callback(int repeat)
{
    if ((audio_status() & AUDIO_STATUS_PLAY) == AUDIO_STATUS_PLAY)
    {
        audio_flush_and_reload_tracks();
    }
    (void)repeat;
}

static void treesort_callback(int value)
{
    (void) value;
    reload_directory();
}

static void qs_load_from_cfg(void *var, char *value)
{
    const struct settings_list **item = var;

    if (*value == '-')
        *item = NULL;
    else
        *item = find_setting_by_cfgname(value);
}

static char* qs_write_to_cfg(void *var, char *buf, int buf_len)
{
    const struct settings_list *setting = *(const struct settings_list **)var;

    strmemccpy(buf, setting ? setting->cfg_name : "-", buf_len);
    return buf;
}

static bool qs_is_changed(void* var, void* defaultval)
{
    const struct settings_list *defaultsetting = find_setting(defaultval);

    return var != defaultsetting;
}

static void qs_set_default(void* var, void* defaultval)
{
    *(const struct settings_list **)var = find_setting(defaultval);
}

/* volume limiter */
static void volume_limit_load_from_cfg(void* var, char*value)
{
    *(int*)var = atoi(value);
}
static char* volume_limit_write_to_cfg(void* setting, char*buf, int buf_len)
{
    int current = *(int*)setting;
    itoa_buf(buf, buf_len, current);
    return buf;
}
static bool volume_limit_is_changed(void* setting, void* defaultval)
{
    (void)defaultval;
    int current = *(int*)setting;
    return (current != sound_max(SOUND_VOLUME));
}
static void volume_limit_set_default(void* setting, void* defaultval)
{
    (void)defaultval;
    *(int*)setting = sound_max(SOUND_VOLUME);
}



const struct settings_list settings[] = {
/* system_status settings .resume.cfg */
    SYSTEM_STATUS_SOUND(F_NO_WRAP, volume, LANG_VOLUME, "volume", SOUND_VOLUME),
    SYSTEM_STATUS(0, resume_index,   -1,     "IDX"),
    SYSTEM_STATUS(0, resume_crc32,   -1,     "CRC"),
    SYSTEM_STATUS(0, resume_elapsed, -1,     "ELA"),
    SYSTEM_STATUS(0, resume_offset,  -1,     "OFF"),
    SYSTEM_STATUS(0, resume_modified, false, "PLM"),
    SYSTEM_STATUS(0, resume_art_hash, 0,     "AAH"),
    SYSTEM_STATUS(0, runtime,         0,     "CRT"),
    SYSTEM_STATUS(0, topruntime,      0,     "TRT"),
    SYSTEM_STATUS(0, last_screen,    -1,     "PVS"),
/* sound settings */
    CUSTOM_SETTING(F_NO_WRAP, volume_limit, LANG_VOLUME_LIMIT,
                  NULL, "volume limit",
                  volume_limit_load_from_cfg, volume_limit_write_to_cfg,
                  volume_limit_is_changed, volume_limit_set_default),
    SOUND_SETTING(0, balance, LANG_BALANCE, "balance", SOUND_BALANCE),
/* Tone controls */
    SOUND_SETTING(F_NO_WRAP,bass, LANG_BASS, "bass", SOUND_BASS),
    SOUND_SETTING(F_NO_WRAP,treble, LANG_TREBLE, "treble", SOUND_TREBLE),
/* Hardware EQ tone controls */
/* 3-d enhancement effect */
    CHOICE_SETTING(0, channel_config, LANG_CHANNEL_CONFIGURATION,
                   0,"channels",
                   "stereo,mono,custom,mono left,mono right,karaoke,swap",
                   sound_set_channels, 7,
                   ID2P(LANG_CHANNEL_STEREO), ID2P(LANG_CHANNEL_MONO),
                   ID2P(LANG_CHANNEL_CUSTOM), ID2P(LANG_CHANNEL_LEFT),
                   ID2P(LANG_CHANNEL_RIGHT), ID2P(LANG_CHANNEL_KARAOKE),
                   ID2P(LANG_CHANNEL_SWAP)),
    SOUND_SETTING(0, stereo_width, LANG_STEREO_WIDTH,
                  "stereo_width", SOUND_STEREO_WIDTH),



    /* playback */
    OFFON_SETTING(F_CB_ON_SELECT_ONLY|F_CB_ONLY_IF_CHANGED, playlist_shuffle,
                  LANG_SHUFFLE, false, "shuffle", shuffle_playlist_callback),

    CHOICE_SETTING(F_CB_ON_SELECT_ONLY|F_CB_ONLY_IF_CHANGED, repeat_mode,
                   LANG_REPEAT, REPEAT_OFF, "repeat", "off,all,one,shuffle"
                   ",ab"
                   , repeat_mode_callback,
                   5,
                   ID2P(LANG_OFF), ID2P(LANG_ALL), ID2P(LANG_REPEAT_ONE),
                   ID2P(LANG_SHUFFLE)
                   ,ID2P(LANG_REPEAT_AB)
                  ), /* CHOICE_SETTING( repeat_mode ) */
     TABLE_SETTING(F_SOUNDSETTING|F_CB_ON_SELECT_ONLY|F_CB_ONLY_IF_CHANGED,
                  play_frequency, LANG_FREQUENCY, 0, "playback frequency", "auto",
                  UNIT_KHZ, formatter_freq_unit_0_is_auto,
                  getlang_freq_unit_0_is_auto,
                  playback_frequency_callback,
                  3,0,SAMPR_44,SAMPR_48),

    CHOICE_SETTING(F_CB_ON_SELECT_ONLY|F_CB_ONLY_IF_CHANGED, album_art,
                   LANG_ALBUM_ART, AA_PREFER_CACHE, "album art",
                   "off,prefer embedded,prefer image file,prefer cache",
                   albumart_callback, 4,
                   ID2P(LANG_OFF), ID2P(LANG_PREFER_EMBEDDED),
                   ID2P(LANG_PREFER_IMAGE_FILE), ID2P(LANG_PREFER_CACHE)),

    /* LCD */
    TABLE_SETTING_LIST(F_TIME_SETTING | F_ALLOW_ARBITRARY_VALS,
                    backlight_timeout, LANG_BACKLIGHT,
                    DEFAULT_BACKLIGHT_TIMEOUT, "backlight timeout",
                    off_on, UNIT_SEC, formatter_time_unit_0_is_always,
                    getlang_time_unit_0_is_always, backlight_set_timeout,
                    23, timeout_sec_common),
    TABLE_SETTING_LIST(F_TIME_SETTING | F_ALLOW_ARBITRARY_VALS,
                    backlight_timeout_plugged, LANG_BACKLIGHT_ON_WHEN_CHARGING,
                    DEFAULT_BACKLIGHT_TIMEOUT, "backlight timeout plugged",
                    off_on, UNIT_SEC, formatter_time_unit_0_is_always,
                    getlang_time_unit_0_is_always, backlight_set_timeout_plugged,
                    23, timeout_sec_common),
    /* display */
     CHOICE_SETTING(F_TEMPVAR|F_THEMESETTING, cursor_style, LANG_INVERT_CURSOR,
                    3, "selector type",
                    "pointer,bar (inverse),bar (color),bar (gradient)", NULL, 4,
                    ID2P(LANG_INVERT_CURSOR_POINTER),
                    ID2P(LANG_INVERT_CURSOR_BAR),
                    ID2P(LANG_INVERT_CURSOR_COLOR),
                    ID2P(LANG_INVERT_CURSOR_GRADIENT)),
    CHOICE_SETTING(F_THEMESETTING|F_TEMPVAR|F_NEEDAPPLY, statusbar,
                  LANG_STATUS_BAR, STATUSBAR_TOP, "statusbar","off,top,bottom",
                  NULL, 3, ID2P(LANG_OFF), ID2P(LANG_STATUSBAR_TOP),
                  ID2P(LANG_STATUSBAR_BOTTOM)),
    CHOICE_SETTING(F_THEMESETTING|F_TEMPVAR, scrollbar,
                  LANG_SCROLL_BAR, SCROLLBAR_DEFAULT, "scrollbar","off,left,right",
                  NULL, 3, ID2P(LANG_OFF), ID2P(LANG_LEFT), ID2P(LANG_RIGHT)),
    INT_SETTING(F_THEMESETTING, scrollbar_width, LANG_SCROLLBAR_WIDTH, 6,
                "scrollbar width",UNIT_INT, 3, MAX(LCD_WIDTH/10,25), 1,
                NULL, NULL, NULL),
    TABLE_SETTING(F_THEMESETTING|F_ALLOW_ARBITRARY_VALS, list_separator_height, LANG_LIST_SEPARATOR,
                  0, "list separator height", "auto,off", UNIT_PIXEL,
                  list_pad_formatter, list_pad_getlang, NULL, 15,
                  -1,0,1,2,3,4,5,7,9,11,13,16,20,25,30),
    {F_T_INT|F_RGB|F_THEMESETTING ,&global_settings.list_separator_color,-1,
        INT(DEFAULT_THEME_SEPARATOR),"list separator color",UNUSED},
    CHOICE_SETTING(F_THEMESETTING, volume_type, LANG_VOLUME_DISPLAY, 0,
                   "volume display", graphic_numeric, NULL, 2,
                   ID2P(LANG_DISPLAY_GRAPHIC),
                   ID2P(LANG_DISPLAY_NUMERIC)),
    CHOICE_SETTING(F_THEMESETTING, battery_display, LANG_BATTERY_DISPLAY, 0,
                   "battery display", graphic_numeric, NULL, 2,
                   ID2P(LANG_DISPLAY_GRAPHIC), ID2P(LANG_DISPLAY_NUMERIC)),
    CHOICE_SETTING(0, timeformat, LANG_TIMEFORMAT, 1,
        "time format", "24hour,12hour", NULL, 2,
        ID2P(LANG_24_HOUR_CLOCK), ID2P(LANG_12_HOUR_CLOCK)),
    OFFON_SETTING(F_THEMESETTING,show_icons, LANG_SHOW_ICONS ,true,"show icons", NULL),
    OFFON_SETTING(0,show_debug_menu, LANG_SHOW_DEBUG_MENU, false,
                  "show debug menu", NULL),
    /* How much of the settings tree is shown. F_BANFROMQS because the
       quickscreen would be a strange place to change the shape of the settings
       menu from. */
    CHOICE_SETTING(F_BANFROMQS, settings_mode, LANG_SETTINGS_MODE,
                   SETTINGS_MODE_STANDARD, "settings mode",
                   "standard,everything",
                   NULL, 2, ID2P(LANG_SETTINGS_MODE_STANDARD),
                   ID2P(LANG_SETTINGS_MODE_EVERYTHING)),
    /* system */
    INT_SETTING(F_TIME_SETTING, poweroff, LANG_POWEROFF_IDLE, 10,
                "idle poweroff", UNIT_MIN, 0,60,1,
                formatter_time_unit_0_is_off, getlang_time_unit_0_is_off,
                set_poweroff_timeout),
    INT_SETTING(F_BANFROMQS, max_files_in_playlist,
                LANG_MAX_FILES_IN_PLAYLIST,
#if CONFIG_CPU == PP5022
                  /** Slow CPU benefits greatly from building smaller playlists
                  On the iPod Mini 2nd gen, creating a playlist of 2000 entries takes around 10 seconds */
                  2000,
#else
                  10000,
#endif
                  "max files in playlist", UNIT_INT, 1000, 32000, 1000,
                  NULL, NULL, NULL),
    INT_SETTING(F_BANFROMQS, max_files_in_dir, LANG_MAX_FILES_IN_DIR,
                MAX_FILES_IN_DIR_DEFAULT, "max files in dir", UNIT_INT,
                MAX_FILES_IN_DIR_STEP /* min */, MAX_FILES_IN_DIR_MAX,
                MAX_FILES_IN_DIR_STEP,
                NULL, NULL, NULL),
    CHOICE_SETTING(0, volume_adjust_mode, LANG_VOLUME_ADJUST_MODE,
                   VOLUME_ADJUST_DIRECT, "volume adjustment mode",
                   "direct,perceptual", NULL, 2,
                   ID2P(LANG_DIRECT), ID2P(LANG_PERCEPTUAL)),
    INT_SETTING_NOWRAP(0, volume_adjust_norm_steps, LANG_VOLUME_ADJUST_NORM_STEPS,
                       50, "perceptual volume step count", UNIT_INT,
                       MIN_NORM_VOLUME_STEPS, MAX_NORM_VOLUME_STEPS, 5,
                       NULL, NULL, NULL),
/* use this setting for user code even if there's no exchangable battery
 * support enabled */
#if BATTERY_CAPACITY_INC > 0
#if defined(IPOD_VIDEO)
    /* its easier to leave this one un-macro()ed for the time being */
    { F_T_INT|F_DEF_ISFUNC|F_INT_SETTING, &global_settings.battery_capacity,
        LANG_BATTERY_CAPACITY, FUNCTYPE(battery_default_capacity),
        "battery capacity" , {
            .int_setting = (struct int_setting[]) {
                { .option_callback = set_battery_capacity,
                  .unit = UNIT_MAH, .step = BATTERY_CAPACITY_INC,
                  .min = BATTERY_CAPACITY_MIN, .max = BATTERY_CAPACITY_MAX,
                  .formatter = NULL, .get_talk_id = NULL }}}},
#else /* IPOD_VIDEO */
    INT_SETTING(0, battery_capacity, LANG_BATTERY_CAPACITY,
                BATTERY_CAPACITY_DEFAULT, "battery capacity", UNIT_MAH,
                BATTERY_CAPACITY_MIN, BATTERY_CAPACITY_MAX,
                BATTERY_CAPACITY_INC, NULL, NULL, set_battery_capacity),
#endif /* IPOD_VIDEO */
#endif
    OFFON_SETTING(0, car_adapter_mode,
                  LANG_CAR_ADAPTER_MODE, false, "car adapter mode", NULL),
    INT_SETTING_NOWRAP(0, car_adapter_mode_delay, LANG_CAR_ADAPTER_MODE_DELAY,
                5, "delay before resume", UNIT_SEC, 5, 30, 5,
                NULL, NULL, NULL),
    CHOICE_SETTING(0, serial_bitrate, LANG_SERIAL_BITRATE, 0, "serial bitrate",
                   "auto,9600,19200,38400,57600", iap_bitrate_set, 5, ID2P(LANG_SERIAL_BITRATE_AUTO),
           ID2P(LANG_SERIAL_BITRATE_9600),ID2P(LANG_SERIAL_BITRATE_19200),
           ID2P(LANG_SERIAL_BITRATE_38400),ID2P(LANG_SERIAL_BITRATE_57600)),
    OFFON_SETTING(0, accessory_supply, LANG_ACCESSORY_SUPPLY,
                  true, "accessory power supply", accessory_supply_set),
    OFFON_SETTING(0, lineout_active, LANG_LINEOUT,
                  true, "lineout", lineout_set),


    OFFON_SETTING(0, bl_filter_first_keypress,
                  LANG_BACKLIGHT_FILTER_FIRST_KEYPRESS, true,
                  "backlight filters first keypress", NULL),

/** End of old RTC config block **/


    OFFON_SETTING(0, caption_backlight, LANG_CAPTION_BACKLIGHT,
                  false, "caption backlight", NULL),

    OFFON_SETTING(F_BANFROMQS, bl_selective_actions,
                  LANG_ACTION_ENABLED, false,
                  "No Backlight On Selected Actions", NULL),

    INT_SETTING(F_BANFROMQS, bl_selective_actions_mask,
                LANG_BACKLIGHT_SELECTIVE,
                0, "Selective Backlight Actions", UNIT_INT,
                0, 2048,2, NULL, NULL, NULL),
    INT_SETTING(F_NO_WRAP, brightness, LANG_BRIGHTNESS,
                DEFAULT_BRIGHTNESS_SETTING, "brightness",UNIT_INT,
                MIN_BRIGHTNESS_SETTING, MAX_BRIGHTNESS_SETTING, 1,
                NULL, NULL, backlight_set_brightness),
    /* backlight fading */
#if defined(HAVE_BACKLIGHT_FADING_INT_SETTING)
    TABLE_SETTING_LIST(F_TIME_SETTING | F_ALLOW_ARBITRARY_VALS, backlight_fade_in,
                  LANG_BACKLIGHT_FADE_IN, 300, "backlight fade in", "off",
                  UNIT_MS, formatter_time_unit_0_is_off, getlang_time_unit_0_is_off,
                  backlight_set_fade_in, 7, backlight_fade),
    TABLE_SETTING_LIST(F_TIME_SETTING | F_ALLOW_ARBITRARY_VALS, backlight_fade_out,
                  LANG_BACKLIGHT_FADE_OUT, 2000, "backlight fade out", "off",
                  UNIT_MS, formatter_time_unit_0_is_off,
                  getlang_time_unit_0_is_off,
                  backlight_set_fade_out, 10, backlight_fade),
#endif
    INT_SETTING(F_THEMESETTING|F_PADTITLE, scroll_speed, LANG_SCROLL_SPEED, 9,"scroll speed",
                UNIT_INT, 0, 17, 1, NULL, NULL, lcd_scroll_speed),
    INT_SETTING(F_THEMESETTING|F_TIME_SETTING | F_PADTITLE, scroll_delay, LANG_SCROLL_DELAY,
                1000, "scroll delay", UNIT_MS, 0, 3000, 100,
                formatter_time_unit_0_is_off,
                getlang_time_unit_0_is_off, lcd_scroll_delay),
    INT_SETTING(F_THEMESETTING, bidir_limit, LANG_BIDIR_SCROLL, 50, "bidir limit",
                UNIT_PERCENT, 0, 200, 25, NULL, NULL, lcd_bidir_scroll),
    OFFON_SETTING(F_THEMESETTING, offset_out_of_view, LANG_SCREEN_SCROLL_VIEW,
                  false, "Screen Scrolls Out Of View", NULL),
    OFFON_SETTING(F_THEMESETTING, disable_mainmenu_scrolling, LANG_DISABLE_MAINMENU_SCROLLING,
                  false, "Disable main menu scrolling", NULL),
    INT_SETTING(F_THEMESETTING|F_PADTITLE, scroll_step, LANG_SCROLL_STEP, 6, "scroll step",
                UNIT_PIXEL, 1, LCD_WIDTH, 1, NULL, NULL, lcd_scroll_step),
    INT_SETTING(F_THEMESETTING|F_PADTITLE, screen_scroll_step, LANG_SCREEN_SCROLL_STEP, 16,
                "screen scroll step", UNIT_PIXEL, 1, LCD_WIDTH, 1, NULL, NULL, NULL),
    OFFON_SETTING(0,scroll_paginated,LANG_SCROLL_PAGINATED,
                  false,"scroll paginated",NULL),
    OFFON_SETTING(0,list_wraparound,LANG_LIST_WRAPAROUND,
                  true,"list wraparound",NULL),
    CHOICE_SETTING(0, list_order, LANG_LIST_ORDER,
                   1,
                   /* values are defined by the enum in option_select.h */
                   "list order", "descending,ascending",
                   NULL, 2, ID2P(LANG_DESCENDING), ID2P(LANG_ASCENDING)),

    {F_T_INT|F_RGB|F_THEMESETTING ,&global_settings.fg_color,-1,
        INT(DEFAULT_THEME_FOREGROUND),"foreground color",UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING ,&global_settings.bg_color,-1,
        INT(DEFAULT_THEME_BACKGROUND),"background color",UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING ,&global_settings.lss_color,-1,
        INT(DEFAULT_THEME_SELECTOR_START),"line selector start color",UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING ,&global_settings.lse_color,-1,
        INT(DEFAULT_THEME_SELECTOR_END),"line selector end color",UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING ,&global_settings.lst_color,-1,
        INT(DEFAULT_THEME_SELECTOR_TEXT),"line selector text color",UNUSED},


    /* Modal dialog chrome: the metrics under Appearance -> Dialogs, the
     * colours under Appearance -> Colours, or a theme .cfg names them all.
     * The metric defaults are the chrome a theme gets for saying nothing, so
     * they are the shipped look rather than dialog_style_default()'s bare
     * 1px square borders.
     *
     * F_THEMERESET throughout is what makes "for saying nothing" true. Without
     * it every value below is inherited, so a theme is handed the last theme's
     * dialog chrome -- rounded buttons on a theme that never asked for them.
     * It applies equally to a value set from the menu: the next theme load
     * returns it to the default below. */
    INT_SETTING(F_THEMESETTING|F_THEMERESET, dialog_box_border_width,
        LANG_DIALOG_BOX_BORDER_WIDTH, 2, "dialog box border width",
        UNIT_PIXEL, 0, 10, 1, NULL, NULL, NULL),
    INT_SETTING(F_THEMESETTING|F_THEMERESET, dialog_box_margin,
        LANG_DIALOG_BOX_MARGIN, 10, "dialog box margin",
        UNIT_PIXEL, 0, 40, 1, NULL, NULL, NULL),
    INT_SETTING(F_THEMESETTING|F_THEMERESET, dialog_box_shadow,
        LANG_DIALOG_BOX_SHADOW, 4, "dialog box shadow",
        UNIT_PIXEL, 0, 16, 1, NULL, NULL, NULL),
    /* Black rather than a theme colour: the shadow's job is to sit the box off
       whatever is behind it, and a colour derived from the theme's own pair is
       the one thing guaranteed not to contrast with the box. Unlike the nine
       below, it is not gated on dialog_colors. */
    {F_T_INT|F_RGB|F_THEMESETTING|F_THEMERESET,
        &global_settings.dialog_box_shadow_color, LANG_DIALOG_BOX_SHADOW_COLOR,
        INT(LCD_BLACK), "dialog box shadow colour", UNUSED},
    INT_SETTING(F_THEMESETTING|F_THEMERESET, dialog_btn_border_width,
        LANG_DIALOG_BTN_BORDER_WIDTH, 2, "dialog button border width",
        UNIT_PIXEL, 0, 10, 1, NULL, NULL, NULL),
    /* Square by default: a radius is a look, and a theme that says nothing
     * should get the plain shape rather than this fork's. A theme that wants
     * rounded buttons asks for one in its own .cfg. */
    INT_SETTING(F_THEMESETTING|F_THEMERESET, dialog_btn_border_radius,
        LANG_DIALOG_BTN_BORDER_RADIUS, 0, "dialog button border radius",
        UNIT_PIXEL, 0, 20, 1, NULL, NULL, NULL),
    /* off:  every dialog colour is inherited from the theme, flat.
     * on:   the nine colours below are used instead.
     * auto: derived from the theme's foreground and background, or from the
     *       album's while dynamic colours are running (the default). */
    CHOICE_SETTING(F_THEMESETTING|F_THEMERESET, dialog_colors,
                   LANG_DIALOG_COLORS, DIALOG_COLORS_AUTO,
                   "dialog colours", "off,on,auto", NULL, 3,
                   ID2P(LANG_OFF), ID2P(LANG_ON), ID2P(LANG_AUTO)),
    {F_T_INT|F_RGB|F_THEMESETTING|F_THEMERESET, &global_settings.dialog_box_fg,
        -1, INT(DEFAULT_THEME_FOREGROUND), "dialog box foreground", UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING|F_THEMERESET, &global_settings.dialog_box_bg,
        -1, INT(DEFAULT_THEME_BACKGROUND), "dialog box background", UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING|F_THEMERESET,
        &global_settings.dialog_box_border, -1,
        INT(DEFAULT_THEME_FOREGROUND), "dialog box border colour", UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING|F_THEMERESET, &global_settings.dialog_btn_fg,
        -1, INT(DEFAULT_THEME_FOREGROUND), "dialog button foreground", UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING|F_THEMERESET, &global_settings.dialog_btn_bg,
        -1, INT(DEFAULT_THEME_BACKGROUND), "dialog button background", UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING|F_THEMERESET,
        &global_settings.dialog_btn_border, -1,
        INT(DEFAULT_THEME_FOREGROUND), "dialog button border colour", UNUSED},
    /* the selected button defaults to the inverse-video look of the plain box */
    {F_T_INT|F_RGB|F_THEMESETTING|F_THEMERESET,
        &global_settings.dialog_btn_fg_sel, -1,
        INT(DEFAULT_THEME_BACKGROUND), "dialog button foreground selected",
        UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING|F_THEMERESET,
        &global_settings.dialog_btn_bg_sel, -1,
        INT(DEFAULT_THEME_FOREGROUND), "dialog button background selected",
        UNUSED},
    {F_T_INT|F_RGB|F_THEMESETTING|F_THEMERESET,
        &global_settings.dialog_btn_border_sel, -1,
        INT(DEFAULT_THEME_FOREGROUND), "dialog button border colour selected",
        UNUSED},

    /* Progress bar chrome, config-file only for the same reason as the dialog
     * metrics above, F_THEMERESET included. The border rounds to this and the
     * fill to one less, so a radius of 1 is the smallest that rounds
     * anything. */
    {F_T_INT|F_THEMESETTING|F_THEMERESET, &global_settings.progress_bar_radius,
        -1, INT(2), "progress bar radius", UNUSED},

    /* more playback */
    OFFON_SETTING(0,play_selected,LANG_PLAY_SELECTED,true,"play selected",NULL),
    CHOICE_SETTING(0, single_mode, LANG_SINGLE_MODE, 0,
                  "single mode",
                  "off,track,album,album artist,artist,composer,work,genre,playlist",
                  NULL, 9,
                  ID2P(LANG_OFF),
                  ID2P(LANG_TRACK),
                  ID2P(LANG_ID3_ALBUM),
                  ID2P(LANG_ID3_ALBUMARTIST),
                  ID2P(LANG_ID3_ARTIST),
                  ID2P(LANG_ID3_COMPOSER),
                  ID2P(LANG_ID3_GROUPING),
                  ID2P(LANG_ID3_GENRE),
                  ID2P(LANG_PLAYLIST)),
    OFFON_SETTING(0,party_mode,LANG_PARTY_MODE,false,"party mode",NULL),
    OFFON_SETTING(0,fade_on_stop,LANG_FADE_ON_STOP,true,"volume fade",NULL),
    INT_SETTING(F_TIME_SETTING, ff_rewind_min_step, LANG_FFRW_STEP, 1,
                "scan min step", UNIT_SEC, 1, 60, 1, NULL, NULL, NULL),
    CHOICE_SETTING(0, ff_rewind_accel, LANG_FFRW_ACCEL, 2,
                   "seek acceleration", "very fast,fast,normal,slow,very slow", NULL, 5,
                   ID2P(LANG_VERY_FAST), ID2P(LANG_FAST), ID2P(LANG_NORMAL),
                   ID2P(LANG_SLOW) , ID2P(LANG_VERY_SLOW)),
    TABLE_SETTING(F_TIME_SETTING | F_ALLOW_ARBITRARY_VALS, buffer_margin,
                  LANG_MP3BUFFER_MARGIN, 5, "antiskip", NULL, UNIT_SEC,
                  NULL, NULL,
                  NULL,8, 5,15,30,60,120,180,300,600),
    /* disk */
    INT_SETTING(F_TIME_SETTING, disk_spindown, LANG_SPINDOWN, 5, "disk spindown",
                    UNIT_SEC, 3, 254, 1, NULL, NULL, STORAGE_FUNCTION(spindown)),
    CHOICE_SETTING(0, storage_mode, LANG_STORAGE_MODE, 0,
                   "storage mode", "auto,hdd,ssd", NULL, 3,
                   ID2P(LANG_AUTO), ID2P(LANG_STORAGE_HDD),
                   ID2P(LANG_STORAGE_SSD)),
    /* browser */
    TEXT_SETTING(0, start_directory, "start directory", "/", NULL, NULL),
    CHOICE_SETTING(0, dirfilter, LANG_FILTER, SHOW_SUPPORTED, "show files",
                   "all,supported,music,playlists", NULL, 4, ID2P(LANG_ALL),
                   ID2P(LANG_FILTER_SUPPORTED), ID2P(LANG_FILTER_MUSIC),
                   ID2P(LANG_PLAYLISTS)),
    /* file sorting */
    OFFON_SETTING(0, sort_case, LANG_SORT_CASE, false, "sort case", NULL),
    CHOICE_SETTING(0, sort_dir, LANG_SORT_DIR, 0 ,
                   "sort dirs", "alpha,oldest,newest", treesort_callback, 3,
                   ID2P(LANG_SORT_ALPHA), ID2P(LANG_SORT_DATE),
                   ID2P(LANG_SORT_DATE_REVERSE)),
    CHOICE_SETTING(0, sort_file, LANG_SORT_FILE, 0 ,
                   "sort files", "alpha,oldest,newest,type", treesort_callback, 4,
                   ID2P(LANG_SORT_ALPHA), ID2P(LANG_SORT_DATE),
                   ID2P(LANG_SORT_DATE_REVERSE) , ID2P(LANG_SORT_TYPE)),
    CHOICE_SETTING(0, sort_playlists, LANG_SORT_PLAYLISTS, 0 ,
                   "sort playlists", "alpha,oldest,newest", treesort_callback, 3,
                   ID2P(LANG_SORT_ALPHA), ID2P(LANG_SORT_DATE),
                   ID2P(LANG_SORT_DATE_REVERSE)),
    CHOICE_SETTING(0, interpret_numbers, LANG_SORT_INTERPRET_NUMBERS, 1,
                    "sort interpret number", "digits,numbers",treesort_callback, 2,
                    ID2P(LANG_SORT_INTERPRET_AS_DIGIT),
                    ID2P(LANG_SORT_INTERPRET_AS_NUMBERS)),
    CHOICE_SETTING(0, show_filename_ext, LANG_SHOW_FILENAME_EXT, 3,
                   "show filename exts", "off,on,unknown,view_all", NULL , 4 ,
                   ID2P(LANG_OFF), ID2P(LANG_ON), ID2P(LANG_UNKNOWN_TYPES),
                   ID2P(LANG_EXT_ONLY_VIEW_ALL)),
    OFFON_SETTING(0,browse_current,LANG_FOLLOW,false,"follow playlist",NULL),
    OFFON_SETTING(F_THEMESETTING,playlist_viewer_icons,LANG_SHOW_ICONS,true,
                  "playlist viewer icons",NULL),
    OFFON_SETTING(F_THEMESETTING,playlist_viewer_indices,LANG_SHOW_INDICES,true,
                  "playlist viewer indices",NULL),
    CHOICE_SETTING(F_THEMESETTING, playlist_viewer_track_display, LANG_TRACK_DISPLAY, 0,
                   "playlist viewer track display",
                   "track name,full path,title and album from tags,title from tags",
                   NULL, 4, ID2P(LANG_DISPLAY_TRACK_NAME_ONLY),
                   ID2P(LANG_DISPLAY_FULL_PATH),ID2P(LANG_DISPLAY_TITLEALBUM_FROMTAGS),
                   ID2P(LANG_DISPLAY_TITLE_FROMTAGS)),
    CHOICE_SETTING(0, recursive_dir_insert, LANG_RECURSE_DIRECTORY , RECURSE_ON,
                   "recursive directory insert", off_on_ask, NULL , 3 ,
                   ID2P(LANG_OFF), ID2P(LANG_ON), ID2P(LANG_ASK)),
    /* bookmarks */
    CHOICE_SETTING(0, autocreatebookmark, LANG_BOOKMARK_SETTINGS_AUTOCREATE,
                   BOOKMARK_NO, "autocreate bookmarks",
                   "off,on,ask,recent only - on,recent only - ask", NULL, 5,
                   ID2P(LANG_SET_BOOL_NO), ID2P(LANG_SET_BOOL_YES),
                   ID2P(LANG_ASK), ID2P(LANG_BOOKMARK_SETTINGS_RECENT_ONLY_YES),
                   ID2P(LANG_BOOKMARK_SETTINGS_RECENT_ONLY_ASK)),
    OFFON_SETTING(0, autoupdatebookmark, LANG_BOOKMARK_SETTINGS_AUTOUPDATE,
                   false, "autoupdate bookmarks", NULL),
    CHOICE_SETTING(0, autoloadbookmark, LANG_BOOKMARK_SETTINGS_AUTOLOAD,
                   BOOKMARK_NO, "autoload bookmarks", off_on_ask, NULL, 3,
                   ID2P(LANG_SET_BOOL_NO), ID2P(LANG_SET_BOOL_YES),
                   ID2P(LANG_ASK)),
    CHOICE_SETTING(0, usemrb, LANG_BOOKMARK_SETTINGS_MAINTAIN_RECENT_BOOKMARKS,
                   BOOKMARK_NO, "use most-recent-bookmarks",
                   "off,on,unique only,one per track", NULL, 4, ID2P(LANG_SET_BOOL_NO),
                   ID2P(LANG_SET_BOOL_YES),
                   ID2P(LANG_BOOKMARK_SETTINGS_ONE_PER_PLAYLIST),
                   ID2P(LANG_BOOKMARK_SETTINGS_ONE_PER_TRACK)),
    /* peak meter */
    TABLE_SETTING_LIST(F_TIME_SETTING | F_ALLOW_ARBITRARY_VALS, peak_meter_clip_hold,
                  LANG_PM_CLIP_HOLD, 60, "peak meter clip hold", "eternal",
                  UNIT_SEC, formatter_time_unit_0_is_eternal,
                  getlang_time_unit_0_is_eternal, peak_meter_set_clip_hold,
                  31, &timeout_sec_common[1]), /* skip -1 entry */
    TABLE_SETTING(F_TIME_SETTING | F_ALLOW_ARBITRARY_VALS, peak_meter_hold,
                  LANG_PM_PEAK_HOLD, 500, "peak meter hold", off, UNIT_MS,
                  formatter_time_unit_0_is_off, getlang_time_unit_0_is_off,NULL,
                  18, 0,200,300,500,1000,2000,3000,4000,5000,6000,7000,8000,
                  9000,10000,15000,20000,30000,60000),
    INT_SETTING(0, peak_meter_release, LANG_PM_RELEASE, 8, "peak meter release",
                UNIT_PM_TICK, 1, 0x7e, 1, NULL, NULL,NULL),
    OFFON_SETTING(0,peak_meter_dbfs,LANG_PM_DBFS,true,"peak meter dbfs",NULL),
    {F_T_INT, &global_settings.peak_meter_min, LANG_PM_MIN,INT(60),
        "peak meter min", UNUSED},
    {F_T_INT, &global_settings.peak_meter_max, LANG_PM_MAX,INT(0),
        "peak meter max", UNUSED},
    /* voice */
    OFFON_SETTING(F_TEMPVAR, talk_menu, LANG_VOICE_MENU, true, "talk menu", NULL),
    CHOICE_SETTING(0, talk_dir, LANG_VOICE_DIR, 0,
                   "talk dir", off_number_spell, NULL, 3,
                   ID2P(LANG_OFF), ID2P(LANG_VOICE_NUMBER),
                   ID2P(LANG_VOICE_SPELL)),
    OFFON_SETTING(F_TEMPVAR, talk_dir_clip, LANG_VOICE_DIR_TALK, false,
                  "talk dir clip", NULL),
    CHOICE_SETTING(0, talk_file, LANG_VOICE_FILE, 0,
                   "talk file", off_number_spell, NULL, 3,
                   ID2P(LANG_OFF), ID2P(LANG_VOICE_NUMBER),
                   ID2P(LANG_VOICE_SPELL)),
    OFFON_SETTING(F_TEMPVAR, talk_file_clip, LANG_VOICE_FILE_TALK, false,
                  "talk file clip", NULL),
    OFFON_SETTING(F_TEMPVAR, talk_filetype, LANG_VOICE_FILETYPE, false,
                  "talk filetype", NULL),
    OFFON_SETTING(F_TEMPVAR, talk_battery_level, LANG_TALK_BATTERY_LEVEL, false,
                  "Announce Battery Level", NULL),
    INT_SETTING(0, talk_mixer_amp, LANG_TALK_MIXER_LEVEL, 100,
        "talk mixer level", UNIT_PERCENT, 0, 100, 5, NULL, NULL, voice_set_mixer_level),



    CHOICE_SETTING(0, next_folder, LANG_NEXT_FOLDER, FOLDER_ADVANCE_OFF,
                   "folder navigation", "off,on,random",NULL ,3,
                   ID2P(LANG_SET_BOOL_NO), ID2P(LANG_SET_BOOL_YES),
                   ID2P(LANG_RANDOM)),
    BOOL_SETTING(0, constrain_next_folder, LANG_CONSTRAIN_NEXT_FOLDER, false,
                 "constrain next folder", off_on,
                 LANG_SET_BOOL_YES, LANG_SET_BOOL_NO, NULL),

    BOOL_SETTING(0, autoresume_enable, LANG_AUTORESUME, false,
                 "autoresume enable", off_on,
                 LANG_SET_BOOL_YES, LANG_SET_BOOL_NO, NULL),
    CHOICE_SETTING(0, autoresume_automatic, LANG_AUTORESUME_AUTOMATIC,
                   AUTORESUME_NEXTTRACK_NEVER,
                   "autoresume next track", "never,all,custom",
                   NULL, 3,
                   ID2P(LANG_SET_BOOL_NO),
                   ID2P(LANG_ALWAYS),
                   ID2P(LANG_AUTORESUME_CUSTOM)),
    TEXT_SETTING(0, autoresume_paths, "autoresume next track paths",
                 "/podcast:/podcasts", NULL, NULL),

    OFFON_SETTING(0, runtimedb, LANG_RUNTIMEDB_ACTIVE, true,
                  "gather runtime data", NULL),
    TEXT_SETTING(0, tagcache_scan_paths, "database scan paths",
                 DEFAULT_TAGCACHE_SCAN_PATHS, NULL, NULL),
    TEXT_SETTING(0, tagcache_db_path, "database path",
                 ROCKBOX_DIR, NULL, NULL),

    /* replay gain */
    CHOICE_SETTING(F_SOUNDSETTING, replaygain_settings.type,
                   LANG_REPLAYGAIN_MODE, REPLAYGAIN_SHUFFLE, "replaygain type",
                   "track,album,track shuffle,off", NULL, 4, ID2P(LANG_TRACK_GAIN),
                   ID2P(LANG_ALBUM_GAIN), ID2P(LANG_SHUFFLE_GAIN), ID2P(LANG_OFF)),
    OFFON_SETTING(F_SOUNDSETTING, replaygain_settings.noclip,
                  LANG_REPLAYGAIN_NOCLIP, false, "replaygain noclip", NULL),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, replaygain_settings.preamp,
                       LANG_REPLAYGAIN_PREAMP, 0, "replaygain preamp",
                       UNIT_DB, -120, 120, 5, db_format, get_dec_talkid, NULL),

    CHOICE_SETTING(0, beep, LANG_BEEP, 0, "beep", "off,weak,moderate,strong",
                   NULL, 4, ID2P(LANG_OFF), ID2P(LANG_WEAK),
                   ID2P(LANG_MODERATE), ID2P(LANG_STRONG)),

    /* crossfade */
    CHOICE_SETTING(F_SOUNDSETTING, crossfade, LANG_CROSSFADE_ENABLE, 0,
                   "crossfade",
                   "off,auto track change,man track skip,shuffle,shuffle or man track skip,always",
                   NULL, 6, ID2P(LANG_OFF), ID2P(LANG_AUTOTRACKSKIP),
                   ID2P(LANG_MANTRACKSKIP), ID2P(LANG_SHUFFLE),
                   ID2P(LANG_SHUFFLE_TRACKSKIP), ID2P(LANG_ALWAYS)),
    INT_SETTING(F_TIME_SETTING | F_SOUNDSETTING, crossfade_fade_in_delay,
                LANG_CROSSFADE_FADE_IN_DELAY, 0,
                "crossfade fade in delay", UNIT_SEC, 0, 7, 1, NULL, NULL, NULL),
    INT_SETTING(F_TIME_SETTING | F_SOUNDSETTING, crossfade_fade_out_delay,
                LANG_CROSSFADE_FADE_OUT_DELAY, 0,
                "crossfade fade out delay", UNIT_SEC, 0, 7, 1, NULL, NULL,NULL),
    INT_SETTING(F_TIME_SETTING | F_SOUNDSETTING, crossfade_fade_in_duration,
                LANG_CROSSFADE_FADE_IN_DURATION, 2,
                "crossfade fade in duration", UNIT_SEC, 0, 15, 1, NULL, NULL, NULL),
    INT_SETTING(F_TIME_SETTING | F_SOUNDSETTING, crossfade_fade_out_duration,
                LANG_CROSSFADE_FADE_OUT_DURATION, 2,
                "crossfade fade out duration", UNIT_SEC, 0, 15, 1, NULL, NULL, NULL),
    CHOICE_SETTING(F_SOUNDSETTING, crossfade_fade_out_mixmode,
                   LANG_CROSSFADE_FADE_OUT_MODE, 0,
                   "crossfade fade out mode", "crossfade,mix", NULL, 2,
                   ID2P(LANG_CROSSFADE), ID2P(LANG_MIX)),

    /* crossfeed */
    CHOICE_SETTING(F_SOUNDSETTING, crossfeed, LANG_CROSSFEED, 0,"crossfeed",
                   "off,meier,custom", dsp_set_crossfeed_type, 3,
                   ID2P(LANG_OFF), ID2P(LANG_CROSSFEED_MEIER),
                   ID2P(LANG_CROSSFEED_CUSTOM)),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, crossfeed_direct_gain,
                       LANG_CROSSFEED_DIRECT_GAIN, -15,
                       "crossfeed direct gain", UNIT_DB, -60, 0, 5,
                       db_format, get_dec_talkid,dsp_set_crossfeed_direct_gain),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, crossfeed_cross_gain,
                       LANG_CROSSFEED_CROSS_GAIN, -60,
                       "crossfeed cross gain", UNIT_DB, -120, -30, 5,
                       db_format, get_dec_talkid, crossfeed_cross_set),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, crossfeed_hf_attenuation,
                       LANG_CROSSFEED_HF_ATTENUATION, -160,
                       "crossfeed hf attenuation", UNIT_DB, -240, -60, 5,
                       db_format, get_dec_talkid, crossfeed_cross_set),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, crossfeed_hf_cutoff,
                       LANG_CROSSFEED_HF_CUTOFF, 700,
                       "crossfeed hf cutoff", UNIT_HERTZ, 500, 2000, 100,
                       NULL, NULL, crossfeed_cross_set),

    /* equalizer */
    OFFON_SETTING(F_EQSETTING, eq_enabled, LANG_EQUALIZER_ENABLED, false,
                  "eq enabled", eq_enabled_option_callback),

    INT_SETTING_NOWRAP(F_EQSETTING, eq_precut, LANG_EQUALIZER_PRECUT, 0,
                       "eq precut", UNIT_DB, 0, 240, 1, eq_precut_format,
                       get_precut_talkid, dsp_set_eq_precut),

/* The pre-type setting names, kept so an existing config or EQ preset still
   loads. F_DEPRECATED reads them but never writes them back, so a config saved
   after this point uses the "eq filter N" names below. */
#define EQ_BAND_OLD(id, string, type) \
        CUSTOM_SETTING(F_EQSETTING|F_DEPRECATED, eq_band_settings[id], -1, \
                  &eq_defaults[id], string,                     \
                  eq_load_from_cfg_old_##type, eq_write_to_cfg, \
                  eq_is_changed, eq_set_default)
    EQ_BAND_OLD(0, "eq low shelf filter", low_shelf),
    EQ_BAND_OLD(1, "eq peak filter 1", peak),
    EQ_BAND_OLD(2, "eq peak filter 2", peak),
    EQ_BAND_OLD(3, "eq peak filter 3", peak),
    EQ_BAND_OLD(4, "eq peak filter 4", peak),
    EQ_BAND_OLD(5, "eq peak filter 5", peak),
    EQ_BAND_OLD(6, "eq peak filter 6", peak),
    EQ_BAND_OLD(7, "eq peak filter 7", peak),
    EQ_BAND_OLD(8, "eq peak filter 8", peak),
    EQ_BAND_OLD(9, "eq high shelf filter", high_shelf),
#undef EQ_BAND_OLD

#define EQ_BAND(id, string) \
        CUSTOM_SETTING(F_EQSETTING, eq_band_settings[id], -1,   \
                  &eq_defaults[id], string,                     \
                  eq_load_from_cfg_new, eq_write_to_cfg,        \
                  eq_is_changed, eq_set_default)
    EQ_BAND(0, "eq filter 0"),
    EQ_BAND(1, "eq filter 1"),
    EQ_BAND(2, "eq filter 2"),
    EQ_BAND(3, "eq filter 3"),
    EQ_BAND(4, "eq filter 4"),
    EQ_BAND(5, "eq filter 5"),
    EQ_BAND(6, "eq filter 6"),
    EQ_BAND(7, "eq filter 7"),
    EQ_BAND(8, "eq filter 8"),
    EQ_BAND(9, "eq filter 9"),
#undef EQ_BAND

    /* dithering */
    OFFON_SETTING(F_SOUNDSETTING, dithering_enabled, LANG_DITHERING, false,
                  "dithering enabled", dsp_dither_enable),
    /* surround */
     TABLE_SETTING(F_TIME_SETTING | F_SOUNDSETTING, surround_enabled,
                  LANG_SURROUND, 0, "surround enabled", off,
                  UNIT_MS, formatter_time_unit_0_is_off,
                  getlang_time_unit_0_is_off,
                  dsp_surround_enable, 6,
                  0,5,8,10,15,30),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, surround_balance,
                       LANG_BALANCE, 35,
                       "surround balance", UNIT_PERCENT, 0, 99,
                       1, NULL, NULL, dsp_surround_set_balance),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, surround_fx1,
                       LANG_SURROUND_FX1, 3400,
                       "surround_fx1", UNIT_HERTZ, 600, 8000,
                       200, NULL, NULL, surround_set_factor),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, surround_fx2,
                       LANG_SURROUND_FX2, 320,
                       "surround_fx2", UNIT_HERTZ, 40, 400,
                       40, NULL, NULL, surround_set_factor),
    OFFON_SETTING(F_SOUNDSETTING, surround_method2, LANG_SURROUND_METHOD2, false,
                  "side only", dsp_surround_side_only),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, surround_mix,
                       LANG_SURROUND_MIX, 50,
                       "surround mix", UNIT_PERCENT, 0, 100,
                       5, NULL, NULL, dsp_surround_mix),
    /* auditory fatigue reduction */
    CHOICE_SETTING(F_SOUNDSETTING|F_NO_WRAP, afr_enabled,
                       LANG_AFR, 0,"afr enabled",
                       "off,weak,moderate,strong", dsp_afr_enable, 4,
                       ID2P(LANG_OFF), ID2P(LANG_WEAK),ID2P(LANG_MODERATE),ID2P(LANG_STRONG)),
    /* PBE */
    INT_SETTING_NOWRAP(F_SOUNDSETTING, pbe,
                       LANG_PBE, 0,
                       "pbe", UNIT_PERCENT, 0, 100,
                       25, NULL, NULL, dsp_pbe_enable),
    INT_SETTING_NOWRAP(F_SOUNDSETTING, pbe_precut,
                       LANG_EQUALIZER_PRECUT, -25,
                       "pbe precut", UNIT_DB, -45, 0,
                       1, db_format, NULL, dsp_pbe_precut),
    /* compressor */
    INT_SETTING_NOWRAP(F_SOUNDSETTING, compressor_settings.threshold,
                       LANG_COMPRESSOR_THRESHOLD, 0,
                       "compressor threshold", UNIT_DB, 0, -24,
                       -3, formatter_unit_0_is_off, getlang_unit_0_is_off,
                       compressor_set),
    CHOICE_SETTING(F_SOUNDSETTING|F_NO_WRAP, compressor_settings.makeup_gain,
                   LANG_COMPRESSOR_GAIN, 1, "compressor makeup gain",
                   "off,auto", compressor_set, 2,
                   ID2P(LANG_OFF), ID2P(LANG_AUTO)),
    CHOICE_SETTING(F_SOUNDSETTING|F_NO_WRAP, compressor_settings.ratio,
                   LANG_COMPRESSOR_RATIO, 1, "compressor ratio",
                   "2:1,4:1,6:1,10:1,limit", compressor_set, 5,
                   ID2P(LANG_COMPRESSOR_RATIO_2), ID2P(LANG_COMPRESSOR_RATIO_4),
                   ID2P(LANG_COMPRESSOR_RATIO_6), ID2P(LANG_COMPRESSOR_RATIO_10),
                   ID2P(LANG_COMPRESSOR_RATIO_LIMIT)),
    CHOICE_SETTING(F_SOUNDSETTING|F_NO_WRAP, compressor_settings.knee,
                   LANG_COMPRESSOR_KNEE, 1, "compressor knee",
                   "hard knee,soft knee", compressor_set, 2,
                   ID2P(LANG_COMPRESSOR_HARD_KNEE), ID2P(LANG_COMPRESSOR_SOFT_KNEE)),
    INT_SETTING_NOWRAP(F_TIME_SETTING | F_SOUNDSETTING,
                       compressor_settings.attack_time,
                       LANG_COMPRESSOR_ATTACK, 5,
                       "compressor attack time", UNIT_MS, 0, 30,
                       5, NULL, NULL, compressor_set),
    INT_SETTING_NOWRAP(F_TIME_SETTING | F_SOUNDSETTING,
                       compressor_settings.release_time,
                       LANG_COMPRESSOR_RELEASE, 500,
                       "compressor release time", UNIT_MS, 100, 1000,
                       100, NULL, NULL, compressor_set),

    SOUND_SETTING(F_NO_WRAP, bass_cutoff, LANG_BASS_CUTOFF,
                  "bass cutoff", SOUND_BASS_CUTOFF),
    SOUND_SETTING(F_NO_WRAP, treble_cutoff, LANG_TREBLE_CUTOFF,
                  "treble cutoff", SOUND_TREBLE_CUTOFF),
    /*enable dircache for all targets > 2MB of RAM by default*/
    OFFON_SETTING(F_BANFROMQS,dircache,LANG_DIRCACHE_ENABLE,true,"dircache",NULL),
    SYSTEM_STATUS(0, dircache_size, 0, "DSZ"),

    CHOICE_SETTING(F_BANFROMQS, tagcache_ram, LANG_TAGCACHE_RAM,
                   2, "tagcache_ram", "off,on,quick",
                   NULL, 3,
                   ID2P(LANG_OFF), ID2P(LANG_ON), ID2P(LANG_QUICK_IGNORE_DIRACHE)),
    OFFON_SETTING(F_BANFROMQS, tagcache_scan_on_eject, LANG_TAGCACHE_SCAN_ON_EJECT, true,
                  "tagcache_scan_on_eject", NULL),
    /* Separate from the above: that one rescans after a USB session that wrote
     * to us, which is when the library has demonstrably changed. This one is
     * the boot-time check, which on a player whose library only changes over
     * USB is usually looking for changes that cannot have happened. */
    OFFON_SETTING(F_BANFROMQS, tagcache_scan_on_startup, LANG_SCAN_ON_STARTUP,
                  false, "tagcache_scan_on_startup", NULL),
    /* A commit cut short (a flat battery, a USB session mid-scan) leaves work
     * to finish at the next boot. On means finish it; off asks first, which is
     * only worth having because the commit holds up the database for as long
     * as it runs. */
    OFFON_SETTING(F_BANFROMQS, tagcache_autocommit, LANG_AUTOCOMMIT_ON_STARTUP,
                  true, "tagcache_autocommit", NULL),
    /* Database search. The row and letter counts are choices rather than int
     * settings so the steps are fixed and the config file holds the figure as
     * written. Both store the index; db_search.c turns it back into a count. */
    STRINGCHOICE_SETTING(F_BANFROMQS, db_search_max_rows, LANG_DB_SEARCH_MAX_ROWS,
                         1, "search max rows", "25,50,75,100,125,150,175,200",
                         NULL, 8,
                         TALK_ID(25, UNIT_INT), TALK_ID(50, UNIT_INT),
                         TALK_ID(75, UNIT_INT), TALK_ID(100, UNIT_INT),
                         TALK_ID(125, UNIT_INT), TALK_ID(150, UNIT_INT),
                         TALK_ID(175, UNIT_INT), TALK_ID(200, UNIT_INT)),
    STRINGCHOICE_SETTING(F_BANFROMQS, db_search_min_letters,
                         LANG_DB_SEARCH_MIN_LETTERS, 0, "search min letters",
                         "1,2,3", NULL, 3,
                         TALK_ID(1, UNIT_INT), TALK_ID(2, UNIT_INT),
                         TALK_ID(3, UNIT_INT)),
    CHOICE_SETTING(F_BANFROMQS, db_search_order, LANG_DB_SEARCH_ORDER,
                   DB_SEARCH_ORDER_TRACK_ALBUM_ARTIST, "search order",
                   "track album artist,track artist album,"
                   "album track artist,album artist track,"
                   "artist album track,artist track album",
                   NULL, DB_SEARCH_ORDER_COUNT,
                   ID2P(LANG_DB_SEARCH_ORDER_TRACK_ALBUM_ARTIST),
                   ID2P(LANG_DB_SEARCH_ORDER_TRACK_ARTIST_ALBUM),
                   ID2P(LANG_DB_SEARCH_ORDER_ALBUM_TRACK_ARTIST),
                   ID2P(LANG_DB_SEARCH_ORDER_ALBUM_ARTIST_TRACK),
                   ID2P(LANG_DB_SEARCH_ORDER_ARTIST_ALBUM_TRACK),
                   ID2P(LANG_DB_SEARCH_ORDER_ARTIST_TRACK_ALBUM)),
    CHOICE_SETTING(F_TEMPVAR, default_codepage, LANG_DEFAULT_CODEPAGE, 14,
                   "default codepage",
                   /* The order must match with that in unicode.c */
                   "iso8859-1,iso8859-7,iso8859-8,cp1251,iso8859-11,cp1256,"
                   "iso8859-9,iso8859-2,cp1250,cp1252,sjis,gb2312,ksx1001,big5,utf-8",
                   NULL, 15,
                   ID2P(LANG_CODEPAGE_LATIN1),
                   ID2P(LANG_CODEPAGE_GREEK),
                   ID2P(LANG_CODEPAGE_HEBREW), ID2P(LANG_CODEPAGE_CYRILLIC),
                   ID2P(LANG_CODEPAGE_THAI), ID2P(LANG_CODEPAGE_ARABIC),
                   ID2P(LANG_CODEPAGE_TURKISH),
                   ID2P(LANG_CODEPAGE_LATIN_EXTENDED),
                   ID2P(LANG_CODEPAGE_CENTRAL_EUROPEAN),
                   ID2P(LANG_CODEPAGE_WESTERN_EUROPEAN),
                   ID2P(LANG_CODEPAGE_JAPANESE),
                   ID2P(LANG_CODEPAGE_SIMPLIFIED), ID2P(LANG_CODEPAGE_KOREAN),
                   ID2P(LANG_CODEPAGE_TRADITIONAL), ID2P(LANG_CODEPAGE_UTF8)),

    OFFON_SETTING(0, warnon_erase_dynplaylist, LANG_WARN_ERASEDYNPLAYLIST_MENU,
                  true, "warn when erasing dynamic playlist",NULL),
    OFFON_SETTING(0, keep_current_track_on_replace_playlist, LANG_KEEP_CURRENT_TRACK_ON_REPLACE,
                  true, "keep current track when replacing playlist",NULL),
    OFFON_SETTING(0, show_shuffled_adding_options, LANG_SHOW_SHUFFLED_ADDING_OPTIONS, true,
                      "show shuffled adding options", NULL),
    CHOICE_SETTING(0, show_queue_options, LANG_SHOW_QUEUE_OPTIONS, 0,
                      "show queue options", "off,on,in submenu",
                      NULL, 3,
                      ID2P(LANG_SET_BOOL_NO),
                      ID2P(LANG_SET_BOOL_YES),
                      ID2P(LANG_IN_SUBMENU)),

    CHOICE_SETTING(0, browser_default, LANG_DEFAULT_BROWSER,
                      1,
                      "default browser",
                      "files,database,playlists",
                      NULL,
                      3
                      ,ID2P(LANG_DIR_BROWSER),
                      ID2P(LANG_MUSIC_BROWSER),
                      ID2P(LANG_PLAYLISTS)),

    CHOICE_SETTING(0, backlight_on_button_hold,
                   LANG_BACKLIGHT_ON_BUTTON_HOLD,
                   1,
                   "backlight on button hold", "normal,off,on",
                   backlight_set_on_button_hold, 3,
                   ID2P(LANG_NORMAL), ID2P(LANG_OFF), ID2P(LANG_ON)),

    TABLE_SETTING_LIST(F_TIME_SETTING | F_ALLOW_ARBITRARY_VALS,
                    lcd_sleep_after_backlight_off, LANG_LCD_SLEEP_AFTER_BACKLIGHT_OFF,
                    5, "lcd sleep after backlight off",
                    off_on, UNIT_SEC, formatter_time_unit_0_is_always,
                    getlang_time_unit_0_is_always, lcd_set_sleep_after_backlight_off,
                    23, timeout_sec_common),

    OFFON_SETTING(0, hold_lr_for_scroll_in_list, -1, true,
                  "hold_lr_for_scroll_in_list",NULL),
    CHOICE_SETTING(0, show_path_in_browser, LANG_SHOW_PATH, SHOW_PATH_CURRENT,
                   "show path in browser", "off,current directory,full path",
                   NULL, 3, ID2P(LANG_OFF), ID2P(LANG_SHOW_PATH_CURRENT),
                   ID2P(LANG_DISPLAY_FULL_PATH)),


    CHOICE_SETTING(0, unplug_mode, LANG_HEADPHONE_UNPLUG, 0,
                   "pause on headphone unplug", "off,pause,pause and resume",
                   NULL, 3, ID2P(LANG_OFF), ID2P(LANG_PAUSE),
                   ID2P(LANG_HEADPHONE_UNPLUG_RESUME)),
    OFFON_SETTING(0, unplug_autoresume,
                  LANG_HEADPHONE_UNPLUG_DISABLE_AUTORESUME, false,
                  "disable autoresume if phones not present",NULL),
    INT_SETTING(F_TIME_SETTING, pause_rewind, LANG_PAUSE_REWIND, 0,
                "rewind duration on pause", UNIT_SEC, 0, 15, 1,
                formatter_time_unit_0_is_off, getlang_time_unit_0_is_off, NULL),
    TEXT_SETTING(F_THEMESETTING|F_NEEDAPPLY, font_file, "font",
                     DEFAULT_FONTNAME, FONT_DIR "/", ".fnt"),
    /* No font of its own: unset means the regular UI font, so a theme naming
     * only "font" gets a bold that matches it rather than the last theme's.
     * F_THEMERESET is what makes that true -- without it the setting keeps the
     * previous value and the fallback in font_get_ui_bold() never runs. */
    TEXT_SETTING(F_THEMESETTING|F_THEMERESET, bold_font_file, "font bold",
                     "-", FONT_DIR "/", ".fnt"),
    INT_SETTING(0, glyphs_to_cache, LANG_GLYPHS, DEFAULT_GLYPHS,
                "glyphs", UNIT_INT, MIN_GLYPHS, MAX_GLYPHS, 10,
                NULL, NULL, NULL),
    /* Core text viewer (viewers/text_viewer) */
    /* Defaults to white on black: long-form reading wants a fixed, high
       contrast page, not the theme's (or the album's) colours. */
    CHOICE_SETTING(0, text_viewer_colour_mode, LANG_TEXT_VIEWER_COLOUR, 3,
                   "text viewer colour mode",
                   "theme,inverted,black on white,white on black", NULL, 4,
                   ID2P(LANG_TEXT_VIEWER_COLOUR_THEME),
                   ID2P(LANG_TEXT_VIEWER_COLOUR_INVERTED),
                   ID2P(LANG_TEXT_VIEWER_COLOUR_BOW),
                   ID2P(LANG_TEXT_VIEWER_COLOUR_WOB)),
    OFFON_SETTING(0, text_viewer_margin, LANG_TEXT_VIEWER_MARGIN, true,
                  "text viewer margin", NULL),
    INT_SETTING(0, text_viewer_line_spacing, LANG_TEXT_VIEWER_LINE_SPACING, 0,
                "text viewer line spacing", UNIT_INT, 0, 8, 1,
                NULL, NULL, NULL),
    TEXT_SETTING(0, text_viewer_font_file, "text viewer font", "22-Literata",
                 FONT_DIR "/", ".fnt"),
    OFFON_SETTING(0, text_viewer_page_number, LANG_TEXT_VIEWER_PAGE_NUMBER,
                  false, "text viewer page number", NULL),
    /* Synchronised lyrics viewer (apps-ipod/viewers/lyric_viewer) */
    CHOICE_SETTING(0, lyric_colour_mode, LANG_TEXT_VIEWER_COLOUR, 0,
                   "lyric colour mode",
                   "theme,inverted,black on white,white on black", NULL, 4,
                   ID2P(LANG_TEXT_VIEWER_COLOUR_THEME),
                   ID2P(LANG_TEXT_VIEWER_COLOUR_INVERTED),
                   ID2P(LANG_TEXT_VIEWER_COLOUR_BOW),
                   ID2P(LANG_TEXT_VIEWER_COLOUR_WOB)),
    TEXT_SETTING(0, lyric_font_file, "lyric font", "", FONT_DIR "/", ".fnt"),
    INT_SETTING(0, lyric_line_spacing, LANG_TEXT_VIEWER_LINE_SPACING, 2,
                "lyric line spacing", UNIT_INT, 0, 10, 1, NULL, NULL, NULL),
    CHOICE_SETTING(0, lyric_align, LANG_LYRICS_ALIGN, 1, "lyric align",
                   "left,centre,right", NULL, 3,
                   ID2P(LANG_LEFT), ID2P(LANG_LYRICS_CENTRE),
                   ID2P(LANG_RIGHT)),
    INT_SETTING(0, lyric_prev_opacity, LANG_LYRICS_PREV_OPACITY, 30,
                "lyric previous opacity", UNIT_PERCENT, 0, 100, 5,
                NULL, NULL, NULL),
    INT_SETTING(0, lyric_next_opacity, LANG_LYRICS_NEXT_OPACITY, 55,
                "lyric next opacity", UNIT_PERCENT, 0, 100, 5,
                NULL, NULL, NULL),
    /* An index, not a duration -- CHOICE_SETTING stores the position in the
     * list. The viewer turns it into milliseconds, so the scale can be
     * retuned without invalidating a saved config. */
    CHOICE_SETTING(0, lyric_anim, LANG_LYRICS_ANIM, 2, "lyric animation",
                   "off,fast,normal,slow", NULL, 4,
                   ID2P(LANG_OFF), ID2P(LANG_LYRICS_ANIM_FAST),
                   ID2P(LANG_LYRICS_ANIM_NORMAL), ID2P(LANG_LYRICS_ANIM_SLOW)),
    OFFON_SETTING(0, lyric_highlight, LANG_LYRICS_HIGHLIGHT, true,
                  "lyric highlight", NULL),
    OFFON_SETTING(0, lyric_backlight, LANG_LYRICS_BACKLIGHT, true,
                  "lyric backlight", NULL),
    TEXT_SETTING(F_THEMESETTING|F_NEEDAPPLY,wps_file, "wps",
                     DEFAULT_WPSNAME, WPS_DIR "/", ".wps"),
    TEXT_SETTING(F_THEMESETTING|F_NEEDAPPLY,sbs_file, "sbs",
                     DEFAULT_SBSNAME, SBS_DIR "/", ".sbs"),
    /* Deliberately neither F_THEMESETTING nor F_THEMERESET: this records
     * *which* theme is loaded, so a theme .cfg must not be able to set it and
     * loading one must not clear it before it can be written. */
    TEXT_SETTING(0, theme_file, "theme", "", THEME_DIR "/", ".cfg"),
    TEXT_SETTING(0,lang_file,"lang","",LANG_DIR "/",".lng"),
    /* F_THEMERESET: a theme that names no backdrop means it wants none, not
     * the last theme's image showing through everything it draws. */
    TEXT_SETTING(F_THEMESETTING|F_THEMERESET|F_NEEDAPPLY,backdrop_file,"backdrop",
                     DEFAULT_BACKDROP, NULL, NULL),
    TEXT_SETTING(0,kbd_file,"kbd","-",ROCKBOX_DIR "/",".kbd"),
    CHOICE_SETTING(0, usb_charging, LANG_USB_CHARGING, TARGET_USB_CHARGING_DEFAULT, "usb charging",
                   "off,on,force", NULL, 3, ID2P(LANG_SET_BOOL_NO),
                   ID2P(LANG_SET_BOOL_YES), ID2P(LANG_FORCE)),
    OFFON_SETTING(F_BANFROMQS,cuesheet,LANG_CUESHEET_ENABLE,false,"cuesheet support",
                  NULL),
    TABLE_SETTING_LIST(F_TIME_SETTING | F_ALLOW_ARBITRARY_VALS, skip_length,
                  LANG_SKIP_LENGTH, 0, "skip length",
                  "outro,track",
                  UNIT_SEC, formatter_time_unit_0_is_skip_track,
                  getlang_time_unit_0_is_skip_track, NULL,
                  25, timeout_sec_common),
    CHOICE_SETTING(F_CB_ON_SELECT_ONLY, start_in_screen, LANG_START_SCREEN, 1,
                   "start in screen", "previous,root,files,"
#define START_DB_COUNT 1
                   "db,"
                   "wps,menu,"
                   "bookmarks"
                   , NULL,
    (6 + START_DB_COUNT),
                   ID2P(LANG_PREVIOUS_SCREEN), ID2P(LANG_MAIN_MENU),
                   ID2P(LANG_DIR_BROWSER),
                   ID2P(LANG_MUSIC_BROWSER),
                   ID2P(LANG_RESUME_PLAYBACK), ID2P(LANG_SETTINGS),
                   ID2P(LANG_BOOKMARK_MENU_RECENT_BOOKMARKS)
                  ),
    CHOICE_SETTING(0, wps_select_action, LANG_WPS_SELECT_ACTION, 0,
                   "wps select action",
                   "default,database,coverflow,files,lyrics",
                   NULL, 5,
                   ID2P(LANG_PREVIOUS_SCREEN),
                   ID2P(LANG_TAGCACHE),
                   ID2P(LANG_COVERFLOW),
                   ID2P(LANG_DIR_BROWSER),
                   ID2P(LANG_LYRICS)),

    /* Customizable icons */
    TEXT_SETTING(F_THEMESETTING|F_NEEDAPPLY, icon_file, "iconset", DEFAULT_ICONSET,
                     ICON_DIR "/", ".bmp"),
    TEXT_SETTING(F_THEMESETTING|F_NEEDAPPLY, viewers_icon_file, "viewers iconset",
                     DEFAULT_VIEWERS_ICONSET,
                     ICON_DIR "/", ".bmp"),
    TEXT_SETTING(F_THEMESETTING|F_NEEDAPPLY, colors_file, "filetype colours", "-",
                     THEME_DIR "/", ".colours"),
    /* Off unless a theme asks for it: it repaints in colours taken from the
     * album art, which a skin not written for it has no reason to expect.
     * F_THEMERESET so loading such a skin turns it back off. */
    OFFON_SETTING(F_THEMERESET, dynamic_colors, LANG_DYNAMIC_COLORS, false,
                  "dynamic colors", NULL),
    /* Re-buffers the artwork on change, so the now-playing screen switches
     * picture without waiting for the next track. Dynamic colours need no
     * part in this: they are extracted from whatever bitmap was buffered. */
    CHOICE_SETTING(F_CB_ON_SELECT_ONLY|F_CB_ONLY_IF_CHANGED, wps_art_source,
                   LANG_WPS_ART_SOURCE, WPS_ART_ALBUM, "wps art source",
                   "album,artist,auto", wps_art_source_callback, 3,
                   ID2P(LANG_WPS_ART_ALBUM), ID2P(LANG_WPS_ART_ARTIST),
                   ID2P(LANG_WPS_ART_AUTO)),
    CHOICE_SETTING(F_THEMESETTING, album_covers_view_mode, LANG_CAROUSEL_VIEW_MODE,
                  0, "album covers view mode", "3d,flat", NULL, 2,
                  ID2P(LANG_CAROUSEL_VIEW_3D), ID2P(LANG_CAROUSEL_VIEW_FLAT)),
    INT_SETTING(F_THEMESETTING, album_covers_pile_fade, LANG_CAROUSEL_PILE_FADE, 0,
                "album covers pile fade", UNIT_PERCENT, 0, 100, 5,
                NULL, NULL, NULL),
    INT_SETTING(F_THEMESETTING, album_covers_pile_offset, LANG_CAROUSEL_PILE_OFFSET, 0,
                "album covers pile offset", UNIT_PIXEL, 0, 8, 1,
                NULL, NULL, NULL),
    INT_SETTING(F_THEMESETTING, album_covers_center_margin, LANG_CENTRE_MARGIN, 0,
                "album covers center margin", UNIT_INT, 0, 80, 1,
                NULL, NULL, NULL),
    INT_SETTING(F_THEMESETTING, album_covers_slide_tuck, LANG_NUMBER_OF_SLIDES, 32,
                "album covers slide tuck", UNIT_INT, 0, 64, 1,
                NULL, NULL, NULL),
    OFFON_SETTING(F_THEMESETTING, album_covers_parallel_slides, LANG_SPACING, true,
                  "album covers parallel slides", NULL),
    /* Diagnostics: append what the two background workers are doing to
     * .rockbox/tagcache.log and .rockbox/artcache.log. Both drive the same
     * "Building" indicator, so when it will not go away these say which. */
    OFFON_SETTING(0, debug_log_tagcache, LANG_DEBUG_LOG, false,
                  "debug log tagcache", NULL),
    OFFON_SETTING(0, debug_log_artcache, LANG_DEBUG_LOG, false,
                  "debug log artcache", NULL),
    INT_SETTING(0, album_covers_scroll_speed, LANG_SCROLL_SPEED, 175,
                "album covers scroll speed", UNIT_PERCENT, 100, 400, 25,
                NULL, NULL, NULL),
    INT_SETTING(0, album_covers_transition_speed,
                LANG_CAROUSEL_TRANSITION_SPEED, 325,
                "album covers transition speed", UNIT_PERCENT, 100, 400, 25,
                NULL, NULL, NULL),
    CHOICE_SETTING(F_THEMESETTING, album_covers_show_album_name, LANG_SHOW_ALBUM_TITLE,
                  4, "album covers show album name",
                  "hide,bottom,top,both top,both bottom", NULL, 5,
                  ID2P(LANG_HIDE_ALBUM_TITLE_NEW), ID2P(LANG_SHOW_AT_THE_BOTTOM_NEW),
                  ID2P(LANG_SHOW_AT_THE_TOP_NEW), ID2P(LANG_SHOW_ALL_AT_THE_TOP),
                  ID2P(LANG_SHOW_ALL_AT_THE_BOTTOM)),
    CHOICE_SETTING(0, album_covers_on_select, LANG_ON_ALBUM_SELECT,
                  0, "album covers on select",
                  "show tracks,play album", NULL, 2,
                  ID2P(LANG_SHOW_TRACKS), ID2P(LANG_PLAY_ALBUM)),
    CHOICE_SETTING(0, album_covers_sort_albums_by, LANG_SORT_ALBUMS_BY,
                  0, "album covers sort albums by",
                  "artist+name,artist+year,year,name", NULL, 4,
                  ID2P(LANG_ARTIST_PLUS_NAME), ID2P(LANG_ARTIST_PLUS_YEAR),
                  ID2P(LANG_ID3_YEAR), ID2P(LANG_NAME)),
    /* Which Music menu rows are turned off, and the row set that was chosen
     * against. Never shown as settings themselves -- the screen in
     * screens/music_menu_config.c is the UI -- so the lang ids here are only
     * there because the table wants one. */
    INT_SETTING(F_BANFROMQS, music_menu_hidden, LANG_MUSIC_BROWSER, 0,
                "music menu hidden", UNIT_INT, 0, 0x7fffffff, 1,
                NULL, NULL, NULL),
    INT_SETTING(F_BANFROMQS, music_menu_sig, LANG_MUSIC_BROWSER, 0,
                "music menu signature", UNIT_INT, 0, 0x7fffffff, 1,
                NULL, NULL, NULL),
    /* Guest credits, mined out of the title and per-track artist tags. Off by
     * default: reading them is a guess at what a tag meant, and a library
     * whose owner did not ask for it should not grow rows out of one. Off,
     * db_featured builds nothing at all -- this is the gate that saves the
     * crawl, not one that only hides its result. */
    OFFON_SETTING(F_BANFROMQS, featured_artists, LANG_FEATURED_ARTISTS, false,
                  "featured artists", NULL),
    /* The database browser's own album ordering. Separate from the carousel's
     * above: that one groups by artist as well, which a browser list has
     * already done by navigation. */
    CHOICE_SETTING(0, database_sort_albums_by, LANG_OTHER_LISTS,
                  0, "database sort albums by",
                  "name,year,year descending", NULL, 3,
                  ID2P(LANG_NAME), ID2P(LANG_SORT_BY_YEAR_ASC),
                  ID2P(LANG_SORT_BY_YEAR_DESC)),
    /* The per-context overrides of the line above, two bits each. Never shown
     * as a setting itself -- the rows in general_settings.c are the UI -- so
     * the lang id here is only there because the table wants one. */
    INT_SETTING(F_BANFROMQS, database_album_sort_ctx, LANG_SORT_ALBUMS_BY, 0,
                "database album sort contexts", UNIT_INT, 0, 0xff, 1,
                NULL, NULL, NULL),
    CHOICE_SETTING(0, album_covers_sort_artists_by, LANG_SORT_ARTISTS_BY,
                  0, "album covers sort artists by", "name,most played", NULL, 2,
                  ID2P(LANG_NAME), ID2P(LANG_MOST_PLAYED_ARTISTS)),
    CHOICE_SETTING(0, album_covers_year_sort_order, LANG_YEAR_SORT_ORDER,
                  0, "album covers year sort order", "ascending,descending",
                  NULL, 2, ID2P(LANG_ASCENDING), ID2P(LANG_DESCENDING)),
    OFFON_SETTING(F_THEMESETTING, album_covers_show_year, LANG_SHOW_YEAR_IN_ALBUM_TITLE,
                  false, "album covers show year", NULL),
#define CAROUSEL_FILTER_CHOICES \
                   ID2P(LANG_OFF),               ID2P(LANG_FILTER_BW),  \
                   ID2P(LANG_FILTER_INVERT),     ID2P(LANG_FILTER_BRIGHTER), \
                   ID2P(LANG_FILTER_DARKER),     ID2P(LANG_FILTER_CONTRAST_UP), \
                   ID2P(LANG_FILTER_CONTRAST_DOWN), ID2P(LANG_FILTER_COLOUR_UP), \
                   ID2P(LANG_FILTER_COLOUR_DOWN), ID2P(LANG_FILTER_HUE), \
                   ID2P(LANG_FILTER_POSTERISE),  ID2P(LANG_DITHERING),   \
                   ID2P(LANG_FILTER_PIXELLATE)
    CHOICE_SETTING(F_THEMESETTING|F_THEMERESET, album_covers_filter[0],
                   LANG_ARTWORK_FILTER_1, 0, "album covers filter 1",
                   CAROUSEL_FILTER_CFG_VALS, NULL,
                   CAROUSEL_FILTER_COUNT, CAROUSEL_FILTER_CHOICES),
    CHOICE_SETTING(F_THEMESETTING|F_THEMERESET, album_covers_filter[1],
                   LANG_ARTWORK_FILTER_2, 0, "album covers filter 2",
                   CAROUSEL_FILTER_CFG_VALS, NULL,
                   CAROUSEL_FILTER_COUNT, CAROUSEL_FILTER_CHOICES),
    CHOICE_SETTING(F_THEMESETTING|F_THEMERESET, album_covers_filter[2],
                   LANG_ARTWORK_FILTER_3, 0, "album covers filter 3",
                   CAROUSEL_FILTER_CFG_VALS, NULL,
                   CAROUSEL_FILTER_COUNT, CAROUSEL_FILTER_CHOICES),
#undef CAROUSEL_FILTER_CHOICES
    /* Defaults for a theme that says nothing, not the shipped look. Few
     * status bars span the full width, and one that does not sits over a
     * screen that does as a gap rather than a bar.
     *
     * F_THEMERESET is what makes that true. Without it these keep the last
     * theme's answer, so a theme saying nothing inherits a reserved status bar
     * strip it never draws into, and the carousel runs with a gap along the
     * top. */
    CHOICE_SETTING(F_THEMESETTING|F_THEMERESET, album_covers_background,
                  LANG_CAROUSEL_BACKGROUND,
                  1, "album covers background",
                  "foreground,background", NULL, 2,
                  ID2P(LANG_FOREGROUND_COLOR), ID2P(LANG_BACKGROUND_COLOR)),
    OFFON_SETTING(F_THEMESETTING|F_THEMERESET, album_covers_statusbar,
                  LANG_STATUS_BAR,
                  false, "album covers statusbar", NULL),
    /* Config-file only (lang_id -1, no menu entry): a theme sets these. Off
     * unless asked for, because they make album rows grow to the tall height
     * to fit a cover -- a theme whose list config doesn't draw the %La cover
     * gets the tall rows with nothing in them. F_THEMERESET so a theme that
     * says nothing gets them off rather than inheriting them from the last
     * theme loaded. */
    OFFON_SETTING(F_THEMESETTING|F_THEMERESET, db_albumart, LANG_DB_ALBUM_ART,
                  false, "database album art", NULL),
    OFFON_SETTING(F_THEMESETTING|F_THEMERESET, db_artistart, LANG_DB_ARTIST_ART,
                  false, "database artist art", NULL),
    OFFON_SETTING(F_THEMESETTING|F_THEMERESET, db_bookart, LANG_DB_BOOK_ART,
                  false, "database audiobook art", NULL),
    OFFON_SETTING(F_BANFROMQS, segregate_audiobooks, LANG_SEGREGATE_AUDIOBOOKS,
                  false, "segregate audiobooks", segregate_audiobooks_callback),
    {F_T_INT|F_THEMESETTING, &global_settings.db_art_row_height, -1,
        INT(52), "database art row height", UNUSED},
    OFFON_SETTING(0, art_cache_fast_build, LANG_ART_CACHE_FAST_BUILD, false,
                  "art cache fast build", NULL),
    /* keyclick */
    CHOICE_SETTING(0, keyclick, LANG_KEYCLICK_SOFTWARE, 0,
                   "keyclick", "off,weak,moderate,strong", NULL, 4,
                   ID2P(LANG_OFF), ID2P(LANG_WEAK), ID2P(LANG_MODERATE),
                   ID2P(LANG_STRONG)),
    OFFON_SETTING(0, keyclick_repeats, LANG_KEYCLICK_REPEATS, false,
                  "keyclick repeats", NULL),
    OFFON_SETTING(0, keyclick_hardware, LANG_KEYCLICK_HARDWARE, true,
        "hardware keyclick", NULL),
    TEXT_SETTING(0, playlist_catalog_dir, "playlist catalog directory",
                     PLAYLIST_CATALOG_DEFAULT_DIR, NULL, NULL),
    INT_SETTING(F_TIME_SETTING, sleeptimer_duration, LANG_SLEEP_TIMER_DURATION,
                30, "sleeptimer duration", UNIT_MIN, 5, 300, 5,
                NULL, NULL, NULL),
    OFFON_SETTING(0, sleeptimer_on_startup, LANG_SLEEP_TIMER_ON_POWER_UP, false,
                  "sleeptimer on startup", NULL),
    OFFON_SETTING(0, keypress_restarts_sleeptimer, LANG_KEYPRESS_RESTARTS_SLEEP_TIMER, false,
                  "keypress restarts sleeptimer", set_keypress_restarts_sleep_timer),

    OFFON_SETTING(0, show_shutdown_message, LANG_SHOW_SHUTDOWN_MESSAGE, true,
                  "show shutdown message", NULL),



   CUSTOM_SETTING(0, qs_items[QUICKSCREEN_TOP], LANG_TOP_QS_ITEM,
                  NULL, "qs top",
                  qs_load_from_cfg, qs_write_to_cfg,
                  qs_is_changed, qs_set_default),
   CUSTOM_SETTING(0, qs_items[QUICKSCREEN_LEFT], LANG_LEFT_QS_ITEM,
                  &global_settings.playlist_shuffle, "qs left",
                  qs_load_from_cfg, qs_write_to_cfg,
                  qs_is_changed, qs_set_default),
   CUSTOM_SETTING(0, qs_items[QUICKSCREEN_RIGHT], LANG_RIGHT_QS_ITEM,
                  &global_settings.repeat_mode, "qs right",
                  qs_load_from_cfg, qs_write_to_cfg,
                  qs_is_changed, qs_set_default),
   CUSTOM_SETTING(0, qs_items[QUICKSCREEN_BOTTOM], LANG_BOTTOM_QS_ITEM,
                  NULL, "qs bottom",
                  qs_load_from_cfg, qs_write_to_cfg,
                  qs_is_changed, qs_set_default),
   /* What the long press opens. Named for the thing being chosen rather than
      the override, so the two options read as alternatives; the cfg name and
      its off/on values are unchanged. */
   BOOL_SETTING(0, shortcuts_replaces_qs, LANG_USE_SHORTCUTS_INSTEAD_OF_QS,
                  false, "shortcuts instead of quickscreen", off_on,
                  LANG_SHORTCUTS_INSTEAD, LANG_ON, NULL),
    OFFON_SETTING(0, prevent_skip, LANG_PREVENT_SKIPPING, false, "prevent track skip", NULL),
    OFFON_SETTING(0, rewind_across_tracks, LANG_REWIND_ACROSS_TRACKS, false, "rewind across tracks", NULL),
    OFFON_SETTING(0, usb_hid, LANG_USB_HID, false, "usb hid", usb_set_hid),
    CHOICE_SETTING(0, usb_keypad_mode, LANG_USB_KEYPAD_MODE, 0,
            "usb keypad mode", "multimedia,presentation,browser"
            ",mouse"
            , NULL,
            4,
            ID2P(LANG_MULTIMEDIA_MODE), ID2P(LANG_PRESENTATION_MODE),
            ID2P(LANG_BROWSER_MODE)
            , ID2P(LANG_MOUSE_MODE)
    ), /* CHOICE_SETTING( usb_keypad_mode ) */

#ifdef USB_ENABLE_AUDIO
    CHOICE_SETTING(0, usb_audio, LANG_USB_DAC, 0, "usb-dac", "never,always,while_charge_only,while_mass_storage", usb_set_audio, 4,
        ID2P(LANG_NEVER), ID2P(LANG_ALWAYS), ID2P(LANG_WHILE_USB_CHARGE_ONLY), ID2P(LANG_WHILE_MASS_STORAGE_USB_ONLY)),
#endif



    /* Customizable list */
    VIEWPORT_SETTING(ui_vp_config, "ui viewport"),

    CUSTOM_SETTING(0, context_wps,
                   LANG_HOTKEY_WPS, /* lang string here is never actually used */
                   &wps_context_menu_default, "context_wps",
                   wps_context_menu_load_from_cfg, wps_context_menu_write_to_cfg,
                   wps_context_menu_is_changed, wps_context_menu_set_default),
    CUSTOM_SETTING(0, hotkey_tree,
                   LANG_HOTKEY_FILE_BROWSER, /* lang string here is never actually used */
                   &tree_hotkey_default, "hotkey tree",
                   wps_context_menu_load_from_cfg, wps_context_menu_write_to_cfg,
                   wps_context_menu_is_changed, wps_context_menu_set_default),

    INT_SETTING(F_TIME_SETTING, resume_rewind, LANG_RESUME_REWIND, 0,
                "resume rewind", UNIT_SEC, 0, 60, 5,
                formatter_time_unit_0_is_off, getlang_time_unit_0_is_off, NULL),
   CUSTOM_SETTING(0, root_menu_customized,
                  LANG_ROCKBOX_TITLE, /* lang string here is never actually used */
                  NULL, "root menu order",
                  root_menu_load_from_cfg, root_menu_write_to_cfg,
                  root_menu_is_changed, root_menu_set_default),

    CHOICE_SETTING(0,
                   usb_mode,
                   LANG_USB_MODE,
                   USBMODE_DEFAULT,
                   "usb mode",
                   "mass storage,charge"
                   ,
                   usb_set_mode,
                   2,
                   ID2P(LANG_USB_MODE_MASS_STORAGE),
                   ID2P(LANG_USB_MODE_CHARGE)
        ),
    OFFON_SETTING(0, clear_settings_on_hold, LANG_CLEAR_SETTINGS_ON_HOLD,
                  false, "clear settings on hold", NULL),
    CHOICE_SETTING(0, playback_log, LANG_LOGGING, 1, "play log",
                   "off,on,last.fm", NULL, 3,
                   ID2P(LANG_OFF), ID2P(LANG_ON), ID2P(LANG_AUDIOSCROBBLER)),
};

const int nb_settings = sizeof(settings)/sizeof(*settings);

const struct settings_list* get_settings_list(int*count)
{
    *count = nb_settings;
    return settings;
}
