/* pbgvm: runs a cart's script on the host, headless -- the engine's own
 * VM, built for this machine, for the SDK's tests.
 *
 *   pbgvm cart.pbg [--ticks n] [--seed s] [--ceiling units] [--units]
 *                  [--shot file.png] [--dump x,y,w,h] [--dirty]
 *                  [--wav file.wav] [--buttons t:mask,...] [--skip t:n,...]
 *                  [--wheel t:clicks[:pos],...] [--playing] [--beat bpm]
 *                  [--setting name=value]... [--menu i]...
 *                  [--scores file] [--options file] [--opt t:i:v,...]
 *                  [--ui answer,...] [--speech] [--suspend-at t]
 *
 * printh() lines go to stdout. A runtime error goes to stderr as
 * file:line: message (in function), and the exit status is 1. With
 * --units, the units of each call follow on stderr. After the last tick,
 * --shot saves the panel as the player shows it, the 160x120 screen
 * doubled to 320x240, and --dump prints a rectangle of the screen as one
 * hex digit a pixel (the colour's low four bits), a row a line, and
 * --dirty prints "dirty lo hi", the rows the last tick drew. --wav saves
 * the sound, 44.1 kHz stereo, a tick's worth after each tick, and click()
 * prints "click 0" or "click 1". --buttons holds button masks (1 PREV,
 * 2 NEXT, 4 MENU, 8 PLAY, 16 SELECT) from the ticks given, each until the
 * next: "30:16,31:0" presses SELECT for tick 30. --skip reports frames
 * the host fell behind by before the ticks given, as the player does when
 * a tick runs late: "10:1" is one skipped frame before tick 10.
 * --wheel turns the wheel: "5:12:30" is 12 clicks at tick 5 with the
 * finger at 30 from then on (-1 lifts it).
 * --playing has playback.playing() report music playing. --beat bpm plays
 * a steady track at that tempo for beat: its clock follows the ticks, the
 * tracker locks a second after beat.listen(true), and presses are stamped
 * with the tick's time. --setting name=value is what sys.setting(name)
 * gives, a number where the value is one; sys.translate() is English. Each
 * --menu i then chooses pause-menu item i with SELECT, printing the items
 * before and the answer: "menu: <label>|<label>..." and "chose i: r".
 * --scores keeps the cart's high scores in file, read before the start
 * and written after the last tick, in the player's own record. score.show
 * prints "show <title>", then a line for each rank, "<rank> <value>
 * <name> <date>", the marked rank starred. --options does the same for
 * the option values the owner set, and --opt sets option i to v before
 * tick t, as the pause menu does. Each ui.* dialog prints a line saying
 * what it showed and takes the next of --ui's answers: an index for
 * choose (-1 backs out), 1 or 0 for confirm, a number for number, and the
 * text for text (~ backs out). message takes none. Without an answer,
 * each backs out. speak.say and queue print "say <text>" and "queue
 * <text>"; speak.on() is true with --speech, and nothing is ever busy.
 * --suspend-at t suspends the cart before tick t as the player's Quit
 * does, overwrites the arena, and resumes it from the copy, printing
 * "resumed" -- the run's output is then the same as one never suspended. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "games/arcade/arcade.h"

struct mem_in
{
    const unsigned char *b;
    uint32_t len, at;
};

static uint32_t mem_read(void *ctx, void *buf, uint32_t n)
{
    struct mem_in *in = ctx;
    uint32_t left = in->len - in->at;
    if (n > left)
        n = left;
    if (buf)
        memcpy(buf, in->b + in->at, n);
    in->at += n;
    return n;
}

static void print(void *ctx, const char *s, int len)
{
    (void)ctx;
    fwrite(s, 1, (size_t)len, stdout);
    fputc('\n', stdout);
}

/* --playing: the owner's music plays, as playback.playing() reports */
static int playing(void *ctx)
{
    (void)ctx;
    return 1;
}

/* --beat bpm: a three-minute track at a steady tempo, its first beat at
 * 0 and the bar starting on it. The clock follows the ticks; the tracker
 * locks a second after beat.listen(true), as a real one takes a while. */
static struct
{
    int32_t period, clock, listen_at, locked, listening;
} fake;

static void fake_listen(void *ctx, int on)
{
    (void)ctx;
    if (on && fake.listen_at < 0 && !fake.locked)
        fake.listen_at = fake.clock;
    fake.listening = on;
}

static void fake_lock(void)
{
    if (fake.listen_at >= 0 && fake.clock >= fake.listen_at + 1000)
        fake.locked = 1;
}

