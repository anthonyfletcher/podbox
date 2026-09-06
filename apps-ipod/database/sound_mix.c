/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Playlists built from how tracks sound.
 *
 * Four kinds of playlist come out of here -- from a track, from a mood, a
 * journey between two moods, and the continuation of any of them -- and all
 * four are one machine. What differs is only what a candidate is scored
 * against, which is the goal passed to mix_build().
 *
 * The index says what every track sounds like but not where any of them are:
 * a record is keyed by a hash of its path and carries no path. So building one
 * is two sequential passes. The first reads the index and keeps the nearest
 * keys; the second walks the database and turns those keys back into
 * filenames by hashing each path it passes.
 *
 * Neither pass allocates. Holding the index in memory would be simpler and is
 * the wrong trade: 220K for a library this size comes out of the audio buffer,
 * and taking that stops playback and rebuffers the track. A feature reached
 * from the playing screen must not stop the music in order to answer.
 *
 * The choosing sits between the two, because a track's artist and length are
 * only visible in the second: the first pass keeps more candidates than the
 * playlist needs, and the rules about who may appear and how often are applied
 * once the walk has said who they are.
 *
 * Parts, in order:
 *   - the axes, and what they are worth against each other
 *   - distance between two tracks
 *   - the goal, the two passes and the choosing
 *   - continuing a playlist that has run out
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "settings/settings.h"
#include "timefuncs.h"
#include "database/sound_index.h"
#include "database/sound_mix.h"
#include "database/sound_mood.h"
#include "database/tagcache.h"
#include "playlist/playlist.h"
#include "system/app_util.h"

#define AX  SOUND_AX

static int nrm(int v, int lo, int hi)
{
    if (hi == lo)
        return 0;
    if (v <= lo)
        return 0;
    if (v >= hi)
        return AX;

    return (v - lo) * AX / (hi - lo);
}

/* Into the range a listener would tap. The record stores the tracker's own
 * reading, and doubling and halving are its commonest errors -- so a reader
 * comparing tempi folds, or it compares 200 against 100 and calls them
 * opposites. */
static int fold_bpm(int bpm)
{
    if (bpm <= 0)
        return 0;

    while (bpm > 140)
        bpm /= 2;
    while (bpm < 70)
        bpm *= 2;

    return bpm;
}

void sound_mix_axes(const struct sound_record *r, struct sound_axes *out)
{
    int bpm;

    memset(out, 0, sizeof (*out));

    out->loud    = nrm(r->loudness_db10, -300, -60);
    out->dens    = nrm(r->rate10[0] + r->rate10[1] + r->rate10[2], 0, 90);
    out->bright  = nrm(r->level[2], 0, 40);
    out->low     = nrm(r->level[0], 0, 80);
    out->mid     = nrm(r->level[1], 0, 75);
    out->crest   = AX - nrm(r->crest_db, 8, 20);
    out->width   = nrm(r->width, 0, 100);
    out->peak    = nrm(r->peakiness, 5, 60);
    out->clarity = nrm(r->tonal_clarity, 110, 230);
    out->change  = nrm(r->harmonic_change, 30, 90);

    /* Tempo only where the tracker held still for it. Measured across 3400
     * tracks, 94% of locked readings sit inside 10ms of spread; the rest are
     * tracks it never settled on, and a number taken from one of those is
     * worse than no number at all. */
    bpm = r->period_ms ? fold_bpm(60000 / r->period_ms) : 0;
    out->tempo = (bpm > 0 && r->tempo_spread <= 10)
                 ? nrm(bpm, 70, 140) : -1;

    /* Available on about three fifths of a real library. Absent is not the
     * same as neutral, so it is marked rather than defaulted. */
    out->mode = r->mode_margin >= CHROMA_MARGIN_MIN ? r->mode : -1;

    out->genre = r->genre_key;
    out->year  = (r->year > 1900 && r->year < 2100) ? r->year : 0;

    /* Loudness and density carry most of what a listener calls energy; tempo
     * adds least, because density has already said how much is happening.
     * A track with no trusted tempo takes the middle rather than zero, so the
     * absence does not read as "calm". */
    out->energy = (30 * out->loud + 28 * out->dens + 18 * out->bright
                   + 14 * (out->tempo >= 0 ? out->tempo : AX / 2)
                   + 10 * out->crest) / 100;
}


