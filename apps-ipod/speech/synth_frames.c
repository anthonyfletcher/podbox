/***************************************************************************
 * GNU General Public License (version 2 only)
 * Port of NV Speech Player, Copyright (C) 2014 NV Access Limited
 *
 * The prosody: turns one clause of IPA into timed, pitched frames and queues
 * them on synth.c. A port of NV Speech Player's ipa.py, whose rules give the
 * voice its rhythm, with the durations of voiced fricatives and a clause's
 * last vowel changed; a hat-pattern tune in place of its intonation table;
 * and formants that glide out of a consonant and towards a stop. The voice
 * is fixed to the add-on's "Adam".
 *
 * A clause is worked out whole into phs[] by synth_clause_begin(), then
 * handed to synth.c a frame at a time by synth_clause_feed() as its queue has
 * room. One clause at a time, on one thread.
 *
 * Fixed point: durations in microseconds, speeds in millionths, pitch Q16.
 *
 * Parts, in order:
 *   - the phoneme list and the voice
 *   - IPA to phonemes: syllables, word starts, stop gaps, aspiration
 *   - durations
 *   - pitch: the hat pattern
 *   - the public interface: frames, and the glides between them
 ****************************************************************************/

#include <string.h>

#include "synth.h"
#include "synth_frames.h"
#include "synth_phonemes.h"

#define SYNTH_MAX_PHONEMES 192  /* per clause, gaps and aspirations included */

#define BASE_PITCH  110         /* Hz: NV Speech Player's pitch setting 50 */

/* What the parse adds to a phoneme beyond its table flags */
#define M_SILENCE        0x001  /* a gap: queued as silence */
#define M_PRE_STOP_GAP   0x002
#define M_POST_STOP_ASP  0x004  /* an h inserted after a voiceless stop */
#define M_TIED_TO        0x008  /* first half of a tied pair */
#define M_TIED_FROM      0x010
#define M_LENGTHENED     0x020
#define M_SYLLABLE_START 0x040
#define M_WORD_START     0x080

struct ph
{
    uint16_t flags;             /* PH_* from the table */
    uint16_t marks;             /* M_* */
    int stress;                 /* on a syllable start: 1 primary, 2 secondary */
    uint64_t present;           /* bit per parameter this phoneme sets */
    synth_param_t p[SYNTH_PARAM_COUNT];
    int32_t duration, fade;     /* us */
};

static struct ph phs[SYNTH_MAX_PHONEMES];
static int nph;
static int clause_type;
static int32_t clause_speed;    /* millionths of NV Speech Player's rate */
static int feed_pos;            /* next phs[] to queue; nph is the end pause */

/** Phoneme list and voice **/

/* Adam: multipliers over the table's values */
static void apply_voice(struct synth_frame *f)
{
    f->p[SP_CB1] = f->p[SP_CB1] * 13 / 10;
    f->p[SP_PA6] = f->p[SP_PA6] * 13 / 10;
    f->p[SP_FRICATION_AMPLITUDE] = f->p[SP_FRICATION_AMPLITUDE] * 17 / 20;
}

/* ms at NV Speech Player's normal rate, in us at speed */
static int32_t at_speed(int ms, int32_t speed)
{
    return (int64_t)ms * 1000000000 / speed;
}

static const struct synth_phoneme *lookup(const uint16_t *s, int n)
{
    for (int i = 0; i < synth_phoneme_count; i++)
    {
        const uint16_t *k = synth_phonemes[i].key;
        if (memcmp(k, s, n * sizeof(*s)) == 0 && (n == 3 || k[n] == 0))
            return &synth_phonemes[i];
    }
    return NULL;
}

static struct ph *push(const struct synth_phoneme *d, unsigned int marks)
{
    if (nph == SYNTH_MAX_PHONEMES)
        return NULL;
    struct ph *ph = &phs[nph++];
    memset(ph, 0, sizeof(*ph));
    ph->marks = marks;
    if (d)
    {
        ph->flags = d->flags;
        ph->present = d->present;
        memcpy(ph->p, d->p, sizeof(ph->p));
    }
    return ph;
}

/** IPA to phonemes **/

static const struct synth_phoneme *table_h(void)
{
    static const uint16_t h = 'h';
    return lookup(&h, 1);
}

