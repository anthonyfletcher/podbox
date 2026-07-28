/***************************************************************************
 * Original code from RockBox
 * was: apps/filetypes.c
 * Copyright (C) 2007 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * The filetype registry: maps extensions to attributes, icons, colours and
 * the core viewer that opens them. Loads the colour and viewer theme
 * files.
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "rbpaths.h"  /* THEME_DIR, ICON_DIR */
#include "string.h"

#include "settings/settings.h"
#include "debug.h"
#include "lang.h"
#include "kernel.h"
#include "system/app_util.h"
#include "draw/color.h"
#include "system/strutil.h"
#include "filetypes.h"
#include "viewers/text_viewer/text_viewer.h"
#include "viewers/image_viewer/image_viewer_pub.h"
#include "dir.h"
#include "file.h"
#include "draw/icon_bitmaps.h"
/*#define LOGF_ENABLE*/
#include "logf.h"

/* max filetypes (extensions & icons stored here) */
#define MAX_FILETYPES 192

static void fill_from_builtin(const char*,int) INIT_ATTR;
static void read_builtin_types_init(void) INIT_ATTR;

/* string array for known audio file types (tree_attr == FILE_ATTR_AUDIO) */
static const char* inbuilt_audio_filetypes[] = {
    "mp3", "mp2", "mpa", "mp1", "ogg", "oga", "wma", "wmv", "asf", "wav",
    "flac", "ac3", "a52", "mpc", "wv", "m4a", "m4b", "mp4", "mod", "mpga",
    "shn", "aif", "aiff", "spx", "opus", "sid", "adx", "nsf", "nsfe", "spc",
    "ape", "mac", "sap", "rm", "ra", "rmvb", "cmc", "cm3", "cmr", "cms", "dmc",
    "dlt", "mpt", "mpd", "rmt", "tmc", "tm8", "tm2", "oma", "aa3", "at3", "mmf",
    "au", "snd", "vox", "w64", "tta", "ay", "vtx", "gbs", "hes", "sgc", "vgm",
    "vgz", "kss", "aac",
};

struct filetype_inbuilt {
    const char* extension;
    int tree_attr;
};

/* a table for the known file types, besides audio */
static const struct filetype_inbuilt inbuilt_filetypes[] = {
    { "m3u",  FILE_ATTR_M3U },
    { "m3u8", FILE_ATTR_M3U },
    { "cfg",  FILE_ATTR_CFG },
    { "wps",  FILE_ATTR_WPS },
    { "log",  FILE_ATTR_LOG   },
    { "lng",  FILE_ATTR_LNG   },
    { "rock", FILE_ATTR_ROCK  },
    { "lua",  FILE_ATTR_LUA   },
    { "opx",  FILE_ATTR_OPX   },
    { "fnt",  FILE_ATTR_FONT  },
    { "kbd",  FILE_ATTR_KBD   },
    { "bmark",FILE_ATTR_BMARK },
    { "cue",  FILE_ATTR_CUE   },
    { "sbs",  FILE_ATTR_SBS   },
#ifdef BOOTFILE_EXT
    { BOOTFILE_EXT,  FILE_ATTR_MOD },
#endif
    /* Types owned by the core-linked viewers. Compiled in here rather than
     * read from a config file, because the viewers are part of the binary. */
    { "txt",  FILE_ATTR_TXT },
    { "md",   FILE_ATTR_TXT },
    { "nfo",  FILE_ATTR_TXT },
    { "html", FILE_ATTR_TXT },
    { "htm",  FILE_ATTR_TXT },
    { "rtf",  FILE_ATTR_TXT },
    { "fb2",  FILE_ATTR_TXT },
    { "epub", FILE_ATTR_TXT },
    { "docx", FILE_ATTR_TXT },
    { "pdf",  FILE_ATTR_TXT },
    { "bmp",  FILE_ATTR_IMG },
    { "jpg",  FILE_ATTR_IMG },
    { "jpe",  FILE_ATTR_IMG },
    { "jpeg", FILE_ATTR_IMG },
    { "png",  FILE_ATTR_IMG },
    { "ppm",  FILE_ATTR_IMG },
    { "gif",  FILE_ATTR_IMG },
};

struct fileattr_icon_voice {
    int tree_attr;
    uint16_t icon;
    int16_t voiceclip; /* -1 for types with no spoken name */
};