/** Distance **/

/* What holds a mix together, in the order it matters.
 *
 * Level and the balance between the bands come first, because that is what
 * separates records the tempo cannot: two tracks at 120 BPM can be a folk
 * ballad and a techno record, and only the band balance says which.
 *
 * Weights are in tenths so they can be integers. */
static const struct { size_t off; int w; } mix_weights[] = {
    { offsetof(struct sound_axes, loud),    10 },
    { offsetof(struct sound_axes, bright),  10 },
    { offsetof(struct sound_axes, low),      8 },
    { offsetof(struct sound_axes, dens),     8 },
    { offsetof(struct sound_axes, tempo),    7 },
    { offsetof(struct sound_axes, peak),     6 },
    { offsetof(struct sound_axes, mid),      5 },
    { offsetof(struct sound_axes, clarity),  5 },
    { offsetof(struct sound_axes, change),   4 },
    { offsetof(struct sound_axes, crest),    4 },
    { offsetof(struct sound_axes, width),    3 },
};

#define MIX_AXES (sizeof (mix_weights) / sizeof (mix_weights[0]))

static uint32_t mix_root(uint32_t v)
{
    uint32_t r = 0;
    uint32_t bit = 1UL << 30;

    while (bit > v)
        bit >>= 2;

    while (bit != 0)
    {
        if (v >= r + bit)
        {
            v -= r + bit;
            r = (r >> 1) + bit;
        }
        else
        {
            r >>= 1;
        }
        bit >>= 2;
    }

    return r;
}

int sound_mix_distance(const struct sound_axes *a, const struct sound_axes *b)
{
    uint32_t sum = 0;
    int total_w = 0;
    int d;
    unsigned int i;

    for (i = 0; i < MIX_AXES; i++)
    {
        int x = *(const int *)((const char *)a + mix_weights[i].off);
        int y = *(const int *)((const char *)b + mix_weights[i].off);

        /* An axis missing on either side is skipped rather than guessed, and
         * the divisor drops with it -- otherwise a track with no tempo would
         * read as closer to everything than one that has a different tempo. */
        if (x < 0 || y < 0)
            continue;

        d = x - y;
        sum += (uint32_t)(d * d / AX) * mix_weights[i].w;
        total_w += mix_weights[i].w;
    }

    if (total_w == 0)
        return AX;

    d = (int)mix_root(sum * AX / total_w);

    /* A mode disagreement is a real difference between two tracks that both
     * committed to one. An abstention is not evidence of anything, so it
     * costs nothing. */
    if (a->mode >= 0 && b->mode >= 0 && a->mode != b->mode)
        d += 60;

    /* Soft, both of them. A hard genre filter would make this a genre
     * browser, which the database already does better. */
    if (a->genre != 0 && a->genre == b->genre)
        d -= 50;

    if (a->year && b->year)
    {
        int gap = a->year > b->year ? a->year - b->year : b->year - a->year;

        d += 30 * (gap > 25 ? 25 : gap) / 25;
    }

    return d < 0 ? 0 : d;
}


/** Building one **/

/* Candidates carried out of the first pass, so the rules below have something
 * to fall back on: the nearest tracks to any goal are mostly one or two
 * albums, and every one the artist rules turn down has to be replaced by the
 * next nearest. */
#define MIX_CAND       (SOUND_MIX_MAX * 3)

/* Tracks one artist may contribute, and how many must separate two of them.
 * The cap stops a mix being a reshuffle of one album; the gap stops the two
 * it does allow arriving as a pair. */
#define MIX_PER_ARTIST 2
#define MIX_ARTIST_GAP 3

/* How many of the eligible candidates a varying mix chooses between at each
 * step. Small on purpose: the list has to stay a list of near tracks, so the
 * variation comes from picking among neighbours rather than from reaching
 * further out. */
#define MIX_VARY_CHOICE 3

/* Tracks shorter than this are not offered. They are intros, interludes and
 * segues -- they measure as real tracks and arrive as real matches, and a
 * playlist of them is not what anybody asked for. Read from the database
 * rather than the index, which does not store a length. */
#define MIX_MIN_LENGTH_MS 90000