/* Returns false when phs[] filled before the IPA ran out. */
static bool parse(const uint16_t *ipa, int len)
{
    bool new_word = true;
    int last = -1;              /* the previous real phoneme */
    int syl = -1;               /* the current syllable's start */
    int cur_stress = 0;
    int offset = 0;

    for (int i = 0; i < len; i++)
    {
        int at = i + offset;
        if (at >= len)
            break;
        uint16_t c = ipa[at];
        if (c == IPA_STRESS_PRIMARY)
        {
            cur_stress = 1;
            continue;
        }
        if (c == IPA_STRESS_SECONDARY)
        {
            cur_stress = 2;
            continue;
        }

        bool lengthened = at + 1 < len && ipa[at + 1] == IPA_LENGTH;
        bool tied_to = at + 1 < len && ipa[at + 1] == IPA_TIE;
        bool tied_from = at > 0 && ipa[at - 1] == IPA_TIE;
        const struct synth_phoneme *d = NULL;
        if (tied_to)
        {
            d = lookup(&ipa[at], len - at < 3 ? len - at : 3);
            offset += d ? 2 : 1;
        }
        else if (lengthened)
        {
            d = lookup(&ipa[at], 2);
            offset += 1;
        }
        if (!d)
            d = lookup(&ipa[at], 1);
        if (!d)
        {
            if (c == ' ')
                new_word = true;
            continue;
        }

        int stress = cur_stress;
        cur_stress = 0;
        unsigned int marks = 0;
        if (tied_from)
            marks |= M_TIED_FROM;
        else if (tied_to)
            marks |= M_TIED_TO;
        if (lengthened)
            marks |= M_LENGTHENED;

        /* syl_here: this phoneme, not yet in phs[], starts the syllable */
        bool syl_here = false;
        if (last >= 0 && !(phs[last].flags & PH_VOWEL)
            && (d->flags & PH_VOWEL))
        {
            phs[last].marks |= M_SYLLABLE_START;
            syl = last;
        }
        else if (stress == 1 && last >= 0 && (phs[last].flags & PH_VOWEL))
        {
            marks |= M_SYLLABLE_START;
            syl_here = true;
        }
        if (last >= 0 && (phs[last].flags & PH_STOP)
            && !(phs[last].flags & PH_VOICED) && (d->flags & PH_VOICED)
            && !(d->flags & (PH_STOP | PH_AFRICATE)))
        {
            if (!push(table_h(), M_POST_STOP_ASP))
                return false;
            last = nph - 1;
        }
        if (new_word)
        {
            new_word = false;
            marks |= M_WORD_START | M_SYLLABLE_START;
            syl_here = true;
        }

        int own_stress = 0;
        if (stress)
        {
            if (syl_here)
                own_stress = stress;
            else
                phs[syl].stress = stress;
        }
        else if (d->flags & (PH_STOP | PH_AFRICATE))
        {
            if (!push(NULL, M_SILENCE | M_PRE_STOP_GAP))
                return false;
        }

        struct ph *ph = push(d, marks);
        if (!ph)
            return false;
        ph->stress = own_stress;
        last = nph - 1;
        if (syl_here)
            syl = last;
    }
    return true;
}

/* An h takes the parameters it lacks from the phoneme after it, or, before
 * a gap or at the end, from the one before. */
static void fill_h(void)
{
    for (int i = 0; i < nph; i++)
    {
        struct ph *ph = &phs[i];
        if (!(ph->flags & PH_COPY_ADJACENT))
            continue;
        const struct ph *adj = NULL;
        if (i + 1 < nph && !(phs[i + 1].marks & M_SILENCE))
            adj = &phs[i + 1];
        else if (i > 0)
            adj = &phs[i - 1];
        if (!adj)
            continue;
        for (int k = 0; k < SYNTH_PARAM_COUNT; k++)
        {
            uint64_t bit = 1ull << k;
            if ((adj->present & bit) && !(ph->present & bit))
            {
                ph->p[k] = adj->p[k];
                ph->present |= bit;
            }
        }
    }
}

/** Durations **/

