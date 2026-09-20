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
 * The order the chosen come out in is a chain, not a ranking. Each slot is
 * filled by weighing every eligible candidate against the goal *and* against
 * the track before it, under a cap on the energy step between neighbours --
 * because how near two tracks are to the same goal says nothing about how
 * they sound one after the other.
 *
 * Parts, in order:
 *   - the axes, and what they are worth against each other
 *   - distance between two tracks, the harmony axis included
 *   - the candidate pool, the sampling and the artist rules
 *   - the goal, the two passes and the chain
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

/* One hundred level units in tenths of a decibel.
 *
 * level[] is spectrum_scale_to_level()'s scale, (ln(raw) - 4) * 100 / 6, so a
 * hundred units span six nats -- 52.1 dB -- and this is where that scale and
 * loudness_db10's meet. */
#define MIX_LEVEL_DB10  521

/* A band's level against the track's own loudness, in level units.
 *
 * Trap: an absolute band level rises and falls with the master rather than
 * with the music. Measured over a 90-record index, level[0], level[1],
 * level[2] and the crest factor correlate with loudness at +0.82, +0.75,
 * +0.86 and +0.95, so an axis table that weighs each of them separately is
 * charging five times over for one thing. Taking the loudness off leaves
 * spectral balance, which is what a band level is wanted for.
 *
 * Reference the loudness and not the mean of the three bands. Three
 * differences from their own mean sum to zero, so one of the three carries
 * nothing of its own -- and the mean decorrelates the middle band no better:
 * mean absolute correlation with loudness 0.34 against 0.33.
 *
 * The constant offset between the two scales does not matter. It shifts all
 * three axes equally and the endpoints absorb it. */
static int band_rel(const struct sound_record *r, int g, int lo, int hi)
{
    return nrm(r->level[g] - r->loudness_db10 * 100 / MIX_LEVEL_DB10, lo, hi);
}

void sound_mix_axes(const struct sound_record *r, struct sound_axes *out)
{
    uint32_t sq = 0;
    int bpm;
    int i;

    memset(out, 0, sizeof (*out));

    out->loud    = nrm(r->loudness_db10, -300, -60);
    out->dens    = nrm(r->rate10[0] + r->rate10[1] + r->rate10[2], 0, 90);

    /* Endpoints from the 2nd and 98th percentile of a 90-record index,
     * rounded outwards. A small sample and a narrow one; they want
     * re-deriving against a library of thousands. */
    out->low     = band_rel(r, BEAT_LOW,  72, 100);
    out->mid     = band_rel(r, BEAT_MID,  72,  92);
    out->bright  = band_rel(r, BEAT_HIGH, 36,  60);

    out->crest   = AX - nrm(r->crest_db, 8, 20);
    out->width   = nrm(r->width, 0, 100);
    out->peak    = nrm(r->peakiness, 5, 60);
    out->clarity = nrm(r->tonal_clarity, 110, 230);
    out->change  = nrm(r->harmonic_change, 30, 90);

    /* How far the level moved across the window: quiet-then-loud high, a
     * track mastered flat low. Endpoints from the same index, where the
     * field spans 9 to 23. */
    out->dynamics = nrm(r->level_spread, 8, 24);

    /* Tempo only where the tracker held still enough for these axes to mean
     * something, which is SOUND_TEMPO_MATCH_PER_MILLE and not the tighter
     * bound beside it -- see sound_index.h. Measured over a rebuilt
     * 3464-record index, 3262 tracks lock and 89% of the library passes this
     * against 66% at the phase bound. */
    bpm = r->period_ms ? 60000 / r->period_ms : 0;

    /* Trusted on the same terms, and then read two ways. Folded, for anything
     * comparing one track against another: 87 and 174 BPM are tapped alike
     * and a mix built on the difference between them is built on nothing.
     * Unfolded, for anything that names a speed outright -- see 'speed' in
     * sound_mix.h.
     *
     * The two are not interchangeable, and the range each is normalised over
     * says why: folding compresses the whole library into one octave, so
     * 60-180 BPM measured across 3400 tracks becomes 70-140 tapped. */
    if (bpm > 0 && r->tempo_spread * 1000 <=
                   r->period_ms * SOUND_TEMPO_MATCH_PER_MILLE)
    {
        out->tempo = nrm(fold_bpm(bpm), 70, 140);
        out->speed = nrm(bpm, 60, 180);
    }
    else
    {
        out->tempo = out->speed = -1;
    }

    /* How well it held, which is worth knowing exactly where the tempo is
     * not: the spread that puts the axis above out of use is this axis's
     * signal. Absent only where there was no lock, since the spread then
     * describes nothing. The 40 rounds the 98th percentile of the measured
     * spread outwards, as the band endpoints above do. */
    out->steady = r->period_ms ? AX - nrm(r->tempo_spread, 0, 40) : -1;

    /* Available on about three fifths of a real library. Absent is not the
     * same as neutral, so it is marked rather than defaulted. */
    out->mode = r->mode_margin >= CHROMA_MARGIN_MIN ? r->mode : -1;

    for (i = 0; i < 12; i++)
    {
        out->pitch[i] = r->pitch[i];
        sq += (uint32_t)r->pitch[i] * r->pitch[i];
    }

    out->pitch_norm = (uint16_t)mix_root(sq);

    out->genre = r->genre_key;
    out->year  = (r->year > 1900 && r->year < 2100) ? r->year : 0;

    /* Loudness and density carry most of what a listener calls energy; tempo
     * adds least, because density has already said how much is happening.
     * A track with no trusted tempo takes the middle rather than zero, so the
     * absence does not read as "calm". Brightness here is the relative axis,
     * so what it adds is treble-forwardness rather than level a second
     * time. */
    out->energy = (30 * out->loud + 28 * out->dens + 18 * out->bright
                   + 14 * (out->tempo >= 0 ? out->tempo : AX / 2)
                   + 10 * out->crest) / 100;
}


