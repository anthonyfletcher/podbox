/***************************************************************************
 * Original code from RockBox
 * was: apps/settings.c
 * Copyright (C) 2002 by Stuart Martin
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Loads, saves and applies settings: the config file format, reading and
 * writing config.cfg, and pushing changed values into the running system.
 *
 * Settings live in one global struct, global_settings. This file moves values
 * between that struct, the config file, and the hardware -- it does not
 * define what a setting is; settings_list.c does, and everything here works
 * by walking that table.
 *
 * Storing a value and acting on it are separate steps. Writing to
 * global_settings changes nothing audible or visible until the matching
 * apply function runs, which is why load and reset both end by applying
 * everything.
 *
 * Parts, in order:
 *   - config file parsing: one line at a time, matched against the table
 *   - writing config.cfg, and the "changed from default" test that decides
 *     what gets written
 *   - loading and saving, including the .cfg-file-in-the-browser path
 *   - reset to defaults
 *   - the apply functions: sound, display, language, and settings_apply()
 ****************************************************************************/
/* Define LOGF_ENABLE to enable logf output in this file */
/*#define LOGF_ENABLE*/
/*Define DEBUG_AVAIL_SETTINGS to get a list of all available settings and flags */
/*#define DEBUG_AVAIL_SETTINGS*/ /* Needs (LOGF_ENABLE) */
#include "logf.h"

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <limits.h>
#include "inttypes.h"
#include "config.h"
#include "rbpaths.h"
#include "kernel.h"
#include "input/action.h"
#include "crc32.h"
#include "sound.h"
#include "settings.h"
#include "debug.h"
#include "usb.h"
#include "backlight.h"
#include "audio.h"
#include "speech/lang_override.h"
#include "speech/talk.h"
#include "string-extra.h"
#include "rtc.h"
#include "power.h"
#include "ata_idle_notify.h"
#include "storage.h"
#include "ctype.h"
#include "file.h"
#include "system.h"
#include "general.h"
#include "system/app_util.h"
#include "draw/color.h"
#include "system/strutil.h"
#include "system/volume.h"
#include "draw/icon_bitmaps.h"
#include "font.h"
#include "audio/peak_meter.h"
#include "lang.h"
#include "speech/language.h"
#include "powermgmt.h"
#include "widgets/keyboard.h"
#include "version.h"
#include "rbunicode.h"
#include "dircache.h"
#include "widgets/splash.h"
#include "widgets/list.h"
#include "settings_list.h"
#include "files/filetypes.h"
#include "widgets/option_select.h"
#include "screens/playback/wps.h"
#include "skin/skin_engine.h"
#include "draw/viewport.h"
#include "skin/statusbar_skinned.h"
#include "bootchart.h"
#include "scroll_engine.h"
#include "widgets/dialog.h"

struct user_settings global_settings;
struct system_status global_status;
static uint32_t user_settings_crc;
static long next_status_update_tick;
static long lasttime = 0;

/* flush system_status more often for spinning harddisks as we may not be able
 * to spin up at shutdown in order to save so keep the gap minimal */
#define SYSTEM_STATUS_UPDATE_TICKS (HZ * 60 * 5) /* flush every 5 minutes */

#include "dsp_proc_settings.h"
#include "audio/playback.h"
#include "metadata/art_cache.h"        /* art_filter_set */
#include "screens/browse/browser.h"    /* browser_albumart_invalidate */
#include "pcm_sampr.h"

#ifdef LOGF_ENABLE
static char *debug_get_flags(uint32_t flags);
#undef DEBUGF /* allow DEBUGF or logf not both */
#define DEBUGF(...) do { } while(0)
#endif


#ifdef ROCKBOX_NO_TEMP_SETTINGS_FILE /* Overwrites same file each time */
#define CONFIGFILE_TEMP CONFIGFILE
#define RESUMEFILE_TEMP RESUMEFILE
#define rename_temp_file(a,b,c)
#else /* creates temp files on save, renames next load, saves old file if desired */
#define CONFIGFILE_TEMP CONFIGFILE".new"
#define RESUMEFILE_TEMP RESUMEFILE".new"

static void debug_available_settings(void);

static void rename_temp_file(const char *tempfile,
                            const char *file,
                            const char *oldfile)
{
    /* if tempfile does not exist -- Return
     * if oldfile is supplied     -- Rename file to oldfile
     * if tempfile does exist     -- Rename tempfile to file
    */
    if (file_exists(tempfile))
    {
        if (oldfile != NULL && file_exists(file))
            rename(file, oldfile);
        rename(tempfile, file);
    }
}
#endif /* ndef ROCKBOX_NO_TEMP_SETTINGS_FILE */

const char* setting_get_cfgvals(const struct settings_list *setting)
{
    if ((setting->flags & F_TABLE_SETTING) == F_TABLE_SETTING)
        return setting->table_setting->cfg_vals;
    else if ((setting->flags & F_CHOICE_SETTING) == F_CHOICE_SETTING)
        return setting->choice_setting->cfg_vals;
    else if ((setting->flags & F_BOOL_SETTING) == F_BOOL_SETTING)
    {
        logf("Setting: %s", setting->cfg_name);
        return setting->bool_setting->cfg_vals;
    }
    else if ((setting->flags & F_HAS_CFGVALS) == F_HAS_CFGVALS)
        return setting->cfg_vals;
    return NULL;
}

/* Longest configuration line, and the longest value within one.
 *
 * Both the reader and the writer used fixed buffers that truncate rather than
 * fail, and neither rejects a truncated line -- it simply parses as a shorter
 * value, losing whatever fell off the end. "root menu order" is the line that
 * outgrew them: every enabled main-menu entry contributes its key, so it grows
 * with the menu. At the old 128-byte read buffer the last entries vanished from
 * the menu while the configuration on disk still named them; at the writer's
 * MAX_PATH they would have been dropped on the way out instead.
 *
 * Sized from the ceiling the writer already enforces rather than from what a
 * menu happens to hold today. main_menu_config.c caps its own output at
 * CFG_STR_SIZE -- MAX_ITEMS keys of at most 13 bytes, currently 572 -- so 589
 * with the setting name is the most that line can be, however many entries are
 * added later. A fully enabled menu as it stands (13 fixed entries and all 20
 * tagnavi slots) is 338, so there is roughly double the headroom.
 *
 * Deliberately one constant for reading and writing. Sizing them differently is
 * how a value gets written that cannot be read back, which is the more
 * confusing half of this bug: the configuration on disk names entries that do
 * not appear. */
#define SETTINGS_MAX_LINE 640

/* The write path's scratch line, static rather than on the stack.
 *
 * Saving happens from flush_config_block_callback(), a storage-idle callback,
 * which runs on the storage thread -- and that thread has ATA_THREAD_STACK_SIZE,
 * two kilobytes. A buffer this size is a third of it in a single frame, before
 * the file I/O underneath gets its own. The reader keeps its buffer on the
 * stack because it runs from the main thread at startup and from the UI thread
 * for a theme, neither of which is short of room.
 *
 * Shared by the two functions below, which the callback calls one after the
 * other rather than nested. Both write the same file, so anything calling them
 * concurrently is already broken for a larger reason. */