static void set_durations(int32_t base_speed)
{
    const struct ph *prev = NULL;
    int syl_stress = 0;
    int32_t speed = base_speed;
    int last_vowel = nph - 1;
    while (last_vowel >= 0 && !(phs[last_vowel].flags & PH_VOWEL))
        last_vowel--;

    for (int i = 0; i < nph; i++)
    {
        struct ph *ph = &phs[i];
        const struct ph *next = i + 1 < nph ? &phs[i + 1] : NULL;
        bool syl_start = ph->marks & M_SYLLABLE_START;
        if (syl_start)
        {
            syl_stress = ph->stress;
            if (syl_stress)     /* stressed syllables are 1.4 or 1.1 slower */
                speed = syl_stress == 1 ? base_speed * 10 / 14
                                        : base_speed * 10 / 11;
            else
                speed = base_speed;
        }

        int32_t duration = at_speed(60, speed);
        int32_t fade = at_speed(10, speed);
        if (ph->marks & M_PRE_STOP_GAP)
            duration = at_speed(41, speed);
        else if (ph->marks & M_POST_STOP_ASP)
            duration = at_speed(20, speed);
        else if (ph->flags & PH_STOP)
        {
            duration = at_speed(6, speed);
            if (duration > 6000)
                duration = 6000;
            fade = 1;
        }
        else if (ph->flags & PH_AFRICATE)
        {
            duration = at_speed(24, speed);
            fade = 1;
        }
        else if (!(ph->flags & PH_VOICED))
            duration = at_speed(45, speed);
        else if (ph->flags & PH_VOWEL)
        {
            if (prev && (prev->flags & (PH_LIQUID | PH_SEMIVOWEL)))
                fade = at_speed(25, speed);
            if (ph->marks & M_TIED_TO)
                duration = at_speed(40, speed);
            else if (ph->marks & M_TIED_FROM)
            {
                duration = at_speed(20, speed);
                fade = at_speed(20, speed);
            }
            else if (!syl_stress && !syl_start && next
                     && !(next->marks & M_WORD_START)
                     && (next->flags & (PH_LIQUID | PH_NASAL)))
            {
                duration = at_speed((next->flags & PH_LIQUID) ? 30 : 40,
                                    speed);
            }
        }
        else if (!(ph->flags & (PH_NASAL | PH_LIQUID | PH_SEMIVOWEL))
                 && ph->p[SP_FRICATION_AMPLITUDE] > 0)
        {
            /* v, z: as long as f and s, or between vowels they are heard
             * as n */
            duration = at_speed(45, speed);
        }
        else
        {
            duration = at_speed(30, speed);
            if (ph->flags & (PH_LIQUID | PH_SEMIVOWEL))
                fade = at_speed(20, speed);
        }
        if (ph->marks & M_LENGTHENED)
            duration = duration * 105 / 100;
        if (i == last_vowel)            /* held before the pause */
            duration = duration * 3 / 2;
        ph->duration = duration;
        ph->fade = fade;
        prev = ph;
    }
}

/** Pitch **/

/* Hat-pattern intonation, in thousandths of BASE_PITCH: a baseline falling
 * through the clause, a plateau from the first stressed vowel, a falling
 * accent on each stressed vowel, and on the last the clause's ending -- a
 * fall for '.' and '!', a rise for ',' and a high rise for '?'. */
#define HAT_HEIGHT   120
#define HAT_ACCENT   50
#define HAT_LOW     (-60)
#define HAT_QUESTION 400
#define HAT_CONTINUE 100

static void hat_pitches(int clause)
{
    static bool stressed[SYNTH_MAX_PHONEMES];
    int64_t total = 0;
    int first = -1, last = -1, stress = 0;
    for (int i = 0; i < nph; i++)
    {
        const struct ph *ph = &phs[i];
        total += ph->duration;
        if (ph->marks & M_SYLLABLE_START)
            stress = ph->stress;
        stressed[i] = (ph->flags & PH_VOWEL) && stress == 1
                      && !(ph->marks & M_TIED_FROM);
        if (stressed[i])
        {
            if (first < 0)
                first = i;
            last = i;
        }
    }
    if (total == 0)
        return;
    bool falls = clause != ',' && clause != '?';
    int64_t t = 0, t_nucleus = 0;
    for (int i = 0; i < nph; i++)
    {
        struct ph *ph = &phs[i];
        int64_t t0 = t, t1 = t + ph->duration;
        t = t1;
        if (i == last)
            t_nucleus = t0;
        int base0 = (falls ? 1080 : 1050)
                    - (falls ? 200 : 100) * t0 / total;
        int base1 = (falls ? 1080 : 1050)
                    - (falls ? 200 : 100) * t1 / total;
        int s0 = 0, s1 = 0;
        if (first >= 0 && i >= first && i < last)
        {
            s0 = s1 = HAT_HEIGHT;
            if (stressed[i])
                s0 += HAT_ACCENT;
        }
        else if (first >= 0 && i >= last)
        {
            int64_t span = total - t_nucleus;
            if (clause == '?')
            {
                s0 = span ? HAT_QUESTION * (t0 - t_nucleus) / span : 0;
                s1 = span ? HAT_QUESTION * (t1 - t_nucleus) / span : 0;
            }
            else if (clause == ',')
            {
                s0 = i == last ? HAT_HEIGHT : HAT_HEIGHT + HAT_CONTINUE;
                s1 = HAT_HEIGHT + HAT_CONTINUE;
            }
            else
            {
                s0 = i == last ? HAT_HEIGHT + HAT_ACCENT : HAT_LOW;
                s1 = HAT_LOW;
            }
        }
        ph->p[SP_VOICE_PITCH] = ((int64_t)BASE_PITCH << 16) * (base0 + s0)
                                / 1000;
        ph->p[SP_END_VOICE_PITCH] = ((int64_t)BASE_PITCH << 16)
                                    * (base1 + s1) / 1000;
        ph->present |= (1ull << SP_VOICE_PITCH) | (1ull << SP_END_VOICE_PITCH);
    }
}

