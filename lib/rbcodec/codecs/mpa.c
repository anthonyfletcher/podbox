/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 Dave Chapman
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "codeclib.h"
#include "libasf/asf.h"
#include <codecs/libmad/mad.h>
#include <inttypes.h>

CODEC_HEADER

/* MP3 decodes on a thread of its own on the COP rather than through the
   other codecs' run there (codec_cop.c), which it is not given */
#if NUM_CORES > 1 && !defined(MPEGPLAYER)
#define DECODE_THREAD
#endif

static struct mad_stream stream IBSS_ATTR;
static struct mad_frame frame IBSS_ATTR;
static struct mad_synth synth IBSS_ATTR;

#define INPUT_CHUNK_SIZE   8192

static mad_fixed_t mad_frame_overlap[2][32][18] IBSS_ATTR;
static mad_fixed_t sbsample[2][36][32] IBSS_ATTR;

static unsigned char mad_main_data[MAD_BUFFER_MDLEN] IBSS_ATTR;
/* TODO: what latency does layer 1 have? */
static int mpeg_latency[3] = { 0, 481, 529 };
static int mpeg_framesize[3] = {384, 1152, 1152};

static unsigned char stream_buffer[INPUT_CHUNK_SIZE] IBSS_ATTR;
static unsigned char *stream_data_start;
static unsigned char *stream_data_end;
static unsigned char *packetdata;
static int stream_data_available;
static int packetlength;
static int packetdatasize;
static int packetrest;
static unsigned char *lastpacketpos;

/* The decoding thread is RockWheel's (github.com/ssarkisy/RockWheel,
 * lib/rbcodec/codecs/mpa.c), GPL v2 or later.
 *
 * With two cores all of the decoding is on the COP, which goes through a window
 * of the input on its own, into a queue that the CPU empties: the CPU, which has
 * the UI to draw too, is left with the DSP. A frame of the UI takes tens of ms
 * during which the CPU can't take anything, threads only switching on a yield,
 * so the COP has to be able to go on without it.
 *
 * The input and the PCM of the queue are only reached by their uncached
 * addresses: no core has to sync its cache per frame, which also leaves the
 * cache of the CPU as the UI and the DSP had it. The DSP works in place, so the
 * CPU copies a frame out of the queue first.
 *
 * Each frame is kept back until the next one is decoded, as it's only known then
 * whether it's the last one, which gets its end cut. */
#ifdef DECODE_THREAD
#define DECODE_QUEUE    8
#define WINDOW_BYTES    (24*1024)
#else
#define DECODE_QUEUE    2
#define WINDOW_BYTES    INPUT_CHUNK_SIZE
#endif

/* shared by the two cores only with the thread */
#if defined(UNCACHED_ADDR) && defined(DECODE_THREAD)
#define SHARED_ADDR(p)  UNCACHED_ADDR(p)
#else
#define SHARED_ADDR(p)  (p)
#endif

struct decoded_frame {
    int length;              /* samples per channel */
    int channels;
    unsigned int samplerate;
    size_t bytes;            /* of the input, anything skipped before it too */
};

static struct {
    size_t size;             /* of the window, 0 if there's none */
    size_t read;             /* of the window, up to the end of the last frame */
    int made;                /* frames from the window */
    bool over;               /* the window is finished */
    bool failed;             /* by an error that can't be recovered from */
    volatile unsigned int head;
    volatile unsigned int tail;
    volatile bool running;
    volatile bool stop;
    volatile bool die;
    struct decoded_frame decoded[DECODE_QUEUE];
#ifdef DECODE_THREAD
    struct semaphore pending;
    struct semaphore ready;
    struct semaphore room;
#endif
} job IBSS_ATTR;

static mad_fixed_t decoded_pcm[DECODE_QUEUE][2][1152] MEM_ALIGN_ATTR;
#ifndef DECODE_THREAD
/* the queue's frame goes to pcmbuf_insert() as it is */
#elif CONFIG_CPU == PP5020
/* The PP5020 has 48 KB of IRAM for a codec, not the PP5022's 80: the frame
   the DSP works on goes to DRAM, as only the CPU reaches it. stream_buffer,
   which only MP3 in ASF fills, could go instead, with the COP reading it
   uncached: which costs less is to be measured */
static mad_fixed_t dsp_pcm[2][1152] MEM_ALIGN_ATTR;
#else
static mad_fixed_t dsp_pcm[2][1152] IBSS_ATTR MEM_ALIGN_ATTR;
#endif

