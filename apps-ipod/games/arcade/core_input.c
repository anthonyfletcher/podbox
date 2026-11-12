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

/* The rows a list moves for clicks of the wheel in a tick of 1/rate s, as
 * the player's own lists move them: a step each SCROLL_CLICKS clicks, and
 * each step then many rows once the wheel's smoothed speed passes
 * SCROLL_START degrees a second, by the speed's fourth power. A step the
 * other way, or one after SCROLL_PAUSE ms with none, starts the speed
 * again. The click wheel's driver and button_apply_acceleration() hold
 * the same numbers for both players. */
#define SCROLL_CLICKS 4
#define SCROLL_START  270
#define SCROLL_PAUSE  250
#define SCROLL_FAST   36000         /* degrees a second: 100 turns */

static int32_t scroll_rows(uint32_t vel)
{
    uint64_t v = ((uint64_t)((1 << 16) / SCROLL_START) * vel) >> 12;
    v = (v * v * v * v + (1 << 15)) >> 16;
    return v > 1 ? (int32_t)v : 1;
}

int32_t arcade_scroll(struct arcade_scroll *s, int32_t clicks, uint32_t rate)
{
    uint32_t ms = 1000 / (rate ? rate : 30);
    s->carry += clicks;
    int32_t steps = s->carry / SCROLL_CLICKS;
    s->carry -= steps * SCROLL_CLICKS;
    if (!steps) {
        if (s->quiet < SCROLL_PAUSE)
            s->quiet += ms;
        return 0;
    }
    int32_t dir = steps > 0 ? 1 : -1, n = steps * dir;
    uint32_t span = s->quiet + ms;
    int restart = dir != s->dir || span > SCROLL_PAUSE;
    /* each step's speed, its clicks over its share of the time: 360
     * degrees a turn of 96 clicks, by the ms */
    uint32_t v = (uint32_t)n * SCROLL_CLICKS * 3750 / span;
    if (v > SCROLL_FAST)
        v = SCROLL_FAST;
    int32_t rows = 0;
    for (int32_t i = 0; i < n; i++) {
        s->vel = (15 * s->vel + v) / 16;
        if (!i && restart)
            s->vel = 0;
        rows += scroll_rows(s->vel);
    }
    s->dir = dir;
    s->quiet = 0;
    return rows * dir;
}

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
    vm->released = (uint8_t)(vm->held & ~held);
    vm->held = (uint8_t)held;
    vm->pressed = pressed;
    vm->wheel = wheel;
    vm->wheelsteps = arcade_scroll(&vm->scroll, wheel, vm->rate);
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
NATIVE(btnr) { return button_arg(vm, a, argc, vm->released, "btnr", out); }

/* unitsnow(): the units this tick has spent so far, to scale an effect down
 * on a slower player before it makes the tick late */
NATIVE(unitsnow)
{
    (void)a; (void)argc;
    *out = mk(T_INT, (int32_t)arcade_units_now(vm));
    return 1;
}

NATIVE(wheel)
{
    (void)a; (void)argc;
    *out = mk(T_INT, vm->wheel);
    return 1;
}