/* What built the playlist now playing. A continuation carries on in the same
 * terms rather than seeding from whatever happened to play last: a mood has a
 * fixed target, so it stays where it was aimed, where a drifting seed wanders
 * off over successive extensions.
 *
 * Forgotten whenever a playlist is created by anything else -- that is the
 * moment the old one stops existing, and stale terms would then be applied to
 * somebody's album. */
static struct mix_goal   remembered;
static struct sound_axes remembered_axes;
static bool              have_remembered;

/* Set from the audio thread when a playlist runs out, read and cleared on the
 * UI thread. A bool written from one side and cleared on the other needs no
 * more protection than that: a lost race costs one continuation. */
static volatile bool     continue_due;

/* How much of the playlist a continuation refuses to repeat. Bounded because
 * this is static and a dynamic playlist has no bound: something heard three
 * hundred tracks ago coming round again is not the complaint, something from
 * twenty minutes ago is. */
#define MIX_EXCLUDE 256

/* What the candidates are being judged against.
 *
 * One shape serves all three kinds of playlist. A track mix scores against
 * the seed; a mood scores against a point in the axis space; a journey scores
 * against both of its moods, blending from one to the other across the run.
 * Everything after this point is the same machinery either way. */
struct mix_goal
{
    const struct sound_axes *seed;   /* NULL unless built from a track */
    int mood_from;
    int mood_to;                     /* == mood_from unless a journey */
    int steps;                       /* 1, or the length of a journey */
};

/* How far this track is from what the goal wants at 'step', or negative where
 * the goal cannot judge it at all. */
static int goal_score(const struct mix_goal *g, const struct sound_axes *a,
                      int step)
{
    int t;

    if (g->seed != NULL)
        return sound_mix_distance(g->seed, a);

    if (g->mood_to == g->mood_from)
        return sound_mood_score(a, g->mood_from);

    t = g->steps > 1 ? step * SOUND_AX / (g->steps - 1) : 0;

    return sound_mood_score_between(a, g->mood_from, g->mood_to, t);
}

struct pick
{
    uint64_t key;
    uint32_t artist;    /* 0 until the database pass finds the track */
    int32_t  idx;       /* Its master index entry, to read the path back */
    int      d;
};

/* Keep the nearest, worst last, so the far end is the one to displace. */
static int mix_insert(struct pick *best, int held, int want,
                      uint64_t key, int d)
{
    int i;

    if (held == want && d >= best[held - 1].d)
        return held;

    if (held < want)
        held++;

    for (i = held - 1; i > 0 && best[i - 1].d > d; i--)
        best[i] = best[i - 1];

    best[i].key = key;
    best[i].d = d;
    best[i].artist = 0;
    best[i].idx = -1;

    return held;
}

/* The folder above the album's -- for the usual Artist/Album/track layout,
 * the artist. Folded the same way keys are, since FAT hands the same folder
 * back cased differently from one read to the next. */
static uint32_t artist_key(const char *path)
{
    const char *p, *last = NULL, *cut = NULL;
    uint32_t h = 2166136261u;

    /* Volume specifier off first: a seed path and the walk's paths come from
     * two different tagcache calls, and one of them carries it. */
    path = sound_index_path(path);

    for (p = path; *p != '\0'; p++)
    {
        if (*p == '/')
        {
            cut = last;
            last = p;
        }
    }

    if (cut == NULL)
        cut = last;
    if (cut == NULL)
        return 0;

    for (p = path; p < cut; p++)
    {
        char c = *p;

        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';

        h = (h ^ (uint8_t)c) * 16777619u;
    }

    return h != 0 ? h : 1;
}

/* The keys of the tracks already in the playlist, most recent first, up to
 * 'max' of them. */
static int mix_playlist_keys(uint64_t *out, int max)
{
    struct playlist_track_info info;
    int amount = playlist_amount();
    int n = 0;
    int i;

    for (i = amount - 1; i >= 0 && n < max; i--)
    {
        if (playlist_get_track_info(NULL, i, &info) < 0)
            continue;

        out[n++] = sound_index_key(info.filename);
    }

    return n;
}

/* Seed the picking so that a mode which is meant to repeat, repeats.
 *
 * Weekly is seeded from the date rather than from a stored number, so it needs
 * nothing remembered and two players with the same library agree. The week it
 * changes on is whichever the clock says; there is no attempt to make that a
 * Monday. */
