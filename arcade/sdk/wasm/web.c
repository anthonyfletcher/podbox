/* web.c: the engine as a WebAssembly module, for pbgrun and the IDE.
 *
 * The host's calls go out as imports from "env", which the JavaScript
 * side supplies; the module exports what drives a cart a tick at a time.
 * A dialog's frame (env.frame) is the one call that may wait: the IDE
 * pauses the module there with Asyncify and resumes it on the next tick,
 * while pbgrun answers it at once, as pbgvm does.
 *
 * The owner's music, its library and its beat are host/fakemusic.c's, as
 * pbgvm's are: made up, playing as the page says.
 *
 * Parts: the imports; the host built from them; driving a cart; the
 * profiler's samples; the music; what the page reads back (the screen, the
 * sound, the error, an open dialog); the pause menu's contents -- the
 * cart's items, its options, its scores and achievements -- and its saved
 * numbers, which the page keeps between runs. */

#include <stdint.h>
#include <string.h>
#include "games/arcade/arcade.h"
#include "fakemusic.h"

#define IMPORT(name) __attribute__((import_module("env"), import_name(name)))
#define EXPORT(name) __attribute__((export_name(name)))

/* ---- the imports --------------------------------------------------------- */

IMPORT("print") void js_print(const char *s, int len);
IMPORT("date") void js_date(int32_t *out);
IMPORT("click") void js_click(int beep);
IMPORT("setting") int js_setting(const char *name, int32_t *num, char *buf,
                                 int cap);
IMPORT("frame") int js_frame(unsigned *held, int *wheel);
IMPORT("say") void js_say(const char *text, int queue);
IMPORT("speech_busy") int js_speech_busy(void);
IMPORT("speech_stop") void js_speech_stop(void);
IMPORT("speech_on") int js_speech_on(void);
IMPORT("store_read") int32_t js_store_read(const char *name, void *buf,
                                           int32_t cap);
IMPORT("store_write") int js_store_write(const char *name, const void *buf,
                                         int32_t n);
IMPORT("store_remove") int js_store_remove(const char *name);
IMPORT("store_list") int js_store_list(char (*names)[33], int max);
IMPORT("glyph") int js_glyph(uint32_t cp, uint8_t *out);
IMPORT("music_say") void js_music_say(const char *s, int len);
IMPORT("watch") void js_watch(const char *name, const char *value);
IMPORT("prof") void js_prof(const char *name);

/* ---- the host ------------------------------------------------------------ */

static void print(void *ctx, const char *s, int len)
{
    (void)ctx;
    js_print(s, len);
}

static void date(void *ctx, int32_t out[7]) { (void)ctx; js_date(out); }
static void click(void *ctx, int beep) { (void)ctx; js_click(beep); }

static int setting(void *ctx, const char *name, int32_t *num, char *buf,
                   int cap)
{
    (void)ctx;
    return js_setting(name, num, buf, cap);
}

static int frame(void *ctx, unsigned *held, int *wheel)
{
    (void)ctx;
    return js_frame(held, wheel);
}

static void say(void *ctx, const char *t, int q) { (void)ctx; js_say(t, q); }
static int busy(void *ctx) { (void)ctx; return js_speech_busy(); }
static void stop(void *ctx) { (void)ctx; js_speech_stop(); }
static int on(void *ctx) { (void)ctx; return js_speech_on(); }

static int32_t st_read(void *ctx, const char *name, void *buf, int32_t cap)
{
    (void)ctx;
    return js_store_read(name, buf, cap);
}

static int st_write(void *ctx, const char *name, const void *buf, int32_t n)
{
    (void)ctx;
    return js_store_write(name, buf, n);
}

static int st_remove(void *ctx, const char *name)
{
    (void)ctx;
    return js_store_remove(name);
}

static int st_list(void *ctx, char (*names)[33], int max)
{
    (void)ctx;
    return js_store_list(names, max);
}

static int glyph(void *ctx, uint32_t cp, uint8_t out[13])
{
    (void)ctx;
    return js_glyph(cp, out);
}

static void watch(void *ctx, const char *name, const char *value)
{
    (void)ctx;
    js_watch(name, value);
}

static void prof_section(void *ctx, const char *name)
{
    (void)ctx;
    js_prof(name);
}

static const struct arcade_speak speak = { say, busy, stop, on };
static const struct arcade_store store = { st_read, st_write, st_remove,
                                           st_list };
