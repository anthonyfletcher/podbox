/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Folds the spectrum onto twelve pitch classes, and names the key.
 *
 * Three octaves of Goertzels, twelve notes each, summed by pitch class. The
 * result is correlated against the Krumhansl-Schmuckler profiles for every
 * major and minor key, and the best fit wins.
 *
 * Two things keep the cost down. The signal is decimated to about 11kHz
 * first, which is four times fewer samples for the same frequency resolution
 * -- nothing above the third octave is wanted, so the bandwidth is not
 * missed. And a frame runs five times a second rather than every hop:
 * harmony moves at the speed of chords, where onsets move at the speed of
 * drums, and it is that difference that makes this affordable beside the
 * onset bank rather than several times its cost.
 *
 * Parts, in order:
 *   - the note table
 *   - decimation and the frame loop
 *   - the profiles, and correlating against them
 *   - read-out
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "audio/spectrum_meter.h"
#include "audio/chroma.h"

/* Frames a second. Chords do not change faster than this, and the whole
 * economy of the module rests on it. */
#define CH_RATE_HZ     5

/* What the signal is decimated towards. The top note analysed is under 2kHz,
 * so this leaves better than two octaves of headroom above it. */
#define CH_TARGET_HZ   12000

#define CH_OCTAVES     3
#define CH_BUF         1024   /* Decimated samples: the longest window */

/* Per octave. Halving with each octave up keeps a Goertzel's main lobe the
 * same fraction of the semitone it has to resolve; a fixed window would be
 * needlessly sharp at the top and too blunt at the bottom. */
static const int ch_window[CH_OCTAVES] = { CH_BUF, CH_BUF / 2, CH_BUF / 4 };

/* Equal temperament from A4 = 440, starting at C4. Whole hertz: the error is
 * a tenth of a percent against a semitone spacing of six, and
 * spectrum_goertzel_magnitude() takes an integer frequency anyway. */
static const int ch_note_hz[12] = {
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494
};

/* Krumhansl-Schmuckler probe-tone profiles, x100: how strongly each scale
 * degree belongs to a major and to a minor key. Correlating the measured
 * pitch classes against all twelve rotations of each is what names a key. */
static const int16_t prof_major[12] = {
    635, 223, 348, 233, 438, 409, 252, 519, 239, 366, 229, 288
};
static const int16_t prof_minor[12] = {
    633, 268, 352, 538, 260, 353, 254, 475, 398, 269, 334, 317
};

static unsigned int  rate;          /* Decimated */
static int           decim;         /* Source samples per decimated one */
static int32_t       decim_acc;
static int           decim_n;

static int16_t       buf[CH_BUF];
static int           fill;
static int           hop;           /* Decimated samples between frames */
static int           since;

/* Summed over the track, and the previous frame for the change measure. */
static uint32_t      acc[12];
static uint16_t      prev[12];
static uint32_t      change_acc;
static unsigned int  frames;


/** Decimation and frames **/

void chroma_reset(unsigned int samplerate)
{
    memset(buf, 0, sizeof (buf));
    memset(acc, 0, sizeof (acc));
    memset(prev, 0, sizeof (prev));

    decim = samplerate / CH_TARGET_HZ;
    if (decim < 1)
        decim = 1;

    rate = samplerate / decim;
    hop = rate / CH_RATE_HZ;
    if (hop < CH_BUF)
        hop = CH_BUF;

    decim_acc = 0;
    decim_n = 0;
    fill = 0;
    since = 0;
    change_acc = 0;
    frames = 0;
}

/* One frame: 36 Goertzels onto twelve pitch classes, normalised so a loud
 * passage does not outvote a quiet one -- what is wanted is which notes are
 * present, not how loudly. */
static void chroma_frame(void)
{
    uint32_t v[12];
    uint32_t sum = 0;
    int pc, oct;

    for (pc = 0; pc < 12; pc++)
        v[pc] = 0;

    for (oct = 0; oct < CH_OCTAVES; oct++)
    {
        int n = ch_window[oct];
        const int16_t *src = buf + CH_BUF - n;

        for (pc = 0; pc < 12; pc++)
        {
            int f = ch_note_hz[pc] << oct;

            if (f * 2 >= (int)rate)
                continue;

            v[pc] += (uint32_t)spectrum_goertzel_magnitude(src, n, 1, f,
                                                           (int)rate);
        }
    }

    for (pc = 0; pc < 12; pc++)
        sum += v[pc];

    if (sum == 0)
        return;

    for (pc = 0; pc < 12; pc++)
    {
        /* Per mille of the frame's total, which keeps the running sums well
         * inside 32 bits over the thousands of frames a track can produce. */
        uint16_t n = (uint16_t)((uint64_t)v[pc] * 1000 / sum);

        acc[pc] += n;

        if (frames > 0)
            change_acc += n > prev[pc] ? n - prev[pc] : prev[pc] - n;

        prev[pc] = n;
    }

    frames++;
}