static char settings_line[SETTINGS_MAX_LINE];

/* calculates and stores crc of settings, returns true if settings have changed */
static bool settings_crc_changed(void)
{
    char *value = settings_line;
    uint32_t custom_crc = 0xFFFFFFFF;
    for(int i=0; i<nb_settings; i++)
    {
        const struct settings_list *setting = &settings[i];
        if (!(setting->flags & F_CUSTOM_SETTING))
            continue;
        cfg_to_string(setting, value, SETTINGS_MAX_LINE);
        custom_crc = crc_32(value, strlen(value), custom_crc);
    }

    uint32_t crc = crc_32(&global_settings, sizeof(global_settings), custom_crc);
    if (crc != user_settings_crc)
    {
        user_settings_crc = crc;
        return true;
    }

    return false;
}

/** Reading from a config file **/

static bool settings_write_config(const char* filename, int options);

/*
 * load settings from disk
 */
void settings_load(void)
{
    logf("\r\n%s()\r\n", __func__);
    debug_available_settings();

    /* make temp files current make current files .old */
    rename_temp_file(RESUMEFILE_TEMP, RESUMEFILE, RESUMEFILE".old");
    rename_temp_file(CONFIGFILE_TEMP, CONFIGFILE, CONFIGFILE".old");

    /* First boot: with no settings of the user's own, the build's defaults
     * stand in and are written out as the user's, which is what stops them
     * being consulted again. Writing now rather than leaving it to the next
     * save is load-bearing -- a save records only what differs from the
     * compiled defaults, so a setting the user moves back to its compiled
     * value would be missing from config.cfg and set again from here on every
     * boot. This has to sit after the renames above, or the boot following the
     * first save finds config.cfg missing and overwrites a full set of the
     * user's settings with the shipped ones. */
    if (!file_exists(CONFIGFILE) &&
        settings_load_config(DEFAULTCONFIGFILE, false))
    {
        settings_write_config(CONFIGFILE, SETTINGS_SAVE_CHANGED);
    }
    else
        settings_load_config(CONFIGFILE, false); /* load user_settings items */

    settings_load_config(RESUMEFILE, false); /* load system_status items */

    /* fixed settings file has final say on user_settings AND system_status items */
    settings_load_config(FIXEDSETTINGSFILE, false);

    /* set initial CRC value - settings_save checks, if changed writes to disk */
    settings_crc_changed();
}

bool cfg_string_to_int(const struct settings_list *setting, int* out, const char* str)
{
    const char* ptr = setting_get_cfgvals(setting);
    size_t len = strlen(str);
    int index = 0;

    while (ptr)
    {
        if (!strncmp(ptr, str, len))
        {
            ptr += len;
            /* if the next character is not a comma or end of string,
             * it means the comparison was only a partial match. */
            if (*ptr == ',' || *ptr == '\0')
            {
                *out = index;
                return true;
            }
        }

        while (*ptr != ',')
        {
            if (!*ptr)
                return false;
            ptr++;
        }

        ptr++;
        index++;
    }
    logf("%s() bad setting\n", __func__);
    return false;
}

/**
 * Copy an input string to an output buffer, stripping the prefix and
 * suffix listed in the filename setting. Returns false if the output
 * string does not fit in the buffer or is longer than the setting's
 * max_len, and the output buffer will not be modified.
 *
 * Returns true if the setting was copied successfully. The input and
 * output buffers are allowed to alias.
 */
bool copy_filename_setting(char *buf, size_t buflen, const char *input,
                           const struct filename_setting *fs)
{
    size_t input_len = strlen(input);
    size_t len;

    if (fs->prefix)
    {
        len = strlen(fs->prefix);
        if (len <= input_len && !strncasecmp(input, fs->prefix, len))
        {
            input += len;
            input_len -= len;
        }
    }

    if (fs->suffix)
    {
        len = strlen(fs->suffix);
        if (len <= input_len &&
            !strcasecmp(input + input_len - len, fs->suffix))
        {
            input_len -= len;
        }
    }

    /* Make sure it fits the output buffer and repsects the setting's max_len.
     * Note that max_len is a buffer size and thus includes a null terminator */
    if (input_len >= (size_t)fs->max_len || input_len >= buflen)
        return false;

    /* Copy what remains into buf - use memmove in case of aliasing */
    memmove(buf, input, input_len);
    buf[input_len] = '\0';
    return true;
}

bool string_to_cfg(const char *name, char* value, bool *theme_changed)
{
    const struct settings_list *setting = find_setting_by_cfgname(name);
    if (!setting)
        return false;

    uint32_t flags = setting->flags;

    if (flags & F_THEMESETTING)
        *theme_changed = true;

    switch (flags & F_T_MASK)
    {
    case F_T_CUSTOM:
        setting->custom_setting->load_from_cfg(setting->setting, value);
        logf("Val: %s\r\n",value);
        break;
    case F_T_INT:
    case F_T_UINT:
        if (flags & F_RGB)
        {
            hex_to_rgb(value, (int*)setting->setting);
            logf("Val: %s\r\n", value);
        }
        else
            if (setting_get_cfgvals(setting) == NULL)
            {
                *(int*)setting->setting = atoi(value);
                logf("Val: %s\r\n",value);
            }
            else
            {
                int temp, *v = (int*)setting->setting;
                bool found = cfg_string_to_int(setting, &temp, value);
                if (found)
                {
                    if (flags & F_TABLE_SETTING)
                        *v = setting->table_setting->values[temp];
                    else
                        *v = temp;
                    logf("Val: %d\r\n", *v);
                }
                else if (flags & F_ALLOW_ARBITRARY_VALS)
                {
                    *v = atoi(value);
                    logf("Val: %s = %d\r\n", value, *v);
                }
                else if (flags & F_TABLE_SETTING)
                {
                    const struct table_setting *info = setting->table_setting;
                    temp = atoi(value);
                    *v = setting->default_val.int_;
                    if (info->values)
                    {
                        for(int i = 0; i < info->count; i++)
                        {
                            if (info->values[i] == temp)
                            {
                                *v = temp;
                                break;
                            }
                        }
                    }
                    logf("Val: %s", *v == temp ? "Found":"Error Not Found");
                    logf("Val: %s = %d\r\n", value, *v);
                }

                else
                {
                    logf("Error: %s: Not Found! [%s]\r\n",
                                      setting->cfg_name, value);
                    return false;
                }
            }
        break;
    case F_T_BOOL:
    {
        int temp;
        if (cfg_string_to_int(setting, &temp, value))
        {
            *(bool*)setting->setting = !!temp;
            logf("Val: %s\r\n", value);
        }
        if (setting->bool_setting->option_callback)
        {
            setting->bool_setting->option_callback(!!temp);
        }
        break;
    }
    /* these can be plain text, filenames, or dirnames */
    case F_T_CHARPTR:
    case F_T_UCHARPTR:
    {
        const struct filename_setting *fs = setting->filename_setting;
        copy_filename_setting((char*)setting->setting,
                              fs->max_len, value, fs);
        logf("Val: %s\r\n", value);
        break;
    }
    }
    return true;
}