static inline mad_fixed_t *decoded_samples(unsigned int n, int ch)
{
    mad_fixed_t *samples = decoded_pcm[n % DECODE_QUEUE][ch];
    return SHARED_ADDR(samples);
}

static void reset_stream_buffer(void)
{
    stream_data_start = stream_buffer;
    stream_data_end = stream_buffer;
    stream_data_available = 1;
    packetdatasize = 0;
    packetrest = 0;
    lastpacketpos = stream_buffer;
}

static inline unsigned char *get_stream_data(size_t *realsize, size_t reqsize)
{
    static int errcount = 0;
    size_t datasize = stream_data_end - stream_data_start;
    if (!ci->id3->is_asf_stream)
        return ci->request_buffer(realsize, reqsize);
    else if (datasize < INPUT_CHUNK_SIZE / 2)
    {
        if (stream_data_start < stream_data_end && stream_data_start > stream_buffer)
        {
            lastpacketpos -= stream_data_start - stream_buffer;
            memmove(stream_buffer, stream_data_start, datasize);
            stream_data_start = stream_buffer;
            stream_data_end = stream_buffer + datasize;
        }
        while (datasize < INPUT_CHUNK_SIZE && (packetrest || stream_data_available > 0))
        {
            if (packetrest && packetdata)
            {
                datasize = INPUT_CHUNK_SIZE - datasize;
                if (datasize > (size_t)packetrest)
                    datasize = packetrest;
                memcpy(stream_data_end, packetdata, datasize);
                packetrest -= datasize;
                stream_data_end += datasize;
                if (packetrest)
                    packetdata += datasize;
                else
                {
                    ci->advance_buffer(packetlength);
                    lastpacketpos = stream_data_end;
                }
                datasize = stream_data_end - stream_data_start;
            }
            else
            {
                asf_waveformatex_t *wfx = (asf_waveformatex_t *)(ci->id3->toc);
                int res = asf_read_packet(&packetdata, &packetdatasize, &packetlength, wfx);
                if (res < 0)
                {
                    if (res == ASF_ERROR_EOF)
                        stream_data_available = 0;
                    else if (++errcount > 5)
                        stream_data_available = -1;
                }
                else errcount = 0;
                packetrest = packetdatasize;
            }
        }
    }
    if (packetdatasize && lastpacketpos > stream_data_start)
      ci->set_offset(ci->curpos - ((lastpacketpos - stream_data_start) / packetdatasize + 1) * packetlength);
    *realsize = (datasize > reqsize) ? reqsize : datasize;
    return stream_data_start;
}

static void advance_stream_buffer(size_t size)
{
    if (!ci->id3->is_asf_stream)
        ci->advance_buffer(size);
    else if (stream_data_start + size > stream_data_end)
        stream_data_start = stream_data_end;
    else stream_data_start += size;
}

static void init_mad(void)
{
    ci->memset(&stream, 0, sizeof(struct mad_stream));
    ci->memset(&frame , 0, sizeof(struct mad_frame));
    ci->memset(&synth , 0, sizeof(struct mad_synth));

    frame.sbsample_prev = &sbsample;
    frame.sbsample      = &sbsample;

    /* We do this so libmad doesn't try to call codec_calloc(). This needs to
     * be called before mad_stream_init(), mad_frame_inti() and 
     * mad_synth_init(). */
    frame.overlap    = &mad_frame_overlap;
    stream.main_data = &mad_main_data;
    
    /* Call mad initialization. Those will zero the arrays frame.overlap,
     * frame.sbsample and frame.sbsample_prev. Therefore there is no need to 
     * zero them here. */
    mad_stream_init(&stream);
    mad_frame_init(&frame);
    mad_synth_init(&synth);
}

