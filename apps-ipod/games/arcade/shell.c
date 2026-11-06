/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade's shell: a cart on the player. It owns the screen, the buttons and
 * the app buffer while the cart runs, and gives the engine (arcade.h) its
 * ticks, its input and the panel.
 *
 * The engine lives wholly in the app buffer, capped at the 5G's 512KB so
 * every cart runs on both players, and never in core_alloc, which would
 * stop the music. Nothing in the buffer is pinned, so USB needs nothing
 * given back.
 *
 * Parts, in order:
 *   - the cart file and the save file
 *   - the engine's host, and the beat: Spike's clock and the beat tracker
 *   - sound: the cart's voices on the beep channel
 *   - present: the 8-bit screen and borders, doubled onto the panel
 *   - input: the buttons and the wheel, as btn() and wheel() see them
 *   - the pause menu, the hold switch and system events
 *   - arcade_play(), the loop
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>           /* atoi */
#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "system.h"           /* TIME_BEFORE, cpu_boost */
#include "kernel.h"
#include "lcd.h"
#include "button.h"
#include "backlight.h"
#include "powermgmt.h"        /* reset_poweroff_timer */
#include "file.h"
#include "dir.h"            /* mkdir */
#include "timefuncs.h"        /* get_time */
#include "lang.h"
#include "settings/settings.h"
#include "settings/settings_list.h"  /* F_T_MASK, F_RGB */
#include "system/app_buffer.h"
#include "system/activity.h"
#include "system/shutdown.h"  /* default_event_handler */
#include "draw/viewport.h"    /* viewportmanager_theme_enable */
#include "widgets/list.h"     /* simplelist */
#include "widgets/splash.h"
#include "root_menu.h"        /* GO_TO_* */
#include "games/arcade/arcade.h"
#include "games/arcade/shell.h"
#include "pcm_mixer.h"
#include "piezo.h"
#include "audio.h"            /* audio_status, audio_current_track */
#include "metadata.h"         /* struct mp3entry */
#include "string-extra.h"     /* strmemccpy */
#include "audio/beat_analyse.h"
#include "audio/beat_track.h"
#include "games/spike/spike_bar.h"
#include "games/spike/spike_clock.h"
#include "games/spike/spike_music.h"
#include "audio/spectrum_meter.h"
#include "database/path_key.h"
#include "database/sound_index.h"

/* The engine's memory: the app buffer, no more than the smaller player's. */
#define ARENA_MAX (512 * 1024)

/* Units one call may run before it is stopped as not responding. Generous
 * until phase 1 measures what a frame costs on each player. */
#define CEILING 2000000

/* Untouched this long, the game pauses: the pause menu hands the
 * backlight, the CPU and idle power-off back to their settings. */
#define IDLE_PAUSE (5 * 60 * HZ)

#define SAVE_DIR "/.rockbox/games/saves"


/* ---- the cart file and the save file ------------------------------------- */

static uint32_t file_read(void *ctx, void *buf, uint32_t n)
{
    int fd = *(int *)ctx;
    if (!buf) {
        off_t at = lseek(fd, 0, SEEK_CUR);
        off_t end = lseek(fd, 0, SEEK_END);
        if (at < 0 || end < 0)
            return 0;
        if ((off_t)n > end - at)
            n = (uint32_t)(end - at);
        lseek(fd, at + n, SEEK_SET);
        return n;
    }
    ssize_t got = read(fd, buf, n);
    return got < 0 ? 0 : (uint32_t)got;
}

static void save_path(struct vm *vm, char *path, size_t size)
{
    const uint8_t *id = arcade_id(vm);
    snprintf(path, size, SAVE_DIR "/%02x%02x%02x%02x%02x%02x%02x%02x.sav",
             id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
}

/* The file: "PBGS", 256 little-endian values, then 32 bytes saying which
 * are fractions. */
static void save_load(struct vm *vm)
{
    char path[MAX_PATH];
    unsigned char b[4 + 256 * 4 + 32];
    int32_t values[256];
    save_path(vm, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    ssize_t got = read(fd, b, sizeof(b));
    close(fd);
    if (got != (ssize_t)sizeof(b) || memcmp(b, "PBGS", 4))
        return;
    for (int i = 0; i < 256; i++) {
        const unsigned char *p = b + 4 + i * 4;
        values[i] = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) |
                              ((uint32_t)p[3] << 24));
    }
    arcade_restore(vm, values, b + 4 + 256 * 4);
}

/* A save the disk refused, tried again at the next chance */
static bool save_owed;

