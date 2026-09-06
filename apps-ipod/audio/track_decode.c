/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Runs a codec over part of a file and throws the audio away.
 *
 * Playback's codec is driven by codec_thread.c through the global `ci`, whose
 * pcmbuf_insert() hands PCM to the buffer the DAC drains. Everything here is
 * that arrangement with the output replaced: a private copy of `ci` whose
 * callbacks read from a window onto the file and hand each block to a sink.
 * Nothing reaches the audio hardware and nothing is buffered for playback.
 *
 * Only one codec may be loaded at a time -- codecs.c keeps the handle and the
 * code buffer as module state -- so this requires playback to be stopped, and
 * is why the analysis it exists for is a modal screen rather than something
 * running alongside the music.
 *
 * Parts, in order:
 *   - the file window
 *   - the codec callbacks
 *   - running one track
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "file.h"
#include "system.h"
#include "codecs.h"
#include "dsp_core.h"
#include "dsp_sample_io.h"
#include "metadata.h"
#include "audio/codec_thread.h"
#include "audio/track_decode.h"

extern struct codec_api ci;   /* from codecs.c */

static int              fd;
static off_t            file_len;   /* Not "filesize": file.h has
                                       that as a macro */

static unsigned char   *window;
static size_t           window_sz;
static off_t            window_at;    /* File offset of window[0] */
static size_t           window_fill;  /* Valid bytes in it */

static struct mp3entry  id3;
static struct codec_api dci;
static struct track_pcm fmt;

static track_decode_sink sink;
static bool            (*want_abort)(void);
static bool            (*have_enough)(void);

static unsigned long    seek_to_ms;
static bool             seek_sent;
static unsigned long    stop_after_ms;  /* Track time the window ends at */
static unsigned long    elapsed_ms;
static unsigned long    frames_since_stamp;
static bool             done;
static bool             gave_up;
static bool             had_enough;

static struct track_decode_stats stats;


/** The file window **/

/* Refill the window so that it starts at 'pos'. False on a read that failed
 * or a position past the end. */
static bool window_fill_at(off_t pos)
{
    ssize_t n;

    if (pos < 0 || pos >= file_len)
        return false;

    if (lseek(fd, pos, SEEK_SET) != pos)
        return false;

    n = read(fd, window, window_sz);
    if (n <= 0)
        return false;

    stats.refills++;

    window_at = pos;
    window_fill = (size_t)n;

    return true;
}

/* Make the window hold 'want' bytes from the codec's position, re-centring it
 * on that position when it does not.
 *
 * Playback's own request_buffer is backed by the buffering system, which
 * guarantees a contiguous run of whatever was asked for. A window that merely
 * covers the position does not: near its end the run left is short, and a
 * codec asking for a frame gets a few bytes and reads that as the end of the
 * file. Moving the window is what makes a short answer mean "the file ended"
 * rather than "the window did". */
static bool window_cover(size_t want)
{
    off_t used = dci.curpos - window_at;

    /* Nothing can supply more than the window holds, and asking would refill
     * on every call for the whole of a large read. */
    if (want > window_sz)
        want = window_sz;

    if (used >= 0 && (size_t)used < window_fill)
    {
        if (window_fill - (size_t)used >= want)
            return true;

        /* Everything left in the file, if that is all there is, is a full
         * answer however short it looks. */
        if (window_at + (off_t)window_fill >= file_len)
            return true;
    }

    return window_fill_at(dci.curpos);
}

/* Bytes of the window available from the codec's position. */
static size_t window_avail(void)
{
    off_t used = dci.curpos - window_at;

    if (used < 0 || (size_t)used >= window_fill)
        return 0;

    return window_fill - (size_t)used;
}

static unsigned char *window_ptr(void)
{
    return window + (dci.curpos - window_at);
}


/** The codec callbacks **/

static size_t cb_read_filebuf(void *ptr, size_t size)
{
    size_t got = 0;

    while (size > 0)
    {
        size_t chunk;

        if (!window_cover(size))
            break;

        chunk = window_avail();
        if (chunk == 0)
            break;
        if (chunk > size)
            chunk = size;

        memcpy((unsigned char *)ptr + got, window_ptr(), chunk);
        dci.curpos += chunk;
        got += chunk;
        size -= chunk;
    }

    return got;
}

/* A codec asking for more than the window holds gets what is there. Every
 * codec copes with a short answer -- playback's own buffer gives them one at
 * every handle boundary -- so the window does not have to grow to meet the
 * request. */