static int get_file_pos(int newtime)
{
    int pos = -1;
    struct mp3entry *id3 = ci->id3;

    if (id3->vbr) {
        int curpos = ci->curpos - id3->first_frame_offset;
        long skipms_from_curpos = curpos > 0 ?  (long) (id3->elapsed - newtime) : - newtime;
        if (id3->has_toc) {
            /* Use the TOC to find the new position */
            unsigned int percent = ((uint64_t)newtime * 100) / id3->length;
            if (percent > 99)
                percent = 99;

            unsigned int pct_timestep = id3->length / 100;

            /* Interpolate between this TOC mark and the next TOC mark */
            int skipms_from_toc = newtime - percent * pct_timestep;
            if (skipms_from_toc < abs(skipms_from_curpos)) 
            {
                unsigned int toc_sizestep = id3->filesize / 256;
                unsigned int cur_toc = id3->toc[percent];
                unsigned int next_toc = percent < 99 ? id3->toc[percent+1] : 256;
                unsigned int plength = (next_toc - cur_toc) * toc_sizestep;

                pos = cur_toc * toc_sizestep + (uint64_t) plength * skipms_from_toc / pct_timestep;
            }
        } else if (newtime < abs(skipms_from_curpos)) {
            /* No TOC exists, estimate the new position */
            pos = (uint64_t)newtime * id3->filesize / id3->length;
        }
        // VBR seek might be very inaccurate in long files 
        // So make sure that seeking actually happened in the intended direction 
        // Fix jumps in the wrong direction by seeking relative to the current position
        if (pos == -1 || (skipms_from_curpos >= 0 && pos > curpos) || (skipms_from_curpos < 0 && pos < curpos))
        {
            pos = curpos - skipms_from_curpos * (id3->filesize / id3->length);
            //LOGF("curpos relative seek: %d, curpos: %d, newtime: %d", pos, curpos, newtime);
        }
    } else if (id3->bitrate) {
        pos = newtime * (id3->bitrate / 8);
    } else {
        return -1;
    }

    /* Don't seek right to the end of the file so that we can
       transition properly to the next song */
    if (pos >= (int)(id3->filesize - id3->id3v1len))
        pos = id3->filesize - id3->id3v1len - 1;

    /* id3->filesize excludes id3->first_frame_offset, so add it now */
    pos += id3->first_frame_offset;

    return pos;
}

static void set_elapsed(struct mp3entry* id3)
{
    unsigned long offset = id3->offset > id3->first_frame_offset ?
        id3->offset - id3->first_frame_offset : 0;
    unsigned long elapsed = id3->elapsed;

    if ( id3->vbr ) {
        if ( id3->has_toc ) {
            unsigned int pct_timestep = id3->length / 100;
            unsigned int toc_sizestep = id3->filesize / 256;

            int percent;
            for (percent = 0; percent < 100; ++percent)
                if (offset < id3->toc[percent] * toc_sizestep)
                    break;
            if (percent > 0)
                --percent;

            unsigned int cur_toc  = id3->toc[percent];
            unsigned int next_toc = percent < 99 ? id3->toc[percent+1] : 256;
            unsigned int plength = (next_toc - cur_toc) * toc_sizestep;

            /* Set elapsed time to the TOC mark */
            elapsed = percent * pct_timestep;

            /* Interpolate between this TOC mark and the next TOC mark */
            offset -= cur_toc * toc_sizestep;
            elapsed += (uint64_t)pct_timestep * offset / plength;
        } else {
            /* No TOC, use an approximation (this'll be wildly inaccurate) */
            uint64_t data_size = id3->filesize -
                                 id3->first_frame_offset - id3->id3v1len;
            elapsed = (uint64_t)id3->length * offset / data_size;
        }
    }
    else
    {
        /* constant bitrate, use exact calculation */
        if (id3->bitrate != 0)
            elapsed = offset / (id3->bitrate / 8);
    }

    ci->set_elapsed(elapsed);
}

/* The next frame of the window, or what ends it: true if a frame was made */
static bool decode_step(void)
{
    if (mad_frame_decode(&frame, &stream)) {
        if (stream.error == MAD_ERROR_BUFLEN) {
            job.over = true;
        } else if (!MAD_RECOVERABLE(stream.error)) {
            /* Some other unrecoverable error */
            job.failed = true;
            job.over = true;
        }
        /* Otherwise probably syncing after a seek */
        return false;
    }

    mad_synth_frame(&synth, &frame);

    const size_t read = stream.next_frame - stream.buffer;
    struct decoded_frame *f = &job.decoded[job.head % DECODE_QUEUE];
    f->length = synth.pcm.length;
    f->channels = MAD_NCHANNELS(&frame.header);
    f->samplerate = frame.header.samplerate;
    f->bytes = read - job.read;
    job.read = read;
    job.made++;

    /* In case of a mono file, the second channel is ignored. */
    for (int ch = 0; ch < f->channels; ch++) {
        memcpy(decoded_samples(job.head, ch), synth.pcm.samples[ch],
               f->length * sizeof (mad_fixed_t));
    }

    return true;
}

/* No window in progress and an empty queue, the COP being idle */
static void decode_clear(void)
{
    job.running = false;
    job.head = job.tail = 0;
    job.size = 0;
    job.read = 0;
    job.made = 0;
    job.over = false;
    job.failed = false;
}