/* True if the open config file names a font, which is what separates a whole
 * theme from a .cfg that only patches something.
 *
 * A theme gets the reset: every setting describing the look goes back to its
 * default before the file is read, so the theme cannot inherit the last one's
 * decisions. A patch must not, or "change my icons" -- themes/rockbox_default_
 * icons.cfg is two lines long -- would take the wps, the sbs, the font and
 * every colour with it.
 *
 * The font is the discriminator because a whole look has an opinion about its
 * text, and nothing else in a .cfg reliably separates the two. `font: -` counts:
 * it says "no font", which is a decision, and upstream's failsafe theme relies
 * on exactly that.
 *
 * Takes the caller's line buffer rather than another 640 bytes of stack, and
 * leaves the read position at wherever it stopped; the caller rewinds. */
static bool config_is_theme(int fd, char *line, int line_size)
{
    while (read_line(fd, line, line_size) > 0)
    {
        char *name, *value;
        const struct settings_list *setting;

        if (!settings_parseline(line, &name, &value))
            continue;

        setting = find_setting_by_cfgname(name);
        if (setting && setting->setting == global_settings.font_file)
            return true;
    }
    return false;
}

/** Theme-local appearance overrides **/

/* Every setting a theme load resets before reading the theme, and so also
 * every setting the overlay is allowed to carry.
 *
 * The union of the two flags, not either alone. F_THEMESETTING is the honest
 * description of "describes the look" and is what the wider reset is for -- a
 * theme that names no iconset should not inherit the last one's. But
 * dynamic_colors carries F_THEMERESET on its own, without F_THEMESETTING, so
 * testing the tidier flag alone would quietly stop it resetting. */
#define F_THEME_RESET_MASK (F_THEMESETTING | F_THEMERESET)

/* Which settings the user has changed by hand under the current theme. A
 * pointer list rather than a bit per setting because the count is tiny -- a
 * handful of deliberate choices, not a configuration -- and a list needs no
 * compile-time bound on nb_settings.
 *
 * Not persisted. The overlay file is the record: reading it back at theme-load
 * time re-marks everything in it, so a reboot loses nothing and no baseline
 * has to be reconstructed. */
#define MAX_TWEAKS 64
static const struct settings_list *tweaked[MAX_TWEAKS];
static int tweak_count;

/* Where the current theme's overlay lives: beside the theme, named after it,
 * and deliberately *not* a .cfg.
 *
 * The theme browser lists every .cfg in this directory, so an overlay called
 * "Themify_2.user.cfg" appears in it as a theme named "Themify_2.user" --
 * loading which would take the reset path, apply only the handful of settings
 * the overlay holds, and then record itself as the current theme, so the next
 * tweak would write "Themify_2.user.user.cfg". An extension the browser does
 * not recognise keeps the file paired with its theme and out of the list.
 *
 * The contents are an ordinary config file and are read as one. */
static bool theme_overlay_path(char *buf, size_t bufsz)
{
    if (!global_settings.theme_file[0])
        return false;
    snprintf(buf, bufsz, THEME_DIR "/%s.usercfg",
             global_settings.theme_file);
    return true;
}

/* Record without writing. Kept separate from the public call because reading
 * the overlay marks everything in it, and writing from there would truncate
 * the file being read. */
static bool mark_tweak(const struct settings_list *setting)
{
    /* Only what a theme load would otherwise discard -- see settings.h. */
    if (!setting || !(setting->flags & F_THEME_RESET_MASK))
        return false;

    for (int i = 0; i < tweak_count; i++)
        if (tweaked[i] == setting)
            return true;

    if (tweak_count >= MAX_TWEAKS)
        return false;

    tweaked[tweak_count++] = setting;
    return true;
}

static void write_theme_overlay(void)
{
    char path[MAX_PATH];

    if (theme_overlay_path(path, sizeof path))
        settings_write_config(path, SETTINGS_SAVE_THEME_OVERLAY);
}

void settings_mark_user_tweak(const struct settings_list *setting)
{
    /* Rewritten whole on every tweak, including one already marked -- the
     * value has changed even when the marking has not. That is a disk write
     * per appearance change, but every path that gets here has just called
     * settings_save() for config.cfg, so the disk is already awake and this
     * costs no spin-up of its own. */
    if (mark_tweak(setting))
        write_theme_overlay();
}

/* The theme name, from the path of the .cfg that described it. */
static void set_theme_file(const char *file)
{
    const char *base = strrchr(file, '/');
    char *dot;

    base = base ? base + 1 : file;
    strlcpy((char*)global_settings.theme_file, base,
            sizeof global_settings.theme_file);

    dot = strrchr((char*)global_settings.theme_file, '.');
    if (dot)
        *dot = '\0';
}

/* Parse an open config file into the settings. Shared by a theme's own .cfg
 * and by the overlay read straight after it, so both feed the same
 * theme_changed flag -- an override that names a skin still has to reach
 * settings_apply_skins().
 *
 * `mark` re-marks every setting the file names, which is how the overlay
 * survives a reboot: the file is the record of what was tweaked. */
static void read_config_lines(int fd, char *line, int line_size,
                              bool *theme_changed, bool mark)
{
    while (read_line(fd, line, line_size) > 0)
    {
        char *name, *value;

        if (!settings_parseline(line, &name, &value))
            continue;

        /* name was not a valid setting; the old "openplugin" config line is
         * ignored now that the plugin system is gone */
        string_to_cfg(name, value, theme_changed);

        if (mark)
            mark_tweak(find_setting_by_cfgname(name));
    }
}

/* The overlay, read after the theme's own .cfg and before anything is applied.
 *
 * It cannot be a settings_load_config() call, whichever flag it is given.
 * With apply = true, the overlay names theme settings, so
 * config_is_theme() reports a theme and the F_THEMERESET loop runs
 * a second time -- wiping the theme just read. With apply = false, the apply
 * has already happened and nothing re-reads the backdrop, the colours or the
 * skins. It has to be a step inside the sequence, which is what this is. */
static void load_theme_overlay(char *line, int line_size, bool *theme_changed)
{
    char path[MAX_PATH];
    int fd;

    if (!theme_overlay_path(path, sizeof path))
        return;

    fd = open_utf8(path, O_RDONLY);
    if (fd < 0)
        return;

    read_config_lines(fd, line, line_size, theme_changed, true);
    close(fd);
}

bool settings_forget_theme_tweaks(void)
{
    char path[MAX_PATH];

    if (!theme_overlay_path(path, sizeof path) || !file_exists(path))
        return false;

    remove(path);
    tweak_count = 0;

    /* Reload the theme so the screen shows what its author shipped, rather
     * than leaving the tweaks standing until the next theme change. */
    snprintf(path, sizeof path, THEME_DIR "/%s.cfg",
             global_settings.theme_file);
    settings_load_config(path, true);

    return true;
}

