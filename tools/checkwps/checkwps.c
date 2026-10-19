/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2008 by Dave Chapman
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "checkwps.h"
#include "draw/resize.h"
#include "screens/playback/wps.h"
#include "skin_buffer.h"
#include "skin_debug.h"
#include "skin/skin_engine.h"
#include "skin/wps_internals.h"
#include "settings/settings.h"
#include "draw/viewport.h"
#include "draw/color.h"
#include "file.h"
#include "font.h"
#include "misc.h"
#include "rbpaths.h"
#include "rbversion.h"

bool debug_wps = false;
int wps_verbose_level = 0;
char *skin_buffer;

/* --viewports. See dump_viewports() at the foot of the file. */
static bool want_viewports = false;
struct viewport checkwps_sbs_info_vp;
bool checkwps_have_sbs_info_vp = false;

#ifdef SIMULATOR
#error beep beep
#endif

/* static endianness conversion */
#define SWAP_16(x) ((typeof(x))(unsigned short)(((unsigned short)(x) >> 8) | \
                                                ((unsigned short)(x) << 8)))

#define SWAP_32(x) ((typeof(x))(unsigned long)( ((unsigned long)(x) >> 24) | \
                                               (((unsigned long)(x) & 0xff0000ul) >> 8) | \
                                               (((unsigned long)(x) & 0xff00ul) << 8) | \
                                                ((unsigned long)(x) << 24)))

#ifndef letoh16
unsigned short letoh16(unsigned short x)
{
    unsigned short n = 0x1234;
    unsigned char* ch = (unsigned char*)&n;

    if (*ch == 0x34)
    {
        /* Little-endian */
        return x;
    } else {
        return SWAP_16(x);
    }
}
#endif

#ifndef letoh32
unsigned short letoh32(unsigned short x)
{
    unsigned short n = 0x1234;
    unsigned char* ch = (unsigned char*)&n;

    if (*ch == 0x34)
    {
        /* Little-endian */
        return x;
    } else {
        return SWAP_32(x);
    }
}
#endif

#ifndef htole32
unsigned int htole32(unsigned int x)
{
    unsigned short n = 0x1234;
    unsigned char* ch = (unsigned char*)&n;

    if (*ch == 0x34)
    {
        /* Little-endian */
        return x;
    } else {
        return SWAP_32(x);
    }
}
#endif

int recalc_dimension(struct dim *dst, struct dim *src)
{
    (void) dst;
    (void) src;
    return 0;
}

#ifdef HAVE_ALBUMART
int playback_claim_aa_slot(struct dim *dim)
{
    (void) dim;
    return 0;
}

int playback_claim_aa_slot_keyed(struct dim *dim, uint32_t key)
{
    (void)key;
    return playback_claim_aa_slot(dim);
}

void playback_release_aa_slot(int slot)
{
    (void) slot;
    return;
}
#endif

int resize_on_load(struct bitmap *bm, bool dither,
                   struct dim *src, struct rowset *tmp_row,
                   unsigned char *buf, unsigned int len,
                   const struct custom_format *cformat,
                   IF_PIX_FMT(int format_index,)
                   struct img_part* (*store_part)(void *args),
                   void *args)
{
    (void) bm;
    (void) dither;
    (void) src;
    (void) tmp_row;
    (void) buf;
    (void) len;
    (void) cformat;
    IF_PIX_FMT((void) format_index);
    (void) store_part;
    (void) args;
    return 0;
}

static char pluginbuf[PLUGIN_BUFFER_SIZE];

static unsigned dummy_func2(void)
{
    return 0;
}

void* plugin_get_buffer(size_t *buffer_size)
{
    *buffer_size = PLUGIN_BUFFER_SIZE;
    return pluginbuf;
}

static struct viewport* init_viewport(struct viewport* vp)
{
    (void) vp;
    return NULL;
}