/* a table for the known file types icons & voice clips */
static const struct fileattr_icon_voice inbuilt_attr_icons_voices[] = {
    { FILE_ATTR_AUDIO, Icon_Audio,     VOICE_EXT_MPA },
    { FILE_ATTR_M3U,   Icon_Playlist,  LANG_PLAYLIST },
    { FILE_ATTR_CFG,   Icon_Config,    VOICE_EXT_CFG },
    { FILE_ATTR_WPS,   Icon_Wps,       VOICE_EXT_WPS },
    { FILE_ATTR_LNG,   Icon_Language,  LANG_LANGUAGE },
    { FILE_ATTR_ROCK,  Icon_Plugin,    VOICE_EXT_ROCK },
    { FILE_ATTR_LUA,   Icon_Plugin,    VOICE_EXT_ROCK },
    { FILE_ATTR_OPX,   Icon_Plugin,    VOICE_EXT_ROCK },
    { FILE_ATTR_FONT,  Icon_Font,      VOICE_EXT_FONT },
    { FILE_ATTR_KBD,   Icon_Keyboard,  VOICE_EXT_KBD },
    { FILE_ATTR_BMARK, Icon_Bookmark,  VOICE_EXT_BMARK },
    { FILE_ATTR_CUE,   Icon_Bookmark,  VOICE_EXT_CUESHEET },
    { FILE_ATTR_SBS,   Icon_Wps,       VOICE_EXT_SBS },
#if defined(BOOTFILE_EXT) || defined(BOOTFILE_EXT2)
    { FILE_ATTR_MOD,   Icon_Firmware,  VOICE_EXT_AJZ },
#endif
    /* Icon codes 1 and 2 of the theme's viewers iconset, which is where
     * viewers.config used to point these. Themes shipping no viewers iconset
     * fall back to Icon_Questionmark, as they always have. */
    { FILE_ATTR_TXT,   Icon_Last_Themeable + 1, -1 },
    { FILE_ATTR_IMG,   Icon_Last_Themeable + 2, -1 },
};

static int filetype_inbuilt_index(int tree_attr)
{
    size_t count = ARRAY_SIZE(inbuilt_attr_icons_voices);
    /* try to find a inbuilt index for the extension, if known */
    tree_attr &= FILE_ATTR_MASK; /* file type */

    for (size_t i = count - 1; i < count; i--)
    {
        if (tree_attr == inbuilt_attr_icons_voices[i].tree_attr)
        {
            logf("%s found attr %d id", __func__, tree_attr);
            return i;
        }
    }
    logf("%s not found attr %d", __func__, tree_attr);
    return -1;
}

long filetype_get_voiceclip(int attr)
{
    if (global_settings.talk_filetype)
    {
        int index = filetype_inbuilt_index(attr);
        if (index >= 0 && inbuilt_attr_icons_voices[index].voiceclip >= 0)
        {
            logf("%s found attr %d id %d", __func__, attr,
                 inbuilt_attr_icons_voices[index].voiceclip);
            return inbuilt_attr_icons_voices[index].voiceclip;
        }
    }
    logf("%s not found attr %d", __func__, attr);
    return -1;
}

struct file_type {
    enum themable_icons icon; /* the icon which shall be used for it, NOICON if unknown */
    unsigned char  attr; /* FILE_ATTR_MASK >> 8 */
    const char* extension; /* NULL for none */
};

static struct file_type filetypes[MAX_FILETYPES];

static enum themable_icons custom_filetype_icons[MAX_FILETYPES];
static bool custom_icons_loaded = false;

static int custom_colors[MAX_FILETYPES];
struct filetype_unknown {
    enum themable_icons icon;
    int color;
};
static struct filetype_unknown unknown_file = {
    .icon = Icon_NOICON,
    .color = -1,
};

static int filetype_count = 0;

static int find_extension(const char* extension)
{
    if (extension)
    {
        for (int i=1; i<filetype_count; i++)
        {
            if (filetypes[i].extension &&
                !strcasecmp(extension, filetypes[i].extension))
                return i;
        }
    }
    return -1;
}

/* Colors file format is similar to icons:
 * ext:hex_color
 * load a colors file from a theme with:
 * filetype colours: filename.colours */
void read_color_theme_file(void) {
    char buffer[MAX_PATH];
    int fd;
    char *ext, *color;
    int i;
    for (i = 0; i < MAX_FILETYPES; i++) {
        custom_colors[i] = -1;
    }
    unknown_file.color = -1;
    if (!global_settings.colors_file[0] || global_settings.colors_file[0] == '-')
        return;

    fd = open_pathfmt(buffer, sizeof(buffer), O_RDONLY,
                      THEME_DIR "/%s.colours", global_settings.colors_file);
    if (fd < 0)
        return;
    while (read_line(fd, buffer, MAX_PATH) > 0)
    {
        if (!settings_parseline(buffer, &ext, &color))
            continue;
        if (!strcasecmp(ext, "folder"))
        {
            hex_to_rgb(color, &custom_colors[0]);
            continue;
        }
        if (!strcmp(ext, "???"))
        {
            hex_to_rgb(color, &unknown_file.color);
            continue;
        }
        i = find_extension(ext);
        if (i >= 0)
            hex_to_rgb(color, &custom_colors[i]);
    }
    close(fd);
}

