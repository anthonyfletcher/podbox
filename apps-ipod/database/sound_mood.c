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
#include "database/sound_cal.h"
#include "database/sound_mood.h"

/* An axis a mood cares about, where it wants that axis, and how much it
 * matters against the mood's other axes. Weights are in tenths, as they are
 * for the track-to-track match.
 *
 * A target is a place in the library plus a tuning: 'pct' picks the place,
 * 'adj' is how far off it the listening put the number, and 'target' is the
 * two added up on the 3,439-record library these were fitted to -- which is
 * what stands where there is no calibration to ask.
 *
 * The tuning is kept separately because the targets were chosen by ear and
 * only *sit near* percentile points; they were never placed at them. Replacing
 * each with its nearest point discards that work, and measurably: on the very
 * library they were fitted to it moved mood membership by up to 23%. Carrying
 * the offset over instead reproduces every tuned number exactly there, and
 * moves it by however much another library's distribution differs.
 *
 * Speed and tempo are calibrated along with the rest, which is the one place
 * this disagrees with the read-out. A mood *selects* where the read-out
 * *describes*: Pace saying 90 BPM must mean 90 BPM whatever else is on the
 * player, but Slow that returns nothing on a fast library has failed at the
 * only thing it does. */
struct mood_axis
{
    uint8_t  cal;      /* CAL_*, which names the axis and its percentiles */
    int16_t  target;   /* 0 - SOUND_AX, the fallback */
    uint16_t pct;      /* per mille of this axis's distribution */
    int16_t  adj;      /* the tuning, in axis units off that point */
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

#define A_LOUD    CAL_LOUD
#define A_DENS    CAL_DENS
#define A_BRIGHT  CAL_BRIGHT
#define A_LOW     CAL_LOW
#define A_MID     CAL_MID
#define A_CREST   CAL_CREST
#define A_WIDTH   CAL_WIDTH
#define A_PEAK    CAL_PEAK
#define A_CLARITY CAL_CLARITY
#define A_CHANGE  CAL_CHANGE
#define A_TEMPO   CAL_TEMPO
#define A_SPEED   CAL_SPEED

/* Trap: the crest axis is inverted against its name. It is SOUND_AX minus the
 * measured crest factor, so a high value here is a compressed track and a low
 * one is a dynamic one. Dense wants it high; Calm and Punchy want it low. */

/* Trap: A_TEMPO is measured linearly here and circularly in
 * sound_mix_distance(). The axis wraps at 140 BPM, so between two tracks the
 * short way round is the true distance -- one read at 141 and one at 139 are
 * the same tempo either side of the fold. A target is not a track, so the
 * moods below read it linearly.
 *
 * That leaves the fold itself, which A_TEMPO still carries: a 160 BPM record
 * is tapped at 80 and reads as slow however the distance is measured. The two
 * moods that name a speed outright use A_SPEED for exactly that reason; the
 * rest name a feel, where the tapped reading is the right one. */

/* Trap: A_LOW, A_MID and A_BRIGHT are spectral balance, not band level --
 * each is its band against the track's own loudness (band_rel() in
 * sound_mix.c). So a low A_BRIGHT names a track with less treble than its
 * loudness would predict, which is what "dark" means, and not simply a quiet
 * one.
 *
 * Those three, and A_WIDTH, A_CLARITY and A_PEAK with them, are now derived
 * against a 3439-record index rather than the 90-record one the table was
 * written on. Eleven targets sat above that library's ninetieth percentile --
 * Dark's A_LOW at the 97th, Bright's A_BRIGHT at the 96th -- which is the
 * failure named at the top of this file: nothing can approach such a target,
 * so it penalises every candidate about equally and the mood stops choosing
 * on its own headline axis. Each was brought to the 85th or 90th percentile
 * of the measured distribution, keeping the order the table was written with,
 * and the three sitting below the tenth went to the fifteenth. */

/* A_SPEED here, and not because Calm is about tempo.
 *
 * The other four axes all read an acoustic recording as calm whatever it is
 * playing: onset density counts transients, and brushed drums and a walking
 * bass produce few, so a hard-swinging 129 BPM big band measured sparser than
 * a slow pop ballad. Mingus's "Boogie Stop Shuffle" scored 90 against Calm
 * while Taylor Swift's "epiphany" scored 105 -- the frantic track ranked
 * ahead of the quiet one. A speed term is what the other axes cannot supply,
 * and it only constrains the tracks that have a trusted tempo. */
static const struct mood_axis mx_calm[] = {
    { A_LOUD, 420, 76, 3, 10 }, { A_DENS, 200, 78, 5, 10 },
    { A_PEAK, 180, 82, 14, 8 }, { A_SPEED, 250, 135, 4, 8 },
    { A_CREST, 300, 97, -7, 4 }, { A_BRIGHT, 220, 417, -16, 4 } };
static const struct mood_axis mx_energetic[] = {
    { A_LOUD, 840, 903, 1, 10 }, { A_DENS, 810, 896, 5, 10 },
    { A_BRIGHT, 541, 837, 13, 6 }, { A_TEMPO, 760, 796, -4, 6 },
    { A_CREST, 800, 737, -7, 4 } };
static const struct mood_axis mx_dark[] = {
    { A_BRIGHT, 60, 162, 10, 10 }, { A_LOW, 571, 878, 10, 6 },
    { A_CLARITY, 250, 167, 4, 5 }, { A_LOUD, 650, 333, 2, 3 } };
static const struct mood_axis mx_bright[] = {
    { A_BRIGHT, 583, 875, 22, 10 }, { A_CLARITY, 658, 843, 11, 5 },
    { A_MID, 530, 849, -13, 4 }, { A_LOUD, 750, 594, 3, 3 } };
static const struct mood_axis mx_warm[] = {
    { A_BRIGHT, 110, 215, 7, 10 }, { A_MID, 550, 849, 7, 8 },
    { A_LOW, 535, 829, 6, 8 }, { A_LOUD, 650, 333, 2, 3 } };
static const struct mood_axis mx_raw[] = {
    { A_CLARITY, 229, 147, -6, 10 }, { A_PEAK, 590, 838, -6, 7 },
    { A_WIDTH, 65, 146, 0, 6 }, { A_CREST, 750, 580, 2, 3 } };
static const struct mood_axis mx_lush[] = {
    { A_WIDTH, 510, 845, 10, 9 }, { A_CLARITY, 658, 843, 11, 8 },
    { A_CHANGE, 650, 878, 5, 6 }, { A_CREST, 350, 159, -57, 3 } };
static const struct mood_axis mx_punchy[] = {
    { A_PEAK, 600, 838, 4, 10 }, { A_CREST, 300, 97, -7, 7 },
    { A_DENS, 700, 737, 3, 5 }, { A_LOUD, 800, 773, 4, 4 } };
static const struct mood_axis mx_smooth[] = {
    { A_PEAK, 208, 139, -6, 10 }, { A_CHANGE, 350, 84, 10, 6 },
    { A_CREST, 480, 241, 53, 4 }, { A_BRIGHT, 330, 560, 39, 3 } };
static const struct mood_axis mx_sparse[] = {
    { A_DENS, 200, 78, 5, 10 }, { A_CHANGE, 340, 84, 0, 6 },
    { A_CREST, 300, 97, -7, 6 }, { A_LOUD, 500, 129, 1, 4 } };
static const struct mood_axis mx_dense[] = {
    { A_DENS, 830, 919, 0, 10 }, { A_CHANGE, 660, 902, 7, 6 },
    { A_CREST, 870, 878, -29, 6 }, { A_LOUD, 830, 883, -2, 4 } };
static const struct mood_axis mx_hypnotic[] = {
    { A_CHANGE, 330, 61, 10, 9 }, { A_DENS, 560, 507, -1, 5 },
    { A_TEMPO, 540, 538, 3, 6 }, { A_PEAK, 300, 325, 1, 4 } };
/* The two that name a speed rather than a feel, and the only two on A_SPEED.
 *
 * Measured over a 3400-track library: with these on the folded axis, 151 of
 * the 199 tracks Slow returned were 140 BPM or faster, because folding halves
 * them into the slow half of the range. Unfolded, a target is a tempo.
 *
 * The targets are that library's tenth and ninetieth percentiles -- about 80
 * and 160 BPM on a 60-180 scale -- rather than the ends of the axis, so each
 * names music that exists rather than a corner nothing reaches. */
static const struct mood_axis mx_slow[] = {
    { A_SPEED, 165, 88, 4, 10 }, { A_DENS, 300, 151, 8, 3 } };
static const struct mood_axis mx_fast[] = {
    { A_SPEED, 835, 902, -1, 10 }, { A_DENS, 750, 829, -1, 3 } };
static const struct mood_axis mx_melancholy[] = {
    { A_TEMPO, 200, 186, 6, 7 }, { A_LOUD, 500, 129, 1, 6 },
    { A_BRIGHT, 170, 349, -33, 5 }, { A_DENS, 300, 151, 8, 5 } };
static const struct mood_axis mx_uplifting[] = {
    { A_TEMPO, 800, 815, 7, 7 }, { A_BRIGHT, 560, 875, -1, 7 },
    { A_LOUD, 800, 773, 4, 5 }, { A_DENS, 750, 829, -1, 5 } };

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

int sound_mood_name(int mood)
{
    if (mood < 0 || mood >= MOOD_COUNT)
        return LANG_MOOD_CALM;

    return moods[mood].lang;
}

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

/* Where this mood wants the axis on the library in front of it, or where it
 * wanted it on the library it was fitted to.
 *
 * sound_cal_ensure() is the scorers' business rather than this one's: it can
 * cost a pass over the index, and here it would be reached once per axis per
 * candidate. */
static int mood_target(const struct mood_axis *x)
{
    int cal = sound_cal_at(x->cal, x->pct);

    if (cal < 0)
        return x->target;

    cal += x->adj;

    return cal < 0 ? 0 : (cal > SOUND_AX ? SOUND_AX : cal);
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

    sound_cal_ensure();

    m = &moods[mood];

    if (m->needs_tempo && a->tempo < 0)
        return -1;

    for (i = 0; i < m->count; i++)
    {
        int v = *(const int *)((const char *)a +
                        sound_cal_offset(m->axes[i].cal));

        /* An axis the analysis could not read is skipped, and its weight with
         * it, exactly as the track-to-track match does. */
        if (v < 0)
            continue;

        d = v - mood_target(&m->axes[i]);
        sum += (uint32_t)(d * d / SOUND_AX) * m->axes[i].weight;
        total_w += m->axes[i].weight;
    }

    if (total_w == 0)
        return -1;

    d = (int)mood_root(sum * SOUND_AX / total_w);

    /* A mood that names a mode wants it. Disagreeing is a real difference and
     * costs; not knowing is not evidence either way and costs a little, so a
     * committed match is preferred without shutting out the undecided share
     * of a library.
     *
     * The penalty has to stay under MIX_MAX_DISTANCE or it stops being a
     * preference: a cost larger than the ceiling puts every disagreeing
     * track outside it whatever the rest of its numbers say, which makes
     * Dark "minor tracks only" and this comment a description of something
     * else. At 80 against a ceiling of 180 a major track can still reach
     * Dark, but only from inside 100 -- which is the intent stated above.
     *
     * Trap: a mood is the one place the pitch content is not also measured
     * directly. sound_mix_distance() has a harmony axis for what the mode
     * stands in for; a mood has no second track, so it skips that axis and
     * the mode is all it has. Measured on a 90-record index, the four
     * mode-named moods return one disagreeing track between them where at
     * 200 they returned none. That is the cost of the ceiling being a
     * ceiling. */
    if (m->mode >= 0)
    {
        if (a->mode < 0)
            d += 25;
        else if (a->mode != m->mode)
            d += 80;
    }

    return d;
}

/* What a mood wants of one axis, or a weight of zero where it does not
 * mention the axis at all. */
static void mood_axis_at(const struct mood_def *m, int cal,
                         int *target, int *weight)
{
    unsigned int i;

    for (i = 0; i < m->count; i++)
    {
        if (m->axes[i].cal == cal)
        {
            *target = mood_target(&m->axes[i]);
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

    sound_cal_ensure();

    ma = &moods[from];
    mb = &moods[to];

    if ((ma->needs_tempo || mb->needs_tempo) && a->tempo < 0)
        return -1;

    /* Every axis either mood names, once. The second loop skips what the
     * first has already covered, which is what makes this the union rather
     * than a list with the shared axes counted twice. */
    for (i = 0; i < (unsigned)(ma->count + mb->count); i++)
    {
        int cal;
        int ta = 0, wa = 0, tb = 0, wb = 0;
        int target, weight, v;

        if (i < ma->count)
        {
            cal = ma->axes[i].cal;
        }
        else
        {
            cal = mb->axes[i - ma->count].cal;

            for (j = 0; j < ma->count; j++)
            {
                if (ma->axes[j].cal == cal)
                    break;
            }

            if (j < ma->count)
                continue;
        }

        mood_axis_at(ma, cal, &ta, &wa);
        mood_axis_at(mb, cal, &tb, &wb);

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

        v = *(const int *)((const char *)a + sound_cal_offset(cal));

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
                d += 25;
            else if (a->mode != mode)
                d += 80;
        }
    }

    return d;
}
