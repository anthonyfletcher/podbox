/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A codec's run on the 5G's second core, the COP. From RockWheel
 * (github.com/ssarkisy/RockWheel, apps/codec_cop.c, a3b35cf..470e770).
 *
 * The codec's codec_run() goes on a thread of the COP, unchanged, through a
 * view of the codec API of its own (ci_cop). The thread that started the run
 * hosts it on the CPU: what the codec asks of playback is done there, by the
 * host's own functions, as they would be for a run on the CPU.
 *
 * - PCM, and what goes with it in order (set_elapsed, set_offset, configure),
 *   goes into a ring the COP fills and the host empties, neither waiting for
 *   the other but for room.
 * - Input comes in windows: the host lends the COP a stretch of its buffer,
 *   which the COP copies into one of its own, so that the codec reads cached
 *   memory. request_buffer, advance_buffer, read_filebuf and seek_buffer are
 *   served from it; only a refill or a seek out of it asks the host. The
 *   emulators, which play a whole file in place, get a window that holds it.
 * - The few fields of id3 a codec sets itself (length, frequency, bitrate,
 *   tail_trim) are set on the COP's copy and go to the host's in the ring.
 * - get_command asks the host only when a message waits for the codec, and
 *   is then answered by the host's own get_command: the codec stops where it
 *   would on the CPU, and the host cannot reach its buffer meanwhile.
 *
 * What each core touches:
 * - Uncached (SHAREDBSS): the control below and the codec's view of the API,
 *   each word written by one core; the ring, through its uncached alias.
 * - The COP's, cached: the codec's code, statics and heap, the window, the
 *   id3 copy and the stack. The CPU writes them only before the run (the
 *   codec's load, the setup here), then writes its cache back; the COP drops
 *   its own at the run's start and writes it back at its end, and the CPU
 *   then drops its own.
 * - The CPU's: the bounce buffer the DSP works in, in place.
 *
 * Everything is taken at the start of a run from the top of the codec's own
 * buffer, never from a heap; a run that cannot have it stays on the CPU.
 * A run that goes quiet is stopped by the host's watchdog, and the caller
 * plays the track on the CPU.
 *
 * Parts, in order:
 *   - on the COP: the ring, the window, the codec's view of the API
 *   - on the CPU: the host, its watchdog
 *   - the switch
 ****************************************************************************/

#include "config.h"
#include "audio/codec_cop.h"

#ifdef HAVE_CODEC_COP
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "codecs.h"
#include "metadata.h"
#include "dsp_core.h"
#include "dsp_sample_io.h"
#include "dsp_proc_entry.h"
#include "dsp_misc.h"
#include "system/debug_log.h"

/* The thread's stack, as the codec thread's */
#define COP_STACK_SIZE   CACHEALIGN_UP(DEFAULT_STACK_SIZE + 0x2000)
/* The input window, but an emulator's (whole_file()); 32 KB of it are kept
   ahead of the codec at each request_buffer() */
#define COP_WIN_SIZE     (64*1024)
#define COP_WIN_LEAST    (32*1024)
/* Zeros after the file's end in the window, as libmad's guard wants them; on
   the CPU a codec reading on past the end reads whatever the buffer has */
#define COP_WIN_GUARD    16
/* The PCM ring: two 4096-sample frames of 32-bit stereo */
#define COP_RING_SIZE    (64*1024)
/* A piece of PCM, a buffer of it at most half of this when there are two */
#define COP_BOUNCE_SIZE  (16*1024)
/* The least left to the codec's own heap */
#define COP_MIN_HEAP     (32*1024)

#define REC_ALIGN 16
enum
{
    REC_PCM = 1,    /* a: samples, b: bytes of each buffer, c: buffers */
    REC_ELAPSED,    /* a: value */
    REC_OFFSET,     /* a: value */
    REC_CONFIGURE,  /* a: setting, b: value; c: 1 with the value after the
                       record (cop_configure) */
    REC_WRAP,       /* the rest to the end of the ring */
    REC_ID3,        /* a: the fields (ID3_*) of the struct cop_id3 after the
                       record that changed */
};

/* The fields of id3 that codecs set (shorten, vgm, asap), as the CPU's
   playback reads them */
enum
{
    ID3_LENGTH    = 0x1,
    ID3_FREQUENCY = 0x2,
    ID3_BITRATE   = 0x4,
    ID3_TAIL_TRIM = 0x8,
};

struct cop_id3
{
    unsigned long length, frequency;
    unsigned int bitrate;
    int tail_trim;
};

struct rec
{
    int32_t type, size;
    intptr_t a, b, c;
    int32_t pad[3]; /* to REC_ALIGN */
};

enum
{
    REQ_REFILL = 1,      /* a: position, b: most bytes, c: the window's start */
    REQ_SEEK,            /* a: position */
    REQ_GET_COMMAND,
    REQ_SEEK_COMPLETE,
    REQ_LOOP_TRACK,
    REQ_STRIP_FILESIZE,  /* a: size */
};

static struct
{
    /* the COP's */
    volatile unsigned int req_seq;
    volatile int req;
    volatile intptr_t arg[3];
    volatile unsigned long head;
    volatile bool ended;
    volatile int status;
    /* the host's */
    volatile unsigned int rep_seq;
    volatile intptr_t reply[2];
    volatile unsigned long tail;
    volatile bool cmd_pending;
    volatile bool stop;     /* the run is given up (the watchdog) */
    /* this run's, set before it starts */
    unsigned char *ring;    /* uncached */
    unsigned char *win;     /* the COP's */
    size_t win_size;
    bool whole;             /* the codec reads the whole file at once */
    void *heap;
    size_t heap_size;
    struct mp3entry *id3;   /* the COP's copy */
    int stereo_mode, depth;
    /* the COP's own state of the run */
    off_t win_pos;          /* the file's position of win[0] */
    size_t win_len;
    bool win_eof;
    bool commanded;         /* a command or a seek's end was asked */
    bool made_pcm;
    struct cop_id3 id3_sent; /* the copy's fields as the host has them */
} sh SHAREDBSS_ATTR;

static struct semaphore host_sem SHAREDBSS_ATTR; /* the host has something */
static struct semaphore cop_sem SHAREDBSS_ATTR;  /* room, or a reply */
static struct codec_api ci_cop SHAREDBSS_ATTR;   /* the codec's view */

/* The CPU's (the host thread's) */
static struct codec_api *host;
static bool host_running;
static bool host_kicked;    /* a message comes, maybe not queued yet */
static volatile unsigned int host_kicks; /* their count */


/** On the COP **/

static struct rec *ring_room(size_t size);
static void ring_publish(struct rec *r, int type, size_t size,
                         intptr_t a, intptr_t b, intptr_t c);

/* The fields of id3 the codec has set on the COP's copy since they last
   went, to the host's before anything that follows them, as the CPU's
   playback would have seen them set by the codec */
static void id3_sync(void)
{
    const struct mp3entry *id3 = sh.id3;
    struct cop_id3 *sent = &sh.id3_sent;
    unsigned int which = 0;

    if (id3->length != sent->length)
        which |= ID3_LENGTH;
    if (id3->frequency != sent->frequency)
        which |= ID3_FREQUENCY;
    if (id3->bitrate != sent->bitrate)
        which |= ID3_BITRATE;
    if (id3->tail_trim != sent->tail_trim)
        which |= ID3_TAIL_TRIM;
    if (!which)
        return;

    sent->length = id3->length;
    sent->frequency = id3->frequency;
    sent->bitrate = id3->bitrate;
    sent->tail_trim = id3->tail_trim;

    const size_t size = sizeof(struct rec) +
                        ALIGN_UP(sizeof(struct cop_id3), REC_ALIGN);
    struct rec *r = ring_room(size);
    memcpy(r + 1, sent, sizeof(*sent));
    ring_publish(r, REC_ID3, size, which, 0, 0);
}

/* A request of the host, and its answer */
static intptr_t cop_call(int req, intptr_t a, intptr_t b, intptr_t c)
{
    id3_sync();
    sh.arg[0] = a;
    sh.arg[1] = b;
    sh.arg[2] = c;
    sh.req = req;
    const unsigned int seq = sh.req_seq + 1;
    sh.req_seq = seq; /* the request is whole now */
    semaphore_release(&host_sem);
    while (sh.rep_seq != seq)
    {
        if (sh.stop)
            return 0;
        semaphore_wait(&cop_sem, TIMEOUT_BLOCK);
    }
    return sh.reply[0];
}

/* Room for a record of size bytes in the ring, as it is to be written. A run
   given up writes at the ring's start and publishes nothing: the host reads
   no more of it. */
static struct rec *ring_room(size_t size)
{
    while (1)
    {
        if (sh.stop)
            return (struct rec *)sh.ring;

        const unsigned long head = sh.head;
        const size_t free = COP_RING_SIZE - (head - sh.tail);
        const size_t off = head % COP_RING_SIZE;
        const size_t to_end = COP_RING_SIZE - off;
        struct rec *r = (struct rec *)(sh.ring + off);

        if (to_end < size)
        {
            /* the rest of the ring skipped, as the record must be whole */
            if (free >= to_end)
            {
                r->type = REC_WRAP;
                r->size = to_end;
                sh.head = head + to_end;
                semaphore_release(&host_sem);
                continue;
            }
        }
        else if (free >= size)
            return r;

        semaphore_wait(&cop_sem, TIMEOUT_BLOCK);
    }
}

/* The record written: the host may have it */
static void ring_publish(struct rec *r, int type, size_t size,
                         intptr_t a, intptr_t b, intptr_t c)
{
    if (sh.stop)
        return;
    r->type = type;
    r->size = size;
    r->a = a;
    r->b = b;
    r->c = c;
    sh.head += size;
    semaphore_release(&host_sem);
}

static void ring_event(int type, intptr_t a, intptr_t b)
{
    id3_sync();
    struct rec *r = ring_room(sizeof(struct rec));
    ring_publish(r, type, sizeof(struct rec), a, b, 0);
}

static void *cop_get_buffer(size_t *size)
{
    *size = sh.heap_size;
    return sh.heap;
}

static void cop_pcmbuf_insert(const void *ch1, const void *ch2, int count)
{
    const int mode = sh.stereo_mode;
    const size_t bytes_sample = (sh.depth > NATIVE_DEPTH ? 4 : 2) *
                                (mode == STEREO_INTERLEAVED ? 2 : 1);
    const int buffers = (mode == STEREO_NONINTERLEAVED && ch2) ? 2 : 1;
    const int most = COP_BOUNCE_SIZE / buffers / bytes_sample;
    const unsigned char *p1 = ch1, *p2 = ch2;

    if (count > 0)
        sh.made_pcm = true;
    id3_sync();

    do
    {
        /* even none goes as is: the DSP lets out what it held */
        const int n = MIN(MAX(count, 0), most);
        const size_t bytes = n * bytes_sample;
        const size_t size = sizeof(struct rec) +
                            ALIGN_UP(bytes * buffers, REC_ALIGN);
        struct rec *r = ring_room(size);

        memcpy(r + 1, p1, bytes);
        if (buffers == 2)
            memcpy((unsigned char *)(r + 1) + bytes, p2, bytes);
        ring_publish(r, REC_PCM, size, n, bytes, buffers);

        p1 += bytes;
        if (buffers == 2)
            p2 += bytes;
        count -= n;
    }
    while (count > 0);

    /* The equalizer's worker shares this core (eq_cop.c), and the host is
       waiting on it for this very PCM */
    yield();
}

static void cop_set_elapsed(unsigned long value)
{
    sh.id3->elapsed = value;
    ring_event(REC_ELAPSED, value, 0);
}

static void cop_set_offset(size_t value)
{
    sh.id3->offset = value;
    ring_event(REC_OFFSET, value, 0);
}

static void cop_configure(int setting, intptr_t value)
{
    if (setting == DSP_SET_STEREO_MODE)
        sh.stereo_mode = value;
    else if (setting == DSP_SET_SAMPLE_DEPTH)
        sh.depth = value;
    else if (setting == REPLAYGAIN_SET_GAINS && value)
    {
        /* The one setting a codec gives by pointer, to a struct on the COP's
           stack and in its cache, gone by the time the host applies it: the
           gains go in the ring by value, and the host gives the DSP a copy */
        id3_sync();
        const size_t size = sizeof(struct rec) +
                            ALIGN_UP(sizeof(struct dsp_replay_gains), REC_ALIGN);
        struct rec *r = ring_room(size);
        memcpy(r + 1, (const void *)value, sizeof(struct dsp_replay_gains));
        ring_publish(r, REC_CONFIGURE, size, setting, 0, 1);
        return;
    }
    ring_event(REC_CONFIGURE, setting, value);
}

/* The bytes of the window from where the codec is, at least need of them if
   the file has them: what is left moved to the window's start, and as much
   as fits asked for after it, again while a refill brings less than needed
   (a need over what the host's buffer promises, at its ring's end) */
static size_t win_fill(size_t need)
{
    const size_t off = ci_cop.curpos - sh.win_pos;
    const size_t have = sh.win_len - off;

    if (have >= need || sh.win_eof)
        return have;

    /* where the window began: the host keeps it buffered (host_serve) */
    const off_t keep = sh.win_pos;
    memmove(sh.win, sh.win + off, have);
    sh.win_pos = ci_cop.curpos;
    sh.win_len = have;

    size_t got;
    do
    {
        /* the host's buffer from the end of what is here */
        const size_t most = sh.win_size - sh.win_len;
        const unsigned char *src = (const unsigned char *)
            cop_call(REQ_REFILL, sh.win_pos + sh.win_len, most, keep);
        got = src ? MIN((size_t)sh.reply[1], most) : 0;

        if (got)
            memcpy(sh.win + sh.win_len, src, got);
        sh.win_len += got;
        /* only the file's end: a refill that brought nothing before it (its
           wait abandoned for a seek or a stop, the command then coming at
           the codec's next get_command; or a read that failed, as on the
           CPU) is only nothing this time */
        if (sh.win_pos + (off_t)sh.win_len >= ci_cop.filesize)
            sh.win_eof = true;
    }
    while (got && sh.win_len < need && sh.win_len < sh.win_size &&
           !sh.win_eof);

    if (sh.win_eof)
        memset(sh.win + sh.win_len, 0, COP_WIN_GUARD);
    return sh.win_len;
}

static void *cop_request_buffer(size_t *realsize, size_t reqsize)
{
    /* 32 KB from here, if the file has them, whatever is asked: on the CPU
       the buffer runs on past what a request gives, at least as far as its
       32 KB guard, and a codec may read on into it (RealMedia AAC reads a
       packet longer than it asked for). An emulator asks for the whole file
       and plays it from there: all of it, as the CPU's buffer has it */
    const size_t have = win_fill(sh.whole ? MIN(reqsize, sh.win_size)
                                          : COP_WIN_LEAST);
    *realsize = MIN(have, reqsize);
    return *realsize ? sh.win + (ci_cop.curpos - sh.win_pos) : NULL;
}

/* The window moved to newpos out of it: the host's buffer seeks there */
static bool win_seek(size_t newpos)
{
    if (!cop_call(REQ_SEEK, newpos, 0, 0))
        return false;
    ci_cop.curpos = newpos;
    sh.win_pos = newpos;
    sh.win_len = 0;
    sh.win_eof = false;
    return true;
}

static void cop_advance_buffer(size_t amount)
{
    /* the amount may be a step back, as bufadvance() takes it */
    const off_t newpos = ci_cop.curpos + (off_t)amount;

    if (newpos >= sh.win_pos && newpos <= sh.win_pos + (off_t)sh.win_len)
        ci_cop.curpos = newpos;
    else if (newpos < 0 || newpos > ci_cop.filesize || !win_seek(newpos))
    {
        /* out of the file, as bufadvance() fails: at the end, no offset */
        win_seek(ci_cop.filesize);
        return;
    }

    cop_set_offset(ci_cop.curpos);
}

static size_t cop_read_filebuf(void *ptr, size_t size)
{
    size_t done = 0;

    while (done < size)
    {
        const size_t have = win_fill(1);
        const size_t n = MIN(have, size - done);
        if (n == 0)
            break;
        memcpy((unsigned char *)ptr + done,
               sh.win + (ci_cop.curpos - sh.win_pos), n);
        ci_cop.curpos += n;
        done += n;
    }

    return done;
}

static bool cop_seek_buffer(size_t newpos)
{
    /* in the window: nothing to ask */
    if ((off_t)newpos >= sh.win_pos &&
        (off_t)newpos <= sh.win_pos + (off_t)sh.win_len)
    {
        ci_cop.curpos = newpos;
        return true;
    }
    return win_seek(newpos);
}

static void cop_seek_complete(void)
{
    sh.commanded = true;
    /* the offset at a seek's end is where the codec is, as on the CPU */
    sh.id3->offset = ci_cop.curpos;
    cop_call(REQ_SEEK_COMPLETE, ci_cop.curpos, 0, 0);
}

static long cop_get_command(intptr_t *param)
{
    if (sh.stop)
        return CODEC_ACTION_HALT;
    if (!sh.cmd_pending)
    {
        yield();
        return CODEC_ACTION_NULL;
    }

    const long action = cop_call(REQ_GET_COMMAND, 0, 0, 0);
    if (action != CODEC_ACTION_NULL)
    {
        sh.commanded = true;
        if (param)
            *param = sh.reply[1];
    }
    return action;
}

static bool cop_loop_track(void)
{
    return cop_call(REQ_LOOP_TRACK, 0, 0, 0);
}

static void cop_strip_filesize(off_t size)
{
    if (!cop_call(REQ_STRIP_FILESIZE, size, 0, 0))
        return;

    /* what the window holds past the new end is not the file's now (MP3's
       ID3v1 or APE tag, whose bytes the decoder must not see) */
    ci_cop.filesize = size;
    if (sh.win_pos + (off_t)sh.win_len >= size)
    {
        sh.win_len = size > sh.win_pos ? (size_t)(size - sh.win_pos) : 0;
        sh.win_eof = true;
        memset(sh.win + sh.win_len, 0, COP_WIN_GUARD);
    }
}

#if defined(DEBUG) || defined(SIMULATOR) || defined(ROCKBOX_HAS_LOGF)
/* their buffers are the CPU's */
static void cop_no_log(const char *fmt, ...)
{
    (void)fmt;
}
#endif

static void codec_cop_thread(void)
{
    /* the codec as the CPU loaded it, and this run as it was set up */
    commit_discard_idcache();

    sh.status = codec_run_proc();

    /* what it set of id3 at the end, before the end */
    id3_sync();
    commit_dcache();
    sh.ended = true;
    semaphore_release(&host_sem);
}


/** On the CPU, the host **/

/* One record of the ring done, as the run on the CPU would have: false if
   there is none */
static bool host_take(unsigned char *bounce)
{
    const unsigned long tail = sh.tail;
    if (tail == sh.head)
        return false;

    const struct rec *r = (const struct rec *)(sh.ring + tail % COP_RING_SIZE);
    const int type = r->type;
    const size_t size = r->size;

    switch (type)
    {
    case REC_PCM:
    {
        const int count = r->a;
        const size_t bytes = r->b;
        const int buffers = r->c;
        void *ch2 = NULL;

        /* the DSP works in place: on a copy of the CPU's own, never on the
           ring, where lines of its cache could later be written back */
        memcpy(bounce, r + 1, bytes);
        if (buffers == 2)
        {
            ch2 = bounce + COP_BOUNCE_SIZE / 2;
            memcpy(ch2, (const unsigned char *)(r + 1) + bytes, bytes);
        }
        host->pcmbuf_insert(bounce, buffers == 2 ? ch2 : bounce, count);
        break;
    }
    case REC_ELAPSED:
        host->set_elapsed(r->a);
        break;
    case REC_OFFSET:
        host->set_offset(r->a);
        break;
    case REC_CONFIGURE:
        if (r->c)
        {
            /* the gains (cop_configure), copied out for the DSP to read */
            struct dsp_replay_gains gains;
            memcpy(&gains, r + 1, sizeof(gains));
            host->configure(r->a, (intptr_t)&gains);
        }
        else
            host->configure(r->a, r->b);
        break;
    case REC_ID3:
    {
        struct cop_id3 v;
        struct mp3entry *id3 = host->id3;
        memcpy(&v, r + 1, sizeof(v));
        if (r->a & ID3_LENGTH)
            id3->length = v.length;
        if (r->a & ID3_FREQUENCY)
            id3->frequency = v.frequency;
        if (r->a & ID3_BITRATE)
            id3->bitrate = v.bitrate;
        if (r->a & ID3_TAIL_TRIM)
            id3->tail_trim = v.tail_trim;
        break;
    }
    }

    sh.tail = tail + size;
    semaphore_release(&cop_sem);
    return true;
}

static void host_serve(unsigned char *bounce)
{
    const int req = sh.req;
    intptr_t reply = 0, reply2 = 0;

    switch (req)
    {
    case REQ_REFILL:
    {
        /* The buffer from pos, up to the window's room. It is asked for from
           the window's start (c), the bytes the COP has already not copied
           again, so that buffering's read position stays there while this
           waits: the data behind it can be freed then, and the codec may yet
           seek back into the window (FLAC's frame copy), which on the CPU it
           does without yielding. Where the buffer's contiguous run from
           there ends too soon after pos (its end), from pos itself, the
           read position then at pos. Never from before where
           the read position is (host->curpos, after such a refill): what is
           behind it may be gone, and a seek there would rebuffer */
        const size_t pos = sh.arg[0];
        size_t from = sh.arg[2];
        if (host->curpos > (off_t)from && host->curpos <= (off_t)pos)
            from = host->curpos;
        const size_t ahead = pos - from;
        size_t got = 0;
        unsigned char *p = NULL;
        if (host->curpos == (off_t)from || host->seek_buffer(from))
            p = host->request_buffer(&got, ahead + sh.arg[1]);
        /* taken only with what request_buffer() promises past pos, or with
           the rest of the file: at the ring's end bufgetdata() cuts a
           request at the guard, measured from where it starts */
        if (p && got > ahead &&
            (got - ahead >= COP_WIN_LEAST ||
             (off_t)(from + got) >= host->filesize))
        {
            p += ahead;
            got -= ahead;
        }
        else if (p)
        {
            p = NULL;
            got = 0;
            if (host->seek_buffer(pos))
                p = host->request_buffer(&got, sh.arg[1]);
        }
        /* the wrap's copy into the guard was made in the CPU's cache */
        commit_dcache();
        reply = p ? (intptr_t)UNCACHED_ADDR(p) : 0;
        reply2 = p ? got : 0;
        break;
    }
    case REQ_SEEK:
        reply = host->seek_buffer(sh.arg[0]);
        break;
    default:
        /* the others after everything that came before them */
        while (host_take(bounce));
        switch (req)
        {
        case REQ_GET_COMMAND:
            reply = host->get_command(&reply2);
            break;
        case REQ_SEEK_COMPLETE:
        {
            /* the offset it sets from curpos is the codec's position, not
               where the host's last refill left it; only this thread reads
               curpos */
            const off_t pos = host->curpos;
            host->curpos = sh.arg[0];
            host->seek_complete();
            host->curpos = pos;
            break;
        }
        case REQ_LOOP_TRACK:
            reply = host->loop_track();
            break;
        case REQ_STRIP_FILESIZE:
            host->strip_filesize(sh.arg[0]);
            reply = host->filesize == sh.arg[0];
            break;
        }
    }

    sh.reply[0] = reply;
    sh.reply[1] = reply2;
    sh.rep_seq = sh.req_seq;
    semaphore_release(&cop_sem);
}

void codec_cop_kick(void)
{
    if (host_running)
    {
        host_kicks++;
        host_kicked = true;
        semaphore_release(&host_sem);
    }
}

/* The formats whose codec asks for the whole file at once and plays it from
   there, in place (the emulators) */
static bool whole_file(int afmt)
{
    switch (afmt)
    {
    case AFMT_SID:
    case AFMT_NSF:
    case AFMT_MOD:
    case AFMT_SAP:
    case AFMT_CMC:
    case AFMT_CM3:
    case AFMT_CMR:
    case AFMT_CMS:
    case AFMT_DMC:
    case AFMT_DLT:
    case AFMT_MPT:
    case AFMT_MPD:
    case AFMT_RMT:
    case AFMT_TMC:
    case AFMT_TM8:
    case AFMT_TM2:
    case AFMT_AY:
    case AFMT_GBS:
    case AFMT_HES:
    case AFMT_SGC:
    case AFMT_VGM:
    case AFMT_KSS:
        return true;
    default:
        return false;
    }
}

/* Sizes of the parts taken from the codec's buffer, in bytes, each a whole
   number of cache lines; the window's is the run's */
#define COP_ID3_SIZE CACHEALIGN_UP(sizeof(struct mp3entry))
#define COP_TAKEN    (COP_STACK_SIZE + COP_ID3_SIZE + COP_RING_SIZE + \
                      COP_BOUNCE_SIZE + CACHEALIGN_UP(COP_WIN_GUARD) + \
                      CACHEALIGN_SIZE)

