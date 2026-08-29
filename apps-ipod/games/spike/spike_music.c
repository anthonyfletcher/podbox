/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The envelope, gathered into beats and bars. See spike_music.h for what
 * this is instead of, and why.
 ****************************************************************************/

#include "config.h"
#include "pcm.h"
#include "pcm_mixer.h"
#include "games/spike/spike_music.h"

/* Beats kept: two bars, so this bar can be held against the one before. */
#define SPK_M_BEATS      8
#define SPK_M_BAR        4

/* Peaks are 16-bit magnitudes; this brings them into a range where a bar's
 * worth still fits a long several times over. */
#define SPK_M_SHIFT      5
#define SPK_M_FULL       (32767 >> SPK_M_SHIFT)

static int  energy[SPK_M_BEATS];
static int  moved[SPK_M_BEATS];
static int  at;
static int  beats;

static int  last;               /* the beat before, for the movement */
static bool have_last;

/* The loudest the track has been, decaying about a percent and a half a beat
 * so a track that opens loud and then stays quiet is not read as quiet for
 * ever. Energy is reported against this rather than against full scale:
 * §9.3's conditions are all about one passage against another. */
static int  loud;

void spk_music_reset(void)
{
    int i;

    for (i = 0; i < SPK_M_BEATS; i++)
        energy[i] = moved[i] = 0;

    at = 0;
    beats = 0;
    last = 0;
    have_last = false;
    loud = 1;
}

/* Once a beat, and only once a beat.
 *
 * The mixer's reading is a peak *since the last call*, so calling it here
 * and nowhere else makes each one the peak over exactly one beat -- a
 * function of the audio and of the grid, and not of how many frames the
 * screen managed in between. Sampled per frame instead it is biased rather
 * than merely noisy: fewer frames means a peak taken over a longer window,
 * so a slow stretch reads louder. That bias is what made a music-driven
 * course and a fixed one mutually exclusive, and it is the whole reason
 * this file was out of the build.
 *
 * Movement is then between beats rather than between frames, which is the
 * same idea at the resolution that is left: a bar whose beats differ from
 * one another is a busy bar. */
void spk_music_beat(void)
{
    static struct pcm_peaks peaks;
    int e;

    mixer_channel_calculate_peaks(PCM_MIXER_CHAN_PLAYBACK, &peaks);

    e = (int)((peaks.left > peaks.right ? peaks.left : peaks.right)
              >> SPK_M_SHIFT);

    if (e > SPK_M_FULL)
        e = SPK_M_FULL;

    energy[at] = e;
    moved[at] = have_last ? (e > last ? e - last : last - e) : 0;
    last = e;
    have_last = true;

    at = (at + 1) % SPK_M_BEATS;

    if (beats < SPK_M_BEATS)
        beats++;

    if (e > loud)
        loud = e;
    else
        loud -= loud >> 6;

    if (loud < 1)
        loud = 1;
}

void spk_music_get(struct spk_mood *mood)
{
    int i, e = 0, prev = 0, mv = 0;

    if (beats < SPK_M_BEATS)
    {
        mood->energy = 50;
        mood->flux = 50;
        mood->trend = 0;
        return;
    }

    for (i = 0; i < SPK_M_BAR; i++)
    {
        e += energy[(at - 1 - i + SPK_M_BEATS) % SPK_M_BEATS];
        mv += moved[(at - 1 - i + SPK_M_BEATS) % SPK_M_BEATS];
        prev += energy[(at - 1 - SPK_M_BAR - i + SPK_M_BEATS) % SPK_M_BEATS];
    }

    e /= SPK_M_BAR;
    mv /= SPK_M_BAR;
    prev /= SPK_M_BAR;

    mood->energy = e * 100 / loud;
    mood->trend = (e - prev) * 100 / loud;

    /* Movement against the bar's own level, so a quiet busy passage reads as
     * busy. A bar whose beats swing by a third of its own height is as
     * moved as this needs to call anything. */
    mood->flux = e ? mv * 300 / e : 0;

    if (mood->flux > 100)
        mood->flux = 100;
}
