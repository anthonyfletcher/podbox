/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The owner's music and library for a cart: struct arcade_tracks, which the
 * engine's playback and tracks calls go through. A track is its database
 * id, the entry number tagcache gives it; the playing track's is found by
 * its path, once a track.
 *
 * The library calls walk the database, which can take a while on a big one,
 * so a cart makes them while it sets up rather than every tick; a walk
 * yields as it goes, so the music keeps playing. A cart that owns the
 * music (playback.play) plays from a playlist of its own, the owner's set
 * aside the way the Music Quiz sets it aside, and brought back when the
 * cart ends; nothing it plays counts as a listen.
 *
 * Parts, in order:
 *   - tracks and their ids
 *   - the playing track, seeking and ducking
 *   - owning the music
 *   - the library: count, pick, field, near, sound
 *   - the queue, the album's colours, and the volume
 *   - a cart starting, ticking and ending
 ****************************************************************************/
#include <stdbool.h>
#include <stdlib.h>                 /* rand */
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"                 /* yield */
#include "audio.h"
#include "metadata.h"               /* struct mp3entry */
#include "pcm_mixer.h"
#include "string-extra.h"           /* strmemccpy */
#include "audio/playback.h"         /* audio_set_unrecorded */
#include "playlist/playlist.h"
#include "database/tagcache.h"
#include "database/path_key.h"
#include "database/sound_index.h"
#include "database/sound_mix.h"
#include "metadata/book_resume.h"
#include "settings/settings.h"         /* global_settings */
#include "skin/skin_albumart_color.h"
#include "sound.h"                     /* sound_min, sound_max */
#include "system/volume.h"             /* adjust_volume */
#include "games/arcade/shell.h"            /* shell_walk_yield */
#include "games/arcade/shell_tracks.h"

/* ---- tracks and their ids ------------------------------------------------ */

static int32_t id_of_path(const char *path)
{
    struct tagcache_search tcs;
    if (!path[0] || !tagcache_find_index(&tcs, path))
        return -1;
    int32_t id = tcs.idx_id;
    tagcache_search_finish(&tcs);
    return id;
}

static bool path_of_id(int32_t id, char *path, size_t size)
{
    struct tagcache_search tcs;
    bool ok;
    if (id < 0 || !tagcache_search(&tcs, tag_filename))
        return false;
    ok = tagcache_retrieve(&tcs, id, tag_filename, path, size);
    tagcache_search_finish(&tcs);
    return ok;
}

static int copy_out(char *buf, int cap, const char *s)
{
    if (!s || cap < 1)
        return 0;
    strmemccpy(buf, s, (size_t)cap);
    return (int)strlen(buf);
}

/* ---- the playing track, seeking and ducking ------------------------------ */

/* The playing track's path and id, found again when the path changes */
static char playing_path[MAX_PATH];
static int32_t playing_id = -1;

static int32_t music_current(void *ctx)
{
    (void)ctx;
    struct mp3entry *id3 = audio_current_track();
    const char *path = id3 ? id3->path : "";
    if (strcmp(path, playing_path)) {
        strmemccpy(playing_path, path, sizeof(playing_path));
        playing_id = id_of_path(playing_path);
    }
    return playing_id;
}

static void music_position(void *ctx, int32_t out[2])
{
    (void)ctx;
    struct mp3entry *id3 = audio_current_track();
    out[0] = id3 ? (int32_t)id3->elapsed : 0;
    out[1] = id3 ? (int32_t)id3->length : 0;
}

static int music_text(void *ctx, int what, char *buf, int cap)
{
    (void)ctx;
    struct mp3entry *id3 = audio_current_track();
    if (!id3)
        return 0;
    /* copied at once: the entry is live, and can change under a yield */
    return copy_out(buf, cap, what == 0 ? id3->title : what == 1 ?
                    id3->artist : id3->album);
}

/* A seek asked for, made at the next tick: each is a message to the audio
 * thread, whose queue a cart seeking in a loop would overrun */
static int32_t seek_ms = -1;

static void music_seek(void *ctx, int32_t ms)
{
    (void)ctx;
    seek_ms = ms < 0 ? 0 : ms;
}

/* The playback channel's level, 16.16 of the owner's, and where a duck
 * takes it, a step a tick */
