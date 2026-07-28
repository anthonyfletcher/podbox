/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to ts_io_core.c.
 ****************************************************************************/
#ifndef TS_IO_CORE_H
#define TS_IO_CORE_H

#include "txt_source.h"

struct ts_core_file {
    int      fd;
    ts_off_t pos;                            /* -1 when unknown */
};

/* Opens `path` and fills `io`. `f` must stay valid until ts_close(), which
 * takes ownership of the fd and closes it. Returns TS_OK or TS_ERR_IO. */
int ts_io_core(ts_io *io, struct ts_core_file *f, const char *path);

#endif /* TS_IO_CORE_H */