/* Reached only through viewportmanager_theme_enable(), which checkwps calls to
 * put viewport_set_defaults() on its inheriting path. Nothing is drawn, so
 * neither needs to do anything -- but they must exist: toggle_theme() calls
 * both unconditionally. */
static struct viewport* set_viewport(struct viewport* vp)
{
    (void)vp; return NULL;
}

static void screen_backdrop_show(char *backdrop_buffer)
{
    (void)backdrop_buffer;
}

struct user_settings global_settings = {
    .statusbar = STATUSBAR_TOP,
#ifdef HAVE_LCD_COLOR
    .bg_color = LCD_DEFAULT_BG,
    .fg_color = LCD_DEFAULT_FG,
#endif
};

struct system_status global_status;

int getwidth(void) { return LCD_WIDTH; }
int getheight(void) { return LCD_HEIGHT; }
int getuifont(void) { return 0; }
#ifdef HAVE_REMOTE_LCD
int remote_getwidth(void) { return LCD_REMOTE_WIDTH; }
int remote_getheight(void) { return LCD_REMOTE_HEIGHT; }
#endif

bool backdrop_load(const char *filename, char* backdrop_buffer)
{
 (void)filename; (void)backdrop_buffer; return true;
}

struct screen screens[NB_SCREENS] =
{
    {
        .screen_type=SCREEN_MAIN,
        .lcdwidth=LCD_WIDTH,
        .lcdheight=LCD_HEIGHT,
        .depth=LCD_DEPTH,
#ifdef HAVE_LCD_COLOR
        .is_color=true,
#else
        .is_color=false,
#endif
        .init_viewport=init_viewport,
        .set_viewport=set_viewport,
        .getwidth = getwidth,
        .getheight = getheight,
        .getuifont = getuifont,
#if LCD_DEPTH > 1
        .get_foreground=dummy_func2,
        .get_background=dummy_func2,
        .backdrop_load=backdrop_load,
        .backdrop_show=screen_backdrop_show,
#endif
    },
#ifdef HAVE_REMOTE_LCD
    {
        .screen_type=SCREEN_REMOTE,
        .lcdwidth=LCD_REMOTE_WIDTH,
        .lcdheight=LCD_REMOTE_HEIGHT,
        .depth=LCD_REMOTE_DEPTH,
        .getuifont = getuifont,
        .is_color=false,/* No color remotes yet */
        .init_viewport=init_viewport,
        .set_viewport=set_viewport,
        .getwidth=remote_getwidth,
        .getheight=remote_getheight,
#if LCD_REMOTE_DEPTH > 1
        .get_foreground=dummy_func2,
        .get_background=dummy_func2,
        .backdrop_load=backdrop_load,
        .backdrop_show=screen_backdrop_show,
#endif
    }
#endif
};

void screen_clear_area(struct screen * display, int xstart, int ystart,
                       int width, int height)
{
    display->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
    display->fillrect(xstart, ystart, width, height);
    display->set_drawmode(DRMODE_SOLID);
}

#if CONFIG_TUNER
bool radio_hardware_present(void)
{
    return true;
}
#endif

#include "fontbundle.h"
#include "themeassets.h"

static int loaded_fonts = 0;
static struct font _font;

