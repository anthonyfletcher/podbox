/* fakemusic: the owner's music, made up, for pbgvm and the IDE.
 *
 * The library is five albums and a chapter of an audiobook: twenty tracks,
 * each with a title, artist, album, genre, year, length and tempo. The
 * queue holds the first ten, playing the first. A track's clock runs while
 * the music plays and the host moves it on; one run out moves the queue on.
 *
 * The beat is the playing track's steady tempo, its first beat at 0 and
 * the bar starting on it, locked a second after the cart starts listening
 * as a real tracker takes a while; the spoken chapter has none. Its
 * spectrum pulses on the beat, its mood rises and falls over eight bars,
 * and the hits ahead are a kick each beat, a snare on two and four and a
 * hat between.
 *
 * Parts: the library; the queue and its clock; the beat; playback and
 * tracks for the engine, with each album's colours. */


#include <stdlib.h>
#include <string.h>
#include "fakemusic.h"

/* ---- the library --------------------------------------------------------- */

struct track
{
    const char *title, *artist, *album, *genre;
    int32_t year, seconds, bpm;
};

#define LIB 20
static const struct track lib_tracks[LIB] = {
    { "Lighthouse", "Harbour Lamps", "Northern Lights", "Rock", 1978, 214, 128 },
    { "Salt Wind", "Harbour Lamps", "Northern Lights", "Rock", 1978, 187, 96 },
    { "Across the Bay", "Harbour Lamps", "Northern Lights", "Rock", 1978, 243, 112 },
    { "Last Ferry", "Harbour Lamps", "Northern Lights", "Rock", 1978, 302, 84 },
    { "Half Past Midnight", "The Quiet Quartet", "Blue Hours", "Jazz", 1962, 326, 72 },
    { "Velvet Rain", "The Quiet Quartet", "Blue Hours", "Jazz", 1962, 281, 88 },
    { "Slow Walk Home", "The Quiet Quartet", "Blue Hours", "Jazz", 1962, 255, 64 },
    { "Coda in Blue", "The Quiet Quartet", "Blue Hours", "Jazz", 1962, 198, 100 },
    { "Paper Planes", "Mira Vale", "Paper Planes", "Pop", 1999, 201, 118 },
    { "Summer Static", "Mira Vale", "Paper Planes", "Pop", 1999, 189, 124 },
    { "Neon Heart", "Mira Vale", "Paper Planes", "Pop", 1999, 223, 120 },
    { "Stay Up Late", "Mira Vale", "Paper Planes", "Pop", 1999, 176, 126 },
    { "Full Steam", "Iron Kettle", "Engine Room", "Rock", 1985, 232, 140 },
    { "Boiler Plate", "Iron Kettle", "Engine Room", "Rock", 1985, 205, 152 },
    { "Night Shift", "Iron Kettle", "Engine Room", "Rock", 1985, 268, 132 },
    { "Pressure", "Iron Kettle", "Engine Room", "Rock", 1985, 249, 160 },
    { "Startup", "Lowfield", "Quiet Machines", "Electronic", 2014, 312, 122 },
    { "Idle Hands", "Lowfield", "Quiet Machines", "Electronic", 2014, 287, 110 },
    { "Soft Reset", "Lowfield", "Quiet Machines", "Electronic", 2014, 354, 90 },
    { "Chapter One", "Ada Rowe", "A Short History of Lighthouses", "Spoken Word",
      2020, 1146, 0 },
};

static int lib_spoken(int32_t id) { return lib_tracks[id].bpm == 0; }
static int32_t lib_length(int32_t id) { return lib_tracks[id].seconds * 1000; }

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

/* ---- the queue and its clock --------------------------------------------- */

static struct
{
    int32_t queue[LIB], nqueue, at, owned, duck, changed, jumped;
    int32_t clock, tempo, playing;
    int32_t listen_at, locked, listening;
    void (*say)(const char *line);
} m;

/* fmt with each %ld given a or then b; written out by hand, as the IDE's
 * engine has no stdio */
static void say(const char *fmt, long a, long b)
{
    char line[64];
    int n = 0, which = 0;
    if (!m.say)
        return;
    for (const char *p = fmt; *p && n < 40; p++) {
        if (p[0] != '%' || p[1] != 'l' || p[2] != 'd') {
            line[n++] = *p;
            continue;
        }
        long v = which++ ? b : a;
        unsigned long u = v < 0 ? 0ul - (unsigned long)v : (unsigned long)v;
        char rev[24];
        int k = 0;
        do
            rev[k++] = (char)('0' + u % 10);
        while ((u /= 10) && k < 20);
        if (v < 0)
            line[n++] = '-';
        while (k)
            line[n++] = rev[--k];
        p += 2;
    }
    line[n] = '\0';
    m.say(line);
}

