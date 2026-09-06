/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Named places in the sound index.
 *
 * A mood is a point in the same axis space a track-to-track match uses, with
 * its own weights: Calm is quiet, sparse, unpeaked and dynamic, and the tracks
 * in it are the ones nearest that description. Nothing here is measured --
 * every number is a target for an axis the record already carries, so the
 * whole table can be re-tuned in a firmware update without rescanning
 * anything.
 *
 * The targets are read off a real library rather than invented: each is a
 * percentile of that axis across a few thousand tracks, so "low brightness"
 * means low compared to music that exists rather than low on a scale nothing
 * reaches. A target of 1000 on an axis nothing scores above 700 selects on
 * noise.
 *
 * Parts, in order:
 *   - the axis references
 *   - the table
 *   - scoring
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "system.h"
#include "lang.h"
#include "database/sound_mood.h"

/* An axis a mood cares about, where it wants that axis, and how much it
 * matters against the mood's other axes. Weights are in tenths, as they are
 * for the track-to-track match. */
struct mood_axis
{
    size_t   off;      /* into struct sound_axes */
    int16_t  target;   /* 0 - SOUND_AX */
    uint8_t  weight;
};

struct mood_def
{
    int lang;
    const struct mood_axis *axes;
    uint8_t count;
    int8_t  mode;         /* -1 any, 0 major, 1 minor */
    bool    needs_tempo;  /* judged only where the tempo is trusted */
};

#define A_LOUD    offsetof(struct sound_axes, loud)
#define A_DENS    offsetof(struct sound_axes, dens)
#define A_BRIGHT  offsetof(struct sound_axes, bright)
#define A_LOW     offsetof(struct sound_axes, low)
#define A_MID     offsetof(struct sound_axes, mid)
#define A_CREST   offsetof(struct sound_axes, crest)
#define A_WIDTH   offsetof(struct sound_axes, width)
#define A_PEAK    offsetof(struct sound_axes, peak)
#define A_CLARITY offsetof(struct sound_axes, clarity)
#define A_CHANGE  offsetof(struct sound_axes, change)
#define A_TEMPO   offsetof(struct sound_axes, tempo)

/* Trap: the crest axis is inverted against its name. It is SOUND_AX minus the
 * measured crest factor, so a high value here is a compressed track and a low
 * one is a dynamic one. Dense wants it high; Calm and Punchy want it low. */

static const struct mood_axis mx_calm[] = {
    { A_LOUD, 420, 10 }, { A_DENS, 200, 10 }, { A_PEAK, 180, 8 },
    { A_CREST, 300, 4 }, { A_BRIGHT, 250, 4 } };
static const struct mood_axis mx_energetic[] = {
    { A_LOUD, 840, 10 }, { A_DENS, 810, 10 }, { A_BRIGHT, 700, 6 },
    { A_TEMPO, 760, 6 }, { A_CREST, 800, 4 } };
static const struct mood_axis mx_dark[] = {
    { A_BRIGHT, 90, 10 }, { A_LOW, 850, 6 }, { A_CLARITY, 250, 5 },
    { A_LOUD, 650, 3 } };
static const struct mood_axis mx_bright[] = {
    { A_BRIGHT, 750, 10 }, { A_CLARITY, 700, 5 }, { A_MID, 790, 4 },
    { A_LOUD, 750, 3 } };
static const struct mood_axis mx_warm[] = {
    { A_BRIGHT, 140, 10 }, { A_MID, 800, 8 }, { A_LOW, 830, 8 },
    { A_LOUD, 650, 3 } };
static const struct mood_axis mx_raw[] = {
    { A_CLARITY, 190, 10 }, { A_PEAK, 680, 7 }, { A_WIDTH, 40, 6 },
    { A_CREST, 750, 3 } };
static const struct mood_axis mx_lush[] = {
    { A_WIDTH, 680, 9 }, { A_CLARITY, 760, 8 }, { A_CHANGE, 650, 6 },
    { A_CREST, 350, 3 } };
static const struct mood_axis mx_punchy[] = {
    { A_PEAK, 690, 10 }, { A_CREST, 300, 7 }, { A_DENS, 700, 5 },
    { A_LOUD, 800, 4 } };
