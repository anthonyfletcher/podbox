/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade core: input -- the buttons and the click wheel, as the host hands
 * them over before each tick (arcade_input) -- quit(), and the music's
 * clock and beat (beat, arcade_music).
 *
 * btn and btnp use PICO-8's numbers: 0 PREV, 1 NEXT, 2 MENU (only for a
 * cart that claims it; the host never sets it otherwise), 3 PLAY,
 * 4 SELECT; any other number is an error, as the wheel has only five.
 * btnp is true on the tick a button goes down, then
 * on the 16th tick it is held and every 4th after.
 ***************************************************************************/
#include <string.h>
#include "games/arcade/vm.h"

#define NATIVE(name) int n_##name(struct vm *vm, struct value *a, int argc, \
                                  struct value *out)
#define RET_INT(v) (*out = mk(T_INT, (int32_t)(v)), 1)

void arcade_input(struct vm *vm, unsigned held, int wheel, int wheelpos)
{
    uint8_t pressed = 0;
    held &= 0x1f;
    for (int i = 0; i < 5; i++) {
        if (!(held & (1u << i))) {
            vm->held_ticks[i] = 0;
            continue;
        }
        uint16_t t = vm->held_ticks[i];
        if (t < 0xffff)
            t++;
        vm->held_ticks[i] = t;
        if (t == 1 || (t >= 16 && (t - 16) % 4 == 0))
            pressed |= (uint8_t)(1u << i);
    }
    vm->held = (uint8_t)held;
    vm->pressed = pressed;
    vm->wheel = wheel;
    vm->wheelpos = wheelpos < 0 || wheelpos > 95 ? -1 : wheelpos;
    /* no presses stamped unless the host follows the music this tick */
    vm->heard.jumped = 0;
    for (int i = 0; i < 5; i++)
        vm->heard.press[i] = -1;
}

void arcade_music(struct vm *vm, const struct arcade_music *m)
{
    vm->heard = *m;
}

static int button_arg(struct vm *vm, struct value *a, int argc, uint8_t bits,
                      const char *fn, struct value *out)
{
    if (argc == 0 || a[0].tag == T_NULL) {
        *out = mk(T_INT, bits);
        return 1;
    }
    if (!is_num(a[0]))
        return vm_fail(vm, fn, "() needs a button number, not a ",
                       vm_typename(a[0].tag));
    int32_t i = floor_int(a[0]);
    if (i < 0 || i > 4)
        return vm_fail(vm, fn, "() takes a button from 0 to 4", "");
    *out = mk(T_INT, (bits >> i) & 1);
    return 1;
}

NATIVE(btn) { return button_arg(vm, a, argc, vm->held, "btn", out); }
NATIVE(btnp) { return button_arg(vm, a, argc, vm->pressed, "btnp", out); }

NATIVE(wheel)
{
    (void)a; (void)argc;
    *out = mk(T_INT, vm->wheel);
    return 1;
}

NATIVE(wheelpos)
{
    (void)a; (void)argc;
    *out = mk(T_INT, vm->wheelpos);
    return 1;
}

/* menuitem(i, label, fn): item i, 1-5, of the pause menu, calling fn when
 * chosen; without a label, removes it. */
NATIVE(menuitem)
{
    (void)out;
    if (!is_num(a[0]))
        return vm_fail(vm, "menuitem() needs an item number, not a ",
                       vm_typename(a[0].tag), "");
    int32_t i = floor_int(a[0]);
    if (i < 1 || i > 5)
        return vm_fail(vm, "menuitem() takes items 1 to 5", "", "");
    if (argc < 2 || a[1].tag == T_NULL) {
        vm->menu_label[i - 1] = mk(T_NULL, 0);
        vm->menu_fn[i - 1] = mk(T_NULL, 0);
        return 1;
    }
    struct value label;
    if (!vm_tostring(vm, a[1], &label))
        return 0;
    if (argc > 2 && a[2].tag != T_NULL && a[2].tag != T_FN)
        return vm_fail(vm, "menuitem() needs a function, not a ",
                       vm_typename(a[2].tag), "");
    vm->menu_label[i - 1] = label;
    vm->menu_fn[i - 1] = argc > 2 ? a[2] : mk(T_NULL, 0);
    return 1;
}

