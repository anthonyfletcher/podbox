/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The equaliser editor: the whole screen given to the response curve, in
 * Simple mode (each band's gain) or Enhanced (frequency, gain, Q and type).
 * The lists it opens -- its menu and the presets -- are the theme's
 * skinned list.
 *
 * Everything drawn comes from the theme: its colours, with the list
 * selection colour as the accent, the UI font and its bold, and for labels a
 * smaller size of the UI font's family if a skin has loaded one. Every height
 * is derived from those fonts; a font too large to leave the curve room
 * sends the user to the band list instead.
 *
 * Every change goes to the DSP as it is made, and the curve is recomputed
 * for the one band that changed (eq_curve.c).
 *
 * Parts, in order:
 *   - built-in presets and the presets list
 *   - state, colours, fonts and layout
 *   - drawing: text, the curve, the readouts
 *   - changing a band
 *   - the menu, on a held Menu
 *   - the input loop
 ****************************************************************************/

#include "config.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "dir.h"
#include "rbpaths.h"
#include "lang.h"
#include "input/action.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "system/shutdown.h"
#include "draw/viewport.h"
#include "draw/color.h"
#include "skin/skin_albumart_color.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "screens/playback/wps.h"
#include "root_menu.h"
#include "eq_settings.h"
#include "eq_curve.h"
#include "eq_editor.h"

/** built-in presets and the presets list **/

struct eq_preset
{
    int lang;
    signed char half_db[EQ_NUM_BANDS];  /* gain at each default band */
};

static const struct eq_preset builtin_presets[] = {
    { LANG_EQUALIZER_PRESET_FLAT,     {   0,  0,  0, 0, 0,  0, 0, 0,  0,  0 } },
    { LANG_BASS,                      {  12, 10,  7, 3, 0,  0, 0, 0,  0,  0 } },
    { LANG_TREBLE,                    {   0,  0,  0, 0, 0,  0, 3, 6,  9, 11 } },
    { LANG_EQUALIZER_PRESET_VOCAL,    {  -4, -3, -2, 0, 3,  6, 6, 3,  0, -2 } },
    { LANG_EQUALIZER_PRESET_LOUDNESS, {  10,  8,  4, 0, -2, -2, 0, 3, 6,  8 } },
    { LANG_EQUALIZER_PRESET_SPOKEN,   { -12, -8, -4, 0, 4,  6, 6, 2, -2, -6 } },
};
#define BUILTIN_COUNT ((int)ARRAYLEN(builtin_presets))

#define PRESET_FILES_MAX 32
#define PRESET_NAME_MAX  64

static char preset_files[PRESET_FILES_MAX][PRESET_NAME_MAX];
static int preset_file_count;

/* What the curve was last set from, for the editor's title; empty once a
 * band is changed by hand. */
static char preset_name[PRESET_NAME_MAX];

/* Bands changed in the editor since it opened or a preset was chosen or
 * saved. */
static bool unsaved;

static void apply_builtin(int i)
{
    for (int b = 0; b < EQ_NUM_BANDS; b++)
    {
        global_settings.eq_band_settings[b] = eq_defaults[b];
        global_settings.eq_band_settings[b].gain =
            builtin_presets[i].half_db[b] * 5;
    }
    global_settings.eq_enabled = true;
    eq_apply();
}

/* The built-in preset the bands are set to exactly, or -1. */
static int builtin_match(void)
{
    for (int i = 0; i < BUILTIN_COUNT; i++)
    {
        int b;
        for (b = 0; b < EQ_NUM_BANDS; b++)
        {
            const struct eq_band_setting *s = &global_settings.eq_band_settings[b];
            const struct eq_band_setting *d = &eq_defaults[b];
            if (s->type != d->type || s->cutoff != d->cutoff || s->q != d->q
                || s->gain != builtin_presets[i].half_db[b] * 5)
                break;
        }
        if (b == EQ_NUM_BANDS)
            return i;
    }
    return -1;
}

/* The .cfg names in EQS_DIR, extension dropped, in order. */
static void read_preset_files(void)
{
    DIR *dir = opendir(EQS_DIR);
    struct dirent *entry;

    preset_file_count = 0;
    if (!dir)
        return;
    while ((entry = readdir(dir)) && preset_file_count < PRESET_FILES_MAX)
    {
        struct dirinfo info = dir_get_info(dir, entry);
        size_t len = strlen(entry->d_name);
        char *name = preset_files[preset_file_count];
        int i;

        if ((info.attribute & ATTR_DIRECTORY) || len < 5 || len - 4 >= PRESET_NAME_MAX
            || strcasecmp(entry->d_name + len - 4, ".cfg"))
            continue;
        strmemccpy(name, entry->d_name, len - 3);
        for (i = preset_file_count; i > 0
             && strcasecmp(preset_files[i - 1], name) > 0; i--)
            ;
        if (i < preset_file_count)
        {
            char tmp[PRESET_NAME_MAX];
            strcpy(tmp, name);
            memmove(preset_files[i + 1], preset_files[i],
                    (preset_file_count - i) * PRESET_NAME_MAX);
            strcpy(preset_files[i], tmp);
        }
        preset_file_count++;
    }
    closedir(dir);
}

static const char *preset_get_name(int item, void *data, char *buf, size_t len)
{
    (void)data; (void)buf; (void)len;
    if (item < BUILTIN_COUNT)
        return str(builtin_presets[item].lang);
    return preset_files[item - BUILTIN_COUNT];
}

static enum list_row_kind preset_get_kind(int item, void *data)
{
    (void)data;
    return item < BUILTIN_COUNT ? LIST_ROW_ACTION : LIST_ROW_FILE;
}

/* What was there before the list opened, put back on Menu. */
static struct {
    struct eq_band_setting bands[EQ_NUM_BANDS];
    unsigned int precut;
    bool enabled;
    int auditioning;
} saved;

static void restore_saved(void)
{
    memcpy(global_settings.eq_band_settings, saved.bands, sizeof saved.bands);
    global_settings.eq_precut = saved.precut;
    global_settings.eq_enabled = saved.enabled;
    eq_apply();
}

static int preset_action(int action, struct gui_synclist *lists)
{
    if (action == ACTION_STD_MENU)
        return ACTION_STD_CANCEL;

    int sel = gui_synclist_get_sel_pos(lists);
    if (sel != saved.auditioning && action != ACTION_REDRAW)
    {
        saved.auditioning = sel;
        if (sel < BUILTIN_COUNT)
            apply_builtin(sel);
        else
            restore_saved();
    }
    return action;
}

int eq_presets_screen(void)
{
    struct simplelist_info info;
    char path[MAX_PATH];
    bool usb;

    read_preset_files();
    memcpy(saved.bands, global_settings.eq_band_settings, sizeof saved.bands);
    saved.precut = global_settings.eq_precut;
    saved.enabled = global_settings.eq_enabled;
    saved.auditioning = 0;      /* the row the list opens on plays nothing */

    simplelist_info_init(&info, str(LANG_EQUALIZER_PRESETS),
                         BUILTIN_COUNT + preset_file_count, NULL);
    info.get_name = preset_get_name;
    info.get_kind = preset_get_kind;
    info.action_callback = preset_action;
    info.title_icon = Icon_EQ;
    info.selection = 0;
    if (global_settings.talk_menu)
        info.speak_onshow = true;

    usb = simplelist_show_list(&info);
    if (usb || info.selection < 0)
    {
        restore_saved();
        return usb ? MENU_ATTACHED_USB : 0;
    }
    if (info.selection < BUILTIN_COUNT)
    {
        apply_builtin(info.selection);
        strmemccpy(preset_name, str(builtin_presets[info.selection].lang),
                   sizeof preset_name);
        unsaved = false;
    }
    else
    {
        const char *name = preset_files[info.selection - BUILTIN_COUNT];
        restore_saved();
        snprintf(path, sizeof path, EQS_DIR "/%s.cfg", name);
        if (settings_load_config(path, true))
        {
            strmemccpy(preset_name, name, sizeof preset_name);
            unsaved = false;
        }
    }
    settings_save();
    return 0;
}

/** state, colours, fonts and layout **/

enum field { FIELD_FREQ, FIELD_GAIN, FIELD_Q, FIELD_TYPE, FIELD_COUNT };

#define CURVE_X     10
#define CURVE_W     (LCD_WIDTH - 2 * CURVE_X)
#define CURVE_MIN_H 72

static struct eq_curve curve;
static int col_total[CURVE_W];  /* the curve per pixel, hundredths of a dB */
static int col_band[CURVE_W];   /* the selected band's own response */

static struct
{
    bool enhanced;
    int band;
    enum field field;
    bool bypass;                /* the EQ is off to compare */
    bool latched;               /* ...from the menu, until a key */
    bool changed;
    int peak;                  /* hundredths of a dB */
    int font_ui, font_bold, font_small;
    int ui_h, small_h, row, srow;
    unsigned fg, bg, dim, grid, zero, accent, shade, column, warn, warn_shade;
} ed;

static void load_colours(void)
{
    ed.fg = dynamic_colors_resolve(global_settings.fg_color);
    ed.bg = dynamic_colors_resolve(global_settings.bg_color);
    ed.accent = dynamic_colors_resolve(global_settings.lss_color);
    if (color_contrast(ed.accent, ed.bg) < 300)
        ed.accent = ed.fg;
    ed.dim = color_blend(ed.fg, ed.bg, 100);
    ed.grid = color_blend(ed.bg, ed.fg, 28);
    ed.zero = color_blend(ed.bg, ed.fg, 90);
    ed.shade = color_blend(ed.bg, ed.accent, 70);
    ed.column = color_blend(ed.bg, ed.accent, 40);
    ed.warn = LCD_RGBPACK(255, 90, 74);
    ed.warn_shade = color_blend(ed.bg, ed.warn, 110);
}

/* A smaller size of the UI font's family, already loaded by a skin: fonts are
 * named <height>-<family>, so 12-noto-sans goes with 18-noto-sans. Matching
 * the family keeps icon fonts out. */
static int find_label_font(int ui_h)
{
    const char *family = strchr(global_settings.font_file, '-');
    char path[MAX_PATH];

    if (!family || global_settings.font_file[0] < '0'
                || global_settings.font_file[0] > '9')
        return -1;
    for (int h = ui_h * 3 / 4; h >= 10; h--)
    {
        snprintf(path, sizeof path, FONT_DIR "/%d%s.fnt", h, family);
        for (int id = 0; id < MAXFONTS; id++)
            if (font_filename_matches_loaded_id(id, path))
                return id;
    }
    return -1;
}

/* Fonts and row heights. False if the curve would be too short to read. */
static bool load_layout(void)
{
    ed.font_ui = screens[SCREEN_MAIN].getuifont();
    ed.font_bold = font_get_ui_bold();
    ed.font_small = find_label_font(font_get(ed.font_ui)->height);
    if (ed.font_small < 0)
        ed.font_small = ed.font_ui;
    ed.ui_h = font_get(ed.font_ui)->height;
    ed.small_h = font_get(ed.font_small)->height;
    ed.row = ed.ui_h + 6;
    ed.srow = ed.small_h + 5;

    /* Enhanced is the tighter of the two layouts. */
    int top = 4 + ed.row;
    int fields = LCD_HEIGHT - 6 - ed.srow - ed.row;
    int axis = fields - 8 - ed.srow;
    return axis - top - 8 >= CURVE_MIN_H;
}

static void rebuild_curve(void)
{
    eq_curve_init(&curve, eq_dsp_frequency());
    for (int b = 0; b < EQ_NUM_BANDS; b++)
        eq_curve_set_band(&curve, b, &global_settings.eq_band_settings[b]);
    ed.peak = eq_curve_peak_of(&curve, global_settings.eq_band_settings);
}

/** drawing **/

static int text_width(int font, const char *s)
{
    int w;
    lcd_setfont(font);
    lcd_getstringsize(s, &w, NULL);
    return w;
}

/* FG, so an anti-aliased face blends against what is under it. */
static void text(int font, int x, int y, unsigned colour, const char *s)
{
    lcd_setfont(font);
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(colour);
    lcd_putsxy(x, y, s);
    lcd_set_drawmode(DRMODE_SOLID);
}

static void text_right(int font, int right, int y, unsigned colour,
                       const char *s)
{
    text(font, right - text_width(font, s), y, colour, s);
}

/* Anti-aliased, centred at (cx, cy) in sixteenths of a pixel. Coverage is
 * (r + 1/2 - distance), taken without a square root as round_rect.c does:
 * across the one-pixel edge band the distance stays within
 * r' - (r'^2 - d^2) / 2r' of the radius r' = r + 1/2. */
static void fill_circle(int cx, int cy, int r, unsigned colour)
{
    int r16 = r * 16 + 8;

    lcd_set_foreground(colour);
    for (int y = (cy - r16) >> 4; y <= (cy + r16) >> 4; y++)
    {
        int dy = y * 16 + 8 - cy;
        for (int x = (cx - r16) >> 4; x <= (cx + r16) >> 4; x++)
        {
            int dx = x * 16 + 8 - cx;
            int e = r16 * r16 - dx * dx - dy * dy;
            int cov = e <= 0 ? 0 : (e * LCD_BLEND_OPAQUE + 16 * r16) / (32 * r16);
            lcd_blendrect(x, y, 1, 1, cov);
        }
    }
}

static void format_db(char *buf, size_t len, int tenths)
{
    int a = tenths < 0 ? -tenths : tenths;
    snprintf(buf, len, "%s%d.%d dB", tenths > 0 ? "+" : tenths < 0 ? "-" : "",
             a / 10, a % 10);
}

static void format_hz(char *buf, size_t len, int hz)
{
    if (hz < 1000)
        snprintf(buf, len, "%d Hz", hz);
    else if (hz % 1000 == 0)
        snprintf(buf, len, "%d kHz", hz / 1000);
    else if (hz % 100 == 0 || hz >= 10000)
        snprintf(buf, len, "%d.%d kHz", hz / 1000, (hz % 1000 + 50) / 100 % 10);
    else
        snprintf(buf, len, "%d.%02d kHz", hz / 1000, (hz % 1000 + 5) / 10);
}

static void format_hz_short(char *buf, size_t len, int hz)
{
    if (hz < 1000)
        snprintf(buf, len, "%d", hz);
    else if (hz % 1000 == 0 || hz >= 10000)
        snprintf(buf, len, "%dk", (hz + 500) / 1000);
    else
        snprintf(buf, len, "%d.%dk", hz / 1000, hz % 1000 / 100);
}

static int curve_x(int hz)
{
    return CURVE_X + eq_curve_position(hz) * (CURVE_W - 1) / 65536;
}

/* The per-pixel curve, interpolated between points. */
static void fill_columns(void)
{
    for (int px = 0; px < CURVE_W; px++)
    {
        int pos = px * (EQ_CURVE_POINTS - 1) * 256 / (CURVE_W - 1);
        int i = pos >> 8, f = pos & 255;
        int j = i + 1 < EQ_CURVE_POINTS ? i + 1 : i;
        int t0 = eq_curve_total(&curve, i), t1 = eq_curve_total(&curve, j);
        int b0 = curve.band_cdb[ed.band][i], b1 = curve.band_cdb[ed.band][j];

        col_total[px] = t0 + (t1 - t0) * f / 256;
        col_band[px] = b0 + (b1 - b0) * f / 256;
    }
}

struct area { int y, h, range; };

static int area_y(const struct area *a, int cdb)
{
    int y = a->y + a->h / 2 - cdb * (a->h / 2) / (a->range * 100);
    return y < a->y ? a->y : y > a->y + a->h - 1 ? a->y + a->h - 1 : y;
}

/* area_y() in sixteenths of a pixel; row y spans y*16 to y*16 + 15. */
static int area_y16(const struct area *a, int cdb)
{
    int y = (a->y + a->h / 2) * 16 + 8 - cdb * (a->h / 2) * 16 / (a->range * 100);
    int lo = a->y * 16 + 8, hi = (a->y + a->h - 1) * 16 + 8;
    return y < lo ? lo : y > hi ? hi : y;
}

/* One column of the 2-pixel curve, from its midpoints with its neighbours,
 * full rows solid and the two ends blended by how much of them it covers. */
static void stroke_column(int x, int ya, int yc, int yb)
{
    int lo = MIN(MIN(ya, yb), yc) - 16, hi = MAX(MAX(ya, yb), yc) + 16;
    int top = lo >> 4, bot = (hi - 1) >> 4;

    if (top == bot)
    {
        lcd_blendrect(x, top, 1, 1, (hi - lo) * LCD_BLEND_OPAQUE / 16);
        return;
    }
    lcd_blendrect(x, top, 1, 1, (16 - (lo & 15)) * LCD_BLEND_OPAQUE / 16);
    if (bot > top + 1)
        lcd_vline(x, top + 1, bot - 1);
    lcd_blendrect(x, bot, 1, 1,
                  ((hi - 1) - bot * 16 + 1) * LCD_BLEND_OPAQUE / 16);
}

static void draw_curve(const struct area *a)
{
    static const int grid_hz[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
    int zero = area_y(a, 0);
    int clip = !global_settings.eq_auto_headroom
               && ed.peak > (int)global_settings.eq_precut * 10
               ? area_y(a, global_settings.eq_precut * 10) : -1;

    lcd_set_foreground(ed.grid);
    for (unsigned i = 0; i < ARRAYLEN(grid_hz); i++)
        lcd_vline(curve_x(grid_hz[i]), a->y, a->y + a->h - 1);
    for (int d = -a->range; d <= a->range; d += 6)
        if (d)
            lcd_hline(CURVE_X, CURVE_X + CURVE_W - 1, area_y(a, d * 100));

    if (!ed.enhanced && !ed.bypass)
    {
        lcd_set_foreground(ed.column);
        int x = curve_x(global_settings.eq_band_settings[ed.band].cutoff);
        lcd_fillrect(x - 11, a->y, 22, a->h);
    }

    for (int px = 0; px < CURVE_W; px++)
    {
        int x = CURVE_X + px, y = area_y(a, col_total[px]);
        if (clip >= 0 && y < clip)
        {
            lcd_set_foreground(ed.warn_shade);
            lcd_vline(x, y, clip);
        }
        if (ed.enhanced)
        {
            lcd_set_foreground(ed.shade);
            lcd_vline(x, zero, area_y(a, col_band[px]));
        }
        else if (!ed.bypass)
        {
            lcd_set_foreground(ed.shade);
            lcd_vline(x, zero, y);
        }
    }

    lcd_set_foreground(ed.zero);
    lcd_hline(CURVE_X, CURVE_X + CURVE_W - 1, zero);
    if (clip >= 0)
    {
        lcd_set_foreground(ed.warn);
        for (int x = CURVE_X; x < CURVE_X + CURVE_W; x += 6)
            lcd_hline(x, x + 2, clip);
    }

    if (ed.bypass)
    {
        lcd_set_foreground(ed.fg);
        lcd_hline(CURVE_X, CURVE_X + CURVE_W - 1, zero);
        lcd_hline(CURVE_X, CURVE_X + CURVE_W - 1, zero + 1);
    }
    lcd_set_foreground(ed.bypass ? ed.dim : ed.accent);
    for (int px = 0; px < CURVE_W; px++)
    {
        int yc = area_y16(a, col_total[px]);
        int yp = px > 0 ? area_y16(a, col_total[px - 1]) : yc;
        int yn = px < CURVE_W - 1 ? area_y16(a, col_total[px + 1]) : yc;
        if (ed.bypass && (px & 4))
            continue;
        stroke_column(CURVE_X + px, (yp + yc) / 2, yc, (yn + yc) / 2);
    }

    for (int b = 0; b < EQ_NUM_BANDS; b++)
    {
        const struct eq_band_setting *s = &global_settings.eq_band_settings[b];
        int x = curve_x(s->cutoff) * 16 + 8;
        bool sel = b == ed.band;

        if (ed.enhanced)
        {
            int y = area_y16(a, s->gain * 10);
            fill_circle(x, y, sel ? 7 : 5, sel ? ed.fg : s->gain ? ed.fg : ed.dim);
            fill_circle(x, y, sel ? 5 : 3, sel ? ed.accent : s->gain ? ed.fg : ed.bg);
        }
        else if (!ed.bypass)
        {
            int px = (x >> 4) - CURVE_X;
            int y = area_y16(a, col_total[px < 0 ? 0 : px >= CURVE_W ? CURVE_W - 1 : px]);
            if (sel)
            {
                fill_circle(x, y, 6, ed.accent);
                fill_circle(x, y, 4, ed.fg);
            }
            else
                fill_circle(x, y, 3, ed.accent);
        }
    }
}

static int curve_range(void)
{
    int big = ed.peak;
    for (int px = 0; px < CURVE_W; px++)
        if (-col_total[px] > big)
            big = -col_total[px];
    for (int b = 0; b < EQ_NUM_BANDS; b++)
    {
        int g = global_settings.eq_band_settings[b].gain * 10;
        if (g > big || -g > big)
            big = g > 0 ? g : -g;
    }
    return big > 1200 ? 24 : 12;
}

/* The title row: bold on the left, the small font on the right. */
static void draw_title(const char *left, const char *right, unsigned right_colour)
{
    text(ed.font_bold, CURVE_X, 4 + (ed.row - font_get(ed.font_bold)->height) / 2,
         ed.fg, left);
    if (right)
        text_right(ed.font_small, LCD_WIDTH - CURVE_X,
                   4 + (ed.row - ed.small_h) / 2, right_colour, right);
}

/* Frequency labels under the columns: the selected band's first, then any
 * that touch nothing already placed. */
static void draw_freq_labels(int y)
{
    int left[EQ_NUM_BANDS], right[EQ_NUM_BANDS], placed = 0;
    char buf[16];

    for (int n = 0; n < EQ_NUM_BANDS; n++)
    {
        int b = n == 0 ? ed.band : n <= ed.band ? n - 1 : n;
        int w, l, r, i;
        bool sel = b == ed.band && !ed.bypass;

        format_hz_short(buf, sizeof buf,
                        global_settings.eq_band_settings[b].cutoff);
        w = text_width(ed.font_small, buf);
        l = curve_x(global_settings.eq_band_settings[b].cutoff) - w / 2;
        if (l < CURVE_X)
            l = CURVE_X;
        if (l + w > CURVE_X + CURVE_W)
            l = CURVE_X + CURVE_W - w;
        r = l + w;
        for (i = 0; i < placed; i++)
            if (l < right[i] + 4 && r > left[i] - 4)
                break;
        if (i < placed)
            continue;
        left[placed] = l;
        right[placed++] = r;
        text(ed.font_small, l, y, sel ? ed.fg : ed.dim, buf);
    }
}

static void headroom_text(char *buf, size_t len, bool *warn)
{
    char db[16];
    int over = ed.peak - (int)global_settings.eq_precut * 10;

    *warn = false;
    if (global_settings.eq_auto_headroom)
    {
        format_db(db, sizeof db, -(int)global_settings.eq_precut);
        snprintf(buf, len, str(LANG_EQUALIZER_HEADROOM_IS), db);
    }
    else if (over > 5)
    {
        format_db(db, sizeof db, (over + 9) / 10);
        snprintf(buf, len, str(LANG_EQUALIZER_CLIPS), db + 1);
        *warn = true;
    }
    else
        strmemccpy(buf, str(LANG_EQUALIZER_NO_CLIPPING), len);
}

static void draw_simple(void)
{
    const struct eq_band_setting *s = &global_settings.eq_band_settings[ed.band];
    int top = 4 + ed.row;
    int foot = LCD_HEIGHT - 4 - ed.row - ed.srow;
    int lab = foot - 6 - ed.srow;
    struct area a = { top + 4, lab - top - 8, 12 };
    int y1 = foot + (ed.srow - ed.small_h) / 2;
    int y2 = foot + ed.srow + (ed.row - font_get(ed.font_bold)->height) / 2;
    char buf[48], db[16];
    bool warn;

    int match = builtin_match();
    const char *right = preset_name[0] ? preset_name
        : match >= 0 ? (const char *)str(builtin_presets[match].lang)
        : (const char *)str(LANG_EQUALIZER_CUSTOM);
    if (!global_settings.eq_enabled)
        right = str(LANG_EQUALIZER_EQ_OFF);
    draw_title(str(LANG_EQUALIZER), right, ed.dim);
    a.range = curve_range();
    draw_curve(&a);
    draw_freq_labels(lab + (ed.srow - ed.small_h) / 2);

    lcd_set_foreground(ed.grid);
    lcd_hline(CURVE_X, CURVE_X + CURVE_W - 1, foot - 3);

    if (ed.bypass)
    {
        text(ed.font_small, CURVE_X, y1, ed.dim, str(LANG_EQUALIZER_COMPARE));
        text(ed.font_bold, CURVE_X, y2, ed.fg, str(LANG_EQUALIZER_EQ_OFF));
        text_right(ed.font_small, LCD_WIDTH - CURVE_X, y1, ed.dim,
                   str(LANG_EQUALIZER_ANY_KEY));
        return;
    }

    format_hz(buf, sizeof buf, s->cutoff);
    text(ed.font_small, CURVE_X, y1, ed.dim, buf);
    format_db(db, sizeof db, s->gain);
    text(ed.font_bold, CURVE_X, y2, ed.fg, db);

    format_db(db, sizeof db, -(int)global_settings.eq_precut);
    if (global_settings.eq_auto_headroom)
    {
        text_right(ed.font_small, LCD_WIDTH - CURVE_X, y1, ed.dim,
                   str(LANG_EQUALIZER_AUTO_HEADROOM));
        text_right(ed.font_bold, LCD_WIDTH - CURVE_X, y2, ed.fg, db);
    }
    else
    {
        snprintf(buf, sizeof buf, "%s %s", str(LANG_EQUALIZER_PRECUT), db);
        text_right(ed.font_small, LCD_WIDTH - CURVE_X, y1, ed.dim, buf);
        headroom_text(buf, sizeof buf, &warn);
        text_right(ed.font_bold, LCD_WIDTH - CURVE_X, y2,
                   warn ? ed.warn : ed.fg, buf);
    }
}

static int type_lang(int t)
{
    return t == EQ_FILTER_LOW_SHELF ? LANG_EQUALIZER_TYPE_LOW_SHELF
         : t == EQ_FILTER_HIGH_SHELF ? LANG_EQUALIZER_TYPE_HIGH_SHELF
         : LANG_EQUALIZER_TYPE_PEAK;
}

static int field_lang(const struct eq_band_setting *s, enum field f)
{
    return f == FIELD_FREQ ? (s->type == EQ_FILTER_PEAK
                              ? LANG_EQUALIZER_FIELD_CENTRE
                              : LANG_EQUALIZER_FIELD_CORNER)
         : f == FIELD_GAIN ? LANG_GAIN
         : f == FIELD_Q ? LANG_EQUALIZER_BAND_Q
         : LANG_EQUALIZER_FIELD_TYPE;
}

static void draw_enhanced(void)
{
    const struct eq_band_setting *s = &global_settings.eq_band_settings[ed.band];
    int top = 4 + ed.row;
    int fields = LCD_HEIGHT - 6 - ed.srow - ed.row;
    int axis = fields - 8 - ed.srow;
    struct area a = { top + 4, axis - top - 8, 12 };
    char title[24], right[64], part[48], vals[FIELD_COUNT][16];
    int active = 0, x;
    bool warn;

    for (int b = 0; b < EQ_NUM_BANDS; b++)
        if (global_settings.eq_band_settings[b].gain)
            active++;
    snprintf(title, sizeof title, str(LANG_EQUALIZER_BAND), ed.band);
    snprintf(right, sizeof right, str(LANG_EQUALIZER_BANDS_ON), active,
             EQ_NUM_BANDS);
    headroom_text(part, sizeof part, &warn);
    draw_title(title, NULL, 0);
    {
        int ry = 4 + (ed.row - ed.small_h) / 2;
        int pw = text_width(ed.font_small, part);
        int sw = text_width(ed.font_small, " · ");
        int edge = LCD_WIDTH - CURVE_X;
        text(ed.font_small, edge - pw, ry, warn ? ed.warn : ed.dim, part);
        text_right(ed.font_small, edge - pw, ry, ed.dim, " · ");
        text_right(ed.font_small, edge - pw - sw, ry, ed.dim, right);
    }

    a.range = curve_range();
    draw_curve(&a);

    static const int axis_hz[] = { 100, 1000, 10000 };
    for (unsigned i = 0; i < ARRAYLEN(axis_hz); i++)
    {
        format_hz_short(part, sizeof part, axis_hz[i]);
        text(ed.font_small, curve_x(axis_hz[i]) - text_width(ed.font_small, part) / 2,
             axis + (ed.srow - ed.small_h) / 2, ed.dim, part);
    }

    lcd_set_foreground(ed.grid);
    lcd_hline(CURVE_X, CURVE_X + CURVE_W - 1, fields - 4);

    format_hz(vals[FIELD_FREQ], sizeof vals[0], s->cutoff);
    format_db(vals[FIELD_GAIN], sizeof vals[0], s->gain);
    snprintf(vals[FIELD_Q], sizeof vals[0], "%d.%d", s->q / 10, s->q % 10);
    strmemccpy(vals[FIELD_TYPE], str(type_lang(s->type)), sizeof vals[0]);

    x = CURVE_X;
    for (int i = 0; i < FIELD_COUNT; i++)
    {
        bool act = (int)ed.field == i;
        const char *name = str(field_lang(s, i));
        int w = text_width(ed.font_small, name);
        int vw = text_width(act ? ed.font_bold : ed.font_ui, vals[i]);

        if (vw > w)
            w = vw;
        if (act)
        {
            lcd_set_foreground(ed.accent);
            lcd_fillrect(x, fields, 3, ed.srow + ed.row);
        }
        text(ed.font_small, x + 9, fields + (ed.srow - ed.small_h) / 2,
             act ? ed.fg : ed.dim, name);
        text(act ? ed.font_bold : ed.font_ui, x + 9,
             fields + ed.srow + (ed.row - ed.ui_h) / 2,
             act ? ed.fg : ed.dim, vals[i]);
        x += 9 + w + 12;
    }
}

static void draw(void)
{
    lcd_set_viewport(NULL);
    lcd_set_backdrop(NULL);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(ed.bg);
    lcd_fillrect(0, 0, LCD_WIDTH, LCD_HEIGHT);
    fill_columns();
    if (ed.enhanced)
        draw_enhanced();
    else
        draw_simple();
    lcd_setfont(FONT_UI);
    lcd_update();
}

/** changing a band **/

static void band_applied(int band)
{
    const struct eq_band_setting *s = &global_settings.eq_band_settings[band];

    if (!global_settings.eq_enabled)
    {
        global_settings.eq_enabled = true;
        if (!ed.bypass)
            dsp_eq_enable(true);
    }
    dsp_set_eq_coefs(band, s);
    eq_curve_set_band(&curve, band, s);
    ed.peak = eq_curve_peak_of(&curve, global_settings.eq_band_settings);
    if (global_settings.eq_auto_headroom)
    {
        global_settings.eq_precut = eq_curve_headroom(ed.peak);
        dsp_set_eq_precut(global_settings.eq_precut);
    }
    preset_name[0] = '\0';
    ed.changed = unsaved = true;
}

static int step_freq(int hz, int dir)
{
    /* a twelfth of an octave: 2^(1/12) = 1.059463 */
    if (dir > 0)
    {
        int up = (int)(((int64_t)hz * 1059463 + 500000) / 1000000);
        hz = up > hz ? up : hz + 1;
    }
    else
    {
        int down = (int)(((int64_t)hz * 1000000 + 529731) / 1059463);
        hz = down < hz ? down : hz - 1;
    }
    return hz < EQ_CUTOFF_MIN ? EQ_CUTOFF_MIN : hz > EQ_CUTOFF_MAX ? EQ_CUTOFF_MAX : hz;
}

static int step_q(int q, int dir)
{
    /* a quarter of a doubling: 2^(1/4) = 1.189 */
    if (dir > 0)
    {
        int up = (q * 1189 + 500) / 1000;
        q = up > q ? up : q + 1;
    }
    else
    {
        int down = (q * 1000 + 594) / 1189;
        q = down < q ? down : q - 1;
    }
    return q < EQ_Q_MIN ? EQ_Q_MIN : q > EQ_Q_MAX ? EQ_Q_MAX : q;
}

static int step_gain_simple(int g, int dir)
{
    /* to the next 0.5 dB, from wherever Enhanced left it */
    int floor5 = g >= 0 ? g / 5 * 5 : -((-g + 4) / 5 * 5);
    if (g != floor5)
        g = dir > 0 ? floor5 + 5 : floor5;
    else
        g += 5 * dir;
    return g < EQ_GAIN_MIN ? EQ_GAIN_MIN : g > EQ_GAIN_MAX ? EQ_GAIN_MAX : g;
}

/* A field's value, after its name when the field has just been reached */
static void speak_value(const struct eq_band_setting *s, enum field f,
                        bool name)
{
    if (!global_settings.talk_menu)
        return;
    if (name)
        talk_id(field_lang(s, f), false);
    switch (f)
    {
    case FIELD_FREQ: talk_value(s->cutoff, UNIT_HERTZ, name); break;
    case FIELD_GAIN: talk_value_decimal(s->gain, UNIT_DB, 1, name); break;
    case FIELD_Q:    talk_value_decimal(s->q, UNIT_INT, 1, name); break;
    case FIELD_TYPE: talk_id(type_lang(s->type), name); break;
    default: break;
    }
}

/* The filter reached: which, where and how much */
static void speak_band(void)
{
    const struct eq_band_setting *s = &global_settings.eq_band_settings[ed.band];

    if (!global_settings.talk_menu)
        return;
    talk_id(LANG_EQUALIZER_BAND, false);
    talk_number(ed.band, true);
    talk_value(s->cutoff, UNIT_HERTZ, true);
    talk_value_decimal(s->gain, UNIT_DB, 1, true);
}

/* One wheel step, or several when the wheel turns fast. */
static void wheel(int dir)
{
    struct eq_band_setting *s = &global_settings.eq_band_settings[ed.band];
    struct eq_band_setting next = *s;
    enum field f = ed.enhanced ? ed.field : FIELD_GAIN;
    int steps = 1;

#ifdef HAVE_WHEEL_ACCELERATION
    if (ed.enhanced && (f == FIELD_GAIN || f == FIELD_FREQ))
        steps = button_apply_acceleration(get_action_data());
    if (steps < 1)
        steps = 1;
#endif
    switch (f)
    {
    case FIELD_FREQ:
        while (steps--)
            next.cutoff = step_freq(next.cutoff, dir);
        break;
    case FIELD_GAIN:
        if (ed.enhanced)
        {
            next.gain += dir * steps;
            if (next.gain < EQ_GAIN_MIN) next.gain = EQ_GAIN_MIN;
            if (next.gain > EQ_GAIN_MAX) next.gain = EQ_GAIN_MAX;
        }
        else
            next.gain = step_gain_simple(next.gain, dir);
        break;
    case FIELD_Q:
        next.q = step_q(next.q, dir);
        break;
    case FIELD_TYPE:
        next.type = (next.type + (dir > 0 ? 1 : 2)) % 3;
        break;
    default:
        break;
    }

    /* A band that does not fit, from the band list or a .cfg, can still be
     * brought toward 0 dB: a smaller gain never fits less, and 0 fits. */
    if (!memcmp(&next, s, sizeof next))
        return;
    if (!eq_band_fits(&next, curve.fs)
        && (eq_band_fits(s, curve.fs) || abs(next.gain) >= abs(s->gain)))
        return;
    *s = next;
    band_applied(ed.band);
    talk_settle();
    speak_value(s, f, false);
}

static void set_bypass(bool on)
{
    ed.bypass = on;
    dsp_eq_enable(on ? false : global_settings.eq_enabled);
}

/* Saves a preset, and names the title after the file. */
static bool save_preset(void)
{
    char path[MAX_PATH];
    const char *base;
    size_t n;

    if (!eq_save_preset(path, sizeof path))
        return false;
    base = strrchr(path, '/');
    strmemccpy(preset_name, base ? base + 1 : path, sizeof preset_name);
    n = strlen(preset_name);
    if (n > 4 && !strcasecmp(preset_name + n - 4, ".cfg"))
        preset_name[n - 4] = '\0';
    unsaved = false;
    return true;
}

/** the menu, on a held Menu **/

enum { SM_PRESETS, SM_COMPARE, SM_AUTO, SM_ENHANCED, SM_SAVE, SM_RESET, SM_COUNT };

static const int select_menu_lang[SM_COUNT] = {
    LANG_EQUALIZER_PRESETS, LANG_EQUALIZER_COMPARE, LANG_EQUALIZER_AUTO_HEADROOM,
    LANG_EQUALIZER_ENHANCED_MODE, LANG_EQUALIZER_SAVE, LANG_RESET_EQUALIZER,
};

static const char *sm_get_name(int item, void *data, char *buf, size_t len)
{
    (void)data; (void)buf; (void)len;
    return str(select_menu_lang[item]);
}

static enum list_row_kind sm_get_kind(int item, void *data)
{
    (void)data;
    return item == SM_AUTO || item == SM_ENHANCED ? LIST_ROW_TOGGLE
                                                  : LIST_ROW_ACTION;
}

static const char *sm_get_value(int item, void *data, char *buf, size_t len)
{
    (void)data; (void)buf; (void)len;
    if (item == SM_AUTO)
        return str(global_settings.eq_auto_headroom ? LANG_SET_BOOL_YES
                                                    : LANG_SET_BOOL_NO);
    if (item == SM_ENHANCED)
        return str(ed.enhanced ? LANG_SET_BOOL_YES : LANG_SET_BOOL_NO);
    return NULL;
}

static int sm_action(int action, struct gui_synclist *lists)
{
    if (action == ACTION_STD_MENU)
        return ACTION_STD_CANCEL;
    if (action != ACTION_STD_OK)
        return action;

    switch (gui_synclist_get_sel_pos(lists))
    {
    case SM_AUTO:
        global_settings.eq_auto_headroom = !global_settings.eq_auto_headroom;
        eq_apply();
        ed.changed = true;
        if (global_settings.talk_menu)
            talk_id(global_settings.eq_auto_headroom ? LANG_SET_BOOL_YES
                                                     : LANG_SET_BOOL_NO, false);
        return ACTION_REDRAW;
    case SM_ENHANCED:
        ed.enhanced = !ed.enhanced;
        if (global_settings.talk_menu)
            talk_id(ed.enhanced ? LANG_SET_BOOL_YES : LANG_SET_BOOL_NO, false);
        global_settings.eq_editor_mode = ed.enhanced ? EQ_EDITOR_ENHANCED
                                                     : EQ_EDITOR_SIMPLE;
        ed.changed = true;
        return ACTION_REDRAW;
    }
    return action;
}

/* True if USB took the screen. */
static bool select_menu(void)
{
    struct simplelist_info info;

    action_wait_for_release();
    simplelist_info_init(&info, str(LANG_EQUALIZER), SM_COUNT, NULL);
    info.get_name = sm_get_name;
    info.get_kind = sm_get_kind;
    info.get_value = sm_get_value;
    info.action_callback = sm_action;
    info.title_icon = Icon_EQ;
    info.selection = 0;

    if (simplelist_show_list(&info))
        return true;

    switch (info.selection)
    {
    case SM_PRESETS:
        if (eq_presets_screen() == MENU_ATTACHED_USB)
            return true;
        rebuild_curve();
        break;
    case SM_COMPARE:
        set_bypass(true);
        ed.latched = true;
        break;
    case SM_SAVE:
        save_preset();
        break;
    case SM_RESET:
        eq_reset_defaults();
        preset_name[0] = '\0';
        rebuild_curve();
        ed.changed = true;
        break;
    }
    return false;
}

static const char *confirm_get_name(int item, void *data, char *buf, size_t len)
{
    (void)buf; (void)len;
    return str(((const int *)data)[item]);
}

static int confirm_action(int action, struct gui_synclist *lists)
{
    (void)lists;
    return action == ACTION_STD_MENU ? ACTION_STD_CANCEL : action;
}

/* Leaving with a curve that is neither saved nor a built-in. 1 to leave,
 * 0 to stay, MENU_ATTACHED_USB if USB took the screen. */
static int confirm_exit(void)
{
    static const int lang[] = { LANG_EQUALIZER_SAVE, LANG_EQUALIZER_EXIT_UNSAVED };
    struct simplelist_info info;

    if (!unsaved || builtin_match() >= 0)
        return 1;
    simplelist_info_init(&info, str(LANG_EQUALIZER_UNSAVED), ARRAYLEN(lang),
                         (void *)lang);
    info.get_name = confirm_get_name;
    info.action_callback = confirm_action;
    info.title_icon = Icon_EQ;
    info.selection = 0;

    if (simplelist_show_list(&info))
        return MENU_ATTACHED_USB;
    if (info.selection == 0)
        return save_preset() ? 1 : 0;
    return info.selection == 1;
}

/** the input loop **/

int eq_editor_screen(void)
{
    struct viewport vp;
    int ret = 0;
    bool redraw = true, dirty = false;

    if (!load_layout())
    {
        splash(HZ * 2, str(LANG_EQUALIZER_FONT_TOO_LARGE));
        return eq_band_list();
    }

    ed.enhanced = global_settings.eq_editor_mode == EQ_EDITOR_ENHANCED
        || (global_settings.eq_editor_mode == EQ_EDITOR_FOLLOW
            && global_settings.settings_mode == SETTINGS_MODE_EVERYTHING);
    ed.band = 0;
    ed.field = FIELD_GAIN;
    ed.bypass = ed.latched = ed.changed = unsaved = false;
    load_colours();
    rebuild_curve();

    viewportmanager_theme_enable(SCREEN_MAIN, false, &vp);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(true);
#endif
    /* Play held is Compare here, and held long enough it would power off */
    button_set_sw_poweroff_state(false);
    speak_band();

    while (true)
    {
        int action;

        /* Steps already queued are applied before the screen is drawn: a
         * fast turn that drew every one would starve playback. */
        dirty |= redraw;
        if (dirty && button_queue_empty())
        {
            draw();
            dirty = false;
        }
        redraw = true;

        action = get_action(CONTEXT_SETTINGS_EQ, HZ / 4);

        if (action == ACTION_NONE)
        {
            redraw = dynamic_colors_needs_repaint();
            if (redraw)
                load_colours();
            continue;
        }
        if (ed.latched && action != ACTION_REDRAW && !(action & SYS_EVENT))
        {
            ed.latched = false;
            set_bypass(false);
            continue;
        }

        switch (action)
        {
        case ACTION_SETTINGS_INC:
        case ACTION_SETTINGS_INCREPEAT:
            wheel(1);
            break;
        case ACTION_SETTINGS_DEC:
        case ACTION_SETTINGS_DECREPEAT:
            wheel(-1);
            break;
        case ACTION_STD_PREV:
        case ACTION_STD_PREVREPEAT:
        case ACTION_STD_NEXT:
        case ACTION_STD_NEXTREPEAT:
        {
            int band = ed.band + (action == ACTION_STD_PREV
                                  || action == ACTION_STD_PREVREPEAT ? -1 : 1);
            if (band < 0 || band >= EQ_NUM_BANDS)
                redraw = false;
            else
            {
                ed.band = band;
                speak_band();
            }
            break;
        }
        case ACTION_STD_OK:
            if (!ed.enhanced)
            {
                redraw = false;
                break;
            }
            ed.field = (ed.field + 1) % FIELD_COUNT;
            speak_value(&global_settings.eq_band_settings[ed.band], ed.field,
                        true);
            break;
        case ACTION_STD_CONTEXT:
            if (select_menu())
            {
                ret = MENU_ATTACHED_USB;
                goto done;
            }
            break;
        case ACTION_WPS_PLAY:
            wps_do_playpause(false);
            redraw = false;
            break;
        case ACTION_EQ_COMPARE:
            redraw = !ed.bypass;
            if (!ed.bypass)
            {
                set_bypass(true);
                if (global_settings.talk_menu)
                    talk_id(LANG_EQUALIZER_COMPARE, false);
            }
            break;
        case ACTION_EQ_COMPARE_END:
            set_bypass(false);
            break;
        case ACTION_STD_CANCEL:
        {
            int leave = confirm_exit();
            if (leave == MENU_ATTACHED_USB)
                ret = MENU_ATTACHED_USB;
            if (leave)
                goto done;
            break;
        }
        case ACTION_REDRAW:
            break;
        default:
            redraw = false;
            if (default_event_handler(action) == SYS_USB_CONNECTED)
            {
                ret = MENU_ATTACHED_USB;
                goto done;
            }
            break;
        }
    }

done:
    button_set_sw_poweroff_state(true);
    set_bypass(false);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(false);
#endif
    lcd_setfont(FONT_UI);
    viewportmanager_theme_undo(SCREEN_MAIN, true);
    if (ed.changed && ret != MENU_ATTACHED_USB)
        settings_save();
    return ret;
}
