/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to codec_cop.c: a codec's run on the 5G's second core.
 ****************************************************************************/

#ifndef CODEC_COP_H
#define CODEC_COP_H

#include "config.h"
#include <stdbool.h>
#include <stddef.h>

struct codec_api;

/* The loaded codec pointed at another api; the one it had, or NULL if none
 * is loaded (codecs.c) */
struct codec_api *codec_set_api(struct codec_api *api);

#if NUM_CORES > 1 && (CONFIG_PLATFORM & PLATFORM_NATIVE) \
    && !defined(BOOTLOADER)
#define HAVE_CODEC_COP

/* codec_cop_run()'s status when the codec did not start on the COP */
#define CODEC_COP_NOT_RUN (-100)

struct codec_cop_result
{
    const char *why;     /* why it did not run there, or NULL */
    bool commanded;      /* a command or a seek's end reached the codec */
    bool made_pcm;       /* it gave PCM */
    bool at_end;         /* it had read the file to its end */
    bool stalled;        /* the watchdog stopped it, quiet too long */
    size_t stack_used;   /* the most of its stack the run used, in bytes */
};

/* Runs the loaded codec on the COP, with host's functions done on the CPU
 * by the calling thread; stereo_mode and depth as the codec has configured
 * them. msg_waiting, if any, says when a message waits for the codec, which
 * then gets its get_command() from host; NULL asks host at every one.
 * Returns the codec's status, or CODEC_COP_NOT_RUN with nothing done. */
int codec_cop_run(struct codec_api *host, int stereo_mode, int depth,
                  bool (*msg_waiting)(void), struct codec_cop_result *res);

/* A message for the hosting thread, about to be queued: wakes it if a run
 * goes on */
void codec_cop_kick(void);

/* The formats that run on the COP; the others stay on the CPU */
bool codec_cop_wanted(int afmt);
#endif /* HAVE_CODEC_COP */

#endif /* CODEC_COP_H */