/* The watchdog: how long the COP may give nothing (no PCM, no request),
   then how long after both sides are woken, then how long a run told to stop
   has to end. A codec sends PCM many times a second, and asks the host for
   its input; a pause or a full PCM buffer holds the host inside a call, so
   neither counts as quiet. */
#define COP_QUIET_TICKS  (10*HZ)
#define COP_WOKEN_TICKS  (5*HZ)
#define COP_STOP_TICKS   (2*HZ)

/* The run's state, to the playback log at once */
static void stall_log(unsigned int id, const char *what)
{
    struct thread_debug_info t;

    if (thread_get_debug_info(id, &t) < 0)
        strcpy(t.statusstr, "-");
    debug_log_sync(DEBUG_LOG_PLAYBACK, "cop: %s; thread %s, req %d %u/%u, "
                   "ring %lu/%lu, at %ld of %ld, window %ld+%lu%s%s%s%s",
                   what, t.statusstr, sh.req, sh.rep_seq, sh.req_seq,
                   sh.tail, sh.head, (long)ci_cop.curpos,
                   (long)ci_cop.filesize, (long)sh.win_pos,
                   (unsigned long)sh.win_len, sh.made_pcm ? ", sound" : "",
                   sh.commanded ? ", commanded" : "",
                   sh.cmd_pending ? ", message waiting" : "",
                   sh.ended ? ", ended" : "");
}