/** Distance **/

/* What holds a mix together, in the order it matters.
 *
 * The balance between the bands comes first, because that is what separates
 * records the tempo cannot: two tracks at 120 BPM can be a folk ballad and a
 * techno record, and only the band balance says which.
 *
 * Crest and width carry the production era, which nothing else here reads: a
 * dynamic narrow record and a compressed wide one sit decades apart however
 * alike the rest of their numbers are.
 *
 * Loudness, crest, peakiness and density carry 21 of 85 between them, and
 * that is a ceiling rather than a valuation: they measure one thing four
 * times over (band_rel()), so weight given to them is weight given twice.
 * What they do not carry goes to the three relative bands and to harmony,
 * which is the only axis here that hears notes.
 *
 * 'circular' is true of the tempo alone, and it is a property of the fold
 * rather than of tempo: fold_bpm() wraps at 140 BPM, so a track the tracker
 * read at 141 lands beside 70 and one at 139 lands at the far end. Between
 * two tracks that wrap is an artefact and the short way round is the true
 * distance. It is not applied to a mood's tempo target -- see
 * sound_mood.c.
 *
 * Weights are in tenths so they can be integers. */
static const struct { size_t off; int w; bool circular; } mix_weights[] = {
    { offsetof(struct sound_axes, bright),   11, false },
    { offsetof(struct sound_axes, low),       9, false },
    { offsetof(struct sound_axes, loud),      7, false },
    { offsetof(struct sound_axes, tempo),     7, true  },
    { offsetof(struct sound_axes, mid),       6, false },
    { offsetof(struct sound_axes, width),     6, false },
    { offsetof(struct sound_axes, crest),     5, false },
    { offsetof(struct sound_axes, dens),      5, false },
    { offsetof(struct sound_axes, clarity),   5, false },
    { offsetof(struct sound_axes, peak),      4, false },
    { offsetof(struct sound_axes, change),    4, false },
    { offsetof(struct sound_axes, dynamics),  4, false },
    { offsetof(struct sound_axes, steady),    4, false },
};

#define MIX_AXES (sizeof (mix_weights) / sizeof (mix_weights[0]))

/* Harmony: how unlike two tracks' note content is.
 *
 * The angle between the two chroma vectors, which is what "the same notes"
 * means when the key is not known -- and it works where the mode does not,
 * because it needs no decision about a tonic. It is the reason the two mode
 * penalties could come down: a mode is a single bit derived from this, and
 * wrong a third of the time.
 *
 * Scaled, because real music uses only the bottom third of 1 - cos. Measured
 * over 4005 pairs the median is 69 where the median across the other axes is
 * 164, so a weight of 8 on the raw figure would buy about a twentieth of
 * what 8 buys elsewhere. The gain puts it on the same footing. */