static int32_t duck_now = MIX_AMP_UNITY, duck_to = MIX_AMP_UNITY, duck_step;

static void music_duck(void *ctx, int32_t level, int32_t ticks)
{
    (void)ctx;
    duck_to = (int32_t)(((int64_t)MIX_AMP_UNITY * level) >> 8);
    int32_t d = duck_to - duck_now;
    duck_step = ticks > 0 ? (d < 0 ? -d : d) / ticks + 1 : MIX_AMP_UNITY;
}

static void duck_tick(void)
{
    if (duck_now == duck_to)
        return;
    if (duck_now < duck_to)
        duck_now = duck_to - duck_now <= duck_step ? duck_to
                                                   : duck_now + duck_step;
    else
        duck_now = duck_now - duck_to <= duck_step ? duck_to
                                                   : duck_now - duck_step;
    mixer_channel_set_amplitude(PCM_MIXER_CHAN_PLAYBACK, (unsigned)duck_now);
}

/* ---- owning the music ---------------------------------------------------- */

/* Whether the owner's playlist is set aside, and the track asked for */
static bool owned;
static char wanted_path[MAX_PATH];
static int32_t wanted_ms;

bool shell_tracks_owned(void)
{
    return owned;
}

/* The owner's music stopped: a book's place written down first, as every
 * other stop does */
static void stop_owners(void)
{
    if (!owned)
        book_resume_save();
    audio_stop();
}

static int music_play(void *ctx, int32_t id, int32_t ms)
{
    (void)ctx;
    char path[MAX_PATH];
    if (!path_of_id(id, path, sizeof(path)))
        return 0;
    /* owned from before the stop, which waits on the audio thread: a dock
     * leaves the queue alone (iap_library_queue_lent()) from here */
    bool was = owned;
    owned = true;
    if (!was)
        book_resume_save();
    audio_stop();
    if (!was) {
        playlist_set_aside();
        audio_set_unrecorded(true);
    }
    if (playlist_create(NULL, NULL) < 0 ||
        playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false,
                              true) < 0)
        return 0;
    strmemccpy(wanted_path, path, sizeof(wanted_path));
    wanted_ms = ms;
    playlist_start(0, (unsigned long)ms, 0);
    return 1;
}

static void music_stop(void *ctx)
{
    (void)ctx;
    stop_owners();
}

/* Heard: playing, the track asked for, and past where it was asked from */
static int music_started(void *ctx)
{
    (void)ctx;
    struct mp3entry *id3 = audio_current_track();
    return (audio_status() & AUDIO_STATUS_PLAY) && id3 &&
           !strcmp(id3->path, wanted_path) &&
           (int32_t)id3->elapsed > wanted_ms;
}

/* ---- the library: count, pick, field, near, sound ------------------------ */

/* Music only, for a filter that leaves spoken word out. tagcache keeps the
 * pointer rather than a copy. */
static struct tagcache_search_clause music_clause = {
    .tag = tag_virt_spoken,
    .type = clause_is,
    .numeric = true,
    .source = source_constant,
    .numeric_data = 0,
    .str = NULL,
};

/* A walk over the library's tracks, the filter's spoken word left out */
static bool walk_begin(struct tagcache_search *tcs,
                       const struct arcade_filter *f)
{
    if (!tagcache_search(tcs, tag_title))
        return false;
    if (f && f->music)
        tagcache_search_add_clause(tcs, &music_clause);
    return true;
}

/* Whether the walk's track passes the filter's other fields */
static bool walk_passes(struct tagcache_search *tcs,
                        const struct arcade_filter *f)
{
    if (!f)
        return true;
    long len = tagcache_get_numeric(tcs, tag_length);
    if ((f->minlen && len < f->minlen) || (f->maxlen && len > f->maxlen))
        return false;
    if (f->decade && tagcache_get_numeric(tcs, tag_year) / 10 * 10 !=
                     f->decade)
        return false;
    if (f->genre) {
        char genre[64];
        if (!tagcache_entry_string(tcs->idx_id, tag_genre, genre,
                                   sizeof(genre)) ||
            strcasecmp(genre, f->genre))
            return false;
    }
    if (f->artist) {
        char artist[64];
        if (!tagcache_entry_string(tcs->idx_id, tag_artist, artist,
                                   sizeof(artist)) ||
            strcasecmp(artist, f->artist))
            return false;
    }
    return true;
}

