/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * What CheckWPS links instead of the rest of the firmware.
 *
 * The parser is the real one from apps-ipod/skin/, so it reaches for whatever
 * the running firmware would have: a buflib heap, a status bar, the playback
 * engine, and every callback named by the settings table. None of that is
 * needed to answer "does this skin parse", so it is answered here instead.
 *
 * The rule is the one apps-ipod/sim/README.md states: a stub must fail the way
 * its caller already handles. Each group below says what its callers do with
 * the answer.
 *
 * Parts, in order:
 *   - the allocator, which always fails, and the bitmap loader, which does not
 *   - screen furniture: the status bar, the skinned list, the theme
 *   - the two settings lookups the parser makes
 *   - the settings table's callbacks, which are addresses and nothing more
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "config.h"
#include "file.h"
#include "lcd.h"
#include "font.h"
#include "buflib.h"
#include "debug.h"
#include "core_alloc.h"
#include "system/app_util.h"
#include "system/app_buffer.h"
#include "system/debug_log.h"
#include "settings/settings.h"
#include "settings/settings_list.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "draw/icon.h"
#include "widgets/list.h"
#include "skin/skin_engine.h"
#include "checkwps.h"

extern bool debug_wps;   /* checkwps.c: the -v flag */

/* ------------------------------------------------------------------------
 * The allocator, and the bitmap loader
 * --------------------------------------------------------------------- */

/* Nothing the parser allocates from the core is read back before it returns.
 * A failed core_alloc() leaves the skin tree in the parse buffer, which is
 * where CheckWPS looks at it, and leaves wps_data->wps_loaded false, which
 * only the render side asks about. So the allocator refuses everything and
 * the handle table is never reached. */
struct buflib_context core_ctx;

int core_alloc(size_t size) { (void)size; return 0; }

int core_alloc_ex(size_t size, struct buflib_callbacks *ops)
{
    (void)size; (void)ops; return 0;
}

int core_free(int handle) { (void)handle; return 0; }
void core_pin(int handle) { (void)handle; }
void core_unpin(int handle) { (void)handle; }

/* The one allocation that has to answer truthfully. Every skin bitmap goes
 * through here and a negative return aborts the parse -- which is the right
 * answer for a bitmap that is missing, and the wrong one for a bitmap that is
 * merely too large to hold. Opening the file settles the first question, and
 * CheckWPS has no opinion on the second. */
int core_load_bmp(const char *filename, struct bitmap *bm, const int bmformat,
                  ssize_t *buf_reqd, struct buflib_callbacks *ops)
{
    int fd = open(filename, O_RDONLY);

    (void)bm; (void)bmformat; (void)ops;

    if (fd < 0)
    {
        *buf_reqd = CLB_READ_ERR;
        return CLB_ALOC_ERR;
    }
    close(fd);
    *buf_reqd = 0;
    return 1;
}

/* The parse buffer, which the parser asks the application layer for. */
static char parse_buffer[512 * 1024];

void *app_get_buffer(size_t *buffer_size, const char *owner)
{
    (void)owner;
    *buffer_size = sizeof(parse_buffer);
    return parse_buffer;
}

/* ------------------------------------------------------------------------
 * Screen furniture
 * --------------------------------------------------------------------- */

/* The info viewport is the one cross-file channel between skins: every
 * viewport a .wps declares without colours of its own is filled from this one
 * by viewport_set_defaults(). Name an .sbs before a .wps on the command line
 * and checkwps.c stashes the .sbs's %Vi here, so the .wps resolves the way it
 * does on the player; with no .sbs it stays empty and viewport_set_defaults()
 * falls back to the full screen.
 *
 * The title is a separate matter: the parser's %Lt handling records a request
 * that nothing later reads. */
struct viewport *sb_skin_get_info_vp(enum screen_type screen)
{
    (void)screen;
    return checkwps_have_sbs_info_vp ? &checkwps_sbs_info_vp : NULL;
}

const char *sb_get_persistent_title(enum screen_type screen)
{
    (void)screen; return NULL;
}

bool sb_set_title_text(const char *title, enum themable_icons icon,
                       enum screen_type screen)
{
    (void)title; (void)icon; (void)screen; return false;
}

int sb_get_backdrop(enum screen_type screen) { (void)screen; return -1; }
void sb_skin_has_title(enum screen_type screen) { (void)screen; }
void sb_skin_update(enum screen_type screen, bool force)
{
    (void)screen; (void)force;
}
void sb_skin_force_next_update(void) { }