/* The COP has been quiet too long: the first time both sides are woken, in
   case a wake-up between the cores was lost; the next the run is told to
   stop. True when it has ended. */
static bool watchdog(unsigned int id, bool *woken)
{
    if (!*woken)
    {
        stall_log(id, "nothing for 10 s, both sides woken");
        *woken = true;
        semaphore_release(&cop_sem);
        semaphore_release(&host_sem);
        return false;
    }

    stall_log(id, "still nothing, the run stopped");
    sh.stop = true;
    const long until = current_tick + COP_STOP_TICKS;
    while (!sh.ended && TIME_BEFORE(current_tick, until))
    {
        semaphore_release(&cop_sem);
        sleep(1);
    }

    if (!sh.ended)
    {
        /* Busy in the codec's own code: what it runs and uses is the
           codec's, which nothing else may touch until it ends */
        stall_log(id, "the run does not stop; waiting for it");
        while (!sh.ended)
        {
            semaphore_release(&cop_sem);
            sleep(HZ/10);
        }
    }
    stall_log(id, "the run has ended");
    return true;
}

int codec_cop_run(struct codec_api *api, int stereo_mode, int depth,
                  bool (*msg_waiting)(void), struct codec_cop_result *res)
{
    res->why = NULL;
    res->stalled = false;
    res->commanded = false;
    res->made_pcm = false;
    res->at_end = false;
    res->stack_used = 0;

    /* the codec's buffer, its top taken here */
    size_t size;
    unsigned char *buf = api->codec_get_buffer(&size);

    /* the window: an emulator's holds the whole file, as it plays it from
       the one request it makes (one larger than the buffer cannot fit) */
    const bool whole = whole_file(api->id3->codectype);
    size_t win_size = COP_WIN_SIZE;
    if (whole && api->filesize > COP_WIN_SIZE)
        win_size = api->filesize > (off_t)size ? size :
                   CACHEALIGN_UP((size_t)api->filesize);

    const size_t taken = COP_TAKEN + win_size;
    if (!buf || size < taken + COP_MIN_HEAP)
    {
        res->why = "no room";
        return CODEC_COP_NOT_RUN;
    }
    unsigned char *top = (unsigned char *)
        ((uintptr_t)(buf + size) & ~(CACHEALIGN_SIZE - 1));
    unsigned char *stack  = top - COP_STACK_SIZE;
    unsigned char *id3    = stack - COP_ID3_SIZE;
    unsigned char *win    = id3 - CACHEALIGN_UP(win_size + COP_WIN_GUARD);
    unsigned char *ring   = win - COP_RING_SIZE;
    unsigned char *bounce = ring - COP_BOUNCE_SIZE;

    host = api;
    ci_cop = *api;
    ci_cop.id3 = (struct mp3entry *)id3;
    memcpy(id3, api->id3, sizeof(struct mp3entry));
    ci_cop.codec_get_buffer = cop_get_buffer;
    ci_cop.pcmbuf_insert = cop_pcmbuf_insert;
    ci_cop.set_elapsed = cop_set_elapsed;
    ci_cop.read_filebuf = cop_read_filebuf;
    ci_cop.request_buffer = cop_request_buffer;
    ci_cop.advance_buffer = cop_advance_buffer;
    ci_cop.seek_buffer = cop_seek_buffer;
    ci_cop.seek_complete = cop_seek_complete;
    ci_cop.set_offset = cop_set_offset;
    ci_cop.configure = cop_configure;
    ci_cop.get_command = cop_get_command;
    ci_cop.loop_track = cop_loop_track;
    ci_cop.strip_filesize = cop_strip_filesize;
#if defined(DEBUG) || defined(SIMULATOR)
    ci_cop.debugf = cop_no_log;
#endif
#ifdef ROCKBOX_HAS_LOGF
    ci_cop.logf = cop_no_log;
#endif

    memset((void *)&sh, 0, sizeof(sh));
    sh.ring = UNCACHED_ADDR(ring);
    sh.win = win;
    sh.win_size = win_size;
    sh.whole = whole;
    sh.heap = buf;
    sh.heap_size = bounce - buf;
    sh.id3 = (struct mp3entry *)id3;
    sh.id3_sent.length = api->id3->length;
    sh.id3_sent.frequency = api->id3->frequency;
    sh.id3_sent.bitrate = api->id3->bitrate;
    sh.id3_sent.tail_trim = api->id3->tail_trim;
    sh.stereo_mode = stereo_mode;
    sh.depth = depth;
    sh.win_pos = api->curpos;
    sh.cmd_pending = true;
    semaphore_init(&host_sem, 1, 0);
    semaphore_init(&cop_sem, 1, 0);

    /* the codec at its view; the CPU's cache of the codec, the id3 copy and
       the rest written back for the COP to read */
    struct codec_api *loaded = codec_set_api(&ci_cop);
    commit_dcache();

    const unsigned int id = create_thread(codec_cop_thread, stack,
                                          COP_STACK_SIZE, 0, "codec cop"
                                          IF_PRIO(, PRIORITY_PLAYBACK)
                                          IF_COP(, COP));
    if (id == 0)
    {
        codec_set_api(loaded);
        res->why = "no thread";
        return CODEC_COP_NOT_RUN;
    }

    long news = current_tick;   /* when the COP last gave anything */
    bool woken = false;         /* the watchdog has woken both sides */
    host_kicked = false;
    host_running = true;
    while (1)
    {
        const long last_news = news;
        const bool waiting = msg_waiting ? msg_waiting() : true;
        if (waiting)
            host_kicked = false;
        sh.cmd_pending = waiting;
        if (sh.rep_seq != sh.req_seq)
        {
            const unsigned int kicks = host_kicks;
            host_serve(bounce);
            /* the message a kick told of may have been taken in there (by
               get_command, or by a seek's end taking the RUN message): with
               none waiting now and no kick since, none is coming */
            if (host_kicks == kicks && !(msg_waiting ? msg_waiting() : true))
                host_kicked = false;
            news = current_tick;
        }
        else if (host_take(bounce))
            news = current_tick;
        else if (sh.ended)
        {
            while (host_take(bounce));
            break;
        }
        else if (TIME_AFTER(current_tick, news +
                            (woken ? COP_WOKEN_TICKS : COP_QUIET_TICKS)))
        {
            if (watchdog(id, &woken))
            {
                res->stalled = true;
                break;
            }
            news = current_tick;
            continue;
        }
        else
            /* kicked by a sender this thread runs above (its priority
               raised as PCM ran low), the message is not in the queue yet:
               the sender queues it when this waits, seen at the next tick */
            semaphore_wait(&host_sem, host_kicked ? 1 : HZ/10);

        if (woken && news != last_news)
        {
            stall_log(id, "went on after the wake-up");
            woken = false;
        }
    }
    host_running = false;

    thread_wait(id);
    /* what the COP changed of the codec, fresh for the CPU */
    commit_discard_dcache();
    codec_set_api(loaded);

    /* how deep its stack went: create_thread() filled it */
    const uint32_t *word = (const uint32_t *)stack;
    while ((const unsigned char *)word < top && *word == 0xdeadbeefu)
        word++;
    res->stack_used = top - (const unsigned char *)word;

    res->commanded = sh.commanded;
    res->made_pcm = sh.made_pcm;
    res->at_end = ci_cop.curpos >= ci_cop.filesize;
    return sh.status;
}