/* the tracks the last walk looked at */
static int32_t walked;

static int32_t music_walked(void *ctx)
{
    (void)ctx;
    return walked;
}

static int32_t music_count(void *ctx, const struct arcade_filter *f)
{
    (void)ctx;
    struct tagcache_search tcs;
    char title[8];
    int32_t n = 0, seen = 0;
    walked = 0;
    if (!walk_begin(&tcs, f))
        return 0;
    while (tagcache_get_next(&tcs, title, sizeof(title))) {
        if (walk_passes(&tcs, f))
            n++;
        if ((++seen & 63) == 0)
            shell_walk_yield();
    }
    tagcache_search_finish(&tcs);
    walked = seen;
    return n;
}

/* n of those that pass, each as likely as any other (a reservoir) */
static int32_t music_pick(void *ctx, int32_t n, const struct arcade_filter *f,
                          int32_t *out)
{
    (void)ctx;
    struct tagcache_search tcs;
    char title[8];
    int32_t found = 0, seen = 0;
    walked = 0;
    if (n <= 0 || !walk_begin(&tcs, f))
        return 0;
    while (tagcache_get_next(&tcs, title, sizeof(title))) {
        if (walk_passes(&tcs, f)) {
            int32_t slot = found < n ? found : rand() % (found + 1);
            if (slot < n)
                out[slot] = tcs.idx_id;
            found++;
        }
        if ((++seen & 63) == 0)
            shell_walk_yield();
    }
    tagcache_search_finish(&tcs);
    walked = seen;
    return found < n ? found : n;
}

static int music_field(void *ctx, int32_t id, int f, int32_t *num, char *buf,
                       int cap)
{
    (void)ctx;
    static const int strings[] = { tag_title, tag_artist, tag_album,
                                   tag_genre };
    long v;
    if (id < 0)
        return 0;
    if (f >= ARCADE_TRACK_TITLE && f <= ARCADE_TRACK_GENRE)
        return tagcache_entry_string(id, strings[f], buf, (size_t)cap) ? 2
                                                                        : 0;
    if (f == ARCADE_TRACK_YEAR || f == ARCADE_TRACK_LENGTH) {
        if (!tagcache_entry_numeric(id, f == ARCADE_TRACK_YEAR ? tag_year
                                                               : tag_length,
                                    &v))
            return 0;
        *num = (int32_t)v;
        return 1;
    }
    return 0;
}

/* The most near() hands over for a track, and the most tracks at once */
#define NEAR_MAX ARCADE_NEAR_MAX
#define NEAR_IDS ARCADE_NEAR_IDS

/* Closer than this by sound, and this near in length, two records are one
 * recording encoded twice: sound_mix.c's MIX_SEED_EPSILON */
#define SAME_RECORDING_D  15
#define SAME_LENGTH_MS    2000

/* By sound: for each track asked about, the nearest records in the Sound
 * Index, nearest first, and the track and length each turns out to be */
struct near_entry { uint64_t key; int d; int32_t id; long len; };
static struct near_entry near_tab[NEAR_IDS][NEAR_MAX];
static int near_ct[NEAR_IDS];

static void keep_nearest(int t, uint64_t key, int d)
{
    struct near_entry *e = near_tab[t];
    int i;
    if (near_ct[t] == NEAR_MAX && d >= e[NEAR_MAX - 1].d)
        return;
    i = near_ct[t] < NEAR_MAX ? near_ct[t]++ : NEAR_MAX - 1;
    for (; i > 0 && e[i - 1].d > d; i--)
        e[i] = e[i - 1];
    e[i].key = key;
    e[i].d = d;
    e[i].id = -1;
}

/* Every kept entry, in key order, for the walk below to look a path up in */
static struct near_entry *by_key[NEAR_IDS * NEAR_MAX];
static int by_key_ct;

static int compare_key(const void *a, const void *b)
{
    uint64_t ka = (*(struct near_entry *const *)a)->key;
    uint64_t kb = (*(struct near_entry *const *)b)->key;
    return ka < kb ? -1 : ka > kb;
}

