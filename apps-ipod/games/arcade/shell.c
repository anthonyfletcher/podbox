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
 *   - the engine's host, and the beat: the music clock and the beat tracker
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
#include "usb.h"                /* usb_inserted */
#include "lcd.h"
#include "font.h"
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
#include "games/arcade/shell_tracks.h"
#include "games/arcade/shell_score.h"
#include "games/arcade/shell_opts.h"
#include "games/arcade/shell_suspend.h"
#include "games/arcade/shell_store.h"
#include "games/arcade/shell_font.h"
#include "speech/talk.h"           /* talk_text, talk_shutup */
#include "audio/voice_thread.h"    /* voice_is_playing */
#include "pcm_mixer.h"
#include "piezo.h"
#include "audio.h"            /* audio_status, audio_current_track */
#include "metadata.h"         /* struct mp3entry */
#include "string-extra.h"     /* strmemccpy */
#include "audio/beat_analyse.h"
#include "audio/beat_track.h"
#include "games/arcade/music_bar.h"
#include "games/arcade/music_clock.h"
#include "games/arcade/music_mood.h"
#include "audio/spectrum_meter.h"
#include "database/path_key.h"
#include "database/sound_index.h"

/* The engine's memory: the app buffer, no more than the smaller player's. */
#define ARENA_MAX (512 * 1024)

/* Units one call may run before it is stopped as not responding. Generous
 * until phase 1 measures what a frame costs on each player. */
#define CEILING 2000000

/* A call running this long is stopped whatever its units say: a native's
 * work on the disk or the library can cost far more time than its units.
 * A dialog's frames don't count, since they wait on the owner. */
#define STALL (10 * HZ)

/* Untouched this long, the game pauses: the pause menu hands the
 * backlight, the CPU and idle power-off back to their settings. */
#define IDLE_PAUSE (5 * 60 * HZ)


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

/* The cart file, opened again for the maps and images it reads as it
 * plays: on the 5G a read may wait for the disk to spin up */
static char cart_path[MAX_PATH];

static uint32_t host_cart_read(void *ctx, uint32_t at, void *buf, uint32_t n)
{
    (void)ctx;
    int fd = open(cart_path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t got = lseek(fd, at, SEEK_SET) == (off_t)at ? read(fd, buf, n) : -1;
    close(fd);
    return got < 0 ? 0 : (uint32_t)got;
}

void shell_id_path(char *path, size_t size, const char *dir,
                   const uint8_t *id, const char *ext)
{
    snprintf(path, size, "%s/%02x%02x%02x%02x%02x%02x%02x%02x%s", dir,
             id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], ext);
}

bool shell_write_file(const char *dir, const char *path, const void *a,
                      size_t na, const void *b, size_t nb)
{
    char part[MAX_PATH];
    mkdir("/.rockbox/games");
    mkdir(dir);
    snprintf(part, sizeof(part), "%s.new", path);
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    bool whole = write(fd, a, na) == (ssize_t)na &&
                 (!nb || write(fd, b, nb) == (ssize_t)nb);
    if (close(fd) < 0)
        whole = false;
    if (whole && rename(part, path) == 0)
        return true;
    remove(part);
    return false;
}

static void save_path(struct vm *vm, char *path, size_t size)
{
    shell_id_path(path, size, SAVE_DIR, arcade_id(vm), ".sav");
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
    for (int i = 0; i < 256; i++)
        values[i] = (int32_t)load_le32(b + 4 + i * 4);
    arcade_restore(vm, values, b + 4 + 256 * 4);
}

/* A save the disk refused, tried again at the next chance */
static bool save_owed;

/* Written beside the old one and renamed over it, so a full disk or a
 * short write leaves the old save as it was */
static void save_write(struct vm *vm)
{
    char path[MAX_PATH];
    unsigned char b[4 + 256 * 4 + 32];
    const uint8_t *frac;
    int dirty;
    const int32_t *values = arcade_save(vm, &dirty, &frac);
    if (!dirty && !save_owed)
        return;
    save_owed = true;
    memcpy(b, "PBGS", 4);
    for (int i = 0; i < 256; i++)
        store_le32(b + 4 + i * 4, (uint32_t)values[i]);
    memcpy(b + 4 + 256 * 4, frac, 32);
    save_path(vm, path, sizeof(path));
    if (shell_write_file(SAVE_DIR, path, b, sizeof(b), NULL, 0))
        save_owed = false;
}

/* ---- the engine's host --------------------------------------------------- */

