/* web.c: the engine as a WebAssembly module, for pbgrun and the IDE.
 *
 * The host's calls go out as imports from "env", which the JavaScript
 * side supplies; the module exports what drives a cart a tick at a time.
 * A dialog's frame (env.frame) is the one call that may wait: the IDE
 * pauses the module there with Asyncify and resumes it on the next tick,
 * while pbgrun answers it at once, as pbgvm does.
 *
 * Parts: the imports; the host built from them; driving a cart; what the
 * page reads back (the screen, the sound, the error, an open dialog). */

#include <stdint.h>
#include <string.h>
#include "games/arcade/arcade.h"

#define IMPORT(name) __attribute__((import_module("env"), import_name(name)))
#define EXPORT(name) __attribute__((export_name(name)))

/* ---- the imports --------------------------------------------------------- */

IMPORT("print") void js_print(const char *s, int len);
IMPORT("date") void js_date(int32_t *out);
IMPORT("click") void js_click(int beep);
IMPORT("setting") int js_setting(const char *name, int32_t *num, char *buf,
                                 int cap);
IMPORT("control") void js_control(int what);
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

/* ---- the host ------------------------------------------------------------ */

static void print(void *ctx, const char *s, int len)
{
    (void)ctx;
    js_print(s, len);
}

static void date(void *ctx, int32_t out[7]) { (void)ctx; js_date(out); }
static void click(void *ctx, int beep) { (void)ctx; js_click(beep); }
static void control(void *ctx, int what) { (void)ctx; js_control(what); }

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

static const struct arcade_speak speak = { say, busy, stop, on };
static const struct arcade_store store = { st_read, st_write, st_remove,
                                           st_list };
static const struct arcade_host host = {
    NULL, print, NULL, date, click, NULL, NULL, setting, NULL, control,
    NULL, frame, &speak, &store, glyph
};

/* ---- driving a cart ------------------------------------------------------ */

/* The player's limit, the 5G's whole app buffer; and room for a cart */
static uint64_t arena[512 * 1024 / 8];
static uint8_t cart[4 << 20];
static uint32_t cart_len, cart_at;
static struct vm *vm;

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

/* where the page puts the cart, before web_load */
EXPORT("web_cart") uint8_t *web_cart(void) { return cart; }
EXPORT("web_cart_size") uint32_t web_cart_size(void) { return sizeof(cart); }

/* 1 loaded, 0 with the error to read */
EXPORT("web_load") int web_load(uint32_t len, uint32_t seed_lo,
                                uint32_t seed_hi)
{
    cart_len = len < sizeof(cart) ? len : sizeof(cart);
    cart_at = 0;
    vm = arcade_load(arena, sizeof(arena), cart_read, NULL, &host,
                     (uint64_t)seed_hi << 32 | seed_lo);
    return vm && !arcade_error(vm, NULL, NULL, NULL);
}

EXPORT("web_start") int web_start(void) { return arcade_start(vm); }
EXPORT("web_rate") int web_rate(void) { return arcade_rate(vm); }
EXPORT("web_units") uint32_t web_units(void) { return arcade_units(vm); }
EXPORT("web_ceiling") void web_ceiling(uint32_t u)
{
    arcade_set_ceiling(vm, u);
}
EXPORT("web_skipped") void web_skipped(unsigned n) { arcade_skipped(vm, n); }

/* One tick, as the player runs it: the input, the music's state (none to
 * follow), the last frame presented, then the tick. 1, or 0 on an error. */
EXPORT("web_tick") int web_tick(unsigned held, int clicks, int pos)
{
    struct arcade_music m = { 0, 0, 0, 0, { -1, -1, -1, -1, -1 } };
    arcade_input(vm, held, clicks, pos);
    arcade_music(vm, &m);
    arcade_presented(vm);
    return arcade_tick(vm, 1);
}

EXPORT("web_idle") void web_idle(void) { arcade_idle(vm); }

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

EXPORT("web_dirty") int web_dirty(void)
{
    const struct arcade_screen *s = arcade_screen(vm);
    return s->dirty_lo << 16 | (s->dirty_hi & 0xffff);
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

/* Asyncify's record of the paused stack: its start and end, then room */
static int32_t async_data[2 + 16384];

EXPORT("web_async_data") int32_t *web_async_data(void)
{
    async_data[0] = (int32_t)(uintptr_t)(async_data + 2);
    async_data[1] = (int32_t)(uintptr_t)(async_data + 2 + 16384);
    return async_data;
}