/* Each kept key's track, music only: from the path index while the
 * database is in RAM, else by one walk of it, since a record has no path */
static void resolve_nearest(int count)
{
    struct tagcache_search tcs;
    char path[MAX_PATH];
    long spoken;
    int32_t seen = 0;
    if (tagcache_is_in_ram()) {
        for (int t = 0; t < count; t++)
            for (int i = 0; i < near_ct[t]; i++) {
                struct near_entry *e = &near_tab[t][i];
                int id = tagcache_find_key(e->key);
                if (id >= 0 &&
                    tagcache_entry_numeric(id, tag_virt_spoken, &spoken) &&
                    !spoken && tagcache_entry_numeric(id, tag_length, &e->len))
                    e->id = id;
            }
        return;
    }
    by_key_ct = 0;
    for (int t = 0; t < count; t++)
        for (int i = 0; i < near_ct[t]; i++)
            by_key[by_key_ct++] = &near_tab[t][i];
    qsort(by_key, by_key_ct, sizeof(by_key[0]), compare_key);
    if (by_key_ct == 0 || !tagcache_search(&tcs, tag_filename))
        return;
    tagcache_search_add_clause(&tcs, &music_clause);
    while (tagcache_get_next(&tcs, path, sizeof(path))) {
        uint64_t key = path_key(path);
        int lo = 0, hi = by_key_ct;
        if ((++seen & 15) == 0)
            shell_walk_yield();
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (by_key[mid]->key < key)
                lo = mid + 1;
            else
                hi = mid;
        }
        /* two tracks asked about can share a neighbour */
        for (; lo < by_key_ct && by_key[lo]->key == key; lo++) {
            by_key[lo]->id = tcs.idx_id;
            by_key[lo]->len = tagcache_get_numeric(&tcs, tag_length);
        }
    }
    tagcache_search_finish(&tcs);
    walked += seen;
}

/* For each of ids, the tracks that sound most like it, nearest first, by
 * Play Similar's distance with its genre and era terms off; a second copy
 * of its own recording left out. One walk of the index answers them all.
 * out holds n for each in turn, -1 after the last; none for a track the
 * index has not measured. The count found in all. */
static int32_t near_by_sound(const int32_t *ids, int count, int32_t n,
                             int32_t *out)
{
    static struct sound_axes self[NEAR_IDS];
    static uint64_t self_key[NEAR_IDS];
    struct sound_index_reader r;
    struct sound_record rec;
    struct sound_axes ax;
    char path[MAX_PATH];
    int32_t found = 0;
    int measured = 0;
    for (int t = 0; t < count * n; t++)
        out[t] = -1;
    if (sound_index_reader_open(&r) != SOUND_OK)
        return 0;
    for (int t = 0; t < count; t++) {
        uint64_t key = ids[t] >= 0 ? tagcache_entry_key(ids[t]) : 0;
        if (!key && path_of_id(ids[t], path, sizeof(path)))
            key = path_key(path);
        near_ct[t] = 0;
        self_key[t] = 0;
        if (key && sound_index_find(&r, key, &rec) &&
            sound_record_usable(&rec)) {
            sound_mix_axes(&rec, &self[t]);
            self[t].genre = self[t].year = 0;
            self_key[t] = key;
            measured++;
        }
    }
    for (int i = 0; measured && i < r.count; i++) {
        if ((i & 63) == 0)
            shell_walk_yield();
        if (!sound_index_read(&r, i, &rec) || !sound_record_usable(&rec))
            continue;
        sound_mix_axes(&rec, &ax);
        ax.genre = ax.year = 0;
        for (int t = 0; t < count; t++)
            if (self_key[t] && rec.key != self_key[t])
                keep_nearest(t, rec.key, sound_mix_distance(&self[t], &ax));
    }
    walked = measured ? r.count : 0;
    sound_index_reader_close(&r);
    resolve_nearest(count);
    for (int t = 0; t < count; t++) {
        long len = 0;
        int32_t k = 0;
        tagcache_entry_numeric(ids[t], tag_length, &len);
        for (int i = 0; i < near_ct[t] && k < n; i++) {
            const struct near_entry *e = &near_tab[t][i];
            long gap = e->len - len;
            if (e->id < 0 || e->id == ids[t] ||
                (e->d < SAME_RECORDING_D &&
                 gap > -SAME_LENGTH_MS && gap < SAME_LENGTH_MS))
                continue;
            out[t * n + k++] = e->id;
        }
        found += k;
    }
    return found;
}

