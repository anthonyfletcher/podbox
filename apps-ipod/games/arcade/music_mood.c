/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The envelope, gathered into beats and bars. See music_mood.h for what
 * this is instead of, and why.
 ****************************************************************************/

#include "config.h"
#include "pcm.h"
#include "pcm_mixer.h"
#include "games/arcade/music_mood.h"

/* Beats kept: two bars, so this bar can be held against the one before. */
#define MUSIC_M_BEATS      8
#define MUSIC_M_BAR        4

/* Peaks are 16-bit magnitudes; this brings them into a range where a bar's
 * worth still fits a long several times over. */
#define MUSIC_M_SHIFT      5
#define MUSIC_M_FULL       (32767 >> MUSIC_M_SHIFT)

static int  energy[MUSIC_M_BEATS];
static int  moved[MUSIC_M_BEATS];
static int  at;
static int  beats;

static int  last;               /* the beat before, for the movement */
static bool have_last;

/* The loudest the track has been, decaying about a percent and a half a beat
 * so a track that opens loud and then stays quiet is not read as quiet for
 * ever. Energy is reported against this rather than against full scale:
 * what a cart asks is one passage against another. */
static int  loud;

void music_mood_reset(void)
{
    int i;

    for (i = 0; i < MUSIC_M_BEATS; i++)
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
 * so a slow stretch reads louder.
 *
 * Movement is then between beats rather than between frames, which is the
 * same idea at the resolution that is left: a bar whose beats differ from
 * one another is a busy bar. */
void music_mood_beat(void)
{
    static struct pcm_peaks peaks;
    int e;

    mixer_channel_calculate_peaks(PCM_MIXER_CHAN_PLAYBACK, &peaks);

    e = (int)((peaks.left > peaks.right ? peaks.left : peaks.right)
              >> MUSIC_M_SHIFT);

    if (e > MUSIC_M_FULL)
        e = MUSIC_M_FULL;

    energy[at] = e;
    moved[at] = have_last ? (e > last ? e - last : last - e) : 0;
    last = e;
    have_last = true;

    at = (at + 1) % MUSIC_M_BEATS;

    if (beats < MUSIC_M_BEATS)
        beats++;

    if (e > loud)
        loud = e;
    else
        loud -= loud >> 6;

    if (loud < 1)
        loud = 1;
}

void music_mood_get(struct music_mood *mood)
{
    int i, e = 0, prev = 0, mv = 0;

    if (beats < MUSIC_M_BEATS)
    {
        mood->energy = 50;
        mood->flux = 50;
        mood->trend = 0;
        return;
    }

    for (i = 0; i < MUSIC_M_BAR; i++)
    {
        e += energy[(at - 1 - i + MUSIC_M_BEATS) % MUSIC_M_BEATS];
        mv += moved[(at - 1 - i + MUSIC_M_BEATS) % MUSIC_M_BEATS];
        prev += energy[(at - 1 - MUSIC_M_BAR - i + MUSIC_M_BEATS) % MUSIC_M_BEATS];
    }

    e /= MUSIC_M_BAR;
    mv /= MUSIC_M_BAR;
    prev /= MUSIC_M_BAR;

    mood->energy = e * 100 / loud;
    mood->trend = (e - prev) * 100 / loud;

    /* Movement against the bar's own level, so a quiet busy passage reads as
     * busy. A bar whose beats swing by a third of its own height is as
     * moved as this needs to call anything. */
    mood->flux = e ? mv * 300 / e : 0;

    if (mood->flux > 100)
        mood->flux = 100;
}
