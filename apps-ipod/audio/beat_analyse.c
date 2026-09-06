/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Feeds beat_hops.c from the audio the DAC has not reached yet.
 *
 * The codec runs ahead of playback by however full the PCM buffer is, so the
 * two to three seconds sitting in it are the only audio the player has not
 * heard. Walking that region is what lets a caller act on a sound before it
 * is heard; everything here exists to hand those samples to the hop machine
 * without touching playback.
 *
 * Parts, in order:
 *   - the cursor: seeding it, and what invalidates it
 *   - start, stop, and the poll loop that feeds chunks from the buffer
 *   - read-out for the debug screen
 ****************************************************************************/

#include "config.h"
#include "audio/pcmbuf.h"
#include "audio/beat_analyse.h"

/* Chunks consumed per poll, and the cost of one call to this.
 *
 * A chunk is 2048 frames, which is eight hops of analysis. Audio arrives at
 * about 21 chunks a second and a caller drawing at 33fps polls 33 times, so
 * one chunk a poll would already keep up; this leaves six times that to
 * catch up with after a stall, and still keeps up at the slowest frame rate
 * the screen will drop to.
 *
 * The cap matters more for what it forbids than for what it allows. At eight
 * the worst poll did sixty-four hops in one go -- measured at fourteen times
 * the median, which on a screen drawing every 30ms is a missed frame. And a
 * caller that has just stalled is the worst possible one to hand that to:
 * catching up in a single call is what causes the next stall. Four bounds
 * the worst poll to four times the median and spreads the recovery over a
 * few frames, which is where it belongs. */
#define BEAT_CHUNKS_PER_POLL  4

static bool          running;

static size_t        cursor;
static unsigned int  cursor_key;    /* Track the timestamps belong to */

static unsigned int  stat_reseeds;
static unsigned int  stat_lead_ms;


/** The cursor **/

static void beat_reseed(void)
{
    cursor = pcmbuf_peek_start();
    cursor_key = pcmbuf_get_position_key();
    beat_hops_resync();
}


/** Entry points **/

void beat_analyse_start(void)
{
    stat_reseeds = 0;
    stat_lead_ms = 0;

    beat_hops_reset(pcmbuf_get_frequency());
    beat_reseed();
    running = true;
}

void beat_analyse_stop(void)
{
    running = false;
}

void beat_analyse_poll(void)
{
    struct pcmbuf_peek peek;
    int chunks;

    if (!running)
        return;

    beat_hops_set_rate(pcmbuf_get_frequency());

    /* Every Goertzel coefficient divides by this, and playback reports zero
     * until it has picked a rate. Committed audio without a rate should not
     * be reachable, but the fault if it were is a division by zero. */
    if (beat_hops_rate() == 0)
        return;

    if (pcmbuf_get_position_key() != cursor_key)
    {
        beat_reseed();
        stat_reseeds++;
    }

    for (chunks = 0; chunks < BEAT_CHUNKS_PER_POLL; chunks++)
    {
        if (!pcmbuf_peek_next(&cursor, &peek))
        {
            /* Caught up with the codec is the ordinary case; a cursor the
             * codec has lapped is not, and reading on from it would analyse
             * whatever has since been written over it. */
            if (!pcmbuf_peek_valid(cursor))
            {
                beat_reseed();
                stat_reseeds++;
            }
            break;
        }

        if (peek.pos_key != 0 && peek.pos_key == cursor_key)
            beat_hops_set_pos(peek.elapsed);

        beat_hops_feed(peek.pcm, peek.frames);
    }

    stat_lead_ms = pcmbuf_peek_lead_ms(cursor);
}


/** Read-out **/

void beat_analyse_status(struct beat_status *status)
{
    struct beat_bands bands;
    unsigned long newest_ms;
    int g;

    status->lookahead_ms = pcmbuf_lookahead_ms();
    status->lead_ms      = stat_lead_ms;

    /* The newest hop is one lead ahead of the DAC, so subtracting the lead
     * from its timestamp gives the track time being heard. Derived rather
     * than read from playback so it stays on the same clock as the hops it
     * is compared against. */
    newest_ms = beat_hops_newest_ms();
    status->play_ms = newest_ms > stat_lead_ms ? newest_ms - stat_lead_ms : 0;

    status->samplerate   = beat_hops_rate();
    status->windows      = beat_hops_windows();
    status->reseeds      = stat_reseeds;

    beat_hops_bands(&bands);
    for (g = 0; g < BEAT_GROUPS; g++)
    {
        status->flux[g]      = bands.flux[g];
        status->threshold[g] = bands.threshold[g];
        status->onsets[g]    = bands.onsets[g];
        status->rate10[g]    = bands.rate10[g];
    }
}

const struct beat_window *beat_analyse_window(int back)
{
    return beat_hops_window(back);
}