/* The tracks most like id: the same genre and decade, then the same genre,
 * then the same decade, each tier taken at random (a reservoir a tier);
 * music only, id itself left out */
static int32_t near_by_tags(int32_t id, int32_t n, int32_t *out)
{
    static int32_t tier[3][NEAR_MAX];
    struct tagcache_search tcs;
    char genre[64], g[64], title[8];
    long year = 0;
    int32_t found[3] = { 0, 0, 0 }, seen = 0, k = 0;
    struct arcade_filter music = { 0, 0, NULL, 0, 1, NULL };
    if (id < 0 || !walk_begin(&tcs, &music))
        return 0;
    bool has_genre = tagcache_entry_string(id, tag_genre, genre,
                                           sizeof(genre));
    tagcache_entry_numeric(id, tag_year, &year);
    while (tagcache_get_next(&tcs, title, sizeof(title))) {
        if ((++seen & 63) == 0)
            shell_walk_yield();
        if (tcs.idx_id == id)
            continue;
        bool same_genre = has_genre &&
            tagcache_entry_string(tcs.idx_id, tag_genre, g, sizeof(g)) &&
            !strcasecmp(g, genre);
        bool same_decade = year > 0 &&
            tagcache_get_numeric(&tcs, tag_year) / 10 == year / 10;
        int t = same_genre && same_decade ? 0 : same_genre ? 1 :
                same_decade ? 2 : -1;
        if (t < 0)
            continue;
        int32_t slot = found[t] < n ? found[t] : rand() % (found[t] + 1);
        if (slot < n)
            tier[t][slot] = tcs.idx_id;
        found[t]++;
    }
    tagcache_search_finish(&tcs);
    walked += seen;
    for (int t = 0; t < 3; t++)
        for (int32_t i = 0; i < found[t] && i < n && k < n; i++)
            out[k++] = tier[t][i];
    return k;
}

/* By tags, a walk of the library for each track; by sound, one walk of
 * the Sound Index for them all */
static int32_t music_near(void *ctx, const int32_t *ids, int32_t count,
                          int32_t n, int sound, int32_t *out)
{
    (void)ctx;
    int32_t found = 0;
    walked = 0;
    if (n <= 0 || count <= 0 || n > NEAR_MAX || count > NEAR_IDS)
        return 0;
    if (sound)
        return near_by_sound(ids, count, n, out);
    for (int32_t t = 0; t < count; t++) {
        int32_t k = near_by_tags(ids[t], n, out + t * n);
        for (int32_t i = k; i < n; i++)
            out[t * n + i] = -1;
        found += k;
    }
    return found;
}


static int music_sound(void *ctx, int32_t id, int32_t out[ARCADE_SOUND_FIELDS])
{
    (void)ctx;
    struct sound_index_reader r;
    struct sound_record rec;
    char path[MAX_PATH];
    uint64_t key = id >= 0 ? tagcache_entry_key(id) : 0;
    bool found = false;
    if (!key && path_of_id(id, path, sizeof(path)))
        key = path_key(path);
    if (!key || sound_index_reader_open(&r) != SOUND_OK)
        return 0;
    found = sound_index_find(&r, key, &rec) && sound_record_usable(&rec);
    sound_index_reader_close(&r);
    if (!found)
        return 0;
    shell_sound_out(&rec, out);
    return 1;
}

void shell_sound_out(const struct sound_record *s,
                     int32_t out[ARCADE_SOUND_FIELDS])
{
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
}

/* ---- the queue ----------------------------------------------------------- */

static int32_t music_queued(void *ctx, int32_t *at)
{
    (void)ctx;
    int32_t n = playlist_amount();
    *at = n > 0 ? playlist_get_display_index() - 1 : -1;
    return n;
}