static void host_print(void *ctx, const char *s, int len)
{
    (void)ctx; (void)s; (void)len;    /* printh is for the IDE's console */
}

static void sound_fill(void);

static struct vm *sound_vm;     /* while a cart runs and may sound */
static long call_began;         /* the current tick of a call or dialog frame */

void shell_walk_yield(void)
{
    call_began = current_tick;
    yield();
}

static void host_yield(void *ctx)
{
    (void)ctx;
    if (sound_vm && TIME_AFTER(current_tick, call_began + STALL))
        arcade_set_ceiling(sound_vm, 0);
    sound_fill();
#ifdef HAVE_LCD_UPDATE_ASYNC
    lcd_update_poll();
#endif
    yield();
}

/* speak: the player's synthesiser, which says nothing while it is off */
static void host_say(void *ctx, const char *text, int queue)
{
    (void)ctx;
    talk_text(text, queue != 0);
}

static int host_speaking(void *ctx)
{
    (void)ctx;
    return voice_is_playing();
}

static void host_hush(void *ctx)
{
    (void)ctx;
    talk_shutup();
}

static int host_speech_on(void *ctx)
{
    (void)ctx;
    return global_settings.talk_menu;
}

static const struct arcade_speak speak_host = {
    host_say, host_speaking, host_hush, host_speech_on
};

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
 * beat: the music clock (track time steered by the audio, music_clock.c),
 * the beat tracker and its analyser, the downbeat (music_bar.c) and the
 * mood (music_mood.c). Beats are the tracker's, unshifted: what a cart's
 * presses are judged against. The analyser runs only
 * while the cart listens. The bar is counted from the tracker's lock
 * until the cart asks for the downbeat, and from the downbeat's time
 * after. */

static bool beat_listening;
static long beat_bar_ms;        /* a downbeat's time, the newest asked about */
static long beat_due;           /* the beat the mood is sampled at next */
static int32_t beat_press[5];   /* stamped as each button went down */

static void beat_start(void)
{
    music_clock_reset();
    beat_track_reset();
    music_bar_reset();
    music_mood_reset();
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
        music_bar_reset();
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
    if (from < 0 || !beat_track_next((unsigned long)from, n, &at, &bar))
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
    return (int32_t)at;
}

static int32_t beat_downbeat(void *ctx, int32_t *margin10)
{
    (void)ctx;
    struct beat_track bt;
    unsigned long at;
    int bar, hits;
    beat_track_get(&bt);
    *margin10 = 0;
    if (!bt.locked || !beat_track_next(music_clock_ms(), 0, &at, &bar))
        return -1;
    /* a beat the tracker counts as the first of four, twelve bars back:
     * the bar is found from the onsets since then, which have to span 32
     * beats */
    long zero = (long)at - ((long)bar + 48) * (long)bt.period_ms;
    int rot = music_bar_downbeat(zero, (int)bt.period_ms);
    beat_bar_ms = rot < 0 ? -1 : zero + (long)rot * (long)bt.period_ms;
    int m;
    music_bar_working(&hits, &m);
    *margin10 = m;
    return rot;
}

static int32_t beat_now(void *ctx)
{
    (void)ctx;
    return (int32_t)music_clock_now();
}

static void beat_mood(void *ctx, int32_t out[3])
{
    (void)ctx;
    struct music_mood m;
    music_mood_get(&m);
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
    shell_sound_out(&sound_rec, out);
    return 1;
}

/* The hits in the analyser's history from from to before to, in the
 * cart's time, oldest first. The history runs
 * newest first, back to some 3.7 s, and reaches the codec's write cursor,
 * so its newest end is audio not yet heard. */