static int parse_icon(const char *line, enum themable_icons *icon)
{
    int num = -1;
    if (*line == '*')
    {
        num = atoi(line+1);
        *icon = num;
    }
    else if (*line == '-')
    {
        *icon = Icon_NOICON;
    }
    else if (*line >= '0' && *line <= '9')
    {
        num = atoi(line);
        *icon = Icon_Last_Themeable + num;
    }
    return num;
}

void read_viewer_theme_file(void)
{
    char buffer[MAX_PATH];
    int fd;
    char *ext, *icon;
    int i;
    enum themable_icons *icon_dest;
    global_status.viewer_icon_count = 0;
    custom_icons_loaded = false;
    /*custom_filetype_icons[0] = Icon_Folder; filetypes[0] is folder icon.. */
    for (i=0; i<filetype_count; i++)
    {
        custom_filetype_icons[i] = filetypes[i].icon;
    }

    fd = open_pathfmt(buffer, sizeof(buffer), O_RDONLY,
                      ICON_DIR "/%s.icons", global_settings.viewers_icon_file);
    if (fd < 0)
        return;

    while (read_line(fd, buffer, MAX_PATH) > 0)
    {
        if (!settings_parseline(buffer, &ext, &icon))
            continue;
        i = find_extension(ext);
        if (i >= 0)
            icon_dest = &custom_filetype_icons[i];
        else if (!strcmp(ext, "???"))
            icon_dest = &unknown_file.icon;
        else
            icon_dest = NULL;

        if (icon_dest)
        {
            if (parse_icon(icon, icon_dest) > global_status.viewer_icon_count)
                global_status.viewer_icon_count++;
        }
    }
    close(fd);
    custom_icons_loaded = true;
}

void filetype_init(void)
{
    /* set the directory item first */
    filetypes[0].extension = NULL;
    filetypes[0].attr   = 0;
    filetypes[0].icon   = Icon_Folder;

    filetype_count = 1;

    read_builtin_types_init();
    read_viewer_theme_file();
    read_color_theme_file();
}

static void fill_from_builtin(const char *ext, int tree_attr)
{
    if (filetype_count >= MAX_FILETYPES)
        return;

    struct file_type *filetype = &filetypes[filetype_count];
    filetype->icon = unknown_file.icon;
    filetype->attr   = tree_attr>>8;
    filetype->extension = ext;

    int index = filetype_inbuilt_index(tree_attr);
    if (index >= 0)
    {
        filetype->icon = inbuilt_attr_icons_voices[index].icon;
    }

    filetype_count++;
}

static void read_builtin_types_init(void)
{
    for(size_t i = 0; (i < ARRAY_SIZE(inbuilt_audio_filetypes)); i++)
    {
        fill_from_builtin(inbuilt_audio_filetypes[i], FILE_ATTR_AUDIO);
    }

    for(size_t i = 0; (i < ARRAY_SIZE(inbuilt_filetypes)); i++)
    {
        fill_from_builtin(inbuilt_filetypes[i].extension,
                          inbuilt_filetypes[i].tree_attr);
    }
}

static int file_find_extension(const char* file)
{
    char *extension = strrchr(file, '.');
    if (extension)
        extension++;
    return find_extension(extension);
}

int filetype_get_attr(const char* file)
{
    int i = file_find_extension(file);
    if (i >= 0)
        return (filetypes[i].attr<<8)&FILE_ATTR_MASK;
    return 0;
}

static int find_attr(int attr)
{
    int i;
    /* skip the directory item */
    if ((attr & ATTR_DIRECTORY)==ATTR_DIRECTORY)
        return 0;
    for (i=1; i<filetype_count; i++)
    {
        if ((attr>>8) == filetypes[i].attr)
            return i;
    }
    return -1;
}

int filetype_get_color(const char * name, int attr)
{
    if ((attr & ATTR_DIRECTORY)==ATTR_DIRECTORY)
        return custom_colors[0];
    int i = file_find_extension(name);
    if (i <= 0)
        return unknown_file.color;
    return custom_colors[i];
}

int filetype_get_icon(int attr)
{
    int index = find_attr(attr);
    if (index < 0)
        return unknown_file.icon;
    if (custom_icons_loaded)
        return custom_filetype_icons[index];
    return filetypes[index].icon;
}

bool filetype_supported(int attr)
{
    return find_attr(attr) >= 0;
}

/* Viewers linked into the core rather than loaded as a .rock. The extension
 * that reaches each one is fixed by inbuilt_filetypes above. */
bool filetype_open_core_viewer(int attr, const char *file, int *rc)
{
    switch (attr & FILE_ATTR_MASK)
    {
        case FILE_ATTR_TXT:
            *rc = text_viewer(file);
            return true;
        case FILE_ATTR_IMG:
            *rc = image_viewer(file);
            return true;
    }
    return false;
}