/* playing and beat are set by web_music, before the cart loads */
static uint32_t cart_at_read(void *ctx, uint32_t at, void *buf, uint32_t n);

static struct arcade_host host = {
    NULL, print, NULL, date, click, NULL, NULL, setting, NULL, fake_control,
    &fake_tracks, frame, &speak, &store, glyph, watch, prof_section,
    cart_at_read
};

/* ---- driving a cart ------------------------------------------------------ */

/* The player's limit, the 5G's whole app buffer; and room for a cart */
static uint64_t arena[512 * 1024 / 8];
static uint8_t cart[4 << 20];
static uint32_t cart_len, cart_at;
static struct vm *vm;

static void music_start(void);

static uint32_t cart_read(void *ctx, void *buf, uint32_t n)
{
    (void)ctx;
    if (n > cart_len - cart_at)
        n = cart_len - cart_at;
    if (buf)
        memcpy(buf, cart + cart_at, n);
    cart_at += n;
    return n;
}

/* the cart's maps and images as it plays, from the cart the page put */
static uint32_t cart_at_read(void *ctx, uint32_t at, void *buf, uint32_t n)
{
    (void)ctx;
    if (at > cart_len)
        return 0;
    if (n > cart_len - at)
        n = cart_len - at;
    memcpy(buf, cart + at, n);
    return n;
}

/* where the page puts the cart, before web_load */
EXPORT("web_cart") uint8_t *web_cart(void) { return cart; }
EXPORT("web_cart_size") uint32_t web_cart_size(void) { return sizeof(cart); }

/* 1 loaded, 0 with the error to read */
EXPORT("web_load") int web_load(uint32_t len, uint32_t seed_lo,
                                uint32_t seed_hi)
{
    cart_len = len < sizeof(cart) ? len : sizeof(cart);
    cart_at = 0;
    music_start();
    vm = arcade_load(arena, sizeof(arena), cart_read, NULL, &host,
                     (uint64_t)seed_hi << 32 | seed_lo);
    return vm && !arcade_error(vm, NULL, NULL, NULL);
}

EXPORT("web_start") int web_start(void) { return arcade_start(vm); }
EXPORT("web_end") int web_end(void) { return arcade_end(vm); }
EXPORT("web_rate") int web_rate(void) { return arcade_rate(vm); }
EXPORT("web_menu_is_game") int web_menu_is_game(void)
{
    return arcade_menu_is_game(vm);
}
EXPORT("web_units") uint32_t web_units(void) { return arcade_units(vm); }
/* inside a call, from the page's prof(): the units so far in it */
EXPORT("web_units_now") uint32_t web_units_now(void) { return arcade_units_now(vm); }

/* ---- the profiler's samples ---------------------------------------------- */

/* While the page profiles, each yield -- every 2048 units -- is a sample of
 * where the cart is: the native call it is in (or -1), the offset in the
 * innermost function, how deep the frames go, the outermost function, and
 * the innermost PROF_FNS functions, innermost first. */
#define PROF_FNS 8
#define PROF_WORDS (4 + PROF_FNS)
#define PROF_MAX 1024
static int32_t prof[PROF_MAX * PROF_WORDS];
static int prof_n;

static void sample(void *ctx)
{
    (void)ctx;
    if (prof_n >= PROF_MAX)
        return;
    int32_t *s = prof + prof_n * PROF_WORDS;
    uint32_t fns[PROF_FNS], pc, root, depth;
    int32_t native;
    depth = (uint32_t)arcade_where(vm, fns, PROF_FNS, &pc, &native);
    s[0] = native;
    s[1] = (int32_t)pc;
    s[2] = (int32_t)depth;
    root = 0;
    if (depth > PROF_FNS) {
        uint32_t all[256];        /* more than a cart may nest */
        uint32_t k = (uint32_t)arcade_where(vm, all, 256, &pc, &native);
        root = all[(k < 256 ? k : 256) - 1];
    } else if (depth)
        root = fns[depth - 1];
    s[3] = depth ? (int32_t)root : -1;
    for (uint32_t i = 0; i < PROF_FNS; i++)
        s[4 + i] = i < depth ? (int32_t)fns[i] : -1;
    prof_n++;
}

/* sampling on or off; the samples so far are dropped */
EXPORT("web_profile") void web_profile(int on)
{
    host.yield = on ? sample : NULL;
    prof_n = 0;
}