#ifdef DECODE_THREAD
/* a line of the cache of its own, as the COP writes it and the CPU what is
   beside it */
static long decode_stack[CACHEALIGN_UP((DEFAULT_STACK_SIZE + 0x2000) / sizeof(long))]
    CACHEALIGN_ATTR;
static unsigned int decode_thread_id;

static void decode_thread(void)
{
    while (1) {
        ci->semaphore_wait(&job.pending, TIMEOUT_BLOCK);

        if (job.die)
            break;

        while (!job.over && !job.stop) {
            if (job.head - job.tail == DECODE_QUEUE) {
                ci->semaphore_wait(&job.room, TIMEOUT_BLOCK);
                continue;
            }

            if (decode_step()) {
                job.head++;
                ci->semaphore_release(&job.ready);
            }
            /* a frame at a time, for the COP's other threads */
            ci->yield();
        }

        job.running = false;
        ci->semaphore_release(&job.ready);
    }
}

static bool decode_thread_create(void)
{
    ci->semaphore_init(&job.pending, 1, 0);
    ci->semaphore_init(&job.ready, 1, 0);
    ci->semaphore_init(&job.room, 1, 0);

    /* The CPU could have the memory of the queue in its cache from when the
       codec was loaded, to be written over frames some time later */
    ci->commit_discard_dcache();

    decode_thread_id = ci->create_thread(decode_thread, decode_stack,
                                         sizeof(decode_stack), 0, "mp3dec"
                                         IF_PRIO(, PRIORITY_PLAYBACK)
                                         IF_COP(, COP));
    return decode_thread_id != 0;
}

static void decode_thread_quit(void)
{
    /* mop up COP thread */
    job.die = true;
    ci->semaphore_release(&job.pending);
    ci->thread_wait(decode_thread_id);
    ci->commit_discard_dcache();
}

/* The window is to be decoded: what it holds may still be in the CPU's cache */
static void decode_run(void)
{
    ci->commit_dcache();
    ci->semaphore_release(&job.pending);
}

/* For two frames, or for fewer if the window ends first */
static void decode_wait(void)
{
    while (job.head - job.tail < 2 && job.running)
        ci->semaphore_wait(&job.ready, TIMEOUT_BLOCK);
}

static void decode_taken(void)
{
    job.tail++;
    ci->semaphore_release(&job.room);
}

static void decode_stop(void)
{
    job.stop = true;
    ci->semaphore_release(&job.room);

    while (job.running)
        ci->semaphore_wait(&job.ready, TIMEOUT_BLOCK);

    job.stop = false;
    decode_clear();
}

/* For a new file: the last one could have ended at any point */
static void decode_reset(void)
{
    decode_stop();
    ci->semaphore_init(&job.ready, 1, 0);
    ci->semaphore_init(&job.room, 1, 0);
}
#else
static inline bool decode_thread_create(void)
{
    return true;
}

static inline void decode_thread_quit(void)
{
}

static inline void decode_run(void)
{
}

static void decode_wait(void)
{
    while (job.head - job.tail < 2 && job.running) {
        if (decode_step())
            job.head++;
        if (job.over)
            job.running = false;
    }
}

static inline void decode_taken(void)
{
    job.tail++;
}

static inline void decode_stop(void)
{
    decode_clear();
}

static inline void decode_reset(void)
{
    decode_clear();
}
#endif /* DECODE_THREAD */

/* this is the codec entry point */
enum codec_status codec_main(enum codec_entry_call_reason reason)
{
    if (reason == CODEC_LOAD) {
        /* Create a decoder instance */
        if (codec_init())
            return CODEC_ERROR;

        ci->configure(DSP_SET_SAMPLE_DEPTH, MAD_F_FRACBITS);

        /* does nothing on 1 processor systems except return true */
        if(!decode_thread_create())
            return CODEC_ERROR;
    }
    else if (reason == CODEC_UNLOAD) {
        /* mop up COP thread - MT only */
        decode_thread_quit();
    }

    return CODEC_OK;
}