void fake_music_init(void (*say_fn)(const char *line))
{
    memset(&m, 0, sizeof(m));
    for (int i = 0; i < 10; i++)
        m.queue[i] = i;
    m.nqueue = 10;
    m.duck = 256;
    m.listen_at = -1;
    m.say = say_fn;
}

void fake_music_tempo(int32_t bpm) { m.tempo = bpm; }
void fake_music_play(int on) { m.playing = on; }

int fake_playing(void *ctx)
{
    (void)ctx;
    return m.playing;
}

static int32_t current(void)
{
    return m.at >= 0 && m.at < m.nqueue ? m.queue[m.at] : -1;
}

/* a new track from its start: the tracker hears it afresh */
static void moved(void)
{
    m.clock = 0;
    m.changed = m.jumped = 1;
    m.locked = 0;
    m.listen_at = m.listening ? 0 : -1;
}

void fake_control(void *ctx, int what)
{
    (void)ctx;
    say("control %ld", what, 0);
    fake_music_control(what);
}

void fake_music_control(int what)
{
    if (what == 0)
        m.playing = !m.playing;
    else if ((what == 1 && m.at + 1 < m.nqueue) || (what == 2 && m.at > 0)) {
        m.at += what == 1 ? 1 : -1;
        moved();
    }
}

void fake_music_advance(int32_t ms)
{
    int32_t id = current();
    if (!m.playing || id < 0)
        return;
    m.clock += ms;
    if (m.clock < lib_length(id))
        return;
    if (m.at + 1 < m.nqueue) {
        m.at++;
        moved();
    } else {
        m.clock = lib_length(id);
        m.playing = 0;
    }
}

void fake_music_tick(struct arcade_music *out)
{
    int32_t id = current();
    out->clock = m.clock;
    out->left = id < 0 ? 0 : lib_length(id) - m.clock;
    out->jumped = m.jumped;
    out->changed = m.changed;
    m.jumped = m.changed = 0;
}

int32_t fake_music_now(int32_t *ms, int32_t *length, int32_t *bpm)
{
    int32_t id = current();
    *ms = m.clock;
    *length = id < 0 ? 0 : lib_length(id);
    *bpm = m.tempo ? m.tempo : id < 0 ? 0 : lib_tracks[id].bpm;
    return id;
}

/* ---- the beat ------------------------------------------------------------ */

/* the playing track's beat period in ms, or 0 for none */
static int32_t period(void)
{
    int32_t id = current();
    int32_t bpm = m.tempo ? m.tempo : id < 0 ? 0 : lib_tracks[id].bpm;
    return bpm ? 60000 / bpm : 0;
}

static void fake_listen(void *ctx, int on)
{
    (void)ctx;
    if (on && m.listen_at < 0 && !m.locked)
        m.listen_at = m.clock;
    m.listening = on;
}

static void fake_lock(void)
{
    if (period() && m.listen_at >= 0 && m.clock >= m.listen_at + 1000)
        m.locked = 1;
}

static void fake_state(void *ctx, int32_t out[6])
{
    int32_t p = period();
    (void)ctx;
    fake_lock();
    out[0] = m.locked;
    out[1] = p ? 60000 / p : 0;
    out[2] = p;
    out[3] = m.locked ? 90 : 40;
    out[4] = m.locked ? 100 : 50;
    out[5] = m.locked ? (m.clock - m.listen_at) / p : 0;
}

static int32_t fake_next(void *ctx, int32_t from, int32_t n, int32_t *in_bar)
{
    int32_t p = period();
    (void)ctx;
    fake_lock();
    if (!m.locked)
        return -1;
    int32_t k = from <= 0 ? 0 : (from + p - 1) / p;
    k += n;
    *in_bar = k % 4;
    return k * p;
}

static int32_t fake_downbeat(void *ctx, int32_t *margin10)
{
    (void)ctx;
    *margin10 = m.locked ? 30 : 0;
    return m.locked ? 0 : -1;
}

static int32_t fake_now(void *ctx)
{
    (void)ctx;
    return m.clock;
}

/* the bar's energy rises and falls over eight bars */
static void fake_mood(void *ctx, int32_t out[3])
{
    int32_t p = period() ? period() : 500;
    int32_t k = m.clock / (p * 4) % 8;
    (void)ctx;
    out[0] = 30 + (k < 4 ? k : 8 - k) * 15;
    out[1] = 20;
    out[2] = k < 4 ? 1 : -1;
}

/* a spectrum that leans to the low end and pulses on the beat */
static void fake_bands(void *ctx, int32_t *out, int32_t n)
{
    int32_t p = period();
    int32_t pulse = p ? 30 - (m.clock % p) * 30 / p : 0;
    (void)ctx;
    for (int32_t k = 0; k < n; k++)
        out[k] = 60 - k * 40 / n + (k == 0 ? pulse : 0);
}