static void fake_state(void *ctx, int32_t out[6])
{
    (void)ctx;
    fake_lock();
    out[0] = fake.locked;
    out[1] = 60000 / fake.period;
    out[2] = fake.period;
    out[3] = fake.locked ? 90 : 40;
    out[4] = fake.locked ? 100 : 50;
    out[5] = fake.locked ? (fake.clock - fake.listen_at) / fake.period : 0;
}

static int32_t fake_next(void *ctx, int32_t from, int32_t n, int32_t *in_bar)
{
    (void)ctx;
    fake_lock();
    if (!fake.locked)
        return -1;
    int32_t k = from <= 0 ? 0 : (from + fake.period - 1) / fake.period;
    k += n;
    *in_bar = k % 4;
    return k * fake.period;
}

static int32_t fake_downbeat(void *ctx, int32_t *margin10)
{
    (void)ctx;
    *margin10 = fake.locked ? 30 : 0;
    return fake.locked ? 0 : -1;
}

static int32_t fake_now(void *ctx)
{
    (void)ctx;
    return fake.clock;
}

/* the bar's energy rises and falls over eight bars */
static void fake_mood(void *ctx, int32_t out[3])
{
    (void)ctx;
    int32_t bar = fake.clock / (fake.period * 4), k = bar % 8;
    out[0] = 30 + (k < 4 ? k : 8 - k) * 15;
    out[1] = 20;
    out[2] = k < 4 ? 1 : -1;
}

/* a spectrum that leans to the low end and pulses on the beat */
static void fake_bands(void *ctx, int32_t *out, int32_t n)
{
    (void)ctx;
    int32_t pulse = 30 - (fake.clock % fake.period) * 30 / fake.period;
    for (int32_t k = 0; k < n; k++)
        out[k] = 60 - k * 40 / n + (k == 0 ? pulse : 0);
}

/* the track's record: steady at the tempo, A minor, moderately loud */
static int fake_sound(void *ctx, int32_t out[ARCADE_SOUND_FIELDS])
{
    (void)ctx;
    static const int32_t rest[28] = {
        -120, 12, 70, 60, 50, 40, 30, 40, 50, 70, 40,
        10, 2, 8, 2, 6, 6, 2, 8, 2, 9, 2, 6,
        9, 1, 25, 60, 30
    };
    out[0] = fake.period;
    out[1] = 80;
    out[2] = 1;
    memcpy(out + 3, rest, 28 * sizeof(int32_t));
    return 1;
}

/* While listening, the 1.8 s ahead are heard: a kick on every beat, a
 * snare (mid) on the second and fourth beats, a hat (high) on the
 * off-beats;
 * the kick that starts the bar an accent */
static int32_t fake_ahead(void *ctx, int32_t from, int32_t to, int32_t *out,
                          int32_t max)
{
    (void)ctx;
    int32_t half = fake.period / 2, n = 0;
    if (!fake.listening)
        return 0;
    if (to > fake.clock + 1800)
        to = fake.clock + 1800;
    for (int32_t k = from <= 0 ? 0 : (from + half - 1) / half;
         k * half < to && n < max; k++) {
        int32_t beat = k / 2, bands = k % 2 ? 4 : 1 | (beat % 2 ? 2 : 0);
        out[n * 3] = k * half;
        out[n * 3 + 1] = bands;
        out[n * 3 + 2] = k % 8 == 0 ? 192 : 128;
        n++;
    }
    return n;
}

static int32_t fake_horizon(void *ctx)
{
    (void)ctx;
    return fake.listening ? fake.clock + 1800 : -1;
}

static const struct arcade_beat fake_beat = {
    fake_listen, fake_state, fake_next, fake_downbeat, fake_now, fake_mood,
    fake_bands, fake_sound, fake_ahead, fake_horizon
};

/* --setting name=value: what sys.setting(name) gives, a number where the
 * value is one; none are set otherwise. sys.translate() is English. */
static const char *settings[16];
static int nsettings;

static int fake_setting(void *ctx, const char *name, int32_t *num, char *buf,
                        int cap)
{
    (void)ctx;
    size_t len = strlen(name);
    for (int i = 0; i < nsettings; i++)
        if (!strncmp(settings[i], name, len) && settings[i][len] == '=') {
            const char *v = settings[i] + len + 1;
            char *end;
            long n = strtol(v, &end, 10);
            if (*v && !*end) {
                *num = (int32_t)n;
                return 1;
            }
            snprintf(buf, (size_t)cap, "%s", v);
            return 2;
        }
    return 0;
}

static void click(void *ctx, int beep)
{
    (void)ctx;
    printf("click %d\n", beep);
}