static int mix_vary_begin(int mode)
{
    struct tm *tm;

    switch (mode)
    {
    case MIX_VARY_WEEKLY:
        tm = get_time();
        srand(valid_time(tm) ? (unsigned)(tm->tm_year * 53 + tm->tm_yday / 7)
                             : 1);
        return MIX_VARY_CHOICE;

    case MIX_VARY_VARIABLE:
        srand((unsigned)current_tick);
        return MIX_VARY_CHOICE;

    default:
        /* Predictable takes the nearest eligible track every time, which is
         * one candidate to choose between. */
        return 1;
    }
}

/* The whole of building one, whatever it is being built from.
 *
 * 'skip_key' is the seed's own record, which must not match itself, and
 * 'seed_path' is the track that plays first. Both are empty for a mood. */
static int mix_build(const struct mix_goal *g, uint64_t skip_key,
                     const char *seed_path, int want, int vary, bool append)
{
    static struct pick cand[MIX_CAND];
    static uint8_t take[MIX_CAND];
    static int order[SOUND_MIX_MAX];   /* chosen, in running order */
    static int held[SOUND_MIX_MAX];    /* candidates kept, per step */
    struct sound_index_reader r;
    struct sound_record rec;
    struct sound_axes ta;
    struct tagcache_search tcs;
    struct playlist_insert_context context;
    char buf[MAX_PATH];
    static uint64_t excl[MIX_EXCLUDE];
    uint32_t seed_artist = seed_path != NULL ? artist_key(seed_path) : 0;
    int bucket = MIX_CAND / g->steps;
    int n_excl = append ? mix_playlist_keys(excl, MIX_EXCLUDE) : 0;
    int base = 0;
    int chosen = 0, added = 0;
    int n_choices = mix_vary_begin(vary);
    int i, j, s;

    for (s = 0; s < g->steps; s++)
        held[s] = 0;

    if (sound_index_reader_open(&r) != SOUND_OK)
        return SOUND_MIX_NO_INDEX;

    /* Pass one: every record, scored against every step of the goal. A
     * journey keeps a few candidates for each point along it rather than the
     * best overall, or the middle of the run would be filled with whatever
     * happened to suit its ends. */
    for (i = 0; i < r.count; i++)
    {
        if (!sound_index_read(&r, i, &rec))
            break;

        if (rec.key == skip_key || !sound_record_usable(&rec))
            continue;

        /* Already in the playlist this is extending. Refused here rather
         * than at the end, so a track that has just played does not take a
         * candidate slot from one that has not. */
        if (n_excl > 0)
        {
            for (s = 0; s < n_excl; s++)
            {
                if (excl[s] == rec.key)
                    break;
            }

            if (s < n_excl)
                continue;
        }

        sound_mix_axes(&rec, &ta);

        for (s = 0; s < g->steps; s++)
        {
            int d = goal_score(g, &ta, s);

            if (d >= 0)
                held[s] = mix_insert(cand + s * bucket, held[s], bucket,
                                     rec.key, d);
        }
    }

    sound_index_reader_close(&r);

    for (s = 0, i = 0; s < g->steps; s++)
        i += held[s];

    if (i == 0)
        return 0;

    /* Pass two: the database, for what is behind those keys. A record carries
     * no path, so this walk is the only way back to one -- and the only place
     * a candidate's artist and length can be read. */
    cpu_boost(true);

    if (!tagcache_search(&tcs, tag_filename))
    {
        cpu_boost(false);
        return SOUND_MIX_NO_DB;
    }

    while (tagcache_get_next(&tcs, buf, sizeof (buf)))
    {
        uint64_t key;

        if (tagcache_get_numeric(&tcs, tag_length) < MIX_MIN_LENGTH_MS)
            continue;

        key = sound_index_key(buf);

        for (s = 0; s < g->steps; s++)
        {
            for (i = 0; i < held[s]; i++)
            {
                struct pick *c = &cand[s * bucket + i];

                if (c->key != key || c->idx >= 0)
                    continue;

                c->artist = artist_key(buf);
                c->idx = tcs.idx_id;
                break;
            }
        }
    }

    tagcache_search_finish(&tcs);

    /* Choose, recording the order rather than a set of marks. The order is
     * the running order, and both artist rules are about where a track sits
     * in it -- reading them off the candidate list instead would measure
     * distance from the goal, which is a different thing entirely.
     *
     * A candidate the walk never reached is a record for a track that has
     * since left the player, or one it has just ruled too short. */
    memset(take, 0, sizeof (take));

    while (chosen < want)
    {
        int pool[MIX_VARY_CHOICE];
        int pooled = 0;
        int pick;
        int first = chosen * g->steps / want;

        /* The step this slot belongs to, then the ones after it: a journey
         * that runs out of candidates for its own point borrows from further
         * along rather than stopping short. A single-step goal has one bucket
         * and this is simply that bucket. */
        for (s = first; s < g->steps && pooled < n_choices; s++)
        {
            for (i = 0; i < held[s] && pooled < n_choices; i++)
            {
                int at = s * bucket + i;

                if (take[at] || cand[at].idx < 0)
                    continue;

                if (cand[at].artist != 0)
                {
                    int back = chosen < MIX_ARTIST_GAP ? chosen
                                                       : MIX_ARTIST_GAP;
                    int used = 0;

                    /* A seed plays first, so it is one of the recent entries
                     * until enough tracks have been chosen to push it out of
                     * range. */
                    if (chosen < MIX_ARTIST_GAP
                        && cand[at].artist == seed_artist)
                        continue;

                    for (j = chosen - back; j < chosen; j++)
                    {
                        if (cand[order[j]].artist == cand[at].artist)
                            break;
                    }

                    if (j < chosen)
                        continue;

                    for (j = 0; j < chosen; j++)
                    {
                        if (cand[order[j]].artist == cand[at].artist)
                            used++;
                    }

                    if (used >= MIX_PER_ARTIST)
                        continue;
                }

                pool[pooled++] = at;
            }
        }

        if (pooled == 0)
            break;

        pick = pooled > 1 ? pool[rand() % pooled] : pool[0];

        take[pick] = 1;
        order[chosen] = pick;
        chosen++;
    }

    if (chosen == 0)
    {
        cpu_boost(false);
        return 0;
    }

    /* Extending leaves the playlist alone: it is the listener's, it is the
     * history this run is avoiding repeats against, and replacing it would
     * throw away both. */
    if (append)
    {
        base = playlist_amount();
    }
    else
    {
        if (!warn_on_pl_erase())
        {
            cpu_boost(false);
            return SOUND_MIX_CANCELLED;
        }

        if (playlist_create(NULL, NULL) < 0)
        {
            cpu_boost(false);
            return SOUND_MIX_NO_PLAYLIST;
        }
    }

    /* Reopened rather than held across the choice: this one only reads the
     * entries already picked out, so it walks nothing. */
    if (!tagcache_search(&tcs, tag_filename))
    {
        cpu_boost(false);
        return SOUND_MIX_NO_DB;
    }

    if (playlist_insert_context_create(NULL, &context, PLAYLIST_INSERT_LAST,
                                       false, false) < 0)
    {
        /* create() keeps the playlist lock even when it fails; release() is
         * the only thing that gives it back. */
        playlist_insert_context_release(&context);
        tagcache_search_finish(&tcs);
        cpu_boost(false);
        return SOUND_MIX_NO_PLAYLIST;
    }

    /* The seed goes first, so a mix starts with what it was asked about. A
     * mood has nothing to start from and begins at its own first choice. */
    if (!append && seed_path != NULL
        && playlist_insert_context_add(&context, seed_path) >= 0)
    {
        added++;
    }

    for (i = 0; i < chosen; i++)
    {
        if (!tagcache_retrieve(&tcs, cand[order[i]].idx, tag_filename,
                               buf, sizeof (buf)))
            continue;

        if (playlist_insert_context_add(&context, buf) < 0)
            break;

        added++;
    }

    playlist_insert_context_release(&context);
    tagcache_search_finish(&tcs);
    cpu_boost(false);

    /* Tracks were chosen and none of them could be read back, which is a
     * different fault from finding nothing near enough. */
    if (added <= (!append && seed_path != NULL ? 1 : 0))
        return SOUND_MIX_NO_PLAYLIST;

    /* What built this, so a continuation can carry on in the same terms
     * rather than inferring them from whatever happened to play last. */
    remembered = *g;
    if (g->seed != NULL)
    {
        remembered_axes = *g->seed;
        remembered.seed = &remembered_axes;
    }
    have_remembered = true;

    playlist_start(base, 0, 0);

    return added;
}

