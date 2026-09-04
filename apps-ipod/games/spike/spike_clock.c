/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The clock a grid is cut from.
 *
 * One clock, and it is the audio's. pcmbuf reports the track time of the
 * chunk now playing, which is exact but only moves once per chunk -- about
 * every 46ms. The tick counter carries the position between those reports
 * and each report steers it back toward the middle of the chunk it names,
 * so nothing cut from it can drift against what the player hears.
 *
 * Parts, in order:
 *   - the clock
 *   - the tempo
 ****************************************************************************/

#include "config.h"
#include "kernel.h"
#include "audio.h"            /* audio_status */
#include "audio.h"            /* audio_current_track */
#include "metadata.h"         /* struct mp3entry */
#include "games/spike/spike_clock.h"

/* One pcmbuf chunk, which is how often the clock is corrected and so the
 * longest the tick counter is ever asked to cover. */
#define SPK_CHUNK_MS         46

/* Margin on a forward move, over and above the wall time that has passed.
 *
 * Measured against the wall clock and not against a constant, because the
 * caller's own loop can be held up for seconds -- a disk spinning up, a
 * track's metadata being read -- and the report is then legitimately that
 * much further on. A fixed ceiling calls that a seek, and everything cut
 * from this clock starts again: on the first track of a session, which is
 * the one that stalls, a run would end the moment it began.
 *
 * Backwards is its own threshold, and a small one: playing never goes
 * backwards, but a rebuffer can resync the report a chunk or two behind
 * where it had got to. A track change goes back by a whole track. */
#define SPK_SEEK_MS          2000
#define SPK_BACK_MS          500

/* How long the report may stand still, while audio is playing, before the
 * clock stops believing it. Several chunks: a report that has not moved for
 * a third of a second is not a slow frame. */
#define SPK_STALL_MS         300

static long          clock_tick;    /* tick the clock was last carried to */
static unsigned long clock_ms;

/* The last report that was different from the one before, and when it
 * arrived. A report that stops moving while the music does not is what the
 * clamps below must not be allowed to believe. */
static unsigned long report_ms;
static long          report_tick;


/** The clock **/

static unsigned long spk_audio_ms(void)
{
    struct mp3entry *id3 = audio_current_track();

    /* mp3entry is live rather than a copy, and its elapsed is written from
     * the pcmbuf chunk callback -- which is exactly the chunked report this
     * clock is built around. */
    return id3 != NULL ? id3->elapsed : 0;
}

void spk_clock_reset(void)
{
    clock_tick = current_tick;
    clock_ms = spk_audio_ms() + SPK_CHUNK_MS / 2;
    report_ms = spk_audio_ms();
    report_tick = current_tick;
}

/* The clock is carried by the ticks between reports, so a caller that stops
 * calling it -- a pause splash, a menu -- leaves the whole of that time
 * waiting to be added on the next tick. It is added as one lump, and
 * everything cut from the clock reads it as the audio having moved: the
 * grid jumps by however long the menu was open, which is a run restarted
 * for having looked at it.
 *
 * The position itself needs no correction. The audio was stopped, so track
 * time did not move; only the two stamps that say when the clock was last
 * carried have to come forward. */
void spk_clock_resume(void)
{
    clock_tick = current_tick;
    report_ms = spk_audio_ms();
    report_tick = current_tick;
}

/* The report has stopped and the music has not.
 *
 * The clamps hold the clock within a chunk of what playback last said, which
 * is what makes it freeze on a pause -- exactly right there, and exactly
 * wrong when the report itself stalls. A track change can leave `elapsed`
 * standing still for seconds while the audio plays on, and the clock then
 * walks into the ceiling and stops within one beat of it. Everything cut
 * from the clock stops with it: no boundaries, a field frozen mid-stride,
 * and a screen that says it is waiting for a beat it can no longer hear. */
static bool spk_report_stalled(void)
{
    int status = audio_status();

    if (!(status & AUDIO_STATUS_PLAY) || (status & AUDIO_STATUS_PAUSE))
        return false;

    return (current_tick - report_tick) * (1000 / HZ) > SPK_STALL_MS;
}