/* playback and tracks: a library of twenty made-up tracks -- "Track n" by
 * Artist A, B or C on Album n / 4, Rock, Jazz or Pop in turn, from 1970 a
 * year each, 2 to 4 minutes, track 19 spoken word -- and a queue of the
 * first ten, playing the first. The clock is the playing track's; a skip
 * or a play moves the queue's place and says the track changed. */
#define LIB 20
static struct
{
    int32_t queue[LIB], nqueue, at, owned, duck, changed;
} lib = { { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 }, 10, 0, 0, 256, 0 };

static int lib_spoken(int32_t id) { return id == 19; }
static int32_t lib_length(int32_t id) { return 120000 + id * 6000; }
static int32_t lib_year(int32_t id) { return 1970 + id; }
static const char *lib_genre(int32_t id)
{
    static const char *const g[] = { "Rock", "Jazz", "Pop" };
    return g[id % 3];
}

/* s into buf, cap bytes with its end, cut short; its length */
static int lib_copy(char *buf, int cap, const char *s)
{
    int n = (int)strlen(s);
    if (n > cap - 1)
        n = cap - 1;
    memcpy(buf, s, (size_t)n);
    buf[n] = '\0';
    return n;
}

static int32_t fake_current(void *ctx)
{
    (void)ctx;
    return lib.at >= 0 && lib.at < lib.nqueue ? lib.queue[lib.at] : -1;
}

static void fake_position(void *ctx, int32_t out[2])
{
    int32_t id = fake_current(ctx);
    out[0] = id < 0 ? 0 : fake.clock;
    out[1] = id < 0 ? 0 : lib_length(id);
}

static int fake_field(void *ctx, int32_t id, int f, int32_t *num, char *buf,
                      int cap)
{
    char t[32];
    (void)ctx;
    if (id < 0 || id >= LIB)
        return 0;
    switch (f) {
    case ARCADE_TRACK_TITLE:
        snprintf(t, sizeof(t), "Track %d", (int)id);
        lib_copy(buf, cap, t);
        return 2;
    case ARCADE_TRACK_ARTIST:
        snprintf(t, sizeof(t), "Artist %c", 'A' + (int)(id % 3));
        lib_copy(buf, cap, t);
        return 2;
    case ARCADE_TRACK_ALBUM:
        snprintf(t, sizeof(t), "Album %d", (int)(id / 4));
        lib_copy(buf, cap, t);
        return 2;
    case ARCADE_TRACK_GENRE:
        lib_copy(buf, cap, lib_genre(id));
        return 2;
    case ARCADE_TRACK_YEAR:
        *num = lib_year(id);
        return 1;
    case ARCADE_TRACK_LENGTH:
        *num = lib_length(id);
        return 1;
    }
    return 0;
}

static int fake_text(void *ctx, int what, char *buf, int cap)
{
    int32_t id = fake_current(ctx), n;
    char t[32];
    if (id < 0 || fake_field(ctx, id, what, &n, t, sizeof(t)) != 2)
        return 0;
    return lib_copy(buf, cap, t);
}

static void fake_seek(void *ctx, int32_t ms)
{
    (void)ctx;
    printf("seek %d\n", (int)ms);
}

static void fake_duck(void *ctx, int32_t level, int32_t ticks)
{
    (void)ctx;
    lib.duck = level;
    printf("duck %d over %d\n", (int)level, (int)ticks);
}

static int fake_play(void *ctx, int32_t id, int32_t ms)
{
    (void)ctx;
    if (id < 0 || id >= LIB)
        return 0;
    lib.owned = 1;
    lib.queue[0] = id;
    lib.nqueue = 1;
    lib.at = 0;
    lib.changed = 1;
    printf("play %d from %d\n", (int)id, (int)ms);
    return 1;
}

static void fake_stop(void *ctx)
{
    (void)ctx;
    printf("stop\n");
}

static int fake_started(void *ctx)
{
    (void)ctx;
    return lib.owned;
}

static int fake_matches(int32_t id, const struct arcade_filter *f)
{
    if (!f)
        return 1;
    if (f->music && lib_spoken(id))
        return 0;
    if (f->minlen && lib_length(id) < f->minlen)
        return 0;
    if (f->maxlen && lib_length(id) > f->maxlen)
        return 0;
    if (f->genre && strcmp(f->genre, lib_genre(id)))
        return 0;
    if (f->decade && lib_year(id) / 10 * 10 != f->decade)
        return 0;
    return 1;
}