static void *cb_request_buffer(size_t *realsize, size_t reqsize)
{
    stats.requests++;

    if (!window_cover(reqsize))
    {
        *realsize = 0;
        return NULL;
    }

    *realsize = window_avail();
    if (*realsize > reqsize)
        *realsize = reqsize;
    else if (*realsize < reqsize)
        stats.short_answers++;

    return *realsize ? window_ptr() : NULL;
}

static void cb_advance_buffer(size_t amount)
{
    dci.curpos += amount;
    id3.offset = dci.curpos;
}

static bool cb_seek_buffer(size_t newpos)
{
    if ((off_t)newpos > file_len)
        return false;

    dci.curpos = newpos;
    stats.seeks++;

    return true;
}

static void cb_seek_complete(void)
{
}

static void cb_set_offset(size_t value)
{
    id3.offset = value;
}

static void cb_set_elapsed(unsigned long value)
{
    elapsed_ms = value;
    frames_since_stamp = 0;
    id3.elapsed = value;

    if (value >= stop_after_ms)
        done = true;
}

/* The codec announces its output format here. Captured rather than passed to
 * a DSP: the shift is all that is needed to reach int16, and taking it this
 * way keeps the samples the codec's own.
 *
 * The scale follows dsp_sample_io.c -- a sample carries frac_bits fractional
 * bits, which is WORD_FRACBITS unless the codec announces a depth above
 * NATIVE_DEPTH, in which case it is that depth. */
static void cb_configure(int setting, intptr_t value)
{
    switch (setting)
    {
    case DSP_SET_FREQUENCY:
        fmt.frequency = value > 0 ? (unsigned int)value : fmt.frequency;
        break;

    case DSP_SET_SAMPLE_DEPTH:
    {
        int frac_bits = value <= NATIVE_DEPTH ? WORD_FRACBITS : (int)value;
        fmt.shift = frac_bits + 1 - NATIVE_DEPTH;
        break;
    }

    case DSP_SET_STEREO_MODE:
        fmt.stereo_mode = (int)value;
        break;

    default:
        break;
    }
}

static long cb_get_command(intptr_t *param)
{
    stats.commands++;

    /* Only where there is a clock to raise. Without the feature this is a
     * macro that expands to nothing, so it can be a statement and never an
     * rvalue. */
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    stats.boost = get_cpu_boost_counter();
#endif

    if (want_abort != NULL && want_abort())
    {
        gave_up = true;
        return CODEC_ACTION_HALT;
    }

    if (have_enough != NULL && have_enough())
    {
        had_enough = true;
        return CODEC_ACTION_HALT;
    }

    if (done)
        return CODEC_ACTION_HALT;

    /* One seek, before any audio is asked for. A codec that cannot honour it
     * simply carries on from the start, and the window then covers the
     * opening of the track instead -- worth having rather than nothing. */
    if (!seek_sent)
    {
        seek_sent = true;

        if (seek_to_ms > 0)
        {
            *param = (intptr_t)seek_to_ms;
            return CODEC_ACTION_SEEK_TIME;
        }
    }

    return CODEC_ACTION_NULL;
}

static bool cb_loop_track(void)
{
    return false;
}

static void cb_strip_filesize(off_t value)
{
    file_len = value;
    dci.filesize = value;
}

static void cb_pcmbuf_insert(const void *ch1, const void *ch2, int count)
{
    unsigned long track_ms = elapsed_ms;

    if (count <= 0 || done)
        return;

    if (fmt.frequency > 0)
        track_ms += (frames_since_stamp * 1000) / fmt.frequency;

    sink(ch1, ch2, count, &fmt, track_ms);
    frames_since_stamp += count;
}


/** Running one track **/