bool settings_load_config(const char* file, bool apply)
{
    logf("%s()\r\n", __func__);
    int fd;
    char line[SETTINGS_MAX_LINE];
    bool theme_changed = false;
    bool is_theme = false;

    fd = open_utf8(file, O_RDONLY);
    if (fd < 0)
        return false;

    /* Only for a file the user chose to load, which is meant to describe a
     * whole look and so must not leave the previous one's settings standing.
     * The files read at startup are the opposite case -- config.cfg holds only
     * what differs from the defaults, and the fixed settings file is a partial
     * overlay on top of it, so resetting for those would discard the very
     * values being restored.
     *
     * Restricted further to files that actually describe a look, which is not
     * every .cfg the user can pick: an EQ preset is one too, and loading one
     * must not quietly drop the current theme's backdrop and colours. A theme
     * always names at least one theme setting (its wps, sbs or a colour), so
     * scan for one first and rewind. */
    if (apply)
    {
        is_theme = config_is_theme(fd, line, sizeof line);
        lseek(fd, 0, SEEK_SET); /* the scan consumed the file either way */
        if (is_theme)
        {
            for (int i = 0; i < nb_settings; i++)
            {
                if (settings[i].flags & F_THEME_RESET_MASK)
                    reset_setting(&settings[i], settings[i].setting);
            }

            /* Clear the marks with the settings they belong to. Without this,
             * the first tweak under the new theme rewrites its overlay with
             * everything still marked from the last one. */
            tweak_count = 0;

            /* Before the file is read, so a tweak made during this load knows
             * which theme it belongs to. Gated on is_theme, so an EQ preset or
             * a saved config -- FILE_ATTR_CFG fires for every .cfg the user
             * can open -- does not become "the current theme". */
            set_theme_file(file);
        }
    }

    read_config_lines(fd, line, sizeof line, &theme_changed, false);
    close(fd);

    /* The user's own changes, on top of the theme and before anything is
     * applied. */
    if (is_theme)
        load_theme_overlay(line, sizeof line, &theme_changed);

    if (apply)
    {
        /* Stop before the loading, not after it. settings_apply_skins() stops
         * playback too, but by then the fonts, the language, the iconset and
         * the colour file have all been read, and each of those allocations
         * makes buflib shrink the audio buffer -- which stops playback and
         * queues a resume (see shrink_callback() in audio/playback.c). The
         * theme change then costs a stop-and-rebuffer per allocation, every
         * one of them competing with the theme's own reads for the disk.
         *
         * Stopping first leaves those allocations nothing to interrupt. The
         * music stops either way; this only decides how long it takes. */
        if (theme_changed)
            audio_stop();

        settings_save();
        settings_apply(true);
        if (theme_changed)
            settings_apply_skins();
    }
    return true;
}

/** Writing to a config file and saving settings **/

bool cfg_int_to_string(const struct settings_list *setting, int val, char* buf, int buf_len)
{
    const char* ptr = setting_get_cfgvals(setting);
    const int *values = NULL;
    int index = 0;

    if (setting->flags & F_TABLE_SETTING)
        values = setting->table_setting->values;

    while (ptr)
    {
        if ((values && values[index] == val) ||
            (!values && index == val))
        {
            char *buf_end = buf + buf_len - 1;
            while (*ptr && *ptr != ',' && buf != buf_end)
                *buf++ = *ptr++;

            *buf++ = '\0';
            return true;
        }

        while (*ptr != ',')
        {
            if (!*ptr)
                return false;
            ptr++;
        }

        ptr++;
        index++;
    }
    logf("%s() bad setting\n", __func__);
    return false;
}

void cfg_to_string(const struct settings_list *setting, char* buf, int buf_len)
{
    switch (setting->flags & F_T_MASK)
    {
        case F_T_CUSTOM:
            setting->custom_setting->write_to_cfg(setting->setting, buf, buf_len);
            break;
        case F_T_INT:
        case F_T_UINT:
            if (setting->flags & F_RGB)
            {
                int colour = *(int*)setting->setting;
                snprintf(buf,buf_len,"%02x%02x%02x",
                            (int)RGB_UNPACK_RED(colour),
                            (int)RGB_UNPACK_GREEN(colour),
                            (int)RGB_UNPACK_BLUE(colour));
                break; /* we got a value */
            }
            else
            if (setting_get_cfgvals(setting) != NULL && cfg_int_to_string(
                setting, *(int*)setting->setting, buf, buf_len))
            {
                break; /* we got a value */
            }

            itoa_buf(buf, buf_len, *(int*)setting->setting);
            break;
        case F_T_BOOL:
            cfg_int_to_string(setting, *(bool*)setting->setting, buf, buf_len);
            break;
        case F_T_CHARPTR:
        case F_T_UCHARPTR:
        {
            char *value = setting->setting;
            const struct filename_setting *fs = setting->filename_setting;
            if (value[0] && fs->prefix)
            {
                if (value[0] == '-')
                {
                    buf[0] = '-';
                    buf[1] = '\0';
                }
                else
                {
                    snprintf(buf, buf_len, "%s%s%s",
                             fs->prefix, value, fs->suffix);
                }
            }
            else
            {
                strmemccpy(buf, value, buf_len);
            }
            break;
        }
    } /* switch () */
}


bool settings_is_changed(const struct settings_list *setting)
{
    switch (setting->flags&F_T_MASK)
    {
    case F_T_CUSTOM:
        return setting->custom_setting->is_changed(setting->setting,
                                            setting->default_val.custom);
        break;
    case F_T_INT:
    case F_T_UINT:
        if (setting->flags&F_DEF_ISFUNC)
        {
            if (*(int*)setting->setting == setting->default_val.func())
                return false;
        }
        else if (setting->flags&F_T_SOUND)
        {
            if (*(int*)setting->setting ==
                sound_default(setting->sound_setting->setting))
                return false;
        }
        else if (setting->flags & F_RESUMESETTING)
        {
            /* exclude resume settings they will get saved to '.resume.cfg' */
            return false;
        }
        else if (*(int*)setting->setting == setting->default_val.int_)
            return false;
        break;
    case F_T_BOOL:
        if (*(bool*)setting->setting == setting->default_val.bool_)
            return false;
        break;
    case F_T_CHARPTR:
    case F_T_UCHARPTR:
        if (!strcmp((char*)setting->setting, setting->default_val.charptr))
            return false;
        break;
    }
    return true;
}

static bool settings_write_config(const char* filename, int options)
{
    logf("%s\r\n", __func__);
    int i;
    int fd;
    char *value = settings_line;   /* not the stack -- see settings_line */
    fd = open(filename,O_CREAT|O_TRUNC|O_WRONLY, 0666);
    if (fd < 0)
        return false;

    if (options != SETTINGS_SAVE_RESUMEINFO)
    {
        fdprintf(fd, "# .cfg file created by rockbox %s - "
                 "http://www.rockbox.org\r\n\r\n", rbversion);
    }

    for(i=0; i<nb_settings; i++)
    {
        const struct settings_list *setting = &settings[i];
        if (!setting->cfg_name || (setting->flags & F_DEPRECATED))
            continue;

        switch (options)
        {
            case SETTINGS_SAVE_CHANGED:
                if (!settings_is_changed(setting))
                    continue;
                break;
            case SETTINGS_SAVE_SOUND:
                if (!(setting->flags & F_SOUNDSETTING))
                    continue;
                break;
            case SETTINGS_SAVE_THEME:
                if (!(setting->flags & F_THEMESETTING))
                    continue;
                break;
            case SETTINGS_SAVE_EQPRESET:
                if (!(setting->flags & F_EQSETTING))
                    continue;
                break;
            case SETTINGS_SAVE_RESUMEINFO:
                if (!(setting->flags & F_RESUMESETTING))
                    continue;
                break;
            case SETTINGS_SAVE_THEME_OVERLAY:
            {
                /* Only what the user changed by hand under this theme. */
                bool marked = false;
                for (int t = 0; t < tweak_count; t++)
                    if (tweaked[t] == setting)
                    {
                        marked = true;
                        break;
                    }
                if (!marked)
                    continue;
                break;
            }
            case SETTINGS_SAVE_ALL:
            {
                /*only save sound settings (volume) from F_RESUMESETTING items */
                uint32_t exclude_flag = (F_RESUMESETTING|F_SOUNDSETTING);
                if ((setting->flags & exclude_flag) == F_RESUMESETTING)
                    continue;
                break;
            }
        }
        cfg_to_string(setting, value, SETTINGS_MAX_LINE);
        logf("Written: '%s: %s'\r\n",setting->cfg_name, value);

        fdprintf(fd,"%s: %s\r\n",setting->cfg_name,value);
    } /* for(...) */
    close(fd);
    return true;
}