/* a track's Sound Index record: its period first, then steady, A minor,
 * moderately loud; 0 for the spoken chapter, which has none */
static int sound_of(int32_t id, int32_t out[ARCADE_SOUND_FIELDS])
{
    static const int32_t rest[28] = {
        -120, 12, 70, 60, 50, 40, 30, 40, 50, 70, 40,
        10, 2, 8, 2, 6, 6, 2, 8, 2, 9, 2, 6,
        9, 1, 25, 60, 30
    };
    memset(out, 0, ARCADE_SOUND_FIELDS * sizeof(int32_t));
    if (id < 0 || id >= LIB || lib_spoken(id))
        return 0;
    out[0] = 60000 / (m.tempo ? m.tempo : lib_tracks[id].bpm);
    out[1] = 80;
    out[2] = 1;
    memcpy(out + 3, rest, 28 * sizeof(int32_t));
    return 1;
}

static int fake_sound(void *ctx, int32_t out[ARCADE_SOUND_FIELDS])
{
    (void)ctx;
    return sound_of(current(), out);
}

/* While listening, the 1.8 s ahead are heard: a kick on every beat, a
 * snare (mid) on the second and fourth, a hat (high) on the off-beats; the
 * kick that starts the bar an accent */
static int32_t fake_ahead(void *ctx, int32_t from, int32_t to, int32_t *out,
                          int32_t max)
{
    int32_t half = period() / 2, n = 0;
    (void)ctx;
    if (!m.listening || !half)
        return 0;
    if (to > m.clock + 1800)
        to = m.clock + 1800;
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
    return m.listening ? m.clock + 1800 : -1;
}

const struct arcade_beat fake_beat = {
    fake_listen, fake_state, fake_next, fake_downbeat, fake_now, fake_mood,
    fake_bands, fake_sound, fake_ahead, fake_horizon
};

/* ---- playback and tracks ------------------------------------------------- */

static int32_t fake_current(void *ctx)
{
    (void)ctx;
    return current();
}

static void fake_position(void *ctx, int32_t out[2])
{
    int32_t id = current();
    (void)ctx;
    out[0] = id < 0 ? 0 : m.clock;
    out[1] = id < 0 ? 0 : lib_length(id);
}

static int fake_field(void *ctx, int32_t id, int f, int32_t *num, char *buf,
                      int cap)
{
    (void)ctx;
    if (id < 0 || id >= LIB)
        return 0;
    const struct track *t = &lib_tracks[id];
    switch (f) {
    case ARCADE_TRACK_TITLE:
        lib_copy(buf, cap, t->title);
        return 2;
    case ARCADE_TRACK_ARTIST:
        lib_copy(buf, cap, t->artist);
        return 2;
    case ARCADE_TRACK_ALBUM:
        lib_copy(buf, cap, t->album);
        return 2;
    case ARCADE_TRACK_GENRE:
        lib_copy(buf, cap, t->genre);
        return 2;
    case ARCADE_TRACK_YEAR:
        *num = t->year;
        return 1;
    case ARCADE_TRACK_LENGTH:
        *num = lib_length(id);
        return 1;
    }
    return 0;
}

static int fake_text(void *ctx, int what, char *buf, int cap)
{
    int32_t id = current(), n;
    char t[64];
    if (id < 0 || fake_field(ctx, id, what, &n, t, sizeof(t)) != 2)
        return 0;
    return lib_copy(buf, cap, t);
}

static void fake_seek(void *ctx, int32_t ms)
{
    (void)ctx;
    say("seek %ld", ms, 0);
    if (current() >= 0) {
        m.clock = ms < 0 ? 0 : ms;
        m.jumped = 1;
    }
}

static void fake_duck(void *ctx, int32_t level, int32_t ticks)
{
    (void)ctx;
    m.duck = level;
    say("duck %ld over %ld", level, ticks);
}

static int fake_play(void *ctx, int32_t id, int32_t ms)
{
    (void)ctx;
    if (id < 0 || id >= LIB)
        return 0;
    m.owned = 1;
    m.queue[0] = id;
    m.nqueue = 1;
    m.at = 0;
    moved();
    m.clock = ms;
    m.playing = 1;
    say("play %ld from %ld", id, ms);
    return 1;
}

static void fake_stop(void *ctx)
{
    (void)ctx;
    m.playing = 0;
    say("stop", 0, 0);
}

static int fake_started(void *ctx)
{
    (void)ctx;
    return m.owned;
}