/* mode 0 is for fonts in the WPS
   mode 1 is for fonts in the cfg file
   mode 2 is everything else in the cfg file

   search is an optional location to search for relative paths
*/
static int checkwps_loader(const char *path, const char *search, int mode, const char *def_ext)
{
    if (!path)
        return -1;
    if (!path[0])
        return mode? 0 : -2;  /* Empty is ok for non-WPS */
    if (path[0] == '-' && !path[1])
        return mode? 0 : -3;  /* Also ok for non-WPS */
    int absolute = (path[0] == '/');

    if (absolute)
        path++;  /* Absolute path, skip leading / */

    /* First see if it exists in the theme */
    int fd = open(path, O_RDONLY);
    if (fd >= 0)
        goto done;

    /* Strip ROCKBOX_DIR from search path */
    if (search) {
        search += strlen(ROCKBOX_DIR);
    }

    const char *rbdir = ROCKBOX_DIR;
    int rbdir_replaced = 0;
    if (rbdir[0] == '/')
        rbdir++; /* Strip leading '/' */

    /* Check for existance of correct ROCKBOX_DIR */
    fd = open(rbdir, O_RDONLY|O_DIRECTORY);
    if (fd > 0)
    {
        close(fd);
    }
    else
    {
        /* If it is not present, try ".rockbox" instead */
        fd = open(".rockbox", O_RDONLY|O_DIRECTORY);
        if (fd >= 0)
        {
            rbdir = ".rockbox";
            rbdir_replaced = 1;
            close(fd);
        }
    }

    /* Strip off anything leading up to .rockbox and try again */
    const char *tmp = strstr(path, ".rockbox");
    if (tmp && tmp != path)
    {
        fd = open(tmp, O_RDONLY);
        if (fd >= 0)
            goto done;
        if (rbdir_replaced)
            path = tmp;
    }

    /* See if file has an extension */
    const char *has_ext = strrchr(path, '.');
    /* if path starts with '.rockbox' we can get a false positive */
    if (has_ext && (has_ext == path || !strncmp(has_ext, rbdir, strlen(rbdir))))
        has_ext = NULL;

    char buf[2048];

    /* Next, try to append the default extension */
    if (!has_ext && def_ext)
    {
        sprintf(buf, "%s%s", path, def_ext);
        fd = open(buf, O_RDONLY);
        if (fd >= 0)
            goto done;
    } else {
        strcpy(buf, path);
    }

    /* At this point, buf is guaranteed to have an extension, so use it
       instead of 'path' going forward */
    char buf2[2500];
    if (!absolute && !mode)
    {
        /* WPS paths are relative to ROCKBOX_DIR */
        snprintf(buf2, sizeof(buf2), "%s/%s", rbdir, buf);
        fd = open(buf2, O_RDONLY);
        if (fd >= 0)
            goto done;
    }
    else if (!absolute && search)
    {
        /* Don't prepend a search path if we have an absolute path */
        snprintf(buf2, sizeof(buf2), "%s/%s/%s", rbdir, search, buf);
        fd = open(buf2, O_RDONLY);
        if (fd >= 0)
            goto done;
    }

    /* Ok, doesn't appear to exist in theme. Find basename. */
    strcpy(buf2, buf);
    char *basename = strrchr(buf2, '/');

    if (basename)
        basename++;
    else
        basename = buf2;

    /* TODO put full paths in asset list, and perform the same
       search path logic as above? */

    /* Check if file is a default theme asset */
    for (int i = 0 ; themeassets[i] != NULL; i++)
    {
        if (!strcmp(basename, themeassets[i]))
        {
            goto done;
        }
    }

    int missing = 1;

    if (mode == 0 || mode == 1) /* not found and it's a font */
    {
        /* Check if font is included in the bundle */
        for (int i = 0 ; bundledfonts[i] != NULL ; i++)
        {
            if (!strcmp(basename, bundledfonts[i]))
            {
                missing = 2;
                break;
            }
        }

        if (missing == 1)
        {
            printf("Font missing >%s<\n", path);
            return -4;
        } else if (missing == 2) {
            printf("INFO: Theme requires rockbox font bundle\n");
        }
    }
    else if (mode == 2) /* Asset that is not found in the default theme */
    {
        if (missing)
        {
            printf("ERROR: File missing: %s\n", path);
            return -5;
        }
    }

done:
    if (fd >= 0)
        close(fd);

    return 0;
}

int font_load(const char *path)
{
    int rval = checkwps_loader(path, FONT_DIR, 0, ".fnt");
    if (rval)
        return rval;

    rval = 2 + loaded_fonts;
    loaded_fonts++;
    return rval;
}

void font_unload(int font_id)
{
    (void)font_id;
}