static void flush_global_status_callback(void)
{
    if (TIME_AFTER(current_tick, next_status_update_tick))
    {
        next_status_update_tick = current_tick + SYSTEM_STATUS_UPDATE_TICKS;
        update_runtime();

        DEBUGF("Writing system_status to disk\n");
        logf("Writing system_status to disk");

        settings_write_config(RESUMEFILE_TEMP, SETTINGS_SAVE_RESUMEINFO);
    }
}

static void flush_config_block_callback(void)
{
    if (settings_crc_changed())
    {
        DEBUGF("Writing changed user_settings to disk\n");
        logf("Writing changed user_settings to disk");

        if (!settings_write_config(CONFIGFILE_TEMP, SETTINGS_SAVE_CHANGED))
        {
            user_settings_crc = 0;
            DEBUGF("Error failed to write settings to disk\n");
            logf("Error failed to write settings to disk");
        }
    }
#ifdef LOGF_ENABLE
    else
        logf("No changes to user_settings");
#endif
    /* remove any outstanding status_cb and call it unconditionally */
    next_status_update_tick = current_tick - 1;
    unregister_storage_idle_func(flush_global_status_callback, true);
}

void reset_runtime(void)
{
    update_runtime(); /* in case this is > topruntimetime */
    zero_runtime();
    global_status.runtime = 0;
}

/*
 * update runtime and if greater topruntime as well
 */
void update_runtime(void)
{
    int elapsed_secs;

    elapsed_secs = (current_tick - lasttime) / HZ;
    global_status.runtime += elapsed_secs;
    lasttime += (elapsed_secs * HZ);

    if ( global_status.runtime > global_status.topruntime )
        global_status.topruntime = global_status.runtime;
}

void zero_runtime(void)
{
    lasttime = current_tick;
}

void status_save(bool force)
{
    if(force)
    {
        settings_save(); /* will force a status flush */
    }
    else
        register_storage_idle_func(flush_global_status_callback);
}

int settings_save(void)
{
    logf("%s", __func__);
    /* remove any oustanding status_cb we will call it in the config_block_cb */
    unregister_storage_idle_func(flush_global_status_callback, false);
    register_storage_idle_func(flush_config_block_callback);
    return 0;
}

bool settings_save_config(int options)
{
    char filename[MAX_PATH];
    const char *folder, *namebase;
    switch (options)
    {
        case SETTINGS_SAVE_THEME:
            folder = THEME_DIR;
            namebase = "theme";
            break;
        case SETTINGS_SAVE_EQPRESET:
            folder = EQS_DIR;
            namebase = "eq";
            break;
        case SETTINGS_SAVE_SOUND:
            folder = ROCKBOX_DIR;
            namebase = "sound";
            break;
        default:
            folder = ROCKBOX_DIR;
            namebase = "config";
            break;
    }
    create_numbered_filename(filename, folder, namebase, ".cfg", 2
                             IF_CNFN_NUM_(, NULL));

    /* allow user to modify filename */
    while (true) {
        if (!kbd_input(filename, sizeof(filename), NULL)) {
            break;
        }
        else {
            return false;
        }
    }

    if (settings_write_config(filename, options))
        splash(HZ, ID2P(LANG_SETTINGS_SAVED));
    else
        splash(HZ, ID2P(LANG_FAILED));
    return true;
}

/** Apply and Reset settings **/

/*
 * Applies the range infos stored in global_settings to
 * the peak meter.
 */
void settings_apply_pm_range(void)
{
    int pm_min, pm_max;

    /* depending on the scale mode (dBfs or percent) the values
       of global_settings.peak_meter_dbfs have different meanings */
    if (global_settings.peak_meter_dbfs)
    {
        /* convert to dBfs * 100          */
        pm_min = -(((int)global_settings.peak_meter_min) * 100);
        pm_max = -(((int)global_settings.peak_meter_max) * 100);
    }
    else
    {
        /* percent is stored directly -> no conversion */
        pm_min = global_settings.peak_meter_min;
        pm_max = global_settings.peak_meter_max;
    }

    /* apply the range */
    peak_meter_init_range(global_settings.peak_meter_dbfs, pm_min, pm_max);
}

void sound_settings_apply(void)
{
    sound_set(SOUND_BASS, global_settings.bass);
    sound_set(SOUND_TREBLE, global_settings.treble);
    sound_set(SOUND_BALANCE, global_settings.balance);
    sound_set(SOUND_VOLUME, global_status.volume);
    sound_set(SOUND_CHANNELS, global_settings.channel_config);
    sound_set(SOUND_STEREO_WIDTH, global_settings.stereo_width);
    sound_set(SOUND_BASS_CUTOFF, global_settings.bass_cutoff);
    sound_set(SOUND_TREBLE_CUTOFF, global_settings.treble_cutoff);
}

/* Shared bold UI font id (see font_get_ui_bold()); -1 when none is loaded. */
static int ui_bold_font_id = -1;