static const struct mood_axis mx_smooth[] = {
    { A_PEAK, 170, 10 }, { A_CHANGE, 350, 6 }, { A_CREST, 480, 4 },
    { A_BRIGHT, 350, 3 } };
static const struct mood_axis mx_sparse[] = {
    { A_DENS, 200, 10 }, { A_CHANGE, 340, 6 }, { A_CREST, 300, 6 },
    { A_LOUD, 500, 4 } };
static const struct mood_axis mx_dense[] = {
    { A_DENS, 830, 10 }, { A_CHANGE, 660, 6 }, { A_CREST, 870, 6 },
    { A_LOUD, 830, 4 } };
static const struct mood_axis mx_hypnotic[] = {
    { A_CHANGE, 330, 9 }, { A_DENS, 560, 5 }, { A_TEMPO, 540, 6 },
    { A_PEAK, 300, 4 } };
static const struct mood_axis mx_slow[] = {
    { A_TEMPO, 90, 10 }, { A_DENS, 300, 3 } };
static const struct mood_axis mx_fast[] = {
    { A_TEMPO, 910, 10 }, { A_DENS, 750, 3 } };
static const struct mood_axis mx_melancholy[] = {
    { A_TEMPO, 200, 7 }, { A_LOUD, 500, 6 }, { A_BRIGHT, 200, 5 },
    { A_DENS, 300, 5 } };
static const struct mood_axis mx_uplifting[] = {
    { A_TEMPO, 800, 7 }, { A_BRIGHT, 720, 7 }, { A_LOUD, 800, 5 },
    { A_DENS, 750, 5 } };

