/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: the engine as a host sees it -- the player's shell, the web
 * shim, and the host test tool all drive a cart through these calls.
 *
 * The engine is platform-free C. What it needs from the platform comes
 * through struct arcade_host; it allocates nothing, and keeps everything
 * in the arena it is given.
 ***************************************************************************/
#ifndef ARCADE_H
#define ARCADE_H

#include <stdint.h>

/* The newest API version this engine provides. */
#define ARCADE_API_VERSION 1

struct arcade_host
{
    void *ctx;
    /* printh(): one line of text, not terminated */
    void (*print)(void *ctx, const char *s, int len);
    /* called every YIELD_UNITS units, so the codec keeps running */
    void (*yield)(void *ctx);
    /* year, month, day, hour, minute, second, weekday (0 = Sunday) */
    void (*date)(void *ctx, int32_t out[7]);
    /* click(): the wheel's tick, or with beep its beep; NULL for none */
    void (*click)(void *ctx, int beep);
    /* playback.playing(): whether the owner's music is playing (not
     * paused or stopped); NULL for never */
    int (*playing)(void *ctx);
    /* beat: the music's beat, below; NULL where there is none to follow */
    const struct arcade_beat *beat;
    /* sys.setting(): the owner's setting by its config.cfg name -- 1 with
     * *num for a number, 2 with its text in buf, 0 for no such setting;
     * NULL for none */
    int (*setting)(void *ctx, const char *name, int32_t *num, char *buf,
                   int cap);
    /* sys.translate(): the firmware's phrase whose English is english, in
     * the owner's language, UTF-8; NULL where the firmware has no such
     * phrase. NULL for English only */
    const char *(*translate)(void *ctx, const char *english);
};

/* The beat of the owner's music, for a cart played to it. Times are track
 * ms. The host also hands over each tick's clock (arcade_music()). */
struct arcade_beat
{
    /* beat.listen(): start or stop the analyser and the tempo search */
    void (*listen)(void *ctx, int on);
    /* locked (0/1), bpm, period ms, confidence 0-100, the envelope's
     * fill 0-100, beats since the lock */
    void (*state)(void *ctx, int32_t out[6]);
    /* the nth beat at or after from (0 the first) and its place in the
     * bar, 0-3, from the downbeat once one was found; -1 with no lock */
    int32_t (*next)(void *ctx, int32_t from, int32_t n, int32_t *in_bar);
    /* which of the four beats starts the bar, or -1 (none wins, or under
     * 32 beats heard), and by what margin in tenths */
    int32_t (*downbeat)(void *ctx, int32_t *margin10);
    /* track ms this instant, past the tick */
    int32_t (*now)(void *ctx);
    /* the bar just heard: energy 0-100, flux 0-100, trend (signed) */
    void (*mood)(void *ctx, int32_t out[3]);
    /* playback.bands(): n spectrum bands (1-8), 0-100, as the skins draw
     * them */
    void (*bands)(void *ctx, int32_t *out, int32_t n);
    /* playback.sound(): the playing track's Sound Index record, in the
     * order of SOUND.* (arcade_api.def); 0 where it has none */
    int (*sound)(void *ctx, int32_t out[31]);
    /* beat.ahead(): the hits the analyser has found from from to before
     * to, oldest first, as time, bands (1 low, 2 mid, 4 high) and
     * strength, at most max of them; the count. Only while listening */
    int32_t (*ahead)(void *ctx, int32_t from, int32_t to, int32_t *out,
                     int32_t max);
    /* beat.horizon(): track ms of the newest audio analysed, or -1 when
     * not listening */
    int32_t (*horizon)(void *ctx);
};

/* What the host read for a tick: the clock, steady for the tick; the ms
 * still to play; whether playback moved underneath (a seek, a skip, a new
 * track) since the last; and when each button went down, in btn()'s
 * numbering, or -1. */
struct arcade_music
{
    int32_t clock, left, jumped;
    int32_t press[5];
};

/* Reads n bytes into buf, or skips n bytes when buf is NULL; returns the
 * number read or skipped. */
typedef uint32_t (*arcade_read_fn)(void *ctx, void *buf, uint32_t n);

struct vm;

/* Loads and verifies a cart into mem, which must be 8-byte aligned. Returns
 * NULL only when mem is too small to report anything; otherwise check
 * arcade_error(). */
struct vm *arcade_load(void *mem, uint32_t size, arcade_read_fn read,
                       void *rctx, const struct arcade_host *host,
                       uint64_t seed);