/* The clock runs on ticks and is steered by audio, rather than being set
 * from audio and extrapolated forward.
 *
 * That distinction is the whole of it. A correction is only noticed on the
 * frame that happens to see it, by which time part of the chunk has already
 * played; restarting the extrapolation from the moment of noticing throws
 * that part away, and throws it away again at every correction. Done that
 * way the clock runs about a fifth slow.
 *
 * pcmbuf reports the chunk now playing, so the true position is somewhere
 * in the 46ms it covers, and a smooth clock's offset from that report
 * sweeps the whole of it -- which is why the correction is a pull toward
 * the middle rather than a clamp at the edges. Sitting on an edge would
 * drag the clock back every frame the report did not move and let it jump a
 * whole chunk on every frame it did: the same limp, from the other end.
 *
 * The clamps either side are for discontinuities only, and are what freezes
 * a field on a pause -- the report stops, the clock walks into the ceiling
 * within a chunk and stays there.
 *
 * What counts as the audio moving underneath is a jump in the *report*, and
 * not pcmbuf's position key. The key is bumped where the next track starts
 * being written, which is a whole buffer -- seconds -- before any of it is
 * heard, so a caller told by the key changes track while the old one is
 * still playing. The report is what everything here is cut from, so a
 * discontinuity in it is the event that invalidates the grid, and it
 * arrives at the speaker. In between, the report stands still and the
 * stall test above carries the clock through on ticks alone. */
bool spk_clock_tick(void)
{
    long now = current_tick;
    long elapsed = (now - clock_tick) * (1000 / HZ);
    unsigned long report = spk_audio_ms();
    long middle, error, slack;

    if (report != report_ms)
    {
        unsigned long was = report_ms;

        report_ms = report;
        report_tick = now;

        if (report + SPK_BACK_MS < was
            || report > was + (unsigned long)elapsed + SPK_SEEK_MS)
        {
            spk_clock_reset();
            return false;
        }
    }

    middle = (long)report + SPK_CHUNK_MS / 2;

    clock_ms += (unsigned long)elapsed;
    clock_tick = now;

    /* Free-running on the ticks alone until the report comes back. It is a
     * worse clock than a steered one and a far better one than a stopped
     * one, and the steering resumes the moment there is something to steer
     * against. */
    if (spk_report_stalled())
        return true;

    error = middle - (long)clock_ms;
    clock_ms = (unsigned long)((long)clock_ms + error / 8);

    /* How far from the report the clock may legitimately be: the chunk the
     * report does not resolve, plus the frame it has extrapolated across.
     * Fixing the slack at one chunk instead makes the clamps bite whenever
     * a frame is long, so a low frame rate would look like a broken clock. */
    slack = SPK_CHUNK_MS + elapsed;

    if ((long)clock_ms < middle - slack)
        clock_ms = (unsigned long)(middle - slack);
    else if ((long)clock_ms > middle + slack)
        clock_ms = (unsigned long)(middle + slack);

    return true;
}

unsigned long spk_clock_ms(void)
{
    return clock_ms;
}

unsigned long spk_clock_left_ms(void)
{
    struct mp3entry *id3 = audio_current_track();
    unsigned long length = id3 != NULL ? id3->length : 0;

    /* A stream, or a track whose length the metadata never gave: there is
     * no end to bow out before, so the run simply carries on. An hour rather
     * than the largest unsigned long, because callers do arithmetic on
     * this. */
    if (length == 0)
        return 3600000ul;

    return length > clock_ms ? length - clock_ms : 0;
}

unsigned long spk_clock_now(void)
{
    return clock_ms
           + (unsigned long)((current_tick - clock_tick) * (1000 / HZ));
}


/** The tempo **/

int spk_octave(unsigned int track_ms)
{
    static const signed char shift[] = { -2, -1, 0, 1 };
    int best = SPK_BEAT_TARGET, best_err = -1;
    unsigned int i;

    if (track_ms == 0)
        return SPK_BEAT_TARGET;

    for (i = 0; i < sizeof (shift) / sizeof (shift[0]); i++)
    {
        int ms = shift[i] < 0 ? (int)(track_ms >> -shift[i])
                              : (int)(track_ms << shift[i]);
        int err = ms > SPK_BEAT_TARGET ? ms - SPK_BEAT_TARGET
                                      : SPK_BEAT_TARGET - ms;

        if (best_err < 0 || err < best_err)
        {
            best_err = err;
            best = ms;
        }
    }

    /* ...and then a floor, because the nearest power of two is not always a
     * playable one. Nearest sends a 170 BPM track to its own beat -- a third
     * of a second a cell -- and, for the same reason from the other end, a 75
     * BPM track to double time at 400 ms. Both are the same fault: the target
     * is in the middle of the range and half of what lands near it lands on
     * the fast side of playable. Below the floor, take the beat above.
     *
     * Only a grid that was usable to begin with is slowed down. An estimate
     * the octave map could not bring into range is not a fast track, it is a
     * bad estimate, and doubling it would launder one into the other -- a 300
     * BPM reading would arrive as a perfectly reasonable 400 ms grid instead
     * of being rejected. */
    if (best >= SPK_BEAT_MIN && best < SPK_BEAT_FAST)
        best *= 2;

    return best;
}