int track_decode_run(const char *path,
                     unsigned long start_ms, unsigned long length_ms,
                     void *buf, size_t bufsz,
                     track_decode_sink out,
                     bool (*enough)(void),
                     bool (*abort)(void),
                     unsigned long *analysed_ms)
{
    const char *codec_fn;
    int status;
    int rc = TRACK_DECODE_OK;

    if (analysed_ms != NULL)
        *analysed_ms = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return TRACK_DECODE_NO_FILE;

    memset(&id3, 0, sizeof (id3));
    if (!get_metadata(&id3, fd, path))
    {
        close(fd);
        return TRACK_DECODE_NO_CODEC;
    }

    codec_fn = get_codec_filename(id3.codectype);
    if (codec_fn == NULL)
    {
        close(fd);
        return TRACK_DECODE_NO_CODEC;
    }

    /* The whole file, not id3.filesize: for MP4 that is the sum of the mdat
     * chunks, and a file whose moov follows them keeps its sample tables
     * outside it -- so a codec parsing the container reads past the end of
     * its own audio and gets a short answer that reads as EOF. Playback hands
     * the codec buf_filesize() for the same reason. */
    file_len    = filesize(fd);
    if (file_len <= 0)
    {
        close(fd);
        return TRACK_DECODE_NO_FILE;
    }

    window      = buf;
    window_sz   = bufsz;
    window_at   = 0;
    window_fill = 0;

    sink        = out;
    want_abort  = abort;
    have_enough = enough;
    memset(&stats, 0, sizeof (stats));

    seek_to_ms  = start_ms;
    seek_sent   = false;
    elapsed_ms  = 0;
    frames_since_stamp = 0;
    done        = false;
    gave_up     = false;
    had_enough  = false;

    /* Past the end of the track is not a window; the caller gets whatever
     * the file holds instead of nothing. */
    stop_after_ms = start_ms + length_ms;
    if (stop_after_ms < start_ms)
        stop_after_ms = (unsigned long)-1;   /* Asked for more than fits */

    fmt.frequency   = id3.frequency;
    fmt.stereo_mode = STEREO_NONINTERLEAVED;
    fmt.shift       = WORD_FRACBITS + 1 - NATIVE_DEPTH;

    /* Everything the codec needs from the kernel and from string.h is
     * already wired into playback's own api struct, so start from a copy of
     * it and replace only what decides where the bytes come from and where
     * the audio goes. The dsp is cleared rather than carried across: nothing
     * here configures one, and a codec that reached for it would otherwise
     * reach playback's. */
    dci = ci;
    dci.dsp              = NULL;
    dci.id3              = &id3;
    dci.filesize         = file_len;
    dci.curpos           = 0;
    dci.audio_hid        = -1;
    dci.codec_get_buffer = codec_get_buffer_callback;
    dci.pcmbuf_insert    = cb_pcmbuf_insert;
    dci.set_elapsed      = cb_set_elapsed;
    dci.read_filebuf     = cb_read_filebuf;
    dci.request_buffer   = cb_request_buffer;
    dci.advance_buffer   = cb_advance_buffer;
    dci.seek_buffer      = cb_seek_buffer;
    dci.seek_complete    = cb_seek_complete;
    dci.set_offset       = cb_set_offset;
    dci.configure        = cb_configure;
    dci.get_command      = cb_get_command;
    dci.loop_track       = cb_loop_track;
    dci.strip_filesize   = cb_strip_filesize;

    /* Take the codec slot properly. audio_stop() calls codec_stop(), which
     * by its own comment leaves the codec *resident*: codecs.c still holds
     * the handle and codec_thread.c still names the format. Loading over that
     * strands playback's codec -- our codec_close() then nulls the handle
     * while codec_thread still believes one is loaded, and the next track
     * finds a codec that is registered and gone.
     *
     * codec_unload() is the call that clears both halves. */
    if (codec_loaded() != AFMT_UNKNOWN)
        codec_unload();

    if (codec_load_file(codec_fn, &dci) < 0)
    {
        close(fd);
        return TRACK_DECODE_NO_CODEC;
    }

    /* codec_thread.c boosts before loading a codec and this path does not
     * go through it. Unboosted, a FLAC decode plus the analysis runs at real
     * time on the 5G, which makes a library scan take as long as playing the
     * library. */
    cpu_boost(true);
    status = codec_run_proc();
    cpu_boost(false);

    codec_close();
    close(fd);

    stats.codec_status = status;
    stats.last_pos = (unsigned long)dci.curpos;
    stats.file_len = (unsigned long)file_len;

    if (gave_up)
        rc = TRACK_DECODE_ABORTED;
    else if (status != CODEC_OK && !done && !had_enough)
        rc = TRACK_DECODE_FAILED;

    if (analysed_ms != NULL && elapsed_ms > start_ms)
        *analysed_ms = elapsed_ms - start_ms;

    return rc;
}

void track_decode_get_stats(struct track_decode_stats *out)
{
    *out = stats;
}