#define MIX_HARM_W     8
#define MIX_HARM_GAIN  24    /* tenths */

static int mix_harmony(const struct sound_axes *a, const struct sound_axes *b)
{
    uint32_t dot = 0;
    int d;
    int i;

    /* A track the chroma never read has no note content to compare, which is
     * the missing-axis case rather than a track unlike everything. */
    if (a->pitch_norm == 0 || b->pitch_norm == 0)
        return -1;

    for (i = 0; i < 12; i++)
        dot += (uint32_t)a->pitch[i] * b->pitch[i];

    d = AX - (int)(dot * AX / ((uint32_t)a->pitch_norm * b->pitch_norm));
    d = d * MIX_HARM_GAIN / 10;

    if (d < 0)
        return 0;

    return d > AX ? AX : d;
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

        d = x > y ? x - y : y - x;

        if (mix_weights[i].circular && d > AX - d)
            d = AX - d;

        sum += (uint32_t)(d * d / AX) * mix_weights[i].w;
        total_w += mix_weights[i].w;
    }

    d = mix_harmony(a, b);
    if (d >= 0)
    {
        sum += (uint32_t)(d * d / AX) * MIX_HARM_W;
        total_w += MIX_HARM_W;
    }

    if (total_w == 0)
        return AX;

    d = (int)mix_root(sum * AX / total_w);

    /* A mode disagreement is a real difference between two tracks that both
     * committed to one. An abstention is not evidence of anything, so it
     * costs nothing.
     *
     * Halved now that harmony carries the note content directly. The mode is
     * one bit squeezed out of the same twelve numbers, and it abstains on two
     * fifths of a library; charging heavily for it was paying for a summary
     * of an axis that is now in the table above. */
    if (a->mode >= 0 && b->mode >= 0 && a->mode != b->mode)
        d += 30;

    /* Soft. A hard genre filter would make this a genre browser, which the
     * database already does better. */
    if (a->genre != 0 && a->genre == b->genre)
        d -= 50;

    /* Era, convex and reaching half a century. A decade between two records
     * is a pleasant surprise and costs almost nothing; fifty years is a
     * different collection. A linear term saturating at 25 charges both the
     * same. */
    if (a->year && b->year)
    {
        int gap = a->year > b->year ? a->year - b->year : b->year - a->year;

        if (gap > 50)
            gap = 50;

        d += 90 * gap * gap / (50 * 50);
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

/* What a slot costs a candidate: how far it is from the goal, and how far it
 * is from the track that will play before it. Tenths.
 *
 * Ordering by distance to the goal alone is what makes a mix lurch: the two
 * nearest tracks to a goal can sit on opposite sides of it and still be
 * adjacent in the list. Charging for the step as well turns the running
 * order into a walk through the neighbourhood rather than a ranking of it.
 * The goal keeps the larger share or the walk drifts away from what was
 * asked for. */
#define MIX_GOAL_W      7
#define MIX_PREV_W      3

/* The most the energy may move between two neighbours, as a hard cap on top
 * of the cost above. This is the axis a listener notices a break in, and a
 * weighted sum will always trade it for something else; a cap will not.
 *
 * Relaxed for one slot rather than ending the playlist where nothing can
 * meet it -- a step nobody can take should shorten the step, not the list. */
#define MIX_ENERGY_STEP 120

/* How spread the sampling is, in the same units as the cost above.
 *
 * A varying mix picks by weight rather than taking the best, with the weight
 * falling off as exp(-over/MIX_TAU) where 'over' is how much worse than the
 * best eligible candidate this one is. Nearer still usually wins -- at 30,
 * a candidate one tau behind is worth a third of the best -- while a run of
 * forty from three hundred candidates returns a different forty each time,
 * which is what the setting promises. Measured on a 90-record index: five
 * runs from one seed return 13 or 14 tracks each and 24 distinct tracks
 * between them. */
#define MIX_TAU         30

/* exp(-x) at x = i/8, times 1024. Forty entries reach x = 4.9, by which the
 * weight is under a hundredth of the best and a candidate is out of reach;
 * past the end the table reads as zero rather than growing a tail of ones. */
static const uint16_t mix_exp[40] = {
    1024,  904,  797,  704,  621,  548,  484,  427,
     377,  332,  293,  259,  228,  202,  178,  157,
     139,  122,  108,   95,   84,   74,   65,   58,
      51,   45,   40,   35,   31,   27,   24,   21,
      19,   17,   15,   13,   11,   10,    9,    8,
};

/* How far from the goal a track may sit and still be offered.
 *
 * Without a ceiling the nearest 'want' tracks win however far away they are,
 * so a mood always returns a full playlist: on a library with a dozen calm
 * records, Calm returns those twelve and then the twenty-eight next least
 * frantic things in the collection. The complaint that follows is not that
 * the ranking is wrong -- it is right -- but that the tail of the list was
 * never an example of the mood.
 *
 * A score is the weighted RMS distance per axis on the 0-1000 scale, and the
 * axes are absolute rather than normalised across the library (sound_mix.h),
 * so the number means the same thing on every player.
 *
 * Short, not empty. Everything nearer than this is still offered, in order,
 * and a mood with nothing inside it at all falls through to the caller's
 * "nothing near enough" -- which sound_mix.h has always promised and nothing
 * ever produced.
 *
 * The rule is one standard deviation below the mean distance between two
 * records picked at random, which is the point where a candidate stops being
 * explicable as chance. Measured over the 4005 pairs of a 90-record index:
 * mean 271, standard deviation 91, so the ceiling admits a candidate nearer
 * than 84% of random pairs and refuses the rest.
 *
 * The number has to be re-derived whenever the axes or the weights change,
 * because it is a point on their scale and not a property of the music. 300
 * on this scale is the 63rd percentile -- a ceiling that admits most of the
 * library is the tail it exists to cut.
 *
 * Ninety records of eight albums is a small and narrow sample, and the
 * absolute number wants re-deriving against a library of thousands. The rule
 * does not. */

/* Tracks shorter than this are not offered. They are intros, interludes and
 * segues -- they measure as real tracks and arrive as real matches, and a
 * playlist of them is not what anybody asked for. Read from the database
 * rather than the index, which does not store a length. */
#define MIX_MIN_LENGTH_MS 90000

/* When a candidate is the seed again under another path -- a second encode of
 * the same file, which a playlist must not offer as a match for it.
 *
 * Distance alone will not do this, and the temptation to let it is why the
 * pair of tests is here rather than in the first pass. Measured over a
 * 90-record index, 21 of the records sit within 15 of another one, and every
 * one of those pairs is two different songs off one album: the axes are
 * eleven numbers and real tracks collide on them. So the length has to agree
 * too, which a re-encode's does and a neighbour's does not. */
#define MIX_SEED_EPSILON  15
#define MIX_SEED_SAME_MS  1000

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

/* A track the listener left inside this much of its start was not a match,
 * whatever the numbers said.
 *
 * Kept separately from the playlist history above, and that is the whole of
 * what it buys: the history is bounded at MIX_EXCLUDE and a playlist that
 * keeps being continued grows past it, at which point an early track is
 * eligible again. That is deliberate for a track that merely played -- it is
 * not for one that was skipped out of after eight seconds.
 *
 * Not persisted, and not fed back into the analysis. The index describes what
 * a track sounds like; this describes one evening. */
#define MIX_SKIP_MS  20000
#define MIX_SKIPPED  32

/* Tracks skipped out of early, oldest first, forgotten with the playlist. */
static uint64_t          skipped[MIX_SKIPPED];
static int               n_skipped;

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
    int32_t  len;       /* Its length in ms, for the duplicate test */
    int      d;         /* To the goal, at whichever step it suits best */
};