void do_sbs_update_callback(unsigned short id, void *param)
{
    (void)id; (void)param;
}

void skinlist_set_cfg(enum screen_type screen, struct listitem_viewport_cfg *cfg)
{
    (void)screen; (void)cfg;
}

void skin_request_full_update(enum skinnable_screens skin) { (void)skin; }
bool skin_take_dirty(enum screen_type screen) { (void)screen; return false; }
bool skin_flush_inhibited(void) { return false; }
void skin_flush_dirty(void) { }
void wps_playlist_percent_enable(void) { }

/* The framebuffer a viewport points at when it has none of its own. Nothing
 * here draws, so it only has to exist. */
struct frame_buffer_t lcd_framebuffer_default;

/* font_load() in checkwps.c resolves the path and checks the bundle; the
 * glyph budget is a buffer size, and there is no buffer. */
int font_load_ex(const char *path, size_t buffer_size, int glyphs)
{
    (void)buffer_size; (void)glyphs;
    return font_load(path);
}

/* Event subscriptions, the tick and the yield: the viewport manager and the
 * alternating-sublines parser want all three, and nothing runs concurrently
 * with a parse. */
bool add_event(unsigned short id,
               void (*handler)(unsigned short id, void *event_data))
{
    (void)id; (void)handler; return true;
}

void remove_event(unsigned short id,
                  void (*handler)(unsigned short id, void *data))
{
    (void)id; (void)handler;
}

void send_event(unsigned short id, void *data) { (void)id; (void)data; }
void yield(void) { }
volatile long current_tick = 0;

void splashf(int ticks, const char *fmt, ...) { (void)ticks; (void)fmt; }

/* firmware/debug.c formats into a buffer and hands it to a debug() that does
 * nothing on a native target, which is where the reason for a failure after
 * the parse -- a missing bitmap, an unloadable font -- goes. Here it goes to
 * stderr under -v, which is what turns a bare "WPS parsing failure" into a
 * sentence naming the file. A skin that parses cleanly is chatty enough that
 * this is not on by default. */