NATIVE(wheelsteps)
{
    (void)a; (void)argc;
    *out = mk(T_INT, vm->wheelsteps);
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

/* playback.toggle(), playback.next(), playback.prev(): the owner's music
 * paused or played, skipped on or back -- for a cart that holds the
 * buttons the music would otherwise have. A cart declares music "control"
 * for them. */
static int control(struct vm *vm, int what)
{
    if (vm->host && vm->host->control)
        vm->host->control(vm->host->ctx, what);
    vm_charge(vm, 20);
    return 1;
}

NATIVE(playback_toggle) { (void)a; (void)argc; (void)out; return control(vm, 0); }
NATIVE(playback_next) { (void)a; (void)argc; (void)out; return control(vm, 1); }
NATIVE(playback_prev) { (void)a; (void)argc; (void)out; return control(vm, 2); }

/* ---- playback and tracks: the playing track, the library, the queue ------
 * Through the host's arcade_tracks; with none (no music to read), the calls
 * answer as for nothing playing and an empty library. A track is its id. */

static const struct arcade_tracks *music_of(struct vm *vm)
{
    return vm->host ? vm->host->tracks : NULL;
}

static int position(struct vm *vm, int i, struct value *out)
{
    const struct arcade_tracks *t = music_of(vm);
    int32_t p[2] = { 0, 0 };
    if (t && t->position)
        t->position(vm->host->ctx, p);
    vm_charge(vm, 4);
    return RET_INT(p[i]);
}

NATIVE(playback_elapsed) { (void)a; (void)argc; return position(vm, 0, out); }
NATIVE(playback_length) { (void)a; (void)argc; return position(vm, 1, out); }

NATIVE(playback_changed)
{
    (void)a; (void)argc;
    return RET_INT(vm->heard.changed ? 1 : 0);
}

/* A string the host writes, up to 255 bytes, as a new string */
static int host_text(struct vm *vm, int n, const char *buf, struct value *out)
{
    vm_charge(vm, 10 + (uint32_t)n / 4);
    return vm_new_string(vm, (const uint8_t *)buf, (uint32_t)n, out);
}

static int playing_text(struct vm *vm, int what, struct value *out)
{
    const struct arcade_tracks *t = music_of(vm);
    char buf[256];
    int n = t && t->text ? t->text(vm->host->ctx, what, buf, 255) : 0;
    return host_text(vm, n < 0 ? 0 : n, buf, out);
}

NATIVE(playback_title) { (void)a; (void)argc; return playing_text(vm, 0, out); }
NATIVE(playback_artist) { (void)a; (void)argc; return playing_text(vm, 1, out); }
NATIVE(playback_album) { (void)a; (void)argc; return playing_text(vm, 2, out); }

NATIVE(playback_track)
{
    (void)a; (void)argc;
    const struct arcade_tracks *t = music_of(vm);
    vm_charge(vm, 10);
    return RET_INT(t && t->current ? t->current(vm->host->ctx) : -1);
}

NATIVE(playback_seek)
{
    (void)argc; (void)out;
    const struct arcade_tracks *t = music_of(vm);
    if (!is_num(a[0]))
        return vm_fail(vm, "playback.seek() needs a time in ms", "", "");
    int32_t ms = floor_int(a[0]);
    if (t && t->seek)
        t->seek(vm->host->ctx, ms < 0 ? 0 : ms);
    vm_charge(vm, 20);
    return 1;
}

/* playback.duck(level, [ticks]): the music to level / 256 of the owner's
 * volume, over ticks (0, at once), never above it; undone at the end */
NATIVE(playback_duck)
{
    (void)out;
    const struct arcade_tracks *t = music_of(vm);
    if (!is_num(a[0]))
        return vm_fail(vm, "playback.duck() needs a level, 0 to 256", "", "");
    int32_t level = floor_int(a[0]);
    int32_t ticks = argc > 1 && is_num(a[1]) ? floor_int(a[1]) : 0;
    level = level < 0 ? 0 : level > 256 ? 256 : level;
    if (t && t->duck)
        t->duck(vm->host->ctx, level, ticks < 0 ? 0 : ticks);
    vm_charge(vm, 10);
    return 1;
}

/* playback.volume(): the owner's volume, 0 to 100 of its range */
NATIVE(playback_volume)
{
    (void)a; (void)argc;
    const struct arcade_tracks *t = music_of(vm);
    vm_charge(vm, 10);
    return RET_INT(t && t->volume ? t->volume(vm->host->ctx, 0) : 0);
}

/* playback.adjust(n): the volume turned up (or, negative, down) n of the
 * player's own steps, as the wheel turns it; where it is then, 0 to 100 */
NATIVE(playback_adjust)
{
    (void)argc;
    const struct arcade_tracks *t = music_of(vm);
    if (!is_num(a[0]))
        return vm_fail(vm, "playback.adjust() needs a number of steps", "", "");
    int32_t n = floor_int(a[0]);
    n = n < -100 ? -100 : n > 100 ? 100 : n;
    vm_charge(vm, 10);
    return RET_INT(t && t->volume ? t->volume(vm->host->ctx, n) : 0);
}

NATIVE(playback_play)
{
    const struct arcade_tracks *t = music_of(vm);
    if (!is_num(a[0]))
        return vm_fail(vm, "playback.play() needs a track", "", "");
    int32_t ms = argc > 1 && is_num(a[1]) ? floor_int(a[1]) : 0;
    vm_charge(vm, 40);
    return RET_INT(t && t->play &&
                   t->play(vm->host->ctx, floor_int(a[0]), ms < 0 ? 0 : ms));
}

NATIVE(playback_stop)
{
    (void)a; (void)argc; (void)out;
    const struct arcade_tracks *t = music_of(vm);
    if (t && t->stop)
        t->stop(vm->host->ctx);
    vm_charge(vm, 20);
    return 1;
}

NATIVE(playback_started)
{
    (void)a; (void)argc;
    const struct arcade_tracks *t = music_of(vm);
    return RET_INT(t && t->started && t->started(vm->host->ctx));
}

/* A filter record ({ minlen, maxlen, genre, decade, music, artist }, any
 * field left out) as the host takes it; genre's and artist's text in buf,
 * half each */
static int filter_of(struct vm *vm, struct value v, struct arcade_filter *f,
                     char *buf, uint32_t cap, const char *fn)
{
    uint32_t half = cap / 2;
    memset(f, 0, sizeof(*f));
    if (v.tag == T_NULL)
        return 1;
    if (v.tag != T_OBJ)
        return vm_fail(vm, fn, "() takes a filter record: { minlen, maxlen, "
                       "genre, decade, music, artist }", "");
    struct value x;
    if (is_num(x = vm_get(vm, v, "minlen")))
        f->minlen = floor_int(x);
    if (is_num(x = vm_get(vm, v, "maxlen")))
        f->maxlen = floor_int(x);
    if (is_num(x = vm_get(vm, v, "decade")))
        f->decade = floor_int(x) / 10 * 10;
    f->music = vm_truthy(vm, vm_get(vm, v, "music"));
    if ((x = vm_get(vm, v, "genre")).tag == T_STR) {
        buf[vm_text(vm, x, buf, half - 1)] = '\0';
        f->genre = buf;
    }
    if ((x = vm_get(vm, v, "artist")).tag == T_STR) {
        char *s = buf + half;
        s[vm_text(vm, x, s, half - 1)] = '\0';
        f->artist = s;
    }
    return 1;
}

/* Up to n ids from the host into the array or Int32Array out */
static int ids_out(struct vm *vm, struct value v, int32_t n, int32_t **ids,
                   const char *fn)
{
    uint32_t len = v.tag == T_I32 ? I32(vm, v)->len :
                   v.tag == T_ARR ? ARR(vm, v)->len : 0;
    if ((v.tag != T_I32 && v.tag != T_ARR) || n < 0 || (uint32_t)n > len)
        return vm_fail(vm, fn, "() writes its tracks to an array of at "
                       "least n", "");
    *ids = scratch(vm, (uint32_t)n * sizeof(int32_t) + 1);
    return *ids != NULL;
}

static void ids_store(struct vm *vm, struct value v, const int32_t *ids,
                      int32_t n)
{
    for (int32_t i = 0; i < n; i++) {
        if (v.tag == T_I32)
            I32(vm, v)->d[i] = ids[i];
        else
            BUF(vm, ARR(vm, v)->buf)->v[i] = mk(T_INT, ids[i]);
    }
}

/* A walk over the library: the call, and each track it looked at */
static uint32_t walk_cost(struct vm *vm, const struct arcade_tracks *t)
{
    int32_t n = t && t->walked ? t->walked(vm->host->ctx) : 0;
    return 200 + (n > 0 ? (uint32_t)n * 4 : 0);
}

NATIVE(tracks_count)
{
    const struct arcade_tracks *t = music_of(vm);
    struct arcade_filter f;
    char text[128];
    if (!filter_of(vm, argc > 0 ? a[0] : mk(T_NULL, 0), &f, text,
                   sizeof(text), "tracks.count"))
        return 0;
    int32_t n = t && t->count ? t->count(vm->host->ctx, &f) : 0;
    if (!vm_spend(vm, walk_cost(vm, t)))
        return 0;
    return RET_INT(n);
}

/* tracks.pick(n, filter, out): n tracks at random, into out; how many */
NATIVE(tracks_pick)
{
    (void)argc;
    const struct arcade_tracks *t = music_of(vm);
    struct arcade_filter f;
    char text[128];
    int32_t n = is_num(a[0]) ? floor_int(a[0]) : -1, *ids;
    if (!filter_of(vm, a[1], &f, text, sizeof(text), "tracks.pick") ||
        !ids_out(vm, a[2], n, &ids, "tracks.pick"))
        return 0;
    int32_t k = t && t->pick && n ? t->pick(vm->host->ctx, n, &f, ids) : 0;
    ids_store(vm, a[2], ids, k);
    if (!vm_spend(vm, walk_cost(vm, t) + (uint32_t)k * 4))
        return 0;
    return RET_INT(k);
}

/* tracks.field(id, TRACK.x): a string or a number, or null */
NATIVE(tracks_field)
{
    (void)argc;
    const struct arcade_tracks *t = music_of(vm);
    char buf[256];
    int32_t num = 0;
    if (!is_num(a[0]) || !is_num(a[1]))
        return vm_fail(vm, "tracks.field() needs a track and a TRACK field",
                       "", "");
    int kind = t && t->field ? t->field(vm->host->ctx, floor_int(a[0]),
                                        floor_int(a[1]), &num, buf, 255) : 0;
    if (kind == 1) {
        vm_charge(vm, 10);
        return RET_INT(num);
    }
    if (kind == 2) {
        buf[255] = '\0';
        return host_text(vm, (int)strlen(buf), buf, out);
    }
    *out = mk(T_NULL, 0);
    vm_charge(vm, 10);
    return 1;
}

/* tracks.near(id, n, out, [sound]): up to n tracks most like id, into out;
 * id a list of tracks, n for each in turn, -1 after the last of each */
NATIVE(tracks_near)
{
    const struct arcade_tracks *t = music_of(vm);
    int32_t n = is_num(a[1]) ? floor_int(a[1]) : -1, *ids;
    int32_t ask[ARCADE_NEAR_IDS], count = 1;
    int sound = argc > 3 && vm_truthy(vm, a[3]);
    if (is_num(a[0])) {
        ask[0] = floor_int(a[0]);
    } else if (a[0].tag == T_I32 || a[0].tag == T_ARR) {
        count = a[0].tag == T_I32 ? (int32_t)I32(vm, a[0])->len
                                  : (int32_t)ARR(vm, a[0])->len;
        if (count < 1 || count > ARCADE_NEAR_IDS)
            return vm_fail(vm, "tracks.near() takes 1 to 10 tracks", "", "");
        for (int32_t i = 0; i < count; i++) {
            struct value v = a[0].tag == T_I32
                ? mk(T_INT, I32(vm, a[0])->d[i])
                : BUF(vm, ARR(vm, a[0])->buf)->v[i];
            ask[i] = is_num(v) ? floor_int(v) : -1;
        }
    } else {
        return vm_fail(vm, "tracks.near() needs a track, or a list of them",
                       "", "");
    }
    if (n > ARCADE_NEAR_MAX)
        n = ARCADE_NEAR_MAX;
    if (!ids_out(vm, a[2], n < 0 ? -1 : n * count, &ids, "tracks.near"))
        return 0;
    int32_t k = t && t->near && n ? t->near(vm->host->ctx, ask, count, n,
                                            sound, ids) : 0;
    ids_store(vm, a[2], ids, count > 1 ? n * count : k);
    if (!vm_spend(vm, walk_cost(vm, t) + (uint32_t)k * 4))
        return 0;
    return RET_INT(k);
}

/* tracks.sound(id, out): as playback.sound(), for any track */
NATIVE(tracks_sound)
{
    (void)argc;
    const struct arcade_tracks *t = music_of(vm);
    int32_t s[ARCADE_SOUND_FIELDS] = { 0 };
    if (!is_num(a[0]) || a[1].tag != T_I32 ||
        I32(vm, a[1])->len < ARCADE_SOUND_FIELDS)
        return vm_fail(vm, "tracks.sound() needs a track and an Int32Array "
                       "of SOUND.SIZE", "", "");
    int has = t && t->sound ? t->sound(vm->host->ctx, floor_int(a[0]), s) : 0;
    memcpy(I32(vm, a[1])->d, s, sizeof(s));
    vm_charge(vm, 40);
    return RET_INT(has ? 1 : 0);
}

static int32_t queued(struct vm *vm, int32_t *at)
{
    const struct arcade_tracks *t = music_of(vm);
    *at = -1;
    vm_charge(vm, 10);
    return t && t->queued ? t->queued(vm->host->ctx, at) : 0;
}

NATIVE(tracks_queued)
{
    (void)a; (void)argc;
    int32_t at;
    return RET_INT(queued(vm, &at));
}

NATIVE(tracks_place)
{
    (void)a; (void)argc;
    int32_t at;
    queued(vm, &at);
    return RET_INT(at);
}

/* tracks.queue(i): the track at place i of the queue, or -1 */
NATIVE(tracks_queue)
{
    (void)argc;
    const struct arcade_tracks *t = music_of(vm);
    if (!is_num(a[0]))
        return vm_fail(vm, "tracks.queue() needs a place in the queue", "",
                       "");
    vm_charge(vm, 20);
    return RET_INT(t && t->queue_at ? t->queue_at(vm->host->ctx,
                                                  floor_int(a[0])) : -1);
}

/* tracks.enqueue(id, [next]): added after the playing track (next true)
 * or at the end of the queue; whether it was */
NATIVE(tracks_enqueue)
{
    const struct arcade_tracks *t = music_of(vm);
    if (!is_num(a[0]))
        return vm_fail(vm, "tracks.enqueue() needs a track", "", "");
    int next = argc > 1 && vm_truthy(vm, a[1]);
    vm_charge(vm, 40);
    return RET_INT(t && t->enqueue &&
                   t->enqueue(vm->host->ctx, floor_int(a[0]), next));
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
    vm->listening = (uint8_t)vm_truthy(vm, a[0]);
    if (b && b->listen)
        b->listen(vm->host->ctx, vm->listening);
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
    int32_t t = (uint32_t)i < 5 ? vm->heard.press[i] : -1;
    if (t >= 0) {
        t -= vm->beat_offset;
        if (t < 0)
            t = 0;
    }
    return RET_INT(t);
}

/* beat.offset([ms]): the cart's calibration, taken off every press
 * beat.press() reports -- a player who presses 40 ms late on what they
 * hear sets 40 -- from -500 to 500; the offset in force */
NATIVE(beat_offset)
{
    if (argc > 0) {
        if (!is_num(a[0]))
            return vm_fail(vm, "beat.offset() takes ms", "", "");
        int32_t ms = floor_int(a[0]);
        vm->beat_offset = ms < -500 ? -500 : ms > 500 ? 500 : ms;
    }
    return RET_INT(vm->beat_offset);
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
    int32_t s[ARCADE_SOUND_FIELDS] = { 0 };
    if (a[0].tag != T_I32 || I32(vm, a[0])->len < ARCADE_SOUND_FIELDS)
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

void arcade_paused(struct vm *vm)
{
    vm->paused = 1;
}

/* resumed(): whether the game was paused -- the pause menu, the hold
 * switch -- since the last call */
NATIVE(resumed)
{
    (void)a; (void)argc;
    *out = mk(T_INT, vm->paused);
    vm->paused = 0;
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