static int32_t beat_ahead(void *ctx, int32_t from, int32_t to, int32_t *out,
                          int32_t max)
{
    (void)ctx;
    static int16_t found[BEAT_HISTORY];
    const struct beat_window *w;
    int n = 0;
    if (!beat_listening)
        return 0;
    for (int back = 0; back < BEAT_HISTORY &&
                       (w = beat_analyse_window(back)) != NULL; back++) {
        long t = (long)w->time_ms;
        if (t < from)
            break;
        if (w->onset && t < to)
            found[n++] = (int16_t)back;
    }
    int32_t k = 0;
    for (int i = n - 1; i >= 0 && k < max; i--, k++) {
        w = beat_analyse_window(found[i]);
        out[k * 3] = (int32_t)w->time_ms;
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
    return w ? (int32_t)w->time_ms : -1;
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
        beat_press[i] = (int32_t)music_clock_now();
}

/* Before each tick: the clock carried to now, the analyser fed while the
 * cart listens, the mood sampled at each beat, and the reading handed over
 * with the presses since the last tick */
static void beat_tick(struct vm *vm)
{
    struct arcade_music m;
    ticks_heard++;
    m.jumped = !music_clock_tick();
    m.changed = shell_tracks_tick();
    if (m.jumped) {
        /* a seek, a skip, a new track: the tempo and the bar start again */
        if (beat_listening)
            beat_analyse_start();
        else
            beat_track_reset();
        music_bar_reset();
        music_mood_reset();
        beat_bar_ms = -1;
        beat_due = -1;
    }
    if (beat_listening) {
        beat_analyse_poll();
        music_bar_listen();
    }
    unsigned long now = music_clock_ms(), at;
    if (beat_due >= 0 && (long)now >= beat_due) {
        music_mood_beat();
        beat_due = -1;
    }
    if (beat_due < 0 && beat_track_next(now + 1, 0, &at, NULL))
        beat_due = (long)at;
    unsigned long left = music_clock_left_ms();
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

/* playback.toggle(), next() and prev(): the remote's keys */
/* One press a kernel tick at most: each is a message to the audio thread,
 * whose queue a cart pressing in a loop would overrun */
static void host_control(void *ctx, int what)
{
    static const long keys[] = { BUTTON_MULTIMEDIA_PLAYPAUSE,
                                 BUTTON_MULTIMEDIA_NEXT,
                                 BUTTON_MULTIMEDIA_PREV };
    static long last = -1;
    (void)ctx;
    if (what < 0 || what >= (int)ARRAYLEN(keys) || current_tick == last)
        return;
    last = current_tick;
    default_event_handler(keys[what]);
}

static int host_frame(void *ctx, unsigned *held, int *wheel);

static const struct arcade_host host = {
    NULL, host_print, host_yield, host_date, host_click, host_playing,
    &beat_host, host_setting, host_translate, host_control, &shell_tracks,
    host_frame, &speak_host, &shell_store, shell_glyph,
    NULL, NULL,                 /* watch and prof: the IDE's alone */
    host_cart_read
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

/* An achievement just earned: a banner across the top of the game for a
 * while, in the dialogs' style, laid over each frame as it is presented
 * and taken off again, so the game's own pixels are never changed. */
#define BANNER_TIME (HZ * 5 / 2)

static int banner_ach = -1;         /* the achievement shown, -1 none */
static long banner_until;           /* its end, pushed back by any time */
static long banner_away;            /* the game spent off the screen */
static int banner_rows;             /* the game's rows it last covered */

/* The banner laid over the game for this frame: the rows to present for
 * it, 0 none -- once more after it ends, to put the game back. A new one
 * is started when one is owed. */
static int banner_due(struct vm *vm)
{
    if (banner_ach >= 0 && TIME_AFTER(current_tick, banner_until)) {
        banner_ach = -1;
        return banner_rows;
    }
    if (banner_ach < 0) {
        banner_ach = arcade_achieved(vm);
        if (banner_ach < 0)
            return 0;
        banner_until = current_tick + BANNER_TIME;
        if (global_settings.talk_menu) {
            talk_text(str(LANG_ACH_EARNED), false);
            talk_text(arcade_scores(vm, NULL)->ach[banner_ach].title, true);
        }
    }
    int rows = arcade_banner(vm, str(LANG_ACH_EARNED),
                             arcade_scores(vm, NULL)->ach[banner_ach].title);
    if (rows > banner_rows)
        banner_rows = rows;
    return banner_rows;
}

/* The screen as last presented, a word at a time, once seen_valid; any
 * present of the whole screen refreshes it. */
static uint32_t seen[160 * 120 / 4];
static bool seen_valid;
/* the next of the rows that go every frame, SWEEP_ROWS of them */
#define SWEEP_ROWS 4
static int sweep;

/* Whether the row now differs from the row was, n words each; a row that
 * does is copied over was. */
static bool row_changed(uint32_t *was, const uint32_t *now, int n)
{
    for (int i = 0; i < n; i += 4)
        if ((was[i] ^ now[i]) | (was[i + 1] ^ now[i + 1]) |
            (was[i + 2] ^ now[i + 2]) | (was[i + 3] ^ now[i + 3])) {
            memcpy(was + i, now + i, (size_t)(n - i) * 4);
            return true;
        }
    return false;
}

static void present(struct vm *vm, bool all)
{
    const struct arcade_screen *s = arcade_screen(vm);

    if (s->palette_changed || all) {
        arcade_present_lut(s, lut2, pack);
        all = true;
    }
    int r0 = all ? 0 : s->dirty_lo, r1 = all ? s->height - 1 : s->dirty_hi;
    int under = banner_due(vm);
    if (under) {
        if (r0 > r1 || r1 < under - 1)
            r1 = under - 1;
        r0 = 0;
    }
    /* Only the rows that differ from the frame last shown are converted,
     * and the band from the first to the last of them sent: a game that
     * redraws its whole screen changes few of its rows. SWEEP_ROWS rows go
     * each frame whether or not they changed, so anything drawn over the
     * game's screen is gone within 30 frames. */
    bool fits = s->width * s->height <= (int)sizeof(seen) &&
                !((uintptr_t)s->px & 3) && !(s->width & 15);
    bool compare = !all && seen_valid && fits;
    int sw0 = 0, sw1 = 0;
    if (compare) {
        sw0 = sweep;
        sw1 = sweep + SWEEP_ROWS;
        sweep = sw1 >= s->height ? 0 : sw1;
    }
    int stride = (int)(FBADDR(0, 1) - FBADDR(0, 0));
    int first = -1, last = -1;
    for (int y = 0; y < s->height; y++) {
        bool dirty = y >= r0 && y <= r1, swept = y >= sw0 && y < sw1;
        if (!dirty && !swept)
            continue;
        const uint8_t *now = s->px + y * s->width;
        uint32_t *was = seen + y * s->width / 4;
        if (compare && !swept) {
            if (!row_changed(was, (const uint32_t *)now, s->width / 4))
                continue;
        } else if (fits)
            memcpy(was, now, (size_t)s->width);
        if (first < 0) {
            present_wait();
            first = y;
        }
        present_rows(s, FBADDR(0, 0), stride, y, y);
        last = y;
    }
    /* the copy holds every row once a whole screen has gone */
    if (!fits)
        seen_valid = false;
    else if (!compare && r0 == 0 && r1 == s->height - 1)
        seen_valid = true;
    if (first >= 0) {
#ifdef HAVE_LCD_UPDATE_ASYNC
        lcd_update_start(first * 2, (last - first + 1) * 2);
#else
        lcd_update_rect(0, first * 2, LCD_WIDTH, (last - first + 1) * 2);
#endif
    }
    if (banner_ach >= 0)
        arcade_banner_undo(vm);
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

/* MENU held for a second opens the pause menu. Let go sooner, it is a
 * press: a cart that claims MENU gets it, for one tick, and any other asks
 * whether to quit. Either way the hold that opens the pause menu is never
 * also a press. */
static bool menu_down, menu_paused, menu_tapped;
static long menu_since;

/* True once MENU has been held for a second: the pause menu. */
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
    if (menu_down && !(status & BUTTON_MENU)) {
        if (!menu_paused && arcade_menu_is_game(vm))
            held |= ARCADE_MENU;
        else if (!menu_paused)
            menu_tapped = true;
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
    if (banner_ach >= 0)
        banner_until += current_tick - banner_away;
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
    banner_away = current_tick;
    cpu_boost(false);
    hold_backlight(false);
    wheel_events(true);
    viewportmanager_theme_undo(SCREEN_MAIN, true);
}

/* A dialog during a tick: the game's viewport to take the screen with,
 * whether one was shown (the frame clock then starts afresh), whether USB
 * came during it (the cart then ends once its tick is over), and whether
 * the screen was taken for it from the pause menu */
static struct viewport *game_vp;
static bool dialog_shown, dialog_to_root, dialog_open, dialog_took;

static void remote_key(struct vm *vm, long b);
static int event(struct vm *vm, long button);
static int hold_pause(struct vm *vm, bool idle);
static void pause_begin(struct vm *vm);
static long dialog_touched;       /* the last input in a dialog */

/* A frame of a dialog the engine drew on the game's screen (ui.*): shown,
 * a tick of the game's rate waited, and the buttons and wheel read --
 * MENU always, which backs out of it. Events are handled as in play. */
static int host_frame(void *ctx, unsigned *held, int *wheel)
{
    (void)ctx;
    struct vm *vm = sound_vm;
    dialog_shown = true;
    call_began = current_tick;
    if (!held) {
        dialog_open = false;
        shell_scores_run(true);
        if (dialog_took) {
            dialog_took = false;
            give_screen();
        } else
            present(vm, true);
        return 1;
    }
    if (!dialog_open) {
        dialog_open = true;
        dialog_touched = current_tick;
        shell_scores_run(false);
    }
    if (dialog_to_root)
        return 0;
    if (!screen_held) {
        take_screen(game_vp);
        dialog_took = true;
    }
    present(vm, true);
    int rate = arcade_rate(vm) > 0 ? arcade_rate(vm) : 30;
    long due = current_tick + HZ / rate;
    while (TIME_BEFORE(current_tick, due)) {
        sound_fill();
        long b = button_get_w_tmo(due - current_tick);
        if (b & SYS_EVENT) {
            if (event(vm, b) == USB) {
                dialog_to_root = true;
                return 0;
            }
        } else if (b & BUTTON_MULTIMEDIA)
            remote_key(vm, b);
    }
    /* held or left alone, a dialog rests as the game does */
    bool idle = TIME_AFTER(current_tick, dialog_touched + IDLE_PAUSE);
    if (button_hold() || idle) {
        /* a question asked in a pause holds the music already, and keeps
         * holding it once the hold switch is off */
        bool was_held = shell_tracks_held();
        pause_begin(vm);
        if (hold_pause(vm, idle && !button_hold()) == USB) {
            dialog_to_root = true;
            return 0;
        }
        if (!was_held)
            shell_tracks_hold(false);
        /* the press that woke it is not also an answer */
        while (button_status())
            button_get_w_tmo(HZ / 20);
        dialog_touched = current_tick;
        call_began = current_tick;
    }
    int status = button_status(), pos = wheel_position();
    unsigned h = 0;
    if (status & BUTTON_LEFT)   h |= ARCADE_PREV;
    if (status & BUTTON_RIGHT)  h |= ARCADE_NEXT;
    if (status & BUTTON_PLAY)   h |= ARCADE_PLAY;
    if (status & BUTTON_SELECT) h |= ARCADE_SELECT;
    if (status & BUTTON_MENU)   h |= ARCADE_MENU;
    *held = h;
    *wheel = wheel_clicks(pos);
    if (status || pos >= 0) {
        dialog_touched = current_tick;
        reset_poweroff_timer();
    }
    return 1;
}

/* A system event: saved first, since USB or a shutdown may follow; the
 * game redrawn after, over any splash the event showed. */
static int event(struct vm *vm, long button)
{
    sound_stop();
    present_wait();
    save_write(vm);
    shell_options_keep(vm);
    shell_scores_write(vm, false);
    /* the screen an event shows reads the wheel: the USB screen's wheel
     * is the volume while the player is a sound card */
    if (screen_held)
        wheel_events(true);
    int r = default_event_handler(button);
    if (screen_held)
        wheel_events(false);
    if (r == SYS_USB_CONNECTED)
        return USB;
    present(vm, true);
    return PLAYING;
}

/* The saves first, as event() writes them; the game to resume only where
 * none of the cart's code is running -- between ticks, in the pause menu or
 * the hold pause -- since inside a call the arena is mid-step */
void shell_power_off(void)
{
    struct vm *vm = sound_vm;
    if (!vm)
        return;
    save_write(vm);
    shell_options_keep(vm);
    shell_scores_write(vm, false);
    if (arcade_between_calls(vm))
        shell_suspend_write(vm);
}

/* The pause menu's rows: Resume, the cart's own (menuitem), its options,
 * High Scores when it has boards, Help when it has any, Now Playing,
 * Restart, Quit. An option row is a setting, the rest plain. */
enum { ROW_RESUME, ROW_ITEM, ROW_OPTION, ROW_SOUND, ROW_SCORES, ROW_HELP,
       ROW_MUSIC, ROW_RESTART, ROW_QUIT };
static char item_label[5][32];
static int item_index[5];
static int row_kind[1 + 5 + ARCADE_OPTIONS + 5], row_item[ARRAYLEN(row_kind)];
static int row_count;

static const char *menu_row(int row, void *data, char *buf, size_t len)
{
    (void)buf; (void)len;
    switch (row_kind[row]) {
    case ROW_RESUME:  return str(LANG_CHESSBOX_MENU_RESUME_GAME);
    case ROW_ITEM:    return item_label[row_item[row]];
    case ROW_OPTION:  return shell_option_title(data, row_item[row]);
    case ROW_SOUND:   return str(LANG_ARCADE_GAME_SOUND);
    case ROW_SCORES:  return str(LANG_HIGH_SCORES);
    case ROW_HELP:    return str(LANG_GAME_HELP);
    case ROW_MUSIC:   return str(LANG_NOW_PLAYING);
    case ROW_RESTART: return str(LANG_CHESSBOX_MENU_RESTART_GAME);
    default:          return str(LANG_MENU_QUIT);
    }
}

static enum list_row_kind menu_kind(int row, void *data)
{
    if (row_kind[row] == ROW_SOUND)
        return LIST_ROW_TOGGLE;
    return row_kind[row] == ROW_OPTION
        ? shell_option_kind(data, row_item[row]) : LIST_ROW_PLAIN;
}

static const char *menu_value(int row, void *data, char *buf, size_t len)
{
    /* Yes and No, as an on/off option says them: a skin draws its switch
     * for those words */
    if (row_kind[row] == ROW_SOUND)
        return str(arcade_muted(data) ? LANG_SET_BOOL_NO : LANG_SET_BOOL_YES);
    return row_kind[row] == ROW_OPTION
        ? shell_option_value(data, row_item[row], buf, len) : NULL;
}

static int menu_range(int row, void *data)
{
    return row_kind[row] == ROW_OPTION
        ? shell_option_range(data, row_item[row]) : -1;
}

static void add_row(int kind, int item)
{
    row_kind[row_count] = kind;
    row_item[row_count++] = item;
}

bool shell_list(struct simplelist_info *info)
{
    push_current_activity(ACTIVITY_CONTEXTMENU);
    bool to_root = simplelist_show_list(info);
    if (get_current_activity() == ACTIVITY_CONTEXTMENU)
        pop_current_activity();
    return to_root;
}

/* Now Playing: the music's controls, which a game holding PREV, NEXT and
 * PLAY leaves no other way to reach; in host_control()'s order */
static const int music_label[] = { LANG_PLAYPAUSE, LANG_NEXTTRACK,
                                   LANG_PREVTRACK };

static const char *music_row(int row, void *data, char *buf, size_t len)
{
    (void)data; (void)buf; (void)len;
    return str(music_label[row]);
}

/* Open until backed out of; true for out to the root (MENU or USB) */
static bool music_menu(void)
{
    struct simplelist_info info;
    int selection = 0;
    for (;;) {
        simplelist_info_init(&info, str(LANG_NOW_PLAYING),
                             ARRAYLEN(music_label), NULL);
        info.get_name = music_row;
        info.selection = selection;
        if (shell_list(&info))
            return true;
        if (info.selection < 0)
            return false;
        selection = info.selection;
        host_control(NULL, selection);
    }
}

/* MENU in the pause menu, or in a list it opened, leaves the game as Quit
 * does, back to the list the game was started from: for a game, that list
 * is the root. A USB connection, which closes a list the same way, goes on
 * to the root. */
static int menu_out(void)
{
    return usb_inserted() ? USB : QUIT;
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
    shell_options_keep(vm);
    shell_scores_run(false);
    shell_scores_write(vm, false);
    give_screen();
    for (;;) {
        int items = 0;
        row_count = 0;
        add_row(ROW_RESUME, 0);
        for (int i = 1; i <= 5; i++)
            if (arcade_menu_item(vm, i, item_label[items],
                                 sizeof(item_label[0]))) {
                item_index[items] = i;
                add_row(ROW_ITEM, items++);
            }
        for (int i = 0; i < arcade_options(vm)->count; i++)
            add_row(ROW_OPTION, i);
        add_row(ROW_SOUND, 0);
        if (arcade_scores(vm, NULL)->count || arcade_scores(vm, NULL)->nach)
            add_row(ROW_SCORES, 0);
        if (shell_has_help(vm))
            add_row(ROW_HELP, 0);
        add_row(ROW_MUSIC, 0);
        add_row(ROW_RESTART, 0);
        add_row(ROW_QUIT, 0);
        simplelist_info_init(&info, str(LANG_PLUGIN_GAMES), row_count, vm);
        info.get_name = menu_row;
        info.get_kind = menu_kind;
        info.get_value = menu_value;
        info.get_range = menu_range;
        info.selection = selection;
        if (shell_list(&info))
            return menu_out();
        selection = info.selection;
        int kind = selection < 0 ? ROW_RESUME : row_kind[selection];
        if (kind == ROW_OPTION) {
            if (shell_option_edit(vm, row_item[selection]))
                return menu_out();
            continue;
        }
        if (kind == ROW_SOUND) {
            shell_mute_toggle(vm);
            continue;
        }
        if (kind == ROW_HELP) {
            if (shell_help(vm))
                return menu_out();
            continue;
        }
        if (kind == ROW_SCORES) {
            if (shell_scores_menu(vm))
                return menu_out();
            continue;
        }
        if (kind == ROW_MUSIC) {
            if (music_menu())
                return menu_out();
            continue;
        }
        if (kind == ROW_RESTART)
            return RESTART;
        if (kind == ROW_QUIT)
            return QUIT;
        if (kind == ROW_RESUME)
            break;
        call_began = current_tick;
        int r = arcade_menu_choose(vm, item_index[row_item[selection]], 4);
        if (r < 0)
            return FAILED;
        if (dialog_to_root)
            return USB;             /* USB came during the item's dialog */
        if (r == 0)
            break;
    }
    take_screen(vp);
    present(vm, true);
    return PLAYING;
}

/* score.show(): the board's table over the game, which resumes when it is
 * closed; written out first, since a new score is usually why */
static int show_scores(struct vm *vm, struct viewport *vp, int board,
                       int rank)
{
    sound_stop();
    present_wait();
    shell_scores_run(false);
    shell_scores_write(vm, false);
    give_screen();
    bool to_root = shell_score_table(vm, board, rank);
    shell_scores_run(true);
    if (to_root)
        return USB;
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

/* A pause, by the menu or the hold switch: the cart told, and music it
 * owns held still until the game plays on */
static void pause_begin(struct vm *vm)
{
    arcade_paused(vm);
    shell_tracks_hold(true);
}

/* The hold switch: the game waits, the screen shows it, until released;
 * meanwhile the CPU and the backlight go back to their settings. Idle,
 * the same until a button or the wheel is touched. */
static int hold_pause(struct vm *vm, bool idle)
{
    int r = PLAYING;
    sound_stop();
    present_wait();
    save_write(vm);
    shell_options_keep(vm);
    shell_scores_run(false);
    shell_scores_write(vm, false);
    cpu_boost(false);
    hold_backlight(false);
    while (r == PLAYING && (idle || button_hold())) {
        long b = button_get_w_tmo(HZ / 10);
        if (b & SYS_EVENT)
            r = event(vm, b);
        else if (b & BUTTON_MULTIMEDIA)
            default_event_handler(b);
        else if (b != BUTTON_NONE || wheel_position() >= 0)
            idle = false;
    }
    cpu_boost(true);
    hold_backlight(true);
    if (r == PLAYING)
        present(vm, true);
    return r;
}

/* Everything queued since the last frame. The buttons are read as held
 * state, so their events are only drained; system events are acted on. */
static int drain(struct vm *vm)
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
    strmemccpy(cart_path, path, sizeof(cart_path));
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
    /* suspended last time: carry on, or start afresh; an arena a failed
     * read left unusable is loaded again */
    bool resumed;
    vm = shell_suspend_offer(vm, mem, (uint32_t)size, &host, &resumed);
    if (!vm)
        return RESTART;
    save_load(vm);
    save_owed = false;
    shell_store_start(vm);
    shell_scores_load(vm);
    shell_options_load(vm);
    arcade_set_ceiling(vm, CEILING);

    game_vp = &vp;
    dialog_shown = dialog_to_root = false;
    banner_ach = -1;
    take_screen(&vp);
    sound_vm = vm;
    /* the stall is timed from here: drawing the question below yields,
     * and a time left from the last game would stop this one */
    call_began = current_tick;
    /* suspended last time: asked over the game as it was left */
    if (resumed && !arcade_confirm(vm, str(LANG_GAME_RESUME_ASK),
                                   str(LANG_CHESSBOX_MENU_RESUME_GAME),
                                   str(LANG_CHESSBOX_MENU_RESTART_GAME),
                                   0)) {
        give_screen();
        sound_vm = NULL;
        shell_font_end();       /* the question may have opened it */
        return dialog_to_root ? USB : RESTART;
    }
    last_wheel = -1;
    menu_down = menu_paused = menu_tapped = false;
    beat_start();
    shell_tracks_start();
    sound_path[0] = '\0';         /* the track's record is read afresh */
    int status = PLAYING;
    bool suspend = false;           /* left by the pause menu's Quit */
    call_began = current_tick;
    if (!resumed && !arcade_start(vm))
        status = FAILED;
    present(vm, true);
    shell_scores_run(true);

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
            if (TIME_BEFORE(current_tick, due))
                continue;
        }
        status = drain(vm);
        if (status != PLAYING)
            break;

        if (menu_held_long(button_status()))
            menu_pressed = true;
        /* a finger on the wheel counts, though it sends no events */
        if (button_status() || wheel_position() >= 0) {
            touched = current_tick;
            reset_poweroff_timer();
        } else if (TIME_AFTER(current_tick, touched + IDLE_PAUSE))
            menu_pressed = true;
        if (menu_pressed) {
            pause_begin(vm);
            status = pause_menu(vm, &vp);
            if (status == PLAYING)
                shell_tracks_hold(false);
            suspend = status == QUIT;
            shell_scores_run(true);
            music_clock_resume();
            start = current_tick - (long)((frame * HZ) / rate);
            touched = call_began = current_tick;
            continue;
        }
        if (button_hold()) {
            pause_begin(vm);
            status = hold_pause(vm, false);
            if (status == PLAYING)
                shell_tracks_hold(false);
            shell_scores_run(true);
            music_clock_resume();
            start = current_tick - (long)((frame * HZ) / rate);
            touched = call_began = current_tick;
            continue;
        }

        unsigned held = buttons(vm, button_status());
        if (menu_tapped) {
            /* asked over the game, in its own dialog style; a pause, as
             * the pause menu is, so a round timed against the music is
             * lost to it */
            menu_tapped = false;
            pause_begin(vm);
            int yes = arcade_confirm(vm, str(LANG_ARCADE_QUIT_ASK),
                                     str(LANG_SET_BOOL_YES),
                                     str(LANG_SET_BOOL_NO), 1);
            if (dialog_to_root) {
                status = USB;
                break;
            }
            if (yes) {
                status = QUIT;
                suspend = true;
                break;
            }
            shell_tracks_hold(false);
            music_clock_resume();
            present(vm, true);
            start = current_tick - (long)((frame * HZ) / rate);
            touched = current_tick;
            continue;
        }
        int pos = wheel_position();
        frame++;

        /* Behind by a whole tick: one update without drawing, so the game
         * keeps its speed when a frame is slow; further behind than that,
         * the clock gives up the time rather than racing to catch it. */
        long late = current_tick - (start + (long)((frame * HZ) / rate));
        if (late >= HZ / rate) {
            /* rounded, and at least the frame not drawn: at 33 a tick is
             * 3 of HZ, and 3 * 33 / 100 truncates to none */
            long missed = (late * rate + HZ / 2) / HZ;
            arcade_skipped(vm, missed > 1 ? (unsigned)missed : 1);
            arcade_input(vm, held, wheel_clicks(pos), pos);
            beat_tick(vm);
            call_began = current_tick;
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
        call_began = current_tick;
        if (!arcade_tick(vm, 1)) {
            status = FAILED;
            break;
        }
        if (arcade_rate(vm) != rate) {
            rate = arcade_rate(vm);
            start = current_tick - (long)((frame * HZ) / rate);
        }
        if (dialog_shown) {
            dialog_shown = false;
            music_clock_resume();
            start = current_tick - (long)((frame * HZ) / rate);
            touched = current_tick;
            if (dialog_to_root) {
                status = USB;
                break;
            }
        }
        present(vm, false);
        int rank, board = arcade_score_shown(vm, &rank);
        if (board >= 0) {
            status = show_scores(vm, &vp, board, rank);
            music_clock_resume();
            start = current_tick - (long)((frame * HZ) / rate);
            touched = call_began = current_tick;
            continue;
        }
        sound_fill();
        arcade_idle(vm);
        /* Once a frame whatever the cart does: a cart always behind its
         * clock never waits above, and one light on script never yields
         * inside a tick, so without this nothing else would run. */
        yield();
        if (arcade_quit_asked(vm))
            status = QUIT;
    }

    /* the game's last word, before its saves and any kept copy are
     * written; not after it failed */
    if (status != FAILED && !arcade_end(vm))
        status = FAILED;
    if (status == FAILED)
        present(vm, false);
    beat_listen(NULL, 0);
    shell_tracks_end();
    talk_shutup();              /* nothing the cart said outlasts it */
    shell_font_end();
    give_screen();
    sound_vm = NULL;
    if (status == FAILED) {
        show_error(vm);
        status = QUIT;
    }
    save_write(vm);
    shell_options_keep(vm);
    shell_scores_run(false);
    shell_scores_write(vm, true);
    if (suspend)
        shell_suspend_write(vm);
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