struct font* font_get(int font)
{
    (void)font;
    return &_font;
}

bool settings_load_config(const char* file, bool apply)
{
    int fd;
    char line[128];
    bool theme_changed = false;
    fd = open_utf8(file, O_RDONLY);
    if (fd < 0)
        return false;

    while (read_line(fd, line, sizeof line) > 0)
    {
        char *name, *value;
        if (!settings_parseline(line, &name, &value))
            continue;
        if (!strcmp(name, "font"))
        {
            if (checkwps_loader(value, FONT_DIR, 1, ".fnt"))
                return false;
        }
#ifdef HAVE_REMOTE_LCD
        if (!strcmp(name, "remote font"))
        {
            if (checkwps_loader(value, FONT_DIR, 1, ".fnt"))
                return false;
        }
        else
#endif
#if LCD_DEPTH > 1
        if (!strcmp(name, "backdrop"))
        {
            if (checkwps_loader(value, NULL, 2, NULL)) /* No default path */
                return false;
        }
        else
#endif
        if (!strcmp(name, "wps"))
        {
            if (checkwps_loader(value, WPS_DIR, 2, ".wps"))
                return false;
        }
        else if (!strcmp(name, "sbs"))
        {
            if (checkwps_loader(value, SBS_DIR, 2, ".sbs"))
                return false;
        }
        else if (!strcmp(name, "fms"))
        {
            if (checkwps_loader(value, SBS_DIR, 2, ".fms"))
                return false;
        }
        else if (!strcmp(name, "wrs"))
        {
            if (checkwps_loader(value, WPS_DIR, 2, ".wrs"))
                return false;
        }
        else if (!strcmp(name, "rwps"))
        {
            if (checkwps_loader(value, WPS_DIR, 2, ".rwps"))
                return false;
        }
        else if (!strcmp(name, "rsbs"))
        {
            if (checkwps_loader(value, SBS_DIR, 2, ".rsbs"))
                return false;
        }
        else if (!strcmp(name, "rfms"))
        {
            if (checkwps_loader(value, SBS_DIR, 2, ".rfms"))
                return false;
        }
        else if (!strcmp(name, "rwrs"))
        {
            if (checkwps_loader(value, WPS_DIR, 2, ".rwrs"))
                return false;
        }
        else if (!strcmp(name, "iconset"))
        {
            if (checkwps_loader(value, ICON_DIR, 2, ".bmp"))
                return false;
        }
        else if (!strcmp(name, "viewers iconset"))
        {
            if (checkwps_loader(value, ICON_DIR, 2, ".bmp"))
                return false;
        }
#ifdef HAVE_REMOTE_LCD
        else if (!strcmp(name, "remote iconset"))
        {
            if (checkwps_loader(value, ICON_DIR, 2, ".bmp"))
                return false;
        }
#endif
#ifdef HAVE_LCD_COLOR
        else if (!strcmp(name, "filetype colours"))
        {
            if (checkwps_loader(value, THEME_DIR, 2, ".colours"))
                return false;
        }
#endif

        if (!string_to_cfg(name, value, &theme_changed))
        {
            if (strcmp(name, "fms") && strcmp(name, "rfms") &&
                strcmp(name, "wrs") && strcmp(name, "rwrs")) {
                /* Unknown settings are ignored by firmware */
                printf("WARNING: Invalid setting: %s = %s\n", name, value);
                continue;
            }
        }
    } /* while(...) */

    close(fd);

    (void)apply;
    return true;
}

/* This is no longer defined in ROCKBOX builds so just use a huge value */
#define SKIN_BUFFER_SIZE (200*1024)

/* The UI viewport a skin would end up using. A theme that declares one %Vi
 * with no label has it chosen for it; a theme that labels them picks between
 * them with %VI at render time, which nothing here does, so the first one
 * declared stands in. */
static struct skin_viewport *first_uivp(struct wps_data *data)
{
    struct skin_element *vp = SKINOFFSETTOPTR(skin_buffer, data->tree);