/* Keep the nearest MIX_CAND of them, and nothing about their order.
 *
 * Unordered on purpose. Nothing downstream reads the candidates in order --
 * the chain weighs every eligible one at every slot -- so the pool has only
 * to know which member to give up next, and that is one scan when a
 * displacement happens rather than a shift per insert. It matters because
 * each candidate now carries its coordinates: a sorted insert would move a
 * hundred and twenty bytes a step, several thousand times over an index. */
static int mix_insert(struct pick *cand, struct sound_axes *cax, int held,
                      uint64_t key, int d, const struct sound_axes *ax,
                      int *worst)
{
    int at, i;

    if (held < MIX_CAND)
    {
        at = held++;
    }
    else
    {
        if (d >= cand[*worst].d)
            return held;

        at = *worst;
    }

    cand[at].key = key;
    cand[at].d = d;
    cand[at].artist = 0;
    cand[at].len = 0;
    cand[at].idx = -1;
    cax[at] = *ax;

    *worst = 0;
    for (i = 1; i < held; i++)
    {
        if (cand[i].d > cand[*worst].d)
            *worst = i;
    }

    return held;
}

/* FNV-1a over a string or a slice of one, folded to lower case -- FAT hands
 * the same name back cased differently from one read to the next. Never
 * zero, which is the "no artist" value. */