static int32_t fake_count(void *ctx, const struct arcade_filter *f)
{
    int32_t n = 0;
    (void)ctx;
    for (int32_t id = 0; id < LIB; id++)
        n += fake_matches(id, f);
    return n;
}

/* the first n that match, in order: the test's "random" */
static int32_t fake_pick(void *ctx, int32_t n, const struct arcade_filter *f,
                         int32_t *out)
{
    int32_t k = 0;
    (void)ctx;
    for (int32_t id = 0; id < LIB && k < n; id++)
        if (fake_matches(id, f))
            out[k++] = id;
    return k;
}

/* the same genre first, then the nearest years */
static int32_t fake_near(void *ctx, int32_t id, int32_t n, int32_t *out)
{
    int32_t k = 0;
    (void)ctx;
    if (id < 0 || id >= LIB)
        return 0;
    for (int32_t d = 1; d < LIB && k < n; d++)
        for (int s = -1; s <= 1 && k < n; s += 2) {
            int32_t j = id + s * d;
            if (j >= 0 && j < LIB && !strcmp(lib_genre(j), lib_genre(id)))
                out[k++] = j;
        }
    return k;
}

static int fake_track_sound(void *ctx, int32_t id, int32_t out[ARCADE_SOUND_FIELDS])
{
    (void)ctx;
    if (id < 0 || id >= LIB)
        return 0;
    memset(out, 0, ARCADE_SOUND_FIELDS * sizeof(int32_t));
    out[0] = 400 + id * 10;
    return 1;
}

static int32_t fake_queued(void *ctx, int32_t *at)
{
    (void)ctx;
    *at = lib.at;
    return lib.nqueue;
}

static int32_t fake_queue_at(void *ctx, int32_t i)
{
    (void)ctx;
    return i >= 0 && i < lib.nqueue ? lib.queue[i] : -1;
}

static int fake_enqueue(void *ctx, int32_t id, int next)
{
    (void)ctx;
    if (id < 0 || id >= LIB || lib.nqueue >= LIB)
        return 0;
    int32_t at = next ? lib.at + 1 : lib.nqueue;
    memmove(lib.queue + at + 1, lib.queue + at,
            (size_t)(lib.nqueue - at) * sizeof(int32_t));
    lib.queue[at] = id;
    lib.nqueue++;
    return 1;
}

static const struct arcade_tracks fake_tracks = {
    fake_current, fake_position, fake_text, fake_seek, fake_duck,
    fake_play, fake_stop, fake_started, fake_count, fake_pick, fake_field,
    fake_near, fake_track_sound, fake_queued, fake_queue_at, fake_enqueue
};

/* playback.toggle(), next(), prev(): printed; a skip moves the queue */
static void control(void *ctx, int what)
{
    (void)ctx;
    printf("control %d\n", what);
    if ((what == 1 && lib.at + 1 < lib.nqueue) || (what == 2 && lib.at > 0)) {
        lib.at += what == 1 ? 1 : -1;
        lib.changed = 1;
    }
}

/* --wav: the sound, gathered tick by tick */
#define WAV_RATE 44100
static int16_t *wav;
static size_t wav_frames, wav_cap;

static void wav_tick(struct vm *vm, long tick)
{
    long rate = arcade_rate(vm);
    size_t want = (size_t)((tick + 1) * WAV_RATE / rate) - wav_frames;
    if (wav_frames + want > wav_cap) {
        wav_cap = (wav_frames + want) * 2;
        wav = realloc(wav, wav_cap * 4);
        if (!wav)
            exit(2);
    }
    arcade_audio(vm, wav + wav_frames * 2, (int)want, WAV_RATE);
    wav_frames += want;
}

static void put32(FILE *f, uint32_t v)
{
    fputc((int)(v & 0xff), f); fputc((int)(v >> 8 & 0xff), f);
    fputc((int)(v >> 16 & 0xff), f); fputc((int)(v >> 24), f);
}

static void wav_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    uint32_t bytes = (uint32_t)(wav_frames * 4);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put32(f, 1 | 2 << 16); put32(f, WAV_RATE);
    put32(f, WAV_RATE * 4); put32(f, 4 | 16 << 16);
    fwrite("data", 1, 4, f); put32(f, bytes);
    for (size_t i = 0; i < wav_frames * 2; i++) {
        uint16_t v = (uint16_t)wav[i];
        fputc(v & 0xff, f);
        fputc(v >> 8, f);
    }
    fclose(f);
}

static void date(void *ctx, int32_t out[7])
{
    (void)ctx;
    static const int32_t fixed[7] = { 2026, 11, 3, 12, 0, 0, 2 };
    memcpy(out, fixed, sizeof(fixed));
}