    while (vp)
    {
        struct skin_viewport *svp = SKINOFFSETTOPTR(skin_buffer, vp->data);

        if (!svp)
            break;
        if (svp->is_infovp)
            return svp;
        vp = SKINOFFSETTOPTR(skin_buffer, vp->next);
    }
    return NULL;
}

/* Where a viewport's colours came from.
 *
 * The engine keeps no record, so it is recovered by comparing: a viewport that
 * named no colour of its own still holds whatever viewport_set_defaults() put
 * there, and that is reproduced here. A viewport that spells out the colour it
 * would have inherited anyway reads as inherited, which costs nothing -- the
 * value is the same either way. */
static const char *colour_origin(unsigned int actual, unsigned int baseline)
{
    if (actual != baseline)
        return "set";
    return checkwps_have_sbs_info_vp ? "sbs" : "cfg";
}

/* Colours are held packed for the display -- RGB565 on both targets -- so a
 * raw print is unrecognisable next to the rrggbb a skin author wrote. Unpack
 * back to 24-bit, and mark a colour the skin pinned with '!'. A palette word
 * prints as the word. */
static const char *colour_text(unsigned int c, char *buf, int len)
{
    static const char *const names[] = { "accent", "dominant", "vivid" };
    const struct color_word *w = color_word(c);
    bool fixed = (c & COLOR_FIXED) != 0;

    if (w)
    {
        int n = snprintf(buf, len, "%s", names[w->kind]);

        if (w->bound && n < len)
            n += snprintf(buf + n, len - n, "%c%u", w->bound > 0 ? '>' : '<',
                          w->tone);
        if (w->shade && n < len)
            n += snprintf(buf + n, len - n, ".%u", w->shade - 1);
        if (w->has_fallback && n < len)
            snprintf(buf + n, len - n, ":%02x%02x%02x",
                     RGB_UNPACK_RED(w->fallback),
                     RGB_UNPACK_GREEN(w->fallback),
                     RGB_UNPACK_BLUE(w->fallback));
        return buf;
    }

    c &= ~COLOR_FIXED;
    snprintf(buf, len, "%s%02x%02x%02x", fixed ? "!" : "",
             RGB_UNPACK_RED(c), RGB_UNPACK_GREEN(c), RGB_UNPACK_BLUE(c));
    return buf;
}

static const char *viewport_label(const struct skin_viewport *svp)
{
    const char *label;

    if (svp->label == VP_DEFAULT_LABEL)
        return "(default)";
    label = SKINOFFSETTOPTR(skin_buffer, svp->label);
    return label ? label : "-";
}

/* One line per viewport: where it is, what font and colours it ended up with,
 * and for each colour whether the skin set it or inherited it. The inherited
 * ones are the point of the table -- a .wps viewport that names no colour
 * silently takes the browser list's, which is invisible in the file itself. */
static void dump_viewports(const char *name, struct wps_data *data,
                           enum screen_type screen)
{
    struct skin_element *vp = SKINOFFSETTOPTR(skin_buffer, data->tree);
    int n = 0, inherited = 0;

    printf("Viewports in %s\n", name);
    printf("  #  label            x    y     w    h  font  "
           "fg           bg           flags\n");