/* the samples since the last call, PROF_WORDS words each, at web_prof_data */
EXPORT("web_prof_take") int web_prof_take(void)
{
    int n = prof_n;
    prof_n = 0;
    return n;
}
EXPORT("web_prof_data") int32_t *web_prof_data(void) { return prof; }
/* What the arena holds now, for the IDE's memory view: the totals and
 * kinds as struct arcade_memory lays them out, and web_memory_globals()
 * the bytes each global reaches. Between ticks only. */
static struct arcade_memory mem_view;
static uint32_t mem_globals[65536];
EXPORT("web_memory") struct arcade_memory *web_memory(void)
{
    memset(mem_globals, 0, sizeof(mem_globals));
    arcade_memory(vm, &mem_view, mem_globals, 65536);
    return &mem_view;
}
EXPORT("web_memory_globals") uint32_t *web_memory_globals(void)
{
    return mem_globals;
}

EXPORT("web_ceiling") void web_ceiling(uint32_t u)
{
    arcade_set_ceiling(vm, u);
}
EXPORT("web_skipped") void web_skipped(unsigned n) { arcade_skipped(vm, n); }
EXPORT("web_paused") void web_paused(void) { arcade_paused(vm); }

/* One tick, as the player runs it: the input, the music ms on and its
 * state, each new press stamped with its clock, the last frame presented,
 * then the tick. Without draw it is the update the player runs alone when
 * it has fallen behind, the screen not presented. 1, or 0 on an error. */
static unsigned last_held;

EXPORT("web_tick") int web_tick(unsigned held, int clicks, int pos, int draw,
                                int32_t ms)
{
    struct arcade_music m = { 0, 0, 0, 0, { -1, -1, -1, -1, -1 } };
    fake_music_advance(ms);
    fake_music_tick(&m);
    for (int i = 0; i < 5; i++)
        if ((held & ~last_held) & (1u << i))
            m.press[i] = m.clock;
    last_held = held;
    arcade_input(vm, held, clicks, pos);
    arcade_music(vm, &m);
    if (draw)
        arcade_presented(vm);
    return arcade_tick(vm, draw);
}

EXPORT("web_idle") void web_idle(void) { arcade_idle(vm); }

/* ---- the music ---------------------------------------------------------- */

static int music_playing, music_bpm;

static void music_say(const char *line)
{
    js_music_say(line, (int)strlen(line));
}

/* Before a cart loads: whether playback.playing() and beat have music to
 * report (pbgvm's --playing and --beat), the music playing at first, and
 * a tempo for every track (0 for each its own) */
EXPORT("web_music") void web_music(int playing_fn, int beat, int playing,
                                   int bpm)
{
    host.playing = playing_fn ? fake_playing : NULL;
    host.beat = beat ? &fake_beat : NULL;
    music_playing = playing;
    music_bpm = bpm;
}

static void music_start(void)
{
    fake_music_init(music_say);
    fake_music_tempo(music_bpm);
    fake_music_play(music_playing);
    last_held = 0;
}

/* the page's own play, pause and skips (0 toggles, 1 next, 2 back) */
EXPORT("web_music_control") void web_music_control(int what)
{
    fake_music_control(what);
}

/* what is playing: the track's id, ms into it, length, tempo, and whether
 * it plays; its title, artist and album through web_music_text */
static int32_t now[5];

EXPORT("web_music_now") int32_t *web_music_now(void)
{
    now[0] = fake_music_now(&now[1], &now[2], &now[3]);
    now[4] = fake_playing(NULL);
    return now;
}

/* ---- what the page reads back -------------------------------------------- */

static uint8_t rgba[160 * 120 * 4];

/* The screen as RGBA, 160x120, and the rows the last tick drew */
EXPORT("web_screen") uint8_t *web_screen(void)
{
    const struct arcade_screen *s = arcade_screen(vm);
    for (int i = 0; i < s->width * s->height; i++) {
        uint32_t c = s->rgb[s->px[i]];
        rgba[i * 4] = (uint8_t)(c >> 16);
        rgba[i * 4 + 1] = (uint8_t)(c >> 8);
        rgba[i * 4 + 2] = (uint8_t)c;
        rgba[i * 4 + 3] = 255;
    }
    return rgba;
}