static uint32_t fold_key(const char *s, const char *end)
{
    uint32_t h = 2166136261u;

    for (; *s != '\0' && (end == NULL || s < end); s++)
    {
        char c = *s;

        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';

        h = (h ^ (uint8_t)c) * 16777619u;
    }

    return h != 0 ? h : 1;
}

/* The folder above the album's -- for the usual Artist/Album/track layout,
 * the artist. The fallback, for a library with no artist tags. */
static uint32_t artist_key(const char *path)
{
    const char *p, *last = NULL, *cut = NULL;

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

    return fold_key(path, cut);
}

/* Who a track is by, as the database says rather than as its folder implies.
 *
 * The folder above the album is the artist only in a tree that is laid out
 * that way. A compilation, a Various Artists folder, a flat directory of
 * singles and a soundtrack all defeat it -- and where it is defeated the two
 * artist rules stop applying, which is the whole of what keeps a playlist
 * from being one album reshuffled.
 *
 * Trap: a tag hash and a folder hash are not the same value for the same
 * artist, so a library where some tracks carry the tag and others do not
 * groups those two sets separately. Unavoidable from here, and the fallback
 * is still better than no artist at all. */
static uint32_t mix_artist_key(struct tagcache_search *tcs, const char *path)
{
    char artist[64];

    if (tagcache_retrieve(tcs, tcs->idx_id, tag_artist, artist,
                          sizeof (artist)) && artist[0] != '\0')
    {
        return fold_key(artist, NULL);
    }

    return artist_key(path);
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
        return MIX_TAU;

    case MIX_VARY_VARIABLE:
        srand((unsigned)current_tick);
        return MIX_TAU;

    default:
        /* Predictable is the same sampling at zero temperature: the best
         * eligible candidate, every time. */
        return 0;
    }
}

/* What a candidate 'over' worse than the best is worth against it. */
static uint32_t mix_weight(int over, int tau)
{
    unsigned int i;

    if (over <= 0)
        return mix_exp[0];

    i = (unsigned int)(over * 8 / tau);

    return i < sizeof (mix_exp) / sizeof (mix_exp[0]) ? mix_exp[i] : 0;
}

/* One of the eligible candidates, with a weight of exp(-over/tau).
 *
 * Returns a position in 'elig'. At tau 0 that is the best of them, which is
 * what makes Predictable a case of this rather than a branch around it. */
static int mix_sample(const int *cost, const int16_t *elig, int n, int best,
                      int tau)
{
    uint32_t total = 0;
    uint32_t r;
    int i;

    if (tau <= 0 || n < 2)
        return 0;

    for (i = 0; i < n; i++)
        total += mix_weight(cost[elig[i]] - best, tau);

    if (total == 0)
        return 0;

    r = (uint32_t)rand() % total;

    for (i = 0; i < n; i++)
    {
        uint32_t w = mix_weight(cost[elig[i]] - best, tau);

        if (r < w)
            return i;

        r -= w;
    }

    return 0;
}

/* The two artist rules, read off the running order rather than off the
 * candidate list: both are about where a track sits in the playlist, and
 * reading them off the candidates would measure distance from the goal
 * instead, which is a different thing entirely. */
static bool mix_artist_ok(const struct pick *cand, const int *order,
                          int chosen, int at, uint32_t seed_artist)
{
    uint32_t who = cand[at].artist;
    int back = chosen < MIX_ARTIST_GAP ? chosen : MIX_ARTIST_GAP;
    int used = 0;
    int j;

    if (who == 0)
        return true;

    /* A seed plays first, so it is one of the recent entries until enough
     * tracks have been chosen to push it out of range. */
    if (chosen < MIX_ARTIST_GAP && who == seed_artist)
        return false;

    for (j = chosen - back; j < chosen; j++)
    {
        if (cand[order[j]].artist == who)
            return false;
    }

    for (j = 0; j < chosen; j++)
    {
        if (cand[order[j]].artist == who)
            used++;
    }

    return used < MIX_PER_ARTIST;
}