void debugf(const char *fmt, ...)
{
    va_list ap;

    if (!debug_wps)
        return;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void ldebugf(const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    if (!debug_wps)
        return;

    fprintf(stderr, "%s:%d ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void debug_init(void) { }

void panicf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "PANIC: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(4);
}

/* ------------------------------------------------------------------------
 * The two settings lookups
 * --------------------------------------------------------------------- */

/* settings_list.c is linked for its table; settings.c is not, because it is
 * the front half of the settings system and reaches most of the application.
 * These two are all of it the parser calls, and both are a walk of the table.
 * %St and %pb(...,setting) are checked for existence at parse time and read no
 * further, so a name that is in the table is the whole answer. */
const struct settings_list *find_setting(const void *variable)
{
    for (int i = 0; i < nb_settings; i++)
        if (settings[i].setting == variable)
            return &settings[i];

    return NULL;
}

const struct settings_list *find_setting_by_cfgname(const char *name)
{
    for (int i = 0; i < nb_settings; i++)
        if (settings[i].cfg_name && !strcasecmp(settings[i].cfg_name, name))
            return &settings[i];

    return NULL;
}

/* ------------------------------------------------------------------------
 * The settings table's callbacks
 *
 * Every entry in settings[] names the function that applies it, and those live
 * across the whole application. CheckWPS reads the table by cfg name and never
 * calls one, so each needs an address and nothing else. A new setting whose
 * callback is not here fails the link, which is where it should fail.
 * --------------------------------------------------------------------- */

void accessory_supply_set(bool on) { (void)on; }
void audio_flush_and_reload_tracks(void) { }
void audio_set_playback_frequency(unsigned int rate) { (void)rate; }
int  audio_status(void) { return 0; }
int  battery_default_capacity(void) { return 0; }
void backlight_set_brightness(int val) { (void)val; }
void backlight_set_fade_in(int value) { (void)value; }
void backlight_set_fade_out(int value) { (void)value; }
void backlight_set_on_button_hold(int index) { (void)index; }
void backlight_set_timeout(int value) { (void)value; }
void backlight_set_timeout_plugged(int value) { (void)value; }
void db_summary_invalidate(void) { }
void debug_log_restart(enum debug_log_id id) { (void)id; }
void dsp_afr_enable(int var) { (void)var; }
void dsp_dither_enable(bool enable) { (void)enable; }
void dsp_pbe_enable(int var) { (void)var; }
void dsp_pbe_precut(int var) { (void)var; }
void dsp_set_compressor(const struct compressor_settings *s) { (void)s; }
void dsp_set_crossfeed_cross_params(long lf_gain, long hf_gain, long cutoff)
{
    (void)lf_gain; (void)hf_gain; (void)cutoff;
}
void dsp_set_crossfeed_direct_gain(int gain) { (void)gain; }
void dsp_set_crossfeed_type(int type) { (void)type; }
void dsp_set_eq_precut(int precut) { (void)precut; }
void dsp_surround_enable(int var) { (void)var; }
void dsp_surround_mix(int var) { (void)var; }
void dsp_surround_set_balance(int delay_ms) { (void)delay_ms; }
void dsp_surround_set_cutoff(int frq_l, int frq_h) { (void)frq_l; (void)frq_h; }
void dsp_surround_side_only(bool var) { (void)var; }
void eq_enabled_option_callback(bool enabled) { (void)enabled; }

const char *eq_precut_format(char *buffer, size_t buffer_size, int value,
                             const char *unit)
{
    (void)buffer_size; (void)value; (void)unit;
    return buffer;
}

void iap_bitrate_set(int ratenum) { (void)ratenum; }
void lcd_bidir_scroll(int threshold) { (void)threshold; }
void lcd_scroll_delay(int ms) { (void)ms; }
void lcd_scroll_speed(int speed) { (void)speed; }
void lcd_scroll_step(int pixels) { (void)pixels; }
void lcd_set_sleep_after_backlight_off(int seconds) { (void)seconds; }
void lineout_set(bool on) { (void)on; }
void peak_meter_set_clip_hold(int time) { (void)time; }
void playback_update_aa_dims(void) { }
struct playlist_info *playlist_get_current(void) { return NULL; }

int playlist_randomise(struct playlist_info *playlist, unsigned int seed,
                       bool start_current)
{
    (void)playlist; (void)seed; (void)start_current; return 0;
}

int playlist_sort(struct playlist_info *playlist, bool start_current)
{
    (void)playlist; (void)start_current; return 0;
}

void reload_directory(void) { }
void replaygain_update(void) { }

bool root_menu_is_changed(void *setting, void *defaultval)
{
    (void)setting; (void)defaultval; return false;
}

void root_menu_load_from_cfg(void *setting, char *value)
{
    (void)setting; (void)value;
}

void root_menu_set_audiobooks_row(bool on) { (void)on; }

void root_menu_set_default(void *setting, void *defaultval)
{
    (void)setting; (void)defaultval;
}

char *root_menu_write_to_cfg(void *setting, char *buf, int buf_len)
{
    (void)setting; (void)buf_len; return buf;
}

void set_albumart_mode(int setting) { (void)setting; }
void set_battery_capacity(int capacity) { (void)capacity; }
void set_keypress_restarts_sleep_timer(bool enable) { (void)enable; }
void set_poweroff_timeout(int timeout) { (void)timeout; }
int  sound_max(int setting) { (void)setting; return 0; }
void sound_set_channels(int value) { (void)value; }
void tag_trim_init(void) { }

int talk_time_intervals(long time, int unit_idx, bool enqueue)
{
    (void)time; (void)unit_idx; (void)enqueue; return 0;
}

int talk_value_decimal(long n, int unit, int decimals, bool enqueue)
{
    (void)n; (void)unit; (void)decimals; (void)enqueue; return 0;
}

void usb_set_audio(int value) { (void)value; }
void usb_set_hid(bool enable) { (void)enable; }
void usb_set_mode(int mode) { (void)mode; }
void voice_set_mixer_level(int percent) { (void)percent; }

bool wps_context_menu_is_changed(void *setting, void *defaultval)
{
    (void)setting; (void)defaultval; return false;
}

void wps_context_menu_load_from_cfg(void *setting, char *value)
{
    (void)setting; (void)value;
}

void wps_context_menu_set_default(void *setting, void *defaultval)
{
    (void)setting; (void)defaultval;
}

char *wps_context_menu_write_to_cfg(void *setting, char *buf, int buf_len)
{
    (void)setting; (void)buf_len; return buf;
}