static int fake_matches(int32_t id, const struct arcade_filter *f)
{
    const struct track *t = &lib_tracks[id];
    if (!f)
        return 1;
    if (f->music && lib_spoken(id))
        return 0;
    if (f->minlen && lib_length(id) < f->minlen)
        return 0;
    if (f->maxlen && lib_length(id) > f->maxlen)
        return 0;
    if (f->genre && strcmp(f->genre, t->genre))
        return 0;
    if (f->artist && strcmp(f->artist, t->artist))
        return 0;
    if (f->decade && t->year / 10 * 10 != f->decade)
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

/* the first n that match, in order: a test's "random" */
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

/* the same genre, nearest in the library first; by sound, the nearest
 * tempo first, the earlier of two as near */
static int32_t fake_near_one(int32_t id, int32_t n, int sound,
                             int32_t *out)
{
    int32_t k = 0;
    if (id < 0 || id >= LIB)
        return 0;
    if (sound) {
        if (lib_spoken(id))
            return 0;
        for (int32_t gap = 0; gap < 200 && k < n; gap++)
            for (int32_t j = 0; j < LIB && k < n; j++)
                if (j != id && !lib_spoken(j) &&
                    abs(lib_tracks[j].bpm - lib_tracks[id].bpm) == gap)
                    out[k++] = j;
        return k;
    }
    for (int32_t d = 1; d < LIB && k < n; d++)
        for (int s = -1; s <= 1 && k < n; s += 2) {
            int32_t j = id + s * d;
            if (j >= 0 && j < LIB &&
                !strcmp(lib_tracks[j].genre, lib_tracks[id].genre))
                out[k++] = j;
        }
    return k;
}

/* each of ids in turn, n a track, -1 after the last of each */
static int32_t fake_near(void *ctx, const int32_t *ids, int32_t count,
                         int32_t n, int sound, int32_t *out)
{
    int32_t found = 0;
    (void)ctx;
    for (int32_t t = 0; t < count; t++) {
        int32_t k = fake_near_one(ids[t], n, sound, out + t * n);
        for (int32_t i = k; i < n; i++)
            out[t * n + i] = -1;
        found += k;
    }
    return found;
}

static int fake_track_sound(void *ctx, int32_t id,
                            int32_t out[ARCADE_SOUND_FIELDS])
{
    (void)ctx;
    return sound_of(id, out);
}

static int32_t fake_queued(void *ctx, int32_t *at)
{
    (void)ctx;
    *at = m.at;
    return m.nqueue;
}

static int32_t fake_queue_at(void *ctx, int32_t i)
{
    (void)ctx;
    return i >= 0 && i < m.nqueue ? m.queue[i] : -1;
}

static int fake_enqueue(void *ctx, int32_t id, int next)
{
    (void)ctx;
    if (id < 0 || id >= LIB || m.nqueue >= LIB)
        return 0;
    int32_t at = next ? m.at + 1 : m.nqueue;
    memmove(m.queue + at + 1, m.queue + at,
            (size_t)(m.nqueue - at) * sizeof(int32_t));
    m.queue[at] = id;
    m.nqueue++;
    return 1;
}

/* every walk looks at the whole library */
static int32_t fake_walked(void *ctx)
{
    (void)ctx;
    return LIB;
}

/* Each album's cover colours, as dynamic colours would take them: the
 * background, the text and the most vivid; the theme's while the music is
 * stopped */
static const uint32_t album_colours[6][3] = {
    { 0x14283c, 0xe8f0f8, 0x3cc8a0 },   /* Northern Lights */
    { 0x1c2440, 0xd8c8a0, 0x4878d0 },   /* Blue Hours */
    { 0xffa4bc, 0x301f17, 0xe0306a },   /* Paper Planes */
    { 0x2a1a12, 0xf0c890, 0xe06020 },   /* Engine Room */
    { 0xe8ece8, 0x203030, 0x30a0b0 },   /* Quiet Machines */
    { 0xf4ecd8, 0x3a2e20, 0xc08030 },   /* A Short History of Lighthouses */
};

static int fake_colours(void *ctx, uint32_t out[3])
{
    (void)ctx;
    int32_t id = current();
    if (!m.playing || id < 0) {
        out[0] = 0x000000;
        out[1] = out[2] = 0xffffff;
        return 0;
    }
    memcpy(out, album_colours[id < 16 ? id / 4 : id < 19 ? 4 : 5],
           3 * sizeof(uint32_t));
    return 1;
}

/* The volume, 0 to 100, two a step */
static int32_t fake_vol = 60;

static int32_t fake_volume(void *ctx, int32_t steps)
{
    (void)ctx;
    fake_vol += steps * 2;
    fake_vol = fake_vol < 0 ? 0 : fake_vol > 100 ? 100 : fake_vol;
    return fake_vol;
}

const struct arcade_tracks fake_tracks = {
    fake_current, fake_position, fake_text, fake_seek, fake_duck,
    fake_play, fake_stop, fake_started, fake_count, fake_pick, fake_field,
    fake_near, fake_track_sound, fake_queued, fake_queue_at, fake_enqueue,
    fake_walked, fake_colours, fake_volume
};