/* The whole of building one, whatever it is being built from.
 *
 * 'skip_key' is the seed's own record, which must not match itself, and
 * 'seed_path' is the track that plays first. Both are empty for a mood. */
static int mix_build(const struct mix_goal *g, uint64_t skip_key,
                     const char *seed_path, int want, int vary, bool append)
{
    static struct pick cand[MIX_CAND];
    static struct sound_axes cand_ax[MIX_CAND];
    static int cost[MIX_CAND];         /* the chain cost, this slot */
    static int16_t elig[MIX_CAND];     /* which candidates passed the rules */
    static uint8_t take[MIX_CAND];
    static int order[SOUND_MIX_MAX];   /* chosen, in running order */
    struct sound_index_reader r;
    struct sound_record rec;
    struct sound_axes ta;
    struct tagcache_search tcs;
    struct playlist_insert_context context;
    char buf[MAX_PATH];
    static uint64_t excl[MIX_EXCLUDE];
    uint32_t seed_artist = seed_path != NULL ? artist_key(seed_path) : 0;
    int n_excl = append ? mix_playlist_keys(excl, MIX_EXCLUDE - MIX_SKIPPED)
                        : 0;
    int32_t seed_len = 0;
    int held = 0, worst = 0;
    int base = 0;
    int chosen = 0, added = 0;
    int tau = mix_vary_begin(vary);
    int i, s;

    /* Skips are refused alongside the history, with room reserved above so a
     * long playlist cannot crowd them out -- which is the only thing they are
     * here for. See MIX_SKIP_MS. */
    if (append)
    {
        for (i = 0; i < n_skipped && n_excl < MIX_EXCLUDE; i++)
            excl[n_excl++] = skipped[i];
    }

    if (sound_index_reader_open(&r) != SOUND_OK)
        return SOUND_MIX_NO_INDEX;

    /* Boosted across both passes, not just the database walk. Pass one is the
     * expensive half: a seek and a read per record, and a scoring with an
     * integer square root in it, done once per step of the goal -- which for
     * a journey is a hundred times over every record in the index. */
    cpu_boost(true);

    /* Pass one: every record, scored against every step of the goal and kept
     * at whichever step suits it best.
     *
     * One pool, and not a bucket per step of a journey.
     *
     * Trap: a bucket per step divides MIX_CAND by the length of the run --
     * seven candidates a step for a journey of forty -- and fills each one
     * whether the library holds anything suitable at that point or not.
     * Worse, a track near two steps occupies a slot in both, and the mark
     * that says it has been taken is per slot, so a journey built that way
     * returns every track twice.
     *
     * The chain below scores each candidate again against the step of the
     * slot it is offered for, so what a candidate is worth at a point in the
     * run is decided where that matters and the pool has only to hold the
     * best of the library. Borrowing from later steps goes with the
     * buckets. */
    for (i = 0; i < r.count; i++)
    {
        int bestd = -1;

        /* Scheduling is cooperative and this pass is boosted, so without
         * this it holds the CPU for the whole library: the UI stops and the
         * codec stops refilling, which is heard. Every sector's worth of
         * records rather than every record, so the yield lands where the
         * read does and costs nothing in between. */
        if ((i & 63) == 0)
            yield();

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

            if (d >= 0 && (bestd < 0 || d < bestd))
                bestd = d;
        }

        if (bestd < 0 || bestd > MIX_MAX_DISTANCE)
            continue;

        held = mix_insert(cand, cand_ax, held, rec.key, bestd, &ta, &worst);
    }

    sound_index_reader_close(&r);

    if (held == 0)
    {
        cpu_boost(false);
        return 0;
    }

    /* Pass two: the database, for what is behind those keys. A record carries
     * no path, so this walk is the only way back to one -- and the only place
     * a candidate's artist and length can be read. */
    if (!tagcache_search(&tcs, tag_filename))
    {
        cpu_boost(false);
        return SOUND_MIX_NO_DB;
    }

    while (tagcache_get_next(&tcs, buf, sizeof (buf)))
    {
        uint64_t key = sound_index_key(buf);
        long len = tagcache_get_numeric(&tcs, tag_length);

        /* The same reason as pass one: a boosted walk of the whole database
         * with nothing else able to run. */
        yield();

        /* The seed's own row, for the two things only the database holds: who
         * it is by, which the artist rules space the playlist against, and
         * how long it is, which is how a second encode of it is told from a
         * track that merely sounds like it. Before the length test, because
         * the seed is what was asked about and is not subject to it. */
        if (skip_key != 0 && key == skip_key)
        {
            seed_len = (int32_t)len;

            if (seed_path != NULL)
                seed_artist = mix_artist_key(&tcs, buf);

            continue;
        }

        if (len < MIX_MIN_LENGTH_MS)
            continue;

        for (i = 0; i < held; i++)
        {
            struct pick *c = &cand[i];

            if (c->key != key || c->idx >= 0)
                continue;

            c->artist = mix_artist_key(&tcs, buf);
            c->len = (int32_t)len;
            c->idx = tcs.idx_id;
            break;
        }
    }

    tagcache_search_finish(&tcs);

    /* The seed again under another path. Both tests, for the reason on
     * MIX_SEED_EPSILON. */
    if (g->seed != NULL && seed_len > 0)
    {
        for (i = 0; i < held; i++)
        {
            int32_t gap = cand[i].len - seed_len;

            if (cand[i].d < MIX_SEED_EPSILON
                && (gap < 0 ? -gap : gap) <= MIX_SEED_SAME_MS)
            {
                cand[i].idx = -1;
            }
        }
    }

    /* Choose, as a chain rather than as a ranking.
     *
     * The order recorded here is the running order, and both artist rules are
     * about where a track sits in it -- reading them off the candidate list
     * instead would measure distance from the goal, which is a different
     * thing entirely.
     *
     * Every slot weighs every eligible candidate against two things: the
     * goal at this point of the run, and the track that will play before it.
     * Ranking by the goal alone is not an order anybody listens in -- two
     * tracks a hair from the goal on opposite sides of it are adjacent in
     * it.
     *
     * A candidate the walk never reached is a record for a track that has
     * since left the player, one it has just ruled too short, or the seed
     * under a second path. */
    memset(take, 0, sizeof (take));

    while (chosen < want)
    {
        const struct sound_axes *prev = NULL;
        int step = g->steps > 1 ? chosen * g->steps / want : 0;
        int cap = MIX_ENERGY_STEP;
        int n_elig = 0;
        int best = 0;

        if (chosen > 0)
            prev = &cand_ax[order[chosen - 1]];
        else if (g->seed != NULL)
            prev = g->seed;   /* the seed plays first, so it is the previous */

        /* Twice at most: with the energy cap, then without it. A step nobody
         * can take shortens the step rather than the playlist. */
        for (;;)
        {
            for (i = 0; i < held; i++)
            {
                int dg, dp;

                if (take[i] || cand[i].idx < 0)
                    continue;

                if (!mix_artist_ok(cand, order, chosen, i, seed_artist))
                    continue;

                if (prev != NULL && cap > 0)
                {
                    int de = cand_ax[i].energy - prev->energy;

                    if ((de < 0 ? -de : de) > cap)
                        continue;
                }

                /* Scored against this slot's point of the journey, not
                 * against the step the pool filed it under. */
                dg = goal_score(g, &cand_ax[i], step);

                if (dg < 0 || dg > MIX_MAX_DISTANCE)
                    continue;

                dp = prev != NULL ? sound_mix_distance(prev, &cand_ax[i]) : 0;
                cost[i] = (MIX_GOAL_W * dg + MIX_PREV_W * dp) / 10;

                if (n_elig == 0 || cost[i] < best)
                    best = cost[i];

                elig[n_elig++] = (int16_t)i;
            }

            if (n_elig > 0 || cap == 0)
                break;

            cap = 0;
        }

        if (n_elig == 0)
            break;

        i = elig[mix_sample(cost, elig, n_elig, best, tau)];

        take[i] = 1;
        order[chosen] = i;
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
    n_skipped = 0;
}

void sound_mix_skipped(const char *path, unsigned long elapsed_ms)
{
    if (!have_remembered || path == NULL || elapsed_ms >= MIX_SKIP_MS)
        return;

    /* The oldest goes first. A listener who has skipped thirty-two tracks out
     * of one playlist is not being served by it, and the first few are the
     * least likely still to be reachable. */
    if (n_skipped == MIX_SKIPPED)
    {
        memmove(skipped, skipped + 1, (MIX_SKIPPED - 1) * sizeof (skipped[0]));
        n_skipped--;
    }

    skipped[n_skipped++] = sound_index_key(path);
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