/* Written beside the old one and renamed over it, so a full disk or a
 * short write leaves the old save as it was */
static void save_write(struct vm *vm)
{
    char path[MAX_PATH], part[MAX_PATH];
    unsigned char b[4 + 256 * 4 + 32];
    const uint8_t *frac;
    int dirty;
    const int32_t *values = arcade_save(vm, &dirty, &frac);
    if (!dirty && !save_owed)
        return;
    save_owed = true;
    memcpy(b, "PBGS", 4);
    for (int i = 0; i < 256; i++) {
        uint32_t v = (uint32_t)values[i];
        unsigned char *p = b + 4 + i * 4;
        p[0] = (unsigned char)v;
        p[1] = (unsigned char)(v >> 8);
        p[2] = (unsigned char)(v >> 16);
        p[3] = (unsigned char)(v >> 24);
    }
    memcpy(b + 4 + 256 * 4, frac, 32);
    mkdir("/.rockbox/games");
    mkdir(SAVE_DIR);
    save_path(vm, path, sizeof(path));
    snprintf(part, sizeof(part), "%s.new", path);
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    bool whole = write(fd, b, sizeof(b)) == (ssize_t)sizeof(b);
    if (close(fd) < 0)
        whole = false;
    if (whole && rename(part, path) == 0)
        save_owed = false;
    else
        remove(part);
}

/* ---- the engine's host --------------------------------------------------- */

static void host_print(void *ctx, const char *s, int len)
{
    (void)ctx; (void)s; (void)len;    /* printh is for the IDE's console */
}

static void sound_fill(void);

static void host_yield(void *ctx)
{
    (void)ctx;
    sound_fill();
#ifdef HAVE_LCD_UPDATE_ASYNC
    lcd_update_poll();
#endif
    yield();
}

/* Not forced: on the 5G a forced click never sounds, as piezo_clear()
 * leaves a stop queued, and on the 6G it waits out the last beep inside
 * the tick. Unforced, a click sounds unless the last is still sounding. */
static void host_click(void *ctx, int beep)
{
    (void)ctx;
    piezo_button_beep(beep, false);
}

static void host_date(void *ctx, int32_t out[7])
{
    (void)ctx;
    struct tm *tm = get_time();
    out[0] = tm->tm_year + 1900;
    out[1] = tm->tm_mon + 1;
    out[2] = tm->tm_mday;
    out[3] = tm->tm_hour;
    out[4] = tm->tm_min;
    out[5] = tm->tm_sec;
    out[6] = tm->tm_wday;
}