/** The switch **/

/* MP3 keeps mpa.c's own synthesis thread on the COP, and SPC its own
   thread there too. KSS's and VGM's emulators take so much of the codec's
   buffer that a run there is never left room. WMA Voice has no metadata
   parser, so it never plays; VTX is built only with an FPU. */
bool codec_cop_wanted(int afmt)
{
    switch (afmt)
    {
    case AFMT_MP4_AAC:
    case AFMT_MP4_AAC_HE:
    case AFMT_MP4_ALAC:
    case AFMT_FLAC:
    case AFMT_OGG_VORBIS:
    case AFMT_OPUS:
    case AFMT_SPEEX:
    case AFMT_WAVPACK:
    case AFMT_APE:
    case AFMT_MPC_SV7:
    case AFMT_MPC_SV8:
    case AFMT_WMA:
    case AFMT_WMAPRO:
    case AFMT_A52:
    case AFMT_RM_AC3:
    case AFMT_RM_COOK:
    case AFMT_RM_ATRAC3:
    case AFMT_OMA_ATRAC3:
    case AFMT_RM_AAC:
    case AFMT_AAC_BSF:
    case AFMT_TTA:
    case AFMT_SHN:
    case AFMT_PCM_WAV:
    case AFMT_AIFF:
    case AFMT_AU:
    case AFMT_WAVE64:
    case AFMT_VOX:
    case AFMT_SMAF:
    case AFMT_ADX:
    case AFMT_DSD:
    case AFMT_SID:
    case AFMT_NSF:
    case AFMT_MOD:
    case AFMT_SAP:
    case AFMT_CMC:
    case AFMT_CM3:
    case AFMT_CMR:
    case AFMT_CMS:
    case AFMT_DMC:
    case AFMT_DLT:
    case AFMT_MPT:
    case AFMT_MPD:
    case AFMT_RMT:
    case AFMT_TMC:
    case AFMT_TM8:
    case AFMT_TM2:
    case AFMT_AY:
    case AFMT_GBS:
    case AFMT_HES:
    case AFMT_SGC:
        return true;
    default:
        return false;
    }
}
#endif /* HAVE_CODEC_COP */
