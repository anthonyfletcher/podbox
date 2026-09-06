/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Stub: this tool is a hosted build like the simulator and has to define
 * SIMULATOR -- struct codec_api's layout depends on it, and the codecs it
 * loads were built by the simulator -- but it has no SDL and no window.
 *
 * sim.h defines HAVE_SDL as a consequence of SIMULATOR, which sends system.h
 * here. The real header wraps system-hosted.h and adds the interrupt controls
 * and a handful of window-loop declarations; this keeps the first and drops
 * the second.
 ****************************************************************************/
#ifndef _SYSTEM_SDL_H_
#define _SYSTEM_SDL_H_

#include <stdbool.h>
#include "config.h"
#include "gcc_extensions.h"

#define HIGHEST_IRQ_LEVEL 1

static inline int set_irq_level(int level) { (void)level; return 0; }

#define disable_irq()        ((void)0)
#define enable_irq()         ((void)0)
#define disable_irq_save()   0
#define restore_irq(level)   ((void)(level))
#define wait_for_interrupt()

#include "system-hosted.h"

#endif /* _SYSTEM_SDL_H_ */
