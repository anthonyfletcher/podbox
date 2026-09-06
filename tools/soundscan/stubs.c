/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Stands in for the firmware, as tools/checkwps/stubs.c does for the skin
 * engine.
 *
 * The tool compiles the player's own codec loader, decoder harness and
 * analysis, so it reaches for a few things that only exist inside a running
 * player. Each one below is either genuinely absent here (the compacting
 * allocator, the PCM mixer, the codec thread) or a one-line equivalent of
 * something the player gets from its kernel.
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "metadata.h"
#include "codecs.h"

/* ---- the codec buffer ------------------------------------------------- *
 *
 * codecs.c loads into this on the player and checks that the codec landed
 * inside it. A host object loads wherever the loader puts it, which is why
 * that check is behind PLATFORM_NATIVE; the buffer is still referenced, so
 * it has to exist. */
unsigned char codecbuf[CODEC_SIZE];

/* ---- the codec thread ------------------------------------------------- *
 *
 * There is none: this tool drives the codec on its own thread, which is what
 * track_decode.c was written to do. codec_unload() exists so track_decode.c
 * can hand the slot over cleanly on a player and find nothing to hand over
 * here. */
int codec_loaded(void)
{
    return AFMT_UNKNOWN;
}

void codec_unload(void)
{
}

/* The player's version logs and clamps; this is the lookup it wraps. */
const char *get_codec_filename(int cod_spec)
{
    if ((unsigned)cod_spec >= AFMT_NUM_CODECS)
        cod_spec = AFMT_UNKNOWN;

    return audio_formats[cod_spec].codec_root_fn;
}

/* ---- memory ----------------------------------------------------------- *
 *
 * sound_index.c takes its key table from the player's compacting allocator.
 * One allocation at a time is all it ever holds, so malloc will do. */
static void *the_block;

int core_alloc(size_t size)
{
    free(the_block);
    the_block = malloc(size);

    return the_block != NULL ? 1 : -1;
}

void *core_get_data(int handle)
{
    (void)handle;
    return the_block;
}

int core_free(int handle)
{
    (void)handle;
    free(the_block);
    the_block = NULL;

    return 0;
}

size_t core_allocatable(void)
{
    return (size_t)-1;
}

/* ---- kernel ----------------------------------------------------------- */

/* Codecs yield to let the rest of the player run. Nothing else is running. */
void yield(void)
{
}

/* The tree's own debugf writes every metadata read and codec transition to
 * stderr, which for a library scan is thousands of lines nobody asked for.
 * Kept behind an environment variable rather than deleted: when a codec will
 * not load it is the only thing that says why. */
void debugf(const char *fmt, ...)
{
    static int on = -1;
    va_list ap;

    if (on < 0)
        on = getenv("SOUNDSCAN_DEBUG") != NULL;

    if (!on)
        return;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void panicf(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "\npanic: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");

    exit(2);
}

/* ---- odds and ends ---------------------------------------------------- *
 *
 * unicode.c loads codepage tables through the allocator when it needs one.
 * Nothing here asks for a codepage -- tags arrive as UTF-8 from the metadata
 * reader -- so these exist to satisfy the link rather than to work. */
int core_alloc_ex(size_t size, void *ops)
{
    (void)size; (void)ops;
    return -1;
}

void core_pin(int handle)
{
    (void)handle;
}

void core_unpin(int handle)
{
    (void)handle;
}

/* MinGW has no localtime_r, and the simulator's filesystem layer wants one
 * for directory timestamps. Single-threaded here, so the copy is safe. */
#ifdef _WIN32
struct tm *localtime_r(const time_t *t, struct tm *out)
{
    struct tm *tmp = localtime(t);

    if (tmp == NULL)
        return NULL;

    *out = *tmp;

    return out;
}
#endif

/* ---- the PCM mixer ---------------------------------------------------- *
 *
 * spectrum_meter.c samples it in spectrum_meter_peek(), which only a live
 * player calls -- the analysis here is fed from decoded files instead. */
const int16_t *mixer_channel_get_buffer(int channel, int *count)
{
    (void)channel;
    *count = 0;

    return NULL;
}

unsigned int mixer_get_frequency(void)
{
    return 44100;
}