void chroma_feed(const int16_t *pcm, int n)
{
    int i;

    if (rate == 0)
        return;

    for (i = 0; i < n; i++)
    {
        decim_acc += (pcm[i * 2] + pcm[i * 2 + 1]) / 2;

        if (++decim_n < decim)
            continue;

        /* A box average over the decimation factor: crude as a filter, but
         * its first null sits at the decimated Nyquist, which is what a
         * decimator has to suppress. */
        if (fill < CH_BUF)
            buf[fill++] = (int16_t)(decim_acc / decim);

        decim_acc = 0;
        decim_n = 0;

        if (++since >= hop)
        {
            since = 0;

            if (fill >= CH_BUF)
                chroma_frame();

            fill = 0;
        }
    }
}


/** Naming the key **/

static uint32_t ch_root(uint32_t v)
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

/* Pearson correlation of a rotated pitch profile against a key profile,
 * x1000. The roots are taken separately rather than of the product: each
 * fits a 32-bit word on its own where the product does not. */
static int ch_corr(const int32_t *a, const int16_t *b)
{
    int32_t ma = 0, mb = 0;
    int64_t num = 0;
    uint32_t na = 0, nb = 0;
    uint32_t d;
    int i;

    for (i = 0; i < 12; i++)
    {
        ma += a[i];
        mb += b[i];
    }
    ma /= 12;
    mb /= 12;

    for (i = 0; i < 12; i++)
    {
        int32_t da = a[i] - ma;
        int32_t db = b[i] - mb;

        num += (int64_t)da * db;
        na += (uint32_t)(da * da);
        nb += (uint32_t)(db * db);
    }

    d = ch_root(na) * ch_root(nb);

    if (d == 0)
        return 0;

    return (int)(num * 1000 / d);
}

void chroma_get(struct chroma_result *out)
{
    int32_t c[12], rot[12];
    int32_t peak = 0, sum = 0, mean;
    int best = -2000, best_major = -2000, best_minor = -2000;
    int pc, k;

    memset(out, 0, sizeof (*out));

    out->frames = (uint16_t)(frames > 65535 ? 65535 : frames);

    if (frames == 0)
        return;

    for (pc = 0; pc < 12; pc++)
    {
        c[pc] = (int32_t)(acc[pc] / frames);
        sum += c[pc];
        if (c[pc] > peak)
            peak = c[pc];
    }

    if (peak == 0)
        return;

    mean = sum / 12;

    for (pc = 0; pc < 12; pc++)
        out->pitch[pc] = (uint8_t)(c[pc] * 255 / peak);

    for (k = 0; k < 12; k++)
    {
        int cm, cn;

        for (pc = 0; pc < 12; pc++)
            rot[pc] = c[(pc + k) % 12];

        cm = ch_corr(rot, prof_major);
        cn = ch_corr(rot, prof_minor);

        if (cm > best_major)
            best_major = cm;
        if (cn > best_minor)
            best_minor = cn;

        if (cm > best)
        {
            best = cm;
            out->key = (uint8_t)k;
            out->minor = false;
        }

        if (cn > best)
        {
            best = cn;
            out->key = (uint8_t)k;
            out->minor = true;
        }
    }

    {
        int m = out->minor ? best_minor - best_major : best_major - best_minor;

        m = m / 10;                 /* x1000 to x100, as the header states */
        out->margin = (uint8_t)(m < 0 ? 0 : (m > 255 ? 255 : m));
    }

    {
        int32_t clarity = mean > 0 ? peak * 100 / mean : 0;

        out->clarity = (uint8_t)(clarity > 255 ? 255 : clarity);
    }

    if (frames > 1)
    {
        /* Per mille across twelve classes, so a frame that changed chord
         * entirely moves by up to 2000. The eighth puts the range real music
         * occupies -- measured at 350 to 680 -- across the middle of the byte
         * instead of pinning every track to the ceiling. */
        uint32_t ch = change_acc / (frames - 1) / 8;

        out->change = (uint8_t)(ch > 255 ? 255 : ch);
    }
}