int arcade_menu_item(const struct vm *vm, int i, char *buf, int cap)
{
    if (i < 1 || i > 5 || vm->menu_label[i - 1].tag != T_STR || cap < 1)
        return 0;
    const struct str *s = MEM(vm, (uint32_t)vm->menu_label[i - 1].v);
    int n = (int)s->len < cap - 1 ? (int)s->len : cap - 1;
    for (int k = 0; k < n; k++)
        buf[k] = (char)s->b[k];
    buf[n] = 0;
    return 1;
}

/* Back to the menu once this tick is over; the host saves on the way. */
NATIVE(quit)
{
    (void)a; (void)argc; (void)out;
    vm->quit = 1;
    return 1;
}

int arcade_rate(const struct vm *vm)
{
    return vm->rate;
}

void arcade_skipped(struct vm *vm, unsigned frames)
{
    vm->skipped += frames;
}

/* rate(): the ticks a second now */
NATIVE(rate)
{
    (void)a; (void)argc;
    *out = mk(T_INT, vm->rate);
    return 1;
}

/* setrate(n): 20, 25, 33 or 50 ticks a second from the next tick. t()
 * carries on from where it is. */
NATIVE(setrate)
{
    (void)argc; (void)out;
    int32_t r = is_num(a[0]) ? floor_int(a[0]) : 0;
    if (r != 20 && r != 25 && r != 33 && r != 50)
        return vm_fail(vm, "setrate() takes 20, 25, 33 or 50", "", "");
    vm->t_base += (int64_t)((uint64_t)(vm->frame - vm->frame_base) * 65536 /
                            vm->rate);
    vm->frame_base = vm->frame;
    vm->rate = (uint8_t)r;
    return 1;
}

/* playback.playing(): whether the owner's music is playing -- for a cart
 * to step its rate back up when the music stops */
NATIVE(playback_playing)
{
    (void)a; (void)argc;
    *out = mk(T_INT, vm->host && vm->host->playing &&
                     vm->host->playing(vm->host->ctx) ? 1 : 0);
    return 1;
}

/* ---- beat: the music's time and its beat --------------------------------
 * Track ms throughout. The clock is the host's reading for this tick, so
 * a frame drawn from it is steady; the tracker's calls go to the host.
 * With no music to follow, the clock stands at 0 and there is no lock. */

static const struct arcade_beat *beat_of(struct vm *vm)
{
    return vm->host ? vm->host->beat : NULL;
}

/* beat.listen(on): start or stop the analyser -- costly, so a cart
 * listens until the tempo is held, then stops and projects from it */
NATIVE(beat_listen)
{
    (void)argc; (void)out;
    const struct arcade_beat *b = beat_of(vm);
    if (b && b->listen)
        b->listen(vm->host->ctx, vm_truthy(vm, a[0]));
    return 1;
}

/* beat.state(out): into an Int32Array of 6 -- locked, bpm, period ms,
 * confidence, the envelope's fill, beats since the lock. Returns locked */
NATIVE(beat_state)
{
    (void)argc;
    int32_t s[6] = { 0 };
    if (a[0].tag != T_I32 || I32(vm, a[0])->len < 6)
        return vm_fail(vm, "beat.state() needs an Int32Array of 6", "", "");
    const struct arcade_beat *b = beat_of(vm);
    if (b && b->state)
        b->state(vm->host->ctx, s);
    memcpy(I32(vm, a[0])->d, s, sizeof(s));
    return RET_INT(s[0] ? 1 : 0);
}

/* beat.next(from, n): the nth beat at or after from, or -1 with no lock;
 * beat.inbar() is then its place in the bar */
NATIVE(beat_next)
{
    (void)argc;
    if (!is_num(a[0]) || !is_num(a[1]))
        return vm_fail(vm, "beat.next() needs a time and a count", "", "");
    const struct arcade_beat *b = beat_of(vm);
    int32_t at = -1, in_bar = -1;
    int32_t from = floor_int(a[0]), n = floor_int(a[1]);
    /* a day of track and 4096 beats are past any music: none */
    if (b && b->next && from > -86400000 && from < 86400000 && n >= 0 &&
        n <= 4096)
        at = b->next(vm->host->ctx, from, n, &in_bar);
    vm->beat_inbar = at < 0 ? -1 : in_bar;
    return RET_INT(at);
}