/* ---- high scores --------------------------------------------------------- */

static void scores_read(struct vm *vm, const char *path)
{
    static struct arcade_scores saved;
    unsigned char b[ARCADE_SCORES_MAX];
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    int n = (int)fread(b, 1, sizeof(b), f);
    fclose(f);
    if (arcade_scores_unpack(&saved, b, n))
        arcade_scores_restore(vm, &saved);
}

static int scores_write(struct vm *vm, const char *path)
{
    unsigned char b[ARCADE_SCORES_MAX];
    int n = arcade_scores_pack(arcade_scores(vm, NULL), b, sizeof(b));
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(b, 1, (size_t)n, f) != (size_t)n) {
        if (f)
            fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

static void scores_show(struct vm *vm)
{
    int rank, i = arcade_score_shown(vm, &rank);
    if (i < 0)
        return;
    const struct arcade_scores *s = arcade_scores(vm, NULL);
    const struct arcade_board *b = &s->b[i];
    printf("show %s\n", b->title);
    for (int k = 0; k < b->count; k++) {
        char v[32];
        arcade_score_text(s, i, b->e[k].value, v, sizeof(v));
        printf("%s%d %s %s %ld\n", k + 1 == rank ? "*" : "", k + 1, v,
               b->e[k].name, (long)b->e[k].date);
    }
}

/* ---- options ------------------------------------------------------------- */

static void options_read(struct vm *vm, const char *path)
{
    unsigned char b[ARCADE_OPTIONS_MAX];
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    int n = (int)fread(b, 1, sizeof(b), f);
    fclose(f);
    arcade_options_unpack(vm, b, n);
}

static int options_write(struct vm *vm, const char *path)
{
    unsigned char b[ARCADE_OPTIONS_MAX];
    int n = arcade_options_pack(vm, b, sizeof(b));
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(b, 1, (size_t)n, f) != (size_t)n) {
        if (f)
            fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

/* each --opt entry for tick t: "t:i:v" */
static void options_set(struct vm *vm, const char *spec, long t)
{
    for (const char *b = spec; b && *b;) {
        long at = strtol(b, (char **)&b, 10), i = -1, v = 0;
        if (*b == ':')
            i = strtol(b + 1, (char **)&b, 10);
        if (*b == ':')
            v = strtol(b + 1, (char **)&b, 10);
        if (at == t)
            arcade_option_set(vm, (int)i, (int32_t)v);
        if (*b == ',')
            b++;
        else
            break;
    }
}

/* ---- dialogs ------------------------------------------------------------- */

static const char *ui_answers;

/* The next --ui answer into buf; 0 when there are no more */
static int ui_next(char *buf, int cap)
{
    if (!ui_answers || !*ui_answers)
        return 0;
    const char *end = strchr(ui_answers, ',');
    int n = end ? (int)(end - ui_answers) : (int)strlen(ui_answers);
    snprintf(buf, (size_t)cap, "%.*s", n, ui_answers);
    ui_answers = end ? end + 1 : "";
    return 1;
}

static int ui_choose(void *ctx, const char *title, const char *const *items,
                     int n, int at)
{
    (void)ctx;
    char a[64];
    int r = ui_next(a, sizeof(a)) ? atoi(a) : -1;
    printf("choose %s:", title);
    for (int i = 0; i < n; i++)
        printf("%s%s", i ? "|" : " ", items[i]);
    printf(" at %d -> %d\n", at, r);
    return r;
}

static int ui_confirm(void *ctx, const char *title, const char *text)
{
    (void)ctx;
    char a[64];
    int r = ui_next(a, sizeof(a)) ? atoi(a) : 0;
    printf("confirm %s: %s -> %d\n", title, text, r);
    return r;
}

static void ui_message(void *ctx, const char *title, const char *text)
{
    (void)ctx;
    printf("message %s: %s\n", title, text);
}

static int32_t ui_number(void *ctx, const char *title, int32_t min,
                         int32_t max, int32_t step, int32_t value)
{
    (void)ctx;
    char a[64];
    int32_t r = ui_next(a, sizeof(a)) ? (int32_t)atol(a) : value;
    printf("number %s: %ld to %ld by %ld from %ld -> %ld\n", title,
           (long)min, (long)max, (long)step, (long)value, (long)r);
    return r;
}

static int ui_text(void *ctx, const char *title, char *buf, int cap)
{
    (void)ctx;
    char a[128];
    printf("text %s: [%s] up to %d", title, buf, cap - 1);
    if (!ui_next(a, sizeof(a)) || !strcmp(a, "~")) {
        printf(" -> cancelled\n");
        return 0;
    }
    snprintf(buf, (size_t)cap, "%s", a);
    printf(" -> [%s]\n", buf);
    return 1;
}

static const struct arcade_ui fake_ui = {
    ui_choose, ui_confirm, ui_message, ui_number, ui_text
};

/* ---- speech -------------------------------------------------------------- */

static int speech_flag;

static void sp_say(void *ctx, const char *text, int queue)
{
    (void)ctx;
    printf("%s %s\n", queue ? "queue" : "say", text);
}

static int sp_busy(void *ctx) { (void)ctx; return 0; }
static void sp_stop(void *ctx) { (void)ctx; printf("stop\n"); }
static int sp_on(void *ctx) { (void)ctx; return speech_flag; }

static const struct arcade_speak fake_speak = {
    sp_say, sp_busy, sp_stop, sp_on
};

/* ---- a PNG, uncompressed, so no zlib is needed -------------------------- */

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const unsigned char *b, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int j = 0; j < 8; j++)
                k = k & 1 ? 0xedb88320u ^ (k >> 1) : k >> 1;
            crc_table[i] = k;
        }
    c = ~c;
    while (n--)
        c = crc_table[(c ^ *b++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void be32(unsigned char *b, uint32_t v)
{
    b[0] = (unsigned char)(v >> 24);
    b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);
    b[3] = (unsigned char)v;
}

static void chunk(FILE *f, const char *tag, const unsigned char *d, size_t n)
{
    unsigned char h[8];
    be32(h, (uint32_t)n);
    memcpy(h + 4, tag, 4);
    fwrite(h, 1, 8, f);
    fwrite(d, 1, n, f);
    uint32_t c = crc(crc(0, (const unsigned char *)tag, 4), d, n);
    be32(h, c);
    fwrite(h, 1, 4, f);
}

static int write_png(const char *path, const unsigned char *rgb, int w, int h)
{
    static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    size_t row = (size_t)w * 3 + 1, raw = row * (size_t)h;
    size_t blocks = (raw + 65534) / 65535;
    unsigned char *z = malloc(raw + blocks * 5 + 6);
    unsigned char *line = malloc(raw);
    if (!z || !line)
        return 0;
    for (int y = 0; y < h; y++) {
        line[y * row] = 0;
        memcpy(line + y * row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    size_t n = 0, at = 0;
    uint32_t s1 = 1, s2 = 0;
    z[n++] = 0x78;
    z[n++] = 0x01;
    while (at < raw) {
        size_t k = raw - at < 65535 ? raw - at : 65535;
        z[n++] = at + k == raw;
        z[n++] = (unsigned char)k;
        z[n++] = (unsigned char)(k >> 8);
        z[n++] = (unsigned char)~k;
        z[n++] = (unsigned char)(~k >> 8);
        memcpy(z + n, line + at, k);
        for (size_t i = 0; i < k; i++) {
            s1 = (s1 + line[at + i]) % 65521;
            s2 = (s2 + s1) % 65521;
        }
        n += k;
        at += k;
    }
    be32(z + n, (s2 << 16) | s1);
    n += 4;
    unsigned char ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    memcpy(ihdr + 8, "\x08\x02\0\0\0", 5);
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    fwrite(sig, 1, 8, f);
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, n);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    free(line);
    return 1;
}

/* RGB565, as the players' panels take it. */
static uint16_t pack565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) |
                      ((c >> 3) & 0x001f));
}