    while (vp)
    {
        struct skin_viewport *svp = SKINOFFSETTOPTR(skin_buffer, vp->data);
        struct viewport base;
        const char *fg_from, *bg_from;
        char flags[48], fg_text[24], bg_text[24];

        if (!svp)
            break;

        /* What this viewport would have held had it named no colour. */
        viewport_set_defaults(&base, screen);
        fg_from = colour_origin(svp->dc_orig_fg, base.fg_pattern);
        bg_from = colour_origin(svp->dc_orig_bg, base.bg_pattern);
        if (!strcmp(fg_from, "sbs") || !strcmp(bg_from, "sbs"))
            inherited++;

        flags[0] = '\0';
        if (svp->is_infovp)
            strcat(flags, "ui ");
        if (svp->output_to_backdrop_buffer)
            strcat(flags, "backdrop ");
        if (svp->hidden_flags & VP_NEVER_VISIBLE)
            strcat(flags, "never-visible ");

        printf("  %-2d %-14s %4d %4d %5d %4d  %4d  %-7s (%s) %-7s (%s) %s\n",
               ++n, viewport_label(svp),
               svp->vp.x, svp->vp.y, svp->vp.width, svp->vp.height,
               svp->parsed_fontid,
               colour_text(svp->dc_orig_fg, fg_text, sizeof(fg_text)), fg_from,
               colour_text(svp->dc_orig_bg, bg_text, sizeof(bg_text)), bg_from,
               flags);

        vp = SKINOFFSETTOPTR(skin_buffer, vp->next);
    }

    printf("\n  set = named on the viewport's own declaration line\n");
    printf("  sbs = inherited from the .sbs %%Vi viewport\n");
    printf("  cfg = neither: the theme default. That is the .cfg's foreground\n"
           "        and background colour on the player; checkwps reads no .cfg\n"
           "        and shows its built-in pair instead.\n");
    if (inherited)
        printf("\n  %d of %d viewports inherit a colour from the .sbs.\n",
               inherited, n);
    printf("\n");
}

int check_filetype(const char *ext, enum skinnable_screens *skin,
                    enum screen_type *screen)
{
    if (!strcmp(ext, "sbs"))
    {
        *skin = CUSTOM_STATUSBAR;
        *screen = SCREEN_MAIN;
    }
    else if (!strcmp(ext, "wps"))
    {
        *skin = WPS;
        *screen = SCREEN_MAIN;
    }
    else if (!strcmp(ext, "fms"))
    {
#if CONFIG_TUNER
        *skin = FM_SCREEN;
        *screen = SCREEN_MAIN;
#else
        return 1;
#endif
    }
    else if (!strcmp(ext, "wrs"))
    {
#ifdef HAVE_RECORDING
        *skin = RECORDING_SCREEN;
        *screen = SCREEN_MAIN;
#else
        return 1;
#endif
    }
    else if (!strcmp(ext, "rsbs"))
    {
#ifdef HAVE_REMOTE_LCD
        *skin = CUSTOM_STATUSBAR;
        *screen = SCREEN_REMOTE;
#else
        return 1;  /* unsupported, but not an error */
#endif
    }
    else if (!strcmp(ext, "rwps"))
    {
#ifdef HAVE_REMOTE_LCD
        *skin = WPS;
        *screen = SCREEN_REMOTE;
#else
        return 1;
#endif
    }
    else if (!strcmp(ext, "rfms"))
    {
#if defined(HAVE_REMOTE_LCD) && CONFIG_TUNER
        *skin = FM_SCREEN;
        *screen = SCREEN_REMOTE;
#else
        return 1;
#endif
    }
    else if (!strcmp(ext, "rwrs"))
    {
#if defined(HAVE_REMOTE_LCD) && defined(HAVE_RECORDING)
        *skin = RECORDING_SCREEN;
        *screen = SCREEN_REMOTE;
#else
        return 1;
#endif
    }
    else if (!strcmp(ext, "cfg"))
    {
        return 2;
    }
    else
        return -1;

    return 0;
}