#define MOOD(name, lang_id, want_mode, want_tempo)                          \
    { lang_id, mx_##name,                                                   \
      sizeof (mx_##name) / sizeof (mx_##name[0]), want_mode, want_tempo }

static const struct mood_def moods[MOOD_COUNT] = {
    [MOOD_CALM]       = MOOD(calm,       LANG_MOOD_CALM,       -1, false),
    [MOOD_ENERGETIC]  = MOOD(energetic,  LANG_MOOD_ENERGETIC,  -1, false),
    [MOOD_DARK]       = MOOD(dark,       LANG_MOOD_DARK,        1, false),
    [MOOD_BRIGHT]     = MOOD(bright,     LANG_MOOD_BRIGHT,      0, false),
    [MOOD_WARM]       = MOOD(warm,       LANG_MOOD_WARM,       -1, false),
    [MOOD_RAW]        = MOOD(raw,        LANG_MOOD_RAW,        -1, false),
    [MOOD_LUSH]       = MOOD(lush,       LANG_MOOD_LUSH,       -1, false),
    [MOOD_PUNCHY]     = MOOD(punchy,     LANG_MOOD_PUNCHY,     -1, false),
    [MOOD_SMOOTH]     = MOOD(smooth,     LANG_MOOD_SMOOTH,     -1, false),
    [MOOD_SPARSE]     = MOOD(sparse,     LANG_MOOD_SPARSE,     -1, false),
    [MOOD_DENSE]      = MOOD(dense,      LANG_MOOD_DENSE,      -1, false),
    [MOOD_HYPNOTIC]   = MOOD(hypnotic,   LANG_MOOD_HYPNOTIC,   -1, true),
    [MOOD_SLOW]       = MOOD(slow,       LANG_MOOD_SLOW,       -1, true),
    [MOOD_FAST]       = MOOD(fast,       LANG_MOOD_FAST,       -1, true),
    [MOOD_MELANCHOLY] = MOOD(melancholy, LANG_MOOD_MELANCHOLY,  1, false),
    [MOOD_UPLIFTING]  = MOOD(uplifting,  LANG_MOOD_UPLIFTING,   0, false),
};

static uint32_t mood_root(uint32_t v)
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

int sound_mood_score(const struct sound_axes *a, int mood)
{
    const struct mood_def *m;
    uint32_t sum = 0;
    int total_w = 0;
    int d;
    unsigned int i;

    if (mood < 0 || mood >= MOOD_COUNT)
        return -1;

    m = &moods[mood];

    if (m->needs_tempo && a->tempo < 0)
        return -1;

    for (i = 0; i < m->count; i++)
    {
        int v = *(const int *)((const char *)a + m->axes[i].off);

        /* An axis the analysis could not read is skipped, and its weight with
         * it, exactly as the track-to-track match does. */
        if (v < 0)
            continue;

        d = v - m->axes[i].target;
        sum += (uint32_t)(d * d / SOUND_AX) * m->axes[i].weight;
        total_w += m->axes[i].weight;
    }

    if (total_w == 0)
        return -1;

    d = (int)mood_root(sum * SOUND_AX / total_w);

    /* A mood that names a mode wants it. Disagreeing is a real difference and
     * costs heavily; not knowing is not evidence either way and costs a
     * little, so a committed match is preferred without shutting out the
     * undecided share of a library. */
    if (m->mode >= 0)
    {
        if (a->mode < 0)
            d += 60;
        else if (a->mode != m->mode)
            d += 200;
    }

    return d;
}

/* What a mood wants of one axis, or a weight of zero where it does not
 * mention the axis at all. */
static void mood_axis_at(const struct mood_def *m, size_t off,
                         int *target, int *weight)
{
    unsigned int i;

    for (i = 0; i < m->count; i++)
    {
        if (m->axes[i].off == off)
        {
            *target = m->axes[i].target;
            *weight = m->axes[i].weight;
            return;
        }
    }

    *weight = 0;
}

int sound_mood_score_between(const struct sound_axes *a, int from, int to,
                             int t)
{
    const struct mood_def *ma, *mb;
    uint32_t sum = 0;
    int total_w = 0;
    int d;
    unsigned int i, j;

    if (from < 0 || from >= MOOD_COUNT || to < 0 || to >= MOOD_COUNT)
        return -1;

    if (from == to)
        return sound_mood_score(a, from);

    ma = &moods[from];
    mb = &moods[to];

    if ((ma->needs_tempo || mb->needs_tempo) && a->tempo < 0)
        return -1;

    /* Every axis either mood names, once. The second loop skips what the
     * first has already covered, which is what makes this the union rather
     * than a list with the shared axes counted twice. */
    for (i = 0; i < (unsigned)(ma->count + mb->count); i++)
    {
        size_t off;
        int ta = 0, wa = 0, tb = 0, wb = 0;
        int target, weight, v;

        if (i < ma->count)
        {
            off = ma->axes[i].off;
        }
        else
        {
            off = mb->axes[i - ma->count].off;

            for (j = 0; j < ma->count; j++)
            {
                if (ma->axes[j].off == off)
                    break;
            }

            if (j < ma->count)
                continue;
        }

        mood_axis_at(ma, off, &ta, &wa);
        mood_axis_at(mb, off, &tb, &wb);

        /* An axis only one mood names keeps that mood's target throughout,
         * so the fading weight is the whole of the change. Interpolating
         * towards a target nobody asked for would drag the axis somewhere
         * neither end wants. */
        if (wa == 0)
            ta = tb;
        if (wb == 0)
            tb = ta;

        target = (ta * (SOUND_AX - t) + tb * t) / SOUND_AX;
        weight = (wa * (SOUND_AX - t) + wb * t) / SOUND_AX;

        if (weight <= 0)
            continue;

        v = *(const int *)((const char *)a + off);

        if (v < 0)
            continue;

        d = v - target;
        sum += (uint32_t)(d * d / SOUND_AX) * weight;
        total_w += weight;
    }

    if (total_w == 0)
        return -1;

    d = (int)mood_root(sum * SOUND_AX / total_w);

    /* The mode of whichever end this point is nearer. There is no half a
     * mode, and a journey between two moods that name different ones should
     * change over rather than want neither. */
    {
        int8_t mode = t < SOUND_AX / 2 ? ma->mode : mb->mode;

        if (mode >= 0)
        {
            if (a->mode < 0)
                d += 60;
            else if (a->mode != mode)
                d += 200;
        }
    }

    return d;
}