/* The screen's palette numbers, 160x120 */
EXPORT("web_pixels") const uint8_t *web_pixels(void)
{
    return arcade_screen(vm)->px;
}

/* the rows the player would send: those drawn, or every row when the
 * palette changed, since each pixel's colour then changes on the panel */
EXPORT("web_dirty") int web_dirty(void)
{
    const struct arcade_screen *s = arcade_screen(vm);
    if (s->palette_changed)
        return 0 << 16 | (s->height - 1);
    return s->dirty_lo << 16 | (s->dirty_hi & 0xffff);
}

/* 1 when the palette changed: the player sends every row without first
 * comparing it with the last frame's */
EXPORT("web_palette_changed") int web_palette_changed(void)
{
    return arcade_screen(vm)->palette_changed != 0;
}

static int16_t sound[2 * 4096];

/* frames of sound at rate Hz, stereo pairs, into the buffer returned */
EXPORT("web_audio") int16_t *web_audio(int frames, int rate)
{
    if (frames > 4096)
        frames = 4096;
    if (!arcade_audio(vm, sound, frames, rate))
        memset(sound, 0, sizeof(int16_t) * 2 * (size_t)frames);
    return sound;
}

static char text[2048];
static int text_len;

static void put(const char *s)
{
    while (*s && text_len < (int)sizeof(text) - 1)
        text[text_len++] = *s++;
    text[text_len] = 0;
}

static void put_num(long v)
{
    char b[24];
    int n = 0;
    unsigned long u = v < 0 ? 0ul - (unsigned long)v : (unsigned long)v;
    do
        b[n++] = (char)('0' + u % 10);
    while ((u /= 10));
    if (v < 0)
        put("-");
    while (n) {
        char c[2] = { b[--n], 0 };
        put(c);
    }
}

/* The error as pbgvm reports it: file:line: message (in function) */
EXPORT("web_error") const char *web_error(void)
{
    const char *file, *fn;
    int line;
    const char *msg = vm ? arcade_error(vm, &file, &line, &fn) : "no memory";
    text_len = 0;
    text[0] = 0;
    if (!msg)
        return text;
    if (vm && file) {
        put(file);
        put(":");
        put_num(line);
        put(": ");
        put(msg);
        put(" (in ");
        put(fn);
        put(")");
    } else
        put(msg);
    return text;
}

/* The playing track's title, artist or album (ARCADE_TRACK_*), or "" */
EXPORT("web_music_text") const char *web_music_text(int what)
{
    text_len = 0;
    text[0] = 0;
    if (!fake_tracks.text(NULL, what, text, sizeof(text)))
        text[0] = 0;
    return text;
}

/* The open dialog, as pbgvm prints it, or "" for none */
EXPORT("web_dialog") const char *web_dialog(void)
{
    static const char *const kinds[] = { "choose", "confirm", "message",
                                         "number", "text" };
    struct arcade_dialog d;
    text_len = 0;
    text[0] = 0;
    if (!arcade_dialog(vm, &d))
        return text;
    put(kinds[d.kind]);
    put(" ");
    for (const char *p = d.prompt; *p; p++) {
        char c[2] = { *p, 0 };
        put(*p == '\n' ? " / " : c);
    }
    if (d.kind == ARCADE_DIALOG_CHOOSE) {
        put(":");
        for (int i = 0; i < d.n; i++) {
            put(i ? "|" : " ");
            put(d.items[i]);
        }
        put(" at ");
        put_num(d.at);
    } else if (d.kind == ARCADE_DIALOG_NUMBER) {
        put(": ");
        put_num(d.min);
        put(" to ");
        put_num(d.max);
        put(" by ");
        put_num(d.step);
        put(" from ");
        put_num(d.value);
    } else if (d.kind == ARCADE_DIALOG_TEXT) {
        put(": [");
        put(d.text);
        put("] up to ");
        put_num(d.most);
    }
    return text;
}

/* ---- the pause menu's contents, and what the page keeps ------------------ */

/* Records pass through here: the page writes one in before a restore, or
 * reads one out after a pack, its length returned. */
static uint8_t record[8192];

EXPORT("web_record") uint8_t *web_record(void) { return record; }

/* menuitem(i)'s label, 1 to 5, or NULL for none; choosing one runs its
 * function, as SELECT on it would: 0 resumes, 1 keeps the menu, -1 fault */