bool seek_by_time(int64_t* samplesdone, unsigned long current_frequency, unsigned long elapsed_ms)
{
    if (ci->id3->is_asf_stream) {
        asf_waveformatex_t *wfx = (asf_waveformatex_t *)(ci->id3->toc);
        int elapsedtime = asf_seek(elapsed_ms, wfx);

        *samplesdone = (elapsedtime > 0) ?
                       (((int64_t)elapsedtime)*current_frequency/1000) : 0;

        if (elapsedtime < 1) {
            ci->set_elapsed(0);
            return false;
        } else {
            ci->set_elapsed(elapsedtime);
            reset_stream_buffer();
        }
    } else {
        int newpos = elapsed_ms ? get_file_pos(elapsed_ms) : (int)(ci->id3->first_frame_offset);

        *samplesdone = ((int64_t)elapsed_ms) * current_frequency / 1000;

        if (!ci->seek_buffer(newpos))
            return false;

        ci->set_elapsed((*samplesdone * 1000LL) / current_frequency);
    }
    return true;
}

/* this is called for each file to process */
enum codec_status codec_run(void)
{
    size_t size;
    int file_end;
    int samples_to_skip; /* samples to skip in total for this file (at start) */
    char *inputbuffer;
    int64_t samplesdone;
    int stop_skip, start_skip;
    int current_stereo_mode = -1;
    unsigned long current_frequency = 0;
    int padding = MAD_BUFFER_GUARD; /* to help mad decode the last frame */
    intptr_t param;
    bool input_over = false;

    /* The COP has to be idle for this */
    decode_reset();

    /* Reinitializing seems to be necessary to avoid playback quircks when seeking. */
    init_mad();

    file_end = 0;

    ci->configure(DSP_SET_FREQUENCY, ci->id3->frequency);
    current_frequency = ci->id3->frequency;
    codec_set_replaygain(ci->id3);
    if (!ci->id3->is_asf_stream)
    {
        // End of file might contain ID3v1 or APE tags. Strip them from decoding
        ci->strip_filesize(ci->id3->first_frame_offset + ci->id3->filesize);
    }

     if (ci->id3->offset) {

        if (ci->id3->is_asf_stream) {
            asf_waveformatex_t *wfx = (asf_waveformatex_t *)(ci->id3->toc);
            int packet_offset = ((ci->id3->offset > ci->id3->first_frame_offset) ?
                                 (ci->id3->offset - ci->id3->first_frame_offset) : 0)
              % wfx->packet_size;
            ci->seek_buffer(ci->id3->offset - packet_offset);
            ci->id3->elapsed = asf_get_timestamp(&packet_offset);
            ci->set_elapsed(ci->id3->elapsed);
        }
        else {
            ci->seek_buffer(ci->id3->offset);
            if (ci->id3->elapsed && ci->id3->elapsed < ci->id3->length)
            {
                ci->set_elapsed(ci->id3->elapsed);
            }
            else
            {
                set_elapsed(ci->id3);
            }
        }
    }
    else if (ci->id3->elapsed)
         /* Have elapsed time but not offset */
        seek_by_time(&samplesdone, current_frequency, ci->id3->elapsed);
    else
        ci->seek_buffer(ci->id3->first_frame_offset);

    if (ci->id3->lead_trim >= 0 && ci->id3->tail_trim >= 0) {
        stop_skip = ci->id3->tail_trim - mpeg_latency[ci->id3->layer];
        if (stop_skip < 0) stop_skip = 0;
        start_skip = ci->id3->lead_trim + mpeg_latency[ci->id3->layer];
    } else {
        stop_skip = 0;
        /* We want to skip this amount anyway */
        start_skip = mpeg_latency[ci->id3->layer];
    }

    /* Libmad will not decode the last frame without 8 bytes of extra padding
       in the buffer. So, we can trick libmad into not decoding the last frame
       if we are to skip it entirely and then cut the appropriate samples from
       final frame that we did decode. Note, if all tags (ID3, APE) are not
       properly stripped from the end of the file, this trick will not work. */
    if (stop_skip >= mpeg_framesize[ci->id3->layer]) {
        padding = 0;
        stop_skip -= mpeg_framesize[ci->id3->layer];
    } else {
        padding = MAD_BUFFER_GUARD;
    }

    samplesdone = ((int64_t)ci->id3->elapsed) * current_frequency / 1000;

    /* Don't skip any samples unless we start at the beginning. */
    if (samplesdone > 0)
        samples_to_skip = 0;
    else
        samples_to_skip = start_skip;

    if (ci->id3->is_asf_stream)
        reset_stream_buffer();