NATIVE(beat_inbar)
{
    (void)a; (void)argc;
    return RET_INT(vm->beat_inbar);
}

/* beat.downbeat(): which of the four beats starts the bar, or -1 where
 * none wins or fewer than 32 beats have been heard; beat.margin() is by
 * how much, in tenths */
NATIVE(beat_downbeat)
{
    (void)a; (void)argc;
    const struct arcade_beat *b = beat_of(vm);
    int32_t margin = 0, d = -1;
    if (b && b->downbeat)
        d = b->downbeat(vm->host->ctx, &margin);
    vm->beat_margin = margin;
    return RET_INT(d);
}

NATIVE(beat_margin)
{
    (void)a; (void)argc;
    return RET_INT(vm->beat_margin);
}

NATIVE(beat_clock)
{
    (void)a; (void)argc;
    return RET_INT(vm->heard.clock);
}

/* beat.now(): this instant, for a moment within a tick */
NATIVE(beat_now)
{
    (void)a; (void)argc;
    const struct arcade_beat *b = beat_of(vm);
    return RET_INT(b && b->now ? b->now(vm->host->ctx) : vm->heard.clock);
}

/* beat.press(i): when button i went down this tick, or -1: a press is
 * judged by this, not by the tick it is seen in, which is up to a frame
 * later */
NATIVE(beat_press)
{
    (void)argc;
    if (!is_num(a[0]))
        return vm_fail(vm, "beat.press() needs a button", "", "");
    int32_t i = floor_int(a[0]);
    return RET_INT((uint32_t)i < 5 ? vm->heard.press[i] : -1);
}

NATIVE(beat_jumped)
{
    (void)a; (void)argc;
    return RET_INT(vm->heard.jumped ? 1 : 0);
}

NATIVE(beat_left)
{
    (void)a; (void)argc;
    return RET_INT(vm->heard.left);
}

/* beat.mood(out): the bar just heard, into an Int32Array of 3 -- energy,
 * flux, trend */
NATIVE(beat_mood)
{
    (void)argc; (void)out;
    int32_t m[3] = { 0 };
    if (a[0].tag != T_I32 || I32(vm, a[0])->len < 3)
        return vm_fail(vm, "beat.mood() needs an Int32Array of 3", "", "");
    const struct arcade_beat *b = beat_of(vm);
    if (b && b->mood)
        b->mood(vm->host->ctx, m);
    memcpy(I32(vm, a[0])->d, m, sizeof(m));
    return 1;
}

/* beat.ahead(out, from, to): the hits in the music from from to before to
 * that the analyser has already heard -- it reads the audio decoded ahead
 * of the speaker, so up to beat.horizon(), some two seconds on. Into an
 * Int32Array, three numbers a hit: its time, its bands (1 low, 2 mid,
 * 4 high) and its strength (128 level with the hits round it, 192 an
 * accent). Returns the hits; only while the cart listens */
NATIVE(beat_ahead)
{
    (void)argc;
    if (a[0].tag != T_I32 || !is_num(a[1]) || !is_num(a[2]))
        return vm_fail(vm, "beat.ahead() needs an Int32Array, and a time "
                       "from and to", "", "");
    int32_t max = (int32_t)(I32(vm, a[0])->len / 3), n = 0;
    const struct arcade_beat *b = beat_of(vm);
    if (b && b->ahead && max > 0)
        n = b->ahead(vm->host->ctx, floor_int(a[1]), floor_int(a[2]),
                     I32(vm, a[0])->d, max);
    vm_charge(vm, 20 + (uint32_t)n * 2);
    return RET_INT(n);
}

/* beat.horizon(): how far ahead beat.ahead() can see: the time of the
 * newest audio analysed, or -1 when not listening */
NATIVE(beat_horizon)
{
    (void)a; (void)argc;
    const struct arcade_beat *b = beat_of(vm);
    return RET_INT(b && b->horizon ? b->horizon(vm->host->ctx) : -1);
}

