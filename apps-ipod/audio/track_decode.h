/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to track_decode.c: decode a window of one file without playing
 * it.
 ****************************************************************************/

#ifndef TRACK_DECODE_H
#define TRACK_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the codec is producing.
 *
 * The samples a sink is handed are the codec's own, untouched by the DSP: no
 * EQ, no ReplayGain, no resampling, no dither. A measurement taken from them
 * describes the file. One taken after the DSP would describe the file as this
 * player's settings happen to render it, and would change when they did --
 * which for a number written down and kept is the wrong answer. */
struct track_pcm
{
    unsigned int frequency;    /* The codec's own rate */
    int          stereo_mode;  /* enum dsp_stereo_modes */
    int          shift;        /* >> this brings a sample to int16 */
};

/* One block of the codec's output, 'count' frames of it. Channel layout
 * follows fmt->stereo_mode: ch2 is the right channel when non-interleaved and
 * unused otherwise.
 *
 * 'track_ms' is where the first frame of the block sits in the track. Codecs
 * report their position only now and then, so it is that report carried
 * forward by the frames since -- which is what lets a sink place the block on
 * the track's own timeline instead of on its own. */
typedef void (*track_decode_sink)(const void *ch1, const void *ch2,
                                  int count, const struct track_pcm *fmt,
                                  unsigned long track_ms);

/* What the run did, for working out why it stopped where it did. */
struct track_decode_stats
{
    int          codec_status;   /* What the codec's run loop returned */
    unsigned int requests;       /* request_buffer calls */
    unsigned int short_answers;  /* ...that could not be met in full */
    unsigned int refills;        /* Window reads */
    unsigned int seeks;          /* seek_buffer calls */
    unsigned long last_pos;      /* Where the codec had got to */
    unsigned long file_len;
    int          boost;          /* Boost counter seen during the run, so a
                                    run that was not boosted says so rather
                                    than being argued about */
    unsigned int commands;       /* get_command calls */
};

void track_decode_get_stats(struct track_decode_stats *out);

#define TRACK_DECODE_OK         0
#define TRACK_DECODE_NO_FILE   -1
#define TRACK_DECODE_NO_CODEC  -2
#define TRACK_DECODE_FAILED    -3
#define TRACK_DECODE_ABORTED   -4

/* Decode 'path' from 'start_ms', for at most 'length_ms' of audio, handing
 * every block to 'sink'.
 *
 * Runs on the calling thread and does not return until the window is done,
 * the file ends, 'enough' says it has what it came for, or 'abort' says to
 * stop. Playback must be stopped first: one codec may be loaded at a time and
 * this loads it.
 *
 * The two callbacks are separate because they mean opposite things. 'enough'
 * is success arriving early -- a measurement that has settled needs no more
 * audio -- and returns TRACK_DECODE_OK. 'abort' is the run being abandoned
 * and returns TRACK_DECODE_ABORTED. Both are polled from inside the codec,
 * often enough for a screen to stay responsive.
 *
 * The buffer is the caller's window onto the file, refilled as the codec
 * seeks. Bigger is fewer reads.
 *
 * 'enough', 'abort' and 'analysed_ms' may all be NULL. *analysed_ms comes back as the
 * audio actually covered, which is short of length_ms on a file that ended
 * first -- the caller needs it to know whether the window it asked for is the
 * window it got.
 *
 * Returns TRACK_DECODE_OK or one of the errors above. */
int track_decode_run(const char *path,
                     unsigned long start_ms, unsigned long length_ms,
                     void *buf, size_t bufsz,
                     track_decode_sink sink,
                     bool (*enough)(void),
                     bool (*abort)(void),
                     unsigned long *analysed_ms);

#endif /* TRACK_DECODE_H */