/* The panel as present.c makes it, checked pixel for pixel against the
 * screen doubled the plain way; 0 when they differ. */
static uint16_t panel[320 * 240];

static int present_panel(struct vm *vm)
{
    const struct arcade_screen *s = arcade_screen(vm);
    static uint32_t lut2[256];
    arcade_present_lut(s, lut2, pack565);
    arcade_present_rows(s, lut2, panel, 320, 0, s->height - 1);
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++)
            if (panel[y * 320 + x] !=
                pack565(s->rgb[s->px[(y / 2) * s->width + x / 2]]))
                return 0;
    return 1;
}

/* The panel: the screen, each pixel doubled. */
static int shot(struct vm *vm, const char *path)
{
    const struct arcade_screen *s = arcade_screen(vm);
    static unsigned char rgb[320 * 240 * 3];
    if (!present_panel(vm)) {
        fprintf(stderr, "present.c does not match the screen\n");
        return 0;
    }
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) {
            uint32_t c = s->rgb[s->px[(y / 2) * s->width + x / 2]];
            unsigned char *o = rgb + ((size_t)y * 320 + x) * 3;
            o[0] = (unsigned char)(c >> 16);
            o[1] = (unsigned char)(c >> 8);
            o[2] = (unsigned char)c;
        }
    return write_png(path, rgb, 320, 240);
}