static int host_playing(void *ctx)
{
    (void)ctx;
    int st = audio_status();
    return (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
}

/* ---- the beat ---------------------------------------------------------------
 * beat, from what Spike plays by: its clock (track time steered by the
 * audio), the beat tracker and its analyser, its downbeat and its mood.
 * Beats are shifted by Spike's audio offset, which is the delay between
 * the player and the ear and not Spike's own. The analyser runs only
 * while the cart listens. The bar is counted from the tracker's lock
 * until the cart asks for the downbeat, and from the downbeat's time
 * after. */

static bool beat_listening;
static long beat_bar_ms;        /* a downbeat's time, the newest asked about */
static long beat_due;           /* the beat the mood is sampled at next */
static int32_t beat_press[5];   /* stamped as each button went down */

static void beat_start(void)
{
    spk_clock_reset();
    beat_track_reset();
    spk_bar_reset();
    spk_music_reset();
    beat_listening = false;
    beat_bar_ms = -1;
    beat_due = -1;
    for (int i = 0; i < 5; i++)
        beat_press[i] = -1;
}

static void beat_listen(void *ctx, int on)
{
    (void)ctx;
    if (on && !beat_listening) {
        beat_analyse_start();
        spk_bar_reset();
        beat_bar_ms = -1;
    } else if (!on && beat_listening)
        beat_analyse_stop();
    beat_listening = on;
}

static void beat_state(void *ctx, int32_t out[6])
{
    (void)ctx;
    struct beat_track bt;
    beat_track_get(&bt);
    out[0] = bt.locked;
    out[1] = (int32_t)bt.bpm;
    out[2] = (int32_t)bt.period_ms;
    out[3] = (int32_t)bt.confidence;
    out[4] = (int32_t)beat_track_fill();
    out[5] = (int32_t)bt.count;
}

static int32_t beat_next(void *ctx, int32_t from, int32_t n, int32_t *in_bar)
{
    (void)ctx;
    unsigned long at;
    int bar;
    long off = global_settings.spike_offset;
    if (from + off < 0 ||
        !beat_track_next((unsigned long)(from + off), n, &at, &bar))
        return -1;
    if (beat_bar_ms < 0) {
        *in_bar = bar;
    } else {
        /* beats since the downbeat, rounded: the tracker's own count can
         * move when it decides again, the downbeat's time does not */
        struct beat_track bt;
        beat_track_get(&bt);
        long p = (long)bt.period_ms, d = (long)at - beat_bar_ms + p / 2;
        long k = d >= 0 ? d / p : -((p - 1 - d) / p);
        *in_bar = (int32_t)(((k % 4) + 4) % 4);
        if (*in_bar == 0 && (long)at > beat_bar_ms)
            beat_bar_ms = (long)at;
    }
    return (int32_t)((long)at - off);
}

static int32_t beat_downbeat(void *ctx, int32_t *margin10)
{
    (void)ctx;
    struct beat_track bt;
    unsigned long at;
    int bar, hits;
    beat_track_get(&bt);
    *margin10 = 0;
    if (!bt.locked || !beat_track_next(spk_clock_ms(), 0, &at, &bar))
        return -1;
    /* a beat the tracker counts as the first of four, twelve bars back:
     * the bar is found from the onsets since then, which have to span 32
     * beats */
    long zero = (long)at - ((long)bar + 48) * (long)bt.period_ms;
    int rot = spk_bar_downbeat(zero, (int)bt.period_ms);
    beat_bar_ms = rot < 0 ? -1 : zero + (long)rot * (long)bt.period_ms;
    int m;
    spk_bar_working(&hits, &m);
    *margin10 = m;
    return rot;
}

static int32_t beat_now(void *ctx)
{
    (void)ctx;
    return (int32_t)spk_clock_now();
}

static void beat_mood(void *ctx, int32_t out[3])
{
    (void)ctx;
    struct spk_mood m;
    spk_music_get(&m);
    out[0] = m.energy;
    out[1] = m.flux;
    out[2] = m.trend;
}

/* The spectrum is worked out from the mixer's audio once a tick, however
 * often a cart asks */
static unsigned long bands_tick, ticks_heard;

static void beat_bands(void *ctx, int32_t *out, int32_t n)
{
    (void)ctx;
    if (bands_tick != ticks_heard) {
        spectrum_meter_peek();
        bands_tick = ticks_heard;
    }
    for (int32_t k = 0; k < n; k++)
        out[k] = spectrum_meter_get_bar(k, n);
}

/* The playing track's record, read from the index once a track: the read
 * may spin the disk up */
static char sound_path[MAX_PATH];
static struct sound_record sound_rec;
static bool sound_have;

static int beat_sound(void *ctx, int32_t out[ARCADE_SOUND_FIELDS])
{
    (void)ctx;
    struct mp3entry *id3 = audio_current_track();
    if (!id3 || !id3->path[0])
        return 0;
    if (strcmp(id3->path, sound_path)) {
        struct sound_index_reader r;
        strmemccpy(sound_path, id3->path, sizeof(sound_path));
        sound_have = false;
        if (sound_index_reader_open(&r) == SOUND_OK) {
            sound_have = sound_index_find(&r, path_key(sound_path),
                                          &sound_rec) &&
                         sound_record_usable(&sound_rec);
            sound_index_reader_close(&r);
        }
    }
    if (!sound_have)
        return 0;
    const struct sound_record *s = &sound_rec;
    bool locked = !(s->flags & SOUND_F_NO_LOCK);
    out[0] = locked ? s->period_ms : 0;
    out[1] = locked ? s->confidence : 0;
    out[2] = (s->flags & SOUND_F_SETTLED) ? 1 : 0;
    out[3] = s->loudness_db10;
    out[4] = s->crest_db;
    out[5] = s->width;
    for (int k = 0; k < 3; k++) {
        out[6 + k] = s->level[k];
        out[9 + k] = s->rate10[k];
    }
    out[12] = s->strength;
    out[13] = s->peakiness;
    for (int k = 0; k < 12; k++)
        out[14 + k] = s->pitch[k];
    out[26] = s->tonic;
    out[27] = (s->flags & SOUND_F_NO_MODE) ? -1 : s->mode;
    out[28] = s->mode_margin;
    out[29] = s->tonal_clarity;
    out[30] = s->harmonic_change;
    return 1;
}

/* The hits in the analyser's history from from to before to, in the
 * cart's time (the offset taken off), oldest first. The history runs
 * newest first, back to some 3.7 s, and reaches the codec's write cursor,
 * so its newest end is audio not yet heard. */
static int32_t beat_ahead(void *ctx, int32_t from, int32_t to, int32_t *out,
                          int32_t max)
{
    (void)ctx;
    static int16_t found[BEAT_HISTORY];
    const struct beat_window *w;
    long off = global_settings.spike_offset;
    int n = 0;
    if (!beat_listening)
        return 0;
    for (int back = 0; back < BEAT_HISTORY &&
                       (w = beat_analyse_window(back)) != NULL; back++) {
        long t = (long)w->time_ms - off;
        if (t < from)
            break;
        if (w->onset && t < to)
            found[n++] = (int16_t)back;
    }
    int32_t k = 0;
    for (int i = n - 1; i >= 0 && k < max; i--, k++) {
        w = beat_analyse_window(found[i]);
        out[k * 3] = (int32_t)((long)w->time_ms - off);
        out[k * 3 + 1] = w->onset;
        out[k * 3 + 2] = w->strength;
    }
    return k;
}

static int32_t beat_horizon(void *ctx)
{
    (void)ctx;
    const struct beat_window *w = beat_listening ? beat_analyse_window(0)
                                                 : NULL;
    return w ? (int32_t)((long)w->time_ms - global_settings.spike_offset)
             : -1;
}

static const struct arcade_beat beat_host = {
    beat_listen, beat_state, beat_next, beat_downbeat, beat_now, beat_mood,
    beat_bands, beat_sound, beat_ahead, beat_horizon
};

/* A button going down, stamped with the track time it arrived at */
static void beat_stamp(struct vm *vm, long b)
{
    int i = b == BUTTON_LEFT ? 0 : b == BUTTON_RIGHT ? 1 :
            b == BUTTON_MENU && arcade_menu_is_game(vm) ? 2 :
            b == BUTTON_PLAY ? 3 : b == BUTTON_SELECT ? 4 : -1;
    if (i >= 0 && beat_press[i] < 0)
        beat_press[i] = (int32_t)spk_clock_now();
}

/* Before each tick: the clock carried to now, the analyser fed while the
 * cart listens, the mood sampled at each beat, and the reading handed over
 * with the presses since the last tick */
static void beat_tick(struct vm *vm)
{
    struct arcade_music m;
    ticks_heard++;
    m.jumped = !spk_clock_tick();
    if (m.jumped) {
        /* a seek, a skip, a new track: the tempo and the bar start again */
        if (beat_listening)
            beat_analyse_start();
        else
            beat_track_reset();
        spk_bar_reset();
        spk_music_reset();
        beat_bar_ms = -1;
        beat_due = -1;
    }
    if (beat_listening) {
        beat_analyse_poll();
        spk_bar_listen();
    }
    unsigned long now = spk_clock_ms(), at;
    if (beat_due >= 0 && (long)now >= beat_due) {
        spk_music_beat();
        beat_due = -1;
    }
    if (beat_due < 0 && beat_track_next(now + 1, 0, &at, NULL))
        beat_due = (long)at;
    unsigned long left = spk_clock_left_ms();
    m.clock = (int32_t)now;
    m.left = left > 0x7fffffff ? 0x7fffffff : (int32_t)left;
    for (int i = 0; i < 5; i++) {
        m.press[i] = beat_press[i];
        beat_press[i] = -1;
    }
    arcade_music(vm, &m);
}

/* ---- sys: settings and phrases ------------------------------------------ */

/* A setting as config.cfg writes it; a number when it is a plain one */
static int host_setting(void *ctx, const char *name, int32_t *num, char *buf,
                        int cap)
{
    (void)ctx;
    const struct settings_list *s = find_setting_by_cfgname(name);
    if (!s)
        return 0;
    cfg_to_string(s, buf, cap);
    int type = s->flags & F_T_MASK;
    if ((type == F_T_INT || type == F_T_UINT) && !(s->flags & F_RGB)) {
        const char *p = buf + (*buf == '-');
        if (*p) {
            while (*p >= '0' && *p <= '9')
                p++;
            if (!*p) {
                *num = atoi(buf);
                return 1;
            }
        }
    }
    return 2;
}

/* The phrase whose built-in English is english, as the loaded language
 * has it: every phrase compared, once for each the cart asks for */
static const char *host_translate(void *ctx, const char *english)
{
    (void)ctx;
    const unsigned char *p = core_language_builtin;
    for (int i = 0; i < LANG_LAST_INDEX_IN_ARRAY; i++) {
        if (*p && !strcmp((const char *)p, english))
            return (const char *)language_strings[i];
        p += strlen((const char *)p) + 1;
    }
    return NULL;
}

static const struct arcade_host host = {
    NULL, host_print, host_yield, host_date, host_click, host_playing,
    &beat_host, host_setting, host_translate
};

/* ---- sound -----------------------------------------------------------------
 * The cart's voices go out on the mixer's beep channel. The game thread
 * renders them into a ring of slots after each tick and at each yield; the
 * mixer's callback, in its interrupt, takes the next filled slot, or
 * silence when the game has fallen behind. The slot it took plays until it
 * asks again, so the game fills at most all but one. */

#define SOUND_FRAMES 512
#define SOUND_SLOTS  4

static int16_t sound_slot[SOUND_SLOTS][SOUND_FRAMES * 2] MEM_ALIGN_ATTR;
static int16_t sound_quiet[SOUND_FRAMES * 2] MEM_ALIGN_ATTR;
static volatile unsigned sound_head, sound_tail;
static struct vm *sound_vm;     /* while a cart runs and may sound */
static bool sound_on;

static void sound_more(const void **start, size_t *size)
{
    if (sound_head != sound_tail)
        *start = sound_slot[sound_tail++ % SOUND_SLOTS];
    else
        *start = sound_quiet;
    *size = sizeof(sound_quiet);
}

static const struct mixer_play_cbs sound_cbs = { sound_more, NULL };

static void sound_stop(void)
{
    if (sound_on)
        mixer_channel_stop(PCM_MIXER_CHAN_BEEP);
    sound_on = false;
    sound_head = sound_tail = 0;
}

static void sound_fill(void)
{
    if (!sound_vm)
        return;
    int rate = (int)mixer_get_frequency();
    bool sounding = false;
    while (sound_head - sound_tail < SOUND_SLOTS - 1) {
        if (!arcade_audio(sound_vm, sound_slot[sound_head % SOUND_SLOTS],
                          SOUND_FRAMES, rate))
            break;
        sound_head++;
        sounding = true;
    }
    /* Started for something to play, and again if a firmware beep, which
     * shares the channel, stopped it -- by then the slots may be full and
     * nothing new rendered; stopped once all is played out */
    bool queued = sound_head != sound_tail;
    if ((sounding && !sound_on) ||
        (queued && sound_on && mixer_channel_status(PCM_MIXER_CHAN_BEEP)
                               == CHANNEL_STOPPED)) {
        mixer_channel_set_amplitude(PCM_MIXER_CHAN_BEEP, MIX_AMP_UNITY);
        mixer_channel_play_data(PCM_MIXER_CHAN_BEEP, &sound_cbs, sound_quiet,
                                sizeof(sound_quiet));
        sound_on = true;
    } else if (!sounding && sound_on && sound_head == sound_tail)
        sound_stop();
}

/* ---- present ---------------------------------------------------------------
 * The 160x120 screen fills the panel, each pixel two wide and two high;
 * only rows drawn since the last present are converted, and they go to
 * the panel as one full-width band, the shape the 5G pushes cheapest. On
 * the 5G the band goes by DMA while the next tick runs, moved along by
 * the script's yields; it is read from the framebuffer, so it lands before
 * anything draws there again. */

static uint32_t lut2[256];

static uint16_t pack(uint32_t c)
{
    return LCD_RGBPACK((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
}

static void present_wait(void)
{
#ifdef HAVE_LCD_UPDATE_ASYNC
    lcd_update_finish();
#endif
}

#ifdef CPU_ARM
void present_row_arm(const uint8_t *src, uint32_t *d0, uint32_t *d1,
                     const uint32_t lut2[256], int n);

/* arcade_present_rows(), each row in assembly (present_arm.S) */
static void present_rows(const struct arcade_screen *s, void *fb, int stride,
                         int r0, int r1)
{
    for (int y = r0; y <= r1; y++) {
        uint16_t *row = (uint16_t *)fb + y * 2 * stride;
        present_row_arm(s->px + y * s->width, (uint32_t *)row,
                        (uint32_t *)(row + stride), lut2, s->width / 8);
    }
}
#else
static void present_rows(const struct arcade_screen *s, void *fb, int stride,
                         int r0, int r1)
{
    arcade_present_rows(s, lut2, fb, stride, r0, r1);
}
#endif

static void present(struct vm *vm, bool all)
{
    const struct arcade_screen *s = arcade_screen(vm);

    if (s->palette_changed || all) {
        arcade_present_lut(s, lut2, pack);
        all = true;
    }
    int r0 = all ? 0 : s->dirty_lo, r1 = all ? s->height - 1 : s->dirty_hi;
    if (r0 <= r1) {
        int stride = (int)(FBADDR(0, 1) - FBADDR(0, 0));
        present_wait();
        present_rows(s, FBADDR(0, 0), stride, r0, r1);
#ifdef HAVE_LCD_UPDATE_ASYNC
        lcd_update_start(r0 * 2, (r1 - r0 + 1) * 2);
#else
        lcd_update_rect(0, r0 * 2, LCD_WIDTH, (r1 - r0 + 1) * 2);
#endif
    }
    arcade_presented(vm);
}

/* ---- input --------------------------------------------------------------- */

static int last_wheel = -1;

/* The wheel's absolute position, 0-95, or -1 untouched; a build with no
 * position to read (the simulator) never has a finger on it. */
static int wheel_position(void)
{
#ifdef HAVE_WHEEL_POSITION
    return wheel_status();
#else
    return -1;
#endif
}

/* The wheel's scroll events, off while a game reads the position. */
static void wheel_events(bool on)
{
#ifdef HAVE_WHEEL_POSITION
    wheel_send_events(on);
#else
    (void)on;
#endif
}

/* A cart that claims MENU gets it as it is let go, for one tick, so the
 * hold that opens the pause menu is never also a press in the game. */
static bool menu_down, menu_paused;
static long menu_since;

/* True once a claimed MENU has been held for a second: the pause menu. */
static bool menu_held_long(int status)
{
    if (!(status & BUTTON_MENU))
        return false;
    if (!menu_down) {
        menu_down = true;
        menu_paused = false;
        menu_since = current_tick;
    }
    if (menu_paused || TIME_BEFORE(current_tick, menu_since + HZ))
        return false;
    menu_paused = true;
    return true;
}

static unsigned buttons(struct vm *vm, int status)
{
    unsigned held = 0;
    if (status & BUTTON_LEFT)   held |= ARCADE_PREV;
    if (status & BUTTON_RIGHT)  held |= ARCADE_NEXT;
    if (status & BUTTON_PLAY)   held |= ARCADE_PLAY;
    if (status & BUTTON_SELECT) held |= ARCADE_SELECT;
    if (arcade_menu_is_game(vm) && menu_down && !(status & BUTTON_MENU)) {
        if (!menu_paused)
            held |= ARCADE_MENU;
        menu_down = false;
    }
    return held;
}

/* The clicks since the last call, from the wheel's absolute position:
 * 96 a turn, clockwise positive. */
static int wheel_clicks(int pos)
{
    int d = 0;
    if (pos >= 0 && last_wheel >= 0) {
        d = pos - last_wheel;
        if (d > 48)
            d -= 96;
        else if (d < -48)
            d += 96;
    }
    last_wheel = pos;
    return d;
}

/* ---- pausing and events -------------------------------------------------- */

enum { PLAYING, RESTART, QUIT, USB, FAILED };

static void hold_backlight(bool hold)
{
    if (hold) {
        if (global_settings.backlight_timeout > 0)
            backlight_set_timeout(0);
        if (global_settings.backlight_timeout_plugged > 0)
            backlight_set_timeout_plugged(0);
    } else {
        backlight_set_timeout(global_settings.backlight_timeout);
        backlight_set_timeout_plugged(
            global_settings.backlight_timeout_plugged);
    }
}

/* The screen handed to the game, and handed back: each once, whichever
 * way the game ends, because the boost and the theme are counted. */
static bool screen_held;

static void take_screen(struct viewport *vp)
{
    if (screen_held)
        return;
    screen_held = true;
    viewportmanager_theme_enable(SCREEN_MAIN, false, vp);
    lcd_set_backdrop(NULL);
    lcd_clear_display();
    lcd_update();
    wheel_events(false);
    hold_backlight(true);
    cpu_boost(true);
}

static void give_screen(void)
{
    if (!screen_held)
        return;
    sound_stop();
    present_wait();
    screen_held = false;
    cpu_boost(false);
    hold_backlight(false);
    wheel_events(true);
    viewportmanager_theme_undo(SCREEN_MAIN, true);
}

/* A system event: saved first, since USB or a shutdown may follow; the
 * game redrawn after, over any splash the event showed. */
static int event(struct vm *vm, long button)
{
    sound_stop();
    present_wait();
    save_write(vm);
    if (default_event_handler(button) == SYS_USB_CONNECTED)
        return USB;
    present(vm, true);
    return PLAYING;
}

/* The pause menu's rows: Resume, the cart's own (menuitem), Restart,
 * Quit. */
static char item_label[5][32];
static int item_index[5], item_count;

static const char *menu_row(int row, void *data, char *buf, size_t len)
{
    (void)data; (void)buf; (void)len;
    if (row == 0)
        return str(LANG_CHESSBOX_MENU_RESUME_GAME);
    if (row <= item_count)
        return item_label[row - 1];
    if (row == item_count + 1)
        return str(LANG_CHESSBOX_MENU_RESTART_GAME);
    return str(LANG_MENU_QUIT);
}

/* An item's function returning true keeps the menu up, relabelled, so a
 * cart can make a setting of its own; anything else resumes. */
static int pause_menu(struct vm *vm, struct viewport *vp)
{
    struct simplelist_info info;
    int selection = 0;
    sound_stop();
    present_wait();
    save_write(vm);
    give_screen();
    for (;;) {
        item_count = 0;
        for (int i = 1; i <= 5; i++)
            if (arcade_menu_item(vm, i, item_label[item_count],
                                 sizeof(item_label[0])))
                item_index[item_count++] = i;
        simplelist_info_init(&info, str(LANG_PLUGIN_GAMES), item_count + 3,
                             NULL);
        info.get_name = menu_row;
        info.selection = selection;
        push_current_activity(ACTIVITY_CONTEXTMENU);
        bool to_root = simplelist_show_list(&info);
        if (get_current_activity() == ACTIVITY_CONTEXTMENU)
            pop_current_activity();
        /* MENU held or USB: out to the root, as everywhere else */
        if (to_root)
            return USB;
        selection = info.selection;
        if (selection == item_count + 1)
            return RESTART;
        if (selection == item_count + 2)
            return QUIT;
        if (selection < 1 || selection > item_count)
            break;
        int r = arcade_menu_choose(vm, item_index[selection - 1], 4);
        if (r < 0)
            return FAILED;
        if (r == 0)
            break;
    }
    take_screen(vp);
    present(vm, true);
    return PLAYING;
}

/* A key from an earphone remote, a dock or a car: acted on as on every
 * other screen, and the game redrawn over anything that showed. */
static void remote_key(struct vm *vm, long b)
{
    default_event_handler(b);
    present(vm, true);
}

/* The hold switch: the game waits, the screen shows it, until released;
 * meanwhile the CPU and the backlight go back to their settings. */
static int hold_pause(struct vm *vm)
{
    int r = PLAYING;
    sound_stop();
    present_wait();
    save_write(vm);
    cpu_boost(false);
    hold_backlight(false);
    while (r == PLAYING && button_hold()) {
        long b = button_get_w_tmo(HZ / 10);
        if (b & SYS_EVENT)
            r = event(vm, b);
        else if (b & BUTTON_MULTIMEDIA)
            default_event_handler(b);
    }
    cpu_boost(true);
    hold_backlight(true);
    if (r == PLAYING)
        present(vm, true);
    return r;
}

/* Everything queued since the last frame. The buttons are read as held
 * state, so their events are only drained; system events are acted on. */
static int drain(struct vm *vm, bool *menu_pressed)
{
    long b;
    while ((b = button_get(false)) != BUTTON_NONE) {
        if (b & SYS_EVENT) {
            int r = event(vm, b);
            if (r != PLAYING)
                return r;
        } else if (b & BUTTON_MULTIMEDIA) {
            remote_key(vm, b);
        } else {
            beat_stamp(vm, b);
            if (b == BUTTON_MENU && !arcade_menu_is_game(vm))
                *menu_pressed = true;
        }
    }
    return PLAYING;
}

/* ---- arcade_play() ------------------------------------------------------- */

static void show_error(struct vm *vm)
{
    const char *file, *fn;
    int line;
    const char *msg = arcade_error(vm, &file, &line, &fn);
    if (file)
        splashf(HZ * 6, "%s:%d: %s", file, line, msg);
    else
        splashf(HZ * 6, "%s", msg);
}

/* One cart from load to leaving; RESTART asks for it again. */
static int run(const char *path, void *mem, size_t size)
{
    struct viewport vp;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        splashf(HZ * 3, "%s", path);
        return QUIT;
    }
    struct vm *vm = arcade_load(mem, (uint32_t)size, file_read, &fd, &host,
                                (uint64_t)current_tick * 2654435761u);
    close(fd);
    if (!vm)
        return QUIT;
    if (arcade_error(vm, NULL, NULL, NULL)) {
        show_error(vm);
        return QUIT;
    }
    save_load(vm);
    save_owed = false;
    arcade_set_ceiling(vm, CEILING);

    take_screen(&vp);
    sound_vm = vm;
    last_wheel = -1;
    menu_down = menu_paused = false;
    beat_start();
    sound_path[0] = '\0';         /* the track's record is read afresh */
    int status = PLAYING;
    if (!arcade_start(vm))
        status = FAILED;
    present(vm, true);

    int rate = arcade_rate(vm);
    long start = current_tick, touched = current_tick;
    unsigned long frame = 0;

    while (status == PLAYING) {
        bool menu_pressed = false;
        long due = start + (long)(((uint64_t)(frame + 1) * HZ) / rate);
        long now = current_tick;

        /* Waiting for the next tick: the last frame lands meanwhile */
        if (TIME_BEFORE(now, due)) {
            sound_fill();
            present_wait();
            now = current_tick;
        }
        if (TIME_BEFORE(now, due)) {
            long b = button_get_w_tmo(due - now);
            if (b & SYS_EVENT) {
                status = event(vm, b);
                continue;
            }
            if (b & BUTTON_MULTIMEDIA) {
                remote_key(vm, b);
                continue;
            }
            beat_stamp(vm, b);
            if (b == BUTTON_MENU && !arcade_menu_is_game(vm))
                menu_pressed = true;
            if (TIME_BEFORE(current_tick, due) && !menu_pressed)
                continue;
        }
        status = drain(vm, &menu_pressed);
        if (status != PLAYING)
            break;

        if (arcade_menu_is_game(vm) && menu_held_long(button_status()))
            menu_pressed = true;
        /* a finger on the wheel counts, though it sends no events */
        if (button_status() || wheel_position() >= 0) {
            touched = current_tick;
            reset_poweroff_timer();
        } else if (TIME_AFTER(current_tick, touched + IDLE_PAUSE))
            menu_pressed = true;
        if (menu_pressed) {
            status = pause_menu(vm, &vp);
            spk_clock_resume();
            start = current_tick - (long)((frame * HZ) / rate);
            touched = current_tick;
            continue;
        }
        if (button_hold()) {
            status = hold_pause(vm);
            spk_clock_resume();
            start = current_tick - (long)((frame * HZ) / rate);
            touched = current_tick;
            continue;
        }

        unsigned held = buttons(vm, button_status());
        int pos = wheel_position();
        frame++;

        /* Behind by a whole tick: one update without drawing, so the game
         * keeps its speed when a frame is slow; further behind than that,
         * the clock gives up the time rather than racing to catch it. */
        long late = current_tick - (start + (long)((frame * HZ) / rate));
        if (late >= HZ / rate) {
            arcade_skipped(vm, (unsigned)(late * rate / HZ));
            arcade_input(vm, held, wheel_clicks(pos), pos);
            beat_tick(vm);
            if (!arcade_tick(vm, 0)) {
                status = FAILED;
                break;
            }
            frame++;
            if (late >= 2 * HZ / rate)
                start = current_tick - (long)((frame * HZ) / rate);
        }
        arcade_input(vm, held, wheel_clicks(pos), pos);
        beat_tick(vm);
        if (!arcade_tick(vm, 1)) {
            status = FAILED;
            break;
        }
        if (arcade_rate(vm) != rate) {
            rate = arcade_rate(vm);
            start = current_tick - (long)((frame * HZ) / rate);
        }
        present(vm, false);
        sound_fill();
        arcade_idle(vm);
        /* Once a frame whatever the cart does: a cart always behind its
         * clock never waits above, and one light on script never yields
         * inside a tick, so without this nothing else would run. */
        yield();
        if (arcade_quit_asked(vm))
            status = QUIT;
    }

    if (status == FAILED)
        present(vm, false);
    beat_listen(NULL, 0);
    give_screen();
    sound_vm = NULL;
    if (status == FAILED) {
        show_error(vm);
        status = QUIT;
    }
    save_write(vm);
    return status;
}

int arcade_play(const char *path)
{
    size_t size;
    void *mem = app_try_claim_buffer(&size, "arcade");
    if (!mem) {
        splash(HZ * 2, ID2P(LANG_PLUGIN_GAMES));
        return GO_TO_PREVIOUS;
    }
    /* The engine needs 8-byte alignment. */
    uintptr_t skew = (8 - ((uintptr_t)mem & 7)) & 7;
    mem = (char *)mem + skew;
    size -= skew;
    if (size > ARENA_MAX)
        size = ARENA_MAX;

    int status;
    do
        status = run(path, mem, size);
    while (status == RESTART);

    app_release_buffer("arcade");
    return status == USB ? GO_TO_ROOT : GO_TO_PREVIOUS;
}