/* Place i in the order the owner sees the queue in */
static int32_t music_queue_at(void *ctx, int32_t i)
{
    (void)ctx;
    struct playlist_track_info info;
    int32_t n = playlist_amount();
    if (i < 0 || i >= n)
        return -1;
    int32_t index = (i + playlist_get_first_index(NULL)) % n;
    if (playlist_get_track_info(NULL, index, &info) < 0)
        return -1;
    return id_of_path(info.filename);
}

static int music_enqueue(void *ctx, int32_t id, int next)
{
    (void)ctx;
    char path[MAX_PATH];
    if (!path_of_id(id, path, sizeof(path)))
        return 0;
    return playlist_insert_track(NULL, path, next ? PLAYLIST_INSERT_FIRST
                                                  : PLAYLIST_INSERT_LAST,
                                 false, true) >= 0;
}

/* What dynamic colours makes of the playing album: the theme's background
 * and text through the resolver, and the vivid colour, which without an
 * album palette is the text's */
static uint32_t rgb_of(unsigned c)
{
    return (uint32_t)RGB_UNPACK_RED(c) << 16 |
           (uint32_t)RGB_UNPACK_GREEN(c) << 8 | (uint32_t)RGB_UNPACK_BLUE(c);
}

static int music_colours(void *ctx, uint32_t out[3])
{
    (void)ctx;
    unsigned dominant, vivid, ink;
    dynamic_colors_check_extraction(-1);
    ink = dynamic_colors_resolve(global_settings.fg_color);
    out[0] = rgb_of(dynamic_colors_resolve(global_settings.bg_color));
    out[1] = rgb_of(ink);
    if (!dynamic_colors_roles(&dominant, &vivid)) {
        out[2] = out[1];
        return 0;
    }
    out[2] = rgb_of(vivid);
    return 1;
}

/* The volume a step at a time, in the owner's own adjust mode, as the
 * wheel turns it on the player's screens */
static int32_t music_volume(void *ctx, int32_t steps)
{
    (void)ctx;
    int lo = sound_min(SOUND_VOLUME), hi = sound_max(SOUND_VOLUME);
    if (steps)
        adjust_volume((int)steps);
    if (hi <= lo)
        return 0;
    return (int32_t)((global_status.volume - lo) * 100 / (hi - lo));
}

const struct arcade_tracks shell_tracks = {
    music_current, music_position, music_text, music_seek, music_duck,
    music_play, music_stop, music_started, music_count, music_pick,
    music_field, music_near, music_sound, music_queued, music_queue_at,
    music_enqueue, music_walked, music_colours, music_volume
};

/* ---- a cart starting, ticking and ending --------------------------------- */

static char ticked_path[MAX_PATH];

/* the cart's own music, paused by the pause */
static bool held;

void shell_tracks_start(void)
{
    struct mp3entry *id3 = audio_current_track();
    strmemccpy(ticked_path, id3 ? id3->path : "", sizeof(ticked_path));
    playing_path[0] = '\0';
    playing_id = -1;
    duck_now = duck_to = MIX_AMP_UNITY;
    owned = false;
    held = false;
    seek_ms = -1;
}

int shell_tracks_tick(void)
{
    struct mp3entry *id3 = audio_current_track();
    const char *path = id3 ? id3->path : "";
    int changed = strcmp(path, ticked_path) != 0;
    if (changed)
        strmemccpy(ticked_path, path, sizeof(ticked_path));
    if (seek_ms >= 0 && (audio_status() & AUDIO_STATUS_PLAY))
        audio_ff_rewind(seek_ms);
    seek_ms = -1;
    duck_tick();
    return changed;
}

bool shell_tracks_held(void)
{
    return held;
}

void shell_tracks_hold(bool hold)
{
    int status = audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE);
    if (hold && owned && status == AUDIO_STATUS_PLAY) {
        audio_pause();
        held = true;
    } else if (!hold && held) {
        held = false;
        if (status & AUDIO_STATUS_PAUSE)
            audio_resume();
    }
}

void shell_tracks_end(void)
{
    if (duck_now != MIX_AMP_UNITY) {
        duck_now = duck_to = MIX_AMP_UNITY;
        mixer_channel_set_amplitude(PCM_MIXER_CHAN_PLAYBACK, MIX_AMP_UNITY);
    }
    if (owned) {
        audio_stop();
        audio_set_unrecorded(false);
        playlist_bring_back();
        owned = false;
    }
}