int main(int argc, char **argv)
{
    int ret = 0;
    int res;
    int filearg = 1;

    struct wps_data wps={0};
    enum screen_type screen = SCREEN_MAIN;
    enum skinnable_screens skin;

    /* No arguments -> print the help text
     * Also print the help text upon -h or --help */
    if( (argc < 2) ||
        strcmp(argv[1],"-h") == 0 ||
        strcmp(argv[1],"--help") == 0 )
    {
        printf("Usage: checkwps.%s [OPTIONS] [filename1] [filename2] ...\n", TARGET_NAME);
        printf("  supported file types: wps, sbs, fms, wrs, cfg, "
               "rwps, rsbs, rfms, rwrs\n");
        printf("\nOPTIONS:\n");
        printf("\t-v\t\tverbose\n");
        printf("\t-vv\t\tmore verbose\n");
        printf("\t-vvv\t\tvery verbose\n");
        printf("\t--viewports\tprint each skin's resolved viewport table\n");
        printf("\t-h,\t--help\tshow this message\n");
        printf("\nName an .sbs before a .wps to resolve them together, the\n"
               "way the player does: a .wps viewport that sets no colour of\n"
               "its own inherits the .sbs's %%Vi viewport.\n");
        return 1;
    }

    while (argv[filearg] && argv[filearg][0] == '-')
    {
        const char *opt = argv[filearg++];

        if (!strcmp(opt, "--viewports"))
        {
            want_viewports = true;
            continue;
        }
        for (int i = 1; opt[i] == 'v'; i++)
        {
            wps_verbose_level++;
            debug_wps = true;
        }
    }

    skin_buffer = malloc(SKIN_BUFFER_SIZE);
    if (!skin_buffer)
    {
        printf("mallloc fail!\n");
        return 1;
    }

    skin_buffer_init(skin_buffer, SKIN_BUFFER_SIZE);

    printf(">>checkwps.%s revision %s\n", TARGET_NAME, RBVERSION);

    /* Go through every skin that was thrown at us.
       Don't error out until we have checked everything */
    while (argv[filearg])
    {
        const char* name = argv[filearg++];
        const char *ext = strrchr(name, '.');
        struct skin_stats stats;
        printf("Checking %s ...\n", name);
        if (!ext)
        {
            printf("Invalid extension\n");
            ret = 2;
            continue;
        }
        ext++;

        int valid = check_filetype(ext, &skin, &screen);
        if (valid < 0)
        {
            printf("Invalid extension\n");
            ret = 2;
            continue;
        }
        else if (valid == 2)
        {
            if (!settings_load_config(name, false))
            {
                printf("Bad configuration\n");
                ret = 3;
                continue;
            }
            printf("%s parsed OK\n\n", ext);
            continue;
        }
        else if (valid > 0)
            continue; /* skip (unsupported by this target but not an error) */

        loaded_fonts = 0;
        res = skin_data_load(skin, screen, &wps, name, true, &stats);

        if (!res) {
            printf("%s parsing failure\n", ext);
            skin_error_format_message();
            /* A skin can also fail after it has parsed, on a bitmap or a font
             * it names. That leaves no error line, and the reason is a debugf
             * which only -v shows. */
            if (!skin_error_line() && !debug_wps)
                printf("Run again with -v for the reason.\n");
            ret = 4;
            continue;
        }

        printf("%s parsed OK\n\n", ext);

        if (want_viewports)
            dump_viewports(name, &wps, screen);

        /* An .sbs names the rectangle lists draw in, and every later skin
         * resolves its own viewports against it. Keep it for the rest of the
         * run; stubs.c hands it back from sb_skin_get_info_vp(). */
        if (!strcmp(ext, "sbs") || !strcmp(ext, "rsbs"))
        {
            struct skin_viewport *info =
                skin_find_item(VP_DEFAULT_LABEL_STRING, SKIN_FIND_UIVP, &wps);

            if (!info)
                info = first_uivp(&wps);
            if (info)
            {
                checkwps_sbs_info_vp = info->vp;
                checkwps_have_sbs_info_vp = true;
                /* viewport_set_defaults() consults the info viewport only
                 * while a theme is on, which is the state a loaded .sbs
                 * represents. */
                viewportmanager_theme_enable(screen, true, NULL);
            }
        }

        if (wps_verbose_level>2)
            skin_debug_tree(SKINOFFSETTOPTR(skin_buffer, wps.tree));
    }

    if (skin_buffer)
        free(skin_buffer);
    return ret;
}
