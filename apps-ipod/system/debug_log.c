/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * See debug_log.h. Each log is opened, appended to and closed per line rather
 * than held open: these run on background threads across USB connects and
 * unmounts, and a handle kept open over one of those is a handle that stops
 * the disk being released.
 ****************************************************************************/
#include <stdio.h>
#include <stdarg.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "kernel.h"
#include "rbpaths.h"
#include "settings/settings.h"
#include "debug_log.h"

static const char * const log_path[DEBUG_LOG_COUNT] = {
    [DEBUG_LOG_TAGCACHE] = ROCKBOX_DIR "/tagcache.log",
    [DEBUG_LOG_ARTCACHE] = ROCKBOX_DIR "/artcache.log",
};

bool debug_log_enabled(enum debug_log_id id)
{
    switch (id)
    {
        case DEBUG_LOG_TAGCACHE: return global_settings.debug_log_tagcache;
        case DEBUG_LOG_ARTCACHE: return global_settings.debug_log_artcache;
        default:                 return false;
    }
}

void debug_log(enum debug_log_id id, const char *fmt, ...)
{
    if (id >= DEBUG_LOG_COUNT || !debug_log_enabled(id))
        return;

    int fd = open(log_path[id], O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;

    /* Ticks since boot rather than the clock: what matters here is how long
     * something took and in what order, and the RTC may not be set. */
    char line[256];
    int len = snprintf(line, sizeof line, "[%8ld] ", (long)current_tick);
    if (len < 0 || len >= (int)sizeof line)
        len = 0;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line + len, sizeof line - len, fmt, ap);
    va_end(ap);

    if (n > 0)
    {
        len += MIN(n, (int)(sizeof line - len - 1));
        line[len++] = '\n';
        write(fd, line, len);
    }
    close(fd);
}

void debug_log_restart(enum debug_log_id id)
{
    if (id >= DEBUG_LOG_COUNT || !debug_log_enabled(id))
        return;

    int fd = open(log_path[id], O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
        close(fd);

    debug_log(id, "--- log started, %s ---",
              id == DEBUG_LOG_TAGCACHE ? "tagcache" : "art cache");
}