/** Interface **/

static void make_frame(const struct ph *ph, struct synth_frame *f)
{
    memset(f, 0, sizeof(*f));
    f->p[SP_PRE_FORMANT_GAIN] = SYNTH_ONE;
    f->p[SP_OUTPUT_GAIN] = 2 * SYNTH_ONE;
    for (int k = 0; k < SYNTH_PARAM_COUNT; k++)
        if (ph->present & (1ull << k))
            f->p[k] = ph->p[k];
    apply_voice(f);
}

bool synth_clause_begin(const uint16_t *ipa, int len, int clause,
                        int speed)
{
    nph = 0;
    feed_pos = 0;
    clause_type = clause;
    clause_speed = speed * 1000;
    bool whole = parse(ipa, len);
    if (nph > 0)
    {
        fill_h();
        set_durations(clause_speed);
        hat_pitches(clause);
    }
    return whole;
}

bool synth_clause_feed(void)
{
    while (feed_pos < nph)
    {
        const struct ph *ph = &phs[feed_pos];
        const struct ph *before = feed_pos > 0 ? &phs[feed_pos - 1] : NULL;
        if ((ph->marks & M_PRE_STOP_GAP) && feed_pos + 1 < nph)
        {
            /* the closure: the stop's formants, silent, so the vowel before
             * bends towards them */
            struct synth_frame f;
            make_frame(&phs[feed_pos + 1], &f);
            f.p[SP_VOICE_PITCH] = ph->p[SP_VOICE_PITCH];
            f.p[SP_END_VOICE_PITCH] = ph->p[SP_END_VOICE_PITCH];
            f.p[SP_VOICE_AMPLITUDE] = 0;
            f.p[SP_ASPIRATION_AMPLITUDE] = 0;
            f.p[SP_FRICATION_AMPLITUDE] = 0;
            int32_t fade = at_speed(20, clause_speed);
            if (fade > ph->duration)
                fade = ph->duration;
            if (!synth_queue_frame(&f, ph->duration, fade, 0))
                return false;
        }
        else if (ph->marks & M_SILENCE)
        {
            if (!synth_queue_frame(NULL, ph->duration, ph->fade, 0))
                return false;
        }
        else
        {
            struct synth_frame f;
            make_frame(ph, &f);
            /* after a consonant a vowel's formants glide from it while its
             * source switches at once */
            int32_t glide = 0;
            if ((ph->flags & PH_VOWEL) && before
                && !(before->marks & M_SILENCE)
                && !(before->flags & (PH_VOWEL | PH_LIQUID | PH_SEMIVOWEL)))
            {
                glide = at_speed(30, clause_speed);
                if (glide > ph->duration * 3 / 4)
                    glide = ph->duration * 3 / 4;
            }
            if (!synth_queue_frame(&f, ph->duration, ph->fade, glide))
                return false;
        }
        feed_pos++;
    }
    if (feed_pos == nph)
    {
        int pause = clause_type == ',' ? 120
                  : (clause_type == '.' || clause_type == '?'
                     || clause_type == '!') ? 300 : 100;
        int32_t fade = at_speed(10, clause_speed);
        if (!synth_queue_frame(NULL, at_speed(pause, clause_speed),
                               fade > 10000 ? fade : 10000, 0))
            return false;
        feed_pos++;
    }
    return true;
}