int sound_mix_from_track(const char *path, int want)
{
    struct sound_index_reader r;
    struct sound_record seed;
    struct sound_axes sa;
    struct mix_goal g;
    uint64_t seed_key = sound_index_key(path);

    if (want < 1)
        want = 1;
    if (want > SOUND_MIX_MAX)
        want = SOUND_MIX_MAX;

    if (sound_index_reader_open(&r) != SOUND_OK)
        return SOUND_MIX_NO_INDEX;

    /* A record the decode failed on is in the index but is not a measurement,
     * and it is no more use as the thing being matched than as a match. */
    if (!sound_index_find(&r, seed_key, &seed) || !sound_record_usable(&seed))
    {
        sound_index_reader_close(&r);
        return SOUND_MIX_NO_RECORD;
    }

    sound_index_reader_close(&r);
    sound_mix_axes(&seed, &sa);

    g.seed = &sa;
    g.mood_from = -1;
    g.mood_to = -1;
    g.steps = 1;

    return mix_build(&g, seed_key, path, want,
                     global_settings.track_playlist, false);
}

int sound_mix_from_mood(int mood, int want)
{
    struct mix_goal g;

    if (want < 1)
        want = 1;
    if (want > SOUND_MIX_MAX)
        want = SOUND_MIX_MAX;

    g.seed = NULL;
    g.mood_from = mood;
    g.mood_to = mood;
    g.steps = 1;

    return mix_build(&g, 0, NULL, want, global_settings.mood_playlist, false);
}