static void dump(struct vm *vm, const char *spec)
{
    int x, y, w, h;
    const struct arcade_screen *s = arcade_screen(vm);
    if (sscanf(spec, "%d,%d,%d,%d", &x, &y, &w, &h) != 4)
        return;
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            int c = i >= 0 && j >= 0 && i < s->width && j < s->height
                ? s->px[j * s->width + i] & 15 : 0;
            putchar("0123456789abcdef"[c]);
        }
        putchar('\n');
    }
}

static int report(struct vm *vm)
{
    const char *file, *fn;
    int line;
    const char *msg = arcade_error(vm, &file, &line, &fn);
    if (file)
        fprintf(stderr, "%s:%d: %s (in %s)\n", file, line, msg, fn);
    else
        fprintf(stderr, "%s\n", msg);
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    long ticks = 0;
    unsigned long long seed = 1;
    int units = 0, dirty = 0;
    unsigned long ceiling = 0;
    const char *shot_path = NULL, *dump_spec = NULL, *wav_path = NULL;
    const char *buttons = NULL, *skips = NULL, *turns = NULL;
    int pos = -1;
    int playing_flag = 0;
    long bpm = 0;
    int menu_picks[16], npicks = 0;
    const char *scores_path = NULL, *options_path = NULL, *opt_sets = NULL;
    long suspend_at = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ticks") && i + 1 < argc)
            ticks = atol(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--ceiling") && i + 1 < argc)
            ceiling = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
            shot_path = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc)
            dump_spec = argv[++i];
        else if (!strcmp(argv[i], "--menu") && i + 1 < argc && npicks < 16)
            menu_picks[npicks++] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--units"))
            units = 1;
        else if (!strcmp(argv[i], "--dirty"))
            dirty = 1;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc)
            wav_path = argv[++i];
        else if (!strcmp(argv[i], "--wheel") && i + 1 < argc)
            turns = argv[++i];
        else if (!strcmp(argv[i], "--setting") && i + 1 < argc &&
                 nsettings < 16)
            settings[nsettings++] = argv[++i];
        else if (!strcmp(argv[i], "--beat") && i + 1 < argc)
            bpm = atol(argv[++i]);
        else if (!strcmp(argv[i], "--playing"))
            playing_flag = 1;
        else if (!strcmp(argv[i], "--scores") && i + 1 < argc)
            scores_path = argv[++i];
        else if (!strcmp(argv[i], "--options") && i + 1 < argc)
            options_path = argv[++i];
        else if (!strcmp(argv[i], "--opt") && i + 1 < argc)
            opt_sets = argv[++i];
        else if (!strcmp(argv[i], "--ui") && i + 1 < argc)
            ui_answers = argv[++i];
        else if (!strcmp(argv[i], "--speech"))
            speech_flag = 1;
        else if (!strcmp(argv[i], "--suspend-at") && i + 1 < argc)
            suspend_at = atol(argv[++i]);
        else if (!strcmp(argv[i], "--buttons") && i + 1 < argc)
            buttons = argv[++i];
        else if (!strcmp(argv[i], "--skip") && i + 1 < argc)
            skips = argv[++i];
        else
            path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: pbgvm cart.pbg [--ticks n] [--seed s] "
                "[--ceiling units] [--units] [--shot file.png] "
                "[--dump x,y,w,h]\n");
        return 2;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 2;
    }
    static unsigned char file[4 << 20];
    struct mem_in in = { file, (uint32_t)fread(file, 1, sizeof(file), f), 0 };
    fclose(f);

    /* The player's limit: the 5G's whole app buffer. */
    static uint64_t arena[512 * 1024 / 8];
    struct arcade_host host = { NULL, print, NULL, date, click,
                                playing_flag || bpm ? playing : NULL,
                                bpm > 0 ? &fake_beat : NULL, fake_setting,
                                NULL, control, &fake_tracks, &fake_ui,
                                &fake_speak };
    fake.period = bpm > 0 ? (int32_t)(60000 / bpm) : 500;
    fake.listen_at = -1;
    unsigned last_held = 0;
    double track_ms = 0;
    struct vm *vm = arcade_load(arena, sizeof(arena), mem_read, &in, &host,
                                seed);
    if (!vm) {
        fprintf(stderr, "no memory\n");
        return 1;
    }
    if (arcade_error(vm, NULL, NULL, NULL))
        return report(vm);
    if (ceiling)
        arcade_set_ceiling(vm, (uint32_t)ceiling);
    if (scores_path)
        scores_read(vm, scores_path);
    if (options_path)
        options_read(vm, options_path);
    if (!arcade_start(vm))
        return report(vm);
    if (units)
        fprintf(stderr, "start %u\n", (unsigned)arcade_units(vm));
    unsigned held = 0;
    for (long t = 0; t < ticks; t++) {
        /* the mask of the latest --buttons entry at or before this tick */
        for (const char *b = buttons; b && *b;) {
            long at = strtol(b, (char **)&b, 10);
            unsigned mask = 0;
            if (*b == ':')
                mask = (unsigned)strtoul(b + 1, (char **)&b, 10);
            if (at == t)
                held = mask;
            if (*b == ',')
                b++;
            else
                break;
        }
        for (const char *b = skips; b && *b;) {
            long at = strtol(b, (char **)&b, 10);
            unsigned n = 0;
            if (*b == ':')
                n = (unsigned)strtoul(b + 1, (char **)&b, 10);
            if (at == t)
                arcade_skipped(vm, n);
            if (*b == ',')
                b++;
            else
                break;
        }
        int clicks = 0;
        for (const char *b = turns; b && *b;) {
            long at = strtol(b, (char **)&b, 10);
            long n = 0, p = pos;
            if (*b == ':')
                n = strtol(b + 1, (char **)&b, 10);
            if (*b == ':')
                p = strtol(b + 1, (char **)&b, 10);
            if (at == t) {
                clicks = (int)n;
                pos = (int)p;
            }
            if (*b == ',')
                b++;
            else
                break;
        }
        if (t == suspend_at) {
            static uint64_t copy[sizeof(arena) / 8];
            uint32_t n;
            const void *snap = arcade_snapshot(vm, &n);
            memcpy(copy, snap, n);
            memset(arena, 0xa5, sizeof(arena));
            memcpy(arena, copy, n);
            vm = arcade_resume(arena, n, &host);
            if (!vm) {
                fprintf(stderr, "not resumable\n");
                return 1;
            }
            printf("resumed\n");
        }
        arcade_input(vm, held, clicks, pos);
        options_set(vm, opt_sets, t);
        if (bpm > 0) {
            /* the clock a tick on, and each new press stamped with it */
            track_ms += 1000.0 / arcade_rate(vm);
            fake.clock = (int32_t)track_ms;
            struct arcade_music m = { fake.clock, 180000 - fake.clock, 0,
                                      lib.changed, { -1, -1, -1, -1, -1 } };
            lib.changed = 0;
            for (int i = 0; i < 5; i++)
                if ((held & ~last_held) & (1u << i))
                    m.press[i] = fake.clock;
            arcade_music(vm, &m);
        } else {
            /* no clock to follow: only a track changing */
            struct arcade_music m = { 0, 0, 0, lib.changed,
                                      { -1, -1, -1, -1, -1 } };
            lib.changed = 0;
            arcade_music(vm, &m);
        }
        last_held = held;
        arcade_presented(vm);
        if (!arcade_tick(vm, 1))
            return report(vm);
        if (units)
            fprintf(stderr, "tick %ld %u\n", t + 1, (unsigned)arcade_units(vm));
        if (wav_path)
            wav_tick(vm, t);
        scores_show(vm);
        arcade_idle(vm);
    }
    if (scores_path && !scores_write(vm, scores_path)) {
        fprintf(stderr, "could not write %s\n", scores_path);
        return 2;
    }
    if (options_path && !options_write(vm, options_path)) {
        fprintf(stderr, "could not write %s\n", options_path);
        return 2;
    }
    if (wav_path)
        wav_save(wav_path);
    if (dump_spec)
        dump(vm, dump_spec);
    if (dirty) {
        const struct arcade_screen *s = arcade_screen(vm);
        printf("dirty %d %d\n", s->dirty_lo, s->dirty_hi);
    }
    for (int k = 0; k < npicks; k++) {
        char label[64];
        printf("menu:");
        for (int i = 1; i <= 5; i++)
            if (arcade_menu_item(vm, i, label, sizeof(label)))
                printf(" %d=%s", i, label);
        printf("\n");
        int r = arcade_menu_choose(vm, menu_picks[k], 4);
        printf("chose %d: %d\n", menu_picks[k], r);
        if (r < 0)
            return report(vm);
    }
    if (shot_path && !shot(vm, shot_path)) {
        fprintf(stderr, "could not write %s\n", shot_path);
        return 2;
    }
    fflush(stdout);
    return 0;
}