int font_get_ui_bold(void)
{
    if (ui_bold_font_id >= 0)
        return ui_bold_font_id;
    return screens[SCREEN_MAIN].getuifont();
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/* Push the theme's dialog chrome into the shared default dialog style. The
 * metrics always apply; the colours depend on "dialog colours".
 *
 * Note what is stored for "auto": sentinels, not colours. The album-derived
 * colours change while the player runs and this function is called once, when
 * settings are applied -- so anything resolved here would be frozen at whatever
 * was playing then. dialog.c resolves the sentinels per draw instead.
 *
 * These values come straight out of a hand-edited .cfg (atoi, no range check in
 * the settings loader), so they are clamped here: a negative margin or an absurd
 * border width would otherwise corrupt the frame's geometry. */
static void settings_apply_dialog_style(void)
{
    struct dialog_style s;
    dialog_style_default(&s);

    s.box_border_width     = clamp_int(global_settings.dialog_box_border_width,
                                       0, 16);
    s.box_margin           = clamp_int(global_settings.dialog_box_margin,
                                       0, LCD_WIDTH / 4);
    s.button_border_width  = clamp_int(global_settings.dialog_btn_border_width,
                                       0, 16);
    s.button_border_radius = clamp_int(global_settings.dialog_btn_border_radius,
                                       0, 64);
    /* Shadow with the metrics, not with the palette below: it applies whatever
     * dialog_colors says, so a theme need not turn the other nine on to set
     * it. */
    s.box_shadow_offset    = clamp_int(global_settings.dialog_box_shadow,
                                       0, 32);
    s.box_shadow_color     = global_settings.dialog_box_shadow_color;

    if (global_settings.dialog_colors == DIALOG_COLORS_ON)
    {
        s.box_fg                       = global_settings.dialog_box_fg;
        s.box_bg                       = global_settings.dialog_box_bg;
        s.box_border_color             = global_settings.dialog_box_border;
        s.button_fg                    = global_settings.dialog_btn_fg;
        s.button_bg                    = global_settings.dialog_btn_bg;
        s.button_border_color          = global_settings.dialog_btn_border;
        s.button_fg_selected           = global_settings.dialog_btn_fg_sel;
        s.button_bg_selected           = global_settings.dialog_btn_bg_sel;
        s.button_border_color_selected = global_settings.dialog_btn_border_sel;
    }
    else if (global_settings.dialog_colors == DIALOG_COLORS_AUTO)
    {
        /* Three colours and no mixes: the theme's pair, plus the accent on the
         * selected button. Everything is drawn on the plain background and
         * outlined in the plain foreground, so the box and its buttons are
         * shapes rather than fills, and the one filled thing in the dialog is
         * the selection.
         *
         * Blending a fourth value out of the pair -- a box background lifted
         * toward the foreground -- reads as washed out rather than as depth,
         * because every step toward the foreground is a step of contrast taken
         * off the text sitting on it.
         *
         * The selected label is whichever of the two reads on the accent --
         * fixing it to either one leaves album colours that make it
         * illegible. */
        s.box_fg                       = DIALOG_COLOR_FG;
        s.box_bg                       = DIALOG_COLOR_BG;
        s.box_border_color             = DIALOG_COLOR_FG;
        s.button_fg                    = DIALOG_COLOR_FG;
        s.button_bg                    = DIALOG_COLOR_BG;
        s.button_border_color          = DIALOG_COLOR_FG;
        s.button_fg_selected           = DIALOG_COLOR_ON_ACCENT;
        s.button_bg_selected           = DIALOG_COLOR_ACCENT;
        s.button_border_color_selected = DIALOG_COLOR_FG;
    }

    dialog_set_default_style(&s);
}

/* The nth entry of ARTWORK_FILTER_CFG_VALS, or NULL past the end. The list is
 * scanned rather than kept twice, so what the config file names and what the
 * filter engine is handed cannot drift apart. */
static const char *artwork_filter_entry(int idx, size_t *len)
{
    const char *p = ARTWORK_FILTER_CFG_VALS;

    while (idx-- > 0)
    {
        p = strchr(p, ',');
        if (!p)
            return NULL;
        p++;
    }
    *len = strcspn(p, ",");
    return p;
}

void artwork_filter_apply(void)
{
    char spec[ARTWORK_FILTER_MAX];
    const char *err = NULL;
    bool changed = false;
    size_t used = 0;

    spec[0] = '\0';
    for (int s = 0; s < ARTWORK_FILTER_SLOTS; s++)
    {
        const int idx = global_settings.artwork_filter[s];
        size_t len;
        const char *entry;

        if (idx <= 0)                            /* entry 0 is "off" */
            continue;
        entry = artwork_filter_entry(idx, &len);
        if (!entry || used + len + 2 > sizeof(spec))
            continue;
        if (used)
            spec[used++] = '+';
        memcpy(spec + used, entry, len);
        used += len;
        spec[used] = '\0';
    }

    /* Refused rather than quietly ignored: a chain the readers cannot run
     * would otherwise look like it had worked. Nothing in the list can be
     * refused today -- `blur` is not offered -- but the tier rule lives in
     * the engine, not in this list, and that is the right way round. */
    if (!art_filter_set(spec, &err, &changed))
    {
        splashf(HZ * 2, "artwork filter: %s", err);
        art_filter_set("", NULL, &changed);
    }
    if (changed)
        browser_albumart_invalidate();
}

void settings_apply(bool read_disk)
{
    logf("%s", __func__);
    int rc;
    CHART(">set_codepage");
    set_codepage(global_settings.default_codepage);
    CHART("<set_codepage");

    sound_settings_apply();

    audio_set_buffer_margin(global_settings.buffer_margin);

    lcd_scroll_speed(global_settings.scroll_speed);
    backlight_set_brightness(global_settings.brightness);
    backlight_set_timeout(global_settings.backlight_timeout);
    backlight_set_timeout_plugged(global_settings.backlight_timeout_plugged);
#if defined(HAVE_BACKLIGHT_FADING_INT_SETTING)
    backlight_set_fade_in(global_settings.backlight_fade_in);
    backlight_set_fade_out(global_settings.backlight_fade_out);
#endif
    storage_spindown(global_settings.disk_spindown);
    storage_set_storage_mode(global_settings.storage_mode);
    set_poweroff_timeout(global_settings.poweroff);
    if (global_settings.sleeptimer_on_startup)
        set_sleeptimer_duration(global_settings.sleeptimer_duration);
    set_keypress_restarts_sleep_timer(
        global_settings.keypress_restarts_sleeptimer);

#if BATTERY_CAPACITY_INC > 0
    /* only call if it's really exchangable */
    set_battery_capacity(global_settings.battery_capacity);
#endif

    lcd_update(); /* refresh after flipping the screen */
    settings_apply_pm_range();
    peak_meter_init_times(
        global_settings.peak_meter_release, global_settings.peak_meter_hold,
        global_settings.peak_meter_clip_hold);

    settings_apply_dialog_style();

    if (read_disk)
    {
        char buf[MAX_PATH];
        /* fonts need to be loaded before the WPS */
        if (global_settings.font_file[0]
            && global_settings.font_file[0] != '-') {
            int font_ui = screens[SCREEN_MAIN].getuifont();
            snprintf(buf, sizeof buf, FONT_DIR "/%s.fnt",
                     global_settings.font_file);
            if (!font_filename_matches_loaded_id(font_ui, buf))
            {
                CHART2(">font_load ", global_settings.font_file);
                if (font_ui >= 0)
                    font_unload(font_ui);
                rc = font_load_ex(buf, 0, global_settings.glyphs_to_cache);
                CHART2("<font_load ", global_settings.font_file);
                screens[SCREEN_MAIN].setuifont(rc);
                screens[SCREEN_MAIN].setfont(rc);
            }
        }
        /* Optional shared bold UI font (font_get_ui_bold()). Loaded here right
         * after the UI font so it and its consumers (album covers, USB screen)
         * share a single font-buffer slot rather than each loading their own. */
        if (global_settings.bold_font_file[0]
            && global_settings.bold_font_file[0] != '-')
        {
            snprintf(buf, sizeof buf, FONT_DIR "/%s.fnt",
                     global_settings.bold_font_file);
            if (ui_bold_font_id < 0
                || !font_filename_matches_loaded_id(ui_bold_font_id, buf))
            {
                if (ui_bold_font_id >= 0)
                    font_unload(ui_bold_font_id);
                ui_bold_font_id = font_load(buf); /* <0 -> falls back to UI font */
            }
        }
        else if (ui_bold_font_id >= 0)
        {
            font_unload(ui_bold_font_id);
            ui_bold_font_id = -1;
        }
        /* Loadable keyboard layouts are gone (click-wheel editor); the
         * kbd_file setting is inert. */
        if ( global_settings.lang_file[0]) {
            snprintf(buf, sizeof buf, LANG_DIR "/%s.lng",
                     global_settings.lang_file);
            CHART(">lang_core_load");
            lang_core_load(buf);
            CHART("<lang_core_load");
        }
        /* Last, because loading a .lng points every string back at the
         * built-in one first. */
        lang_override_load();
        CHART(">talk_init");
        talk_init(); /* use voice of same language */
        CHART("<talk_init");

        /* load the icon set */
        CHART(">icons_init");
        icons_init();
        CHART("<icons_init");

        CHART(">read_color_theme_file");
        read_color_theme_file();
        CHART("<read_color_theme_file");
    }
    screens[SCREEN_MAIN].set_foreground(global_settings.fg_color);
    screens[SCREEN_MAIN].set_background(global_settings.bg_color);

    lcd_scroll_step(global_settings.scroll_step);
    lcd_bidir_scroll(global_settings.bidir_limit);
    lcd_scroll_delay(global_settings.scroll_delay);

    set_albumart_mode(global_settings.album_art);

    /* before crossfade */
    audio_set_playback_frequency(global_settings.play_frequency);
    audio_set_crossfade(global_settings.crossfade);
    replaygain_update();
    dsp_set_crossfeed_type(global_settings.crossfeed);
    dsp_set_crossfeed_direct_gain(global_settings.crossfeed_direct_gain);
    dsp_set_crossfeed_cross_params(global_settings.crossfeed_cross_gain,
                                   global_settings.crossfeed_hf_attenuation,
                                   global_settings.crossfeed_hf_cutoff);

    /* Configure software equalizer, hardware eq is handled in audio_init() */
    dsp_eq_enable(global_settings.eq_enabled);
    dsp_set_eq_precut(global_settings.eq_precut);
    for(int i = 0; i < EQ_NUM_BANDS; i++) {
        dsp_set_eq_coefs(i, &global_settings.eq_band_settings[i]);
    }

    dsp_dither_enable(global_settings.dithering_enabled);
    dsp_surround_set_balance(global_settings.surround_balance);
    dsp_surround_set_cutoff(global_settings.surround_fx1, global_settings.surround_fx2);
    dsp_surround_mix(global_settings.surround_mix);
    dsp_surround_enable(global_settings.surround_enabled);
    dsp_afr_enable(global_settings.afr_enabled);
    dsp_pbe_precut(global_settings.pbe_precut);
    dsp_pbe_enable(global_settings.pbe);
    dsp_set_compressor(&global_settings.compressor_settings);


    set_backlight_filter_keypress(global_settings.bl_filter_first_keypress);
    set_selective_backlight_actions(global_settings.bl_selective_actions,
                                    global_settings.bl_selective_actions_mask,
                                    global_settings.bl_filter_first_keypress);
    backlight_set_on_button_hold(global_settings.backlight_on_button_hold);

    lcd_set_sleep_after_backlight_off(global_settings.lcd_sleep_after_backlight_off);




    usb_charging_enable(global_settings.usb_charging);


    usb_set_mode(global_settings.usb_mode);

    artwork_filter_apply();

    /* already called with THEME_STATUSBAR in settings_apply_skins() */
    CHART(">viewportmanager_theme_changed");
    viewportmanager_theme_changed(THEME_UI_VIEWPORT|THEME_LANGUAGE|THEME_BUTTONBAR);
    CHART("<viewportmanager_theme_changed");
}

/*
 * reset all settings to their default value
 */
void reset_setting(const struct settings_list *setting, void *var)
{
    switch (setting->flags&F_T_MASK)
    {
    case F_T_CUSTOM:
        setting->custom_setting->set_default(setting->setting,
                                             setting->default_val.custom);
        break;
    case F_T_INT:
    case F_T_UINT:
        if (setting->flags&F_DEF_ISFUNC)
            *(int*)var = setting->default_val.func();
        else if (setting->flags&F_T_SOUND)
            *(int*)var = sound_default(setting->sound_setting->setting);
        else *(int*)var = setting->default_val.int_;
        break;
    case F_T_BOOL:
        *(bool*)var = setting->default_val.bool_;
        break;
    case F_T_CHARPTR:
    case F_T_UCHARPTR:
        strmemccpy((char*)var, setting->default_val.charptr,
                   setting->filename_setting->max_len);
        break;
    }
}

void settings_reset(void)
{
    for(int i=0; i<nb_settings; i++)
        reset_setting(&settings[i], settings[i].setting);
    FOR_NB_SCREENS(i)
    {
        if (screens[i].getuifont() > FONT_SYSFIXED)
        {
            font_unload(screens[i].getuifont());
            screens[i].setuifont(FONT_SYSFIXED);
            screens[i].setfont(FONT_SYSFIXED);
        }
    }
}

/** Changing setting values **/
const struct settings_list* find_setting(const void* variable)
{
    for(int i = 0; i < nb_settings; i++)
    {
        const struct settings_list *setting = &settings[i];
        if (setting->setting == variable)
            return setting;
    }

    return NULL;
}

const struct settings_list* find_setting_by_cfgname(const char* name)
{
    logf("Searching for Setting: '%s'",name);
    for(int i = 0; i < nb_settings; i++)
    {
        const struct settings_list *setting = &settings[i];
        if (setting->cfg_name && !strcasecmp(setting->cfg_name, name))
        {
#ifdef LOGF_ENABLE
            name = debug_get_flags(settings[i].flags);
            logf("Found, %s", name);
#endif
            return setting;
        }
    }
    logf("Setting: '%s' Not Found!",name);

    return NULL;
}

bool set_bool(const char* string, const bool* variable )
{
    return set_bool_options(string, variable,
                            (char *)STR(LANG_SET_BOOL_YES),
                            (char *)STR(LANG_SET_BOOL_NO),
                            NULL);
}


bool set_bool_options(const char* string, const bool* variable,
                      const char* yes_str, int yes_voice,
                      const char* no_str, int no_voice,
                      void (*function)(bool))
{
    struct opt_items names[] = {
        {(unsigned const char *)no_str, no_voice},
        {(unsigned const char *)yes_str, yes_voice}
    };
    bool result;

    result = set_option(string, variable, RB_BOOL, names, 2,
                        (void (*)(int))(void (*)(void))function);
    return result;
}

bool set_int(const unsigned char* string,
             const char* unit,
             int voice_unit,
             const int* variable,
             void (*function)(int),
             int step,
             int min,
             int max,
             const char* (*formatter)(char*, size_t, int, const char*) )
{
    return set_int_ex(string, unit, voice_unit, variable, function,
                      step, min, max, formatter, NULL);
}

bool set_int_ex(const unsigned char* string,
                const char* unit,
                int voice_unit,
                const int* variable,
                void (*function)(int),
                int step,
                int min,
                int max,
                const char* (*formatter)(char*, size_t, int, const char*),
                int32_t (*get_talk_id)(int, int))
{
    (void)unit;
    struct settings_list item;
    const struct int_setting data = {
        .option_callback = function,
        .unit = voice_unit,
        .step = step,
        .min = min,
        .max = max,
        .formatter = formatter,
        .get_talk_id = get_talk_id,
    };
    item.int_setting = &data;
    item.flags = F_INT_SETTING|F_T_INT;
    item.lang_id = -1;
    item.setting = (void *)variable;
    return option_screen(&item, NULL, false, string);
}


static const struct opt_items *set_option_options;
static const char* set_option_formatter(char* buf, size_t size, int item, const char* unit)
{
    (void)buf, (void)unit, (void)size;
    return P2STR(set_option_options[item].string);
}

static int32_t set_option_get_talk_id(int value, int unit)
{
    (void)unit;
    return set_option_options[value].voice_id;
}

bool set_option(const char* string, const void* variable, enum optiontype type,
                const struct opt_items* options,
                int numoptions, void (*function)(int))
{
    int temp;
    struct settings_list item;
    const struct int_setting data = {
        .option_callback = function,
        .unit = UNIT_INT,
        .step = 1,
        .min = 0,
        .max = numoptions-1,
        .formatter = set_option_formatter,
        .get_talk_id = set_option_get_talk_id
    };
    memset(&item, 0, sizeof(struct settings_list));
    set_option_options = options;
    item.int_setting = &data;
    item.flags = F_INT_SETTING|F_T_INT;
    item.lang_id = -1;
    item.setting = &temp;
    if (type == RB_BOOL)
        temp = *(bool*)variable? 1: 0;
    else
        temp = *(int*)variable;
    if (!option_screen(&item, NULL, false, string))
    {
        if (type == RB_BOOL)

            *(bool*)variable = (temp == 1);
        else
            *(int*)variable = temp;
        return false;
    }
    return true;
}

/*
 * Takes filename, removes the directory and the extension,
 * and then copies the basename into setting, unless the basename exceeds maxlen
 **/
void set_file(const char* filename, char* setting)
{
    const int maxlen = MAX_FILENAME;
    const char* fptr = strrchr(filename,'/');
    const char* extptr;
    int len;
    int extlen = 0;

    if (!fptr)
        return;

    fptr++;

    extptr = strrchr(fptr, '.');

    if (!extptr || extptr < fptr)
        extlen = 0;
    else
        extlen = strlen(extptr);

    len = strlen(fptr) - extlen + 1;

    /* error later if filename isn't in ROCKBOX_DIR */
    if (len > maxlen || !file_exists(filename))
    {
        DEBUGF("%s Error %s\n", __func__, filename);
        return;
    }

    strmemccpy(setting, fptr, len);
    settings_save();
}

#ifdef LOGF_ENABLE
static char *debug_get_flags(uint32_t flags)
{
    static char buf[256];
    uint32_t ftype = flags & F_T_MASK; /* the variable type for the setting */
    flags &= ~F_T_MASK;
    uint32_t flags_rem = flags;
    switch (ftype)
    {
        case F_T_CUSTOM:
            strlcpy(buf, "Type: [CUSTOM]   Flags: ", sizeof(buf));
            break;
        case F_T_INT:
            strlcpy(buf, "Type: [INT]      Flags: ", sizeof(buf));
            break;
        case F_T_UINT:
            strlcpy(buf, "Type: [UINT]     Flags: ", sizeof(buf));
            break;
        case F_T_BOOL:
            strlcpy(buf, "Type: [BOOL]     Flags: ", sizeof(buf));
            break;
        case F_T_CHARPTR:
            strlcpy(buf, "Type: [CHARPTR]  Flags: ", sizeof(buf));
            break;
        case F_T_UCHARPTR:
            strlcpy(buf, "Type: [UCHARPTR] Flags: ", sizeof(buf));
            break;
        default:
            snprintf(buf, sizeof(buf),
                     "Type: [!UNKNOWN TYPE! (0x%lx)] Flags: ", (long)ftype);
            break;
    }

#define SETTINGFLAGS(n)                 \
        if(flags_rem & n) {             \
           flags_rem &= ~n;             \
    strlcat(buf, "["#n"]", sizeof(buf));}

    SETTINGFLAGS(F_RESUMESETTING);
    SETTINGFLAGS(F_THEMESETTING);
    SETTINGFLAGS(F_RECSETTING);
    SETTINGFLAGS(F_EQSETTING);
    SETTINGFLAGS(F_SOUNDSETTING);
    SETTINGFLAGS(F_NEEDAPPLY);
    SETTINGFLAGS(F_CHOICE_SETTING);
    SETTINGFLAGS(F_CHOICETALKS);
    SETTINGFLAGS(F_TABLE_SETTING);
    SETTINGFLAGS(F_CUSTOM_SETTING);
    SETTINGFLAGS(F_TIME_SETTING);

    SETTINGFLAGS(F_FILENAME);
    SETTINGFLAGS(F_INT_SETTING);
    SETTINGFLAGS(F_T_SOUND);
    SETTINGFLAGS(F_RGB);
    SETTINGFLAGS(F_BOOL_SETTING);

    SETTINGFLAGS(F_MIN_ISFUNC);
    SETTINGFLAGS(F_MAX_ISFUNC);
    SETTINGFLAGS(F_DEF_ISFUNC);
    SETTINGFLAGS(F_ALLOW_ARBITRARY_VALS);
    SETTINGFLAGS(F_CB_ON_SELECT_ONLY);
    SETTINGFLAGS(F_CB_ONLY_IF_CHANGED);
    SETTINGFLAGS(F_TEMPVAR);
    SETTINGFLAGS(F_PADTITLE);
    SETTINGFLAGS(F_NO_WRAP);
    SETTINGFLAGS(F_BANFROMQS);
    SETTINGFLAGS(F_DEPRECATED);

    SETTINGFLAGS(F_HAS_CFGVALS);
#undef SETTINGFLAGS

    /* anything left is unknown */
    if (flags_rem)
    {
        strlcat(buf, "[!UNKNOWN FLAGS!]", sizeof(buf));
        size_t len = strlen(buf);
        if (len < sizeof(buf))
            snprintf(buf + len, sizeof(buf) - len - 1, "[0x%lx]", flags_rem);
    }
    /* no flags set */
    if (flags == 0)
        strlcat(buf, "[0x0]", sizeof(buf));
    return buf;
}
#endif
static void debug_available_settings(void)
{
#if defined(DEBUG_AVAIL_SETTINGS) && defined(LOGF_ENABLE)
    static char namebuf[128];

    logf("\r\nAvailable Settings:");
    for (int i=0; i<nb_settings; i++)
    {
        uint32_t flags = settings[i].flags;
        const char *name;
        if (settings[i].cfg_name)
        {
            snprintf(namebuf, sizeof(namebuf), "'%s'", settings[i].cfg_name);
            name = namebuf;
        }
        else if (settings[i].RESERVED == NULL)
        {
            name = "SYS";
        }
        else
        {
            name = "?? UNKNOWN NAME ?? ";
        }

        logf("%-45s %s",name, debug_get_flags(flags));
    }
    logf("End Available Settings\r\n");
#endif
}