    /* This is the decoding loop. */
    while (1) {
        long action = ci->get_command(&param);

        if (action == CODEC_ACTION_HALT)
            break;

        if (action == CODEC_ACTION_SEEK_TIME) {
            /*make sure the COP is idle before seeking - MT only*/
            decode_stop();

            if (param == 0) {
                samples_to_skip = start_skip;
            } else {
                samples_to_skip = 0;
            }

            bool success = seek_by_time(&samplesdone, current_frequency, param);
            ci->seek_complete();
            if (!success)
                break;

            init_mad();
            input_over = false;
        }

        /* The COP is finished with a window: it can go on with the next one
           while the queue is still being emptied */
        if (!job.running && !input_over) {
            if (job.failed)
                return CODEC_ERROR;

            if (job.size > 0) {
                /* Past all the frames, and anything after them that's not
                   going to be one */
                for (unsigned int n = job.tail; n != job.head; n++) {
                    struct decoded_frame *f = &job.decoded[n % DECODE_QUEUE];
                    advance_stream_buffer(f->bytes);
                    f->bytes = 0;
                }

                size_t read = job.size;
                if (stream.next_frame)
                    read = MIN((size_t)(stream.next_frame - stream.buffer), read);

                const size_t rest = read > job.read ? read - job.read : 0;
                advance_stream_buffer(rest);

                /* This makes the codec support partially corrupted files */
                if (job.made > 0)
                    file_end = 0;
                else if (rest == 0 || ++file_end == 30)
                    input_over = true;
            }

            if (!input_over) {
                /* Lock buffers */
                inputbuffer = get_stream_data(&size, WINDOW_BYTES);
                if (size == 0 || inputbuffer == NULL) {
                    if (ci->id3->is_asf_stream && stream_data_available < 0)
                        return CODEC_ERROR;
                    input_over = true;
                } else {
                    /* ASF's data was already put in IRAM */
                    mad_stream_buffer(&stream, (unsigned char *)
                                      (ci->id3->is_asf_stream ?
                                       inputbuffer : SHARED_ADDR(inputbuffer)),
                                      size + padding);
                    stream.error = 0;
                    job.size = size;
                    job.read = 0;
                    job.made = 0;
                    job.over = false;
                    job.running = true;
                    decode_run();
                }
            }
        }

        decode_wait();

        const unsigned int queued = job.head - job.tail;

        if (queued < 2 && !input_over) {
            continue; /* Needs more input to tell */
        } else if (queued == 0) {
            break;
        }

        struct decoded_frame *f = &job.decoded[job.tail % DECODE_QUEUE];

        advance_stream_buffer(f->bytes);

        /* Check if sample rate and stereo settings changed in this frame. */
        if (f->samplerate != current_frequency) {
            current_frequency = f->samplerate;
            ci->configure(DSP_SET_FREQUENCY, current_frequency);
        }
        if (f->channels == 2) {
            if (current_stereo_mode != STEREO_NONINTERLEAVED) {
                ci->configure(DSP_SET_STEREO_MODE, STEREO_NONINTERLEAVED);
                current_stereo_mode = STEREO_NONINTERLEAVED;
            }
        } else {
            if (current_stereo_mode != STEREO_MONO) {
                ci->configure(DSP_SET_STEREO_MODE, STEREO_MONO);
                current_stereo_mode = STEREO_MONO;
            }
        }

        int framelength = f->length - samples_to_skip;
        if (framelength <= 0) {
            framelength = 0;
            samples_to_skip -= f->length;
        }

        samplesdone += framelength;
        ci->set_elapsed((samplesdone * 1000LL) / current_frequency);

        /* Cut the required samples from the end of the last frame. */
        const int count = framelength - (queued == 1 ? stop_skip : 0);

        if (count > 0) {
#ifdef DECODE_THREAD
            for (int ch = 0; ch < f->channels; ch++) {
                memcpy(&dsp_pcm[ch][samples_to_skip],
                       decoded_samples(job.tail, ch) + samples_to_skip,
                       count * sizeof (mad_fixed_t));
            }

            /* In case of a mono file, the second array will be ignored. */
            ci->pcmbuf_insert(&dsp_pcm[0][samples_to_skip],
                              &dsp_pcm[1][samples_to_skip],
                              count);
#else
            /* the frame is taken: the DSP may work on it in place */
            ci->pcmbuf_insert(decoded_samples(job.tail, 0) + samples_to_skip,
                              decoded_samples(job.tail, 1) + samples_to_skip,
                              count);
#endif
        }

        /* Only skip samples for the first frame added. */
        if (framelength > 0)
            samples_to_skip = 0;

        decode_taken();
    }

    decode_stop();

    return CODEC_OK;
}