EXPORT("web_menu_item") const char *web_menu_item(int i)
{
    return arcade_menu_item(vm, i, text, sizeof(text)) ? text : NULL;
}
EXPORT("web_menu_choose") int web_menu_choose(int i)
{
    return arcade_menu_choose(vm, i, 4);
}

/* option i's value now, and the owner setting it */
EXPORT("web_option") int32_t web_option(int i)
{
    const struct arcade_options *o = arcade_options(vm);
    return i >= 0 && i < o->count ? o->o[i].value : 0;
}
EXPORT("web_option_set") void web_option_set(int i, int32_t v)
{
    arcade_option_set(vm, i, v);
}

EXPORT("web_options_pack") int web_options_pack(void)
{
    return arcade_options_pack(vm, record, sizeof(record));
}
EXPORT("web_options_unpack") int web_options_unpack(int n)
{
    return arcade_options_unpack(vm, record, n);
}

/* the scores record, and whether a score was recorded since the last
 * pack; and one taken back for the cart's boards */
static int scores_dirty;

EXPORT("web_scores_pack") int web_scores_pack(void)
{
    const struct arcade_scores *sc = arcade_scores(vm, &scores_dirty);
    return arcade_scores_pack(sc, record, sizeof(record));
}
EXPORT("web_scores_dirty") int web_scores_dirty(void) { return scores_dirty; }
EXPORT("web_scores_restore") int web_scores_restore(int n)
{
    static struct arcade_scores saved;
    if (!arcade_scores_unpack(&saved, record, n))
        return 0;
    arcade_scores_restore(vm, &saved);
    return 1;
}

/* board i as text: its title, then a line a rank -- value, name, date --
 * or NULL past the boards */

EXPORT("web_board") const char *web_board(int i)
{
    const struct arcade_scores *sc = arcade_scores(vm, NULL);
    if (i < 0 || i >= sc->count)
        return NULL;
    const struct arcade_board *b = &sc->b[i];
    text_len = 0;
    put(b->title);
    for (int k = 0; k < b->count; k++) {
        char v[32];
        arcade_score_text(sc, i, b->e[k].value, v, sizeof(v));
        put("\n");
        put(v);
        put("\t");
        put(b->e[k].name);
        put("\t");
        put_num(b->e[k].date);
    }
    text[text_len] = 0;
    return text;
}

/* achievement i: its title, what earns it, and the date earned (0 not
 * yet), a line each; NULL past them */
EXPORT("web_achievement") const char *web_achievement(int i)
{
    const struct arcade_scores *sc = arcade_scores(vm, NULL);
    if (i < 0 || i >= sc->nach)
        return NULL;
    text_len = 0;
    put(sc->ach[i].title);
    put("\n");
    put(sc->ach[i].text);
    put("\n");
    put_num(sc->ach[i].date);
    text[text_len] = 0;
    return text;
}

/* score.show() since the last call: board << 8 | rank, or -1; ach.earn()'s
 * newly earned, or -1; quit() asked */
EXPORT("web_score_shown") int web_score_shown(void)
{
    int rank, b = arcade_score_shown(vm, &rank);
    return b < 0 ? -1 : b << 8 | rank;
}
EXPORT("web_achieved") int web_achieved(void) { return arcade_achieved(vm); }
EXPORT("web_quit_asked") int web_quit_asked(void) { return arcade_quit_asked(vm); }

/* the 256 saved numbers, then a bit each saying which are fractions;
 * returns whether any changed since the last call */
EXPORT("web_saves_pack") int web_saves_pack(void)
{
    int dirty;
    const uint8_t *frac;
    const int32_t *v = arcade_save(vm, &dirty, &frac);
    memcpy(record, v, 256 * sizeof(int32_t));
    memcpy(record + 256 * sizeof(int32_t), frac, 32);
    return dirty;
}
EXPORT("web_saves_restore") void web_saves_restore(void)
{
    int32_t v[256];
    memcpy(v, record, sizeof(v));
    arcade_restore(vm, v, record + sizeof(v));
}

/* Asyncify's record of the paused stack: its start and end, then room */
static int32_t async_data[2 + 16384];

EXPORT("web_async_data") int32_t *web_async_data(void)
{
    async_data[0] = (int32_t)(uintptr_t)(async_data + 2);
    async_data[1] = (int32_t)(uintptr_t)(async_data + 2 + 16384);
    return async_data;
}