/* The cart's settings the host needs: ticks a second, whether MENU is the
 * cart's button 2 rather than the pause menu, and the 8-byte id its save,
 * options and scores are filed under. */
int arcade_rate(const struct vm *vm);
int arcade_menu_is_game(const struct vm *vm);

/* The host fell behind by this many frames and did not draw them: what
 * skipped() reports. The rate can change at any tick (setrate()), so the
 * host reads arcade_rate() after each. */
void arcade_skipped(struct vm *vm, unsigned frames);
const uint8_t *arcade_id(const struct vm *vm);

/* The buttons for btn(): one bit each, in btn()'s numbering. */
#define ARCADE_PREV   0x01
#define ARCADE_NEXT   0x02
#define ARCADE_MENU   0x04  /* only when arcade_menu_is_game() */
#define ARCADE_PLAY   0x08
#define ARCADE_SELECT 0x10

/* Before each tick: the buttons held, the clicks the wheel turned since
 * the last tick (96 a turn, clockwise positive), and where the finger is
 * on it, 0-95 clockwise from the top, or -1 when nothing touches it. */
void arcade_input(struct vm *vm, unsigned held, int wheel, int wheelpos);

/* Before each tick, where the host follows the music: beat.clock() and
 * the rest of the tick's reading. */
void arcade_music(struct vm *vm, const struct arcade_music *m);

/* The cart's own pause-menu rows, made by menuitem(): copies item i's
 * label (1-5) into buf and returns 1, or returns 0 when there is none. */
int arcade_menu_item(const struct vm *vm, int i, char *buf, int cap);

/* Item i chosen: calls its function with the button that chose it, in
 * btn()'s numbering. 1 when it returned a true value -- keep the menu open
 * and redraw it -- 0 to resume, -1 when the cart stopped with an error. */
int arcade_menu_choose(struct vm *vm, int i, int button);

/* Whether the cart has called quit(). */
int arcade_quit_asked(const struct vm *vm);

/* Runs the cart's global setup, then init(). */
int arcade_start(struct vm *vm);
/* One tick: update(), then draw() when draw is set and the cart has one. */
int arcade_tick(struct vm *vm, int draw);
/* Between ticks: collect garbage if enough has been allocated. */
void arcade_idle(struct vm *vm);

/* Units charged by the last arcade_start or arcade_tick. */
uint32_t arcade_units(const struct vm *vm);
/* Units at which one call is stopped as not responding. */
void arcade_set_ceiling(struct vm *vm, uint32_t units);

/* NULL while the cart is running; otherwise what stopped it, and where:
 * the source file, line and function, when known. */
const char *arcade_error(const struct vm *vm, const char **file,
                         int *line, const char **fn);

/* The screen, 8-bit palette indices, for present. Rows dirty_lo..dirty_hi
 * have been drawn since the last arcade_presented() (none when lo > hi);
 * rgb gives each index's colour as 0xRRGGBB, the screen palette already
 * applied, and palette_changed says it has changed, so every row needs
 * converting again. */
struct arcade_screen
{
    const uint8_t *px;
    int width, height;
    int dirty_lo, dirty_hi;
    const uint32_t *rgb;
    int palette_changed;
};

const struct arcade_screen *arcade_screen(struct vm *vm);
void arcade_presented(struct vm *vm);

/* core_sound.c: the cart's next frames of sound at rate Hz, each a stereo pair
 * of 16-bit samples, the two the same. Returns 0, the frames silent, when
 * no voice is sounding. */
int arcade_audio(struct vm *vm, int16_t *out, int frames, int rate);

/* present.c: the conversion for a 16-bit panel at twice the size. The
 * table holds each colour's pixel twice in one word, packed by the host's
 * own pack(); rows r0..r1 of the screen become panel rows 2*r0..2*r1+1 of
 * fb, stride pixels a row. */
void arcade_present_lut(const struct arcade_screen *s, uint32_t lut2[256],
                        uint16_t (*pack)(uint32_t rgb));
void arcade_present_rows(const struct arcade_screen *s,
                         const uint32_t lut2[256], void *fb, int stride,
                         int r0, int r1);

/* The 256 saved values, for the host to write out: their 32 bits, a bit
 * each saying which are fractions, and whether any changed since the last
 * call. */
const int32_t *arcade_save(struct vm *vm, int *dirty,
                           const uint8_t **fractions);
void arcade_restore(struct vm *vm, const int32_t save[256],
                    const uint8_t fractions[32]);

#endif