int sound_mix_journey(int from, int to, int want)
{
    struct mix_goal g;

    if (want < 2)
        want = 2;
    if (want > SOUND_MIX_MAX)
        want = SOUND_MIX_MAX;

    g.seed = NULL;
    g.mood_from = from;
    g.mood_to = to;
    g.steps = want;

    return mix_build(&g, 0, NULL, want, global_settings.mood_playlist, false);
}

/** Carrying on **/

void sound_mix_forget(void)
{
    have_remembered = false;
}

void sound_mix_playlist_ended(void)
{
    continue_due = true;
}

bool sound_mix_continue_due(void)
{
    bool due = continue_due;

    continue_due = false;

    return due && global_settings.playlist_engine
           && global_settings.continue_playing;
}

int sound_mix_continue(int want)
{
    struct playlist_track_info info;
    struct sound_index_reader r;
    struct sound_record seed;
    struct sound_axes sa;
    struct mix_goal g;
    uint64_t seed_key = 0;
    int last = playlist_amount() - 1;

    if (want < 1)
        want = 1;
    if (want > SOUND_MIX_MAX)
        want = SOUND_MIX_MAX;

    if (last < 0)
        return 0;

    if (have_remembered)
    {
        /* The terms the playlist was built on. A mood keeps aiming at its own
         * point, which is the whole reason for remembering: seeding from the
         * last track instead would let a Calm run climb away from calm one
         * extension at a time. */
        g = remembered;

        if (g.seed != NULL)
            g.seed = &remembered_axes;
    }
    else
    {
        /* Nothing built this -- an album, a saved playlist, a folder. The
         * track that just finished is all there is to go on. */
        if (playlist_get_track_info(NULL, last, &info) < 0)
            return SOUND_MIX_NO_RECORD;

        seed_key = sound_index_key(info.filename);

        if (sound_index_reader_open(&r) != SOUND_OK)
            return SOUND_MIX_NO_INDEX;

        if (!sound_index_find(&r, seed_key, &seed)
            || !sound_record_usable(&seed))
        {
            sound_index_reader_close(&r);
            return SOUND_MIX_NO_RECORD;
        }

        sound_index_reader_close(&r);
        sound_mix_axes(&seed, &sa);

        g.seed = &sa;
        g.mood_from = -1;
        g.mood_to = -1;
        g.steps = 1;
    }

    /* A journey is not extended along its own path: it has arrived, and
     * walking it again would send the listener back to the beginning. What
     * carries on is the mood it ended in. */
    if (g.seed == NULL && g.mood_from != g.mood_to)
    {
        g.mood_from = g.mood_to;
        g.steps = 1;
    }

    return mix_build(&g, seed_key, NULL, want,
                     g.seed != NULL ? global_settings.track_playlist
                                    : global_settings.mood_playlist,
                     true);
}