/* playback.bands(out, n): n spectrum bands, 1 to 8, low to high, each
 * 0-100, into an Int32Array -- the skins' spectrum, for this tick */
NATIVE(playback_bands)
{
    (void)argc; (void)out;
    if (a[0].tag != T_I32 || !is_num(a[1]))
        return vm_fail(vm, "playback.bands() needs an Int32Array and a count",
                       "", "");
    int32_t n = floor_int(a[1]);
    if (n < 1 || n > 8 || I32(vm, a[0])->len < (uint32_t)n)
        return vm_fail(vm, "playback.bands() takes 1 to 8 bands, and an "
                       "Int32Array that holds them", "", "");
    int32_t *d = I32(vm, a[0])->d;
    memset(d, 0, (size_t)n * sizeof(*d));
    const struct arcade_beat *b = beat_of(vm);
    if (b && b->bands)
        b->bands(vm->host->ctx, d, n);
    return 1;
}

/* playback.sound(out): the playing track's Sound Index record into an
 * Int32Array of SOUND.SIZE, field by field as SOUND.* names them: the whole
 * track's character, for a cart to choose by before it starts. Returns 0,
 * the array cleared, where the track has none */
NATIVE(playback_sound)
{
    (void)argc;
    int32_t s[31] = { 0 };
    if (a[0].tag != T_I32 || I32(vm, a[0])->len < 31)
        return vm_fail(vm, "playback.sound() needs an Int32Array of "
                       "SOUND.SIZE", "", "");
    const struct arcade_beat *b = beat_of(vm);
    int has = b && b->sound ? b->sound(vm->host->ctx, s) : 0;
    memcpy(I32(vm, a[0])->d, s, sizeof(s));
    return RET_INT(has ? 1 : 0);
}

/* ---- sys: the owner's settings and the firmware's phrases ---------------- */

/* sys.setting(name): the owner's setting by the name config.cfg gives it,
 * read-only -- a number where it is one, else its text as config.cfg
 * writes it ("on", a choice's name, a colour as rrggbb); null for no such
 * setting */
NATIVE(sys_setting)
{
    (void)argc;
    char name[64], text[128];
    if (a[0].tag != T_STR)
        return vm_fail(vm, "sys.setting() needs a setting's name", "", "");
    name[vm_text(vm, a[0], name, sizeof(name) - 1)] = '\0';
    int32_t num = 0;
    int kind = vm->host && vm->host->setting ?
               vm->host->setting(vm->host->ctx, name, &num, text,
                                 sizeof(text)) : 0;
    vm_charge(vm, 50);
    if (kind == 1)
        return RET_INT(num);
    if (kind != 2) {
        *out = mk(T_NULL, 0);
        return 1;
    }
    return vm_new_string(vm, (const uint8_t *)text, (uint32_t)strlen(text),
                         out);
}

/* sys.translate(phrase): a phrase the firmware has, given in English, in
 * the owner's language; anything else back as it came. It searches every
 * phrase, so a cart asks once and keeps the answer */
NATIVE(sys_translate)
{
    (void)argc;
    char english[128];
    if (a[0].tag != T_STR)
        return vm_fail(vm, "sys.translate() needs a phrase", "", "");
    english[vm_text(vm, a[0], english, sizeof(english) - 1)] = '\0';
    const char *t = vm->host && vm->host->translate ?
                    vm->host->translate(vm->host->ctx, english) : NULL;
    vm_charge(vm, 500);
    if (!t) {
        *out = a[0];
        return 1;
    }
    return vm_new_string(vm, (const uint8_t *)t, (uint32_t)strlen(t), out);
}

/* skipped(): frames the engine ran late and did not draw since the last
 * call */
NATIVE(skipped)
{
    (void)a; (void)argc;
    *out = mk(T_INT, (int32_t)vm->skipped);
    vm->skipped = 0;
    return 1;
}

int arcade_menu_is_game(const struct vm *vm)
{
    return vm->menu_button == 1;
}

const uint8_t *arcade_id(const struct vm *vm)
{
    return vm->id;
}

int arcade_quit_asked(const struct vm *vm)
{
    return vm->quit;
}
